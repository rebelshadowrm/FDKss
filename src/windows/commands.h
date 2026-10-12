#pragma once
#include <cstddef>
#include "fdkss/api.h"
namespace native_commands {
int FDKSS_CALL groups(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL info(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL page(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
}
