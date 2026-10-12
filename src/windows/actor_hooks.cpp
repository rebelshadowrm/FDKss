#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "actor_hooks.h"
#include "../actor_types.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <type_traits>
#include <utility>
namespace {
constexpr std::size_t type_slots=66, maximum=88;
#define FDKSS_ACTOR_METHOD(name, slot, rva) constexpr std::uintptr_t first_transform = rva;
#include "fdkss/sky1_actor_methods.inc"
#undef FDKSS_ACTOR_METHOD
#define FDKSS_ACTOR_METHOD(name, slot, rva) constexpr std::uintptr_t second_transform = rva;
#include "fdkss/sky2_actor_methods.inc"
#undef FDKSS_ACTOR_METHOD
struct Binding { void **slot=nullptr; void *original=nullptr; const char *type=nullptr; };
std::array<Binding,maximum> bindings{};
std::size_t count=0;
std::atomic<bool> enabled{false};
bool attempted=false;
fdkss_runtime *runtime=nullptr;
DWORD owner=0;
thread_local bool nested=false;
std::atomic<std::uint64_t> calls{0}, observed{0};
std::array<std::atomic<std::uint64_t>,4> method_calls{}, method_observed{};
std::uintptr_t probe_base=0;
bool probe_sky2=false;
std::atomic<bool> pending_probe{false},running_probe{false};
const char *probe_status="idle",*probe_error="";
std::array<char,128> probe_name{};
std::array<float,16> probe_matrix{};
std::uint32_t probe_id=0,probe_flags=0;
bool read(std::uintptr_t address,void *out,std::size_t size) {
    SIZE_T copied=0;return address && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void *>(address),out,size,&copied) && copied==size;
}
bool swap(void **slot,void *from,void *to) {
    DWORD protection=0,ignored=0;
    if (!VirtualProtect(slot,sizeof(void *),PAGE_READWRITE,&protection)) return false;
    const auto prior=InterlockedCompareExchangePointer(reinterpret_cast<void *volatile *>(slot),to,from);
    const bool restored=VirtualProtect(slot,sizeof(void *),protection,&ignored)!=0;
    return prior==from && restored;
}
template<std::size_t I> using Return=std::conditional_t<(I>=type_slots),const float *,std::conditional_t<I%3==0,const char *,std::uint32_t>>;
template<std::size_t I> Return<I> __fastcall dispatch(void *object) {
    using Getter=Return<I> (__fastcall *)(void *);
    const auto original=reinterpret_cast<Getter>(bindings[I].original);
    constexpr std::size_t kind=I>=type_slots ? 3 : I%3;
    calls.fetch_add(1,std::memory_order_relaxed);
    method_calls[kind].fetch_add(1,std::memory_order_relaxed);
    if (!enabled.load(std::memory_order_acquire) || GetCurrentThreadId()!=owner || nested || fdkss_native_caller(runtime)) return original(object);
    nested=true;
    struct Reset { ~Reset(){nested=false;} } reset;
    char address[32]{};std::snprintf(address,sizeof(address),"0x%llx",static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(object)));
    fdkss_value values[I>=type_slots ? 18 : 4]{};
    values[0].type=values[1].type=FDKSS_STRING;
    values[0].string=address;values[0].string_size=std::strlen(address);
    values[1].string=bindings[I].type;values[1].string_size=std::strlen(bindings[I].type);
    constexpr const char *pre=I>=type_slots ? "hook.actor.get_transform.pre" : I%3==0 ? "hook.actor.type_name.pre" : I%3==1 ? "hook.actor.type_id.pre" : "hook.actor.type_flags.pre";
    constexpr const char *post=I>=type_slots ? "hook.actor.get_transform.post" : I%3==0 ? "hook.actor.type_name.post" : I%3==1 ? "hook.actor.type_id.post" : "hook.actor.type_flags.post";
    try { fdkss_emit_values(runtime,pre,values,2); } catch (...) {}
    auto result=original(object);
    try {
        if constexpr (I>=type_slots) {
            std::array<float,16> matrix{};
            bool valid = reinterpret_cast<std::uintptr_t>(result)==reinterpret_cast<std::uintptr_t>(object)+0x58 &&
                read(reinterpret_cast<std::uintptr_t>(result),matrix.data(),sizeof(matrix));
            for (const auto value : matrix) if (!std::isfinite(value)) valid=false;
            if (valid) {
                for (std::size_t n=0;n<matrix.size();++n) {values[n+2].type=FDKSS_NUMBER;values[n+2].number=matrix[n];}
                fdkss_emit_values(runtime,post,values,18);
            } else {
                // Paired post still occurs, without exposing a borrowed or invalid buffer.
                values[2].type=FDKSS_NIL;
                values[3].type=FDKSS_STRING;values[3].string="transform could not be copied";
                values[3].string_size=std::strlen(values[3].string);
                fdkss_emit_values(runtime,post,values,4);
            }
        } else if constexpr (I%3==0) {
            std::array<char,128> text{};
            if (read(reinterpret_cast<std::uintptr_t>(result),text.data(),text.size()) && std::memchr(text.data(),0,text.size())) {
                values[2].type=FDKSS_STRING;values[2].string=text.data();values[2].string_size=std::strlen(text.data());
                fdkss_emit_values(runtime,post,values,3);
            } else {
                values[2].type=FDKSS_NIL;
                values[3].type=FDKSS_STRING;values[3].string="type name could not be copied";
                values[3].string_size=std::strlen(values[3].string);
                fdkss_emit_values(runtime,post,values,4);
            }
        } else {
            values[2].type=FDKSS_INTEGER;values[2].integer=result;
            if (fdkss_filter_values(runtime,post,values,3,2,0,4294967295.0)==FDKSS_OK)
                result=static_cast<std::uint32_t>(values[2].integer);
        }
        observed.fetch_add(1,std::memory_order_relaxed);
        method_observed[kind].fetch_add(1,std::memory_order_relaxed);
    } catch (...) {}
    return result;
}
template<std::size_t... I> auto thunks(std::index_sequence<I...>) {
    return std::array<void *,sizeof...(I)>{reinterpret_cast<void *>(&dispatch<I>)...};
}
const auto replacements=thunks(std::make_index_sequence<maximum>{});
bool checked(std::uintptr_t base,const actor_types::Profile &profile,unsigned kind,std::uintptr_t transform=0,void *replacement=nullptr) {
    const auto target=kind==3 ? base+transform : base+(kind==0 ? profile.name : kind==1 ? profile.id : profile.flags);
    void *actual_target=nullptr;
    if (!read(base+profile.table+(kind==3 ? 6 : kind+1)*8,&actual_target,8) || actual_target!=(replacement ? replacement : reinterpret_cast<void *>(target))) return false;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<void *>(target),&region,sizeof(region)) || region.State!=MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    if (kind==3) {
        std::array<unsigned char,5> actual{};
        return read(target,actual.data(),actual.size()) && actual==std::array<unsigned char,5>{0x48,0x8d,0x41,0x58,0xc3};
    }
    if (!kind) {
        std::array<unsigned char,8> expected{0x48,0x8d,5,0,0,0,0,0xc3},actual{};
        const auto offset=static_cast<std::int32_t>(profile.text-profile.name-7);std::memcpy(expected.data()+3,&offset,4);
        return read(target,actual.data(),actual.size()) && actual==expected;
    }
    const auto value=kind==1 ? profile.expected_id : profile.expected_flags;
    if (kind==2 && !value) {
        std::array<unsigned char,3> actual{};
        return read(target,actual.data(),actual.size()) && actual==std::array<unsigned char,3>{0x33,0xc0,0xc3};
    }
    std::array<unsigned char,6> expected{0xb8,0,0,0,0,0xc3},actual{};std::memcpy(expected.data()+1,&value,4);
    return read(target,actual.data(),actual.size()) && actual==expected;
}
}
namespace actor_hooks {
bool start(std::uintptr_t base,bool sky2,fdkss_runtime *target_runtime,unsigned long thread) {
    if (attempted || !base || !target_runtime || !thread) return false;
    const auto &profiles=sky2 ? actor_types::second : actor_types::first;
    static_assert(std::size(actor_types::first)*4==maximum && std::size(actor_types::second)*4==maximum);
    for (const auto &profile : profiles) for (unsigned kind=0;kind<3;++kind) if (!checked(base,profile,kind)) return false;
    const auto transform=sky2 ? second_transform : first_transform;
    for (const auto &profile : profiles) if (!checked(base,profile,3,transform)) return false;
    attempted=true;runtime=target_runtime;owner=thread;probe_base=base;probe_sky2=sky2;
    for (const auto &profile : profiles) for (unsigned kind=0;kind<3;++kind) {
        bindings[count++]={reinterpret_cast<void **>(base+profile.table+(kind+1)*8),reinterpret_cast<void *>(base+(kind==0 ? profile.name : kind==1 ? profile.id : profile.flags)),profile.class_name};
    }
    for (const auto &profile : profiles)
        bindings[count++]={reinterpret_cast<void **>(base+profile.table+6*8),reinterpret_cast<void *>(base+transform),profile.class_name};
    enabled.store(true,std::memory_order_release);
    for (std::size_t i=0;i<count;++i) if (!swap(bindings[i].slot,bindings[i].original,replacements[i])) {stop();return false;}
    return true;
}
bool stop() {
    if(pending_probe.exchange(false)) {probe_status="cancelled";probe_error="actor hooks stopped";}
    enabled.store(false,std::memory_order_release);
    bool restored=true;
    for (std::size_t i=0;i<count;++i) if (*bindings[i].slot==replacements[i] && !swap(bindings[i].slot,replacements[i],bindings[i].original)) restored=false;
    return restored;
}
bool owns(void **slot,void *original) {
    if (!enabled.load(std::memory_order_acquire)) return false;
    for (std::size_t i=0;i<count;++i) if (bindings[i].slot==slot && bindings[i].original==original && *slot==replacements[i]) return true;
    return false;
}
bool active(){return enabled.load(std::memory_order_acquire);}
bool probe_pending(){return pending_probe.load();}
int FDKSS_CALL request_probe(void *,const fdkss_value *,std::size_t argc,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    if(argc || !active() || GetCurrentThreadId()!=owner || pending_probe.load() || running_probe.load()) {
        std::snprintf(error,size,"actor getter probe requires the owner thread, active hooks and an empty diagnostic queue");return 1;
    }
    probe_status="pending";probe_error="";pending_probe.store(true);
    fdkss_value value{};value.type=FDKSS_BOOLEAN;value.boolean=1;return emit(sink,&value);
}
void service_probe(std::uintptr_t object) {
    if(GetCurrentThreadId()!=owner || !active() || fdkss_native_caller(runtime) || running_probe.load() || !pending_probe.exchange(false))return;
    running_probe.store(true);struct Reset {~Reset(){running_probe.store(false);}} reset;
    auto failed=[](const char *error){probe_status="failed";probe_error=error;};
    std::uintptr_t table=0;
    if(!read(object,&table,8)) {failed("current actor unavailable");return;}
    const auto &profiles=probe_sky2 ? actor_types::second : actor_types::first;
    std::size_t index=0;while(index<std::size(profiles) && table!=probe_base+profiles[index].table)++index;
    if(index==std::size(profiles)) {failed("current actor has no covered getter profile");return;}
    const auto &profile=profiles[index];const auto transform=probe_sky2 ? second_transform : first_transform;
    auto valid=[&](unsigned kind) {
        std::uintptr_t current=0;
        return active() && read(object,&current,8) && current==table &&
            checked(probe_base,profile,kind,transform,replacements[kind==3 ? type_slots+index : index*3+kind]);
    };
    for(unsigned kind=0;kind<4;++kind)if(!valid(kind)){failed("getter slot/code changed before probe");return;}
    const char *name=reinterpret_cast<const char *(__fastcall *)(void *)>(replacements[index*3])(reinterpret_cast<void *>(object));
    if(!read(reinterpret_cast<std::uintptr_t>(name),probe_name.data(),probe_name.size()) || !std::memchr(probe_name.data(),0,probe_name.size())) {failed("getter type name could not be copied");return;}
    if(!valid(1)){failed("getter changed during probe");return;}
    probe_id=reinterpret_cast<std::uint32_t (__fastcall *)(void *)>(replacements[index*3+1])(reinterpret_cast<void *>(object));
    if(!valid(2)){failed("getter changed during probe");return;}
    probe_flags=reinterpret_cast<std::uint32_t (__fastcall *)(void *)>(replacements[index*3+2])(reinterpret_cast<void *>(object));
    if(!valid(3)){failed("getter changed during probe");return;}
    const auto matrix=reinterpret_cast<const float *(__fastcall *)(void *)>(replacements[type_slots+index])(reinterpret_cast<void *>(object));
    if(reinterpret_cast<std::uintptr_t>(matrix)!=object+0x58 || !read(object+0x58,probe_matrix.data(),sizeof(probe_matrix)) ||
        std::any_of(probe_matrix.begin(),probe_matrix.end(),[](float value){return !std::isfinite(value);})) {failed("getter transform could not be copied");return;}
    for(unsigned kind=0;kind<4;++kind)if(!valid(kind)){failed("getter changed during probe");return;}
    probe_status="succeeded";probe_error="";
}
int FDKSS_CALL probe_result(void *,const fdkss_value *,std::size_t argc,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    if(argc || GetCurrentThreadId()!=owner){std::snprintf(error,size,"actor probe result requires no arguments on the owner thread");return 1;}
    fdkss_value value{};value.type=FDKSS_STRING;
    for(const char *text : {probe_status,probe_error}) {value.string=text;value.string_size=std::strlen(text);if(emit(sink,&value))return 1;}
    if(std::strcmp(probe_status,"succeeded"))return 0;
    value.string=probe_name.data();value.string_size=std::strlen(value.string);if(emit(sink,&value))return 1;
    value.type=FDKSS_INTEGER;value.integer=probe_id;if(emit(sink,&value))return 1;value.integer=probe_flags;if(emit(sink,&value))return 1;
    value.type=FDKSS_NUMBER;for(float number : probe_matrix){value.number=number;if(emit(sink,&value))return 1;}
    return 0;
}
int FDKSS_CALL info(void *,const fdkss_value *,std::size_t argc,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    if (argc) {std::snprintf(error,size,"actor hook info expects no arguments");return 1;}
    char text[1024]{};
    std::snprintf(text,sizeof(text),
        "{\"hooked\":%s,\"slots\":%u,\"calls\":%llu,\"observed\":%llu,\"methods\":["
        "{\"name\":\"actor.type_name\",\"calls\":%llu,\"observed\":%llu},"
        "{\"name\":\"actor.type_id\",\"calls\":%llu,\"observed\":%llu},"
        "{\"name\":\"actor.type_flags\",\"calls\":%llu,\"observed\":%llu},"
        "{\"name\":\"actor.get_transform\",\"calls\":%llu,\"observed\":%llu}]}",
        active()?"true":"false",static_cast<unsigned>(active()?count:0),
        static_cast<unsigned long long>(calls.load()),static_cast<unsigned long long>(observed.load()),
        static_cast<unsigned long long>(method_calls[0].load()),static_cast<unsigned long long>(method_observed[0].load()),
        static_cast<unsigned long long>(method_calls[1].load()),static_cast<unsigned long long>(method_observed[1].load()),
        static_cast<unsigned long long>(method_calls[2].load()),static_cast<unsigned long long>(method_observed[2].load()),
        static_cast<unsigned long long>(method_calls[3].load()),static_cast<unsigned long long>(method_observed[3].load()));
    fdkss_value value{};value.type=FDKSS_STRING;value.string=text;value.string_size=std::strlen(text);return emit(sink,&value);
}
}
