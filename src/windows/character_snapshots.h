#pragma once
#include "player_conditions.h"

namespace character_snapshots {
// Resolve only the existing player root or generation-stamped scene references.
// The caller checks the owner thread before entering this shared validator.
template<class Resolve> int snapshot(std::uintptr_t image,const player_resources::Profile &resources,
    const player_conditions::Profile &conditions,bool condition_records,Resolve resolve,
    const fdkss_value *args,std::size_t count,fdkss_result_fn emit,void *sink,char *error,std::size_t capacity) {
    const auto fail=[&](const char *message){std::snprintf(error,capacity,"%s",message);return 1;};
    if(count!=1 || !args || args[0].type!=FDKSS_STRING || !args[0].string || !emit)
        return fail("character snapshot expects one current character reference");
    const std::string_view reference(args[0].string,args[0].string_size);
    if(reference!="player" && (reference.empty() || reference.front()!='@'))
        return fail("character snapshot expects player or a scene actor reference");
    const auto actor=resolve(reference);
    std::uintptr_t table=0;
    if(!actor || !player_resources::read(actor,&table,8) || table!=image+resources.table)
        return fail("character snapshot reference is stale or is not a covered character");
    bool first=true;
    const auto current=[&] {if(first){first=false;return actor;}return resolve(reference);};
    return condition_records ? player_conditions::snapshot(image,resources,conditions,current,0,emit,sink,error,capacity)
        : player_resources::snapshot(image,resources,current,0,emit,sink,error,capacity);
}
}
