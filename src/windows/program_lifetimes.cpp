#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include "program_lifetimes.h"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <limits>
#include <mutex>

namespace program_lifetimes {
namespace {
using Destructor = void *(__fastcall *)(void *);
Destructor original = nullptr;
void *target = nullptr;
bool attempted = false, restored = false, second = false;
DWORD owner = 0;
std::atomic<bool> enabled{false};
std::atomic<unsigned> active{0};
std::mutex mutex;
struct Record { std::uintptr_t address = 0; std::uint64_t generation = 0; bool alive = false; };
std::array<Record,64> records{};
std::array<Event,128> queued{};
std::size_t queued_count = 0;
std::uint64_t serial = 0, dropped = 0, destroyed = 0;
std::array<unsigned char,5> patch{};
bool read(std::uintptr_t address, void *out, std::size_t size) {
    SIZE_T count = 0;
    return address && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void *>(address),out,size,&count) && count == size;
}
std::uintptr_t pointer(std::uintptr_t address) { std::uintptr_t result = 0; read(address,&result,8); return result; }
void *__fastcall destroy(void *descriptor) {
    active.fetch_add(1,std::memory_order_acq_rel);
    struct Exit { ~Exit() { active.fetch_sub(1,std::memory_order_release); } } exit;
    if (enabled.load(std::memory_order_acquire)) {
        Event event;
        {
            std::lock_guard guard(mutex);
            for (auto &record : records) if (record.address == reinterpret_cast<std::uintptr_t>(descriptor)) {
                event.generation = record.generation; record.alive = false; break;
            }
            ++destroyed;
        }
        // The stamp is already invalid. Copy the bounded path before the engine
        // clears it and frees variables/descriptor. Never call Lua here.
        if (!read(reinterpret_cast<std::uintptr_t>(descriptor) + 12,event.path.data(),event.path.size()) ||
            !std::memchr(event.path.data(),0,event.path.size())) event.path.fill(0);
        std::lock_guard guard(mutex);
        if (enabled.load(std::memory_order_acquire)) {
            if (queued_count < queued.size()) queued[queued_count++] = event;
            else ++dropped;
        }
    }
    return original(descriptor);
}
struct Profile { std::uintptr_t destructor, memory, allocate; };
constexpr Profile first{0x3de830,0x8feff0,0x49caf0}, next{0x4cb850,0xc60e78,0x5ca0c0};
}
Stamp observe(std::uintptr_t descriptor) {
    std::lock_guard guard(mutex);
    if (!descriptor || !enabled.load(std::memory_order_acquire) || active.load(std::memory_order_acquire)) return {};
    Record *available = nullptr;
    for (auto &record : records) {
        if (record.address == descriptor) {
            if (record.alive) return {record.generation};
            available = &record; break;
        }
        if (!record.alive && !available) available = &record;
    }
    if (!available || serial == static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return {};
    *available = {descriptor,++serial,true}; return {serial};
}
bool valid(std::uintptr_t descriptor, Stamp stamp) {
    std::lock_guard guard(mutex);
    if (!stamp.generation || !enabled.load(std::memory_order_acquire)) return false;
    for (const auto &record : records) if (record.address == descriptor)
        return record.alive && record.generation == stamp.generation;
    return false;
}
bool install(void *address, std::uint32_t owning_thread) {
    if (attempted || !address || !owning_thread) return false;
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return false;
    attempted = true; target = address; owner = owning_thread;
    if (MH_CreateHook(target,reinterpret_cast<void *>(&destroy),reinterpret_cast<void **>(&original)) != MH_OK) return false;
    enabled.store(true,std::memory_order_release);
    if (MH_EnableHook(target) != MH_OK) { enabled.store(false,std::memory_order_release); MH_RemoveHook(target); original = nullptr; return false; }
    std::memcpy(patch.data(),target,patch.size()); return true;
}
bool start(bool sky2, std::uint32_t owning_thread) {
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    constexpr unsigned char prefix1[]{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b};
    constexpr unsigned char prefix2[]{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b};
    unsigned char actual[8]{};
    const auto &profile = sky2 ? next : first;
    if (!read(base + profile.destructor,actual,sizeof(actual)) || std::memcmp(actual,sky2 ? prefix2 : prefix1,sizeof(actual))) return false;
    second = sky2; return install(reinterpret_cast<void *>(base + profile.destructor),owning_thread);
}
bool stop() {
    enabled.store(false,std::memory_order_release);
    if (original && !restored) {
        if (std::memcmp(target,patch.data(),patch.size()) == 0) {
            const auto disabled = MH_DisableHook(target);
            if (disabled != MH_OK && disabled != MH_ERROR_DISABLED) return false;
            restored = true;
        } else restored = true; // Preserve a later external patch.
    }
    if (active.load(std::memory_order_acquire)) return false;
    std::lock_guard guard(mutex);
    records.fill({}); queued_count = 0; dropped = 0;
    // This DLL stays pinned. Retain the trampoline for a thread that had already
    // branched to the detour before its active counter increment, and for any
    // later external hook that may chain through our detour.
    return true;
}
Events drain() {
    std::lock_guard guard(mutex);
    Events result; result.unloaded.assign(queued.begin(),queued.begin() + queued_count); result.dropped = dropped;
    queued_count = 0; dropped = 0; return result;
}
void emit_events(fdkss_runtime *runtime) {
    if (GetCurrentThreadId() != owner) return;
    const auto events = drain();
    for (const auto &event : events.unloaded) {
        fdkss_value values[2]{};
        values[0].type = FDKSS_STRING; values[0].string = event.path.data(); values[0].string_size = std::strlen(event.path.data());
        values[1].type = FDKSS_INTEGER; values[1].integer = static_cast<std::int64_t>(event.generation);
        fdkss_emit_values(runtime,"script.unloaded",values,2);
    }
    if (events.dropped) {
        fdkss_value value{}; value.type = FDKSS_INTEGER; value.integer = static_cast<std::int64_t>(events.dropped);
        fdkss_emit_values(runtime,"script.unloaded.overflow",&value,1);
    }
}
int FDKSS_CALL info(void *, const char *, char *out, std::size_t size) {
    std::lock_guard guard(mutex);
    std::snprintf(out,size,"{\"enabled\":%s,\"destroyed\":%llu,\"active\":%u,\"pending\":%zu,\"dropped\":%llu}",
        enabled.load() ? "true" : "false",static_cast<unsigned long long>(destroyed),active.load(),queued_count,static_cast<unsigned long long>(dropped));
    return 0;
}
int FDKSS_CALL self_test(void *, const fdkss_value *, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    auto fail = [&](const char *reason) { std::snprintf(error,size,"%s",reason); return 1; };
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    const auto &profile = second ? next : first;
    if (count || !enabled.load() || GetCurrentThreadId() != owner || target != reinterpret_cast<void *>(base + profile.destructor))
        return fail("script lifetime probe requires the active owner profile and no arguments");
    constexpr unsigned char prefix[]{0x4c,0x8b,0xdc,0x53,0x41,0x56,0x48,0x81};
    unsigned char actual[8]{}; const auto memory = pointer(base + profile.memory);
    if (!memory || !read(base + profile.allocate,actual,8) || std::memcmp(actual,prefix,8)) return fail("script lifetime allocator profile changed");
    using Allocate = void *(__fastcall *)(void *,std::uint32_t);
    for (unsigned probe = 0; probe < 4; ++probe) {
        auto *descriptor = reinterpret_cast<Allocate>(base + profile.allocate)(reinterpret_cast<void *>(memory + 8),0x58);
        if (!descriptor) return fail("script lifetime allocation failed");
        std::memset(descriptor,0,0x58);
        std::snprintf(static_cast<char *>(descriptor) + 12,32,"FDKssLifetime%u",probe);
        const auto stamp = observe(reinterpret_cast<std::uintptr_t>(descriptor));
        const bool before = valid(reinterpret_cast<std::uintptr_t>(descriptor),stamp);
        reinterpret_cast<Destructor>(target)(descriptor);
        if (!before || valid(reinterpret_cast<std::uintptr_t>(descriptor),stamp)) return fail("script lifetime invalidation failed");
    }
    fdkss_value value{}; value.type = FDKSS_INTEGER; value.integer = 4; return emit(sink,&value);
}
}
