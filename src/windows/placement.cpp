#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "placement.h"
#include "placement_hooks.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
namespace {
int fail(const char *text, char *error, std::size_t capacity) { std::snprintf(error, capacity, "%s", text); return 1; }
template<class T> bool read(std::uintptr_t address, T &value) {
    SIZE_T count = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(address), &value, sizeof(value), &count) && count == sizeof(value);
}
bool number(const fdkss_value &value, double &out) {
    if (value.type == FDKSS_NUMBER) out = value.number;
    else if (value.type == FDKSS_INTEGER) out = static_cast<double>(value.integer);
    else return false;
    return std::isfinite(out);
}
bool upright_yaw(void *actor, double &yaw) {
    std::array<float, 4> rotation{};
    if (!actor || !read(reinterpret_cast<std::uintptr_t>(actor) + 0x108, rotation)) return false;
    for (const auto value : rotation) if (!std::isfinite(value)) return false;
    const double norm = static_cast<double>(rotation[1]) * rotation[1] + static_cast<double>(rotation[3]) * rotation[3];
    if (std::abs(rotation[0]) > 0.00001f || std::abs(rotation[2]) > 0.00001f || norm < 0.5 || norm > 1.5) return false;
    // q and -q encode the same orientation. Return a canonical angle in [-pi, pi].
    yaw = std::remainder(2 * std::atan2(static_cast<double>(rotation[1]), static_cast<double>(rotation[3])), 2 * std::numbers::pi);
    return true;
}
}
namespace character_placement {
int yaw(void *actor, fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    double angle = 0;
    if (!upright_yaw(actor, angle)) return fail("yaw requires a finite upright character quaternion", error, capacity);
    fdkss_value result{}; result.type = FDKSS_NUMBER; result.number = angle; return emit(sink, &result);
}
Setter checked_target(std::uintptr_t address) {
    // Both independently traced routines have this same instruction prefix.
    constexpr std::array<unsigned char, 13> expected{0x40,0x53,0x48,0x83,0xec,0x30,0xf6,0x42,0x03,0x01,0x48,0x8b,0xd9};
    std::array<unsigned char, 13> actual{};
    return read(address, actual) && actual == expected ? reinterpret_cast<Setter>(address) : placement_hooks::original_for(address);
}
int invoke(void *actor, Setter setter, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if (!actor || !setter) return fail("character placement target unavailable or changed", error, capacity);
    if (count != 4) return fail("set_placement expects x, y, z and yaw_radians", error, capacity);
    double values[4]{};
    for (std::size_t i = 0; i < 4; ++i) {
        if (!number(args[i], values[i])) return fail("placement arguments must be finite numbers", error, capacity);
        const auto bound = i < 3 ? 1000000.0 : 2 * std::numbers::pi;
        if (values[i] < -bound || values[i] > bound) return fail("placement coordinates must be within +/-1000000 and yaw within +/-2*pi", error, capacity);
    }
    const Placement placement{0x02000000, static_cast<float>(values[0]), static_cast<float>(values[1]),
        static_cast<float>(values[2]), static_cast<float>(values[3])};
    setter(actor, &placement);
    fdkss_value result{}; return emit(sink, &result);
}
int coordinate(void *actor, Setter setter, std::string_view property, const fdkss_value &replacement,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    const auto index = property == "position_x" ? 0 : property == "position_y" ? 1 : property == "position_z" ? 2 : property == "yaw_radians" ? 3 : -1;
    if (index < 0) return fail("unknown placement coordinate", error, capacity);
    std::array<float, 3> position{};
    const auto address = reinterpret_cast<std::uintptr_t>(actor);
    if (!actor || !read(address + 0xd8, position))
        return fail("character pose could not be read", error, capacity);
    // The recovered placement routine is yaw-only. Never silently discard pitch/roll.
    double yaw = 0;
    if (!upright_yaw(actor, yaw))
        return fail("pose editing requires an upright character; use explicit set_placement for yaw-only placement", error, capacity);
    fdkss_value args[4]{};
    for (int i = 0; i < 3; ++i) { args[i].type = FDKSS_NUMBER; args[i].number = position[i]; }
    args[3].type = FDKSS_NUMBER; args[3].number = yaw; args[index] = replacement;
    // Consume the method's nil result internally, then return the written float.
    const auto discard = [](void *, const fdkss_value *) -> int { return 0; };
    if (invoke(actor, setter, args, 4, discard, nullptr, error, capacity)) return 1;
    double value = 0; number(replacement, value);
    fdkss_value result{}; result.type = FDKSS_NUMBER; result.number = static_cast<float>(value); return emit(sink, &result);
}
}
