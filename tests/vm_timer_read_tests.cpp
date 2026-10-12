#include "../src/vm_timer_read.h"
#include "../src/vm_delta_read.h"
#include <stdexcept>
#include <vector>
int main() {
    const auto check=[](bool condition){if(!condition)throw std::runtime_error("private timer read profile check failed");};
    for (const bool second : {false,true}) {
        const auto &profile=second ? private_timer::second_read : private_timer::first_read;
        constexpr std::uintptr_t base=0x100000000ull,manager=base+0x2000,command=base+0x3000;
        std::vector<unsigned char> image(0xc61000);
        const auto put=[&](std::uintptr_t address,auto value){std::memcpy(image.data()+address-base,&value,sizeof(value));};
        const auto copy=[&](std::uintptr_t address,const auto &bytes){std::memcpy(image.data()+address-base,bytes.data(),bytes.size());};
        put(base+profile.manager,manager);put(manager+0x20,command);put(command,base+profile.table);
        put(base+profile.table+16,base+profile.scan);put(base+profile.table+24,base+profile.mask);
        put(command+8+31*24,base+profile.handler);put(command+profile.mask_offset+31*4,std::uint32_t{1});
        // Only the low DWORD of the last member-pointer word is meaningful; padding may be nonzero.
        put(command+8+31*24+16,std::uint64_t{0xffffffff00000000ull});
        const std::vector<unsigned char> handler{0x8b,0x82,0x10,0x04,0,0,0xc1,0xe8,2,0x0f,0xba,0xe8,0x1f,
            0x80,0xba,0x80,0,0,0,0,0x74,3,0x89,0x42,0x14,0xc3};
        copy(base+profile.handler,handler);copy(base+profile.scan,command_catalog::scan_code(profile.capacity));
        copy(base+profile.mask,command_catalog::mask_code(profile.mask_offset));
        bool replaced=false,executable=true;unsigned manager_reads=0;
        auto read=[&](std::uintptr_t address,void *out,std::size_t size) {
            if(address<base || address-base>image.size() || size>image.size()-(address-base))return false;
            std::memcpy(out,image.data()+address-base,size);
            if(address==manager+0x20 && ++manager_reads==2 && replaced) {const std::uintptr_t different=command+8;std::memcpy(out,&different,8);}
            return true;
        };
        auto verify=[&] {manager_reads=0;return private_timer::read_profile(base,second,command,read,[&](std::uintptr_t){return executable;});};
        check(verify());
        put(command+profile.mask_offset+31*4,std::uint32_t{0});check(!verify());
        put(command+profile.mask_offset+31*4,std::uint32_t{1});
        put(command+8+31*24+8,std::uint64_t{4});check(!verify());put(command+8+31*24+8,std::uint64_t{0});
        put(command+8+31*24+16,std::uint64_t{0xffffffff00000001ull});check(!verify());
        put(command+8+31*24+16,std::uint64_t{0xffffffff00000000ull});
        image[profile.handler+24]^=1;check(!verify());image[profile.handler+24]^=1;
        image[profile.mask]^=1;check(!verify());image[profile.mask]^=1;
        put(base+profile.table+16,base+profile.scan+1);check(!verify());put(base+profile.table+16,base+profile.scan);
        executable=false;check(!verify());executable=true;
        replaced=true;check(!verify());replaced=false;check(verify());
        const auto delta=second ? 0x4c47c0 : 0x3d7770;
        const std::vector<unsigned char> delta_code=second ?
            std::vector<unsigned char>{0x8b,0x05,0xda,0x92,0x79,0,0xc1,0xe8,2,0x0f,0xba,0xe8,0x1f,0x80,0xba,0x80,0,0,0,0,0x74,3,0x89,0x42,0x14,0xc3} :
            std::vector<unsigned char>{0x8b,0x42,0x7c,0xc1,0xe8,2,0x0f,0xba,0xe8,0x1f,0x80,0xba,0x80,0,0,0,0,0x74,3,0x89,0x42,0x14,0xc3};
        copy(base+delta,delta_code);put(command+8+32*24,base+delta);put(command+profile.mask_offset+32*4,std::uint32_t{1});
        put(base+0xc5daa0,std::uint32_t{0x3c888889});
        auto delta_verify=[&](std::uint32_t &raw,std::uint32_t &source){return private_delta::profile(base,second,read,[&](std::uintptr_t){return executable;},raw,source);};
        std::uint32_t raw=0,source=0;check(delta_verify(raw,source));check(raw==(second ? (0x3c888889u>>2)|0x80000000u : 0x80000000u));
        image[delta+2]^=1;check(!delta_verify(raw,source));image[delta+2]^=1;
        put(command+8+32*24+8,std::uint64_t{1});check(!delta_verify(raw,source));put(command+8+32*24+8,std::uint64_t{0});
        put(command+profile.mask_offset+32*4,std::uint32_t{0});check(!delta_verify(raw,source));put(command+profile.mask_offset+32*4,std::uint32_t{1});
        executable=false;check(!delta_verify(raw,source));executable=true;
        if(second) {
            for(const auto bad : {0x7fc00000u,0x7f800000u,0xbc888889u,0x42000000u}) {put(base+0xc5daa0,bad);check(!delta_verify(raw,source));}
        }
    }
}
