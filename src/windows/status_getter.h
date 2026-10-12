#pragma once
#include "player_resources.h"

namespace status_getter {
struct Field {std::uint32_t selector,offset;};
inline constexpr std::array<Field,9> fields{{{0,4},{7,12},{9,20},{11,28},
    {80,36},{81,40},{82,44},{83,48},{84,52}}};
inline bool executable(std::uintptr_t address,std::size_t length) {
    MEMORY_BASIC_INFORMATION region{};
    if(!VirtualQuery(reinterpret_cast<void *>(address),&region,sizeof(region)) || region.State!=MEM_COMMIT ||
        region.Protect&(PAGE_GUARD|PAGE_NOACCESS) ||
        !(region.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
    const auto begin=reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    return address>=begin && address-begin<=region.RegionSize && length<=region.RegionSize-(address-begin);
}
// Owner-thread validation and generation-stamped reference resolution belong to
// the adapter. Only fully pinned leaf paths are callable through this binding.
template<class Resolve> int invoke(std::uintptr_t image,const player_resources::Profile &p,Resolve resolve,
    const fdkss_value *args,std::size_t count,fdkss_result_fn emit,void *sink,char *error,std::size_t capacity) {
    using player_resources::read;
    const auto fail=[&](const char *message){std::snprintf(error,capacity,"%s",message);return 1;};
    if(count!=2 || !args || !emit || args[0].type!=FDKSS_STRING || !args[0].string || args[1].type!=FDKSS_INTEGER)
        return fail("status_value expects a current character reference and an integer selector");
    const std::string_view reference(args[0].string,args[0].string_size);
    if(reference!="player" && (reference.empty() || reference.front()!='@'))
        return fail("status_value expects player or a scene actor reference");
    const Field *field=nullptr;
    for(const auto &entry:fields)if(args[1].integer==entry.selector){field=&entry;break;}
    if(!field)return fail("status_value supports only raw selectors 0,7,9,11,80,81,82,83,84");
    const auto target=image+p.getter;
    if(!player_resources::pinned(image,p) || !executable(target,p.code.size()/2))
        return fail("native status getter profile changed or is not executable");
    const auto actor=resolve(reference);std::uintptr_t table=0,link=0,status=0;
    if(!actor || !read(actor,&table,8) || table!=image+p.table || !read(actor+p.link,&link,8))
        return fail("status_value reference is stale or is not a covered character");
    fdkss_value result{};
    if(!link)return emit(sink,&result);
    if(!read(link,&status,8))return fail("character status link is unreadable");
    if(!status)return emit(sink,&result);
    if(status%4)return fail("character status is not aligned");
    std::array<std::uint32_t,14> before{},again{};
    if(!read(status,before.data(),sizeof(before)))return fail("character status prefix is unreadable");
    const auto stable=[&]{
        std::uintptr_t new_table=0,new_link=0,new_status=0;
        return resolve(reference)==actor && read(actor,&new_table,8) && new_table==table &&
            read(actor+p.link,&new_link,8) && new_link==link && read(link,&new_status,8) && new_status==status;
    };
    if(!stable() || !read(status,again.data(),sizeof(again)) || again!=before ||
        !player_resources::pinned(image,p) || !executable(target,p.code.size()/2))
        return fail("character status or native getter changed before invocation");
    using Getter=std::uint32_t (__fastcall *)(const void *,std::uint32_t);
    const auto value=reinterpret_cast<Getter>(target)(reinterpret_cast<const void *>(status),field->selector);
    if(!stable() || !read(status,again.data(),sizeof(again)) || again!=before || value!=before[field->offset/4])
        return fail("native status result or character chain changed during invocation");
    result.type=FDKSS_INTEGER;result.integer=value;return emit(sink,&result);
}
}
