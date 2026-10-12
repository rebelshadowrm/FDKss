#pragma once
#include "fdkss/api.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace character_models {
struct Profile {std::uintptr_t actor_table,link,model_table;std::uint32_t maximum_version;};
inline constexpr Profile first{0x7eb7d0,0x2a8,0x800db0,4};
inline constexpr Profile second{0xafdec8,0x2e0,0xb18c80,5};
inline constexpr Profile object_first{0x7f2278,0x190,0x800db0,4};
inline constexpr Profile object_second{0xb048b8,0x190,0xb18c80,5};
struct Copy {
    std::uintptr_t table=0;
    std::uint32_t magic=0,version=0;
    std::array<char,64> name{};
    std::uint64_t meshes=0,nodes=0;
    bool operator==(const Copy &) const = default;
};
// Owner validation belongs to the adapter. Only copied metadata escapes; the
// linked Model never becomes a retained handle or arbitrary address input.
template<class Resolve,class Read,class Class> int snapshot(std::uintptr_t image,const Profile &p,
    Resolve resolve,Read read,Class model_class,const fdkss_value *args,std::size_t count,
    fdkss_result_fn emit,void *sink,char *error,std::size_t capacity,const Profile *additional=nullptr) {
    const auto fail=[&](const char *message){std::snprintf(error,capacity,"%s",message);return 1;};
    if(count!=1 || !args || !emit || args[0].type!=FDKSS_STRING || !args[0].string)
        return fail("model snapshot expects one current actor reference");
    const std::string_view reference(args[0].string,args[0].string_size);
    if(reference!="player" && (reference.empty() || reference.front()!='@'))
        return fail("model snapshot expects player or a scene actor reference");
    const auto actor=resolve(reference);
    fdkss_value value{};
    if(!actor) {
        if(reference!="player")return fail("model snapshot scene reference expired");
        if(resolve(reference))return fail("model snapshot player changed");
        return emit(sink,&value);
    }
    std::uintptr_t table=0,model=0;
    if(!read(actor,&table,8))return fail("model snapshot actor is unreadable");
    const auto *selected=table==image+p.actor_table ? &p :
        additional && table==image+additional->actor_table ? additional : nullptr;
    if(!selected || !read(actor+selected->link,&model,8))return fail("model snapshot requires a covered Model owner");
    const auto stable=[&] {
        std::uintptr_t current_table=0,current_model=0;
        return resolve(reference)==actor && read(actor,&current_table,8) && current_table==table &&
            read(actor+selected->link,&current_model,8) && current_model==model;
    };
    if(!model) {
        if(!stable())return fail("character model link changed");
        return emit(sink,&value);
    }
    const auto copy=[&](Copy &out) {
        return read(model,&out.table,8) && out.table==image+selected->model_table &&
            model_class(model) && read(model+0x10,&out.magic,4) && out.magic==0x204c444d &&
            read(model+0x14,&out.version,4) && out.version>=4 && out.version<=selected->maximum_version &&
            read(model+0x1c,out.name.data(),out.name.size()) && std::memchr(out.name.data(),0,out.name.size()) &&
            read(model+0x258,&out.meshes,8) && out.meshes<=10000 &&
            read(model+0x288,&out.nodes,8) && out.nodes<=10000;
    };
    Copy before{},after{};
    if(!copy(before) || !stable() || !copy(after) || before!=after || !stable())
        return fail("character model metadata is unavailable, invalid or changed");
    value.type=FDKSS_STRING;value.string=before.name.data();value.string_size=std::strlen(before.name.data());
    if(emit(sink,&value))return 1;
    for(const auto number:{static_cast<std::uint64_t>(before.version),before.meshes,before.nodes}) {
        value={};value.type=FDKSS_INTEGER;value.integer=static_cast<std::int64_t>(number);
        if(emit(sink,&value))return 1;
    }
    return 0;
}
}
