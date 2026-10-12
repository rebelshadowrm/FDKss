#pragma once
#include "fdkss/api.h"
#include <cstdint>
#include <cstddef>
namespace player_resources {struct SetterProfile;}
namespace resource_hooks {
using Setter=void (__fastcall *)(void *,std::uint32_t,std::int32_t);
bool start(std::uintptr_t image,bool second,fdkss_runtime *,unsigned long owner);
bool stop();
bool active();
Setter checked_target(std::uintptr_t,const player_resources::SetterProfile &);
int FDKSS_CALL info(void *,const fdkss_value *,std::size_t,fdkss_result_fn,void *,char *,std::size_t);
}
