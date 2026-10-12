#pragma once
#include <cstdint>
#include <vector>
#include <array>
#include "fdkss/api.h"
namespace program_lifetimes {
struct Stamp { std::uint64_t generation = 0; };
struct Event { std::array<char,32> path{}; std::uint64_t generation = 0; };
struct Events { std::vector<Event> unloaded; std::uint64_t dropped = 0; };
// Registration requires a live descriptor protected by engine ownership/order.
// Stamps detect invalidation; they do not pin memory during a VM slice.
Stamp observe(std::uintptr_t descriptor);
bool valid(std::uintptr_t descriptor, Stamp stamp);
bool start(bool sky2, std::uint32_t owner);
bool install(void *destructor, std::uint32_t owner); // Also used by native hook fixtures.
bool stop();
Events drain();
void emit_events(fdkss_runtime *);
int FDKSS_CALL info(void *, const char *, char *, std::size_t);
int FDKSS_CALL self_test(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
}
