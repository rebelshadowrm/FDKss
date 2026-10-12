#include "fdkss/plugin.h"
#include <cstdio>
#include <cstring>
#include <string_view>
#include <thread>
#ifndef FDKSS_FIXTURE_BUILD
#define FDKSS_FIXTURE_BUILD 1
#endif
struct State { const fdkss_plugin_host *host; double elapsed = 0; };
static int FDKSS_CALL elapsed(void *user, const fdkss_value *, size_t count,
    fdkss_result_fn emit, void *sink, char *error, size_t capacity) {
    if (count) { std::snprintf(error, capacity, "elapsed takes no arguments"); return 1; }
    fdkss_value value{}; value.type = FDKSS_NUMBER; value.number = static_cast<State *>(user)->elapsed;
    return emit(sink, &value);
}
static int FDKSS_CALL version(void *, const char *, char *output, size_t capacity) {
    std::snprintf(output, capacity, "build-%d", FDKSS_FIXTURE_BUILD); return 0;
}
static int FDKSS_CALL capture(void *user, const fdkss_value *value) {
    if (value->type != FDKSS_INTEGER || value->integer != 42) return 1;
    *static_cast<bool *>(user) = true; return 0;
}
static int FDKSS_CALL start(const fdkss_plugin_host *host, void **output, char *error, size_t capacity) {
    if (host->size != sizeof(*host) || host->abi_version != FDKSS_PLUGIN_ABI_VERSION) return 1;
    auto *state = new State{host}; *output = state;
    host->log(host->context, "native-start");
    if (host->register_typed(host->context, "native.elapsed", elapsed, state) != FDKSS_OK ||
        host->register_string(host->context, "native.version", version, nullptr) != FDKSS_OK) return 1;
    fdkss_status other = FDKSS_OK;
    std::thread thread([&] { other = host->register_typed(host->context, "native.other", elapsed, state); }); thread.join();
    if (other != FDKSS_WRONG_THREAD) return 1;
    char response[128];
    if (host->call_string(host->context, "test.echo", "plugin", response, sizeof(response)) != FDKSS_OK ||
        std::string_view(response) != "echo:plugin") return 1;
    fdkss_value argument{}; argument.type = FDKSS_INTEGER; argument.integer = 42;
    bool captured = false;
    if (host->invoke(host->context, "typed.echo", &argument, 1, capture, &captured, error, capacity) != FDKSS_OK || !captured) return 1;
    if (std::string_view(host->mod_name) == "StartFailure") {
        std::snprintf(error, capacity, "fixture start failure"); return 1;
    }
    return 0;
}
static int FDKSS_CALL update(void *user, double delta, char *error, size_t capacity) {
    auto *state = static_cast<State *>(user);
    state->elapsed += delta;
    state->host->log(state->host->context, "native-update");
    if (state->host->register_typed(state->host->context, "native.late", elapsed, state) != FDKSS_ERROR) return 1;
    if (std::string_view(state->host->mod_name) == "UpdateFailure") {
        std::snprintf(error, capacity, "fixture update failure"); return 1;
    }
    return 0;
}
static int FDKSS_CALL event(void *user, const char *name, const fdkss_value *values, size_t count,
    char *error, size_t capacity) {
    auto *state = static_cast<State *>(user);
    if (std::string_view(name) != "custom.native") return 0;
    state->host->log(state->host->context, "native-event");
    if (count != 1 || values[0].type != FDKSS_INTEGER || values[0].integer != 42) return 1;
    if (std::string_view(state->host->mod_name) == "EventFailure") {
        std::snprintf(error, capacity, "fixture event failure"); return 1;
    }
    return 0;
}
static void FDKSS_CALL stop(void *user) {
    auto *state = static_cast<State *>(user);
    if (state) { state->host->log(state->host->context, "native-stop"); delete state; }
}
static int FDKSS_CALL filter(void *user, const char *name, const fdkss_value *, size_t count, size_t index,
    fdkss_value *value, char *, size_t) {
    auto *state = static_cast<State *>(user);
    if (std::string_view(name) != "custom.filter" || count != 1 || index != 0) return 0;
    if (std::string_view(state->host->mod_name) == "FilterNative") value->integer += 10;
    if (std::string_view(state->host->mod_name) == "FilterFailure") value->type = FDKSS_STRING;
    return 0;
}
extern "C" FDKSS_PLUGIN_EXPORT int FDKSS_CALL FDKssPluginQuery(uint32_t abi, fdkss_plugin_exports *output, size_t capacity) {
    if (abi != FDKSS_PLUGIN_ABI_VERSION || !output || capacity < FDKSS_PLUGIN_EXPORTS_BASE_SIZE) return 1;
#if defined(FDKSS_FIXTURE_LEGACY_ABI)
    if (capacity != FDKSS_PLUGIN_EXPORTS_BASE_SIZE) return 1;
#endif
    const size_t bytes = capacity >= sizeof(*output) ? sizeof(*output) : FDKSS_PLUGIN_EXPORTS_BASE_SIZE;
    fdkss_plugin_exports table{}; table.size = static_cast<uint32_t>(bytes); table.abi_version = FDKSS_PLUGIN_ABI_VERSION;
#if defined(FDKSS_FIXTURE_BAD_ABI)
    table.abi_version = 999;
#endif
    table.start = start; table.update = update; table.event = event; table.stop = stop; table.filter = filter;
    std::memcpy(output, &table, bytes);
    return 0;
}
