#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "scripts.h"
#include "../vm_pure.h"
#include "../vm_timer_read.h"
#include "../vm_delta_read.h"
#include "native_math.h"
#include "../vm_instrument.h"
#include "../vm_jobs.h"
#include "../vm_snapshot.h"
#include "../vm_thread_dispatch.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#if defined(FDKSS_TEST_SKY2_LAYOUT)
extern std::uintptr_t sky2_test_base();
#endif

namespace {
bool bytes(std::uintptr_t address, void *output, std::size_t size) {
    SIZE_T read = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(address), output, size, &read) && read == size;
}
template<typename T> bool read(std::uintptr_t address, T &value) { return bytes(address, &value, sizeof(value)); }
std::uintptr_t pointer(std::uintptr_t address) { std::uintptr_t value = 0; read(address, value); return value; }
std::uintptr_t base() {
#if defined(FDKSS_TEST_SKY2_LAYOUT)
    return sky2_test_base();
#else
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
#endif
}
bool catalog_owner(void *context) {
    if (!context) return true;
    const auto &profile = *static_cast<const sky1_scripts::CatalogProfile *>(context);
    const auto framework = pointer(base() + profile.framework_global);
    DWORD worker = 0;
    return framework && pointer(framework) == base() + profile.framework_vtable &&
        read(framework + profile.worker_offset, worker) && worker == GetCurrentThreadId();
}
bool type(std::uintptr_t address, const char *expected) {
    const auto table = pointer(address), locator = table >= 8 ? pointer(table - 8) : 0;
    std::uint32_t data[6]{};
    char name[128]{};
    return read(locator, data) && data[0] == 1 && locator >= data[5] && locator - data[5] == base() &&
        read(base() + data[3] + 16, name) && std::memchr(name, 0, sizeof(name)) && std::strcmp(name, expected) == 0;
}
struct Header { char magic[4]; std::uint32_t functions, count, variables, inputs, outputs; };
struct Program { std::string name; std::uintptr_t data, descriptor; std::uint32_t size; Header header; };
struct Function {
    std::uint32_t code; std::uint8_t inputs, flags0, flags1, outputs;
    std::uint32_t output_args, input_args, structs, struct_data, checksum, name;
};
static_assert(sizeof(Header) == 24 && sizeof(Function) == 32);
std::vector<Program> catalog(void *context) {
    std::vector<Program> result;
    const auto *profile = static_cast<const sky1_scripts::CatalogProfile *>(context);
    const auto manager = pointer(base() + (profile ? profile->manager_global : 0x8fefc8));
    if (!manager) return result;
    if (profile && pointer(manager) != base() + profile->manager_vtable) return result;
    if (!type(manager, ".?AVManager@script@sora@@")) return result;
    struct Vector { std::uintptr_t data, count, capacity; } vector{};
    if (!read(manager + 8, vector) || vector.count > vector.capacity || vector.capacity > 4096 ||
        (vector.count && !vector.data)) return result;
    for (std::uintptr_t index = 0; index < vector.count; ++index) {
        const auto entry = vector.data + index * 0x48;
        const auto resource = pointer(entry + 8), descriptor = pointer(entry + 0x10);
        Program program{};
        char name[32]{};
        if (!type(resource, ".?AVBinaryResource@fdk@@") || !read(entry + 0x18, name) ||
            !std::memchr(name, 0, sizeof(name)) || !*name) continue;
        program.name = name; program.data = pointer(descriptor); program.descriptor = descriptor;
        if (!read(descriptor + 8, program.size) || program.size < sizeof(Header) || program.size > 16 * 1024 * 1024 ||
            !read(program.data, program.header) || std::memcmp(program.header.magic, "#scp", 4) ||
            program.header.functions < sizeof(Header) || program.header.functions > program.size ||
            program.header.count > 65536 || program.header.count > (program.size - program.header.functions) / sizeof(Function)) continue;
        result.push_back(std::move(program));
    }
    return result;
}
std::uint32_t checksum(std::string_view value) {
    std::uint32_t crc = 0xffffffff;
    for (const unsigned char ch : value) {
        crc ^= ch;
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320 & (0u - (crc & 1)));
    }
    return crc;
}
bool pure_metadata(const Program &program, const Function &record) {
    // The independently traced Start path reads parameters/defaults, not these
    // extra records. CALL/EXIT use flags0=1 only to suppress diagnostic identity.
    return record.inputs <= 32 && record.outputs <= record.inputs && record.flags0 <= 1 && !record.flags1 &&
        record.structs <= 65536 && record.struct_data <= program.size &&
        record.structs <= (program.size - record.struct_data) / 12;
}
bool function(const Program &program, std::uint32_t index, Function &record, std::string &name) {
    if (index >= program.header.count || !read(program.data + program.header.functions + index * sizeof(Function), record) ||
        record.code >= program.size || (record.name >> 30) != 3) return false;
    const auto offset = record.name & 0x3fffffff;
    if (offset >= program.size || record.input_args > program.size || record.output_args > program.size ||
        record.inputs > (program.size - record.input_args) / 4 || record.outputs > (program.size - record.output_args) / 4) return false;
    char text[256]{};
    const auto size = std::min<std::size_t>(sizeof(text), program.size - offset);
    if (!bytes(program.data + offset, text, size) || !std::memchr(text, 0, size)) return false;
    name = text;
    return checksum(name) == record.checksum;
}
int fail(const char *message, char *error, std::size_t size) { std::snprintf(error, size, "%s", message); return 1; }
struct ProgramLease {
    std::uintptr_t manager = 0, descriptor = 0, resource = 0, release = 0, manager_global = 0;
    static std::uintptr_t entry(std::uintptr_t manager, std::uintptr_t descriptor) {
        struct Vector { std::uintptr_t data, count, capacity; } vector{};
        if (!read(manager + 8, vector) || vector.count > vector.capacity || vector.capacity > 4096 || (vector.count && !vector.data)) return 0;
        std::uintptr_t found = 0;
        for (std::uintptr_t i = 0; i < vector.count; ++i) {
            const auto candidate = vector.data + i * 0x48;
            if (pointer(candidate + 0x10) == descriptor) { if (found) return 0; found = candidate; }
        }
        return found;
    }
    bool acquire(void *context, const Program &program) {
        const bool second = context && static_cast<const sky1_scripts::CatalogProfile *>(context)->manager_global == 0xc60e58;
        if (context && !second) return false;
        const auto manager_rva = second ? 0xc60e58 : 0x8fefc8;
        const auto acquire_rva = second ? 0x4cb910 : 0x3de8b0;
        const auto release_rva = second ? 0x4cbed0 : 0x3dee70;
        const auto current_manager = pointer(base() + manager_rva);
        if (!type(current_manager, ".?AVManager@script@sora@@")) return false;
        constexpr unsigned char acquire_prefix[]{0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x6c};
        constexpr unsigned char first_release[]{0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x6c};
        constexpr unsigned char second_release[]{0x48,0x85,0xd2,0x0f,0x84,0xf5,0x00,0x00};
        unsigned char actual[8]{};
        if (!read(base() + acquire_rva, actual) || std::memcmp(actual, acquire_prefix, 8) ||
            !read(base() + release_rva, actual) || std::memcmp(actual, second ? second_release : first_release, 8)) return false;
        const auto current_entry = entry(current_manager, program.descriptor);
        std::uint32_t owner = 0, group = 0; std::int32_t references = 0;
        char path[32]{};
        if (!current_entry || !read(current_entry, owner) || !read(current_entry + 0x3c, group) ||
            !read(current_entry + 0x40, references) || references <= 0 || references == INT32_MAX ||
            !read(program.descriptor + 12, path) || !*path || !std::memchr(path, 0, sizeof(path)) ||
            pointer(program.descriptor) != program.data) return false;
        const auto current_resource = pointer(current_entry + 8);
        if (!type(current_resource, ".?AVBinaryResource@fdk@@")) return false;
        using Acquire = void *(__fastcall *)(void *, const char *, std::uint32_t, std::uint32_t, bool);
        // The descriptor's actual path and owner match a currently loaded entry.
        // The engine's mutex-protected fast path increments that entry's count.
        const auto held = reinterpret_cast<std::uintptr_t>(reinterpret_cast<Acquire>(base() + acquire_rva)(
            reinterpret_cast<void *>(current_manager), reinterpret_cast<const char *>(program.descriptor + 12), owner, group, true));
        if (!held) return false;
        manager = current_manager; descriptor = held; release = base() + release_rva; manager_global = base() + manager_rva;
        const auto held_entry = entry(manager, descriptor);
        resource = held_entry ? pointer(held_entry + 8) : 0;
        std::int32_t after = 0;
        return held == program.descriptor && resource == current_resource && read(held_entry + 0x40, after) && after == references + 1;
    }
    ~ProgramLease() {
        if (!descriptor || pointer(manager_global) != manager || !type(manager, ".?AVManager@script@sora@@")) return;
        // Forced group unload ignores reference counts. Never release a stale entry.
        const auto current = entry(manager, descriptor);
        std::int32_t references = 0;
        if (!current || pointer(current + 8) != resource || !read(current + 0x40, references) || references <= 0) return;
        using Release = void (__fastcall *)(void *, void *);
        reinterpret_cast<Release>(release)(reinterpret_cast<void *>(manager), reinterpret_cast<void *>(descriptor));
    }
    ProgramLease() = default;
    ProgramLease(const ProgramLease &) = delete;
    ProgramLease &operator=(const ProgramLease &) = delete;
};
struct ExecutionProfile { std::uintptr_t manager, table, constructor, destructor, start, update, dispatch; };
bool execution_profile(void *context, ExecutionProfile &profile, char *error, std::size_t size) {
    auto reject = [&](const char *message, char *out, std::size_t capacity) { fail(message, out, capacity); return false; };
    constexpr ExecutionProfile sky1{0x8fefc8, 0x7fc550, 0x3e00e0, 0x3e0290, 0x3e0490, 0x3e04b0,0x4b29f0};
    constexpr ExecutionProfile sky2{0xc60e58, 0xb13128, 0x4cd3f0, 0x4cd5a0, 0x4cd7d0, 0x4cd7f0,0x5e63c0};
    if (context && static_cast<const sky1_scripts::CatalogProfile *>(context)->manager_global != sky2.manager)
        return reject("unknown VM execution profile", error, size);
    profile = context ? sky2 : sky1;
    const auto manager = pointer(base() + profile.manager);
    if (!type(manager, ".?AVManager@script@sora@@") || pointer(base() + profile.table) != base() + profile.destructor ||
        pointer(base() + profile.table + 8) != base() + profile.start || pointer(base() + profile.table + 0x18) != base() + profile.update)
        return reject("VM profile does not match", error, size);
    constexpr unsigned char expected[]{0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c};
    unsigned char actual[sizeof(expected)]{};
    if (!read(base() + profile.constructor, actual) || std::memcmp(actual, expected, sizeof(expected)))
        return reject("VM constructor changed", error, size);
    return true;
}
bool private_timer_read_profile(const ExecutionProfile &profile, std::uintptr_t expected=0) {
    return private_timer::read_profile(base(),profile.manager==0xc60e58,expected,
        [](std::uintptr_t address,void *out,std::size_t size){return bytes(address,out,size);},
        [](std::uintptr_t address) {
            MEMORY_BASIC_INFORMATION region{};
            return VirtualQuery(reinterpret_cast<void *>(address),&region,sizeof(region)) && region.State==MEM_COMMIT &&
                !(region.Protect & (PAGE_GUARD|PAGE_NOACCESS)) &&
                (region.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
        });
}

bool private_delta_read_profile(bool second,std::uint32_t &raw,std::uint32_t &source) {
    return private_delta::profile(base(),second,bytes,[](std::uintptr_t address) {
        MEMORY_BASIC_INFORMATION region{};
        return VirtualQuery(reinterpret_cast<void *>(address),&region,sizeof(region)) && region.State==MEM_COMMIT &&
            !(region.Protect & (PAGE_GUARD|PAGE_NOACCESS)) &&
            (region.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
    },raw,source);
}
int execute_constant(void *context, void *descriptor, std::uint32_t index, std::uint32_t expected_result,
    char *error, std::size_t size, const std::uint32_t *arguments = nullptr,
    std::uint32_t argument_count = 0, bool verify_registers = false, const Program *loaded_program = nullptr, bool timer_read = false,
    const std::uint32_t *delta_source=nullptr,unsigned scalar_natives=0) {
    if (!catalog_owner(context)) return fail("VM execution requires the verified update thread", error, size);
    ExecutionProfile profile{};
    if (!execution_profile(context, profile, error, size)) return 1;
    if (timer_read && !private_timer_read_profile(profile)) return fail("private timer read profile changed",error,size);
    if(!native_math::verified_commands({base(),context!=nullptr},scalar_natives))return fail("native scalar profile changed",error,size);
    ProgramLease lease;
    if (loaded_program && !lease.acquire(context, *loaded_program)) return fail("VM program lease could not be acquired", error, size);
    alignas(16) std::array<unsigned char, 0x420> thread{};
    using Constructor = void *(__fastcall *)(void *, const char *);
    using Destructor = void *(__fastcall *)(void *, unsigned);
    using Start = bool (__fastcall *)(void *, void *, std::uint32_t, const std::uint32_t *, std::uint32_t);
    reinterpret_cast<Constructor>(base() + profile.constructor)(thread.data(), "FDKssCall");
    struct Cleanup { void *thread; Destructor destroy; ~Cleanup() { destroy(thread, 0); } } cleanup{thread.data(), reinterpret_cast<Destructor>(base() + profile.destructor)};
    if (verify_registers) {
        std::uint32_t registers[16]{}, capacity = 0; std::uintptr_t stack = 0;
        std::memcpy(registers, thread.data() + 0x14, sizeof(registers));
        std::memcpy(&capacity, thread.data() + 0x60, sizeof(capacity));
        std::memcpy(&stack, thread.data() + 0x58, sizeof(stack));
        if (std::any_of(std::begin(registers), std::end(registers), [](auto raw) { return raw != 0; }) || capacity != 512 || stack != reinterpret_cast<std::uintptr_t>(thread.data() + 0xc8))
            return fail("private VM register or stack initialization changed", error, size);
    }
    if (timer_read) {
        std::uint32_t elapsed=0;unsigned char active=0;std::memcpy(&elapsed,thread.data()+0x410,4);std::memcpy(&active,thread.data()+0x415,1);
        if (elapsed!=0 || active) return fail("private timer initialization changed",error,size);
    }
    if (delta_source) {
        std::uint32_t raw=0,source=0,thread_delta=0;
        std::memcpy(&thread_delta,thread.data()+0x7c,4);
        if (!private_delta_read_profile(profile.manager==0xc60e58,raw,source) || source!=*delta_source ||
            (profile.manager!=0xc60e58 && thread_delta!=0)) return fail("private delta read profile/value changed",error,size);
    }
    if(!native_math::verified_commands({base(),context!=nullptr},scalar_natives))return fail("native scalar profile changed before VM start",error,size);
    if (!reinterpret_cast<Start>(base() + profile.start)(thread.data(), descriptor, index, arguments, argument_count))
        return fail("VM refused function start", error, size);
    std::uint32_t flags = 0, result = 0;
    std::memcpy(&flags, thread.data() + 0x74, sizeof(flags));
    std::memcpy(&result, thread.data() + 0x14, sizeof(result));
    if (flags & 1 || result != expected_result) return fail("VM function did not finish with the preflight result", error, size);
    return 0;
}
int integer(fdkss_result_fn emit, void *sink, std::int64_t number) {
    fdkss_value value{}; value.type = FDKSS_INTEGER; value.integer = number; return emit(sink, &value);
}
int string(fdkss_result_fn emit, void *sink, const std::string &text) {
    fdkss_value value{}; value.type = FDKSS_STRING; value.string = text.data(); value.string_size = text.size(); return emit(sink, &value);
}
std::string_view text(const fdkss_value &value) { return {value.string, value.string_size}; }
bool page(const fdkss_value *args) {
    return args[0].type == FDKSS_INTEGER && args[0].integer >= 0 && args[0].integer <= 65536 &&
        args[1].type == FDKSS_INTEGER && args[1].integer >= 1 && args[1].integer <= 32;
}
bool variable_layout(const Program &program, std::uint32_t &count, std::uintptr_t &values) {
    const auto total = static_cast<std::uint64_t>(program.header.inputs) + program.header.outputs;
    if (total > 4096 || program.header.variables > program.size || total > (program.size - program.header.variables) / 8 ||
        pointer(program.descriptor + 0x30) != program.data ||
        pointer(program.descriptor + 0x40) != program.data + program.header.variables ||
        !read(program.descriptor + 0x50, count) || count != total) return false;
    values = pointer(program.descriptor + 0x48);
    return !count || (values && values <= std::numeric_limits<std::uintptr_t>::max() - static_cast<std::size_t>(count) * 4);
}
bool variable_owner(void *context) {
    static sky1_scripts::CatalogProfile first{0, 0, 0x8ff030, 0x802d80, 0x68};
    // Only framework identity and the worker are used by catalog_owner.
    return catalog_owner(context ? context : &first);
}
bool variable_name(const fdkss_value &value) {
    return value.type == FDKSS_STRING && value.string && value.string_size > 0 && value.string_size <= 255 &&
        !std::memchr(value.string, 0, value.string_size);
}
bool variable_record(const Program &program, std::uint32_t index, std::string &name, std::uint32_t &metadata) {
    std::uint32_t entry[2]{};
    if (!read(program.data + program.header.variables + static_cast<std::size_t>(index) * 8, entry) || (entry[0] >> 30) != 3) return false;
    const auto offset = entry[0] & 0x3fffffff;
    if (offset >= program.size) return false;
    char text[256]{};
    const auto size = std::min<std::size_t>(sizeof(text), program.size - offset);
    if (!bytes(program.data + offset, text, size) || !std::memchr(text, 0, size) || !*text) return false;
    name = text; metadata = entry[1]; return true;
}
int resolve_variable(void *context, const fdkss_value *args, Program &program, std::int64_t &index,
    std::uint32_t &metadata, std::uint32_t &raw, std::uintptr_t &address, char *error, std::size_t size) {
    const auto programs = catalog(context);
    const Program *found = nullptr;
    for (const auto &candidate : programs) if (candidate.name == text(args[0])) {
        if (found) return fail("script name is ambiguous; variable access requires a unique loaded program", error, size);
        found = &candidate;
    }
    index = -1;
    if (!found) return 0;
    std::uint32_t total = 0; std::uintptr_t values = 0;
    if (!variable_layout(*found, total, values)) return fail("script variable layout changed", error, size);
    for (std::uint32_t cursor = 0; cursor < total; ++cursor) {
        std::string name; std::uint32_t current = 0;
        if (!variable_record(*found, cursor, name, current)) return fail("invalid script variable metadata", error, size);
        if (name != text(args[1])) continue;
        if (index != -1) return fail("script variable name is ambiguous", error, size);
        index = cursor; metadata = current;
    }
    if (index == -1) return 0;
    address = values + static_cast<std::size_t>(index) * 4;
    if (!read(address, raw)) return fail("script variable value unavailable", error, size);
    program = *found; return 0;
}
}
namespace sky1_scripts {
int FDKSS_CALL variables(void *context, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!variable_owner(context)) return fail("script variable access requires the verified update thread", error, size);
    if (count != 3 || !variable_name(args[0]) || !page(args + 1)) return fail("expected script name, offset, limit (1..32)", error, size);
    const auto programs = catalog(context);
    const Program *found = nullptr;
    for (const auto &program : programs) if (program.name == text(args[0])) {
        if (found) return fail("script name is ambiguous; variable access requires a unique loaded program", error, size);
        found = &program;
    }
    if (!found) return integer(emit, sink, -1);
    std::uint32_t total = 0; std::uintptr_t values = 0;
    if (!variable_layout(*found, total, values)) return fail("script variable layout changed", error, size);
    std::vector<std::string> names;
    auto index = static_cast<std::uint32_t>(args[1].integer);
    for (; index < total && names.size() < static_cast<std::size_t>(args[2].integer); ++index) {
        std::string name; std::uint32_t metadata = 0;
        if (!variable_record(*found, index, name, metadata)) return fail("invalid script variable metadata", error, size);
        names.push_back(std::move(name));
    }
    if (integer(emit, sink, index < total ? index : -1LL)) return 1;
    for (const auto &name : names) if (string(emit, sink, name)) return 1;
    return 0;
}
int FDKSS_CALL variable_info(void *context, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!variable_owner(context)) return fail("script variable access requires the verified update thread", error, size);
    if (count != 2 || !variable_name(args[0]) || !variable_name(args[1])) return fail("expected script and variable names", error, size);
    fdkss_value result{};
    Program program{}; std::int64_t index = -1; std::uint32_t metadata = 0, raw = 0; std::uintptr_t address = 0;
    if (resolve_variable(context, args, program, index, metadata, raw, address, error, size)) return 1;
    if (index == -1) return emit(sink, &result);
    std::string copied;
    if ((raw >> 30) == 1) { result.type = FDKSS_INTEGER; result.integer = static_cast<std::int32_t>(raw << 2) >> 2; }
    else if ((raw >> 30) == 2) {
        result.type = FDKSS_NUMBER; result.number = std::bit_cast<float>(raw << 2);
        if (!std::isfinite(result.number)) return fail("script variable float is not finite", error, size);
    } else if ((raw >> 30) == 3) {
        const auto offset = raw & 0x3fffffff;
        if (!offset || offset >= program.size) return fail("script variable string offset is invalid", error, size);
        copied.resize(std::min<std::size_t>(16384, program.size - offset));
        if (!bytes(program.data + offset, copied.data(), copied.size())) return fail("script variable string unavailable", error, size);
        const auto end = copied.find('\0');
        if (end == copied.npos) return fail("script variable string exceeds result limit", error, size);
        copied.resize(end); result.type = FDKSS_STRING; result.string = copied.data(); result.string_size = copied.size();
    } else if (raw) return fail("unsupported script variable value tag", error, size);
    if (integer(emit, sink, index) || integer(emit, sink, metadata) || integer(emit, sink, raw)) return 1;
    return emit(sink, &result);
}
int FDKSS_CALL set_variable(void *context, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!variable_owner(context)) return fail("script variable writes require the verified update thread", error, size);
    if (count != 3 || !variable_name(args[0]) || !variable_name(args[1]) ||
        (args[2].type != FDKSS_INTEGER && args[2].type != FDKSS_NUMBER))
        return fail("expected script, variable name and numeric value", error, size);
    Program program{}; std::int64_t index = -1; std::uint32_t metadata = 0, raw = 0; std::uintptr_t address = 0;
    if (resolve_variable(context, args, program, index, metadata, raw, address, error, size)) return 1;
    if (index == -1) return fail("script or variable is not loaded", error, size);
    std::uint32_t encoded = 0; fdkss_value result{};
    if ((raw >> 30) == 1) {
        if (args[2].type != FDKSS_INTEGER || args[2].integer < -536870912 || args[2].integer > 536870911)
            return fail("integer variable requires an integer in -536870912..536870911", error, size);
        encoded = (static_cast<std::uint32_t>(args[2].integer) & 0x3fffffff) | 0x40000000;
        result.type = FDKSS_INTEGER; result.integer = args[2].integer;
    } else if ((raw >> 30) == 2) {
        const auto requested = args[2].type == FDKSS_INTEGER ? static_cast<double>(args[2].integer) : args[2].number;
        if (!std::isfinite(std::bit_cast<float>(raw << 2)) || !std::isfinite(requested) ||
            std::abs(requested) > std::numeric_limits<float>::max()) return fail("float variable requires a finite representable value", error, size);
        encoded = (std::bit_cast<std::uint32_t>(static_cast<float>(requested)) >> 2) | 0x80000000;
        result.type = FDKSS_NUMBER; result.number = std::bit_cast<float>(encoded << 2);
    } else return fail("only existing numeric script variables are writable", error, size);
    MEMORY_BASIC_INFORMATION region{};
    if (address % 4 || !VirtualQuery(reinterpret_cast<void *>(address), &region, sizeof(region)) || region.State != MEM_COMMIT ||
        (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
        ((region.Protect & 0xff) != PAGE_READWRITE && (region.Protect & 0xff) != PAGE_WRITECOPY) ||
        region.RegionSize < sizeof(encoded) ||
        address - reinterpret_cast<std::uintptr_t>(region.BaseAddress) > region.RegionSize - sizeof(encoded))
        return fail("script variable storage is not writable", error, size);
    SIZE_T written = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(address), &encoded, sizeof(encoded), &written) || written != sizeof(encoded))
        return fail("script variable write failed", error, size);
    std::uint32_t verified = 0;
    if (!read(address, verified) || verified != encoded) return fail("script variable readback failed", error, size);
    return emit(sink, &result);
}
bool constant_code(const unsigned char *code, std::size_t size, std::uint32_t &raw) {
    if (!code) return false;
    std::size_t offset = 0;
    // LINE consumes a u16 and updates only the thread's diagnostic line number.
    for (int lines = 0; lines < 8 && offset < size && code[offset] == 0x26; ++lines) {
        if (size - offset < 3) return false;
        offset += 3;
    }
    if (offset < size && code[offset] == 0x0d) { raw = 0; return true; }
    if (size - offset < 9 || code[offset] != 0 || code[offset + 1] != 4 ||
        code[offset + 6] != 0x0a || code[offset + 7] != 0 || code[offset + 8] != 0x0d) return false;
    std::memcpy(&raw, code + offset + 2, sizeof(raw));
    if ((raw >> 30) == 0) return raw == 0;
    if ((raw >> 30) == 2) return std::isfinite(std::bit_cast<float>(raw << 2));
    return true;
}
int FDKSS_CALL call_constant(void *context, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!catalog_owner(context)) return fail("VM execution requires the verified update thread", error, size);
    if (count != 2 || args[0].type != FDKSS_STRING || args[1].type != FDKSS_STRING)
        return fail("expected script and constant function names", error, size);
    for (const auto &program : catalog(context)) if (program.name == text(args[0])) {
        for (std::uint32_t index = 0; index < program.header.count; ++index) {
            Function record{}; std::string name;
            if (!function(program, index, record, name)) return fail("invalid compiled function metadata", error, size);
            if (name != text(args[1])) continue;
            std::array<unsigned char, 33> code{};
            const auto bytes_to_read = std::min<std::size_t>(code.size(), program.size - record.code);
            std::uint32_t raw = 0;
            if (record.inputs || !bytes(program.data + record.code, code.data(), bytes_to_read) ||
                !constant_code(code.data(), bytes_to_read, raw))
                return fail("function is not a verified zero-argument constant return; broader script calls are not supported yet", error, size);
            if (pointer(program.descriptor + 0x30) != program.data ||
                pointer(program.descriptor + 0x38) != program.data + program.header.functions)
                return fail("compiled program descriptor changed", error, size);
            if (execute_constant(context, reinterpret_cast<void *>(program.descriptor), index, raw, error, size, nullptr, 0, false, &program)) return 1;
            fdkss_value result{};
            if ((raw >> 30) == 1) { result.type = FDKSS_INTEGER; result.integer = static_cast<std::int32_t>(raw << 2) >> 2; }
            else if ((raw >> 30) == 2) { result.type = FDKSS_NUMBER; result.number = std::bit_cast<float>(raw << 2); }
            else if ((raw >> 30) == 3) {
                const auto offset = raw & 0x3fffffff;
                if (!offset || offset >= program.size) return fail("invalid constant string offset", error, size);
                std::string value(std::min<std::size_t>(16384, program.size - offset), '\0');
                if (!bytes(program.data + offset, value.data(), value.size())) return fail("constant string unavailable", error, size);
                const auto end = value.find('\0');
                if (end == value.npos) return fail("constant string exceeds result limit", error, size);
                value.resize(end); return string(emit, sink, value);
            }
            return emit(sink, &result);
        }
        return fail("function is not loaded", error, size);
    }
    return fail("script is not loaded", error, size);
}
namespace {
struct CallGraph {
    const Program &program;
    struct Cached { std::array<unsigned char, 1024> code{}; pure_vm::FunctionCode view{}; };
    std::deque<Cached> functions;
    struct Global { std::uint32_t index, raw; };
    std::vector<Global> globals;
    std::uintptr_t global_values = 0;
    std::uint32_t global_count = 0;
    private_timer::Preview timer;
    bool allow_delta=false, delta_read=false, second=false;
    bool allow_scalar_natives=false;
    unsigned scalar_reads=0;
    std::uint32_t delta_raw=0,delta_source=0;
    static bool native(void *context,unsigned group,unsigned command,const std::uint32_t *args,unsigned count) {
        return static_cast<CallGraph *>(context)->timer.invoke(group,command,args,count);
    }
    static bool native_result(void *context,unsigned group,unsigned command,const std::uint32_t *args,unsigned count,std::uint32_t &raw) {
        auto &graph=*static_cast<CallGraph *>(context);
        if(group==0 && command==39 && count==1 && args) {
            if(!graph.allow_scalar_natives || graph.timer.used || !native_math::preview({base(),graph.second},args[0],raw))return false;
            graph.scalar_reads|=1;return true;
        }
        if(group==0 && (command==25 || command==26) && count==2 && args) {
            // The evaluator supplies stack order; logical arg0 is at the top.
            if(!graph.allow_scalar_natives || graph.timer.used ||
                !native_math::preview_selection({base(),graph.second},args[1],args[0],command==26,raw))return false;
            graph.scalar_reads|=command==25 ? 2u : 4u;return true;
        }
        if(group==0 && command==23 && count==1 && args) {
            if(!graph.allow_scalar_natives || graph.timer.used || !native_math::preview_sqrt({base(),graph.second},args[0],raw))return false;
            graph.scalar_reads|=8u;return true;
        }
        if (private_delta::command(group,command,count)) {
            if (!graph.allow_delta || graph.timer.used) return false;
            std::uint32_t source=0,predicted=0;
            if (!private_delta_read_profile(graph.second,predicted,source) || (graph.delta_read && source!=graph.delta_source)) return false;
            graph.delta_read=true;graph.delta_source=source;graph.delta_raw=raw=predicted;return true;
        }
        return graph.timer.read_result(group,command,args,count,raw);
    }
    static bool read_global(void *context, std::uint32_t index, std::uint32_t &raw) {
        auto &graph = *static_cast<CallGraph *>(context);
        if (graph.globals.empty() && !variable_layout(graph.program, graph.global_count, graph.global_values)) return false;
        if (index >= graph.global_count) return false;
        for (const auto &entry : graph.globals) if (entry.index == index) { raw = entry.raw; return true; }
        if (!read(graph.global_values + static_cast<std::size_t>(index) * 4, raw)) return false;
        graph.globals.push_back({index, raw}); return true;
    }
    bool globals_unchanged() const {
        if (globals.empty()) return true;
        std::uint32_t count = 0; std::uintptr_t values = 0;
        if (!variable_layout(program, count, values) || count != global_count || values != global_values) return false;
        for (const auto &entry : globals) {
            std::uint32_t raw = 0;
            if (!read(values + static_cast<std::size_t>(entry.index) * 4, raw) || raw != entry.raw) return false;
        }
        return true;
    }
    static bool resolve(void *context, std::uint32_t index, pure_vm::FunctionCode &view) {
        auto &graph = *static_cast<CallGraph *>(context);
        for (const auto &entry : graph.functions) if (entry.view.index == index) { view = entry.view; return true; }
        if (graph.functions.size() >= 32 || index >= graph.program.header.count) return false;
        Function record{}; std::string name;
        if (!function(graph.program, index, record, name) || !pure_metadata(graph.program, record)) return false;
        std::array<std::uint32_t, 32> metadata{};
        if (!bytes(graph.program.data + record.input_args, metadata.data(), record.inputs * 4) ||
            std::any_of(metadata.begin(), metadata.begin() + record.inputs, [](auto raw) { return (raw & 4) != 0; })) return false;
        graph.functions.emplace_back();
        auto &entry = graph.functions.back();
        const auto length = std::min<std::size_t>(entry.code.size(), graph.program.size - record.code);
        if (!bytes(graph.program.data + record.code, entry.code.data(), length)) { graph.functions.pop_back(); return false; }
        entry.view = {entry.code.data(), length, record.inputs, record.code, index};
        view = entry.view; return true;
    }
};
struct PureSnapshot {
    std::vector<unsigned char> source;
    std::vector<std::uint32_t> globals;
    std::array<std::uint32_t,32> arguments{};
    std::uint32_t count = 0, index = 0, raw = 0;
    std::string result;
    bool timer_read=false;
};
}
int pure_call(void *context, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t size, bool execute, PureSnapshot *snapshot = nullptr,
    const Program *local = nullptr, bool allow_wait=false) {
    if (!variable_owner(context)) return fail("VM execution requires the verified update thread", error, size);
    if (count < 2 || count > 34 || !variable_name(args[0]) || !variable_name(args[1]))
        return fail("expected script, function and at most 32 scalar arguments", error, size);
    std::array<std::uint32_t, 32> arguments{};
    for (std::size_t i = 2; i < count; ++i) {
        const auto &value = args[i];
        if (value.type == FDKSS_NIL) arguments[i - 2] = 0;
        else if (value.type == FDKSS_INTEGER && value.integer >= -536870912 && value.integer <= 536870911)
            arguments[i - 2] = (static_cast<std::uint32_t>(value.integer) & 0x3fffffff) | 0x40000000;
        else if (value.type == FDKSS_NUMBER && std::isfinite(value.number) && std::abs(value.number) <= std::numeric_limits<float>::max())
            arguments[i - 2] = (std::bit_cast<std::uint32_t>(static_cast<float>(value.number)) >> 2) | 0x80000000;
        else return fail("pure VM arguments require nil, signed 30-bit integers or finite VM floats", error, size);
    }
    const auto programs = local ? std::vector<Program>{} : catalog(context);
    const Program *program = local;
    for (const auto &candidate : programs) if (candidate.name == text(args[0])) {
        if (program) return fail("script name is ambiguous", error, size);
        program = &candidate;
    }
    if (!program) return fail("script is not loaded", error, size);
    Function record{}; std::uint32_t index = 0; bool found = false;
    for (std::uint32_t cursor = 0; cursor < program->header.count; ++cursor) {
        Function candidate{}; std::string name;
        if (!function(*program, cursor, candidate, name)) return fail("invalid compiled function metadata", error, size);
        if (name != text(args[1])) continue;
        if (found) return fail("function name is ambiguous", error, size);
        found = true; index = cursor; record = candidate;
    }
    if (!found) return fail("function is not loaded", error, size);
    const auto supplied = count - 2;
    if (!pure_metadata(*program, record) || supplied > record.inputs ||
        supplied < static_cast<std::size_t>(record.inputs - record.outputs))
        return fail("pure VM call requires supported arity and function metadata", error, size);
    std::array<std::uint32_t, 32> defaults{};
    if (supplied < record.inputs) {
        if (!bytes(program->data + record.output_args, defaults.data(), record.outputs * 4))
            return fail("pure VM default arguments are unavailable", error, size);
        for (std::size_t i = supplied; i < record.inputs; ++i) {
            const auto raw = defaults[i - (record.inputs - record.outputs)];
            if (!pure_vm::scalar(raw) || (raw >> 30) == 3)
                return fail("pure VM defaults require nil or finite numeric values", error, size);
            arguments[i] = raw;
        }
    }
    std::array<std::uint32_t, 32> metadata{};
    if (!bytes(program->data + record.input_args, metadata.data(), record.inputs * 4) ||
        std::any_of(metadata.begin(), metadata.begin() + record.inputs, [](auto raw) { return (raw & 4) != 0; }))
        return fail("pure VM reference parameters are unsupported", error, size);
    std::array<unsigned char, 1024> code{};
    const auto length = std::min<std::size_t>(code.size(), program->size - record.code);
    std::uint32_t raw = 0;
    CallGraph graph{*program};
    graph.allow_delta=!snapshot && !allow_wait;
    graph.allow_scalar_natives=!snapshot && !allow_wait;
    graph.second=context!=nullptr;
    if (!bytes(program->data + record.code, code.data(), length) ||
        !pure_vm::evaluate(code.data(), length, arguments.data(), record.inputs, raw, record.code,
            CallGraph::resolve, &graph, index, CallGraph::read_global,allow_wait ? CallGraph::native : nullptr,CallGraph::native_result))
        return fail("function exceeds the verified bounded pure VM subset", error, size);
    if (graph.timer.read) {
        ExecutionProfile profile{};
        if (!execution_profile(context,profile,error,size) || !private_timer_read_profile(profile))
            return fail("private timer read profile changed",error,size);
    }
    // Decode strings before invoking the engine, so invalid results never reach Start.
    fdkss_value result{}; std::string copied;
    if ((raw >> 30) == 1) { result.type = FDKSS_INTEGER; result.integer = static_cast<std::int32_t>(raw << 2) >> 2; }
    else if ((raw >> 30) == 2) { result.type = FDKSS_NUMBER; result.number = std::bit_cast<float>(raw << 2); }
    else if ((raw >> 30) == 3) {
        const auto offset = raw & 0x3fffffff;
        if (!offset || offset >= program->size) return fail("pure VM string offset is invalid", error, size);
        copied.resize(std::min<std::size_t>(16384, program->size - offset));
        if (!bytes(program->data + offset, copied.data(), copied.size())) return fail("pure VM string unavailable", error, size);
        const auto end = copied.find('\0');
        if (end == copied.npos) return fail("pure VM string exceeds result limit", error, size);
        copied.resize(end); result.type = FDKSS_STRING; result.string = copied.data(); result.string_size = copied.size();
    }
    if (pointer(program->descriptor + 0x30) != program->data ||
        pointer(program->descriptor + 0x38) != program->data + program->header.functions)
        return fail("compiled program descriptor changed", error, size);
    if (!graph.globals_unchanged()) return fail("script variables changed during VM preflight", error, size);
    if (snapshot) {
        ProgramLease lease;
        if (!local && !lease.acquire(context,*program)) return fail("VM snapshot lease could not be acquired",error,size);
        snapshot->source.resize(program->size);
        std::uint32_t global_count = 0; std::uintptr_t values = 0;
        if (!bytes(program->data,snapshot->source.data(),snapshot->source.size()) ||
            !variable_layout(*program,global_count,values)) return fail("VM snapshot source changed",error,size);
        snapshot->globals.resize(global_count);
        if (global_count && !bytes(values,snapshot->globals.data(),global_count*4)) return fail("VM variable snapshot unavailable",error,size);
        if (std::memcmp(snapshot->source.data()+record.code,code.data(),length)) return fail("VM snapshot code changed",error,size);
        for (const auto &entry : graph.functions) if (std::memcmp(snapshot->source.data()+entry.view.offset,entry.code.data(),entry.view.size))
            return fail("VM snapshot callee changed",error,size);
        for (const auto &entry : graph.globals) if (snapshot->globals[entry.index]!=entry.raw)
            return fail("VM snapshot variable changed",error,size);
        snapshot->arguments=arguments; snapshot->count=record.inputs; snapshot->index=index;
        snapshot->timer_read=graph.timer.read;
        snapshot->raw=raw; snapshot->result=std::move(copied);
        return 0;
    }
    if (execute && execute_constant(context, reinterpret_cast<void *>(program->descriptor), index, raw, error, size,
        arguments.data(), static_cast<std::uint32_t>(count - 2), true, local ? nullptr : program, graph.timer.read,
        graph.delta_read ? &graph.delta_source : nullptr,graph.scalar_reads)) return 1;
    return emit(sink, &result);
}
int FDKSS_CALL call_pure(void *context, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    return pure_call(context, args, count, emit, sink, error, size, true);
}
int FDKSS_CALL preview_pure(void *context, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    return pure_call(context, args, count, emit, sink, error, size, false);
}
int FDKSS_CALL names(void *context, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!catalog_owner(context)) return fail("script catalog access requires the verified update thread", error, size);
    if (count != 3 || !page(args) || args[2].type != FDKSS_STRING) return fail("expected offset, limit (1..32), script-name filter", error, size);
    const auto programs = catalog(context);
    std::vector<std::string> result;
    auto index = static_cast<std::size_t>(args[0].integer);
    for (; index < programs.size() && result.size() < static_cast<std::size_t>(args[1].integer); ++index)
        if (programs[index].name.find(text(args[2])) != std::string::npos) result.push_back(programs[index].name);
    if (integer(emit, sink, index < programs.size() ? static_cast<std::int64_t>(index) : -1)) return 1;
    for (const auto &name : result) if (string(emit, sink, name)) return 1;
    return 0;
}
int FDKSS_CALL info(void *context, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!catalog_owner(context)) return fail("script catalog access requires the verified update thread", error, size);
    if (count != 1 || args[0].type != FDKSS_STRING) return fail("expected script name", error, size);
    for (const auto &program : catalog(context)) if (program.name == text(args[0])) {
        for (const auto value : {program.size, program.header.count, program.header.inputs, program.header.outputs})
            if (integer(emit, sink, value)) return 1;
        return 0;
    }
    fdkss_value missing{}; return emit(sink, &missing);
}
int FDKSS_CALL functions(void *context, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!catalog_owner(context)) return fail("script catalog access requires the verified update thread", error, size);
    if (count != 3 || args[0].type != FDKSS_STRING || !page(args + 1)) return fail("expected script name, offset, limit (1..32)", error, size);
    for (const auto &program : catalog(context)) if (program.name == text(args[0])) {
        std::vector<std::string> result;
        auto index = static_cast<std::uint32_t>(args[1].integer);
        for (; index < program.header.count && result.size() < static_cast<std::size_t>(args[2].integer); ++index) {
            Function record{}; std::string name;
            if (!function(program, index, record, name)) return fail("invalid compiled script function metadata", error, size);
            result.push_back(std::move(name));
        }
        if (integer(emit, sink, index < program.header.count ? static_cast<std::int64_t>(index) : -1)) return 1;
        for (const auto &name : result) if (string(emit, sink, name)) return 1;
        return 0;
    }
    return integer(emit, sink, -1);
}
int FDKSS_CALL function_info(void *context, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!catalog_owner(context)) return fail("script catalog access requires the verified update thread", error, size);
    if (count != 2 || args[0].type != FDKSS_STRING || args[1].type != FDKSS_STRING) return fail("expected script and function names", error, size);
    for (const auto &program : catalog(context)) if (program.name == text(args[0])) {
        for (std::uint32_t index = 0; index < program.header.count; ++index) {
            Function record{}; std::string name;
            if (!function(program, index, record, name)) return fail("invalid compiled script function metadata", error, size);
            if (name != text(args[1])) continue;
            const std::uint32_t values[]{index, record.inputs, record.outputs, record.flags0, record.flags1, record.checksum, record.code};
            for (const auto value : values) if (integer(emit, sink, value)) return 1;
            return 0;
        }
    }
    fdkss_value missing{}; return emit(sink, &missing);
}
int FDKSS_CALL function_at(void *context, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!catalog_owner(context)) return fail("script catalog access requires the verified update thread", error, size);
    if (count != 2 || args[0].type != FDKSS_STRING || args[1].type != FDKSS_INTEGER ||
        args[1].integer < 0 || args[1].integer >= 65536) return fail("expected script name and zero-based function index", error, size);
    for (const auto &program : catalog(context)) if (program.name == text(args[0])) {
        const auto index = static_cast<std::uint32_t>(args[1].integer);
        if (index >= program.header.count) break;
        Function record{}; std::string name;
        if (!function(program, index, record, name)) return fail("invalid compiled script function metadata", error, size);
        if (string(emit, sink, name)) return 1;
        const std::uint32_t values[]{index, record.inputs, record.outputs, record.flags0, record.flags1, record.checksum, record.code};
        for (const auto value : values) if (integer(emit, sink, value)) return 1;
        return 0;
    }
    fdkss_value missing{}; return emit(sink, &missing);
}
int FDKSS_CALL arithmetic_self_test(void *context, const fdkss_value *, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!variable_owner(context)) return fail("VM execution requires the verified update thread", error, size);
    if (count) return fail("arithmetic VM self test takes no arguments", error, size);
    std::array<unsigned char, 128> program{};
    std::array<unsigned char, 88> descriptor{};
    const Header header{{'#','s','c','p'}, 24, 1, 112, 0, 0};
    const std::string name = "FDKssArithmetic";
    Function record{}; record.code = 56; record.input_args = 96;
    record.name = 0xc0000042; record.checksum = checksum(name);
    std::memcpy(program.data(), &header, sizeof(header));
    std::memcpy(program.data() + 66, name.c_str(), name.size() + 1);
    auto put_pointer = [&](std::size_t offset, const void *value) { std::memcpy(descriptor.data() + offset, &value, sizeof(value)); };
    put_pointer(0, program.data()); put_pointer(0x30, program.data());
    put_pointer(0x38, program.data() + 24); put_pointer(0x40, program.data() + 112);
    const std::uint32_t length = static_cast<std::uint32_t>(program.size());
    std::memcpy(descriptor.data() + 8, &length, sizeof(length));
    std::memcpy(descriptor.data() + 12, "FDKssMathProbe", 15);
    auto encode = [](int value) { return (static_cast<std::uint32_t>(value) & 0x3fffffff) | 0x40000000; };
    std::int64_t completed = 0;
    for (unsigned op = 16; op <= 33; ++op) {
        if (op == 21 || op == 22) continue;
        const unsigned char code[]{static_cast<unsigned char>(op), 10, 0, 13};
        std::memcpy(program.data() + 56, code, sizeof(code));
        record.inputs = op >= 31 ? 1 : 2;
        std::memcpy(program.data() + 24, &record, sizeof(record));
        for (int a : {-7,0,3}) for (int b : {-7,0,3}) {
            if ((op == 19 || op == 20) && !b) continue;
            if (op >= 31 && a != 0) continue;
            const std::uint32_t arguments[]{encode(b), encode(a)};
            std::uint32_t predicted = 0;
            if (!pure_vm::evaluate(code, sizeof(code), arguments, record.inputs, predicted))
                return fail("arithmetic VM self test preflight failed", error, size);
            if (execute_constant(context, descriptor.data(), 0, predicted, error, size, arguments, record.inputs, true)) return 1;
            ++completed;
        }
    }
    return integer(emit, sink, completed);
}
int FDKSS_CALL native_math_self_test(void *context,const fdkss_value *,std::size_t count,
    fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    if(count || !variable_owner(context))return fail("native math VM self-test requires no arguments on the verified update thread",error,size);
    std::array<unsigned char,128> program{};std::array<unsigned char,88> descriptor{};
    const Header header{{'#','s','c','p'},24,1,112,0,0};
    const std::string name="FDKssNativeAbs";Function record{};
    record.code=56;record.inputs=1;record.input_args=96;record.name=0xc0000042;record.checksum=checksum(name);
    constexpr unsigned char code[]{36,0,39,1,1,4,13};
    std::memcpy(program.data(),&header,sizeof(header));std::memcpy(program.data()+24,&record,sizeof(record));
    std::memcpy(program.data()+56,code,sizeof(code));std::memcpy(program.data()+66,name.c_str(),name.size()+1);
    const auto put=[&](std::size_t at,const void *pointer){std::memcpy(descriptor.data()+at,&pointer,8);};
    put(0,program.data());put(0x30,program.data());put(0x38,program.data()+24);put(0x40,program.data()+112);
    const auto length=static_cast<std::uint32_t>(program.size());std::memcpy(descriptor.data()+8,&length,4);
    std::memcpy(descriptor.data()+12,"FDKssNativeMath",15);
    native_math::Context native_context{base(),context!=nullptr};
    const auto predict=+[](void *opaque,unsigned group,unsigned command,const std::uint32_t *args,unsigned count,std::uint32_t &out)->bool {
        return group==0 && command==39 && count==1 && args && native_math::preview(*static_cast<native_math::Context *>(opaque),args[0],out);
    };
    std::int64_t passed=0;
    const auto verify=[&](std::uint32_t argument)->bool {
        std::uint32_t result=0;
        return pure_vm::evaluate(code,sizeof(code),&argument,1,result,0,nullptr,&native_context,0,nullptr,nullptr,predict) &&
            !execute_constant(context,descriptor.data(),0,result,error,size,&argument,1,true,nullptr,false,nullptr,true);
    };
    for(const std::int32_t value : {-536870911,-19,-1,0,1,19,536870911}) {
        if(!verify((static_cast<std::uint32_t>(value)&0x3fffffff)|0x40000000))return fail("native integer absolute-value VM self-test failed",error,size);
        ++passed;
    }
    for(const float value : {-1.25f,-0.f,0.f,1.25f,-1234.12345f,std::numeric_limits<float>::max()}) {
        if(!verify((std::bit_cast<std::uint32_t>(value)>>2)|0x80000000))return fail("native float absolute-value VM self-test failed",error,size);
        ++passed;
    }
    return integer(emit,sink,passed);
}
int FDKSS_CALL float_self_test(void *context, const fdkss_value *, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!variable_owner(context)) return fail("VM execution requires the verified update thread", error, size);
    if (count) return fail("float VM self test takes no arguments", error, size);
    std::array<unsigned char, 128> program{};
    std::array<unsigned char, 88> descriptor{};
    const Header header{{'#','s','c','p'}, 24, 1, 112, 0, 0};
    const std::string name = "FDKssFloats";
    Function record{}; record.code = 56; record.input_args = 96;
    record.name = 0xc0000042; record.checksum = checksum(name);
    std::memcpy(program.data(), &header, sizeof(header));
    std::memcpy(program.data() + 66, name.c_str(), name.size() + 1);
    auto put_pointer = [&](std::size_t offset, const void *value) { std::memcpy(descriptor.data() + offset, &value, sizeof(value)); };
    put_pointer(0, program.data()); put_pointer(0x30, program.data());
    put_pointer(0x38, program.data() + 24); put_pointer(0x40, program.data() + 112);
    const std::uint32_t length = static_cast<std::uint32_t>(program.size());
    std::memcpy(descriptor.data() + 8, &length, sizeof(length));
    std::memcpy(descriptor.data() + 12, "FDKssFloatProbe", 16);
    auto encode = [](float value) { return (std::bit_cast<std::uint32_t>(value) >> 2) | 0x80000000; };
    const std::uint32_t values[]{encode(-7.f), encode(0.f), encode(1.5f), 0x40000003};
    std::int64_t completed = 0;
    for (unsigned op : {16u,17u,18u,19u,23u,24u,25u,26u,31u}) {
        const unsigned char code[]{static_cast<unsigned char>(op), 10, 0, 13};
        std::memcpy(program.data() + 56, code, sizeof(code));
        record.inputs = op == 31 ? 1 : 2;
        std::memcpy(program.data() + 24, &record, sizeof(record));
        for (auto a : values) for (auto b : values) {
            if (op == 19 && b == encode(0.f)) continue;
            if (op == 31 && a != encode(0.f)) continue;
            const std::uint32_t arguments[]{b, a};
            std::uint32_t predicted = 0;
            if (!pure_vm::evaluate(code, sizeof(code), arguments, record.inputs, predicted))
                return fail("float VM self test preflight failed", error, size);
            if (execute_constant(context, descriptor.data(), 0, predicted, error, size, arguments, record.inputs, true)) return 1;
            ++completed;
        }
    }
    return integer(emit, sink, completed);
}
int FDKSS_CALL branch_self_test(void *context, const fdkss_value *, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!variable_owner(context)) return fail("VM execution requires the verified update thread", error, size);
    if (count) return fail("branch VM self test takes no arguments", error, size);
    std::array<unsigned char, 256> program{};
    std::array<unsigned char, 88> descriptor{};
    const Header header{{'#','s','c','p'}, 24, 1, 160, 0, 0};
    const std::string name = "FDKssBranches";
    Function record{}; record.code = 56; record.inputs = 1; record.input_args = 128;
    record.name = 0xc0000060; record.checksum = checksum(name);
    std::memcpy(program.data(), &header, sizeof(header));
    std::memcpy(program.data() + 96, name.c_str(), name.size() + 1);
    auto put_pointer = [&](std::size_t offset, const void *value) { std::memcpy(descriptor.data() + offset, &value, sizeof(value)); };
    put_pointer(0, program.data()); put_pointer(0x30, program.data());
    put_pointer(0x38, program.data() + 24); put_pointer(0x40, program.data() + 160);
    const std::uint32_t length = static_cast<std::uint32_t>(program.size());
    std::memcpy(descriptor.data() + 8, &length, sizeof(length));
    std::memcpy(descriptor.data() + 12, "FDKssBranchProbe", 17);
    unsigned char code[]{14,70,0,0,0,0,4,1,0,0,0x40,10,0,13,0,4,2,0,0,0x40,10,0,13};
    std::memcpy(program.data() + 24, &record, sizeof(record));
    for (auto op : {14,15}) for (int condition : {-7,0,3}) {
        code[0] = static_cast<unsigned char>(op);
        std::memcpy(program.data() + 56, code, sizeof(code));
        const auto argument = (static_cast<std::uint32_t>(condition) & 0x3fffffff) | 0x40000000;
        const auto expected = (op == 14 ? condition != 0 : condition == 0) ? 0x40000002u : 0x40000001u;
        std::uint32_t predicted = 0;
        if (!pure_vm::evaluate(code, sizeof(code), &argument, 1, predicted, record.code) || predicted != expected)
            return fail("branch VM self test preflight failed", error, size);
        if (execute_constant(context, descriptor.data(), 0, predicted, error, size, &argument, 1, true)) return 1;
    }
    record.inputs = 0; std::memcpy(program.data() + 24, &record, sizeof(record));
    constexpr unsigned char jump[]{11,61,0,0,0,13};
    std::memcpy(program.data() + 56, jump, sizeof(jump));
    std::uint32_t predicted = 0;
    if (!pure_vm::evaluate(jump, sizeof(jump), nullptr, 0, predicted, record.code) || predicted != 0)
        return fail("jump VM self test preflight failed", error, size);
    if (execute_constant(context, descriptor.data(), 0, predicted, error, size, nullptr, 0, true)) return 1;
    return integer(emit, sink, 7);
}
namespace {
struct PureProbe {
    std::array<unsigned char, 256> program{};
    std::array<unsigned char, 88> descriptor{};
    Function record{};
    explicit PureProbe(const std::string &name, unsigned char inputs) {
        const Header header{{'#','s','c','p'}, 24, 1, 224, 0, 0};
        record.code = 56; record.inputs = inputs; record.input_args = 192;
        record.name = 0xc0000080; record.checksum = checksum(name);
        std::memcpy(program.data(), &header, sizeof(header));
        std::memcpy(program.data() + 24, &record, sizeof(record));
        std::memcpy(program.data() + 128, name.c_str(), name.size() + 1);
        auto put = [&](std::size_t offset, const void *value) { std::memcpy(descriptor.data() + offset, &value, sizeof(value)); };
        put(0, program.data()); put(0x30, program.data());
        put(0x38, program.data() + 24); put(0x40, program.data() + 224);
        const std::uint32_t length = static_cast<std::uint32_t>(program.size());
        std::memcpy(descriptor.data() + 8, &length, sizeof(length));
        std::memcpy(descriptor.data() + 12, name.c_str(), name.size() + 1);
    }
};
struct BudgetCommand {
    void **vtable;
    std::uint32_t remaining = 0, steps = 0, yields = 0;
    static void *__fastcall destroy(BudgetCommand *self, unsigned) { return self; }
    static bool __fastcall dispatch(BudgetCommand *self, void *thread, unsigned command) {
        auto *state = static_cast<unsigned char *>(thread) + 0x74;
        std::uint32_t flags = 0; std::memcpy(&flags, state, 4);
        if (command) flags = 0;
        else if (!self->remaining) { flags |= 4; ++self->yields; }
        else { --self->remaining; ++self->steps; flags &= ~4u; }
        std::memcpy(state, &flags, 4); return true;
    }
    static std::uint32_t __fastcall count(BudgetCommand *) { return 1; }
    static std::uint32_t __fastcall mask(BudgetCommand *, unsigned command) { return command ? 0 : 0xffffffff; }
};
bool private_timer_profile(const ExecutionProfile &profile, std::uintptr_t expected=0) {
    const bool second=profile.manager==0xc60e58;
    const std::uintptr_t handlers[]{second ? 0x4c4700u : 0x3d76b0u,second ? 0x4c4720u : 0x3d76d0u,second ? 0x4c4730u : 0x3d76e0u};
    constexpr unsigned char prefixes[][16]{
        {0xc6,0x82,0x15,0x04,0,0,1,0xc7,0x82,0x10,0x04,0,0,0,0,0},
        {0xc6,0x82,0x15,0x04,0,0,0,0xc3,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc},
        {0x83,0x7a,0x70,0,0x77,7,0xb8,0,0,0,0x40,0xeb,0x28,0x48,0x63,0x4a}};
    const auto manager=pointer(base()+(second ? 0xc60e58 : 0x8fefc8)), command=pointer(manager+0x20);
    if (!command || (expected && command!=expected) || pointer(command)!=base()+(second ? 0xb12a48 : 0x7fbea8)) return false;
    for (unsigned i=0;i<3;++i) {
        unsigned char code[16]{};
        if (pointer(command+8+(28+i)*24)!=base()+handlers[i] || !bytes(base()+handlers[i],code,16) || std::memcmp(code,prefixes[i],16))
            return false;
    }
    return true;
}
struct PersistentThread final : vm_jobs::Task {
    PureSnapshot snapshot;
    budget_vm::Image image;
    ExecutionProfile profile{};
    std::array<unsigned char,88> descriptor{};
    std::array<void *,4> vtable{reinterpret_cast<void *>(&BudgetCommand::destroy),reinterpret_cast<void *>(&BudgetCommand::dispatch),
        reinterpret_cast<void *>(&BudgetCommand::count),reinterpret_cast<void *>(&BudgetCommand::mask)};
    BudgetCommand budget{vtable.data()};
    std::array<std::uintptr_t,29> commands{};
    budget_vm::ThreadMethods private_vtable{};
    alignas(16) std::array<unsigned char,0x420> thread{};
    bool constructed = false, started = false, timer = false, timer_read = false;
    using Constructor = void *(__fastcall *)(void *,const char *);
    using Destructor = void *(__fastcall *)(void *,unsigned);
    using Start = bool (__fastcall *)(void *,void *,std::uint32_t,const std::uint32_t *,std::uint32_t);
    using Update = void (__fastcall *)(void *,float);
    ~PersistentThread() override { if (constructed) reinterpret_cast<Destructor>(base()+profile.destructor)(thread.data(),0); }
    void initialize() {
        auto put = [&](std::size_t at, std::uintptr_t value) { std::memcpy(descriptor.data()+at,&value,8); };
        Header header{}; std::memcpy(&header,image.data.data(),sizeof(header));
        put(0,reinterpret_cast<std::uintptr_t>(image.data.data())); put(0x30,reinterpret_cast<std::uintptr_t>(image.data.data()));
        put(0x38,reinterpret_cast<std::uintptr_t>(image.data.data()+header.functions));
        put(0x40,reinterpret_cast<std::uintptr_t>(image.data.data()+header.variables));
        put(0x48,reinterpret_cast<std::uintptr_t>(snapshot.globals.data()));
        const auto length=static_cast<std::uint32_t>(image.data.size()), globals=static_cast<std::uint32_t>(snapshot.globals.size());
        std::memcpy(descriptor.data()+8,&length,4); std::memcpy(descriptor.data()+0x50,&globals,4);
        std::memcpy(descriptor.data()+12,"FDKssAsync",11);
        reinterpret_cast<Constructor>(base()+profile.constructor)(thread.data(),"FDKssAsync"); constructed=true;
        // Private checkpoint dispatch must never invoke global Lua command hooks:
        // those observers could submit/cancel jobs while Queue::service iterates.
        // EXIT calls completion at slot six; error reporting uses slot seven.
        // Preserve the entire independently traced eight-method primary prefix.
        budget_vm::ThreadMethods primary{};
        if (!bytes(pointer(reinterpret_cast<std::uintptr_t>(thread.data())),primary.data(),primary.size()*8) ||
            !budget_vm::private_dispatch(primary,base()+profile.dispatch,private_vtable))
            throw std::runtime_error("async VM vtable unavailable");
        const auto private_table=reinterpret_cast<std::uintptr_t>(private_vtable.data());
        std::memcpy(thread.data(),&private_table,8);
        std::uintptr_t table=0; std::uint32_t capacity=0;
        std::memcpy(&table,thread.data()+0xb8,8); std::memcpy(&capacity,thread.data()+0xc0,4);
        if (timer_read) {
            std::uint32_t elapsed=0;unsigned char active=0;std::memcpy(&elapsed,thread.data()+0x410,4);std::memcpy(&active,thread.data()+0x415,1);
            if (elapsed!=0 || active) throw std::runtime_error("private timer initialization changed");
        }
        if (capacity!=28 || table!=pointer(base()+profile.manager)+0x20 || !bytes(table,commands.data(),28*8))
            throw std::runtime_error("async VM command table changed");
        commands[28]=reinterpret_cast<std::uintptr_t>(&budget); table=reinterpret_cast<std::uintptr_t>(commands.data()); capacity=29;
        std::memcpy(thread.data()+0xb8,&table,8); std::memcpy(thread.data()+0xc0,&capacity,4);
    }
    bool step(unsigned quantum, float delta) override {
        if (timer_read && !private_timer_read_profile(profile,commands[0])) throw std::runtime_error("private timer read profile changed during execution");
        if (timer && !private_timer_profile(profile,commands[0])) throw std::runtime_error("private wait command profile changed during execution");
        budget.remaining=quantum; const auto before=budget.steps;
        if (!started) {
            started=true;
            if (!reinterpret_cast<Start>(base()+profile.start)(thread.data(),descriptor.data(),snapshot.index,snapshot.arguments.data(),snapshot.count))
                throw std::runtime_error("async VM Start refused");
        } else reinterpret_cast<Update>(base()+profile.update)(thread.data(),timer ? delta : 0.f);
        if (budget.steps-before>quantum || budget.remaining>quantum) throw std::runtime_error("async VM exceeded instruction quantum");
        std::uint32_t flags=0, result=0; std::memcpy(&flags,thread.data()+0x74,4); std::memcpy(&result,thread.data()+0x14,4);
        if (!(flags&1)) {
            if (result!=snapshot.raw || !budget.steps) throw std::runtime_error("async VM result differs from preflight");
            return true;
        }
        std::uint32_t group=0, command=0;
        std::memcpy(&group,thread.data()+0x68,4); std::memcpy(&command,thread.data()+0x6c,4);
        if (!budget_vm::private_suspend(flags,group,command,budget.remaining,budget.steps-before,quantum,timer))
            throw std::runtime_error("async VM failed to suspend");
        return false;
    }
};
vm_jobs::Queue persistent_jobs;
struct JobContext { fdkss_runtime *runtime=nullptr; void *profile=nullptr; } job_context;
std::uint64_t submit_snapshot(PureSnapshot snapshot, unsigned quantum, std::uint64_t owner, bool timer=false, bool authored_wait=false) {
    auto task=std::make_unique<PersistentThread>(); task->snapshot=std::move(snapshot);
    task->timer=timer;
    char error[256]{}; std::string reason;
    if (!execution_profile(job_context.profile,task->profile,error,sizeof(error))) throw std::runtime_error(error);
    if (!budget_vm::build(task->snapshot.source,task->snapshot.index,task->image,reason)) throw std::runtime_error(reason);
    bool has_timer=false, has_timer_read=false;
    if (!timer && !budget_vm::pure_snapshot(task->snapshot.source,task->image,task->snapshot.globals,reason,authored_wait,&has_timer,true,&has_timer_read)) throw std::runtime_error(reason);
    task->timer=timer || has_timer;
    task->timer_read=task->snapshot.timer_read || has_timer_read;
    if (task->timer_read && (task->timer || !private_timer_read_profile(task->profile))) throw std::runtime_error("private timer read profile changed or mixed with waits");
    if (task->timer && !private_timer_profile(task->profile)) throw std::runtime_error("private wait command profile changed");
    const auto cost=task->image.data.capacity()+task->snapshot.globals.capacity()*4+task->snapshot.result.capacity()*2+sizeof(PersistentThread);
    if (!persistent_jobs.available(cost)) throw std::runtime_error("VM job capacity exceeded");
    task->snapshot.source.clear(); task->snapshot.source.shrink_to_fit();
    task->initialize(); const auto raw=task->snapshot.raw; auto result=task->snapshot.result;
    return persistent_jobs.submit(owner,quantum,cost,raw,std::move(result),std::move(task));
}
int FDKSS_CALL async_pure(void *, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    const auto caller=fdkss_native_caller(job_context.runtime);
    if (!caller || !variable_owner(job_context.profile)) return fail("async VM requires the calling mod on the owner thread",error,size);
    if (count<3 || args[0].type!=FDKSS_INTEGER || args[0].integer<1 || args[0].integer>128)
        return fail("expected instruction quantum 1..128, script, function and scalar arguments",error,size);
    if (!persistent_jobs.available(0)) return fail("VM job capacity exceeded",error,size);
    PureSnapshot snapshot;
    if (pure_call(job_context.profile,args+1,count-1,nullptr,nullptr,error,size,false,&snapshot)) return 1;
    try { return integer(emit,sink,static_cast<std::int64_t>(submit_snapshot(std::move(snapshot),static_cast<unsigned>(args[0].integer),caller))); }
    catch (const std::exception &e) { return fail(e.what(),error,size); }
}
int FDKSS_CALL async_wait(void *, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    const auto caller=fdkss_native_caller(job_context.runtime);
    if (!caller || !variable_owner(job_context.profile) || count!=2 ||
        (args[0].type!=FDKSS_NUMBER && args[0].type!=FDKSS_INTEGER) || args[1].type!=FDKSS_INTEGER || args[1].integer<1 || args[1].integer>128)
        return fail("expected wait seconds 0..30 and instruction quantum 1..128 on the owner thread",error,size);
    const double seconds=args[0].type==FDKSS_NUMBER ? args[0].number : static_cast<double>(args[0].integer);
    if (!std::isfinite(seconds) || seconds<0 || seconds>30) return fail("wait seconds must be finite and within 0..30",error,size);
    // All three handlers touch only this private Thread's timer/flags. Reject a
    // changed native table before constructing a task; no arbitrary code input.
    ExecutionProfile profile{};
    if (!execution_profile(job_context.profile,profile,error,size)) return 1;
    if (!private_timer_profile(profile)) return fail("private wait command profile changed",error,size);
    PureProbe probe("FDKssWait",0);
    // Start private timer, retain its argument through native wait retries,
    // discard it only after completion, stop timer, return nil and EXIT.
    unsigned char code[]{36,0,28,0,0,4,0,0,0,0,36,0,30,1,1,4,36,0,29,0,0,4,0,0,0,0,10,0,13};
    float duration=static_cast<float>(seconds);
    if (seconds>0) duration=std::max(duration,std::numeric_limits<float>::min());
    if (static_cast<double>(duration)<seconds) duration=std::nextafter(duration,std::numeric_limits<float>::infinity());
    const auto packed=((std::bit_cast<std::uint32_t>(duration)+3u)>>2)|0x80000000u;
    std::memcpy(code+6,&packed,4); std::memcpy(probe.program.data()+56,code,sizeof(code));
    PureSnapshot snapshot; snapshot.source.assign(probe.program.begin(),probe.program.end());
    try { return integer(emit,sink,static_cast<std::int64_t>(submit_snapshot(std::move(snapshot),static_cast<unsigned>(args[1].integer),caller,true))); }
    catch (const std::exception &e) { return fail(e.what(),error,size); }
}
int compiled_call(void *context,const fdkss_value *args,std::size_t count,fdkss_result_fn emit,void *sink,char *error,std::size_t size,bool execute) {
    if(!variable_owner(context) || count<2 || count>34 || args[0].type!=FDKSS_STRING ||
        !args[0].string || args[0].string_size<sizeof(Header) || args[0].string_size>1024*1024 || !variable_name(args[1]))
        return fail("expected compiled bytes (24 bytes..1 MiB), function and up to 32 scalar arguments on the update thread",error,size);
    Header header{};std::memcpy(&header,args[0].string,sizeof(header));
    const auto length=static_cast<std::uint32_t>(args[0].string_size);
    if(std::memcmp(header.magic,"#scp",4) || header.functions<sizeof(Header) || header.functions>length || !header.count || header.count>256 ||
        header.count>(length-header.functions)/sizeof(Function) || header.inputs || header.outputs || header.variables>length)
        return fail("compiled call requires bounded metadata and no shared variable declarations",error,size);
    std::array<unsigned char,88> descriptor{};const auto data=reinterpret_cast<std::uintptr_t>(args[0].string);
    const auto put=[&](std::size_t at,std::uintptr_t pointer){std::memcpy(descriptor.data()+at,&pointer,8);};
    put(0,data);put(0x30,data);put(0x38,data+header.functions);put(0x40,data+header.variables);
    std::memcpy(descriptor.data()+8,&length,4);
    Program program{"FDKssAuthored",data,reinterpret_cast<std::uintptr_t>(descriptor.data()),length,header};
    std::array<fdkss_value,34> call{};call[0].type=FDKSS_STRING;call[0].string="FDKssAuthored";call[0].string_size=12;
    for(std::size_t i=1;i<count;++i)call[i]=args[i];
    // Borrow bytes through synchronous Thread destruction. Local data is not an
    // engine-managed program and must not request a manager lease.
    return pure_call(context,call.data(),count,emit,sink,error,size,execute,nullptr,&program,false);
}
int FDKSS_CALL sync_compiled(void *context,const fdkss_value *args,std::size_t count,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    return compiled_call(context,args,count,emit,sink,error,size,true);
}
int FDKSS_CALL preview_compiled(void *context,const fdkss_value *args,std::size_t count,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    return compiled_call(context,args,count,emit,sink,error,size,false);
}
int FDKSS_CALL async_compiled(void *, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    const auto caller=fdkss_native_caller(job_context.runtime);
    if (!caller || !variable_owner(job_context.profile) || count<3 || count>35 || args[0].type!=FDKSS_STRING ||
        !args[0].string || args[0].string_size<sizeof(Header) || args[0].string_size>1024*1024 || !variable_name(args[1]) ||
        args[2].type!=FDKSS_INTEGER || args[2].integer<1 || args[2].integer>128)
        return fail("expected compiled bytes (24 bytes..1 MiB), function, quantum 1..128 and up to 32 scalar arguments",error,size);
    if (!persistent_jobs.available(0)) return fail("VM job capacity exceeded",error,size);
    // Lua's binary string is borrowed only within this call. Validate a local
    // descriptor and take an owned snapshot before returning the job identifier.
    Header header{}; std::memcpy(&header,args[0].string,sizeof(header));
    const auto length=static_cast<std::uint32_t>(args[0].string_size);
    if (std::memcmp(header.magic,"#scp",4) || header.functions<sizeof(Header) || header.functions>length || !header.count || header.count>256 ||
        header.count>(length-header.functions)/sizeof(Function) || header.inputs || header.outputs || header.variables>length)
        return fail("compiled job requires bounded metadata and no shared variable declarations",error,size);
    std::array<unsigned char,88> descriptor{};
    const auto data=reinterpret_cast<std::uintptr_t>(args[0].string);
    auto put=[&](std::size_t at,std::uintptr_t value){std::memcpy(descriptor.data()+at,&value,8);};
    put(0,data); put(0x30,data); put(0x38,data+header.functions); put(0x40,data+header.variables);
    std::memcpy(descriptor.data()+8,&length,4);
    Program program{"FDKssAuthored",data,reinterpret_cast<std::uintptr_t>(descriptor.data()),length,header};
    std::array<fdkss_value,34> call{};
    call[0].type=FDKSS_STRING; call[0].string="FDKssAuthored"; call[0].string_size=12;
    call[1]=args[1]; for (std::size_t i=3;i<count;++i) call[i-1]=args[i];
    PureSnapshot snapshot;
    if (pure_call(job_context.profile,call.data(),count-1,nullptr,nullptr,error,size,false,&snapshot,&program,true)) return 1;
    try { return integer(emit,sink,static_cast<std::int64_t>(submit_snapshot(std::move(snapshot),static_cast<unsigned>(args[2].integer),caller,false,true))); }
    catch (const std::exception &e) { return fail(e.what(),error,size); }
}
int FDKSS_CALL job_result(void *, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    const auto caller=fdkss_native_caller(job_context.runtime);
    if (!caller || !variable_owner(job_context.profile) || count!=1 || args[0].type!=FDKSS_INTEGER || args[0].integer<=0)
        return fail("expected an owned positive VM job identifier on the update thread",error,size);
    const auto *job=persistent_jobs.find(caller,static_cast<std::uint64_t>(args[0].integer));
    fdkss_value result{}; if (!job) return emit(sink,&result);
    if (string(emit,sink,vm_jobs::name(job->state))) return 1;
    if (job->state==vm_jobs::State::complete) {
        if ((job->raw>>30)==1) { result.type=FDKSS_INTEGER; result.integer=static_cast<std::int32_t>(job->raw<<2)>>2; }
        else if ((job->raw>>30)==2) { result.type=FDKSS_NUMBER; result.number=std::bit_cast<float>(job->raw<<2); }
        else if ((job->raw>>30)==3) { result.type=FDKSS_STRING; result.string=job->text.data(); result.string_size=job->text.size(); }
    }
    if (emit(sink,&result) || integer(emit,sink,job->slices)) return 1;
    return string(emit,sink,job->error);
}
int FDKSS_CALL job_cancel(void *, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    const auto caller=fdkss_native_caller(job_context.runtime);
    if (!caller || !variable_owner(job_context.profile) || count!=1 || args[0].type!=FDKSS_INTEGER || args[0].integer<=0)
        return fail("expected an owned positive VM job identifier on the update thread",error,size);
    fdkss_value result{}; result.type=FDKSS_BOOLEAN; result.boolean=persistent_jobs.cancel(caller,static_cast<std::uint64_t>(args[0].integer));
    return emit(sink,&result);
}
int FDKSS_CALL job_counts(void *, const fdkss_value *, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (count || !variable_owner(job_context.profile)) return fail("VM job counts require no arguments on the update thread",error,size);
    const auto stats=persistent_jobs.stats();
    if (integer(emit,sink,stats.active) || integer(emit,sink,stats.retained)) return 1;
    return integer(emit,sink,stats.bytes);
}
int FDKSS_CALL job_probe(void *, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    const auto caller=fdkss_native_caller(job_context.runtime);
    if (!caller || !variable_owner(job_context.profile) || count!=2 || args[0].type!=FDKSS_INTEGER || args[0].integer<1 || args[0].integer>128 ||
        args[1].type!=FDKSS_STRING || (text(args[1])!="sum" && text(args[1])!="infinite")) return fail("expected quantum 1..128 and sum/infinite diagnostic",error,size);
    PureProbe probe("FDKssAsyncProbe",1);
    const unsigned char loop[]{10,1,0,4,0,0,0,0x40,10,0,9,1,15,96,0,0,0,9,0,9,1,16,10,0,9,1,0,4,1,0,0,0x40,17,10,1,11,66,0,0,0,13};
    const unsigned char infinite[]{11,56,0,0,0};
    const bool forever=text(args[1])=="infinite";
    std::memcpy(probe.program.data()+56,forever ? infinite : loop,forever ? sizeof(infinite) : sizeof(loop));
    PureSnapshot snapshot; snapshot.source.assign(probe.program.begin(),probe.program.end()); snapshot.count=1;
    snapshot.arguments[0]=0x40000014; snapshot.raw=0x400000d2;
    try { return integer(emit,sink,static_cast<std::int64_t>(submit_snapshot(std::move(snapshot),static_cast<unsigned>(args[0].integer),caller))); }
    catch (const std::exception &e) { return fail(e.what(),error,size); }
}
int run_budget_probe(void *context, const unsigned char *source, std::size_t source_size,
    const std::uint32_t *arguments, std::uint32_t count, unsigned quantum,
    std::uint32_t expected_result, bool should_finish, char *error, std::size_t size) {
    ExecutionProfile profile{};
    if (!execution_profile(context, profile, error, size)) return 1;
    budget_vm::Image image; std::string reason;
    if (!budget_vm::build({source,source_size},0,image,reason)) return fail(reason.c_str(),error,size);
    std::array<unsigned char,88> descriptor{};
    auto put = [&](std::size_t at, std::uintptr_t value) { std::memcpy(descriptor.data() + at,&value,8); };
    put(0,reinterpret_cast<std::uintptr_t>(image.data.data())); put(0x30,reinterpret_cast<std::uintptr_t>(image.data.data()));
    put(0x38,reinterpret_cast<std::uintptr_t>(image.data.data() + 24));
    Header header{}; std::memcpy(&header,image.data.data(),sizeof(header));
    put(0x40,reinterpret_cast<std::uintptr_t>(image.data.data() + header.variables));
    const auto length = static_cast<std::uint32_t>(image.data.size());
    std::memcpy(descriptor.data() + 8,&length,4); std::memcpy(descriptor.data() + 12,"FDKssBudget",12);
    if (header.functions != 24 || header.inputs || header.outputs) return fail("budget probe requires local synthetic metadata",error,size);
    std::array<void *,4> vtable{reinterpret_cast<void *>(&BudgetCommand::destroy),reinterpret_cast<void *>(&BudgetCommand::dispatch),
        reinterpret_cast<void *>(&BudgetCommand::count),reinterpret_cast<void *>(&BudgetCommand::mask)};
    BudgetCommand budget{vtable.data()}; std::array<std::uintptr_t,29> commands{};
    alignas(16) std::array<unsigned char,0x420> thread{};
    using Constructor = void *(__fastcall *)(void *,const char *);
    using Destructor = void *(__fastcall *)(void *,unsigned);
    using Start = bool (__fastcall *)(void *,void *,std::uint32_t,const std::uint32_t *,std::uint32_t);
    using Update = void (__fastcall *)(void *,float);
    reinterpret_cast<Constructor>(base() + profile.constructor)(thread.data(),"FDKssBudget");
    struct Cleanup { void *thread; Destructor destroy; ~Cleanup() { destroy(thread,0); } } cleanup{thread.data(),reinterpret_cast<Destructor>(base() + profile.destructor)};
    std::uintptr_t table = 0; std::uint32_t capacity = 0;
    std::memcpy(&table,thread.data() + 0xb8,8); std::memcpy(&capacity,thread.data() + 0xc0,4);
    if (capacity != 28 || table != pointer(base() + profile.manager) + 0x20 || !bytes(table,commands.data(),28 * 8))
        return fail("budget probe command table changed",error,size);
    commands[28] = reinterpret_cast<std::uintptr_t>(&budget); table = reinterpret_cast<std::uintptr_t>(commands.data()); capacity = 29;
    std::memcpy(thread.data() + 0xb8,&table,8); std::memcpy(thread.data() + 0xc0,&capacity,4);
    bool finished = false;
    for (unsigned slice = 0; slice < (should_finish ? 2048u : 8u); ++slice) {
        budget.remaining = quantum; const auto before = budget.steps;
        if (!slice) {
            if (!reinterpret_cast<Start>(base() + profile.start)(thread.data(),descriptor.data(),0,arguments,count))
                return fail("budget probe Start refused",error,size);
        } else reinterpret_cast<Update>(base() + profile.update)(thread.data(),0.f);
        if (budget.steps - before > quantum || budget.remaining > quantum) return fail("budget probe exceeded its instruction quantum",error,size);
        std::uint32_t flags = 0, result = 0;
        std::memcpy(&flags,thread.data() + 0x74,4); std::memcpy(&result,thread.data() + 0x14,4);
        if (!(flags & 1)) {
            if (!should_finish || result != expected_result || !budget.steps) return fail("budget probe completion mismatch",error,size);
            finished = true; break;
        }
        if (!(flags & 4) || budget.remaining || budget.steps - before != quantum)
            return fail("budget probe failed to suspend at its instruction boundary",error,size);
    }
    if (finished != should_finish || (!should_finish && budget.steps != quantum * 8))
        return fail("budget probe frame limit mismatch",error,size);
    return 0;
}
}
int FDKSS_CALL call_compiled(void *context,const fdkss_value *args,std::size_t count,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    return sync_compiled(context,args,count,emit,sink,error,size);
}
int FDKSS_CALL preview_compiled_call(void *context,const fdkss_value *args,std::size_t count,fdkss_result_fn emit,void *sink,char *error,std::size_t size) {
    return preview_compiled(context,args,count,emit,sink,error,size);
}
void bind_jobs(fdkss_runtime *runtime, void *profile) {
    job_context={runtime,profile};
    fdkss_register_typed_native(runtime,"engine.script_pure_async",async_pure,nullptr);
    fdkss_register_typed_native(runtime,"engine.vm_wait",async_wait,nullptr);
    fdkss_register_typed_native(runtime,"engine.script_compiled_async",async_compiled,nullptr);
    fdkss_register_typed_native(runtime,"engine.script_compiled",call_compiled,profile);
    fdkss_register_typed_native(runtime,"engine.script_compiled_preview",preview_compiled_call,profile);
    fdkss_register_typed_native(runtime,"engine.vm_job_result",job_result,nullptr);
    fdkss_register_typed_native(runtime,"engine.vm_job_cancel",job_cancel,nullptr);
    fdkss_register_typed_native(runtime,"engine.vm_job_probe",job_probe,nullptr);
    fdkss_register_typed_native(runtime,"engine.vm_job_counts",job_counts,nullptr);
}
void service_jobs(fdkss_runtime *runtime, float delta) {
    if (runtime==job_context.runtime && variable_owner(job_context.profile))
        persistent_jobs.service([&](std::uint64_t identity){return fdkss_mod_alive(runtime,identity)!=0;},delta);
}
void stop_jobs() { persistent_jobs.clear(); job_context={}; }
int FDKSS_CALL loop_self_test(void *context, const fdkss_value *, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!variable_owner(context)) return fail("VM execution requires the verified update thread", error, size);
    if (count) return fail("loop VM self test takes no arguments", error, size);
    PureProbe probe("FDKssLoop", 1);
    const unsigned char code[]{10,1,0,4,0,0,0,0x40,10,0,
        9,1,15,96,0,0,0,9,0,9,1,16,10,0,9,1,0,4,1,0,0,0x40,17,10,1,11,66,0,0,0,13};
    std::memcpy(probe.program.data() + probe.record.code, code, sizeof(code));
    for (int n : {0,1,3,7,11}) {
        const auto argument = 0x40000000u | static_cast<std::uint32_t>(n);
        const auto expected = 0x40000000u | static_cast<std::uint32_t>(n * (n + 1) / 2);
        std::uint32_t predicted = 0;
        if (!pure_vm::evaluate(code, sizeof(code), &argument, 1, predicted, probe.record.code) || predicted != expected)
            return fail("loop VM self test preflight failed", error, size);
        if (execute_constant(context, probe.descriptor.data(), 0, predicted, error, size, &argument, 1, true)) return 1;
    }
    return integer(emit, sink, 5);
}
int FDKSS_CALL nested_self_test(void *context, const fdkss_value *, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!variable_owner(context)) return fail("VM execution requires the verified update thread", error, size);
    if (count) return fail("nested VM self test takes no arguments", error, size);
    std::array<unsigned char, 768> storage{};
    std::array<unsigned char, 88> descriptor{};
    const Header header{{'#','s','c','p'}, 24, 3, 704, 0, 0};
    std::memcpy(storage.data(), &header, sizeof(header));
    const char *names[]{"FDKssNested", "Child", "Leaf"};
    for (std::uint32_t index = 0; index < 3; ++index) {
        Function record{}; record.code = 128 + index * 128;
        record.inputs = index < 2 ? 1 : 0; record.input_args = 640;
        const auto name_offset = 512 + index * 16;
        record.name = 0xc0000000u | name_offset; record.checksum = checksum(names[index]);
        std::memcpy(storage.data() + 24 + index * 32, &record, sizeof(record));
        std::memcpy(storage.data() + name_offset, names[index], std::strlen(names[index]) + 1);
    }
    const unsigned char root[]{10,4,0,4,0,0,0,0,0,4,147,0,0,0,9,4,12,1,0,
        9,0,0,4,2,0,0,0x40,16,10,0,13};
    const unsigned char child[]{10,1,0,4,1,0,0,0,0,4,17,1,0,0,12,2,0,
        9,1,9,0,16,10,0,13};
    const unsigned char leaf[]{0,4,5,0,0,0x40,10,0,13};
    std::memcpy(storage.data() + 128, root, sizeof(root));
    std::memcpy(storage.data() + 256, child, sizeof(child));
    std::memcpy(storage.data() + 384, leaf, sizeof(leaf));
    auto put = [&](std::size_t offset, const void *value) { std::memcpy(descriptor.data() + offset, &value, sizeof(value)); };
    put(0, storage.data()); put(0x30, storage.data()); put(0x38, storage.data() + 24); put(0x40, storage.data() + 704);
    const auto length = static_cast<std::uint32_t>(storage.size());
    std::memcpy(descriptor.data() + 8, &length, sizeof(length));
    std::memcpy(descriptor.data() + 12, "FDKssNested", 12);
    Program program{"FDKssNested", reinterpret_cast<std::uintptr_t>(storage.data()), reinterpret_cast<std::uintptr_t>(descriptor.data()), length, header};
    const std::uint32_t arguments[]{0x7ffffff9u,0x40000000u,0x4000002au,
        (std::bit_cast<std::uint32_t>(1.5f) >> 2) | 0x80000000u,
        (std::bit_cast<std::uint32_t>(-7.f) >> 2) | 0x80000000u};
    std::int64_t completed = 0;
    for (auto argument : arguments) {
        CallGraph graph{program}; std::uint32_t predicted = 0;
        if (!pure_vm::evaluate(root, sizeof(root), &argument, 1, predicted, 128, CallGraph::resolve, &graph))
            return fail("nested VM self test preflight failed", error, size);
        if (execute_constant(context, descriptor.data(), 0, predicted, error, size, &argument, 1, true)) return 1;
        ++completed;
    }
    // A nested identity also transports nil and finite floats without arithmetic.
    std::array<unsigned char, 20> identity{};
    std::memcpy(identity.data(), root, 19); identity[19] = 13;
    const unsigned char identity_child[]{10,0,13};
    std::memcpy(storage.data() + 128, identity.data(), identity.size());
    std::memcpy(storage.data() + 256, identity_child, sizeof(identity_child));
    for (auto argument : {0u, arguments[3]}) {
        CallGraph graph{program}; std::uint32_t predicted = 0;
        if (!pure_vm::evaluate(identity.data(), identity.size(), &argument, 1, predicted, 128, CallGraph::resolve, &graph) || predicted != argument)
            return fail("nested identity self test preflight failed", error, size);
        if (execute_constant(context, descriptor.data(), 0, predicted, error, size, &argument, 1, true)) return 1;
        ++completed;
    }
    // Known diagnostic-only flags and bounded extra records do not alter calls.
    std::memcpy(storage.data() + 128, root, sizeof(root));
    std::memcpy(storage.data() + 256, child, sizeof(child));
    for (std::uint32_t index = 0; index < 3; ++index) {
        Function record{};
        std::memcpy(&record, storage.data() + 24 + index * 32, sizeof(record));
        record.flags0 = 1; record.structs = 1; record.struct_data = 680;
        std::memcpy(storage.data() + 24 + index * 32, &record, sizeof(record));
    }
    for (auto argument : {arguments[2], arguments[3]}) {
        CallGraph graph{program}; std::uint32_t predicted = 0;
        if (!pure_vm::evaluate(root, sizeof(root), &argument, 1, predicted, 128, CallGraph::resolve, &graph))
            return fail("nested metadata self test preflight failed", error, size);
        if (execute_constant(context, descriptor.data(), 0, predicted, error, size, &argument, 1, true)) return 1;
        ++completed;
    }
    return integer(emit, sink, completed);
}
int FDKSS_CALL budget_self_test(void *context, const fdkss_value *, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!variable_owner(context)) return fail("VM execution requires the verified update thread",error,size);
    if (count) return fail("budget VM self test takes no arguments",error,size);
    PureProbe probe("FDKssBudget",1);
    const unsigned char loop[]{10,1,0,4,0,0,0,0x40,10,0,
        9,1,15,96,0,0,0,9,0,9,1,16,10,0,9,1,0,4,1,0,0,0x40,17,10,1,11,66,0,0,0,13};
    std::memcpy(probe.program.data() + 56,loop,sizeof(loop));
    const std::uint32_t argument = 0x40000014u;
    std::int64_t completed = 0;
    for (unsigned quantum : {1u,7u,16u,128u}) {
        if (run_budget_probe(context,probe.program.data(),probe.program.size(),&argument,1,quantum,0x400000d2u,true,error,size)) return 1;
        ++completed;
    }
    probe.program[28] = 0; // No arguments on the private timer and infinite-loop cases.
    const bool second = context != nullptr;
    const unsigned char timer[]{36,0,31,0,9,0,10,0,13};
    const auto manager = pointer(base() + (second ? 0xc60e58 : 0x8fefc8));
    const auto command = pointer(manager + 0x20);
    if (pointer(command + 8 + 31 * 24) != base() + (second ? 0x4c47a0 : 0x3d7750))
        return fail("private timer command profile changed",error,size);
    std::memcpy(probe.program.data() + 56,timer,sizeof(timer));
    for (unsigned quantum : {1u,7u,16u,128u}) {
        if (run_budget_probe(context,probe.program.data(),probe.program.size(),nullptr,0,quantum,0x80000000u,true,error,size)) return 1;
        ++completed;
    }
    const unsigned char infinite[]{11,56,0,0,0};
    std::memcpy(probe.program.data() + 56,infinite,sizeof(infinite));
    for (unsigned quantum : {1u,7u,16u,128u}) {
        if (run_budget_probe(context,probe.program.data(),probe.program.size(),nullptr,0,quantum,0,false,error,size)) return 1;
        ++completed;
    }
    std::array<unsigned char,768> nested{};
    const Header header{{'#','s','c','p'},24,2,704,0,0}; std::memcpy(nested.data(),&header,sizeof(header));
    for (std::uint32_t index = 0; index < 2; ++index) {
        Function record{}; record.code = 128 + index * 128; record.input_args = 640;
        const char *name = index ? "Child" : "FDKssBudget"; const auto offset = 512 + index * 16;
        record.name = 0xc0000000u | offset; record.checksum = checksum(name);
        std::memcpy(nested.data() + 24 + index * 32,&record,sizeof(record));
        std::memcpy(nested.data() + offset,name,std::strlen(name) + 1);
    }
    const unsigned char caller[]{0,4,0,0,0,0,0,4,143,0,0,0,12,1,0,9,0,0,4,2,0,0,0x40,16,10,0,13};
    const unsigned char child[]{0,4,5,0,0,0x40,10,0,13};
    std::memcpy(nested.data() + 128,caller,sizeof(caller)); std::memcpy(nested.data() + 256,child,sizeof(child));
    for (unsigned quantum : {1u,7u,16u,128u}) {
        if (run_budget_probe(context,nested.data(),nested.size(),nullptr,0,quantum,0x40000007u,true,error,size)) return 1;
        ++completed;
    }
    return integer(emit,sink,completed);
}
int FDKSS_CALL lease_self_test(void *context, const fdkss_value *, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!variable_owner(context)) return fail("VM execution requires the verified update thread", error, size);
    if (count) return fail("lease VM self test takes no arguments", error, size);
    const auto programs = catalog(context);
    if (programs.empty()) return fail("no loaded script for lease self test", error, size);
    const auto &program = programs.front();
    const auto manager = pointer(base() + (context ? 0xc60e58 : 0x8fefc8));
    const auto current = ProgramLease::entry(manager, program.descriptor);
    std::int32_t before = 0;
    if (!current || !read(current + 0x40, before)) return fail("lease self test entry unavailable", error, size);
    for (int test = 0; test < 4; ++test) {
        {
            ProgramLease outer, inner;
            if (!outer.acquire(context, program) || !inner.acquire(context, program)) return fail("lease self test acquire failed", error, size);
            std::int32_t held = 0;
            if (!read(ProgramLease::entry(manager, program.descriptor) + 0x40, held) || held != before + 2)
                return fail("lease self test reference count mismatch", error, size);
        }
        std::int32_t after = 0;
        if (!read(ProgramLease::entry(manager, program.descriptor) + 0x40, after) || after != before || pointer(program.descriptor) != program.data)
            return fail("lease self test release mismatch", error, size);
    }
    return integer(emit, sink, 4);
}
int FDKSS_CALL global_self_test(void *context, const fdkss_value *, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!variable_owner(context)) return fail("VM execution requires the verified update thread", error, size);
    if (count) return fail("global VM self test takes no arguments", error, size);
    PureProbe probe("FDKssGlobal", 0);
    Header header{}; std::memcpy(&header, probe.program.data(), sizeof(header)); header.inputs = 2;
    std::memcpy(probe.program.data(), &header, sizeof(header));
    std::array<std::uint32_t, 2> values{};
    const auto address = reinterpret_cast<std::uintptr_t>(values.data());
    const std::uint32_t total = 2;
    std::memcpy(probe.descriptor.data() + 0x48, &address, sizeof(address));
    std::memcpy(probe.descriptor.data() + 0x50, &total, sizeof(total));
    Program program{"FDKssGlobal", reinterpret_cast<std::uintptr_t>(probe.program.data()),
        reinterpret_cast<std::uintptr_t>(probe.descriptor.data()), static_cast<std::uint32_t>(probe.program.size()), header};
    const unsigned char code[]{7,0,0,0,0,10,0,13};
    std::memcpy(probe.program.data() + 56, code, sizeof(code));
    const std::uint32_t samples[]{0u,0x40000000u,0x7ffffff9u,0x5fffffffu,
        (std::bit_cast<std::uint32_t>(1.5f) >> 2) | 0x80000000u};
    std::int64_t completed = 0;
    for (auto value : samples) {
        values[0] = value; CallGraph graph{program}; std::uint32_t predicted = 0;
        if (!pure_vm::evaluate(code, sizeof(code), nullptr, 0, predicted, 56, CallGraph::resolve, &graph, 0, CallGraph::read_global) ||
            predicted != value || !graph.globals_unchanged()) return fail("global VM self test preflight failed", error, size);
        if (execute_constant(context, probe.descriptor.data(), 0, predicted, error, size, nullptr, 0, true)) return 1;
        ++completed;
        values[0] ^= 1;
        if (graph.globals_unchanged()) return fail("global VM self test missed a changed value", error, size);
    }
    // Both global slots participate in arithmetic, without changing either one.
    const unsigned char add[]{7,0,0,0,0,7,1,0,0,0,16,10,0,13};
    std::memcpy(probe.program.data() + 56, add, sizeof(add));
    values = {0x4000002au,0x7ffffff9u}; CallGraph graph{program}; std::uint32_t predicted = 0;
    if (!pure_vm::evaluate(add, sizeof(add), nullptr, 0, predicted, 56, CallGraph::resolve, &graph, 0, CallGraph::read_global) ||
        predicted != 0x40000023u || !graph.globals_unchanged()) return fail("global arithmetic preflight failed", error, size);
    if (execute_constant(context, probe.descriptor.data(), 0, predicted, error, size, nullptr, 0, true)) return 1;
    if (values != std::array<std::uint32_t,2>{0x4000002au,0x7ffffff9u}) return fail("global read changed storage", error, size);
    return integer(emit, sink, completed + 1);
}
int FDKSS_CALL comparison_self_test(void *context, const fdkss_value *, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!variable_owner(context)) return fail("VM execution requires the verified update thread", error, size);
    if (count) return fail("comparison VM self test takes no arguments", error, size);
    PureProbe probe("FDKssCompare", 2);
    auto encode = [](int value) { return (static_cast<std::uint32_t>(value) & 0x3fffffff) | 0x40000000; };
    auto real = [](float value) { return (std::bit_cast<std::uint32_t>(value) >> 2) | 0x80000000; };
    const std::uint32_t values[]{encode(-7), encode(0), encode(3), real(-7.f), real(0.f), real(3.f),
        real(1.5f), encode(16777217), real(16777216.f)};
    std::int64_t completed = 0;
    for (auto op : {21,22}) {
        const unsigned char code[]{static_cast<unsigned char>(op),10,0,13};
        std::memcpy(probe.program.data() + probe.record.code, code, sizeof(code));
        for (auto left : values) for (auto right : values) {
            const std::uint32_t arguments[]{right, left};
            std::uint32_t predicted = 0;
            if (!pure_vm::evaluate(code, sizeof(code), arguments, 2, predicted))
                return fail("comparison VM self test preflight failed", error, size);
            if (execute_constant(context, probe.descriptor.data(), 0, predicted, error, size, arguments, 2, true)) return 1;
            ++completed;
        }
    }
    return integer(emit, sink, completed);
}
int FDKSS_CALL default_self_test(void *context, const fdkss_value *, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!variable_owner(context)) return fail("VM execution requires the verified update thread", error, size);
    if (count) return fail("default VM self test takes no arguments", error, size);
    std::array<unsigned char, 256> program{};
    std::array<unsigned char, 88> descriptor{};
    const Header header{{'#','s','c','p'}, 24, 1, 160, 0, 0};
    const std::string name = "FDKssDefaults";
    Function record{}; record.code = 56; record.inputs = 3; record.outputs = 2;
    record.input_args = 128; record.output_args = 144; record.name = 0xc0000060; record.checksum = checksum(name);
    std::memcpy(program.data(), &header, sizeof(header));
    std::memcpy(program.data() + 24, &record, sizeof(record));
    std::memcpy(program.data() + 96, name.c_str(), name.size() + 1);
    const std::uint32_t defaults[]{0x4000002a,0x8ff00000};
    std::memcpy(program.data() + 144, defaults, sizeof(defaults));
    auto put_pointer = [&](std::size_t offset, const void *value) { std::memcpy(descriptor.data() + offset, &value, sizeof(value)); };
    put_pointer(0, program.data()); put_pointer(0x30, program.data());
    put_pointer(0x38, program.data() + 24); put_pointer(0x40, program.data() + 160);
    const std::uint32_t length = static_cast<std::uint32_t>(program.size());
    std::memcpy(descriptor.data() + 8, &length, sizeof(length));
    std::memcpy(descriptor.data() + 12, "FDKssDefaultProbe", 18);
    const std::array<unsigned char, 8> bodies[]{ {{10,0,1,8,13}}, {{10,1,10,0,1,4,13}}, {{1,8,10,0,13}} };
    const std::uint32_t arguments[]{0x7ffffff9,0x40000003,0};
    for (std::uint32_t supplied = 1; supplied <= 3; ++supplied) {
        std::array<std::uint32_t, 3> effective{};
        std::copy(arguments, arguments + supplied, effective.begin());
        for (auto i = supplied; i < 3; ++i) effective[i] = defaults[i - 1];
        for (unsigned selected = 0; selected < 3; ++selected) {
            const auto &code = bodies[selected];
            std::memcpy(program.data() + 56, code.data(), code.size());
            std::uint32_t predicted = 0;
            if (!pure_vm::evaluate(code.data(), code.size(), effective.data(), 3, predicted) || predicted != effective[selected])
                return fail("default VM self test preflight failed", error, size);
            if (execute_constant(context, descriptor.data(), 0, predicted, error, size, arguments, supplied, true)) return 1;
        }
    }
    return integer(emit, sink, 9);
}
int FDKSS_CALL pure_self_test(void *context, const fdkss_value *, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (!variable_owner(context)) return fail("VM execution requires the verified update thread", error, size);
    if (count) return fail("pure VM self test takes no arguments", error, size);
    std::array<unsigned char, 256> program{};
    std::array<unsigned char, 88> descriptor{};
    const Header header{{'#','s','c','p'}, 24, 1, 160, 0, 0};
    const std::string name = "FDKssArguments";
    Function record{}; record.code = 56; record.inputs = 2; record.input_args = 128;
    record.name = 0xc0000060; record.checksum = checksum(name);
    std::memcpy(program.data(), &header, sizeof(header));
    std::memcpy(program.data() + 24, &record, sizeof(record));
    std::memcpy(program.data() + 96, name.c_str(), name.size() + 1);
    auto put_pointer = [&](std::size_t offset, const void *value) { std::memcpy(descriptor.data() + offset, &value, sizeof(value)); };
    put_pointer(0, program.data()); put_pointer(0x30, program.data());
    put_pointer(0x38, program.data() + 24); put_pointer(0x40, program.data() + 160);
    const std::uint32_t length = static_cast<std::uint32_t>(program.size());
    std::memcpy(descriptor.data() + 8, &length, sizeof(length));
    std::memcpy(descriptor.data() + 12, "FDKssPureProbe", 15);
    struct Case { std::array<unsigned char, 16> code; std::array<std::uint32_t, 2> args; std::uint32_t expected; };
    const Case cases[]{
        {{{10,0,1,4,13}}, {{0x4000002a,0x7ffffff9}}, 0x4000002a},
        {{{10,1,10,0,13}}, {{0x4000002a,0x7ffffff9}}, 0x7ffffff9},
        {{{2,0xfc,0xff,0xff,0xff,10,3,1,8,9,3,10,0,13}}, {{0x8fe00000,0}}, 0x8fe00000},
        {{{10,0,1,4,13}}, {{0,0x40000001}}, 0}
    };
    for (const auto &test : cases) {
        std::uint32_t predicted = 0;
        if (!pure_vm::evaluate(test.code.data(), test.code.size(), test.args.data(), 2, predicted) || predicted != test.expected)
            return fail("pure VM self test preflight failed", error, size);
        std::memcpy(program.data() + 56, test.code.data(), test.code.size());
        if (execute_constant(context, descriptor.data(), 0, predicted, error, size, test.args.data(), 2, true)) return 1;
    }
    return integer(emit, sink, std::size(cases));
}
int FDKSS_CALL self_test(void *context, const fdkss_value *, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t size) {
    if (count) return fail("VM self test takes no arguments", error, size);
    std::array<unsigned char, 128> program{};
    std::array<unsigned char, 88> descriptor{};
    const Header header{{'#','s','c','p'}, 24, 1, 96, 0, 0};
    const std::string name = "FDKssConstant";
    Function record{}; record.code = 56; record.name = 0xc0000042; record.checksum = checksum(name);
    constexpr unsigned char code[]{0x00, 0x04, 0x2a, 0x00, 0x00, 0x40, 0x0a, 0x00, 0x0d};
    std::memcpy(program.data(), &header, sizeof(header));
    std::memcpy(program.data() + 24, &record, sizeof(record));
    std::memcpy(program.data() + 56, code, sizeof(code));
    std::memcpy(program.data() + 66, name.c_str(), name.size() + 1);
    auto put_pointer = [&](std::size_t offset, const void *value) { std::memcpy(descriptor.data() + offset, &value, sizeof(value)); };
    put_pointer(0, program.data()); put_pointer(0x30, program.data());
    put_pointer(0x38, program.data() + 24); put_pointer(0x40, program.data() + 96);
    const std::uint32_t bytes = static_cast<std::uint32_t>(program.size());
    std::memcpy(descriptor.data() + 8, &bytes, sizeof(bytes));
    std::memcpy(descriptor.data() + 12, "FDKssProbe", 11);
    if (execute_constant(context, descriptor.data(), 0, 0x4000002a, error, size)) return 1;
    return integer(emit, sink, 42);
}
}
