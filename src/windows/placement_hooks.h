#pragma once
#include "placement.h"
namespace placement_hooks {
bool start(std::uintptr_t image,bool second,fdkss_runtime *,unsigned long owner);
bool stop();
bool active();
character_placement::Setter original_for(std::uintptr_t address);
void service_probe();
int FDKSS_CALL info(void *,const fdkss_value *,std::size_t,fdkss_result_fn,void *,char *,std::size_t);
int FDKSS_CALL request_probe(void *,const fdkss_value *,std::size_t,fdkss_result_fn,void *,char *,std::size_t);
int FDKSS_CALL probe_result(void *,const fdkss_value *,std::size_t,fdkss_result_fn,void *,char *,std::size_t);
}
