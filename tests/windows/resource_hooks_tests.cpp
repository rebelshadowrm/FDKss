#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "resource_hooks.h"
#include "player_resources.h"
#include "resource_properties.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
namespace {
using Target=std::uint64_t (__fastcall *)(void *,std::uint32_t,std::int32_t);
Target target=nullptr;
std::array<std::uint32_t,9> data{42,68,77,100,1000,200,500,50,200};
void check(bool good,const char *message){if(!good)throw std::runtime_error(message);}
void bytes(std::uintptr_t at,std::string_view hex) {
    auto nibble=[](char c){return c<='9' ? c-'0' : c-'a'+10;};
    auto *out=reinterpret_cast<unsigned char *>(at);
    for(std::size_t i=0;i<hex.size()/2;++i)out[i]=static_cast<unsigned char>(nibble(hex[i*2])*16+nibble(hex[i*2+1]));
}
struct Case {std::uint32_t selector;std::int32_t request;std::array<std::uint32_t,9> before,after;std::uint64_t result;};
}
int main(int argc,char **argv) {
    bool second=false,third_party=false;
    for(int i=1;i<argc;++i){second|=std::string_view(argv[i])=="sky2";third_party|=std::string_view(argv[i])=="third-party";}
    const auto root=std::filesystem::temp_directory_path()/("fdkss-resource-hooks-"+std::to_string(GetCurrentProcessId()));
    auto *memory=static_cast<unsigned char *>(VirtualAlloc(nullptr,0xc61000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    unsigned char *page=nullptr;
    fdkss_runtime *runtime=nullptr;
    try {
        check(memory!=nullptr,"image allocation");const auto base=reinterpret_cast<std::uintptr_t>(memory);
        const auto &profile=second ? player_resources::sky2_setter : player_resources::sky1_setter;
        bytes(base+profile.entry,profile.code);target=reinterpret_cast<Target>(base+profile.entry);
        std::vector<Case> cases;
        for(const std::uint32_t maximum : {0u,200u,2147483647u})
            for(std::uint32_t selector=7;selector<=12;++selector)
                for(const std::int32_t request : {std::numeric_limits<std::int32_t>::min(),-250,-1,0,25,250,std::numeric_limits<std::int32_t>::max()}) {
                    data={42,68,77,maximum/2,maximum,maximum/2,maximum,maximum/2,maximum};
                    const auto before=data;const auto result=target(data.data(),selector,request);
                    const auto field=3+((selector-7)/2)*2;
                    const auto sum=selector%2 ? static_cast<std::uint32_t>(request) : before[field]+static_cast<std::uint32_t>(request);
                    // Original x64 add wraps to 32 bits before the signed clamp.
                    const auto signed_value=static_cast<std::int64_t>(sum<0x80000000u ? sum : static_cast<std::int64_t>(sum)-0x100000000ll);
                    const auto expected=static_cast<std::uint32_t>(signed_value<0 ? 0 : signed_value>maximum ? maximum : signed_value);
                    auto after=before;after[field]=expected;
                    check(data==after,"original selected setter/add path semantics");
                    cases.push_back({selector,request,before,after,result});
                }
        std::filesystem::create_directories(root/"Probe/Scripts");std::ofstream(root/"Probe/enabled.txt");
        std::ofstream(root/"Probe/Scripts/main.lua")<<R"(
            pre,post,errors,bad_calls=0,0,0,0
            fdk.hook('status.resource_change','pre',function(address,word,level,name,operation,request,before,maximum)
                assert(type(address)=='string' and word==42 and level==68)
                assert(name=='hp' or name=='ep' or name=='cp')
                assert(operation=='set' or operation=='add')
                assert(math.type(request)=='integer' and before<=maximum)
                pre=pre+1
                if recursive then fdk.invoke('fixture.direct') end
                if corrupt then fdk.invoke('fixture.corrupt') end
                return 'ignored replacement'
            end)
            fdk.hook('status.resource_change','post',function(address,word,level,name,operation,request,before,maximum,after,after_max,error)
                post=post+1
                if error then assert(after==nil and after_max==nil);errors=errors+1
                else assert(after<=after_max) end
                return 0
            end)
            fdk.hook('status.resource_change','pre',function()
                if fail_callback then bad_calls=bad_calls+1;error('fixture') end
            end)
        )";
        const auto path=root.u8string();check(fdkss_create(reinterpret_cast<const char *>(path.c_str()),nullptr,nullptr,&runtime)==FDKSS_OK,"runtime create");
        check(fdkss_register_typed_native(runtime,"engine.hook_supported",+[](void *,const fdkss_value *,std::size_t,fdkss_result_fn emit,void *sink,char *,std::size_t)->int {
            fdkss_value value{};value.type=FDKSS_BOOLEAN;value.boolean=resource_hooks::active();return emit(sink,&value);
        },nullptr)==FDKSS_OK,"hook support");
        check(fdkss_register_typed_native(runtime,"fixture.direct",+[](void *,const fdkss_value *,std::size_t,fdkss_result_fn,void *,char *,std::size_t)->int {
            target(data.data(),11,50);return 0;
        },nullptr)==FDKSS_OK,"native bypass fixture");
        check(fdkss_register_typed_native(runtime,"fixture.corrupt",+[](void *,const fdkss_value *,std::size_t,fdkss_result_fn,void *,char *,std::size_t)->int {
            data[0]=43;return 0;
        },nullptr)==FDKSS_OK,"post-copy error fixture");
        check(fdkss_register_typed_native(runtime,"engine.resource_hook_info",resource_hooks::info,nullptr)==FDKSS_OK,"info binding");
        memory[profile.entry+100]^=1;check(!resource_hooks::start(base,second,runtime,GetCurrentThreadId()),"changed body accepted");memory[profile.entry+100]^=1;
        check(resource_hooks::start(base,second,runtime,GetCurrentThreadId()),"conditional entry hook installation");
        check(fdkss_start(runtime)==FDKSS_OK,"runtime start");
        char result[4096]{};auto lua=[&](const char *source){if(fdkss_execute(runtime,"Probe",source,result,sizeof(result))!=FDKSS_OK)throw std::runtime_error(std::string("Lua execution: ")+source+" — "+fdkss_last_error(runtime));return std::string(result);};
        for(const auto &item:cases) {
            data=item.before;const auto actual=target(data.data(),item.selector,item.request);
            check(data==item.after && actual==item.result,"hook changed original effect or RAX result");
        }
        const auto expected_count=std::to_string(cases.size());
        check(lua("return pre,post,errors")==expected_count+"\t"+expected_count+"\t0","paired selected observers");
        check(memory[profile.entry]==0xe9,"unexpected MinHook entry encoding");
        std::int32_t relay_offset=0;std::memcpy(&relay_offset,memory+profile.entry+1,4);
        const auto saved_relay=reinterpret_cast<Target>(base+profile.entry+5+relay_offset);
        data={42,68,77,100,1000,200,500,50,200};
        const auto direct=resource_hooks::checked_target(base,profile);check(direct && reinterpret_cast<Target>(direct)!=target,"owned trampoline unavailable");
        direct(data.data(),11,75);check(data[7]==75,"explicit original call effect");
        const auto &getter=second ? player_resources::sky2 : player_resources::sky1;
        bytes(base+getter.getter,getter.code);
        alignas(16) std::array<unsigned char,0x2600> actor{};
        const auto actor_address=reinterpret_cast<std::uintptr_t>(actor.data());
        const auto table=base+getter.table,status_address=reinterpret_cast<std::uintptr_t>(data.data());
        auto link=status_address;const auto link_address=reinterpret_cast<std::uintptr_t>(&link);
        std::memcpy(actor.data(),&table,8);std::memcpy(actor.data()+getter.link,&link_address,8);
        fdkss_value args[2]{};args[0].type=FDKSS_STRING;args[0].string="cp";args[0].string_size=2;
        args[1].type=FDKSS_INTEGER;args[1].integer=85;
        std::int64_t edited=-1;char edit_error[256]{};
        const auto emit=+[](void *sink,const fdkss_value *value)->int {check(value->type==FDKSS_INTEGER,"edit result type");*static_cast<std::int64_t *>(sink)=value->integer;return 0;};
        check(!player_resources::edit(base,getter,profile,[&]{return actor_address;},args,2,emit,&edited,edit_error,sizeof(edit_error),resource_hooks::checked_target) && edited==85 && data[7]==85,"safe edit rejected owned hook or changed resource");
        fdkss_value property_args[3]{};property_args[0].type=FDKSS_STRING;
        property_args[0].string="player";property_args[0].string_size=6;
        property_args[1]=args[0];property_args[2]=args[1];property_args[2].integer=95;
        const auto resolve=[&](std::string_view reference){return reference=="player" || reference=="@fixture" ? actor_address : std::uintptr_t{0};};
        check(!resource_properties::set(base,getter,profile,resolve,property_args,emit,&edited,edit_error,sizeof(edit_error),resource_hooks::checked_target) && edited==95 && data[7]==95,"player property edit rejected owned hook");
        property_args[0].string="@fixture";property_args[0].string_size=8;property_args[2].integer=105;
        check(!resource_properties::set(base,getter,profile,resolve,property_args,emit,&edited,edit_error,sizeof(edit_error),resource_hooks::checked_target) && edited==105 && data[7]==105,"scene-reference property edit rejected owned hook");
        lua("return fdk.invoke('fixture.direct')");check(data[7]==50,"native call effect");
        std::thread foreign([&]{target(data.data(),11,60);});foreign.join();check(data[7]==60,"foreign original call lost");
        target(nullptr,0xffffffffu,123);check(lua("return pre,post")==expected_count+"\t"+expected_count,"bypass emitted observers");
        lua("recursive=true");target(data.data(),11,72);check(data[7]==72,"nested original effect");
        check(lua("recursive=false;return pre,post")==std::to_string(cases.size()+1)+"\t"+std::to_string(cases.size()+1),"nested observer recursion");
        const auto diagnostics=lua("return fdk.invoke('engine.resource_hook_info')");
        for(const auto *field : {"\"foreign\":1","\"nested\":1","\"native\":1","\"unselected\":1"})check(diagnostics.find(field)!=std::string::npos,"bypass telemetry");
        page=static_cast<unsigned char *>(VirtualAlloc(nullptr,0x2000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));check(page!=nullptr,"partial prefix allocation");
        DWORD previous=0;check(VirtualProtect(page+0x1000,0x1000,PAGE_NOACCESS,&previous)!=0,"prefix guard");
        auto *short_status=reinterpret_cast<std::uint32_t *>(page+0x1000-20);short_status[3]=50;short_status[4]=100;
        target(short_status,7,75);check(short_status[3]==75,"short prefix bypass changed original");
        data[7]=201;data[8]=200;target(data.data(),7,80);check(data[3]==80,"invalid prefix bypass changed original");
        const auto guarded=lua("return fdk.invoke('engine.resource_hook_info')");
        check(guarded.find("\"unreadable\":1")!=std::string::npos && guarded.find("\"invalid\":1")!=std::string::npos,"copy guard counters");
        data[7]=50;
        lua("corrupt=true");target(data.data(),11,65);data[0]=42;
        check(lua("corrupt=false;return errors")=="1","paired post-copy failure missing");
        lua("fail_callback=true");
        target(data.data(),12,10);target(data.data(),12,10);check(data[7]==85 && lua("return bad_calls")=="1","callback error containment/original once");
        check(fdkss_reload(runtime,"Probe")==FDKSS_OK,"reload");target(data.data(),11,75);check(lua("return pre,post")=="1\t1","reload observer state");
        memory[profile.entry+100]^=1;check(!resource_hooks::checked_target(base,profile),"foreign tail accepted by explicit call");
        const auto unedited=data;
        check(resource_properties::set(base,getter,profile,resolve,property_args,emit,&edited,edit_error,sizeof(edit_error),resource_hooks::checked_target) && data==unedited,"property edit accepted changed tail or mutated on refusal");
        target(data.data(),11,80);check(lua("return pre,post")=="1\t1","changed tail observed");memory[profile.entry+100]^=1;
        std::array<unsigned char,9> replacement{0xc3,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90};
        if(third_party){std::memcpy(memory+profile.entry,replacement.data(),replacement.size());check(!resource_hooks::checked_target(base,profile),"foreign entry accepted");}
        check(resource_hooks::stop() && !resource_hooks::active(),"hook stop");
        if(third_party)check(!std::memcmp(memory+profile.entry,replacement.data(),replacement.size()),"foreign patch overwritten");
        else {std::vector<unsigned char> expected(profile.code.size()/2);bytes(reinterpret_cast<std::uintptr_t>(expected.data()),profile.code);check(!std::memcmp(memory+profile.entry,expected.data(),expected.size()),"original body not restored");}
        saved_relay(data.data(),11,90);check(data[7]==90,"saved stopped thunk original forwarding");
        check(fdkss_destroy(runtime)==FDKSS_OK,"runtime destroy");runtime=nullptr;
        saved_relay(data.data(),11,95);check(data[7]==95,"saved thunk used destroyed runtime");
        VirtualFree(page,0,MEM_RELEASE);VirtualFree(memory,0,MEM_RELEASE);std::filesystem::remove_all(root);
        std::cout<<"Resource hook original/native fixtures passed: "<<cases.size()<<" cases\n";
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';resource_hooks::stop();if(runtime)fdkss_destroy(runtime);if(page)VirtualFree(page,0,MEM_RELEASE);VirtualFree(memory,0,MEM_RELEASE);std::filesystem::remove_all(root);return 1;}
}
