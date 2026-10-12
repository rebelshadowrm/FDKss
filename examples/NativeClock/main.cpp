#include "fdkss/plugin.h"
#include <cstdio>
#include <cstring>
#include <string_view>
struct Clock {
    const fdkss_plugin_host *host;
    double elapsed = 0;
    int64_t frames = 0, pre = 0, post = 0, thread = 0;
};
static int FDKSS_CALL clock_values(void *user, const fdkss_value *, size_t count,
    fdkss_result_fn emit, void *sink, char *error, size_t capacity) {
    if (count) { std::snprintf(error, capacity, "native.clock takes no arguments"); return 1; }
    auto &clock = *static_cast<Clock *>(user);
    fdkss_value value{}; value.type = FDKSS_NUMBER; value.number = clock.elapsed;
    if (emit(sink, &value)) return 1;
    value.type = FDKSS_INTEGER;
    for (const auto integer : {clock.frames, clock.pre, clock.post, clock.thread}) {
        value.integer = integer; if (emit(sink, &value)) return 1;
    }
    return 0;
}
static int FDKSS_CALL record_thread(void *user, const fdkss_value *value) {
    if (value->type == FDKSS_INTEGER) static_cast<Clock *>(user)->thread = value->integer;
    return 0;
}
static int FDKSS_CALL start(const fdkss_plugin_host *host, void **state, char *error, size_t capacity) {
    if (host->size != sizeof(*host) || host->abi_version != FDKSS_PLUGIN_ABI_VERSION) return 1;
    auto *clock = new Clock{host}; *state = clock;
    if (host->register_typed(host->context, "native.clock", clock_values, clock) != FDKSS_OK) {
        std::snprintf(error, capacity, "could not register native.clock"); return 1;
    }
    fdkss_value args[2]{};
    args[0].type = FDKSS_STRING; args[0].string = "framework"; args[0].string_size = 9;
    args[1].type = FDKSS_STRING; args[1].string = "update_thread"; args[1].string_size = 13;
    // The standalone host has no engine backend, which is also supported by this example.
    host->invoke(host->context, "engine.get", args, 2, record_thread, clock, error, capacity);
    host->log(host->context, "NativeClock started");
    return 0;
}
static int FDKSS_CALL update(void *state, double delta, char *, size_t) {
    auto &clock = *static_cast<Clock *>(state);
    clock.elapsed += delta; ++clock.frames;
    return 0;
}
static int FDKSS_CALL event(void *state, const char *name, const fdkss_value *, size_t, char *, size_t) {
    auto &clock = *static_cast<Clock *>(state);
    if (std::string_view(name) == "hook.application.update.pre") ++clock.pre;
    if (std::string_view(name) == "hook.application.update.post") ++clock.post;
    return 0;
}
static void FDKSS_CALL stop(void *state) {
    auto *clock = static_cast<Clock *>(state);
    if (clock) { clock->host->log(clock->host->context, "NativeClock stopped"); delete clock; }
}
extern "C" FDKSS_PLUGIN_EXPORT int FDKSS_CALL FDKssPluginQuery(uint32_t abi, fdkss_plugin_exports *output, size_t capacity) {
    if (abi != FDKSS_PLUGIN_ABI_VERSION || !output || capacity < FDKSS_PLUGIN_EXPORTS_BASE_SIZE) return 1;
    const size_t bytes = capacity >= sizeof(*output) ? sizeof(*output) : FDKSS_PLUGIN_EXPORTS_BASE_SIZE;
    fdkss_plugin_exports table{}; table.size = static_cast<uint32_t>(bytes); table.abi_version = FDKSS_PLUGIN_ABI_VERSION;
    table.start = start; table.update = update; table.event = event; table.stop = stop;
    std::memcpy(output, &table, bytes);
    return 0;
}
