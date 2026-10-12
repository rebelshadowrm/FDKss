#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace class_metadata {
struct Row {
    std::string name, base_name;
    std::uint32_t count=0, flags=0, object_offset=0, constructor_offset=0, contained=0, attributes=0;
    std::int32_t member_offset=0, vbtable_offset=0, virtual_displacement=0;
};
// MSVC x64 image-relative RTTI. Read only image-contained metadata, never use
// the displacement fields to cast or dereference an object.
template<class Read>
bool inspect(std::uintptr_t image, std::size_t size, std::uintptr_t table,
    std::uint32_t index, Read read, Row &out) {
    out={};
    if (!image || size < 8 || image > std::numeric_limits<std::uintptr_t>::max()-size ||
        table < image+8 || table > image+size-8) return false;
    auto bytes=[&](std::uintptr_t address, void *dest, std::size_t length) {
        return address >= image && address <= image+size && length <= image+size-address && read(address,dest,length);
    };
    auto relative=[&](std::uint32_t rva, void *dest, std::size_t length) {
        return rva && bytes(image+rva,dest,length);
    };
    std::uintptr_t locator=0;
    struct Locator {std::uint32_t signature,offset,constructor,type,hierarchy,self;} col{};
    struct Hierarchy {std::uint32_t signature,flags,count,array;} hierarchy{};
    if (!bytes(table-8,&locator,8) || !bytes(locator,&col,sizeof(col)) || col.signature!=1 ||
        locator-image!=col.self || !relative(col.hierarchy,&hierarchy,sizeof(hierarchy)) ||
        hierarchy.signature || !hierarchy.count || hierarchy.count>1024 || index>=hierarchy.count) return false;
    auto name=[&](std::uint32_t type, std::string &dest) {
        if (!type || type>size || size-type<17) return false;
        // TypeDescriptor has two pointer words followed by the decorated name.
        std::array<char,256> text{};
        for (std::size_t i=0;i<text.size();++i) {
            if (!bytes(image+type+16+i,&text[i],1)) return false;
            if (!text[i]) {dest.assign(text.data(),i);return i>=5 && dest.starts_with(".?A");}
        }
        return false;
    };
    if (!hierarchy.array || hierarchy.array>size ||
        static_cast<std::size_t>(hierarchy.count)*4>size-hierarchy.array) return false;
    std::uint32_t descriptor=0;
    struct Base {std::uint32_t type,contained;std::int32_t member,vbtable,displacement;std::uint32_t flags;} base{};
    if (!relative(hierarchy.array+index*4,&descriptor,4) || !relative(descriptor,&base,sizeof(base)) ||
        base.contained>=hierarchy.count-index || !name(col.type,out.name) || !name(base.type,out.base_name)) {out={};return false;}
    // Entry zero represents the complete hierarchy, including itself.
    if (index==0 && (base.type!=col.type || base.contained!=hierarchy.count-1)) {out={};return false;}
    out.count=hierarchy.count;out.flags=hierarchy.flags;out.object_offset=col.offset;
    out.constructor_offset=col.constructor;out.contained=base.contained;out.attributes=base.flags;
    out.member_offset=base.member;out.vbtable_offset=base.vbtable;out.virtual_displacement=base.displacement;
    return true;
}
}
