#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include "resource_hooks.h"
#include "player_resources.h"
#include <atomic>
#include <limits>

namespace resource_hooks {
namespace {
// Preserve the original RAX even though the approved public setter ignores it.
using Original=std::uint64_t (__fastcall *)(void *,std::uint32_t,std::int32_t);
Original original=nullptr;
const player_resources::SetterProfile *profile=nullptr;
std::uintptr_t image=0;
fdkss_runtime *runtime=nullptr;
DWORD owner=0;
bool attempted=false;
std::atomic<bool> enabled{false};
std::atomic<std::uint64_t> calls{0},observed{0},copy_errors{0};
enum Bypass {inactive,foreign,nested_call,native_call,unselected,contract,unreadable,invalid};
std::array<std::atomic<std::uint64_t>,8> bypass{};
thread_local bool nested=false;
// The first instructions are cmp edx,build limit (3 bytes), then ja rel32 (6 bytes).
// MinHook relocates that conditional branch. Pin the whole 9-byte entry region.
std::array<unsigned char,9> patch{};
bool executable(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION region{};
    return VirtualQuery(reinterpret_cast<void *>(address),&region,sizeof(region)) && region.State==MEM_COMMIT &&
        !(region.Protect&(PAGE_GUARD|PAGE_NOACCESS)) &&
        (region.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
}
bool owned() {
    std::array<unsigned char,9> bytes{};
    return profile && player_resources::read(image+profile->entry,bytes.data(),bytes.size()) && bytes==patch;
}
bool tail() {
    return profile && profile->code.size()>patch.size()*2 &&
        player_resources::pinned(image,profile->entry+patch.size(),profile->code.substr(patch.size()*2));
}
bool copy(void *status,std::array<std::uint32_t,9> &out) {
    const auto address=reinterpret_cast<std::uintptr_t>(status);
    return address && !(address%4) && address<=std::numeric_limits<std::uintptr_t>::max()-sizeof(out) &&
        player_resources::read(address,out.data(),sizeof(out));
}
bool valid(const std::array<std::uint32_t,9> &data) {
    for(unsigned i=3;i<9;i+=2)
        if(data[i]>data[i+1] || data[i+1]>static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))return false;
    return true;
}
void integer(fdkss_value &value,std::int64_t number){value.type=FDKSS_INTEGER;value.integer=number;}
void string(fdkss_value &value,const char *text){value.type=FDKSS_STRING;value.string=text;value.string_size=std::strlen(text);}
std::uint64_t __fastcall dispatch(void *status,std::uint32_t selector,std::int32_t request) {
    calls.fetch_add(1,std::memory_order_relaxed);
    std::array<std::uint32_t,9> before{};
    int skipped=-1;
    if(!enabled.load(std::memory_order_acquire))skipped=inactive;
    else if(GetCurrentThreadId()!=owner)skipped=foreign;
    else if(nested)skipped=nested_call;
    else if(fdkss_native_caller(runtime))skipped=native_call;
    else if(selector<7 || selector>12)skipped=unselected;
    else if(!owned() || !tail())skipped=contract;
    else if(!copy(status,before))skipped=unreadable;
    else if(!valid(before))skipped=invalid;
    if(skipped>=0){bypass[skipped].fetch_add(1,std::memory_order_relaxed);return original(status,selector,request);}
    nested=true;struct Reset {~Reset(){nested=false;}} reset;
    const auto index=3+((selector-7)/2)*2;
    const char *names[]{"hp","ep","cp"};
    char address[32]{};std::snprintf(address,sizeof(address),"0x%llx",static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(status)));
    fdkss_value values[11]{};
    string(values[0],address);integer(values[1],before[0]);integer(values[2],before[1]);
    string(values[3],names[(selector-7)/2]);string(values[4],selector%2 ? "set" : "add");
    integer(values[5],request);integer(values[6],before[index]);integer(values[7],before[index+1]);
    try{fdkss_emit_values(runtime,"hook.status.resource_change.pre",values,8);}catch(...){}
    const auto result=original(status,selector,request);
    std::array<std::uint32_t,9> after{};
    std::size_t count=10;
    if(copy(status,after) && valid(after) && after[0]==before[0] && after[1]==before[1]) {
        integer(values[8],after[index]);integer(values[9],after[index+1]);
    }else {
        string(values[10],"resource result could not be copied or its copied header changed");count=11;
        copy_errors.fetch_add(1,std::memory_order_relaxed);
    }
    try{fdkss_emit_values(runtime,"hook.status.resource_change.post",values,count);}catch(...){}
    observed.fetch_add(1,std::memory_order_relaxed);
    return result;
}
}
bool start(std::uintptr_t base,bool second,fdkss_runtime *r,unsigned long thread) {
    if(attempted || !base || !r || !thread)return false;
    const auto &candidate=second ? player_resources::sky2_setter : player_resources::sky1_setter;
    if(!executable(base+candidate.entry) || !player_resources::pinned(base,candidate.entry,candidate.code))return false;
    attempted=true;image=base;profile=&candidate;runtime=r;owner=thread;
    const auto initialized=MH_Initialize();if(initialized!=MH_OK && initialized!=MH_ERROR_ALREADY_INITIALIZED)return false;
    auto *target=reinterpret_cast<void *>(base+candidate.entry);
    if(MH_CreateHook(target,reinterpret_cast<void *>(&dispatch),reinterpret_cast<void **>(&original))!=MH_OK)return false;
    enabled.store(true,std::memory_order_release);
    if(MH_EnableHook(target)!=MH_OK){enabled.store(false,std::memory_order_release);return false;}
    std::memcpy(patch.data(),target,patch.size());return true;
}
bool stop() {
    enabled.store(false,std::memory_order_release);
    if(original && owned()) {
        const auto result=MH_DisableHook(reinterpret_cast<void *>(image+profile->entry));
        if(result!=MH_OK && result!=MH_ERROR_DISABLED)return false;
    }
    return true; // Keep the trampoline for saved/chained callers after runtime destruction.
}
bool active(){return enabled.load(std::memory_order_acquire);}
player_resources::Setter checked_target(std::uintptr_t base,const player_resources::SetterProfile &candidate) {
    const auto target=base+candidate.entry;
    if(!executable(target))return nullptr;
    if(active() && profile && base==image && candidate.entry==profile->entry && candidate.code==profile->code)
        return owned() && tail() ? reinterpret_cast<player_resources::Setter>(original) : nullptr;
    return player_resources::pinned(base,candidate.entry,candidate.code) ? reinterpret_cast<player_resources::Setter>(target) : nullptr;
}
int FDKSS_CALL info(void *,const fdkss_value *,std::size_t argc,fdkss_result_fn emit,void *sink,char *error,std::size_t capacity) {
    if(argc || !emit){if(error && capacity)std::snprintf(error,capacity,"resource hook info expects no arguments and a result sink");return 1;}
    char text[768]{};
    std::snprintf(text,sizeof(text),"{\"hooked\":%s,\"owner_thread\":%lu,\"calls\":%llu,\"observed\":%llu,\"copy_errors\":%llu,\"bypassed\":{\"inactive\":%llu,\"foreign\":%llu,\"nested\":%llu,\"native\":%llu,\"unselected\":%llu,\"contract\":%llu,\"unreadable\":%llu,\"invalid\":%llu}}",
        active()?"true":"false",static_cast<unsigned long>(owner),static_cast<unsigned long long>(calls.load()),
        static_cast<unsigned long long>(observed.load()),static_cast<unsigned long long>(copy_errors.load()),
        static_cast<unsigned long long>(bypass[0].load()),static_cast<unsigned long long>(bypass[1].load()),
        static_cast<unsigned long long>(bypass[2].load()),static_cast<unsigned long long>(bypass[3].load()),
        static_cast<unsigned long long>(bypass[4].load()),static_cast<unsigned long long>(bypass[5].load()),
        static_cast<unsigned long long>(bypass[6].load()),static_cast<unsigned long long>(bypass[7].load()));
    fdkss_value value{};string(value,text);return emit(sink,&value);
}
}
