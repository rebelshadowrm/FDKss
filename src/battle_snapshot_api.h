#pragma once
#include "battle_snapshot.h"
#include "fdkss/api.h"
#include <cstdio>

namespace battle_snapshot {
// Wire record: status word, level, HP/max, EP/max, CP/max, eight little-endian
// DWORDs. No address, lifetime token or callable receiver is returned.
template<class Read,class Contract,class Owner>
int invoke(std::uintptr_t base,const Profile &profile,Read read,Contract contract,Owner owner,
    std::size_t count,fdkss_result_fn emit,void *sink,char *error,std::size_t capacity) {
    const auto fail=[&](const char *message){if(error && capacity)std::snprintf(error,capacity,"%s",message);return 1;};
    if(!owner())return fail("battle snapshots require the owning update thread");
    if(count || !emit)return fail("battle_snapshot expects no arguments and a result sink");
    Snapshot snapshot{};
    const auto valid=[&]{return owner() && contract();};
    const auto result=copy(base,profile,read,valid,snapshot);
    fdkss_value value{};
    if(result==Result::absent)return emit(sink,&value);
    if(result!=Result::valid) {
        switch(result) {
        case Result::contract:return fail("battle snapshot executable or getter contract changed");
        case Result::unreadable:return fail("battle snapshot chain is unreadable");
        case Result::structure:return fail("battle snapshot collection or reciprocal chain is invalid");
        case Result::resources:return fail("battle snapshot resources are outside the verified range");
        default:return fail("battle snapshot changed while copying");
        }
    }
    std::array<unsigned char,limit*32> bytes{};
    for(std::size_t i=0;i<snapshot.count;++i) {
        const auto &record=snapshot.records[i];
        const std::array<std::uint32_t,8> fields{record.raw_status_word,record.level,
            record.resources[0],record.resources[1],record.resources[2],record.resources[3],record.resources[4],record.resources[5]};
        for(std::size_t field=0;field<fields.size();++field)
            for(unsigned byte=0;byte<4;++byte)
                bytes[i*32+field*4+byte]=static_cast<unsigned char>(fields[field]>>(byte*8));
    }
    // The entire copy passed before either result is emitted. The typed runtime
    // discards emitted values if a sink fails; emission does not revisit memory.
    value.type=FDKSS_INTEGER;value.integer=static_cast<std::int64_t>(snapshot.count);
    if(emit(sink,&value))return 1;
    value={};value.type=FDKSS_STRING;value.string=reinterpret_cast<const char *>(bytes.data());
    value.string_size=snapshot.count*32;return emit(sink,&value);
}
}
