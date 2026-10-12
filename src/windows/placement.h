#pragma once
#include "fdkss/api.h"
#include <cstdint>
#include <cstddef>
#include <string_view>
namespace character_placement {
struct Placement { std::uint32_t flags; float x, y, z, yaw_radians; };
static_assert(sizeof(Placement) == 20 && offsetof(Placement, yaw_radians) == 16);
using Setter = void (__fastcall *)(void *, const Placement *);
// Caller must resolve a current character and enforce its engine owner thread.
Setter checked_target(std::uintptr_t address);
// Derived from the verified yaw quaternion; rejects tilted/invalid poses.
int yaw(void *actor, fdkss_result_fn, void *, char *, std::size_t);
int invoke(void *actor, Setter, const fdkss_value *, std::size_t,
    fdkss_result_fn, void *, char *, std::size_t);
int coordinate(void *actor, Setter, std::string_view property, const fdkss_value &,
    fdkss_result_fn, void *, char *, std::size_t);
}
