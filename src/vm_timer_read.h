#pragma once
#include "command_catalog.h"
namespace private_timer {
struct ReadProfile {
    std::uintptr_t manager, table, handler, scan, mask;
    unsigned capacity, mask_offset;
};
inline constexpr ReadProfile first_read{0x8fefc8,0x7fbea8,0x3d7750,0x3d7ef0,0x3d7ea0,40,0x3c8};
inline constexpr ReadProfile second_read{0xc60e58,0xb12a48,0x4c47a0,0x4c4fc0,0x4c4f70,43,0x410};
template<class Read,class Executable> bool read_profile(std::uintptr_t base, bool second,
    std::uintptr_t expected, Read read, Executable executable) {
    const auto &profile=second ? second_read : first_read;
    std::uintptr_t manager=0,command=0,table=0,scan=0,mask=0,handler=0;
    std::uint64_t adjustment=0;std::uint32_t extra=0,raw_mask=0;
    if (!base || !read(base+profile.manager,&manager,8) || !manager || !read(manager+0x20,&command,8) || !command ||
        (expected && command!=expected) || !read(command,&table,8) || table!=base+profile.table ||
        !read(table+16,&scan,8) || scan!=base+profile.scan || !read(table+24,&mask,8) || mask!=base+profile.mask ||
        !read(command+8+31*24,&handler,8) || handler!=base+profile.handler ||
        !read(command+8+31*24+8,&adjustment,8) || adjustment || !read(command+8+31*24+16,&extra,4) || extra ||
        !read(command+profile.mask_offset+31*4,&raw_mask,4) || raw_mask!=1 ||
        !executable(handler) || !executable(scan) || !executable(mask)) return false;
    constexpr unsigned char code[]{0x8b,0x82,0x10,0x04,0,0,0xc1,0xe8,2,0x0f,0xba,0xe8,0x1f,
        0x80,0xba,0x80,0,0,0,0,0x74,3,0x89,0x42,0x14,0xc3};
    std::array<unsigned char,32> actual{};
    const auto scan_code=command_catalog::scan_code(profile.capacity), mask_code=command_catalog::mask_code(profile.mask_offset);
    if (!read(handler,actual.data(),sizeof(code)) || std::memcmp(actual.data(),code,sizeof(code)) ||
        !read(scan,actual.data(),scan_code.size()) || std::memcmp(actual.data(),scan_code.data(),scan_code.size()) ||
        !read(mask,actual.data(),mask_code.size()) || std::memcmp(actual.data(),mask_code.data(),mask_code.size())) return false;
    std::uintptr_t current=0;
    return read(manager+0x20,&current,8) && current==command && read(command,&current,8) && current==table;
}
}
