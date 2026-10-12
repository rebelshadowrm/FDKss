#pragma once
#include <windows.h>
#include "fdkss/api.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace resource_stream {
struct Profile {
    std::uintptr_t framework, framework_table, worker, table, constructor, destructor, open, read, size, seek;
};
// Each profile was recovered separately from the pinned executable's constructor,
// vtable and stream dispatch. Only the checked ABI/storage implementation is shared.
inline constexpr Profile sky1{0x8ff030, 0x802d80, 0x68, 0x806508, 0x51bd50, 0x51be30, 0x51bf50, 0x51c530, 0x51bef0, 0x51c5b0};
inline constexpr Profile sky2{0xc60ec0, 0xb1ad08, 0xa0, 0xb1eb38, 0x654410, 0x6544f0, 0x654640, 0x654b90, 0x6545e0, 0x654c10};
inline bool memory(std::uintptr_t address, void *out, std::size_t size) {
    SIZE_T read = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(address), out, size, &read) && read == size;
}
inline std::uintptr_t pointer(std::uintptr_t address) {
    std::uintptr_t value = 0; memory(address, &value, sizeof(value)); return value;
}
inline int fail(const char *message, char *error, std::size_t capacity) {
    std::snprintf(error, capacity, "%s", message); return 1;
}
inline int read(std::uintptr_t image, const Profile &profile, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    DWORD worker = 0;
    const auto framework = pointer(image + profile.framework);
    if (!framework || pointer(framework) != image + profile.framework_table ||
        !memory(framework + profile.worker, &worker, sizeof(worker)) || worker != GetCurrentThreadId())
        return fail("resource reads require the verified update thread", error, capacity);
    if (count != 3 || args[0].type != FDKSS_STRING || args[1].type != FDKSS_INTEGER || args[2].type != FDKSS_INTEGER ||
        args[1].integer < 0 || args[1].integer > INT32_MAX || args[2].integer < 1 || args[2].integer > 8192)
        return fail("expected relative asset path, offset (0..INT32_MAX) and byte count (1..8192)", error, capacity);
    std::string path(args[0].string, args[0].string_size);
    if (path.empty() || path.size() >= 512) return fail("invalid resource path length", error, capacity);
    for (auto &ch : path) {
        if (static_cast<unsigned char>(ch) < 32 || static_cast<unsigned char>(ch) >= 127 || ch == ':')
            return fail("resource paths must be printable ASCII relative paths", error, capacity);
        if (ch == '\\') ch = '/';
    }
    for (std::size_t begin = 0; begin <= path.size();) {
        const auto end = path.find('/', begin);
        const auto part = path.substr(begin, end == path.npos ? end : end - begin);
        if (part.empty() || part == "." || part == "..") return fail("resource paths cannot contain empty or dot components", error, capacity);
        if (end == path.npos) break;
        begin = end + 1;
    }
    const auto [framework_rva, framework_table, worker_offset, table, constructor, destructor, open, read_rva, size_rva, seek_rva] = profile;
    if (pointer(image + table) != image + destructor || pointer(image + table + 0x28) != image + open ||
        pointer(image + table + 0x38) != image + read_rva || pointer(image + table + 0x48) != image + seek_rva ||
        pointer(image + table + 0x18) != image + size_rva)
        return fail("FileStream resource profile changed", error, capacity);
    constexpr unsigned char expected[]{0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x4c};
    unsigned char actual[sizeof(expected)]{};
    if (!memory(image + constructor, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected)))
        return fail("FileStream constructor changed", error, capacity);
    // Constructor writes through +0x50; reserve additional storage beyond that prefix.
    alignas(16) std::array<unsigned char, 128> stream{};
    using Construct = void *(__fastcall *)(void *);
    using Destroy = void *(__fastcall *)(void *, unsigned);
    using Open = int (__fastcall *)(void *, const char *, std::uint32_t, std::uint32_t, std::uint16_t);
    using Read = int (__fastcall *)(void *, void *, std::uint32_t);
    using Size = std::uint64_t (__fastcall *)(void *);
    using Seek = bool (__fastcall *)(void *, std::int32_t, std::uint32_t);
    reinterpret_cast<Construct>(image + constructor)(stream.data());
    struct Cleanup { void *stream; Destroy destroy; ~Cleanup() { destroy(stream, 0); } } cleanup{stream.data(), reinterpret_cast<Destroy>(image + destructor)};
    if (!pointer(reinterpret_cast<std::uintptr_t>(stream.data()) + 0x18)) return fail("FileStream allocation failed", error, capacity);
    if (reinterpret_cast<Open>(image + open)(stream.data(), path.c_str(), 0, 3, 0) != 0)
        return fail("engine could not open resource", error, capacity);
    const auto length = reinterpret_cast<Size>(image + size_rva)(stream.data());
    if (length > INT64_MAX) return fail("resource length exceeds supported range", error, capacity);
    const auto offset = static_cast<std::uint64_t>(args[1].integer);
    std::array<char, 8192> data{}; int bytes = 0;
    if (offset < length) {
        if (offset && !reinterpret_cast<Seek>(image + seek_rva)(stream.data(), static_cast<std::int32_t>(offset), 0))
            return fail("engine resource seek failed", error, capacity);
        const auto requested = static_cast<std::uint32_t>(std::min<std::uint64_t>(args[2].integer, length - offset));
        bytes = reinterpret_cast<Read>(image + read_rva)(stream.data(), data.data(), requested);
        if (bytes < 0 || static_cast<std::uint32_t>(bytes) > requested) return fail("FileStream returned an invalid byte count", error, capacity);
    }
    fdkss_value result{}; result.type = FDKSS_STRING; result.string = data.data(); result.string_size = bytes;
    if (emit(sink, &result)) return 1;
    result = {}; result.type = FDKSS_INTEGER; result.integer = static_cast<std::int64_t>(length); return emit(sink, &result);
}
}
