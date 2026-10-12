#pragma once
#include <cstddef>
#include <cstdint>
#include "fdkss/api.h"
namespace vm_hooks {
bool start(void ***slots, std::size_t count, void *expected, fdkss_runtime *runtime, unsigned long owner);
bool stop();
}
