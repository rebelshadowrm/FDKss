#pragma once
#include <cstdint>
namespace actor_types {
struct Profile {
    std::uintptr_t table, name, id, flags, text;
    std::uint32_t expected_id, expected_flags;
    const char *class_name;
};
#define FDKSS_ACTOR_TYPE(table,name,id,flags,text,expected_id,expected_flags,class_name) {table,name,id,flags,text,expected_id,expected_flags,class_name},
inline constexpr Profile first[]{
#include "fdkss/sky1_actor_types.inc"
}, second[]{
#include "fdkss/sky2_actor_types.inc"
};
#undef FDKSS_ACTOR_TYPE
}
