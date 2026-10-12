#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "lifetimes.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace actor_lifetimes {
namespace {
constexpr std::size_t maximum_hooks = 32, maximum_addresses = 65536;
std::array<Hook, maximum_hooks> bindings{};
std::size_t binding_count = 0;
bool attempted = false;
std::atomic<bool> enabled{false};
SRWLOCK registry_lock = SRWLOCK_INIT;
struct Guard {
    Guard() { AcquireSRWLockExclusive(&registry_lock); }
    ~Guard() { ReleaseSRWLockExclusive(&registry_lock); }
};
struct Record { std::uint64_t generation; bool alive; };
std::unordered_map<std::uintptr_t, Record> addresses;
std::uint64_t epoch = 1, serial = 0, dropped = 0;
std::array<Event, event_queue_capacity> queued{};
std::size_t queued_count = 0;
std::size_t queue_peak = 0;
std::uint64_t total_dropped = 0;
void enqueue(std::uintptr_t address, const char *class_name) {
    // Caller holds registry_lock. Fixed storage only on an engine thread.
    if (queued_count < queued.size()) {
        queued[queued_count++] = {address, class_name};
        queue_peak = std::max(queue_peak, queued_count);
    } else { ++dropped; ++total_dropped; }
}

template<std::size_t Index>
void *__fastcall destroy(void *object, unsigned flags) {
    // No allocations, engine reads or Lua callbacks on a destructor thread.
    if (enabled.load(std::memory_order_acquire)) {
        Guard guard;
        const auto address = reinterpret_cast<std::uintptr_t>(object);
        if (auto found = addresses.find(address); found != addresses.end()) {
            found->second = {++serial, false};
        }
        enqueue(address, bindings[Index].class_name);
    }
    return bindings[Index].original(object, flags);
}
template<std::size_t... Index>
constexpr auto thunks(std::index_sequence<Index...>) {
    return std::array<Destructor, sizeof...(Index)>{&destroy<Index>...};
}
constexpr auto replacements = thunks(std::make_index_sequence<maximum_hooks>{});
bool exchange(void **slot, void *expected, void *replacement) {
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protection)) return false;
    const auto previous = InterlockedCompareExchangePointer(reinterpret_cast<void *volatile *>(slot), replacement, expected);
    DWORD ignored = 0;
    const bool restored = VirtualProtect(slot, sizeof(void *), protection, &ignored) != 0;
    return previous == expected && restored;
}
}
bool install(std::span<const Hook> hooks) {
    if (attempted || hooks.empty() || hooks.size() > bindings.size()) return false;
    for (std::size_t index = 0; index < hooks.size(); ++index) {
        const auto &hook = hooks[index];
        if (!hook.slot || !hook.original || !hook.class_name || *hook.slot != reinterpret_cast<void *>(hook.original)) return false;
        for (std::size_t earlier = 0; earlier < index; ++earlier)
            if (hook.slot == hooks[earlier].slot) return false;
    }
    attempted = true;
    // Publish every call-through target before making any thunk reachable.
    std::copy(hooks.begin(), hooks.end(), bindings.begin());
    binding_count = hooks.size();
    enabled.store(true, std::memory_order_release);
    for (std::size_t index = 0; index < hooks.size(); ++index) {
        if (!exchange(bindings[index].slot, reinterpret_cast<void *>(bindings[index].original),
            reinterpret_cast<void *>(replacements[index]))) {
            stop(); return false;
        }
    }
    return true;
}
bool stop() {
    enabled.store(false, std::memory_order_release);
    bool success = true;
    for (std::size_t index = 0; index < binding_count; ++index) {
        if (*bindings[index].slot != reinterpret_cast<void *>(replacements[index])) continue;
        if (!exchange(bindings[index].slot, reinterpret_cast<void *>(replacements[index]),
            reinterpret_cast<void *>(bindings[index].original))) success = false;
    }
    Guard guard;
    ++epoch; addresses.clear(); queued_count = 0; dropped = 0; queue_peak = 0; total_dropped = 0;
    return success;
}
Stamp observe(std::uintptr_t address) {
    Guard guard;
    auto found = addresses.find(address);
    if (found == addresses.end()) {
        if (addresses.size() == maximum_addresses) { addresses.clear(); ++epoch; }
        found = addresses.emplace(address, Record{++serial, true}).first;
    } else if (!found->second.alive) found->second = {++serial, true};
    return {epoch, found->second.generation};
}
bool valid(std::uintptr_t address, Stamp stamp) {
    Guard guard;
    const auto found = addresses.find(address);
    return stamp.epoch == epoch && found != addresses.end() && found->second.alive &&
        found->second.generation == stamp.generation;
}
void constructing(std::uintptr_t address) {
    Guard guard;
    if (const auto found = addresses.find(address); found != addresses.end())
        found->second = {++serial, false};
}
Events drain() {
    Guard guard;
    Events result{{queued.begin(), queued.begin() + queued_count}, dropped};
    queued_count = 0; dropped = 0;
    return result;
}
void destructing(std::uintptr_t address, const char *class_name) {
    Guard guard;
    const auto found = addresses.find(address);
    if (found == addresses.end() || !found->second.alive) return;
    found->second = {++serial, false};
    enqueue(address, class_name);
}
QueueInfo queue_info() {
    Guard guard;
    return {queued_count,queue_peak,dropped,total_dropped};
}
}
