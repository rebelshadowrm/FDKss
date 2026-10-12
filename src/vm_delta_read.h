#pragma once
#include "vm_timer_read.h"
#include "vm_pure.h"
namespace private_delta {
inline bool command(unsigned group,unsigned id,unsigned count) {return group==0 && id==32 && count==0;}
// Sky 1 reads private Thread+7c; Sky 2 reads the shared frame delta at C5DAA0.
// Shared delta is a synchronous owner-thread snapshot, never an async constant.
template<class Read,class Executable>
bool profile(std::uintptr_t base,bool second,Read read,Executable executable,std::uint32_t &raw,std::uint32_t &source) {
    if (!private_timer::read_profile(base,second,0,read,executable)) return false;
    const auto &p=second ? private_timer::second_read : private_timer::first_read;
    std::uintptr_t manager=0,object=0,handler=0;std::uint64_t adjustment=0;std::uint32_t extra=0,mask=0;
    const std::uintptr_t rva=second ? 0x4c47c0 : 0x3d7770;
    if (!read(base+p.manager,&manager,8) || !manager || !read(manager+0x20,&object,8) || !object ||
        !read(object+8+32*24,&handler,8) || handler!=base+rva || !executable(handler) ||
        !read(object+8+32*24+8,&adjustment,8) || adjustment || !read(object+8+32*24+16,&extra,4) || extra ||
        !read(object+p.mask_offset+32*4,&mask,4) || mask!=1) return false;
    constexpr unsigned char first[]{0x8b,0x42,0x7c,0xc1,0xe8,2,0x0f,0xba,0xe8,0x1f,0x80,0xba,0x80,0,0,0,0,0x74,3,0x89,0x42,0x14,0xc3};
    constexpr unsigned char second_code[]{0x8b,0x05,0xda,0x92,0x79,0,0xc1,0xe8,2,0x0f,0xba,0xe8,0x1f,0x80,0xba,0x80,0,0,0,0,0x74,3,0x89,0x42,0x14,0xc3};
    std::array<unsigned char,26> actual{};
    const auto length=second ? sizeof(second_code) : sizeof(first);
    if (!read(handler,actual.data(),length) || std::memcmp(actual.data(),second ? second_code : first,length)) return false;
    source=0;
    if (second && !read(base+0xc5daa0,&source,4)) return false;
    raw=(source>>2)|0x80000000u;
    const float value=std::bit_cast<float>(raw<<2);
    std::uintptr_t current=0;
    return pure_vm::scalar(raw) && value>=0 && value<=30 &&
        read(manager+0x20,&current,8) && current==object;
}
}
