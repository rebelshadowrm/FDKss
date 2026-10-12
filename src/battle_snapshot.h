#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

// Passive, bounded copies only. Not a lifetime tracker or an atomic snapshot.
// An adapter must independently pin its executable/getter contracts and establish
// the access context before using this reader. No native calls or writes occur.
namespace battle_snapshot {
inline constexpr std::size_t limit=70;
struct Profile { std::uintptr_t global, table, character_table, backlink, status_link; };
inline constexpr Profile first{0x8fba18,0x7dde58,0x7eb7d0,0x1d30,0x1d38};
inline constexpr Profile second{0xc5d768,0xaeb7c0,0xafdec8,0x1d88,0x1d90};
struct Record {
    // The first status word is copied verbatim; its identity semantics remain
    // research. Level and the six resources match independently pinned getters.
    std::uint32_t raw_status_word=0,level=0;
    std::array<std::uint32_t,6> resources{};
};
struct Snapshot { std::size_t count=0; std::array<Record,limit> records{}; };
enum class Result { valid, absent, contract, unreadable, structure, resources, changed };

template<class Read,class Contract>
Result copy(std::uintptr_t base,const Profile &profile,Read read,Contract contract,Snapshot &output) {
    output={};
    if(!base || !contract())return Result::contract;
    const auto fetch=[&](std::uintptr_t object,std::uintptr_t offset,void *value,std::size_t size) {
        const auto maximum=std::numeric_limits<std::uintptr_t>::max();
        return object && offset<=maximum-object && size<=maximum-(object+offset) &&
            read(object+offset,value,size);
    };
    const auto aligned=[](std::uintptr_t value,std::uintptr_t alignment=8) {return value && value%alignment==0;};
    std::uintptr_t root=0;
    if(!fetch(base,profile.global,&root,sizeof(root)))return Result::unreadable;
    if(!root) {
        std::uintptr_t again=0;
        if(!fetch(base,profile.global,&again,sizeof(again)))return Result::unreadable;
        return again || !contract() ? Result::changed : Result::absent;
    }
    std::uintptr_t table=0,self=0,storage=0;
    std::int32_t count=0;
    if(!aligned(root))return Result::structure;
    if(!fetch(root,0,&table,8) || !fetch(root,8,&self,8) ||
       !fetch(root,0x2448,&storage,8) || !fetch(root,0x2450,&count,4))return Result::unreadable;
    const auto maximum=std::numeric_limits<std::uintptr_t>::max();
    if(profile.table>maximum-base || profile.character_table>maximum-base ||
       table!=base+profile.table || self!=root || root>maximum-0x2218 ||
       storage!=root+0x2218 || count<0 || count>static_cast<std::int32_t>(limit))return Result::structure;
    std::array<std::uintptr_t,limit> entries{};
    if(count && !fetch(storage,0,entries.data(),static_cast<std::size_t>(count)*8))return Result::unreadable;
    struct Chain {std::uintptr_t context=0,link=0,status=0;std::array<std::uint32_t,9> prefix{};};
    std::array<Chain,limit> chains{};
    Snapshot candidate{};candidate.count=static_cast<std::size_t>(count);
    for(std::size_t i=0;i<candidate.count;++i) {
        if(!aligned(entries[i]))return Result::structure;
        for(std::size_t j=0;j<i;++j)if(entries[i]==entries[j])return Result::structure;
        auto &chain=chains[i];std::uintptr_t context_table=0,backlink=0;
        if(!fetch(entries[i],0x328,&chain.context,8))return Result::unreadable;
        if(!aligned(chain.context))return Result::structure;
        if(!fetch(chain.context,0,&context_table,8) || !fetch(chain.context,profile.backlink,&backlink,8) ||
           !fetch(chain.context,profile.status_link,&chain.link,8))return Result::unreadable;
        if(context_table!=base+profile.character_table || backlink!=entries[i] || !aligned(chain.link))return Result::structure;
        if(!fetch(chain.link,0,&chain.status,8))return Result::unreadable;
        if(!aligned(chain.status,4))return Result::structure;
        if(!fetch(chain.status,0,chain.prefix.data(),sizeof(chain.prefix)))return Result::unreadable;
        auto &record=candidate.records[i];record.raw_status_word=chain.prefix[0];record.level=chain.prefix[1];
        for(std::size_t field=0;field<6;++field)record.resources[field]=chain.prefix[field+3];
        for(std::size_t field=0;field<6;field+=2)
            if(record.resources[field]>record.resources[field+1] ||
               record.resources[field+1]>static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))return Result::resources;
    }
    // Recheck every copied chain before publishing anything. Changes or failures
    // discard the entire candidate. This cannot detect same-address ABA reuse.
    for(std::size_t i=candidate.count;i>0;--i) {
        const auto index=i-1;const auto &chain=chains[index];
        std::uintptr_t context=0,context_table=0,backlink=0,link=0,status=0;
        std::array<std::uint32_t,9> prefix{};
        if(!fetch(entries[index],0x328,&context,8) || !fetch(chain.context,0,&context_table,8) ||
           !fetch(chain.context,profile.backlink,&backlink,8) || !fetch(chain.context,profile.status_link,&link,8) ||
           !fetch(chain.link,0,&status,8) || !fetch(chain.status,0,prefix.data(),sizeof(prefix)))return Result::unreadable;
        if(context!=chain.context || context_table!=base+profile.character_table || backlink!=entries[index] ||
           link!=chain.link || status!=chain.status || prefix!=chain.prefix)return Result::changed;
    }
    std::array<std::uintptr_t,limit> current_entries{};
    std::uintptr_t current_root=0,current_table=0,current_self=0,current_storage=0;std::int32_t current_count=0;
    if((count && !fetch(storage,0,current_entries.data(),static_cast<std::size_t>(count)*8)) ||
       !fetch(root,0,&current_table,8) || !fetch(root,8,&current_self,8) ||
       !fetch(root,0x2448,&current_storage,8) || !fetch(root,0x2450,&current_count,4) ||
       !fetch(base,profile.global,&current_root,8))return Result::unreadable;
    if(current_entries!=entries || current_root!=root || current_table!=table || current_self!=self ||
       current_storage!=storage || current_count!=count || !contract())return Result::changed;
    output=candidate;return Result::valid;
}
}
