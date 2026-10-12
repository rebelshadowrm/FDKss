#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "vm_hooks.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <cstring>
#include <bit>
using Dispatch = void (__fastcall *)(void *, std::uint32_t, std::uint32_t);
std::uint32_t raw_result = 0x40000007;
unsigned original_calls = 0;
void *fixture_thread = nullptr;
int FDKSS_CALL invalidate(void *, const char *, char *out, std::size_t size) {
    const std::uint32_t zero = 0;
    std::memcpy(static_cast<unsigned char *>(fixture_thread) + 0x64, &zero, 4);
    if (size) *out = 0; return 0;
}
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void __fastcall original(void *thread, std::uint32_t group, std::uint32_t id) {
    require(group == 3 && id == 25, "VM dispatch arguments changed");
    ++original_calls;
    std::memcpy(static_cast<unsigned char *>(thread) + 0x14, &raw_result, 4);
}
int main() {
    const auto root = std::filesystem::temp_directory_path() / ("fdkss-vm-hooks-" + std::to_string(GetCurrentProcessId()));
    fdkss_runtime *runtime = nullptr;
    try {
        std::filesystem::create_directories(root / "VM" / "Scripts");
        std::ofstream(root / "VM" / "enabled.txt") << '1';
        std::ofstream(root / "VM" / "Scripts" / "main.lua") << R"(
            pres, posts = 0, 0
            legacy_pre = fdk.on('hook.script.command.pre', function(group, id, script, fn, arguments)
                assert(group == 3 and id == 25 and script == 'scena/test' and fn == 'Test')
                assert(string.unpack('<I4', arguments) == 0x40000003)
                pres = pres + 1
            end)
            argmode, arg_seen, invalid_seen = 0, 0, 0
            assert(not pcall(fdk.hook, 'script.command.argument', 'post', function() end))
            fdk.hook('script.command.argument', 'pre', function(group, id, script, fn, index, value)
                assert(group == 3 and id == 25 and script == 'scena/test' and fn == 'Test')
                arg_seen = arg_seen + 1
                if argmode == 1 then return value + 1 end
                if argmode == 2 then return value + 0.25 end
                if argmode == 3 then return value + index + 1 end
                if argmode == 4 then fdk.call('fixture.invalidate'); return value + 1 end
            end)
            fdk.on('hook.script.command.argument.pre', function(group, id, script, fn, index, value)
                if argmode == 9 then invalid_seen = invalid_seen + 1; return 536870912 end
            end)
            fdk.on('hook.script.command.post', function(group, id, script, fn, arguments, result)
                assert(result == 7)
                posts = posts + 1
                if posts == 1 then return result + 1 end
                return 536870912 -- out of signed 30-bit range; callback must be disabled
            end)
            fdk.on('hook.script.command.post', function(group, id, script, fn, arguments, result)
                if type(result) == 'number' and math.type(result) == 'float' then return result + 0.5 end
                if type(result) == 'string' then assert(result == 'text'); return 'ignored' end
            end)
        )";
        const auto path = root.u8string();
        require(fdkss_create(reinterpret_cast<const char *>(path.c_str()), nullptr, nullptr, &runtime) == FDKSS_OK, "runtime create failed");
        require(fdkss_register_native(runtime, "fixture.invalidate", invalidate, nullptr) == FDKSS_OK, "fixture native registration failed");
        require(fdkss_register_typed_native(runtime, "engine.hook_supported",
            [](void *, const fdkss_value *, std::size_t, fdkss_result_fn emit, void *sink, char *, std::size_t) -> int {
                fdkss_value result{}; result.type = FDKSS_BOOLEAN; result.boolean = 1; return emit(sink, &result);
            }, nullptr) == FDKSS_OK, "hook fixture registration failed");
        require(fdkss_start(runtime) == FDKSS_OK, "runtime start failed");
        std::array<unsigned char, 0x100> thread{};
        fixture_thread = thread.data();
        std::array<unsigned char, 88> descriptor{};
        std::array<char, 256> function{}; std::memcpy(function.data(), "Test", 5);
        std::memcpy(descriptor.data() + 12, "scena/test", 11);
        std::uint32_t argument = 0x40000003;
        auto pointer = [&](std::size_t offset, void *p) { std::memcpy(thread.data() + offset, &p, 8); };
        auto integer = [&](std::size_t offset, std::uint32_t n) { std::memcpy(thread.data() + offset, &n, 4); };
        pointer(8, descriptor.data()); pointer(0x88, function.data()); pointer(0x58, &argument);
        integer(0x60, 4); integer(0x64, 4); integer(0x70, 1);
        void *first = reinterpret_cast<void *>(original), *second = first;
        void **targets[]{&first, &second};
        require(vm_hooks::start(targets, 2, first, runtime, GetCurrentThreadId()), "VM hooks failed to install");
        const auto saved = reinterpret_cast<Dispatch>(first);
        auto invoke = [&] { saved(thread.data(), 3, 25); return *reinterpret_cast<std::uint32_t *>(thread.data() + 0x14); };
        require(invoke() == 0x40000008, "VM result replacement failed");
        require(invoke() == 0x40000007 && invoke() == 0x40000007, "invalid VM result replacement was accepted");
        std::thread other([&] { require(invoke() == 0x40000007, "foreign thread ran VM hooks"); }); other.join();
        char result[64]{};
        require(fdkss_execute(runtime, "VM", "return pres, posts", result, sizeof(result)) == FDKSS_OK && std::string(result) == "3\t2", "VM events were not isolated");
        raw_result = (std::bit_cast<std::uint32_t>(1.5f) >> 2) | 0x80000000;
        require(invoke() == ((std::bit_cast<std::uint32_t>(2.0f) >> 2) | 0x80000000), "VM float replacement failed");
        std::array<char, 64> program{}; std::memcpy(program.data() + 32, "text", 5);
        void *program_pointer = program.data(); std::memcpy(descriptor.data(), &program_pointer, 8);
        const std::uint32_t program_size = 64; std::memcpy(descriptor.data() + 8, &program_size, 4);
        raw_result = 0xc0000020;
        require(invoke() == raw_result, "VM string observer changed a borrowed string result");
        raw_result = 0;
        require(invoke() == 0, "VM undefined result changed");
        require(fdkss_execute(runtime, "VM", "fdk.off(legacy_pre); argmode=1", result, sizeof(result)) == FDKSS_OK, "argument setup failed");
        raw_result = 0x40000007; argument = 0x40000003;
        auto previous_calls = original_calls;
        require(invoke() == raw_result && argument == 0x40000004 && original_calls == previous_calls + 1, "integer argument replacement or original dispatch count failed");
        std::array<std::uint32_t, 2> pair{0x40000003, 0x40000004};
        pointer(0x58, pair.data()); integer(0x60, 8); integer(0x64, 8); integer(0x70, 2);
        require(invoke() == raw_result && pair[0] == 0x40000004 && pair[1] == 0x40000005, "argument ordering changed");
        require(fdkss_execute(runtime, "VM", "argmode=3", result, sizeof(result)) == FDKSS_OK, "index mode setup failed");
        pair = {0x40000003, 0x40000004}; invoke();
        require(pair[0] == 0x40000005 && pair[1] == 0x40000005, "logical first-argument index was reversed");
        pointer(0x58, &argument); integer(0x60, 4); integer(0x64, 4); integer(0x70, 1);
        require(fdkss_execute(runtime, "VM", "argmode=2", result, sizeof(result)) == FDKSS_OK, "float mode setup failed");
        argument = (std::bit_cast<std::uint32_t>(1.5f) >> 2) | 0x80000000;
        invoke(); require(argument == ((std::bit_cast<std::uint32_t>(1.75f) >> 2) | 0x80000000), "float argument replacement failed");
        for (const std::uint32_t readonly : {0u, 1u, 0xc0000020u, 0x9fe00000u}) {
            argument = readonly; invoke(); require(argument == readonly, "unsupported argument tag was changed");
        }
        require(fdkss_execute(runtime, "VM", "argmode=9", result, sizeof(result)) == FDKSS_OK, "invalid mode setup failed");
        argument = 0x40000003; invoke(); invoke();
        require(argument == 0x40000003, "out-of-range argument replacement accepted");
        require(fdkss_execute(runtime, "VM", "return invalid_seen", result, sizeof(result)) == FDKSS_OK && std::string(result) == "1", "invalid argument filter was not isolated");
        require(fdkss_execute(runtime, "VM", "argmode=4", result, sizeof(result)) == FDKSS_OK, "stale stack mode setup failed");
        argument = 0x40000003; invoke(); require(argument == 0x40000003, "changed stack state was written through");
        integer(0x64, 4);
        require(fdkss_execute(runtime, "VM", "argmode=1", result, sizeof(result)) == FDKSS_OK, "readonly setup failed");
        auto *readonly_stack = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        require(readonly_stack != nullptr, "readonly fixture allocation failed");
        std::memcpy(readonly_stack, &argument, 4); DWORD old_protection = 0;
        require(VirtualProtect(readonly_stack, 4096, PAGE_READONLY, &old_protection), "readonly fixture protection failed");
        pointer(0x58, readonly_stack); invoke();
        require(*static_cast<std::uint32_t *>(readonly_stack) == 0x40000003, "readonly stack was changed");
        VirtualFree(readonly_stack, 0, MEM_RELEASE); pointer(0x58, &argument);
        std::thread foreign_arguments([&] { invoke(); }); foreign_arguments.join();
        require(argument == 0x40000003, "foreign thread filtered arguments");
        second = reinterpret_cast<void *>(static_cast<Dispatch>([](void *, std::uint32_t, std::uint32_t) {}));
        const auto replacement = second;
        require(vm_hooks::stop() && first == reinterpret_cast<void *>(original) && second == replacement, "VM slots were not conditionally restored");
        raw_result = 0x40000007;
        require(invoke() == raw_result, "cached VM thunk lost its original call target");
        fdkss_destroy(runtime); runtime = nullptr;
        std::filesystem::remove_all(root);
        std::cout << "VM hook tests passed\n"; return 0;
    } catch (const std::exception &e) {
        vm_hooks::stop(); if (runtime) fdkss_destroy(runtime);
        std::filesystem::remove_all(root);
        std::cerr << e.what() << '\n'; return 1;
    }
}
