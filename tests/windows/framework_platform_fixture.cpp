#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "framework_bridge.h"
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
struct Framework { std::uintptr_t table; std::array<unsigned char, 0x34> padding{}; float delta = .25f; };
static_assert(offsetof(Framework, delta) == 0x3c);
std::uintptr_t table[1]{};
Framework object{};
Framework *root = &object;
DWORD owner = 0;
std::atomic<unsigned> native_calls{0};
std::atomic<std::uintptr_t> native_result{0};
__declspec(noinline) std::uintptr_t __fastcall native_frame(void *receiver) {
    ++native_calls;
    const auto value = reinterpret_cast<std::uintptr_t>(receiver) ^ GetTickCount64();
    native_result = value;
    return value;
}
void check(bool value) { if (!value) throw std::runtime_error("framework platform fixture assertion failed"); }
std::size_t occurrences(const std::string &text, const char *needle) {
    std::size_t count = 0, at = 0;
    while ((at = text.find(needle, at)) != text.npos) { ++count; ++at; }
    return count;
}
}
extern "C" __declspec(dllexport) const framework_bridge::Profile *__cdecl TestFrameworkProfile() {
    static std::vector<unsigned char> body;
    static framework_bridge::Profile profile{};
    if (body.empty()) {
        const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        const auto *pe = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        const auto target = reinterpret_cast<std::uintptr_t>(&native_frame);
        DWORD64 function_base = 0;
        const auto function = RtlLookupFunctionEntry(target, &function_base, nullptr);
        check(function && function_base == base && function->BeginAddress == target - base);
        body.assign(reinterpret_cast<unsigned char *>(target), reinterpret_cast<unsigned char *>(base + function->EndAddress));
        const auto rva = [&](auto pointer) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(pointer) - base); };
        profile = {base, pe->OptionalHeader.SizeOfImage, rva(&native_frame), rva(&root), rva(table), rva(&owner), 0x3c, body};
    }
    return &profile;
}
int wmain(int argc, wchar_t **argv) {
    // Isolated injector refusal host: no DLL or game memory is ever loaded.
    if (argc == 2 && std::wcscmp(argv[1], L"--hold") == 0) { Sleep(30000); return 0; }
    try {
        check(argc == 2);
        owner = GetCurrentThreadId(); object.table = reinterpret_cast<std::uintptr_t>(table);
        const std::filesystem::path dll(argv[1]);
        const auto directory = dll.parent_path();
        const auto scripts = directory / "Mods" / "CoreProbe" / "Scripts";
        std::filesystem::create_directories(scripts);
        std::ofstream(directory / "FDKss.log", std::ios::trunc);
        std::ofstream(directory / "Mods" / "CoreProbe" / "enabled.txt") << "1";
        std::ofstream(scripts / "main.lua") << R"(
            fdk.on('start', function()
                assert(fdk.call('game.name') == 'FDKss framework platform test host')
                assert(#fdk.call('game.build_sha256') == 64)
                assert(fdk.call('game.update_info'):find('fdk.framework.frame', 1, true))
                local caps = fdk.call('game.capabilities')
                assert(caps:find('"actor_api":false', 1, true))
                assert(caps:find('"engine_properties":false', 1, true))
                assert(caps:find('"vm_api":false', 1, true))
                assert(not pcall(fdk.call, 'engine.actor_constructor_info'))
                assert(not pcall(fdk.call, 'game.browser_info'))
                assert(not pcall(fdk.call, 'game.overlay'))
                assert(not pcall(fdk.hook, 'application.update', 'pre', function() end))
                fdk.log('core-start')
            end)
            fdk.on('update', function(dt) assert(dt == .25); fdk.log('core-update') end)
            fdk.on('stop', function() fdk.log('core-stop') end)
        )";
        const auto module = LoadLibraryW(dll.c_str()); check(module != nullptr);
        using Command = DWORD (WINAPI *)(void *);
        const auto attach = reinterpret_cast<Command>(GetProcAddress(module, "FDKssAttach"));
        const auto reload = reinterpret_cast<Command>(GetProcAddress(module, "FDKssRequestReload"));
        const auto detach = reinterpret_cast<Command>(GetProcAddress(module, "FDKssRequestDetach"));
        check(attach && reload && detach && attach(nullptr) == ERROR_SUCCESS);
        const auto contract = TestFrameworkProfile();
        const auto call = [] { check(native_frame(&object) == native_result.load()); };
        call(); call();
        std::thread foreign(call); foreign.join();
        check(reload(nullptr) == ERROR_SUCCESS); call(); call();
        check(detach(nullptr) == ERROR_SUCCESS); call(); call();
        check(native_calls == 7);
        check(!std::memcmp(reinterpret_cast<void *>(contract->image_base + contract->frame_rva),
                           contract->original_body.data(), contract->original_body.size()));
        std::ifstream log(directory / "FDKss.log");
        const std::string contents{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
        if (occurrences(contents, "core-start") != 2 || occurrences(contents, "core-stop") != 2 ||
            occurrences(contents, "core-update") != 5 || contents.find("stack traceback") != contents.npos ||
            contents.find("complete original body verified") == contents.npos) {
            std::cerr << contents; return 1;
        }
        std::cout << "Framework core platform and extension isolation passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
