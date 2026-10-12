#include "battle_snapshot.h"
#include "battle_snapshot_api.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace battle_snapshot;
namespace {
constexpr std::uintptr_t base=0x100000000ull,root=base+0x10000;
constexpr auto entry=[](std::size_t i){return base+0x100000+i*0x4000;};
constexpr auto context=[](std::size_t i){return base+0x500000+i*0x3000;};
constexpr auto link=[](std::size_t i){return base+0x700000+i*0x80;};
constexpr auto status=[](std::size_t i){return base+0x740000+i*0x44;};
void check(bool condition) {if(!condition)throw std::runtime_error("battle copied snapshot check failed");}
struct Memory {
    const Profile &profile;std::vector<unsigned char> bytes;
    explicit Memory(const Profile &p):profile(p),bytes(0xd00000) {reset(2);}
    template<class T> void put(std::uintptr_t address,T value) {std::memcpy(bytes.data()+address-base,&value,sizeof(value));}
    void reset(std::int32_t count) {
        std::fill(bytes.begin(),bytes.end(),0);
        put(base+profile.global,root);put(root,base+profile.table);put(root+8,root);
        put(root+0x2448,root+0x2218);put(root+0x2450,count);
        for(std::size_t i=0;i<limit;++i) {
            put(root+0x2218+i*8,entry(i));put(entry(i)+0x328,context(i));
            put(context(i),base+profile.character_table);put(context(i)+profile.backlink,entry(i));
            put(context(i)+profile.status_link,link(i));put(link(i),status(i));
            const std::array<std::uint32_t,9> values{static_cast<std::uint32_t>(60000+i),45,70,
                static_cast<std::uint32_t>(1813+i),12186,1000,1000,199,200};
            std::memcpy(bytes.data()+status(i)-base,values.data(),sizeof(values));
        }
    }
    bool read(std::uintptr_t address,void *out,std::size_t size) const {
        if(address<base || address-base>bytes.size() || size>bytes.size()-(address-base))return false;
        std::memcpy(out,bytes.data()+address-base,size);return true;
    }
};
bool empty(const Snapshot &value) {
    if(value.count)return false;
    for(const auto &row:value.records)
        if(row.raw_status_word || row.level || row.resources!=std::array<std::uint32_t,6>{})return false;
    return true;
}
}
int main() {
    for(const auto &profile:{first,second}) {
        Memory memory(profile);Snapshot output{};
        const auto read=[&](auto address,auto out,auto size){return memory.read(address,out,size);};
        const auto run=[&]{return copy(base,profile,read,[]{return true;},output);};
        check(run()==Result::valid && output.count==2);
        check(output.records[0].raw_status_word==60000 && output.records[1].level==45);
        check(output.records[0].resources==std::array<std::uint32_t,6>{1813,12186,1000,1000,199,200});
        // Status is a DWORD structure: the second record is deliberately only
        // four-byte aligned, and must not inherit the pointer's eight-byte rule.
        check(status(1)%8==4);
        memory.reset(70);check(run()==Result::valid && output.count==70);
        memory.reset(0);check(run()==Result::valid && empty(output));
        memory.put(base+profile.global,std::uintptr_t{0});check(run()==Result::absent && empty(output));

        // Inject read failure at EVERY read boundary, including late validation.
        // A previously populated output must be discarded in every case.
        memory.reset(2);std::size_t reads=0;
        const auto count_read=[&](auto address,auto out,auto size){++reads;return memory.read(address,out,size);};
        check(copy(base,profile,count_read,[]{return true;},output)==Result::valid);
        for(std::size_t failure=1;failure<=reads;++failure) {
            std::size_t current=0;output.count=63;output.records[0].level=999;
            const auto fail_read=[&](auto address,auto out,auto size){return ++current!=failure && memory.read(address,out,size);};
            check(copy(base,profile,fail_read,[]{return true;},output)==Result::unreadable && empty(output));
        }
        // Corrupt each ownership/collection link; no partial party rows escape.
        const std::array<std::pair<std::uintptr_t,std::uintptr_t>,11> corruptions{{
            {base+profile.global,root+1},{root,base+profile.table+8},{root+8,root+8},
            {root+0x2448,root+0x2220},{root+0x2220,entry(0)},
            {entry(1)+0x328,context(1)+1},{context(1),base+profile.character_table+8},
            {context(1)+profile.backlink,entry(0)},{context(1)+profile.backlink,0},
            {context(1)+profile.status_link,0},{link(1),status(1)+1}}};
        for(const auto &[address,value]:corruptions) {
            memory.reset(2);memory.put(address,value);output.count=63;
            check(run()==Result::structure && empty(output));
        }
        for(const auto bad_count:{-1,71,std::numeric_limits<std::int32_t>::max()}) {
            memory.reset(bad_count);check(run()==Result::structure && empty(output));
        }
        for(const auto offset:{12,20,28}) {
            memory.reset(2);memory.put(status(1)+offset,std::uint32_t{0xffffffff});
            check(run()==Result::resources && empty(output));
            memory.reset(2);memory.put(status(1)+offset+4,std::uint32_t{0x80000000});
            check(run()==Result::resources && empty(output));
        }
        // Replacements during rechecks: manager, roster, context, backlink,
        // status link, status pointer, resources and raw identity are covered.
        const std::array<std::uintptr_t,12> changed_addresses{{base+profile.global,root,root+8,
            root+0x2448,root+0x2450,root+0x2218,entry(1)+0x328,context(1),
            context(1)+profile.backlink,context(1)+profile.status_link,link(1),status(1)}};
        for(const auto address_to_change:changed_addresses) {
            memory.reset(2);unsigned hits=0;
            const auto changed_read=[&](auto address,void *out,auto size) {
                if(!memory.read(address,out,size))return false;
                if(address==address_to_change && ++hits==2)static_cast<unsigned char *>(out)[0]^=1;
                return true;
            };
            output.count=63;
            check(copy(base,profile,changed_read,[]{return true;},output)==Result::changed && empty(output));
        }
        memory.reset(2);unsigned contracts=0;
        check(copy(base,profile,read,[&]{return ++contracts==1;},output)==Result::changed && empty(output));
        check(copy(base,profile,read,[]{return false;},output)==Result::contract && empty(output));
        check(copy(std::numeric_limits<std::uintptr_t>::max()-8,profile,read,[]{return true;},output)==Result::unreadable && empty(output));
        memory.reset(2);check(run()==Result::valid && output.count==2);
        struct Output {std::vector<fdkss_value_type> types;std::int64_t count=0;std::string bytes;};
        Output packet;
        const auto emit=+[](void *opaque,const fdkss_value *value)->int {
            auto &packet=*static_cast<Output *>(opaque);packet.types.push_back(value->type);
            if(value->type==FDKSS_INTEGER)packet.count=value->integer;
            if(value->type==FDKSS_STRING)packet.bytes.assign(value->string,value->string_size);
            return 0;
        };
        char error[256]{};
        auto api=[&](bool owner=true,std::size_t arguments=0) {
            return invoke(base,profile,read,[]{return true;},[=]{return owner;},arguments,emit,&packet,error,sizeof(error));
        };
        check(!api() && packet.count==2 && packet.bytes.size()==64 && packet.types.size()==2);
        check(static_cast<unsigned char>(packet.bytes[0])==(60000&255) &&
              static_cast<unsigned char>(packet.bytes[4])==45);
        const auto held=packet.bytes;memory.put(status(0)+0xc,std::uint32_t{900});packet={};
        check(!api() && packet.bytes!=held && static_cast<unsigned char>(held[8])==(1813&255));
        packet={};check(api(false) && packet.types.empty() && std::strstr(error,"owning update thread"));
        check(api(true,1) && packet.types.empty());
        memory.reset(0);check(!api() && packet.count==0 && packet.bytes.empty() && packet.types.size()==2);
        packet={};memory.put(base+profile.global,std::uintptr_t{0});
        check(!api() && packet.types.size()==1 && packet.types[0]==FDKSS_NIL);
        packet={};memory.reset(2);memory.put(context(0)+profile.backlink,std::uintptr_t{0});
        check(api() && packet.types.empty());
    }
}
