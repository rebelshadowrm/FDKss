#pragma once
#include "fdkss/api.h"
#include <cstdint>
#include <cstddef>
#include <string_view>
namespace actor_methods {
// Caller must resolve a current covered Actor base and enforce its owner thread.
int transform(void *actor, std::uintptr_t getter, fdkss_result_fn, void *, char *, std::size_t);
int type(void *actor, std::uintptr_t base, bool sky2, std::string_view method,
    fdkss_result_fn, void *, char *, std::size_t);
int state_byte(void *actor,std::uintptr_t base,bool sky2,const fdkss_value *,std::size_t,
    fdkss_result_fn,void *,char *,std::size_t);
}
