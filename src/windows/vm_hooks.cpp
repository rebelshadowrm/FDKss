#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "vm_hooks.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace {
using Dispatch = void (__fastcall *)(void *, std::uint32_t, std::uint32_t);
Dispatch original = nullptr;
std::array<void **, 2> slots{};
std::size_t slot_count = 0;
fdkss_runtime *runtime = nullptr;
DWORD owner = 0;
std::atomic<bool> enabled{false};
bool installed_once = false;
thread_local bool nested = false;

bool read(std::uintptr_t address, void *value, std::size_t size) {
    SIZE_T count = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(address), value, size, &count) && count == size;
}
template<typename T> T value(std::uintptr_t address) { T result{}; read(address, &result, sizeof(result)); return result; }
std::string text(std::uintptr_t address, std::size_t bound) {
    std::string result(bound, '\0');
    if (!read(address, result.data(), bound)) return {};
    const auto end = result.find('\0');
    if (end == std::string::npos) return {};
    result.resize(end); return result;
}
bool swap(void **slot, void *from, void *to) {
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protection)) return false;
    const auto previous = InterlockedCompareExchangePointer(slot, to, from);
    DWORD ignored = 0;
    const bool restored = VirtualProtect(slot, sizeof(void *), protection, &ignored) != FALSE;
    return previous == from && restored;
}
void number(fdkss_value &value, std::int64_t integer) { value.type = FDKSS_INTEGER; value.integer = integer; }
void string(fdkss_value &value, const std::string &text) { value.type = FDKSS_STRING; value.string = text.data(); value.string_size = text.size(); }
void filter_arguments(std::uintptr_t thread, std::uint32_t group, std::uint32_t command,
    const std::string &program, const std::string &function, std::string &arguments) {
    const auto count = value<std::uint32_t>(thread + 0x70);
    const auto used = value<std::int32_t>(thread + 0x64), capacity = value<std::int32_t>(thread + 0x60);
    const auto stack = value<std::uintptr_t>(thread + 0x58);
    if (!count || count > 32 || used < 0 || used > capacity || capacity > 1024 * 1024 ||
        count * 4 > static_cast<std::uint32_t>(used) || arguments.size() != count * 4 || !stack ||
        stack > std::numeric_limits<std::uintptr_t>::max() - static_cast<std::size_t>(used)) return;
    const auto address = stack + used - count * 4;
    auto changed = arguments;
    for (std::uint32_t index = 0; index < count; ++index) {
        // FDK's first logical argument is the final DWORD on the argument stack.
        const auto offset = (count - index - 1) * 4;
        std::uint32_t raw = 0; std::memcpy(&raw, arguments.data() + offset, 4);
        fdkss_value fields[6]{};
        number(fields[0], group); number(fields[1], command); string(fields[2], program); string(fields[3], function); number(fields[4], index);
        if ((raw >> 30) == 1) number(fields[5], static_cast<std::int32_t>(raw << 2) >> 2);
        else if ((raw >> 30) == 2 && std::isfinite(std::bit_cast<float>(raw << 2))) {
            fields[5].type = FDKSS_NUMBER; fields[5].number = std::bit_cast<float>(raw << 2);
        } else continue; // Strings, nil and reference-like values are never rewritten.
        const double bound = std::numeric_limits<float>::max();
        if (fdkss_filter_values(runtime, "hook.script.command.argument.pre", fields, 6, 5,
            fields[5].type == FDKSS_INTEGER ? -536870912.0 : -bound,
            fields[5].type == FDKSS_INTEGER ? 536870911.0 : bound) != FDKSS_OK) continue;
        const auto encoded = fields[5].type == FDKSS_INTEGER ?
            (static_cast<std::uint32_t>(fields[5].integer) & 0x3fffffff) | 0x40000000 :
            (std::bit_cast<std::uint32_t>(static_cast<float>(fields[5].number)) >> 2) | 0x80000000;
        // Returning nil must preserve even noncanonical float encodings exactly.
        const bool differs = fields[5].type == FDKSS_INTEGER ?
            fields[5].integer != (static_cast<std::int32_t>(raw << 2) >> 2) :
            std::bit_cast<std::uint64_t>(fields[5].number) != std::bit_cast<std::uint64_t>(static_cast<double>(std::bit_cast<float>(raw << 2)));
        if (differs) std::memcpy(changed.data() + offset, &encoded, 4);
    }
    if (changed == arguments || value<std::uint32_t>(thread + 0x70) != count ||
        value<std::int32_t>(thread + 0x64) != used || value<std::int32_t>(thread + 0x60) != capacity ||
        value<std::uintptr_t>(thread + 0x58) != stack) return;
    std::string current(arguments.size(), '\0');
    if (!read(address, current.data(), current.size()) || current != arguments) return;
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQuery(reinterpret_cast<void *>(address), &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || (region.Protect & 0xff) != PAGE_READWRITE ||
        region.RegionSize < changed.size() || address - reinterpret_cast<std::uintptr_t>(region.BaseAddress) > region.RegionSize - changed.size()) return;
    SIZE_T written = 0;
    if (WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(address), changed.data(), changed.size(), &written) && written == changed.size())
        arguments = std::move(changed);
}

void __fastcall dispatch(void *object, std::uint32_t group, std::uint32_t command) {
    if (!enabled.load(std::memory_order_acquire) || GetCurrentThreadId() != owner || nested) {
        original(object, group, command); return;
    }
    nested = true;
    const auto thread = reinterpret_cast<std::uintptr_t>(object);
    // Arguments and strings are copied before invoking Lua. Nothing borrowed survives this call.
    std::string program, function, arguments;
    fdkss_value values[6]{};
    try {
        const auto descriptor = value<std::uintptr_t>(thread + 8);
        program = text(descriptor + 12, 32);
        function = text(value<std::uintptr_t>(thread + 0x88), 256);
        const auto count = value<std::uint32_t>(thread + 0x70);
        const auto used = value<std::int32_t>(thread + 0x64);
        const auto capacity = value<std::int32_t>(thread + 0x60);
        if (count <= 32 && used >= 0 && used <= capacity && count * 4 <= static_cast<std::uint32_t>(used)) {
            arguments.resize(count * 4);
            if (!read(value<std::uintptr_t>(thread + 0x58) + used - count * 4, arguments.data(), arguments.size())) arguments.clear();
        }
        number(values[0], group); number(values[1], command);
        string(values[2], program); string(values[3], function); string(values[4], arguments);
        fdkss_emit_values(runtime, "hook.script.command.pre", values, 5);
        filter_arguments(thread, group, command, program, function, arguments);
        string(values[4], arguments);
    } catch (...) {} // A diagnostic allocation failure must not skip the engine command.
    original(object, group, command);
    try {
        const auto raw = value<std::uint32_t>(thread + 0x14);
        const auto tag = raw >> 30;
        std::string result;
        if (tag == 1) number(values[5], static_cast<std::int32_t>(raw << 2) >> 2);
        else if (tag == 2) {
            const auto decoded = std::bit_cast<float>(raw << 2);
            if (std::isfinite(decoded)) { values[5].type = FDKSS_NUMBER; values[5].number = decoded; }
        } else if (tag == 3) {
            const auto descriptor = value<std::uintptr_t>(thread + 8);
            const auto offset = raw & 0x3fffffff;
            const auto bytes = value<std::uint32_t>(descriptor + 8);
            if (offset && offset < bytes) {
                result = text(value<std::uintptr_t>(descriptor) + offset, std::min<std::size_t>(16384, bytes - offset));
                string(values[5], result);
            }
        }
        if (values[5].type == FDKSS_INTEGER || values[5].type == FDKSS_NUMBER) {
            const auto before = values[5];
            const double bound = std::numeric_limits<float>::max();
            const auto status = fdkss_filter_values(runtime, "hook.script.command.post", values, 6, 5,
                values[5].type == FDKSS_INTEGER ? -536870912.0 : -bound,
                values[5].type == FDKSS_INTEGER ? 536870911.0 : bound);
            if (status == FDKSS_OK && (values[5].integer != before.integer ||
                std::bit_cast<std::uint64_t>(values[5].number) != std::bit_cast<std::uint64_t>(before.number))) {
                const std::uint32_t replacement = values[5].type == FDKSS_INTEGER ?
                    (static_cast<std::uint32_t>(values[5].integer) & 0x3fffffff) | 0x40000000 :
                    (std::bit_cast<std::uint32_t>(static_cast<float>(values[5].number)) >> 2) | 0x80000000;
                std::memcpy(reinterpret_cast<void *>(thread + 0x14), &replacement, sizeof(replacement));
            }
        } else fdkss_emit_values(runtime, "hook.script.command.post", values, 6);
    } catch (...) {}
    nested = false;
}
}
namespace vm_hooks {
bool start(void ***targets, std::size_t count, void *expected, fdkss_runtime *target_runtime, unsigned long thread) {
    if (installed_once || !count || count > slots.size() || !expected || !target_runtime || !thread) return false;
    for (std::size_t i = 0; i < count; ++i) if (!targets[i] || *targets[i] != expected) return false;
    original = reinterpret_cast<Dispatch>(expected); runtime = target_runtime; owner = thread;
    slot_count = count; installed_once = true;
    for (std::size_t i = 0; i < count; ++i) slots[i] = targets[i];
    for (std::size_t i = 0; i < count; ++i) if (!swap(slots[i], expected, reinterpret_cast<void *>(dispatch))) { stop(); return false; }
    enabled.store(true, std::memory_order_release);
    return true;
}
bool stop() {
    enabled.store(false, std::memory_order_release);
    bool success = true;
    for (std::size_t i = 0; i < slot_count; ++i)
        if (*slots[i] == reinterpret_cast<void *>(dispatch) && !swap(slots[i], reinterpret_cast<void *>(dispatch), reinterpret_cast<void *>(original))) success = false;
    return success;
}
}
