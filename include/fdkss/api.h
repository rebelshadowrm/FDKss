#ifndef FDKSS_API_H
#define FDKSS_API_H
#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32)
# if defined(FDKSS_BUILD)
#  define FDKSS_API __declspec(dllexport)
# else
#  define FDKSS_API __declspec(dllimport)
# endif
# define FDKSS_CALL __cdecl
#else
# define FDKSS_API __attribute__((visibility("default")))
# define FDKSS_CALL
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define FDKSS_ABI_VERSION 1u
typedef struct fdkss_runtime fdkss_runtime;
typedef enum fdkss_status {
    FDKSS_OK = 0, FDKSS_INVALID_ARGUMENT = 1, FDKSS_WRONG_THREAD = 2,
    FDKSS_REENTRANT = 3, FDKSS_ERROR = 4
} fdkss_status;
/* Strings are UTF-8, borrowed only for the duration of the callback. */
typedef void (FDKSS_CALL *fdkss_log_fn)(void *user, const char *mod, const char *message);
/* Return 0 on success. Write a NUL-terminated result/error into output.
   Native functions and log callbacks must not reenter the runtime. */
typedef int (FDKSS_CALL *fdkss_native_fn)(void *user, const char *input,
                                       char *output, size_t capacity);
/* Additive ABI 1 extension. Typed calls use fdk.invoke(name, ...).
   Strings may contain NUL bytes; string_size is authoritative. No coercion.
   Arguments and emitted values are borrowed during each callback only. */
typedef enum fdkss_value_type {
    FDKSS_NIL = 0, FDKSS_BOOLEAN = 1, FDKSS_INTEGER = 2,
    FDKSS_NUMBER = 3, FDKSS_STRING = 4
} fdkss_value_type;
typedef struct fdkss_value {
    fdkss_value_type type;
    int boolean;
    int64_t integer;
    double number;
    const char *string;
    size_t string_size;
} fdkss_value;
/* Emit results in order. Returns nonzero when invalid or over the limit.
   Maximum: 64 arguments/results and 16 KiB of result strings per call.
   Emit only synchronously from this callback; never retain the sink. */
typedef int (FDKSS_CALL *fdkss_result_fn)(void *sink, const fdkss_value *value);
typedef int (FDKSS_CALL *fdkss_typed_native_fn)(void *user,
    const fdkss_value *arguments, size_t count, fdkss_result_fn emit, void *sink,
    char *error, size_t error_capacity);
/* All operations, including destruction, run on the creating/game thread.
   Handles are single-threaded; do not call concurrently. No work in DllMain. */
FDKSS_API uint32_t FDKSS_CALL fdkss_abi_version(void);
FDKSS_API fdkss_status FDKSS_CALL fdkss_create(const char *mods_directory,
    fdkss_log_fn logger, void *user, fdkss_runtime **out);
FDKSS_API fdkss_status FDKSS_CALL fdkss_register_native(fdkss_runtime *,
    const char *name, fdkss_native_fn callback, void *user);
FDKSS_API fdkss_status FDKSS_CALL fdkss_register_typed_native(fdkss_runtime *,
    const char *name, fdkss_typed_native_fn callback, void *user);
FDKSS_API fdkss_status FDKSS_CALL fdkss_start(fdkss_runtime *);
FDKSS_API fdkss_status FDKSS_CALL fdkss_tick(fdkss_runtime *, double delta_seconds);
/* Custom events cannot use reserved names: start, update, stop. */
FDKSS_API fdkss_status FDKSS_CALL fdkss_emit(fdkss_runtime *, const char *event,
                                          const char *payload);
FDKSS_API fdkss_status FDKSS_CALL fdkss_emit_values(fdkss_runtime *, const char *event,
    const fdkss_value *arguments, size_t count);
/* Hook callbacks may return a replacement for one scalar argument. Nil leaves it
   unchanged. Types must match; numeric replacements must be finite and in bounds.
   Erroring/invalid callbacks are disabled while other subscribers continue. */
/* Number bounds always apply. Integer bounds apply when minimum < maximum;
   equal bounds leave integers unrestricted (existing raw-return hooks use 0,0). */
FDKSS_API fdkss_status FDKSS_CALL fdkss_filter_values(fdkss_runtime *, const char *event,
    fdkss_value *arguments, size_t count, size_t writable_index, double minimum, double maximum);
/* Explicit hot reload at a frame boundary. Replaces only a successfully loaded mod. */
FDKSS_API fdkss_status FDKSS_CALL fdkss_reload(fdkss_runtime *, const char *mod_name);
/* Stop/unload one loaded mod; does not edit its enabled marker or other mods. */
FDKSS_API fdkss_status FDKSS_CALL fdkss_unload(fdkss_runtime *, const char *mod_name);
FDKSS_API fdkss_status FDKSS_CALL fdkss_mod_count(fdkss_runtime *, size_t *out);
FDKSS_API fdkss_status FDKSS_CALL fdkss_mod_name(fdkss_runtime *, size_t index, char *output, size_t capacity);
/* Additive ABI 1: opaque identities distinguish successful reloads of the same
   mod. Caller identity is available only inside a typed native callback and may
   be queried there despite runtime reentry restrictions. Zero means unavailable.
   Liveness queries run on the owner thread outside callbacks; failed reloads
   retain the previous identity. These functions never invoke mod code. */
FDKSS_API uint64_t FDKSS_CALL fdkss_native_caller(const fdkss_runtime *);
FDKSS_API int FDKSS_CALL fdkss_mod_alive(const fdkss_runtime *, uint64_t identity);
/* Execute a UTF-8 text chunk in an existing mod's state at an owner-thread boundary.
   Returns tab-separated tostring results. Errors are available through last_error.
   Execution side effects persist even if result formatting/capacity fails. */
FDKSS_API fdkss_status FDKSS_CALL fdkss_execute(fdkss_runtime *, const char *mod_name,
    const char *source, char *output, size_t capacity);
/* Borrowed until the next API operation; query only on the creating thread. */
FDKSS_API const char *FDKSS_CALL fdkss_last_error(const fdkss_runtime *);
FDKSS_API fdkss_status FDKSS_CALL fdkss_destroy(fdkss_runtime *);
#ifdef __cplusplus
}
#endif
#endif
