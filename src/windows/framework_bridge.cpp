#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include "framework_bridge.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

namespace framework_bridge {
namespace {
using Frame = std::uintptr_t (__fastcall *)(void *);
std::mutex mutex;
std::atomic<Frame> original{nullptr};
std::atomic<bool> enabled{false};
bool attempted = false;
Profile binding{};
Callback callback = nullptr;
std::array<unsigned char, 4096> expected{};
std::array<unsigned char, 5> patch{};
thread_local bool dispatching = false;

bool read(std::uintptr_t at, void *out, std::size_t size) {
    SIZE_T copied = 0;
    return at && at <= std::numeric_limits<std::uintptr_t>::max() - size &&
        ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(at), out, size, &copied) && copied == size;
}
template<class T> bool read(std::uintptr_t at, T &out) { return read(at, &out, sizeof(out)); }
bool inside(std::uint32_t rva, std::size_t size) {
    return rva < binding.image_size && size <= binding.image_size - rva;
}
bool body(bool patched) {
    std::array<unsigned char, 4096> value{};
    const auto size = binding.original_body.size();
    if (!read(binding.image_base + binding.frame_rva, value.data(), size)) return false;
    return patched ? !std::memcmp(value.data(), patch.data(), patch.size()) &&
        !std::memcmp(value.data() + patch.size(), expected.data() + patch.size(), size - patch.size()) :
        !std::memcmp(value.data(), expected.data(), size);
}
bool context(void *receiver, double &delta) {
    std::uintptr_t root = 0, table = 0; DWORD owner = 0; float seconds = 0;
    const auto address = reinterpret_cast<std::uintptr_t>(receiver);
    if (!address || address > std::numeric_limits<std::uintptr_t>::max() - binding.delta_offset - sizeof(seconds) ||
        !read(binding.image_base + binding.root_rva, root) || root != address ||
        !read(address, table) || table != binding.image_base + binding.table_rva ||
        !read(binding.image_base + binding.owner_thread_rva, owner) || !owner || owner != GetCurrentThreadId() ||
        !read(address + binding.delta_offset, seconds) || !std::isfinite(seconds) || seconds < 0 || !body(true)) return false;
    delta = seconds;
    return true;
}
std::uintptr_t __fastcall frame(void *receiver) {
    // Native execution retains its original receiver/result on every refusal.
    const auto result = original.load(std::memory_order_acquire)(receiver);
    if (!enabled.load(std::memory_order_acquire) || dispatching) return result;
    double delta = 0;
    if (!context(receiver, delta)) return result;
    dispatching = true;
    try { callback(delta, result); } catch (...) { /* Never unwind into the engine. */ }
    dispatching = false;
    return result;
}
}
bool install(const Profile &profile, Callback handler) {
    std::lock_guard lock(mutex);
    if (attempted) return false;
    attempted = true;
    binding = profile;
    const auto size = profile.original_body.size();
    if (!handler || !profile.image_base || !profile.image_size || profile.image_size > 128 * 1024 * 1024 ||
        profile.image_base > std::numeric_limits<std::uintptr_t>::max() - profile.image_size ||
        size < 16 || size > expected.size() || profile.root_rva % 8 || profile.table_rva % 8 ||
        profile.owner_thread_rva % 4 || profile.delta_offset > 0x100000 ||
        !inside(profile.frame_rva, size) || !inside(profile.root_rva, 8) ||
        !inside(profile.table_rva, 8) || !inside(profile.owner_thread_rva, 4)) return false;
    std::memcpy(expected.data(), profile.original_body.data(), size);
    // Own the complete body; the caller's evidence buffer need not outlive install.
    binding.original_body = {expected.data(), size};
    if (!body(false)) return false;
    MEMORY_BASIC_INFORMATION region{};
    const auto target = profile.image_base + profile.frame_rva;
    if (!VirtualQuery(reinterpret_cast<void *>(target), &region, sizeof(region)) || region.State != MEM_COMMIT ||
        region.Protect & (PAGE_GUARD | PAGE_NOACCESS) || !(region.Protect &
        (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    const auto initialized = MH_Initialize();
    if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) return false;
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&frame), &pinned)) return false;
    void *trampoline = nullptr;
    if (MH_CreateHook(reinterpret_cast<void *>(target), reinterpret_cast<void *>(&frame), &trampoline) != MH_OK) return false;
    original.store(reinterpret_cast<Frame>(trampoline), std::memory_order_release);
    callback = handler;
    if (!body(false)) return false;
    if (MH_EnableHook(reinterpret_cast<void *>(target)) != MH_OK) return false;
    if (!read(target, patch) || patch[0] != 0xe9 || !body(true)) {
        // A late external mutation must not be overwritten during rollback.
        // Dispatch remains disabled, and the pinned trampoline preserves any
        // calls which still enter our pass-through bridge.
        return false;
    }
    enabled.store(true, std::memory_order_release);
    return true;
}
bool stop() {
    std::lock_guard lock(mutex);
    if (!enabled.exchange(false, std::memory_order_acq_rel)) return false;
    // A foreign patch is never overwritten. The pinned trampoline stays valid.
    if (!body(true)) return false;
    const auto target = binding.image_base + binding.frame_rva;
    if (MH_DisableHook(reinterpret_cast<void *>(target)) != MH_OK) return false;
    return body(false);
}
bool active() { return enabled.load(std::memory_order_acquire); }
}
