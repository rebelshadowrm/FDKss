#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "actor_methods.h"
#include "actor_hooks.h"
#include "../actor_types.h"
#include "../engine_member_copy.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
namespace {
template<class T> bool read(std::uintptr_t address, T &value) {
    SIZE_T count = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(address), &value, sizeof(value), &count) && count == sizeof(value);
}
int fail(const char *text, char *error, std::size_t capacity) { std::snprintf(error, capacity, "%s", text); return 1; }
bool executable(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION region{};
    return VirtualQuery(reinterpret_cast<void *>(address), &region, sizeof(region)) && region.State == MEM_COMMIT &&
        !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        (region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

}

namespace actor_methods {
namespace {
bool writable(std::uintptr_t address) {
    MEMORY_BASIC_INFORMATION region{};
    return VirtualQuery(reinterpret_cast<void *>(address),&region,sizeof(region)) && region.State==MEM_COMMIT &&
        !(region.Protect&(PAGE_GUARD|PAGE_NOACCESS)) &&
        (region.Protect&(PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY));
}
}
int state_byte(void *actor,std::uintptr_t base,bool sky2,const fdkss_value *args,std::size_t count,
    fdkss_result_fn emit,void *sink,char *error,std::size_t capacity) {
    if(count!=1 || !args)return fail("native state byte setter expects one unsigned byte",error,capacity);
    double value=0;
    if(args[0].type==FDKSS_INTEGER)value=static_cast<double>(args[0].integer);
    else if(args[0].type==FDKSS_NUMBER)value=args[0].number;
    else return fail("native state byte requires an integer within 0..255",error,capacity);
    if(!std::isfinite(value) || value<0 || value>255 || std::trunc(value)!=value)
        return fail("native state byte requires an integer within 0..255",error,capacity);
    std::uintptr_t table=0,target=0;
    const auto expected_table=base+(sky2 ? 0xafdec8 : 0x7eb7d0);
    const auto setter=base+(sky2 ? 0x5ab260 : 0x480b70);const auto slot=sky2 ? 18u : 17u;
    constexpr std::array<unsigned char,7> expected{0x88,0x91,0x6c,0x01,0,0,0xc3};std::array<unsigned char,7> code{};
    if(!actor || !base || !read(reinterpret_cast<std::uintptr_t>(actor),table) || table!=expected_table ||
        !read(table+slot*8,target) || target!=setter || !executable(setter) || !read(setter,code) || code!=expected)
        return fail("character native state byte setter unavailable or changed",error,capacity);
    if(!writable(reinterpret_cast<std::uintptr_t>(actor)+0x16c))return fail("character native state byte is not writable",error,capacity);
    using Setter=void (__fastcall *)(void *,unsigned char);
    const auto byte=static_cast<unsigned char>(value);reinterpret_cast<Setter>(setter)(actor,byte);
    unsigned char stored=0;
    if(!read(reinterpret_cast<std::uintptr_t>(actor)+0x16c,stored) || stored!=byte)
        return fail("character native state byte result could not be verified",error,capacity);
    fdkss_value result{};result.type=FDKSS_INTEGER;result.integer=stored;return emit(sink,&result);
}
int type(void *actor, std::uintptr_t base, bool sky2, std::string_view method,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    std::uintptr_t table = 0, target = 0;
    if (!actor || !base || !read(reinterpret_cast<std::uintptr_t>(actor), table)) return fail("actor type getter unavailable", error, capacity);
    const actor_types::Profile *profile = nullptr;
    const auto *types = sky2 ? actor_types::second : actor_types::first;
    const auto count = sky2 ? std::size(actor_types::second) : std::size(actor_types::first);
    for (std::size_t i = 0; i < count; ++i) if (table == base + types[i].table) profile = &types[i];
    if (!profile) return fail("actor class has no verified type getter", error, capacity);
    const bool name = method == "type_name", id = method == "type_id";
    if (!name && !id && method != "type_flags") return fail("unknown actor type method", error, capacity);
    const auto slot = name ? 1 : id ? 2 : 3;
    const auto getter = base + (name ? profile->name : id ? profile->id : profile->flags);
    if (!read(table + slot * sizeof(void *), target) || (target != getter && !actor_hooks::owns(reinterpret_cast<void **>(table + slot * sizeof(void *)), reinterpret_cast<void *>(getter))) || !executable(getter))
        return fail("actor type virtual slot unavailable or changed", error, capacity);
    if (name) {
        std::array<unsigned char, 8> expected{0x48,0x8d,0x05,0,0,0,0,0xc3}, actual{};
        const auto displacement = static_cast<std::int32_t>(profile->text - profile->name - 7);
        std::memcpy(expected.data() + 3, &displacement, sizeof(displacement));
        if (!read(getter, actual) || actual != expected) return fail("actor type name getter bytes changed", error, capacity);
        using Getter = const char *(__fastcall *)(void *);
        const auto text = reinterpret_cast<Getter>(getter)(actor);
        std::array<char, 128> value{};
        if (reinterpret_cast<std::uintptr_t>(text) != base + profile->text || !read(base + profile->text, value) ||
            !std::memchr(value.data(), 0, value.size())) return fail("actor type name could not be copied", error, capacity);
        fdkss_value result{}; result.type = FDKSS_STRING; result.string = value.data(); result.string_size = std::strlen(value.data());
        return emit(sink, &result);
    }
    const auto expected_value = id ? profile->expected_id : profile->expected_flags;
    // The pinned zero-flags routine uses xor eax,eax; all others use mov eax,imm32.
    if (!id && expected_value == 0) {
        std::array<unsigned char, 3> actual{};
        if (!read(getter, actual) || actual != std::array<unsigned char, 3>{0x33,0xc0,0xc3})
            return fail("actor type numeric getter bytes changed", error, capacity);
    } else {
        std::array<unsigned char, 6> expected{0xb8,0,0,0,0,0xc3}, actual{};
        std::memcpy(expected.data() + 1, &expected_value, sizeof(expected_value));
        if (!read(getter, actual) || actual != expected) return fail("actor type numeric getter bytes changed", error, capacity);
    }
    using Getter = std::uint32_t (__fastcall *)(void *);
    const auto value = reinterpret_cast<Getter>(getter)(actor);
    if (value != expected_value) return fail("actor type numeric result changed", error, capacity);
    fdkss_value result{}; result.type = FDKSS_INTEGER; result.integer = value; return emit(sink, &result);
}
int transform(void *actor, std::uintptr_t getter, fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    std::array<float, 16> matrix{};
    // The only production contract remains the independently accepted Sky block.
    // Contrast-build discovery does not install profiles or authorize receivers.
    constexpr engine_members::FloatBlockProfile profile{6, 0x58, 0};
    const auto status = engine_members::copy_floats(reinterpret_cast<std::uintptr_t>(actor), getter, profile, matrix,
        [](std::uintptr_t at, void *destination, std::size_t size) {
            SIZE_T count = 0;
            return at && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(at), destination, size, &count) && count == size;
        }, executable,
        [](std::uintptr_t slot, std::uintptr_t original) {
            return actor_hooks::owns(reinterpret_cast<void **>(slot), reinterpret_cast<void *>(original));
        }, [](std::uintptr_t original, std::uintptr_t receiver) {
            using Getter = const float *(__fastcall *)(void *);
            return reinterpret_cast<std::uintptr_t>(reinterpret_cast<Getter>(original)(reinterpret_cast<void *>(receiver)));
        });
    if (status == engine_members::CopyStatus::getter_changed)
        return fail("actor transform getter unavailable or changed", error, capacity);
    if (status == engine_members::CopyStatus::copy_failed)
        return fail("actor transform could not be copied", error, capacity);
    if (status == engine_members::CopyStatus::nonfinite)
        return fail("actor transform contains a nonfinite value", error, capacity);
    for (const auto number : matrix) {
        fdkss_value value{}; value.type = FDKSS_NUMBER; value.number = number;
        if (emit(sink, &value)) return 1;
    }
    return 0;
}
}
