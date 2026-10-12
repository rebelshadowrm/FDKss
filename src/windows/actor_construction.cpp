#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include "actor_construction.h"
#include "actor_construction_profiles.h"
#include "lifetimes.h"
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
namespace actor_construction {
namespace {
using Constructor = void *(__fastcall *)(void *);
Constructor original = nullptr;
actor_lifetimes::Destructor original_destructor = nullptr;
const Profile *profile = nullptr;
std::uintptr_t image = 0;
DWORD owner = 0;
void *target = nullptr;
void *destructor_target = nullptr;
bool attempted = false;
std::atomic<bool> enabled{false};
std::atomic<std::uint64_t> calls{0};
std::atomic<std::uint64_t> destructor_calls{0};
std::array<unsigned char,5> patch{};
std::array<unsigned char,5> destructor_patch{};
struct Event { std::uintptr_t address; bool diagnostic; };
SRWLOCK queue_lock = SRWLOCK_INIT;
struct Guard {
    Guard() { AcquireSRWLockExclusive(&queue_lock); }
    ~Guard() { ReleaseSRWLockExclusive(&queue_lock); }
};
std::array<Event,2048> queued{};
std::size_t count = 0;
std::uint64_t dropped = 0;
thread_local bool diagnostic = false;
bool read(std::uintptr_t address, void *output, std::size_t size) {
    SIZE_T copied=0;
    return address && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void *>(address),output,size,&copied) && copied==size;
}
bool executable(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION region{};
    return VirtualQuery(reinterpret_cast<void *>(address),&region,sizeof(region)) && region.State==MEM_COMMIT &&
        !(region.Protect & (PAGE_GUARD|PAGE_NOACCESS)) &&
        (region.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
}
bool matches(std::uintptr_t address, const char *hex, std::size_t skip=0) {
    std::array<unsigned char,256> code{};
    const auto length=std::strlen(hex)/2;
    if (!length || length>code.size() || skip>=length || !read(address+skip,code.data()+skip,length-skip)) return false;
    const auto nibble=[](char c)->unsigned { return c<='9' ? c-'0' : c-'a'+10; };
    for (std::size_t i=skip;i<length;++i) if (code[i]!=(nibble(hex[2*i])*16+nibble(hex[2*i+1]))) return false;
    return true;
}
void *__fastcall construct(void *object) {
    calls.fetch_add(1,std::memory_order_relaxed);
    if (!enabled.load(std::memory_order_acquire)) return original(object);
    const auto address=reinterpret_cast<std::uintptr_t>(object);
    actor_lifetimes::constructing(address);
    const auto result=original(object);
    if (result==object && enabled.load(std::memory_order_acquire)) {
        Guard guard;
        if (enabled.load(std::memory_order_acquire)) {
            if (count<queued.size()) queued[count++]={address,diagnostic}; else ++dropped;
        }
    }
    return result;
}
int fail(const char *message, char *error, std::size_t size) {std::snprintf(error,size,"%s",message);return 1;}
void *__fastcall destruct(void *object, unsigned flags) {
    destructor_calls.fetch_add(1,std::memory_order_relaxed);
    if (enabled.load(std::memory_order_acquire))
        actor_lifetimes::destructing(reinterpret_cast<std::uintptr_t>(object),".?AVActor@fdk@@");
    return original_destructor(object,flags);
}
}
bool start(std::uintptr_t base, bool second_game, unsigned long thread) {
    if (attempted || !base || !thread) return false;
    const auto &candidate=second_game ? actor_construction::sky2 : actor_construction::sky1;
    if (!executable(base+candidate.constructor) || !executable(base+candidate.destructor) ||
        !matches(base+candidate.constructor,candidate.constructor_hex) || !matches(base+candidate.destructor,candidate.destructor_hex)) return false;
    attempted=true;image=base;profile=&candidate;owner=thread;target=reinterpret_cast<void *>(base+candidate.constructor);
    destructor_target=reinterpret_cast<void *>(base+candidate.destructor);
    const auto initialization=MH_Initialize();
    if (initialization!=MH_OK && initialization!=MH_ERROR_ALREADY_INITIALIZED) return false;
    if (MH_CreateHook(target,reinterpret_cast<void *>(&construct),reinterpret_cast<void **>(&original))!=MH_OK) return false;
    if (MH_CreateHook(destructor_target,reinterpret_cast<void *>(&destruct),reinterpret_cast<void **>(&original_destructor))!=MH_OK) return false;
    enabled.store(true,std::memory_order_release);
    if (MH_EnableHook(target)!=MH_OK) {enabled.store(false,std::memory_order_release);return false;}
    std::memcpy(patch.data(),target,patch.size());
    if (MH_EnableHook(destructor_target)!=MH_OK) {
        enabled.store(false,std::memory_order_release);MH_DisableHook(target);return false;
    }
    std::memcpy(destructor_patch.data(),destructor_target,destructor_patch.size());return true;
}
bool stop() {
    enabled.store(false,std::memory_order_release);
    bool success=true;
    if (original && !std::memcmp(target,patch.data(),patch.size())) {
        const auto status=MH_DisableHook(target);
        if (status!=MH_OK && status!=MH_ERROR_DISABLED) success=false;
    }
    if (original_destructor && !std::memcmp(destructor_target,destructor_patch.data(),destructor_patch.size())) {
        const auto status=MH_DisableHook(destructor_target);
        if (status!=MH_OK && status!=MH_ERROR_DISABLED) success=false;
    }
    Guard guard;count=0;dropped=0;
    // Keep the trampoline callable for an in-flight or chained call. DLL is pinned.
    return success;
}
void emit_events(fdkss_runtime *runtime) {
    if (GetCurrentThreadId()!=owner || !enabled.load(std::memory_order_acquire)) return;
    std::array<Event,2048> events{};std::size_t size=0;std::uint64_t lost=0;
    {Guard guard;size=count;std::copy_n(queued.begin(),size,events.begin());count=0;lost=dropped;dropped=0;}
    for (std::size_t i=0;i<size;++i) {
        char address[32]{};std::snprintf(address,sizeof(address),"0x%llx",static_cast<unsigned long long>(events[i].address));
        constexpr char base_class[]=".?AVActor@fdk@@";
        fdkss_value values[3]{};values[0].type=values[1].type=FDKSS_STRING;
        values[0].string=address;values[0].string_size=std::strlen(address);
        values[1].string=base_class;values[1].string_size=sizeof(base_class)-1;
        values[2].type=FDKSS_BOOLEAN;values[2].boolean=events[i].diagnostic;
        fdkss_emit_values(runtime,"engine.actor_base_constructed",values,3);
    }
    if (lost) {fdkss_value value{};value.type=FDKSS_INTEGER;value.integer=static_cast<std::int64_t>(lost);fdkss_emit_values(runtime,"engine.lifecycle_overflow",&value,1);}
}
int FDKSS_CALL info(void *,const fdkss_value *,std::size_t argc,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    if (argc) return fail("actor constructor info expects no arguments",error,size);
    const auto destruction_queue=actor_lifetimes::queue_info();
    char json[512]{};
    {Guard guard;std::snprintf(json,sizeof(json),"{\"hooked\":%s,\"calls\":%llu,\"pending\":%u,\"dropped\":%llu,\"base_destructor_hooked\":%s,\"base_destructor_calls\":%llu,\"destruction_queue_capacity\":%u,\"destruction_events_pending\":%u,\"destruction_queue_peak\":%u,\"destruction_events_dropped_pending\":%llu,\"destruction_events_dropped\":%llu}",
        enabled.load()?"true":"false",static_cast<unsigned long long>(calls.load()),static_cast<unsigned>(count),static_cast<unsigned long long>(dropped),
        enabled.load()?"true":"false",static_cast<unsigned long long>(destructor_calls.load()),
        static_cast<unsigned>(actor_lifetimes::event_queue_capacity),static_cast<unsigned>(destruction_queue.pending),
        static_cast<unsigned>(destruction_queue.peak),static_cast<unsigned long long>(destruction_queue.pending_dropped),
        static_cast<unsigned long long>(destruction_queue.total_dropped));}
    fdkss_value value{};value.type=FDKSS_STRING;value.string=json;value.string_size=std::strlen(json);return emit(sink,&value);
}
int FDKSS_CALL self_test(void *,const fdkss_value *,std::size_t argc,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    if (argc || !enabled.load(std::memory_order_acquire) || GetCurrentThreadId()!=owner ||
        std::memcmp(target,patch.data(),patch.size()) || !matches(image+profile->constructor,profile->constructor_hex,patch.size()) ||
        std::memcmp(destructor_target,destructor_patch.data(),destructor_patch.size()) || !matches(image+profile->destructor,profile->destructor_hex,destructor_patch.size()))
        return fail("actor constructor self-test requires the active verified owner profile",error,size);
    alignas(16) std::array<unsigned char,0x190> storage{};
    const auto address=reinterpret_cast<std::uintptr_t>(storage.data());
    const auto previous=diagnostic;diagnostic=true;
    struct Reset { bool prior; ~Reset(){diagnostic=prior;} } reset{previous};
    auto stamp=actor_lifetimes::observe(address);
    unsigned passed=0;
    for (unsigned iteration=0;iteration<2;++iteration) {
        const auto result=reinterpret_cast<Constructor>(target)(storage.data());
        std::uintptr_t table=0;std::memcpy(&table,storage.data(),8);
        std::array<float,16> matrix{};std::memcpy(matrix.data(),storage.data()+0x58,sizeof(matrix));
        bool valid=result==storage.data() && table==image+profile->table && !actor_lifetimes::valid(address,stamp);
        for (unsigned i=0;i<16;++i) if (matrix[i]!=(i%5==0 ? 1.0f : 0.0f)) valid=false;
        stamp=actor_lifetimes::observe(address);
        // The exact pinned deleting destructor with flags zero releases members without freeing storage.
        reinterpret_cast<actor_lifetimes::Destructor>(image+profile->destructor)(storage.data(),0);
        valid=valid && !actor_lifetimes::valid(address,stamp);
        if (!valid) {actor_lifetimes::constructing(address);return fail("private actor construction identity, matrix or generation check failed",error,size);}
        ++passed;
    }
    actor_lifetimes::constructing(address);
    fdkss_value value{};value.type=FDKSS_INTEGER;value.integer=passed;return emit(sink,&value);
}
}
