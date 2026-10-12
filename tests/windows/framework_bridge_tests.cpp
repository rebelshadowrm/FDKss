#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "framework_bridge.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
void check(bool condition) { if (!condition) throw std::runtime_error("framework bridge assertion failed"); }
struct Framework {
    std::uintptr_t table;
    std::array<unsigned char, 0x34> padding{};
    float delta = .25f;
};
static_assert(offsetof(Framework, delta) == 0x3c);
std::uintptr_t table[1]{};
Framework object{};
Framework *root = &object;
DWORD owner = 0;
std::atomic<unsigned> native_calls{0}, callbacks{0};
double copied_delta = 0;
bool throwing = false, recursive = false, detach = false;
bool detached = false;
std::atomic<bool> block_foreign{false};
HANDLE entered = nullptr, resume = nullptr;
__declspec(noinline) std::uintptr_t __fastcall native_frame(void *receiver) {
    // Deliberately observable native result and receiver, retained on refusals.
    const auto count = native_calls.fetch_add(1);
    if (block_foreign.load() && GetCurrentThreadId() != owner) {
        SetEvent(entered);
        WaitForSingleObject(resume, INFINITE);
    }
    return reinterpret_cast<std::uintptr_t>(receiver) ^ (count + 0x1234);
}
void callback(double delta, std::uintptr_t result) {
    check(result != 0); copied_delta = delta; ++callbacks;
    if (throwing) throw std::runtime_error("mod error");
    if (recursive) native_frame(&object);
    if (detach) detached = framework_bridge::stop();
}
void write(std::uintptr_t at, const void *data, std::size_t size) {
    DWORD previous = 0, ignored = 0;
    check(VirtualProtect(reinterpret_cast<void *>(at), size, PAGE_EXECUTE_READWRITE, &previous) != 0);
    std::memcpy(reinterpret_cast<void *>(at), data, size);
    check(VirtualProtect(reinterpret_cast<void *>(at), size, previous, &ignored) != 0);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(at), size);
}
}
int main(int argc, char **argv) {
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "normal";
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        const auto *pe = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        const auto target = reinterpret_cast<std::uintptr_t>(&native_frame);
        const auto rva = [&](auto address) {
            const auto value = reinterpret_cast<std::uintptr_t>(address) - base;
            check(value < pe->OptionalHeader.SizeOfImage);
            return static_cast<std::uint32_t>(value);
        };
        DWORD64 function_base = 0;
        const auto function = RtlLookupFunctionEntry(target, &function_base, nullptr);
        check(function && function_base == base && function->BeginAddress == rva(&native_frame));
        const auto size = function->EndAddress - function->BeginAddress;
        check(size >= 16 && size <= 4096);
        std::vector<unsigned char> original(size);
        std::memcpy(original.data(), reinterpret_cast<void *>(target), original.size());
        auto expected = original;
        owner = GetCurrentThreadId(); object.table = reinterpret_cast<std::uintptr_t>(table);
        const framework_bridge::Profile profile{base, pe->OptionalHeader.SizeOfImage,
            rva(&native_frame), rva(&root), rva(table), rva(&owner), 0x3c, expected};
        if (mode == "changed-body") {
            expected[8] ^= 1;
            check(!framework_bridge::install(profile, callback));
            check(!framework_bridge::active() && !std::memcmp(reinterpret_cast<void *>(target), original.data(), original.size()));
            check(!framework_bridge::install(profile, callback));
            return 0;
        }
        check(framework_bridge::install(profile, callback));
        check(framework_bridge::active());
        std::fill(expected.begin(), expected.end(), 0); // The bridge owns its evidence buffer after install.
        if (mode == "foreign-prefix" || mode == "foreign-tail") {
            auto foreign = original;
            std::memcpy(foreign.data(), reinterpret_cast<void *>(target), foreign.size());
            foreign[mode == "foreign-prefix" ? 0 : 20] ^= 1;
            write(target, foreign.data(), foreign.size());
            check(!framework_bridge::stop() && !framework_bridge::active());
            check(!std::memcmp(reinterpret_cast<void *>(target), foreign.data(), foreign.size()));
            return 0; // Never execute intentionally foreign/corrupted fixture code.
        }
        const auto call = [&] {
            const auto count = native_calls.load();
            const auto value = native_frame(&object);
            check(value == (reinterpret_cast<std::uintptr_t>(&object) ^ (count + 0x1234)));
        };
        call(); check(callbacks == 1 && copied_delta == .25);
        const auto held = copied_delta;
        object.delta = .5f; check(copied_delta == held); call(); check(copied_delta == .5);
        const auto before = callbacks.load();
        std::thread other([&] { native_frame(&object); }); other.join(); check(callbacks == before);
        root = nullptr; call(); root = &object; check(callbacks == before);
        object.table = 0; call(); object.table = reinterpret_cast<std::uintptr_t>(table); check(callbacks == before);
        object.delta = -1; call(); object.delta = std::numeric_limits<float>::quiet_NaN(); call();
        object.delta = std::numeric_limits<float>::infinity(); call(); check(callbacks == before);
        object.delta = .75f; throwing = true; call(); throwing = false; check(callbacks == before + 1);
        recursive = true; const auto nested_before = native_calls.load(); call(); recursive = false;
        check(callbacks == before + 2 && native_calls == nested_before + 2);
        entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        resume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        check(entered && resume);
        block_foreign = true;
        bool worker_result = false;
        std::thread in_flight([&] {
            const auto count = native_calls.load();
            worker_result = native_frame(&object) == (reinterpret_cast<std::uintptr_t>(&object) ^ (count + 0x1234));
        });
        check(WaitForSingleObject(entered, 5000) == WAIT_OBJECT_0);
        detach = true; call(); check(detached && !framework_bridge::active());
        SetEvent(resume); in_flight.join(); check(worker_result);
        block_foreign = false; CloseHandle(entered); CloseHandle(resume);
        check(!std::memcmp(reinterpret_cast<void *>(target), original.data(), original.size()));
        const auto after = callbacks.load(); call(); check(callbacks == after);
        check(!framework_bridge::stop());
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%s\n", error.what()); return 1;
    }
}
