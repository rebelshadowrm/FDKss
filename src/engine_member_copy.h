#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace engine_members {
// Instruction/layout parameters, not an adapter approval. The caller must
// establish the receiver's current identity, lifetime, owner and field extent.
struct FloatBlockProfile {
    std::size_t slot;
    std::uint32_t member_offset;
    std::uint32_t float_offset;
    bool displacement32 = false;
};
enum class CopyStatus { ok, getter_changed, copy_failed, nonfinite };

// Read(address, destination, byte_count), Executable(address),
// Owns(slot_address, original_getter), Invoke(original_getter, receiver).
// Invocation happens only after the original pointer/body have been checked.
// Output is published only after a complete finite copy; no engine storage escapes.
template<std::size_t Count, class Read, class Executable, class Owns, class Invoke>
CopyStatus copy_floats(std::uintptr_t actor, std::uintptr_t getter,
    const FloatBlockProfile &profile, std::array<float, Count> &output,
    Read read, Executable executable, Owns owns, Invoke invoke) {
    static_assert(Count > 0 && Count <= 64);
    constexpr auto maximum = std::numeric_limits<std::uintptr_t>::max();
    if (!actor || !getter || profile.slot >= 512 || profile.member_offset < 8 ||
        profile.member_offset > 0x100000 || profile.float_offset > 0x100000 ||
        (!profile.displacement32 && profile.member_offset > 127) ||
        actor > maximum - profile.member_offset ||
        actor + profile.member_offset > maximum - profile.float_offset ||
        actor + profile.member_offset + profile.float_offset > maximum - sizeof(output))
        return CopyStatus::getter_changed;
    std::uintptr_t table = 0, target = 0;
    if (!read(actor, &table, sizeof(table)) || !table ||
        table > maximum - profile.slot * sizeof(std::uintptr_t) - sizeof(std::uintptr_t))
        return CopyStatus::getter_changed;
    const auto slot = table + profile.slot * sizeof(std::uintptr_t);
    if (!read(slot, &target, sizeof(target)) || (target != getter && !owns(slot, getter)) || !executable(getter))
        return CopyStatus::getter_changed;
    std::array<unsigned char, 8> expected{0x48,0x8d,0x81,0,0,0,0,0xc3}, actual{};
    const std::size_t size = profile.displacement32 ? 8 : 5;
    if (profile.displacement32) std::memcpy(expected.data() + 3, &profile.member_offset, 4);
    else {
        expected[2] = 0x41; expected[3] = static_cast<unsigned char>(profile.member_offset); expected[4] = 0xc3;
    }
    if (!read(getter, actual.data(), size) || std::memcmp(actual.data(), expected.data(), size))
        return CopyStatus::getter_changed;
    const auto member = actor + profile.member_offset;
    if (invoke(getter, actor) != member) return CopyStatus::copy_failed;
    std::array<float, Count> copy{};
    if (!read(member + profile.float_offset, copy.data(), sizeof(copy))) return CopyStatus::copy_failed;
    for (const auto value : copy) if (!std::isfinite(value)) return CopyStatus::nonfinite;
    output = copy;
    return CopyStatus::ok;
}
}
