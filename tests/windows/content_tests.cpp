#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "content.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
namespace fs = std::filesystem;
using namespace content_overlay;
namespace {
struct Seen { void *stream; std::string path; std::uint32_t mode, flags; std::uint16_t extra; } seen{};
__declspec(noinline) int __fastcall engine_open(void *stream, const char *path,
    std::uint32_t mode, std::uint32_t flags, std::uint16_t extra) {
    seen = {stream, path, mode, flags, extra};
    return static_cast<int>(mode ^ flags ^ extra);
}
void require(bool condition) { if (!condition) throw std::runtime_error("content assertion failed"); }
}
int main() {
    const auto root = fs::temp_directory_path() / ("fdkss-content-" + std::to_string(GetCurrentProcessId()));
    try {
        auto first = root / "A" / "Assets" / "script" / "scena";
        auto second = root / "Z" / "Assets" / "script" / "scena";
        fs::create_directories(first); fs::create_directories(second);
        std::ofstream(root / "A" / "enabled.txt") << '1'; std::ofstream(root / "Z" / "enabled.txt") << '1';
        std::ofstream(first / "map.dat") << "first"; std::ofstream(second / "map.dat") << "second";
        require(start(reinterpret_cast<void *>(&engine_open), root));
        Open volatile call = &engine_open;
        void *stream = reinterpret_cast<void *>(0x12345678);
        require(call(stream, "SCRIPT\\SCENA\\MAP.DAT", 0, 3, 0xabcd) == (0xabcd ^ 4));
        require(seen.stream == stream && seen.extra == 0xabcd && seen.mode == 0 && seen.flags == 4 &&
            fs::path(seen.path) == fs::canonical(second / "map.dat"));
        // Writes and explicit absolute-path requests retain every original argument.
        require(call(stream, "script/scena/map.dat", 1, 3, 99) == (1 ^ 3 ^ 99));
        require(seen.path == "script/scena/map.dat" && seen.flags == 3 && seen.mode == 1);
        call(stream, "script/scena/map.dat", 0, 4, 77); require(seen.flags == 4 && seen.path == "script/scena/map.dat");
        call(stream, "../script/scena/map.dat", 0, 3, 11); require(seen.path == "../script/scena/map.dat");
        std::thread worker([&] { call(stream, "script/scena/map.dat", 2, 0x80, 0xfedc); }); worker.join();
        require(seen.mode == 2 && seen.flags == 0x84 && seen.extra == 0xfedc);
        auto state = status(); require(state.enabled && state.files == 1 && state.redirects == 2 && state.opens == 5);
        auto requests = drain(); require(requests.size() == 5 && requests[0].redirected && !requests[1].redirected);
        // Failed indexing retains the previous immutable overlay map.
        std::ofstream(root / "Z" / "Assets" / fs::path(L"unsupported-\u00e9.dat")) << 'x';
        require(!refresh(root) && !error().empty() && status().files == 1);
        fs::remove(root / "Z" / "Assets" / fs::path(L"unsupported-\u00e9.dat"));
        fs::remove(root / "Z" / "enabled.txt"); require(refresh(root));
        call(stream, "script/scena/map.dat", 0, 3, 99); require(fs::path(seen.path) == fs::canonical(first / "map.dat"));
        require(stop() && !status().enabled);
        require(call(stream, "script/scena/map.dat", 0, 3, 99) == (3 ^ 99) && seen.path == "script/scena/map.dat");
        // Only this process's explicitly created temporary fixture directory is removed.
        fs::remove_all(root);
        std::cout << "Inline trampoline, five-argument ABI, overlays, refresh and restoration passed\n";
        return 0;
    } catch (const std::exception &exception) {
        stop(); fs::remove_all(root); std::cerr << exception.what() << '\n'; return 1;
    }
}
