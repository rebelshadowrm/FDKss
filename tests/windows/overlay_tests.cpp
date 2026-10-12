#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "overlay.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::uintptr_t image = 0;
void check(bool value) { if (!value) throw std::runtime_error("renderer hook check failed"); }
template<class T> void store(std::uintptr_t address, T value) { std::memcpy(reinterpret_cast<void *>(address), &value, sizeof(value)); }
struct Output {
    std::vector<fdkss_value_type> types;
    std::vector<std::string> strings;
    std::int64_t id = 0;
};
int FDKSS_CALL collect(void *sink, const fdkss_value *value) {
    auto &output = *static_cast<Output *>(sink);
    output.types.push_back(value->type);
    if (value->type == FDKSS_STRING) output.strings.emplace_back(value->string, value->string_size);
    if (value->type == FDKSS_INTEGER) output.id = value->integer;
    return 0;
}
void browser_api() {
    fdkss_value root{}; root.type = FDKSS_STRING; root.string = "player"; root.string_size = 6;
    char error[256]{}; Output accepted;
    check(!developer_overlay::browser_request(nullptr, &root, 1, collect, &accepted, error, sizeof(error)));
    check(accepted.types == std::vector<fdkss_value_type>{FDKSS_BOOLEAN, FDKSS_INTEGER} && accepted.id > 0);
    fdkss_value id{}; id.type = FDKSS_INTEGER; id.integer = accepted.id;
    Output pending;
    check(!developer_overlay::browser_result(nullptr, &id, 1, collect, &pending, error, sizeof(error)));
    check(pending.strings == std::vector<std::string>{"pending", ""});
    fdkss_runtime *runtime = nullptr;
    check(fdkss_create(".", nullptr, nullptr, &runtime) == FDKSS_OK);
    developer_overlay::service(runtime); // Unbound adapter fails on the owner thread, not while queuing.
    Output failed;
    check(!developer_overlay::browser_result(nullptr, &id, 1, collect, &failed, error, sizeof(error)));
    check(failed.strings == std::vector<std::string>{"failed", "Engine browser unavailable"});
    id.integer = 0;
    check(developer_overlay::browser_result(nullptr, &id, 1, collect, &failed, error, sizeof(error)) != 0);
    Output next;
    check(!developer_overlay::browser_request(nullptr, &root, 1, collect, &next, error, sizeof(error)));
    check(next.id > accepted.id);
    check(developer_overlay::stop());
    id.integer = next.id; Output cancelled;
    check(!developer_overlay::browser_result(nullptr, &id, 1, collect, &cancelled, error, sizeof(error)));
    check(cancelled.strings.size() == 2 && cancelled.strings[0] == "cancelled");
    check(developer_overlay::browser_request(nullptr, &root, 1, collect, &next, error, sizeof(error)) != 0);
    check(fdkss_destroy(runtime) == FDKSS_OK);
}
}
std::uintptr_t render_test_base() { return image; }
int main(int argc, char **) {
    try {
        image = reinterpret_cast<std::uintptr_t>(VirtualAlloc(nullptr, 0xc61000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        check(image != 0);
        const bool sky2 = argc == 1;
        const std::uintptr_t global = sky2 ? 0xc60ea0 : 0x8ff010;
        const std::uintptr_t table = sky2 ? 0xb1e138 : 0x805d18;
        const std::uintptr_t target = sky2 ? 0x640700 : 0x50bbe0;
        char status[512]{};
        check(developer_overlay::command(nullptr, "show", status, sizeof(status)) != 0);
        check(!developer_overlay::start(sky2)); // Missing singleton leaves all slots alone.
        store(image + global, image + 0x3000); store(image + 0x3000, image + table);
        store(image + table + 28 * 8, image + target);
        std::array<unsigned char, 26> code{0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,
            0xff,0x05,0,0,0,0,0xb8,0x2a,0,0,0,0x48,0x83,0xc4,0x20,0x5b,0xc3};
        const auto displacement = static_cast<std::int32_t>(0x5000 - static_cast<std::int64_t>(target + 15));
        std::memcpy(code.data() + 11, &displacement, sizeof(displacement));
        std::memcpy(reinterpret_cast<void *>(image + target), code.data(), code.size());
        DWORD protection = 0;
        check(VirtualProtect(reinterpret_cast<void *>(image + target), code.size(), PAGE_EXECUTE_READ, &protection));
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(image + target), code.size());
        check(developer_overlay::start(sky2)); check(!developer_overlay::start(sky2));
        using Present = std::uint32_t (__fastcall *)(void *, bool);
        auto **slot = reinterpret_cast<void **>(image + table + 28 * 8);
        auto hooked = reinterpret_cast<Present>(*slot);
        check(hooked(reinterpret_cast<void *>(image + 0x3000), false) == 42);
        check(hooked(reinterpret_cast<void *>(image + 0x3000), true) == 42);
        check(*reinterpret_cast<std::uint32_t *>(image + 0x5000) == 2);
        check(!developer_overlay::command(nullptr, "", status, sizeof(status)) && std::strstr(status, "\"presents\":2"));
        check(developer_overlay::command(nullptr, "invalid", status, sizeof(status)) != 0);
        browser_api();
        check(developer_overlay::stop()); check(*slot == reinterpret_cast<void *>(image + target));
        check(developer_overlay::start(sky2));
        // Preserve another mod's replacement and keep previously loaded callbacks callable.
        store(reinterpret_cast<std::uintptr_t>(slot), image + 0x10000);
        check(developer_overlay::stop()); check(*slot == reinterpret_cast<void *>(image + 0x10000));
        check(hooked(reinterpret_cast<void *>(image + 0x3000), false) == 42);
        check(*reinterpret_cast<std::uint32_t *>(image + 0x5000) == 3);
        VirtualFree(reinterpret_cast<void *>(image), 0, MEM_RELEASE);
        std::cout << "Renderer dispatch/profile/restoration checks passed\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
