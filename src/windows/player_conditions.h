#pragma once
#include "player_resources.h"

namespace player_conditions {
struct Profile { std::uintptr_t manager,reader;std::string_view code; };
#include "player_conditions_profiles.inc"
// Preserve raw records. Array presence, counters and flags do not establish
// application, expiration, a stat delta, or even an active condition.
template<class Resolve> int snapshot(std::uintptr_t image,const player_resources::Profile &actor_profile,
    const Profile &p,Resolve resolve,std::size_t count,fdkss_result_fn emit,void *sink,char *error,std::size_t capacity) {
    using player_resources::read;
    const auto fail=[&](const char *message){std::snprintf(error,capacity,"%s",message);return 1;};
    if(count || !emit)return fail("player_conditions expects no arguments");
    if(!player_resources::pinned(image,p.reader,p.code))return fail("player condition layout profile changed");
    fdkss_value value{};const auto actor=resolve();
    if(!actor)return emit(sink,&value);
    std::uintptr_t table=0,link=0,manager=0,owner=0;
    if(!read(actor,&table,8) || table!=image+actor_profile.table || !read(actor+actor_profile.link,&link,8))
        return fail("player condition actor is invalid or unreadable");
    if(!link)return emit(sink,&value);
    if(!read(link+p.manager,&manager,8))return fail("player condition manager link is unreadable");
    if(!manager)return emit(sink,&value);
    if(!read(manager,&owner,8) || owner!=actor)return fail("player condition manager owner changed");
    struct Vector {std::uintptr_t data;std::uint32_t count;};
    // +914 is deliberately not interpreted as capacity; the reader fragments
    // independently establish only the +908 pointer and +910 count.
    Vector vector{},again{};
    if(!read(manager+0x908,&vector.data,8) || !read(manager+0x910,&vector.count,4) ||
        vector.count>128 || (vector.count && (!vector.data || vector.data%4)))
        return fail("player condition vector is unreadable or exceeds 128 records");
    std::array<unsigned char,128*0x48> bytes{},second{};
    const auto length=static_cast<std::size_t>(vector.count)*0x48;
    if(length && !read(vector.data,bytes.data(),length))return fail("player condition records are unreadable");
    std::uintptr_t new_table=0,new_link=0,new_manager=0,new_owner=0;
    if(resolve()!=actor || !read(actor,&new_table,8) || new_table!=table ||
        !read(actor+actor_profile.link,&new_link,8) || new_link!=link ||
        !read(link+p.manager,&new_manager,8) || new_manager!=manager ||
        !read(manager,&new_owner,8) || new_owner!=owner ||
        !read(manager+0x908,&again.data,8) || !read(manager+0x910,&again.count,4) ||
        again.data!=vector.data || again.count!=vector.count ||
        (length && (!read(vector.data,second.data(),length) || std::memcmp(bytes.data(),second.data(),length))))
        return fail("player conditions changed during the snapshot");
    value.type=FDKSS_INTEGER;value.integer=vector.count;if(emit(sink,&value))return 1;
    value.type=FDKSS_STRING;value.string=reinterpret_cast<const char *>(bytes.data());value.string_size=length;
    return emit(sink,&value);
}
}
