#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace framework_bridge {
// A manually proved machine-code contract, not output from a discovery scanner.
// The sole input is Framework* in RCX; RAX is preserved through the original call.
// Its successful phase must finish before the callback. No Actor access is implied.
struct Profile {
    std::uintptr_t image_base;
    std::size_t image_size;
    std::uint32_t frame_rva, root_rva, table_rva, owner_thread_rva, delta_offset;
    std::span<const unsigned char> original_body;
};
using Callback = void (*)(double delta, std::uintptr_t original_result);
// One install attempt per pinned module lifetime. The trampoline remains allocated
// after stop so calls already entering the bridge can finish safely.
bool install(const Profile &profile, Callback callback);
// False after a foreign mutation leaves its bytes untouched and dispatch disabled;
// a residual pass-through detour/trampoline can remain pinned. True verifies the
// complete original body after restoration.
bool stop();
// Whether callback dispatch is enabled, not a claim that code has been restored.
bool active();
}
