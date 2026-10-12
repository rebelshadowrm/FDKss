#pragma once
#include "fdkss/api.h"
#include <cstdint>
#include <cstddef>
namespace actor_hooks {
bool start(std::uintptr_t base, bool sky2, fdkss_runtime *, unsigned long owner);
bool stop();
bool owns(void **slot, void *original);
bool active();
bool probe_pending();
void service_probe(std::uintptr_t current_actor);
int FDKSS_CALL request_probe(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL probe_result(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL info(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
}
