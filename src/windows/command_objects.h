#pragma once
#include "../command_catalog.h"
#include "fdkss/api.h"
#include <windows.h>
#include <charconv>
#include <string_view>
#include <algorithm>
#include <cstdio>

namespace command_objects {
#define FDKSS_COMMAND_GROUP(group,table,scan,capacity,mask,offset,type) {group,table,scan,capacity,mask,offset,type},
inline constexpr command_catalog::Profile first[]{
#include "fdkss/sky1_command_groups.inc"
},second[]{
#include "fdkss/sky2_command_groups.inc"
};
#undef FDKSS_COMMAND_GROUP
static_assert(std::size(first)==28 && std::size(second)==28);
inline bool named(std::string_view root) {return root.starts_with("script_group:");}
struct Object {std::uintptr_t address=0;unsigned group=0,slots=0,implemented=0;const char *type=nullptr;};
inline bool resolve(std::uintptr_t image,bool sky2,std::string_view root,Object &out) {
    out={};
    if (!named(root)) return false;
    const auto number=root.substr(13);unsigned group=0;
    if (number.empty() || (number.size()>1 && number[0]=='0')) return false;
    const auto parsed=std::from_chars(number.data(),number.data()+number.size(),group);
    if(parsed.ec!=std::errc{} || parsed.ptr!=number.data()+number.size() || group>=28) return false;
    auto read=[](std::uintptr_t address,void *dest,std::size_t length){SIZE_T copied=0;return address && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void *>(address),dest,length,&copied) && copied==length;};
    auto pointer=[&](std::uintptr_t address){std::uintptr_t value=0;read(address,&value,8);return value;};
    const auto framework=pointer(image+(sky2 ? 0xc60ec0 : 0x8ff030));DWORD worker=0;
    if(!framework || pointer(framework)!=image+(sky2 ? 0xb1ad08 : 0x802d80) ||
        !read(framework+(sky2 ? 0xa0 : 0x68),&worker,4) || worker!=GetCurrentThreadId()) return false;
    const auto manager_slot=image+(sky2 ? 0xc60e58 : 0x8fefc8), manager=pointer(manager_slot);
    if(!manager || pointer(manager)!=image+(sky2 ? 0xb12fd0 : 0x7fc400))return false;
    IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS64 pe{};
    if(!read(image,&dos,sizeof(dos)) || dos.e_magic!=IMAGE_DOS_SIGNATURE || dos.e_lfanew<0 || dos.e_lfanew>0x100000 ||
        !read(image+dos.e_lfanew,&pe,sizeof(pe)) || pe.Signature!=IMAGE_NT_SIGNATURE ||
        pe.OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC || !pe.OptionalHeader.SizeOfImage || pe.OptionalHeader.SizeOfImage>512*1024*1024)return false;
    const auto address=pointer(manager+0x20+group*8);const auto &profile=(sky2 ? second : first)[group];
    std::vector<command_catalog::Entry> entries;
    if(!command_catalog::snapshot(image,address,pe.OptionalHeader.SizeOfImage,profile,read,entries))return false;
    for(const auto &entry : entries) if(entry.words[0]) {
        MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQuery(reinterpret_cast<void *>(entry.words[0]),&region,sizeof(region)) || region.State!=MEM_COMMIT ||
            (region.Protect & (PAGE_GUARD|PAGE_NOACCESS)) ||
            !(region.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
    }
    if(pointer(manager_slot)!=manager || pointer(manager+0x20+group*8)!=address)return false;
    out={address,group,profile.capacity,static_cast<unsigned>(std::count_if(entries.begin(),entries.end(),[](const auto &entry){return entry.words[0]!=0;})),profile.type};
    return true;
}
inline int get(std::uintptr_t image,bool sky2,const fdkss_value *args,std::size_t count,
    fdkss_result_fn emit,void *sink,char *error,std::size_t capacity) {
    auto fail=[&](const char *text){std::snprintf(error,capacity,"%s",text);return 1;};
    if(count!=2 || args[0].type!=FDKSS_STRING || args[1].type!=FDKSS_STRING)return fail("expected command group root and property strings");
    Object object;
    if(!resolve(image,sky2,{args[0].string,args[0].string_size},object))return fail("command group root/profile unavailable on the owning thread");
    const std::string_view field(args[1].string,args[1].string_size);fdkss_value value{};char address[32]{};
    if(field=="class") {value.type=FDKSS_STRING;value.string=object.type;value.string_size=std::strlen(object.type);}
    else if(field=="address") {std::snprintf(address,sizeof(address),"0x%llx",static_cast<unsigned long long>(object.address));value.type=FDKSS_STRING;value.string=address;value.string_size=std::strlen(address);}
    else {
        value.type=FDKSS_INTEGER;
        if(field=="group")value.integer=object.group;
        else if(field=="slot_count")value.integer=object.slots;
        else if(field=="implemented_count")value.integer=object.implemented;
        else return fail("unknown command group property; metadata is read-only");
    }
    return emit(sink,&value);
}
}
