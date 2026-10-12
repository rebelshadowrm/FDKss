#pragma once
#include "fdkss/api.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

// Copied resource values only. The intermediate status structures have no
// verified RTTI or public lifetime, so they never become object handles.
namespace player_resources {
struct Profile { std::uintptr_t table, link, getter; std::string_view code; };
struct SetterProfile { std::uintptr_t entry; std::string_view code; };
using Setter=void (__fastcall *)(void *,std::uint32_t,std::int32_t);
// Full getter dispatch and tables, independently checked against both originals.
#include "player_resources_profiles.inc"
#include "resource_setter_profiles.inc"
inline bool read(std::uintptr_t address, void *out, std::size_t size) {
    SIZE_T copied=0;
    return address && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void *>(address),out,size,&copied) && copied==size;
}
inline bool pinned(std::uintptr_t image,std::uintptr_t entry,std::string_view code) {
    std::array<unsigned char,2048> bytes{};
    if(code.size()%2 || code.size()/2>bytes.size() || !read(image+entry,bytes.data(),code.size()/2))return false;
    const auto nibble=[](char c){return c<='9' ? c-'0' : c-'a'+10;};
    for(std::size_t i=0;i<code.size()/2;++i)
        if(bytes[i]!=nibble(code[2*i])*16+nibble(code[2*i+1]))return false;
    return true;
}
inline bool pinned(std::uintptr_t image,const Profile &p){return pinned(image,p.getter,p.code);}
template<class Resolve> int snapshot(std::uintptr_t image,const Profile &p,Resolve resolve,
    std::size_t count,fdkss_result_fn emit,void *sink,char *error,std::size_t capacity) {
    const auto fail=[&](const char *message){std::snprintf(error,capacity,"%s",message);return 1;};
    if(count || !emit)return fail("player_resources expects no arguments");
    if(!pinned(image,p))return fail("player resource getter profile changed");
    fdkss_value value{};
    const auto actor=resolve();
    if(!actor)return emit(sink,&value);
    std::uintptr_t table=0,link=0,status=0;
    if(!read(actor,&table,8) || table!=image+p.table || !read(actor+p.link,&link,8))
        return fail("player resource actor is invalid or unreadable");
    if(!link)return emit(sink,&value);
    if(!read(link,&status,8))return fail("player resource link is unreadable");
    if(!status)return emit(sink,&value);
    std::array<std::uint32_t,6> values{},again{};
    if(!read(status+0xc,values.data(),sizeof(values)))return fail("player resources are unreadable");
    for(std::size_t i=0;i<values.size();i+=2)
        if(values[i]>values[i+1] || values[i+1]>static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
            return fail("player resources are outside the verified range");
    std::uintptr_t current_link=0,current_status=0,current_table=0;
    if(resolve()!=actor || !read(actor,&current_table,8) || current_table!=table ||
        !read(actor+p.link,&current_link,8) || current_link!=link ||
        !read(link,&current_status,8) || current_status!=status ||
        !read(status+0xc,again.data(),sizeof(again)) || again!=values)
        return fail("player resources changed during the snapshot");
    // Emit only after the complete chain and copy have passed validation.
    value.type=FDKSS_INTEGER;
    for(const auto item:values){value.integer=item;if(emit(sink,&value))return 1;}
    return 0;
}
inline bool writable(std::uintptr_t address,std::size_t size) {
    MEMORY_BASIC_INFORMATION region{};
    if(!VirtualQuery(reinterpret_cast<void *>(address),&region,sizeof(region)) || region.State!=MEM_COMMIT ||
        region.Protect&(PAGE_GUARD|PAGE_NOACCESS) ||
        !(region.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
    const auto begin=reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    return address>=begin && address-begin<=region.RegionSize && size<=region.RegionSize-(address-begin);
}
template<class Resolve> int edit(std::uintptr_t image,const Profile &p,const SetterProfile &setter,Resolve resolve,
    const fdkss_value *args,std::size_t count,fdkss_result_fn emit,void *sink,char *error,std::size_t capacity,
    Setter (*checked_target)(std::uintptr_t,const SetterProfile &)=nullptr) {
    const auto fail=[&](const char *message){std::snprintf(error,capacity,"%s",message);return 1;};
    if(count!=2 || !args || !emit || args[0].type!=FDKSS_STRING || args[1].type!=FDKSS_INTEGER ||
        args[1].integer<std::numeric_limits<std::int32_t>::min() || args[1].integer>std::numeric_limits<std::int32_t>::max())
        return fail("set_player_resource expects hp/ep/cp and a signed 32-bit integer");
    const std::string_view name(args[0].string,args[0].string_size);
    const std::uint32_t selector=name=="hp" ? 7u : name=="ep" ? 9u : name=="cp" ? 11u : 0u;
    if(!selector)return fail("unknown player resource; expected hp/ep/cp");
    if(!pinned(image,p))return fail("player resource getter profile changed");
    const auto invoke=checked_target ? checked_target(image,setter) :
        (pinned(image,setter.entry,setter.code) ? reinterpret_cast<Setter>(image+setter.entry) : nullptr);
    if(!invoke)return fail("player resource setter profile changed");
    MEMORY_BASIC_INFORMATION code{};const auto target=image+setter.entry;
    if(!VirtualQuery(reinterpret_cast<void *>(target),&code,sizeof(code)) || code.State!=MEM_COMMIT ||
        code.Protect&(PAGE_GUARD|PAGE_NOACCESS) ||
        !(code.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))
        return fail("player resource setter is not executable");
    const auto actor=resolve();std::uintptr_t table=0,link=0,status=0;
    if(!actor || !read(actor,&table,8) || table!=image+p.table || !read(actor+p.link,&link,8) ||
        !link || !read(link,&status,8) || !status || status%4)
        return fail("current player resource chain is unavailable");
    std::array<std::uint32_t,9> before{},again{};
    if(!read(status,before.data(),sizeof(before)))return fail("player resources are unreadable");
    for(std::size_t i=3;i<before.size();i+=2)
        if(before[i]>before[i+1] || before[i+1]>static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
            return fail("player resources are outside the verified range");
    const auto index=static_cast<std::size_t>(3+(selector-7));
    if(!writable(status+index*4,4))return fail("player resource field is not writable");
    const auto stable=[&]{
        std::uintptr_t current_table=0,current_link=0,current_status=0;
        return resolve()==actor && read(actor,&current_table,8) && current_table==table &&
            read(actor+p.link,&current_link,8) && current_link==link && read(link,&current_status,8) && current_status==status;
    };
    if(!stable() || !read(status,again.data(),sizeof(again)) || again!=before)
        return fail("player resources changed before editing");
    const auto requested=static_cast<std::int32_t>(args[1].integer);
    const auto maximum=static_cast<std::int32_t>(before[index+1]);
    const auto expected=requested<0 ? 0 : requested>maximum ? maximum : requested;
    // Only selectors 7/9/11 are permitted. Their pinned paths use RCX status,
    // EDX selector, R8D signed request; they clamp and write exactly one field.
    invoke(reinterpret_cast<void *>(status),selector,requested);
    before[index]=static_cast<std::uint32_t>(expected);
    if(!stable() || !read(status,again.data(),sizeof(again)) || again!=before)
        return fail("native resource setter result or surrounding fields changed unexpectedly");
    fdkss_value value{};value.type=FDKSS_INTEGER;value.integer=expected;
    return emit(sink,&value);
}
}
