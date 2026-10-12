#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <cstdint>
#include <atomic>
#include <chrono>
extern "C" __declspec(dllimport) void __cdecl SteamAPI_RunCallbacks(void);
extern "C" __declspec(dllimport) unsigned __cdecl TestPumpCount(void);
static unsigned engine_calls = 0;
static float expected_original_delta = 0.25f;
static std::uintptr_t __fastcall fixture_update(void *application, float delta) {
    if (application != reinterpret_cast<void *>(0x1234) || delta != expected_original_delta) return 0;
    ++engine_calls;
    return 0x123456789abcdef0ull;
}
using Update = std::uintptr_t (__fastcall *)(void *, float);
static Update update_slot = fixture_update;
extern "C" __declspec(dllexport) void **__cdecl TestUpdateSlot(void) {
    return reinterpret_cast<void **>(&update_slot);
}
int wmain(int argc, wchar_t **argv) {
    if (argc != 2 && argc != 3) return 2;
    const bool mutate = argc == 3 && std::wstring(argv[2]) == L"--engine-mutate";
    const bool engine = mutate || (argc == 3 && std::wstring(argv[2]) == L"--engine");
    const bool console = argc == 3 && std::wstring(argv[2]) == L"--console";
    const auto dll = std::filesystem::path(argv[1]);
    const auto root = dll.parent_path();
    const auto scripts = root / "Mods" / "AdapterTest" / "Scripts";
    std::filesystem::create_directories(scripts);
    std::ofstream(root / "Mods" / "AdapterTest" / "enabled.txt") << "1";
    std::ofstream script(scripts / "main.lua");
    script << R"(
        fdk.on('start', function()
            assert(fdk.call('game.name') == 'FDKss adapter test host')
            assert(#fdk.call('game.build_sha256') == 64)
            fdk.log('fixture-start')
        end)
        fdk.on('update', function(dt)
            assert(dt >= 0)
            fdk.log('fixture-update')
        end)
        fdk.on('stop', function() fdk.log('fixture-stop') end)
    )";
    if (engine) {
        script << "local mutate = " << (mutate ? "true" : "false") << '\n';
        script << R"(
        fdk.hook('application.update', 'pre', function(app, dt)
            assert(tostring(app) == 'FDK root: application' and dt == 0.25)
            fdk.log('fixture-pre')
        end)
        fdk.hook('application.update', 'post', function(app, dt, result)
            assert((dt == 0.25 or (mutate and dt == 0.125)) and result == 0x123456789abcdef0)
            fdk.log('fixture-post')
        end)
        local removed = fdk.hook('application.update', 'pre', function() error('removed hook ran') end)
        assert(fdk.unhook(removed) and not fdk.unhook(removed))
        fdk.on('update', function(dt)
            assert(dt == 0.25 or (mutate and dt == 0.125))
            assert(fdk.call('game.update_info'):find('fdk.application.update', 1, true))
        end)
        if mutate then
            fdk.hook('application.update', 'pre', function(app, dt) return dt / 2 end)
            fdk.hook('application.update', 'post', function(app, dt, result) return 42 end)
        end
    )";
    }
    script.close();
    // A dedicated writable test directory; removing only this fixture's log.
    std::error_code ignored;
    std::filesystem::remove(root / "FDKss.log", ignored);
    SteamAPI_RunCallbacks();
    HMODULE module = LoadLibraryW(dll.c_str());
    if (!module) { std::cerr << "Test adapter did not load\n"; return 1; }
    const auto attach = reinterpret_cast<DWORD (WINAPI *)(void *)>(GetProcAddress(module, "FDKssAttach"));
    if (!attach || attach(nullptr) != ERROR_SUCCESS || attach(nullptr) != ERROR_ALREADY_EXISTS) {
        std::cerr << "Test adapter did not attach\n"; return 1;
    }
    const DWORD fixture_owner = GetCurrentThreadId();
    auto invoke_update = [&] {
        const bool hooked_owner = update_slot != fixture_update && GetCurrentThreadId() == fixture_owner;
        expected_original_delta = mutate && hooked_owner && engine_calls != 0 ? 0.125f : 0.25f;
        const auto result = update_slot(reinterpret_cast<void *>(0x1234), 0.25f);
        const auto expected_result = mutate && hooked_owner ? 42ull : 0x123456789abcdef0ull;
        if (result != expected_result) throw std::runtime_error("Engine arguments/result corrupted");
    };
    if (engine) { invoke_update(); invoke_update(); }
    SteamAPI_RunCallbacks(); SteamAPI_RunCallbacks();
    // The original pump must still execute on other threads, without entering Lua there.
    std::thread other([] { SteamAPI_RunCallbacks(); }); other.join();
    if (engine) {
        std::thread other_update(invoke_update); other_update.join();
        if (engine_calls != 3) { std::cerr << "Original engine update lost calls\n"; return 1; }
    }
    if (TestPumpCount() != 4) { std::cerr << "Original pump lost calls\n"; return 1; }
    size_t expected_updates = 3;
    std::atomic<bool> client_done{false};
    std::string console_error;
    std::thread client;
    if (console) {
        client = std::thread([&] {
            const auto name = L"\\\\.\\pipe\\FDKss-" + std::to_wstring(GetCurrentProcessId());
            for (const auto &query : {std::string("AdapterTest\nreturn 6 * 7"), std::string("AdapterTest\nerror('pipe-test-error')")}) {
                if (!WaitNamedPipeW(name.c_str(), 1000)) { console_error = "Console pipe unavailable"; break; }
                HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (pipe == INVALID_HANDLE_VALUE) { console_error = "Console pipe did not open"; break; }
                DWORD mode = PIPE_READMODE_MESSAGE, sent = 0, received = 0;
                char response[16384]{};
                bool ok = SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr) &&
                    WriteFile(pipe, query.data(), static_cast<DWORD>(query.size()), &sent, nullptr) && sent == query.size();
                // Delay the reader: disconnect must not discard a buffered response.
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                ok = ok && ReadFile(pipe, response, sizeof(response), &received, nullptr);
                CloseHandle(pipe);
                const std::string text(response, received);
                if (!ok || (query.find("6 * 7") != query.npos ? text != "OK\n42" :
                    !text.starts_with("ERROR\n") || text.find("pipe-test-error") == text.npos)) {
                    console_error = "Wrong console response: " + text; break;
                }
            }
            client_done.store(true);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!client_done.load() && std::chrono::steady_clock::now() < deadline) {
            SteamAPI_RunCallbacks(); ++expected_updates;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    const auto detach = reinterpret_cast<DWORD (WINAPI *)(void *)>(GetProcAddress(module, "FDKssRequestDetach"));
    if (!detach || detach(nullptr) != ERROR_SUCCESS) { std::cerr << "Detach request failed\n"; return 1; }
    if (engine) invoke_update(); else SteamAPI_RunCallbacks();
    if (client.joinable()) client.join();
    if (console && (!client_done.load() || !console_error.empty())) { std::cerr << console_error; return 1; }
    // Once detached, originals still run but no Lua callback should fire.
    if (engine) invoke_update(); else SteamAPI_RunCallbacks();
    std::ifstream log(root / "FDKss.log");
    const std::string contents{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
    const auto start = contents.find("fixture-start");
    size_t updates = 0, cursor = 0;
    while ((cursor = contents.find("fixture-update", cursor)) != contents.npos) { ++updates; ++cursor; }
    if (engine) {
        size_t pres = 0, posts = 0;
        cursor = 0; while ((cursor = contents.find("fixture-pre", cursor)) != contents.npos) { ++pres; ++cursor; }
        cursor = 0; while ((cursor = contents.find("fixture-post", cursor)) != contents.npos) { ++posts; ++cursor; }
        if (pres != 2 || posts != 3 || contents.find("fixture-post") > contents.find("fixture-pre")) {
            std::cerr << "Wrong hook phase counts/order:\n" << contents; return 1;
        }
    }
    if (contents.find("stack traceback") != std::string::npos ||
        start == std::string::npos || updates != expected_updates ||
        contents.find("fixture-stop") == std::string::npos ||
        contents.find("Hook detached") == std::string::npos) {
        std::cerr << "Wrong adapter lifecycle/update count:\n" << contents; return 1;
    }
    std::cout << "Windows adapter fixture passed\n";
    return 0;
}
