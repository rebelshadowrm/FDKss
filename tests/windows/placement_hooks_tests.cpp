#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "placement_hooks.h"
#include "placement_profiles.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
namespace {
std::atomic<unsigned> math_calls{0};
void *actor=nullptr;
character_placement::Setter target=nullptr;
character_placement::Placement pose{0x02000000,1.25f,2.5f,3.75f,.5f};
float __fastcall sine(float value){++math_calls;return std::sin(value);}
float __fastcall cosine(float value){++math_calls;return std::cos(value);}
void check(bool good,const char *message){if(!good)throw std::runtime_error(message);}
void bytes(std::uintptr_t at,const char *hex) {
    auto nibble=[](char c){return c<='9' ? c-'0' : c-'a'+10;};
    auto *out=reinterpret_cast<unsigned char *>(at);
    for(std::size_t i=0;i<std::strlen(hex)/2;++i)out[i]=static_cast<unsigned char>(nibble(hex[i*2])*16+nibble(hex[i*2+1]));
}
}
int main(int argc,char **argv) {
    const bool second=argc>1 && std::string_view(argv[1])=="sky2";
    const bool third_party=argc>1 && std::string_view(argv[1])=="third-party";
    const auto root=std::filesystem::temp_directory_path()/("fdkss-placement-hooks-"+std::to_string(GetCurrentProcessId()));
    auto *memory=static_cast<unsigned char *>(VirtualAlloc(nullptr,0xc61000,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    fdkss_runtime *runtime=nullptr;
    try {
        check(memory!=nullptr,"image allocation");const auto base=reinterpret_cast<std::uintptr_t>(memory);
        const auto &profile=second ? placement_hooks::sky2 : placement_hooks::sky1;
        bytes(base+profile.handler,profile.code);
        auto bridge=[&](std::uintptr_t at,void *function) {
            const unsigned char jump[]{0x48,0xb8,0,0,0,0,0,0,0,0,0xff,0xe0};
            std::memcpy(reinterpret_cast<void *>(at),jump,sizeof(jump));std::memcpy(reinterpret_cast<void *>(at+2),&function,8);
        };
        bridge(base+(second ? 0x88b0a0 : 0x75fa50),reinterpret_cast<void *>(sine));
        bridge(base+(second ? 0x889ab0 : 0x75bc10),reinterpret_cast<void *>(cosine));
        const float half=.5f;std::memcpy(memory+(second ? 0xb2d9b8 : 0x814a2c),&half,4);
        alignas(16) std::array<unsigned char,0x2600> storage{};actor=storage.data();
        const auto table=base+profile.table;std::memcpy(actor,&table,8);target=reinterpret_cast<character_placement::Setter>(base+profile.handler);
        std::filesystem::create_directories(root/"Probe/Scripts");std::ofstream(root/"Probe/enabled.txt");
        std::ofstream(root/"Probe/Scripts/main.lua")<<R"(
            pre,post,bad,mark,reentrant,flags_only=0,0,0,0,0,0
            local function observe(address,class,flags,...)
                assert(type(address)=='string' and class=='.?AVActor@chara@sora@@')
                local p=table.pack(...)
                if flags==0x01000000 then assert(p.n==0);mark=mark+1
                elseif flags==0 or flags==0x80000000 then assert(p.n==0);flags_only=flags_only+1
                elseif p[1]==nil then assert(p.n==2 and p[2]=='placement pose could not be copied');bad=bad+1
                else assert(flags==0x02000000 and p.n==4 and p[1]==1.25 and p[2]==2.5 and p[3]==3.75 and p[4]==.5) end
            end
            fdk.hook('actor.set_placement','pre',function(...)
                pre=pre+1;observe(...)
                if recursive then fdk.call('fixture.recursive') end
                if probing then assert(not pcall(fdk.invoke,'engine.placement_hook_probe'));reentrant=reentrant+1 end
                return 0 -- Observer only.
            end)
            fdk.hook('actor.set_placement','post',function(...) post=post+1;observe(...);return 0 end)
        )";
        const auto path=root.u8string();check(fdkss_create(reinterpret_cast<const char *>(path.c_str()),nullptr,nullptr,&runtime)==FDKSS_OK,"runtime create");
        check(fdkss_register_typed_native(runtime,"engine.hook_supported",+[](void *,const fdkss_value *,std::size_t,fdkss_result_fn emit,void *sink,char *,std::size_t)->int {
            fdkss_value value{};value.type=FDKSS_BOOLEAN;value.boolean=placement_hooks::active();return emit(sink,&value);
        },nullptr)==FDKSS_OK,"hook support binding");
        check(fdkss_register_native(runtime,"fixture.recursive",+[](void *,const char *,char *out,std::size_t size)->int{target(actor,&pose);if(size)*out=0;return 0;},nullptr)==FDKSS_OK,"recursive binding");
        check(fdkss_register_typed_native(runtime,"fixture.direct",+[](void *,const fdkss_value *,std::size_t,fdkss_result_fn,void *,char *,std::size_t)->int{target(actor,&pose);return 0;},nullptr)==FDKSS_OK,"typed native binding");
        check(fdkss_register_typed_native(runtime,"engine.placement_hook_probe",placement_hooks::request_probe,nullptr)==FDKSS_OK &&
            fdkss_register_typed_native(runtime,"engine.placement_hook_info",placement_hooks::info,nullptr)==FDKSS_OK &&
            fdkss_register_typed_native(runtime,"engine.placement_hook_probe_result",placement_hooks::probe_result,nullptr)==FDKSS_OK,"probe bindings");
        memory[profile.handler+100]^=1;
        check(!placement_hooks::start(base,second,runtime,GetCurrentThreadId()),"changed body accepted");memory[profile.handler+100]^=1;
        check(placement_hooks::start(base,second,runtime,GetCurrentThreadId()) && fdkss_start(runtime)==FDKSS_OK,"hook/runtime start");
        char result[2048]{};auto lua=[&](const char *source){check(fdkss_execute(runtime,"Probe",source,result,sizeof(result))==FDKSS_OK,"Lua execution");return std::string(result);};
        const auto before=math_calls.load();target(actor,&pose);check(math_calls==before+2,"original placement was not called once");
        std::array<float,3> current{};std::memcpy(current.data(),storage.data()+0xd8,sizeof(current));check(current==std::array<float,3>{1.25f,2.5f,3.75f},"native pose effect");
        check(lua("return pre,post")=="1\t1","paired callbacks");
        const auto direct=character_placement::checked_target(base+profile.handler);check(direct && direct!=target,"owned target did not resolve trampoline");
        direct(actor,&pose);check(lua("return pre,post")=="1\t1","explicit API bypass");
        std::thread foreign([&]{target(actor,&pose);});foreign.join();check(lua("return pre,post")=="1\t1","foreign callback");
        const auto other_table=table+8;std::memcpy(actor,&other_table,8);
        const auto other_before=math_calls.load();target(actor,&pose);
        check(math_calls==other_before+2 && lua("return pre,post")=="1\t1","non-Chara bypass");
        check(lua("local s=fdk.invoke('engine.placement_hook_info');return s:find('\"foreign\":1',1,true)~=nil,s:find('\"wrong_class\":1',1,true)~=nil")=="true\ttrue","foreign/class bypass diagnostics");
        std::memcpy(actor,&table,8);
        const auto native_before=math_calls.load();lua("return fdk.invoke('fixture.direct')");
        check(math_calls==native_before+2 && lua("return pre,post")=="1\t1","native caller bypass");
        lua("recursive=true");const auto nested_before=math_calls.load();target(actor,&pose);check(math_calls==nested_before+4,"nested original calls");
        check(lua("recursive=false;return pre,post")=="2\t2","nested/busy callback suppression");
        const auto diagnostics=lua("return fdk.invoke('engine.placement_hook_info')");
        check(diagnostics.find("\"nested\":1")!=std::string::npos && diagnostics.find("\"native\":1")!=std::string::npos,("nested/native bypass diagnostics: "+diagnostics).c_str());
        const std::uint32_t mark=0x01000000;target(actor,reinterpret_cast<const character_placement::Placement *>(&mark));
        check(storage[second ? 0x2569 : 0x24e1]==1 && lua("return mark")=="2","flags-only invocation");
        auto *flags_page=static_cast<unsigned char *>(VirtualAlloc(nullptr,0x2000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        check(flags_page!=nullptr,"flags page allocation");DWORD previous_protection=0;
        check(VirtualProtect(flags_page+0x1000,0x1000,PAGE_NOACCESS,&previous_protection)!=0,"flags page guard");
        for(const std::uint32_t flag : {0u,0x80000000u}) {
            std::memcpy(flags_page+0xffc,&flag,4);
            target(actor,reinterpret_cast<const character_placement::Placement *>(flags_page+0xffc));
        }
        VirtualFree(flags_page,0,MEM_RELEASE);
        check(lua("return flags_only")=="4","flags-only payload read beyond four bytes or lost unsigned flag");
        auto invalid=pose;invalid.x=std::numeric_limits<float>::quiet_NaN();target(actor,&invalid);check(lua("return bad")=="2","nonfinite paired errors");
        check(lua("return fdk.invoke('engine.placement_hook_probe')")=="true","probe queue");
        check(lua("return pcall(fdk.invoke,'engine.placement_hook_probe')").starts_with("false\t"),"second pending probe");
        std::thread foreign_probe([]{placement_hooks::service_probe();});foreign_probe.join();
        check(lua("return fdk.invoke('engine.placement_hook_probe_result')")=="pending\t","foreign probe consumed queue");
        lua("probing=true");placement_hooks::service_probe();check(lua("probing=false;return reentrant")=="1","reentrant probe");
        check(lua("local r=table.pack(fdk.invoke('engine.placement_hook_probe_result'));return r.n,r[1],r[3],r[4],r[5],r[6]")=="10\tsucceeded\t1.25\t2.5\t3.75\t0.5","private probe result");
        memory[profile.handler+100]^=1;check(!character_placement::checked_target(base+profile.handler),"changed tail API accepted");
        lua("return fdk.invoke('engine.placement_hook_probe')");placement_hooks::service_probe();check(lua("return fdk.invoke('engine.placement_hook_probe_result')").starts_with("failed\t"),"changed tail probe accepted");memory[profile.handler+100]^=1;
        lua("return fdk.invoke('engine.placement_hook_probe')");
        const auto saved=direct;
        std::array<unsigned char,6> replacement{0xc3,0x90,0x90,0x90,0x90,0x90};
        if(third_party)std::memcpy(memory+profile.handler,replacement.data(),replacement.size());
        check(placement_hooks::stop() && !placement_hooks::active(),"stop");
        check(lua("return fdk.invoke('engine.placement_hook_probe_result')")=="cancelled\tplacement hooks stopped","probe cancellation");
        if(third_party)check(!std::memcmp(memory+profile.handler,replacement.data(),replacement.size()),"third-party patch overwritten");
        else {std::array<unsigned char,241> expected{};bytes(reinterpret_cast<std::uintptr_t>(expected.data()),profile.code);check(!std::memcmp(memory+profile.handler,expected.data(),expected.size()),"body not restored");}
        saved(actor,&pose);check(fdkss_destroy(runtime)==FDKSS_OK,"runtime destroy");runtime=nullptr;saved(actor,&pose);
        VirtualFree(memory,0,MEM_RELEASE);std::filesystem::remove_all(root);std::cout<<"Placement hook tests passed\n";
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';placement_hooks::stop();if(runtime)fdkss_destroy(runtime);VirtualFree(memory,0,MEM_RELEASE);std::filesystem::remove_all(root);return 1;}
}
