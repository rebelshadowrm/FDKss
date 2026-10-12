#include "command_catalog.h"
#include <vector>
#include <array>
#include <cstring>
#include <stdexcept>
#include <cstdio>
namespace {
void check(bool value) { if (!value) throw std::runtime_error("command catalog assertion failed"); }
}
int main() {
    try {
        std::vector<unsigned char> memory(4096);
        constexpr std::uintptr_t base=0x10000000, object=base+0x300;
        command_catalog::Profile profile{0,0x100,0x200,3,0x240,0x50,"Fixture"};
        auto put=[&](std::uintptr_t address,const auto &value){std::memcpy(memory.data()+address-base,&value,sizeof(value));};
        put(object,base+profile.table); put(base+profile.table+16,base+profile.scan); put(base+profile.table+24,base+profile.mask);
        const unsigned char scan[]{0x33,0xc0,0x48,0x83,0xc1,8,0x48,0x83,0x39,0,0x74,16,0xff,0xc0,0x48,0x83,0xc1,0x18,0x83,0xf8,3,0x72,0xef,0xb8,0xff,0xff,0xff,0xff,0xc3};
        const unsigned char mask[]{0x8b,0xc2,0x8b,0x44,0x81,0x50,0xc3};
        std::memcpy(memory.data()+profile.scan,scan,sizeof(scan)); std::memcpy(memory.data()+profile.mask,mask,sizeof(mask));
        for (unsigned i=0;i<3;++i) { put(object+8+i*24,std::array<std::uint64_t,3>{base+0x800+i*16,i,0}); put(object+profile.mask_offset+i*4,i+5); }
        auto read=[&](std::uintptr_t address,void *out,std::size_t size){if(address<base || size>memory.size() || address-base>memory.size()-size)return false;std::memcpy(out,memory.data()+address-base,size);return true;};
        std::vector<command_catalog::Entry> entries;
        auto inspect=[&]{return command_catalog::snapshot(base,object,memory.size(),profile,read,entries);};
        check(inspect() && entries.size()==3 && entries[2].mask==7); // Full tables need no sentinel.
        put(object+8+24,std::uint64_t{0}); check(inspect() && entries.size()==3 && entries[1].words[0]==0 && entries[2].words[0]!=0); // Holes do not truncate later entries.
        put(object+8,std::uint64_t{base+memory.size()}); check(!inspect() && entries.empty());
        put(object+8,std::uint64_t{base+0x800});
        memory[profile.scan+1]^=1; check(!inspect() && entries.empty()); memory[profile.scan+1]^=1;
        put(base+profile.table+24,base+profile.mask+1); check(!inspect() && entries.empty()); put(base+profile.table+24,base+profile.mask);
        memory[profile.mask+5]^=1; check(!inspect() && entries.empty()); memory[profile.mask+5]^=1;
        profile.capacity=257; check(!inspect() && entries.empty()); profile.capacity=3;
        profile.mask_offset=0x51; check(!inspect() && entries.empty()); profile.mask_offset=0x50;
        int identity_reads=0;
        auto replaced=[&](std::uintptr_t address,void *out,std::size_t size){if(address==object && ++identity_reads==2){const auto changed=base+0x108;std::memcpy(out,&changed,8);return true;}return read(address,out,size);};
        check(!command_catalog::snapshot(base,object,memory.size(),profile,replaced,entries) && entries.empty());
        puts("command catalog full tables, holes, profile, bounds and identity checks passed");return 0;
    } catch(const std::exception &error){puts(error.what());return 1;}
}
