#ifndef FDKSS_PLUGIN_H
#define FDKSS_PLUGIN_H
#include "api.h"
#if defined(_WIN32)
#define FDKSS_PLUGIN_EXPORT __declspec(dllexport)
#else
#define FDKSS_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define FDKSS_PLUGIN_ABI_VERSION 1u
/* All host functions run synchronously on the runtime owner thread.
   The host and context are borrowed until stop returns. Do not retain them afterward.
   Registration is allowed only during start; bindings belong to this mod's Lua state.
   Use the host table, not imported fdkss.dll symbols. No work in DllMain. */
typedef struct fdkss_plugin_host {
    uint32_t size, abi_version;
    void *context;
    const char *mod_name;
    void (FDKSS_CALL *log)(void *context, const char *message);
    fdkss_status (FDKSS_CALL *register_string)(void *context, const char *name,
        fdkss_native_fn, void *user);
    fdkss_status (FDKSS_CALL *register_typed)(void *context, const char *name,
        fdkss_typed_native_fn, void *user);
    fdkss_status (FDKSS_CALL *call_string)(void *context, const char *name,
        const char *input, char *output, size_t capacity);
    fdkss_status (FDKSS_CALL *invoke)(void *context, const char *name,
        const fdkss_value *, size_t count, fdkss_result_fn, void *sink,
        char *error, size_t error_capacity);
} fdkss_plugin_host;
typedef struct fdkss_plugin_exports {
    uint32_t size, abi_version;
    /* Set *state to plugin-owned storage. Stop must handle partially initialized state. */
    int (FDKSS_CALL *start)(const fdkss_plugin_host *, void **state, char *error, size_t capacity);
    int (FDKSS_CALL *update)(void *state, double delta_seconds, char *error, size_t capacity);
    /* Custom typed/string events, including observer hooks. Start/stop/update use lifecycle functions. */
    int (FDKSS_CALL *event)(void *state, const char *event, const fdkss_value *, size_t count,
        char *error, size_t capacity);
    /* Join plugin-owned threads and release state before returning. Required, even without state. */
    void (FDKSS_CALL *stop)(void *state);
    /* Optional ABI 1 tail extension. Return a replacement scalar in value.
       Core validates its type/range. Args are read-only; observer event follows.
       Older plugins may export only FDKSS_PLUGIN_EXPORTS_BASE_SIZE bytes. */
    int (FDKSS_CALL *filter)(void *state, const char *event, const fdkss_value *, size_t count,
        size_t writable_index, fdkss_value *value, char *error, size_t capacity);
} fdkss_plugin_exports;
#define FDKSS_PLUGIN_EXPORTS_BASE_SIZE offsetof(fdkss_plugin_exports, filter)
/* Export this function from Native/main.dll. Query must not start engine work.
   Return nonzero if the requested ABI or output capacity is unsupported. */
typedef int (FDKSS_CALL *fdkss_plugin_query_fn)(uint32_t abi_version,
    fdkss_plugin_exports *output, size_t capacity);
#ifdef __cplusplus
}
#endif
#endif
