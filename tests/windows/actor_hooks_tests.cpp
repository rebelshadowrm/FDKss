#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "actor_hooks.h"
#include "actor_methods.h"
#include "../../src/actor_types.h"
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <thread>
namespace {
void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void *object;
using Numeric = std::uint32_t (__fastcall *)(void *);
using Name = const char *(__fastcall *)(void *);
void **slot;
std::uint32_t expected;
int FDKSS_CALL recursive(void *, const char *, char *out, std::size_t size) {
    check(reinterpret_cast<Numeric>(*slot)(object) == expected, "busy/nested hook bypass failed");
    if (size) *out = 0; return 0;
}
int capture(void *sink, const fdkss_value *value) {
    *static_cast<std::uint32_t *>(sink) = static_cast<std::uint32_t>(value->integer); return 0;
}
std::uint32_t __fastcall third_party(void *) { return 123; }
}
int main(int argc, char **argv) {
    const bool sky2 = argc > 1 && std::string_view(argv[1]) == "sky2";
    const auto root = std::filesystem::temp_directory_path() / ("fdkss-actor-hooks-" + std::to_string(GetCurrentProcessId()));
    fdkss_runtime *runtime = nullptr;
    const auto release = [](void *p) { if (p) VirtualFree(p, 0, MEM_RELEASE); };
    std::unique_ptr<void, decltype(release)> image(VirtualAlloc(nullptr, 0xc61000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE), release);
    try {
        check(image != nullptr, "fixture allocation failed");
        const auto base = reinterpret_cast<std::uintptr_t>(image.get());
        const auto &profiles = sky2 ? actor_types::second : actor_types::first;
        const auto transform = base+(sky2 ? 0x5a9c40 : 0x47f1c0);
        const unsigned char transform_code[]{0x48,0x8d,0x41,0x58,0xc3};
        std::memcpy(reinterpret_cast<void *>(transform),transform_code,sizeof(transform_code));
        std::array<void *, 66> originals{};
        std::size_t index = 0;
        for (const auto &p : profiles) {
            const std::uintptr_t targets[]{base+p.name, base+p.id, base+p.flags};
            std::memcpy(reinterpret_cast<void *>(base+p.table+8), targets, sizeof(targets));
            for (const auto target : targets) originals[index++] = reinterpret_cast<void *>(target);
            unsigned char name[]{0x48,0x8d,5,0,0,0,0,0xc3};
            const auto displacement = static_cast<std::int32_t>(p.text-p.name-7);
            std::memcpy(name+3, &displacement, 4); std::memcpy(reinterpret_cast<void *>(targets[0]), name, sizeof(name));
            unsigned char numeric[]{0xb8,0,0,0,0,0xc3};
            std::memcpy(numeric+1, &p.expected_id, 4); std::memcpy(reinterpret_cast<void *>(targets[1]), numeric, sizeof(numeric));
            if (p.expected_flags) {
                std::memcpy(numeric+1, &p.expected_flags, 4); std::memcpy(reinterpret_cast<void *>(targets[2]), numeric, sizeof(numeric));
            } else {
                const unsigned char zero[]{0x33,0xc0,0xc3}; std::memcpy(reinterpret_cast<void *>(targets[2]), zero, sizeof(zero));
            }
            std::strcpy(reinterpret_cast<char *>(base+p.text), "T");
            std::memcpy(reinterpret_cast<void *>(base+p.table+48),&transform,8);
        }
        FlushInstructionCache(GetCurrentProcess(), image.get(), 0xc61000);
        object = reinterpret_cast<void *>(base+0x1000);
        auto matrix = reinterpret_cast<float *>(base+0x1000+0x58);
        for (unsigned i=0;i<16;++i) matrix[i]=static_cast<float>(i)+0.5f;
        slot = reinterpret_cast<void **>(base+profiles[0].table+16);
        expected = profiles[0].expected_id;
        std::filesystem::create_directories(root/"Test"/"Scripts");
        std::ofstream(root/"Test"/"enabled.txt") << '1';
        std::ofstream(root/"Test"/"Scripts"/"main.lua") << R"(
            pres, posts, names, flags = 0, 0, 0, 0
            bad_names = 0
            mode = 0
            transforms, bad_transforms = 0, 0
            fdk.hook('actor.get_transform','post',function(address,class,...)
                local values=table.pack(...)
                if values[1]==nil then
                    assert(values.n==2 and values[2]=='transform could not be copied'); bad_transforms=bad_transforms+1
                else
                    assert(values.n==16)
                    for i=1,16 do assert(values[i]==i-0.5) end
                    transforms=transforms+1
                end
                return 123 -- observer cannot replace the native pointer
            end)
            fdk.hook('actor.type_id', 'pre', function(address, class)
                assert(type(address)=='string' and address:match('^0x%x+$') and type(class)=='string')
                pres = pres + 1
                if probing then
                    assert(not pcall(fdk.invoke,'engine.actor_hook_probe'))
                    probe_rejections=(probe_rejections or 0)+1
                end
                fdk.call('fixture.recursive')
                return 0 -- pre is observer only
            end)
            fdk.hook('actor.type_id', 'post', function(address, class, value)
                posts = posts + 1
                if mode == 1 then return value+1 end
            end)
            fdk.hook('actor.type_name', 'post', function(address, class, value, error)
                if value==nil then assert(error=='type name could not be copied'); bad_names=bad_names+1
                else assert(value=='T'); names=names+1 end; return 'ignored'
            end)
            fdk.hook('actor.type_flags', 'post', function(address, class, value)
                flags=flags+1
            end)
            invalidmode, bad = 0, 0
            local invalid = {-1, 4294967296, 1.25, 'wrong', 0/0}
            for i=1,5 do
                fdk.hook('actor.type_flags','post',function()
                    if invalidmode==i then bad=bad+1; return invalid[i] end
                end)
            end
            fdk.hook('actor.type_flags','post',function()
                if invalidmode==6 then return 4294967295 end
            end)
        )";
        const auto path = root.u8string();
        check(fdkss_create(reinterpret_cast<const char *>(path.c_str()), nullptr, nullptr, &runtime)==FDKSS_OK, "runtime create failed");
        check(fdkss_register_native(runtime,"fixture.recursive",recursive,nullptr)==FDKSS_OK, "recursive native failed");
        check(fdkss_register_typed_native(runtime,"engine.actor_hook_probe",actor_hooks::request_probe,nullptr)==FDKSS_OK &&
            fdkss_register_typed_native(runtime,"engine.actor_hook_probe_result",actor_hooks::probe_result,nullptr)==FDKSS_OK,"probe registration");
        check(fdkss_register_typed_native(runtime,"fixture.probe_drain",+[](void *,const fdkss_value *,std::size_t,fdkss_result_fn emit,void *sink,char *,std::size_t)->int {
            actor_hooks::service_probe(reinterpret_cast<std::uintptr_t>(object));
            fdkss_value value{};value.type=FDKSS_BOOLEAN;value.boolean=actor_hooks::probe_pending();return emit(sink,&value);
        },nullptr)==FDKSS_OK,"probe busy fixture registration");
        check(fdkss_register_typed_native(runtime,"engine.hook_supported",
            [](void *, const fdkss_value *, std::size_t, fdkss_result_fn emit, void *sink, char *, std::size_t)->int {
                fdkss_value value{};value.type=FDKSS_BOOLEAN;value.boolean=1;return emit(sink,&value);
            },nullptr)==FDKSS_OK, "hook support registration failed");
        char result[256]{}, error[256]{};
        auto lua = [&](const char *source) {
            const auto status = fdkss_execute(runtime,"Test",source,result,sizeof(result));
            if (status != FDKSS_OK) std::cerr << source << ": " << fdkss_last_error(runtime) << "\n";
            check(status==FDKSS_OK, "fixture Lua execution failed");
        };
        // A single changed getter blocks all patches; failed preflight remains retryable.
        auto bad = reinterpret_cast<void **>(base+profiles[21].table+24);
        auto saved_bad = *bad; *bad = reinterpret_cast<void *>(third_party);
        check(!actor_hooks::start(base,sky2,runtime,GetCurrentThreadId()), "changed slot accepted");
        check(*slot==originals[1] && !actor_hooks::active(), "preflight partially patched tables");
        *bad = saved_bad;
        *reinterpret_cast<unsigned char *>(transform)=0x90;
        check(!actor_hooks::start(base,sky2,runtime,GetCurrentThreadId()) && *slot==originals[1], "changed transform code accepted or partially patched");
        *reinterpret_cast<unsigned char *>(transform)=transform_code[0];
        FlushInstructionCache(GetCurrentProcess(),reinterpret_cast<void *>(transform),sizeof(transform_code));
        check(actor_hooks::start(base,sky2,runtime,GetCurrentThreadId()), "actor hooks did not start");
        check(fdkss_start(runtime)==FDKSS_OK, "runtime start failed");
        const auto first_thunk = reinterpret_cast<Numeric>(*slot);
        check(first_thunk(object)==expected, "observer changed ID");
        lua("mode=1"); check(first_thunk(object)==expected+1, "numeric replacement failed");
        std::thread foreign([&] { check(first_thunk(object)==expected, "foreign hook dispatched Lua"); }); foreign.join();
        lua("fdk.call('fixture.recursive'); return pres, posts");
        check(std::string(result)=="2\t2", "nested, busy, or foreign calls dispatched events");
        lua("mode=0");
        for (const auto &p : profiles) {
            const auto table = base+p.table; std::memcpy(object,&table,8);
            slot = reinterpret_cast<void **>(table+16); expected=p.expected_id;
            check(std::strcmp(reinterpret_cast<Name>(*reinterpret_cast<void **>(table+8))(object),"T")==0, "name changed");
            check(reinterpret_cast<Numeric>(*slot)(object)==expected, "ID mismatch");
            check(reinterpret_cast<Numeric>(*reinterpret_cast<void **>(table+24))(object)==p.expected_flags, "flags mismatch");
            using Transform = const float *(__fastcall *)(void *);
            const auto pointer = reinterpret_cast<Transform>(*reinterpret_cast<void **>(table+48))(object);
            check(pointer==matrix && pointer[15]==15.5f, "transform hook changed original pointer/data");
            unsigned copied_values=0;
            check(!actor_methods::transform(object,transform,
                [](void *sink,const fdkss_value *value)->int {
                    auto &count=*static_cast<unsigned *>(sink);
                    check(value->type==FDKSS_NUMBER && value->number==count+0.5,"explicit transform data changed"); ++count; return 0;
                },&copied_values,error,sizeof(error)) && copied_values==16,"explicit transform unavailable while hooked");
            std::uint32_t copied=0;
            check(actor_hooks::owns(slot,reinterpret_cast<void *>(base+p.id)), "owned slot rejected");
            check(!actor_methods::type(object,base,sky2,"type_id",capture,&copied,error,sizeof(error)) && copied==expected, "explicit getter unavailable while hooked");
        }
        lua("return pres, posts, names, flags");
        check(std::string(result)=="24\t24\t22\t22", "all class callbacks not observed");
        lua("return transforms, bad_transforms"); check(std::string(result)=="22\t0", "transform callbacks incomplete");
        using Transform = const float *(__fastcall *)(void *);
        auto transform_thunk = reinterpret_cast<Transform>(*reinterpret_cast<void **>(base+profiles[21].table+48));
        matrix[0]=std::numeric_limits<float>::quiet_NaN();
        check(transform_thunk(object)==matrix, "nonfinite matrix changed original pointer");
        matrix[0]=0.5f;
        lua("return transforms, bad_transforms"); check(std::string(result)=="22\t1", "invalid copied buffer was exposed");
        std::thread foreign_transform([&] { check(transform_thunk(object)==matrix,"foreign transform pointer changed"); }); foreign_transform.join();
        lua("return transforms"); check(std::string(result)=="22", "foreign transform dispatched Lua");
        check(transform_thunk(nullptr)==reinterpret_cast<float *>(0x58),"unreadable transform changed original pointer");
        lua("return bad_transforms"); check(std::string(result)=="2","unreadable transform did not post error");
        std::memset(reinterpret_cast<void *>(base+profiles[21].text),'x',128);
        auto name_thunk=reinterpret_cast<Name>(*reinterpret_cast<void **>(base+profiles[21].table+8));
        check(name_thunk(object)==reinterpret_cast<char *>(base+profiles[21].text),"unterminated name changed original pointer");
        lua("return bad_names"); check(std::string(result)=="1","unterminated name did not post error");
        std::strcpy(reinterpret_cast<char *>(base+profiles[21].text),"T");
        // Invalid numeric callbacks are isolated and disabled; subsequent observers continue.
        for (unsigned mode=1;mode<=5;++mode) {
            lua((std::string("bad=0; invalidmode=")+std::to_string(mode)).c_str());
            auto flags = reinterpret_cast<Numeric>(*reinterpret_cast<void **>(base+profiles[21].table+24));
            check(flags(object)==profiles[21].expected_flags && flags(object)==profiles[21].expected_flags, "invalid replacement accepted");
            lua("return bad"); check(std::string(result)=="1", "invalid callback was not disabled");
        }
        lua("invalidmode=6");
        check(reinterpret_cast<Numeric>(*bad)(object)==0xffffffffu, "uint32 maximum rejected");
        lua("return fdk.invoke('engine.actor_hook_probe')");check(std::string(result)=="true" && actor_hooks::probe_pending(),"probe not queued");
        lua("return pcall(fdk.invoke,'engine.actor_hook_probe')");check(std::string(result).starts_with("false\t"),"second pending probe accepted");
        lua("return fdk.invoke('fixture.probe_drain')");check(std::string(result)=="true","probe executed while native caller busy");
        std::thread foreign_probe([&]{actor_hooks::service_probe(reinterpret_cast<std::uintptr_t>(object));});foreign_probe.join();check(actor_hooks::probe_pending(),"foreign probe consumed queue");
        lua("probing=true");
        actor_hooks::service_probe(reinterpret_cast<std::uintptr_t>(object));
        lua("probing=false;return probe_rejections");check(std::string(result)=="1" && !actor_hooks::probe_pending(),"reentrant probe accepted");
        lua("local r=table.pack(fdk.invoke('engine.actor_hook_probe_result'));return r.n,r[1],r[3],r[4],r[5],r[6],r[21]");
        check(std::string(result)=="21\tsucceeded\tT\t"+std::to_string(profiles[21].expected_id)+"\t4294967295\t0.5\t15.5","probe copied results/filter behavior");
        lua("return fdk.invoke('engine.actor_hook_probe')");
        auto *name_body=reinterpret_cast<unsigned char *>(base+profiles[21].name);const auto first_byte=*name_body;*name_body=0x90;
        actor_hooks::service_probe(reinterpret_cast<std::uintptr_t>(object));*name_body=first_byte;
        FlushInstructionCache(GetCurrentProcess(),name_body,8);
        lua("return fdk.invoke('engine.actor_hook_probe_result')");check(std::string(result).starts_with("failed\tgetter slot/code changed"),"probe changed-body preflight");
        lua("return fdk.invoke('engine.actor_hook_probe')");actor_hooks::service_probe(0);
        lua("return fdk.invoke('engine.actor_hook_probe_result')");check(std::string(result)=="failed\tcurrent actor unavailable","probe absent actor");
        // Third-party replacements survive stop, and saved chained thunks safely call through.
        *bad = reinterpret_cast<void *>(third_party);
        lua("return fdk.invoke('engine.actor_hook_probe')");actor_hooks::service_probe(reinterpret_cast<std::uintptr_t>(object));
        lua("return fdk.invoke('engine.actor_hook_probe_result')");check(std::string(result).starts_with("failed\tgetter slot/code changed"),"probe third-party slot guard");
        lua("return fdk.invoke('engine.actor_hook_probe')");
        check(actor_hooks::stop() && !actor_hooks::active() && !actor_hooks::owns(slot,reinterpret_cast<void *>(base+profiles[21].id)), "stop failed");
        lua("return fdk.invoke('engine.actor_hook_probe_result')");check(std::string(result)=="cancelled\tactor hooks stopped" && !actor_hooks::probe_pending(),"probe stop cancellation");
        index=0;
        for (const auto &p : profiles) for (unsigned kind=0;kind<3;++kind) {
            auto current = *reinterpret_cast<void **>(base+p.table+(kind+1)*8);
            check(current==(index==65 ? reinterpret_cast<void *>(third_party) : originals[index]), "slot restoration failed"); ++index;
        }
        for (const auto &p : profiles) check(*reinterpret_cast<void **>(base+p.table+48)==reinterpret_cast<void *>(transform), "transform slot restoration failed");
        check(transform_thunk(object)==matrix, "saved transform thunk failed after stop");
        check(first_thunk(object)==profiles[0].expected_id, "saved thunk failed after stop");
        check(fdkss_destroy(runtime)==FDKSS_OK, "runtime destroy failed"); runtime=nullptr;
        check(first_thunk(object)==profiles[0].expected_id, "saved thunk touched destroyed runtime");
        std::filesystem::remove_all(root); return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n'; actor_hooks::stop(); if (runtime) fdkss_destroy(runtime);
        std::filesystem::remove_all(root); return 1;
    }
}
