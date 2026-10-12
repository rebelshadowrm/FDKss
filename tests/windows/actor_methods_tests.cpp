#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "actor_methods.h"
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
#include <string>
#include <cstdio>
namespace {
void check(bool value) { if (!value) throw std::runtime_error("actor transform assertion failed"); }
int emit(void *sink, const fdkss_value *value) {
    check(value->type == FDKSS_NUMBER);
    static_cast<std::vector<double> *>(sink)->push_back(value->number); return 0;
}
struct TypeProfile { std::uintptr_t table, name, id, flags, text; std::uint32_t ident, bits; };
#define FDKSS_ACTOR_TYPE(table, name, id, flags, text, ident, bits, class_name) {table,name,id,flags,text,ident,bits},
constexpr TypeProfile first[]{
#include "fdkss/sky1_actor_types.inc"
}, second[]{
#include "fdkss/sky2_actor_types.inc"
};
#undef FDKSS_ACTOR_TYPE
struct Result { fdkss_value value{}; std::string text; int count = 0; };
int capture(void *sink, const fdkss_value *value) {
    auto &result = *static_cast<Result *>(sink); result.value = *value; ++result.count;
    if (value->type == FDKSS_STRING) result.text.assign(value->string, value->string_size);
    return 0;
}
void test_types() {
    const auto release = [](void *p) { if (p) VirtualFree(p, 0, MEM_RELEASE); };
    std::unique_ptr<void, decltype(release)> memory(VirtualAlloc(nullptr, 0xc61000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE), release);
    check(memory != nullptr);
    const auto base = reinterpret_cast<std::uintptr_t>(memory.get());
    auto actor = reinterpret_cast<void *>(base + 0x1000);
    char error[256]{}; Result result;
    for (bool sky2 : {false, true}) {
        for (const auto &profile : (sky2 ? second : first)) {
            const auto table = base + profile.table;
            std::memcpy(actor, &table, sizeof(table));
            const std::uintptr_t targets[]{base + profile.name, base + profile.id, base + profile.flags};
            std::memcpy(reinterpret_cast<void *>(table + 8), targets, sizeof(targets));
            unsigned char name[]{0x48,0x8d,0x05,0,0,0,0,0xc3};
            const auto displacement = static_cast<std::int32_t>(profile.text - profile.name - 7);
            std::memcpy(name + 3, &displacement, 4); std::memcpy(reinterpret_cast<void *>(targets[0]), name, sizeof(name));
            unsigned char numeric[]{0xb8,0,0,0,0,0xc3};
            std::memcpy(numeric + 1, &profile.ident, 4); std::memcpy(reinterpret_cast<void *>(targets[1]), numeric, sizeof(numeric));
            if (profile.bits) {
                std::memcpy(numeric + 1, &profile.bits, 4); std::memcpy(reinterpret_cast<void *>(targets[2]), numeric, sizeof(numeric));
            } else {
                const unsigned char zero[]{0x33,0xc0,0xc3}; std::memcpy(reinterpret_cast<void *>(targets[2]), zero, sizeof(zero));
            }
            std::memcpy(reinterpret_cast<void *>(base + profile.text), "Copied type", 12);
            FlushInstructionCache(GetCurrentProcess(), memory.get(), 0xc61000);
            result = {}; check(!actor_methods::type(actor, base, sky2, "type_name", capture, &result, error, sizeof(error)) && result.text == "Copied type" && result.count == 1);
            result = {}; check(!actor_methods::type(actor, base, sky2, "type_id", capture, &result, error, sizeof(error)) && result.value.type == FDKSS_INTEGER && result.value.integer == profile.ident && result.count == 1);
            result = {}; check(!actor_methods::type(actor, base, sky2, "type_flags", capture, &result, error, sizeof(error)) && result.value.integer == profile.bits && result.count == 1);
            auto invalid = targets[0] + 1; std::memcpy(reinterpret_cast<void *>(table + 8), &invalid, 8);
            result = {}; check(actor_methods::type(actor, base, sky2, "type_name", capture, &result, error, sizeof(error)) && result.count == 0);
            std::memcpy(reinterpret_cast<void *>(table + 8), &targets[0], 8);
            *reinterpret_cast<unsigned char *>(targets[1] + 1) ^= 1;
            result = {}; check(actor_methods::type(actor, base, sky2, "type_id", capture, &result, error, sizeof(error)) && result.count == 0);
            std::memset(reinterpret_cast<void *>(base + profile.text), 'x', 128);
            result = {}; check(actor_methods::type(actor, base, sky2, "type_name", capture, &result, error, sizeof(error)) && result.count == 0);
        }
    }
    const std::uintptr_t pure_virtual_table = base + 0xb19b50; std::memcpy(actor, &pure_virtual_table, 8);
    result = {}; check(actor_methods::type(actor, base, true, "type_name", capture, &result, error, sizeof(error)) && result.count == 0);
}
void test_state_byte() {
    const auto release=[](void *p){if(p)VirtualFree(p,0,MEM_RELEASE);};
    std::unique_ptr<void,decltype(release)> memory(VirtualAlloc(nullptr,0xc61000,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE),release);
    check(memory!=nullptr);const auto base=reinterpret_cast<std::uintptr_t>(memory.get());
    auto &storage=*reinterpret_cast<std::array<unsigned char,0x200> *>(base+0x1000);
    char error[256]{};Result result;
    for(const bool second : {false,true}) {
        const auto table=base+(second ? 0xafdec8 : 0x7eb7d0),setter=base+(second ? 0x5ab260 : 0x480b70);const auto slot=second ? 18 : 17;
        std::fill(storage.begin(),storage.end(),0x5a);std::memcpy(storage.data(),&table,8);
        std::memcpy(reinterpret_cast<void *>(table+slot*8),&setter,8);
        constexpr unsigned char body[]{0x88,0x91,0x6c,1,0,0,0xc3};std::memcpy(reinterpret_cast<void *>(setter),body,sizeof(body));
        fdkss_value argument{};argument.type=FDKSS_INTEGER;
        const auto call=[&](std::size_t count=1){result={};return actor_methods::state_byte(storage.data(),base,second,&argument,count,capture,&result,error,sizeof(error));};
        for(const int value : {0,1,127,255}) {
            const auto before=storage;argument.integer=value;
            check(!call() && result.count==1 && result.value.type==FDKSS_INTEGER && result.value.integer==value);
            for(std::size_t i=0;i<storage.size();++i)check(i==0x16c ? storage[i]==value : storage[i]==before[i]);
        }
        const auto before=storage;
        for(const int value : {-1,256}){argument.integer=value;check(call() && result.count==0 && storage==before);}
        argument.type=FDKSS_NUMBER;
        for(const double value : {.5,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){argument.number=value;check(call() && storage==before);}
        argument.number=1;check(!call() && result.value.integer==1);
        for(const auto type : {FDKSS_STRING,FDKSS_BOOLEAN,FDKSS_NIL}){argument.type=type;check(call());}
        argument.type=FDKSS_INTEGER;argument.integer=0;check(call(0) && call(2));
        const auto wrong=setter+1;std::memcpy(reinterpret_cast<void *>(table+slot*8),&wrong,8);check(call());
        std::memcpy(reinterpret_cast<void *>(table+slot*8),&setter,8);
        *reinterpret_cast<unsigned char *>(setter+6)^=1;check(call());*reinterpret_cast<unsigned char *>(setter+6)^=1;
        DWORD previous=0,ignored=0;check(VirtualProtect(reinterpret_cast<void *>(setter),7,PAGE_READWRITE,&previous));check(call());
        check(VirtualProtect(reinterpret_cast<void *>(setter),7,previous,&ignored));check(!call() && storage[0x16c]==0);
        check(VirtualProtect(storage.data(),storage.size(),PAGE_READONLY,&previous));check(call() && storage[0x16c]==0);
        check(VirtualProtect(storage.data(),storage.size(),previous,&ignored));check(!call());
        const auto base_table=base+(second ? 0xb19b50 : 0x801c28);std::memcpy(storage.data(),&base_table,8);check(call());
    }
}
}
int main() {
    try {
        const auto release = [](void *p) { if (p) VirtualFree(p, 0, MEM_RELEASE); };
        std::unique_ptr<void, decltype(release)> code(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE), release);
        check(code != nullptr);
        const unsigned char getter[]{0x48,0x8d,0x41,0x58,0xc3};
        std::memcpy(code.get(), getter, sizeof(getter));
        DWORD previous = 0;
        check(VirtualProtect(code.get(), 4096, PAGE_EXECUTE_READ, &previous));
        FlushInstructionCache(GetCurrentProcess(), code.get(), sizeof(getter));
        const auto target = reinterpret_cast<std::uintptr_t>(code.get());
        std::array<std::uintptr_t, 7> table{}; table[6] = target;
        std::array<std::byte, 0x200> actor{};
        const auto table_address = reinterpret_cast<std::uintptr_t>(table.data());
        std::memcpy(actor.data(), &table_address, sizeof(table_address));
        std::array<float, 16> matrix{};
        for (int i = 0; i < 16; ++i) matrix[i] = i + 0.25f;
        std::memcpy(actor.data() + 0x58, matrix.data(), sizeof(matrix));
        char error[256]{}; std::vector<double> values;
        auto call = [&] { return actor_methods::transform(actor.data(), target, emit, &values, error, sizeof(error)); };
        check(!call() && values.size() == 16 && values[12] == 12.25);
        // The result is copied; it does not expose engine-owned storage.
        matrix[12] = 99; std::memcpy(actor.data() + 0x58, matrix.data(), sizeof(matrix));
        check(values[12] == 12.25); values.clear();
        matrix[15] = std::numeric_limits<float>::infinity();
        std::memcpy(actor.data() + 0x58, matrix.data(), sizeof(matrix));
        check(call() && values.empty());
        table[6] = target + 1; check(call() && values.empty()); table[6] = target;
        check(VirtualProtect(code.get(), 4096, PAGE_READWRITE, &previous));
        check(call() && values.empty());
        static_cast<unsigned char *>(code.get())[3] = 0x60;
        check(VirtualProtect(code.get(), 4096, PAGE_EXECUTE_READ, &previous));
        FlushInstructionCache(GetCurrentProcess(), code.get(), sizeof(getter));
        check(call() && values.empty());
        check(actor_methods::transform(nullptr, target, emit, &values, error, sizeof(error)) && values.empty());
        check(actor_methods::transform(reinterpret_cast<void *>(1), target, emit, &values, error, sizeof(error)) && values.empty());
        test_types();
        test_state_byte();
        puts("actor transform and both games' 22 concrete type profiles passed"); return 0;
    } catch (const std::exception &error) { puts(error.what()); return 1; }
}
