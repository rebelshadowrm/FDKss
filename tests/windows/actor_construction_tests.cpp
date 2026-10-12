#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "actor_construction.h"
#include "actor_construction_profiles.h"
#include "lifetimes.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
namespace {
void check(bool condition,const char *message) {if(!condition)throw std::runtime_error(message);}
void write_hex(std::uintptr_t address,const char *hex) {
    for(std::size_t i=0;i<std::strlen(hex)/2;++i) {
        unsigned value=0;std::sscanf(hex+2*i,"%2x",&value);reinterpret_cast<unsigned char *>(address)[i]=static_cast<unsigned char>(value);
    }
}
std::uintptr_t call_target(std::uintptr_t instruction) {
    std::int32_t relative=0;std::memcpy(&relative,reinterpret_cast<void *>(instruction+1),4);return instruction+5+relative;
}
int result(void *sink,const fdkss_value *value) {*static_cast<std::int64_t *>(sink)=value->integer;return 0;}
}
int main(int argc,char **argv) {
    const bool sky2=argc>1 && std::string_view(argv[1])=="sky2";
    const bool external=argc>2;
    const auto &profile=sky2 ? actor_construction::sky2 : actor_construction::sky1;
    const auto release=[](void *p){if(p)VirtualFree(p,0,MEM_RELEASE);};
    std::unique_ptr<void,decltype(release)> image(VirtualAlloc(nullptr,0xc61000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE),release);
    const auto root=std::filesystem::temp_directory_path()/("fdkss-construction-"+std::to_string(GetCurrentProcessId()));
    fdkss_runtime *runtime=nullptr;
    try {
        check(image!=nullptr,"fixture allocation failed");const auto base=reinterpret_cast<std::uintptr_t>(image.get());
        auto target=base+profile.constructor;
        write_hex(target,profile.constructor_hex);write_hex(base+profile.destructor,profile.destructor_hex);
        // Preserve the exact independently pinned constructor/destructor bodies while replacing
        // their leaf helpers in this private fixture image. No game or engine global is used.
        *reinterpret_cast<unsigned char *>(call_target(target+0x21))=0xc3;
        const auto matrix=call_target(target+0x3a);
        for(unsigned i=0;i<16;++i) {
            auto code=reinterpret_cast<unsigned char *>(matrix+i*7);
            code[0]=0xc7;code[1]=0x41;code[2]=static_cast<unsigned char>(i*4);
            const std::uint32_t value=i%5==0 ? 0x3f800000u : 0;std::memcpy(code+3,&value,4);
        }
        *reinterpret_cast<unsigned char *>(matrix+112)=0xc3;
        *reinterpret_cast<unsigned char *>(call_target(base+profile.destructor+0x19))=0xc3;
        *reinterpret_cast<unsigned char *>(call_target(base+profile.destructor+0x2d))=0xc3;
        FlushInstructionCache(GetCurrentProcess(),image.get(),0xc61000);
        std::array<unsigned char,139> original{};std::memcpy(original.data(),reinterpret_cast<void *>(target),original.size());
        std::array<unsigned char,256> original_destructor{};
        const auto destructor_size=std::strlen(profile.destructor_hex)/2;
        check(destructor_size<=original_destructor.size(),"destructor fixture body exceeds storage");
        std::memcpy(original_destructor.data(),reinterpret_cast<void *>(base+profile.destructor),destructor_size);
        *reinterpret_cast<unsigned char *>(base+profile.destructor+50)^=1;
        check(!actor_construction::start(base,sky2,GetCurrentThreadId()),"changed destructor body accepted");
        *reinterpret_cast<unsigned char *>(base+profile.destructor+50)^=1;
        *reinterpret_cast<unsigned char *>(target+80)^=1;
        check(!actor_construction::start(base,sky2,GetCurrentThreadId()),"changed constructor body accepted");
        *reinterpret_cast<unsigned char *>(target+80)^=1;
        check(actor_construction::start(base,sky2,GetCurrentThreadId()),"constructor hook installation failed");
        std::filesystem::create_directories(root/"Events"/"Scripts");std::ofstream(root/"Events"/"enabled.txt")<<'1';
        std::ofstream(root/"Events"/"Scripts"/"main.lua")<<R"(
            constructed,diagnostics,overflows=0,0,0
            fdk.on('engine.actor_base_constructed',function(address,class,diagnostic)
                assert(type(address)=='string' and address:match('^0x%x+$') and class=='.?AVActor@fdk@@')
                assert(type(diagnostic)=='boolean');constructed=constructed+1
                if diagnostic then diagnostics=diagnostics+1 end
            end)
            fdk.on('engine.lifecycle_overflow',function(count) overflows=overflows+count end)
        )";
        const auto path=root.u8string();check(fdkss_create(reinterpret_cast<const char *>(path.c_str()),nullptr,nullptr,&runtime)==FDKSS_OK && fdkss_start(runtime)==FDKSS_OK,"event runtime failed");
        std::int64_t passed=0;char error[256]{};
        check(!actor_construction::self_test(nullptr,nullptr,0,result,&passed,error,sizeof(error)) && passed==2,"private constructor self-test failed");
        *reinterpret_cast<unsigned char *>(base+profile.destructor+50)^=1;
        check(actor_construction::self_test(nullptr,nullptr,0,result,&passed,error,sizeof(error))!=0,"changed hooked destructor body accepted");
        *reinterpret_cast<unsigned char *>(base+profile.destructor+50)^=1;
        *reinterpret_cast<unsigned char *>(target+80)^=1;
        check(actor_construction::self_test(nullptr,nullptr,0,result,&passed,error,sizeof(error))!=0,"changed hooked constructor body accepted");
        *reinterpret_cast<unsigned char *>(target+80)^=1;
        FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void *>(target),139);
        actor_construction::emit_events(runtime);
        char output[128]{};
        auto counts=[&](const char *expected) {
            check(fdkss_execute(runtime,"Events","return constructed,diagnostics,overflows",output,sizeof(output))==FDKSS_OK && std::string(output)==expected,"constructor event counts incorrect");
        };
        counts("2\t2\t0");
        alignas(16) std::array<unsigned char,0x190> actor{};
        auto stamp=actor_lifetimes::observe(reinterpret_cast<std::uintptr_t>(actor.data()));
        using Constructor=void *(__fastcall *)(void *);
        const auto construct=reinterpret_cast<Constructor>(target);
        bool foreign_failed=false;
        std::thread foreign([&] {
            foreign_failed=actor_construction::self_test(nullptr,nullptr,0,result,&passed,error,sizeof(error))!=0;
            check(construct(actor.data())==actor.data(),"foreign constructor return changed");
            actor_construction::emit_events(runtime); // Must not dispatch on a foreign thread.
        });foreign.join();
        check(foreign_failed && !actor_lifetimes::valid(reinterpret_cast<std::uintptr_t>(actor.data()),stamp),"foreign construction did not invalidate reused address");counts("2\t2\t0");
        actor_construction::emit_events(runtime);counts("3\t2\t0");
        const auto actor_address=reinterpret_cast<std::uintptr_t>(actor.data());
        stamp=actor_lifetimes::observe(actor_address);
        auto destruct=reinterpret_cast<actor_lifetimes::Destructor>(base+profile.destructor);
        check(destruct(actor.data(),0)==actor.data(),"direct base destructor return changed");
        check(!actor_lifetimes::valid(actor_address,stamp),"direct base destructor left an observed reference alive");
        auto destroyed=actor_lifetimes::drain();
        check(destroyed.destroyed.size()==3 && !destroyed.dropped,"private/direct destructor diagnostics missing");
        for(const auto &event:destroyed.destroyed)
            check(std::string_view(event.class_name)==".?AVActor@fdk@@","direct destructor class diagnostic changed");
        stamp=actor_lifetimes::observe(actor_address);
        std::thread foreign_destructor([&] {check(destruct(actor.data(),2)==actor.data(),"foreign direct destructor return changed");});
        foreign_destructor.join();
        check(!actor_lifetimes::valid(actor_address,stamp),"foreign direct destructor left an observed reference alive");
        check(actor_lifetimes::drain().destroyed.size()==1,"foreign direct destructor diagnostic missing");
        void *destructor_slot=reinterpret_cast<void *>(destruct);
        actor_lifetimes::Hook lifetime_hook{&destructor_slot,destruct,"DerivedFixture"};
        check(actor_lifetimes::install(std::span(&lifetime_hook,1)),"vtable/direct interception fixture installation failed");
        auto virtual_destruct=reinterpret_cast<actor_lifetimes::Destructor>(destructor_slot);
        stamp=actor_lifetimes::observe(actor_address);
        check(virtual_destruct(actor.data(),0)==actor.data() && !actor_lifetimes::valid(actor_address,stamp),"vtable/direct interception call-through changed");
        destroyed=actor_lifetimes::drain();
        check(destroyed.destroyed.size()==1 && std::string_view(destroyed.destroyed[0].class_name)=="DerivedFixture","vtable/direct interception duplicated or relabeled destruction");
        for(unsigned i=0;i<2050;++i)check(construct(actor.data())==actor.data(),"constructor call-through failed");
        actor_construction::emit_events(runtime);counts("2051\t2\t2");
        if(external) {
            const unsigned char replacement[]{0x48,0x89,0xc8,0xc3,0x90}; // Third-party pointer-preserving leaf replacement.
            std::memcpy(reinterpret_cast<void *>(target),replacement,sizeof(replacement));
            std::memcpy(original.data(),replacement,sizeof(replacement));
            FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void *>(target),sizeof(replacement));
            std::memcpy(reinterpret_cast<void *>(base+profile.destructor),replacement,sizeof(replacement));
            std::memcpy(original_destructor.data(),replacement,sizeof(replacement));
            FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void *>(base+profile.destructor),sizeof(replacement));
        }
        check(actor_construction::stop() && !std::memcmp(reinterpret_cast<void *>(target),original.data(),original.size()),"constructor code did not restore");
        check(!std::memcmp(reinterpret_cast<void *>(base+profile.destructor),original_destructor.data(),destructor_size),"direct destructor code did not restore or preserve external patch");
        check(actor_lifetimes::stop() && destructor_slot==reinterpret_cast<void *>(destruct),"vtable fixture did not restore");
        stamp=actor_lifetimes::observe(reinterpret_cast<std::uintptr_t>(actor.data()));
        check(construct(actor.data())==actor.data() && actor_lifetimes::valid(reinterpret_cast<std::uintptr_t>(actor.data()),stamp),"stopped constructor still observed");
        check(destruct(actor.data(),0)==actor.data() && actor_lifetimes::valid(actor_address,stamp),"stopped direct destructor still observed");
        check(virtual_destruct(actor.data(),0)==actor.data() && actor_lifetimes::valid(actor_address,stamp),"cached restored vtable thunk still observed");
        actor_construction::emit_events(runtime);counts("2051\t2\t2");
        check(fdkss_destroy(runtime)==FDKSS_OK,"runtime destroy failed");runtime=nullptr;
        std::filesystem::remove_all(root);return 0;
    } catch(const std::exception &e) {
        std::cerr<<e.what()<<'\n';actor_construction::stop();if(runtime)fdkss_destroy(runtime);
        std::filesystem::remove_all(root);return 1;
    }
}
