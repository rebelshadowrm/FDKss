#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include "placement_hooks.h"
#include "placement_profiles.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
namespace placement_hooks {
namespace {
using Setter=character_placement::Setter;
Setter original=nullptr;
const Profile *profile=nullptr;
std::uintptr_t image=0;
fdkss_runtime *runtime=nullptr;
DWORD owner=0;
bool attempted=false;
std::atomic<bool> enabled{false},pending{false},probing{false};
std::atomic<std::uint64_t> calls{0},observed{0};
enum Bypass {inactive,foreign,nested_call,native_call,unreadable_actor,wrong_class};
std::array<std::atomic<std::uint64_t>,6> bypass{};
std::atomic<DWORD> last_thread{0};
thread_local bool nested=false;
std::array<unsigned char,6> patch{};
const char *status="idle",*probe_error="";
std::array<float,8> probe_values{};
bool read(std::uintptr_t address,void *out,std::size_t size) {
    SIZE_T copied=0;return address && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void *>(address),out,size,&copied) && copied==size;
}
bool executable(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION region{};
    return VirtualQuery(reinterpret_cast<void *>(address),&region,sizeof(region)) && region.State==MEM_COMMIT &&
        !(region.Protect & (PAGE_GUARD|PAGE_NOACCESS)) &&
        (region.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
}
bool body(std::uintptr_t target,const char *hex,std::size_t skip=0) {
    std::array<unsigned char,241> code{};
    if(std::strlen(hex)!=code.size()*2 || skip>=code.size() || !read(target+skip,code.data()+skip,code.size()-skip))return false;
    auto nibble=[](char c)->unsigned{return c<='9' ? c-'0' : c-'a'+10;};
    for(std::size_t i=skip;i<code.size();++i)if(code[i]!=nibble(hex[i*2])*16+nibble(hex[i*2+1]))return false;
    return true;
}
bool owned() {
    std::array<unsigned char,6> current{};
    return profile && read(image+profile->handler,current.data(),current.size()) && current==patch;
}
void __fastcall dispatch(void *actor,const character_placement::Placement *input) {
    calls.fetch_add(1,std::memory_order_relaxed);
    const auto thread=GetCurrentThreadId();last_thread.store(thread,std::memory_order_relaxed);
    std::uintptr_t table=0;
    int skipped=-1;
    if(!enabled.load(std::memory_order_acquire))skipped=inactive;
    else if(thread!=owner)skipped=foreign;
    else if(nested)skipped=nested_call;
    else if(fdkss_native_caller(runtime))skipped=native_call;
    else if(!read(reinterpret_cast<std::uintptr_t>(actor),&table,8))skipped=unreadable_actor;
    else if(table!=image+profile->table)skipped=wrong_class;
    if(skipped>=0){bypass[skipped].fetch_add(1,std::memory_order_relaxed);original(actor,input);return;}
    nested=true;struct Reset {~Reset(){nested=false;}} reset;
    char address[32]{};std::snprintf(address,sizeof(address),"0x%llx",static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(actor)));
    constexpr const char *type=".?AVActor@chara@sora@@";
    fdkss_value values[7]{};values[0].type=values[1].type=FDKSS_STRING;
    values[0].string=address;values[0].string_size=std::strlen(address);values[1].string=type;values[1].string_size=std::strlen(type);
    std::uint32_t flags=0;std::size_t count=3;const char *error=nullptr;
    if(!read(reinterpret_cast<std::uintptr_t>(input),&flags,4))error="placement flags could not be copied";
    else {
        values[2].type=FDKSS_INTEGER;values[2].integer=flags;
        if(flags & 0x02000000u) {
            character_placement::Placement copy{};
            if(!read(reinterpret_cast<std::uintptr_t>(input),&copy,sizeof(copy)) || copy.flags!=flags ||
                !std::isfinite(copy.x) || !std::isfinite(copy.y) || !std::isfinite(copy.z) || !std::isfinite(copy.yaw_radians))error="placement pose could not be copied";
            else {
                const float pose[]{copy.x,copy.y,copy.z,copy.yaw_radians};
                for(unsigned i=0;i<4;++i){values[3+i].type=FDKSS_NUMBER;values[3+i].number=pose[i];}count=7;
            }
        }
    }
    if(error){const auto index=values[2].type==FDKSS_INTEGER ? 3u : 2u;values[index].type=FDKSS_NIL;values[index+1].type=FDKSS_STRING;values[index+1].string=error;values[index+1].string_size=std::strlen(error);count=index+2;}
    try{fdkss_emit_values(runtime,"hook.actor.set_placement.pre",values,count);}catch(...){}
    original(actor,input);
    try{fdkss_emit_values(runtime,"hook.actor.set_placement.post",values,count);observed.fetch_add(1,std::memory_order_relaxed);}catch(...){}
}
int fail(const char *text,char *error,std::size_t size){std::snprintf(error,size,"%s",text);return 1;}
}
bool start(std::uintptr_t base,bool second,fdkss_runtime *r,unsigned long thread) {
    if(attempted || !base || !r || !thread)return false;
    const auto &candidate=second ? sky2 : sky1;
    if(!executable(base+candidate.handler) || !body(base+candidate.handler,candidate.code))return false;
    attempted=true;image=base;profile=&candidate;runtime=r;owner=thread;
    const auto initialized=MH_Initialize();if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED)return false;
    auto *target=reinterpret_cast<void *>(image+profile->handler);
    if(MH_CreateHook(target,reinterpret_cast<void *>(&dispatch),reinterpret_cast<void **>(&original))!=MH_OK)return false;
    enabled.store(true,std::memory_order_release);
    if(MH_EnableHook(target)!=MH_OK){enabled.store(false,std::memory_order_release);return false;}
    std::memcpy(patch.data(),target,patch.size());return true;
}
bool stop() {
    enabled.store(false,std::memory_order_release);
    if(pending.exchange(false)){status="cancelled";probe_error="placement hooks stopped";}
    if(original && owned()) {
        const auto result=MH_DisableHook(reinterpret_cast<void *>(image+profile->handler));
        if(result!=MH_OK && result!=MH_ERROR_DISABLED)return false;
    }
    return true; // Retain the trampoline for chained callers; DLL remains pinned.
}
bool active(){return enabled.load(std::memory_order_acquire);}
Setter original_for(std::uintptr_t address) {
    return active() && profile && address==image+profile->handler && owned() && body(address,profile->code,patch.size()) ? original : nullptr;
}
int FDKSS_CALL info(void *,const fdkss_value *,std::size_t argc,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    if(argc)return fail("placement hook info expects no arguments",error,size);
    char text[512]{};std::snprintf(text,sizeof(text),
        "{\"hooked\":%s,\"calls\":%llu,\"observed\":%llu,\"owner_thread\":%lu,\"last_thread\":%lu,"
        "\"bypassed\":{\"inactive\":%llu,\"foreign\":%llu,\"nested\":%llu,\"native\":%llu,\"unreadable_actor\":%llu,\"wrong_class\":%llu}}",
        active()?"true":"false",static_cast<unsigned long long>(calls.load()),static_cast<unsigned long long>(observed.load()),
        static_cast<unsigned long>(owner),static_cast<unsigned long>(last_thread.load()),
        static_cast<unsigned long long>(bypass[inactive].load()),static_cast<unsigned long long>(bypass[foreign].load()),
        static_cast<unsigned long long>(bypass[nested_call].load()),static_cast<unsigned long long>(bypass[native_call].load()),
        static_cast<unsigned long long>(bypass[unreadable_actor].load()),static_cast<unsigned long long>(bypass[wrong_class].load()));
    fdkss_value value{};value.type=FDKSS_STRING;value.string=text;value.string_size=std::strlen(text);return emit(sink,&value);
}
int FDKSS_CALL request_probe(void *,const fdkss_value *,std::size_t argc,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    if(argc || !active() || GetCurrentThreadId()!=owner || pending.load() || probing.load())return fail("private placement probe requires active owner hooks and an empty diagnostic queue",error,size);
    status="pending";probe_error="";pending.store(true);fdkss_value value{};value.type=FDKSS_BOOLEAN;value.boolean=1;return emit(sink,&value);
}
void service_probe() {
    if(!active() || GetCurrentThreadId()!=owner || fdkss_native_caller(runtime) || probing.load() || !pending.exchange(false))return;
    if(!original_for(image+profile->handler)){status="failed";probe_error="placement slot/code changed before private probe";return;}
    probing.store(true);struct Reset {~Reset(){probing.store(false);}} reset;
    alignas(16) std::array<unsigned char,0x2600> storage{};
    const auto table=image+profile->table;std::memcpy(storage.data(),&table,8);
    const character_placement::Placement input{0x02000000,1.25f,2.5f,3.75f,.5f};
    reinterpret_cast<Setter>(image+profile->handler)(storage.data(),&input);
    std::array<float,3> current{},previous{};std::array<float,4> rotation{};
    std::memcpy(current.data(),storage.data()+0xd8,sizeof(current));std::memcpy(previous.data(),storage.data()+0xe8,sizeof(previous));std::memcpy(rotation.data(),storage.data()+0x108,sizeof(rotation));
    const float expected[]{input.x,input.y,input.z};bool valid=true;
    for(unsigned i=0;i<3;++i)if(current[i]!=expected[i] || previous[i]!=expected[i])valid=false;
    for(const auto value : rotation)if(!std::isfinite(value))valid=false;
    if(rotation[0]!=0 || rotation[2]!=0 || std::abs(rotation[1]-std::sin(.25f))>0.000001f || std::abs(rotation[3]-std::cos(.25f))>0.000001f)valid=false;
    if(!valid){status="failed";probe_error="private placement pose check failed";return;}
    probe_values={current[0],current[1],current[2],input.yaw_radians,rotation[0],rotation[1],rotation[2],rotation[3]};status="succeeded";probe_error="";
}
int FDKSS_CALL probe_result(void *,const fdkss_value *,std::size_t argc,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    if(argc || GetCurrentThreadId()!=owner)return fail("placement probe result requires no arguments on the owner thread",error,size);
    fdkss_value value{};value.type=FDKSS_STRING;
    for(const char *text : {status,probe_error}){value.string=text;value.string_size=std::strlen(text);if(emit(sink,&value))return 1;}
    if(std::strcmp(status,"succeeded"))return 0;
    value.type=FDKSS_NUMBER;for(const auto number : probe_values){value.number=number;if(emit(sink,&value))return 1;}return 0;
}
}
