#include "engine_member_copy.h"
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <cstdio>

namespace {
void check(bool value) { if (!value) throw std::runtime_error("engine member copy assertion failed"); }
struct Fixture { std::size_t slot; std::uint32_t offset; std::array<unsigned char,5> body; };
// Independently inspected originals in engine_getter_originals.json, ordered:
// Sky1, Sky2, Proud Nordics, original Nordics, Daybreak1, Daybreak2.
constexpr Fixture originals[]{
    {6,0x58,{0x48,0x8d,0x41,0x58,0xc3}}, {6,0x58,{0x48,0x8d,0x41,0x58,0xc3}},
    {6,0x70,{0x48,0x8d,0x41,0x70,0xc3}}, {8,0x70,{0x48,0x8d,0x41,0x70,0xc3}},
    {5,0x70,{0x48,0x8d,0x41,0x70,0xc3}}, {6,0x70,{0x48,0x8d,0x41,0x70,0xc3}}
};
void test(const Fixture &original) {
    constexpr std::uintptr_t actor=0x1000, table=0x800, getter=0x100;
    std::array<unsigned char,0x2000> image{};
    auto store=[&](std::uintptr_t at,const auto &value) { std::memcpy(image.data()+at,&value,sizeof(value)); };
    store(actor,table); store(table+original.slot*8,getter);
    std::memcpy(image.data()+getter,original.body.data(),original.body.size());
    std::array<float,16> first{},second{};
    for(std::size_t i=0;i<first.size();++i) { first[i]=static_cast<float>(i)+.25f;second[i]=static_cast<float>(i)+100.5f; }
    store(actor+original.offset,first);store(actor+original.offset+0x70,second);
    auto profile=engine_members::FloatBlockProfile{original.slot,original.offset,0};
    int invocations=0;bool execute=true,hook_owned=false,fail_copy=false;
    std::array<float,16> output{};output.fill(-1);
    auto read=[&](std::uintptr_t at,void *destination,std::size_t size) {
        if(at>image.size() || size>image.size()-at || (fail_copy && at==actor+original.offset+profile.float_offset))return false;
        std::memcpy(destination,image.data()+at,size);return true;
    };
    auto invoke=[&](std::uintptr_t at,std::uintptr_t receiver) {
        ++invocations;
        // Evaluate the independently frozen instruction's operand, not the profile.
        return receiver+static_cast<std::int8_t>(image[at+3]);
    };
    auto call=[&](std::uintptr_t receiver=0x1000) {
        return engine_members::copy_floats(receiver,getter,profile,output,read,
            [&](auto at){return execute && at==getter;},
            [&](auto at,auto target){return hook_owned && at==table+original.slot*8 && target==getter;},invoke);
    };
    using Status=engine_members::CopyStatus;
    check(call()==Status::ok && invocations==1 && output==first);
    auto changed=first;changed[0]=99;store(actor+original.offset,changed);
    check(output==first);store(actor+original.offset,first);
    profile.float_offset=0x70;check(call()==Status::ok && output==second);profile.float_offset=0;
    const auto held=output;
    first[8]=std::numeric_limits<float>::infinity();store(actor+original.offset,first);
    check(call()==Status::nonfinite && output==held);
    first[8]=std::numeric_limits<float>::quiet_NaN();store(actor+original.offset,first);
    check(call()==Status::nonfinite && output==held);
    first[8]=8.25f;store(actor+original.offset,first);
    fail_copy=true;check(call()==Status::copy_failed && output==held);fail_copy=false;
    auto previous=invocations;
    execute=false;check(call()==Status::getter_changed && invocations==previous);execute=true;
    image[getter+4]=0x90;check(call()==Status::getter_changed && invocations==previous);image[getter+4]=0xc3;
    ++profile.slot;check(call()==Status::getter_changed && invocations==previous);--profile.slot;
    profile.member_offset+=8;check(call()==Status::getter_changed && invocations==previous);profile.member_offset-=8;
    const std::uintptr_t third_party=getter+0x10;store(table+original.slot*8,third_party);
    check(call()==Status::getter_changed && invocations==previous);
    hook_owned=true;check(call()==Status::ok && output==first);hook_owned=false;
    store(table+original.slot*8,getter);
    previous=invocations;
    check(call(0)==Status::getter_changed && call(1)==Status::getter_changed && invocations==previous);
    check(call(std::numeric_limits<std::uintptr_t>::max()-4)==Status::getter_changed && invocations==previous);
    profile.member_offset=128;check(call()==Status::getter_changed && invocations==previous);
    profile.member_offset=original.offset;profile.slot=512;
    check(call()==Status::getter_changed && invocations==previous);
}
void test_disp32_and_native_return() {
    std::array<float,4> output{1,2,3,4}; const auto held=output;
    const std::uintptr_t actor=0x1000,table=0x2000,getter=0x3000;
    constexpr std::array<unsigned char,8> body{0x48,0x8d,0x81,0,1,0,0,0xc3};
    int invocations=0;
    auto read=[&](std::uintptr_t at,void *data,std::size_t size) {
        if(at==actor && size==8)std::memcpy(data,&table,8);
        else if(at==table+7*8 && size==8)std::memcpy(data,&getter,8);
        else if(at==getter && size==body.size())std::memcpy(data,body.data(),size);
        else if(at==actor+0x120 && size==sizeof(held))std::memcpy(data,held.data(),size);
        else return false;
        return true;
    };
    const engine_members::FloatBlockProfile profile{7,0x100,0x20,true};
    auto invoke=[&](auto,auto){++invocations;return actor+0x100;};
    using Status=engine_members::CopyStatus;
    check(engine_members::copy_floats(actor,getter,profile,output,read,[](auto){return true;},[](auto,auto){return false;},invoke)==Status::ok);
    check(output==held && invocations==1);
    check(engine_members::copy_floats(actor,getter,profile,output,read,[](auto){return true;},[](auto,auto){return false;},
        [](auto,auto){return std::uintptr_t{0};})==Status::copy_failed && output==held);
}
}
int main() {
    try { for(const auto &original:originals)test(original);test_disp32_and_native_return();
        puts("shared member copy: six original encodings, copied fields and refusal checks passed");return 0;
    } catch(const std::exception &error) { puts(error.what());return 1; }
}
