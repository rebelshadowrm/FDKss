#pragma once
#include <cstddef>
#include <cstdint>
#include "fdkss/api.h"
namespace sky1_scripts {
void bind_jobs(fdkss_runtime *, void *catalog_profile);
void service_jobs(fdkss_runtime *, float);
void stop_jobs();
// Optional read-only catalog layout. Null context retains the pinned Sky 1 layout.
// This does not adapt the VM interpreter or its command hooks.
struct CatalogProfile {
    std::uintptr_t manager_global, manager_vtable, framework_global, framework_vtable;
    std::size_t worker_offset;
};
int FDKSS_CALL names(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL info(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL functions(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL function_info(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL function_at(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL variables(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL variable_info(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL set_variable(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL self_test(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL call_pure(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL call_compiled(void *,const fdkss_value *,std::size_t,fdkss_result_fn,void *,char *,std::size_t);
int FDKSS_CALL preview_compiled_call(void *,const fdkss_value *,std::size_t,fdkss_result_fn,void *,char *,std::size_t);
int FDKSS_CALL pure_self_test(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL arithmetic_self_test(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL preview_pure(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL float_self_test(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL native_math_self_test(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL branch_self_test(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL loop_self_test(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL budget_self_test(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL lease_self_test(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL global_self_test(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL nested_self_test(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL comparison_self_test(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL default_self_test(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
bool constant_code(const unsigned char *, std::size_t, std::uint32_t &);
int FDKSS_CALL call_constant(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
}
