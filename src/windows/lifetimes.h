#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace actor_lifetimes {
using Destructor = void *(__fastcall *)(void *, unsigned);
struct Hook { void **slot; Destructor original; const char *class_name; };
struct Stamp { std::uint64_t epoch, generation; };
struct Event { std::uintptr_t address; const char *class_name; };
struct Events { std::vector<Event> destroyed; std::uint64_t dropped; };
// Both supported scene enumerators admit at most 16384 actors. Keep a bounded
// queue large enough for that many destruction notifications between updates.
inline constexpr std::size_t event_queue_capacity = 16384;
struct QueueInfo { std::size_t pending, peak; std::uint64_t pending_dropped, total_dropped; };
QueueInfo queue_info();
// Install once per pinned DLL. Thunks remain valid after restoration for in-flight calls.
bool install(std::span<const Hook> hooks);
bool stop();
Stamp observe(std::uintptr_t address);
// Invalidate an existing stamp before a constructor can reuse the address.
// Does not allocate or create a registry entry on an engine constructor thread.
void constructing(std::uintptr_t address);
// Direct verified destructor entries invalidate tracked live addresses and queue
// their base-class diagnostic once. A preceding vtable thunk already did both.
// No allocation or registry insertion on the destructor thread.
void destructing(std::uintptr_t address, const char *class_name);
bool valid(std::uintptr_t address, Stamp stamp);
Events drain();
}
