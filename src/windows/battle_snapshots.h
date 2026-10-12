#pragma once
#include "player_resources.h"
#include "../battle_snapshot_api.h"

namespace battle_snapshots {
struct Code {std::uintptr_t rva;std::string_view hex;};
#include "battle_profiles.inc"
template<class Owner>
int snapshot(std::uintptr_t image,bool second,Owner owner,std::size_t count,
    fdkss_result_fn emit,void *sink,char *error,std::size_t capacity) {
    const auto contract=[&] {
        const auto &resources=second ? player_resources::sky2 : player_resources::sky1;
        if(!player_resources::pinned(image,resources))return false;
        const auto *codes=second ? second_codes : first_codes;
        const auto code_count=second ? std::size(second_codes) : std::size(first_codes);
        std::array<unsigned char,4096> bytes{};
        const auto nibble=[](char c){return c<='9' ? c-'0' : c-'a'+10;};
        for(std::size_t i=0;i<code_count;++i) {
            const auto &code=codes[i];
            if(code.hex.size()%2 || code.hex.size()/2>bytes.size() ||
               !player_resources::read(image+code.rva,bytes.data(),code.hex.size()/2))return false;
            for(std::size_t byte=0;byte<code.hex.size()/2;++byte)
                if(bytes[byte]!=nibble(code.hex[byte*2])*16+nibble(code.hex[byte*2+1]))return false;
        }
        return true;
    };
    return battle_snapshot::invoke(image,second ? battle_snapshot::second : battle_snapshot::first,
        player_resources::read,contract,owner,count,emit,sink,error,capacity);
}
}
