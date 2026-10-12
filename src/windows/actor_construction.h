#pragma once
#include "fdkss/api.h"
#include <cstddef>
#include <cstdint>
namespace actor_construction {
bool start(std::uintptr_t base, bool sky2, unsigned long owner);
bool stop();
void emit_events(fdkss_runtime *);
int FDKSS_CALL info(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL self_test(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
}
