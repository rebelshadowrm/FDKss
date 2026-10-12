#include "fdkss/api.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
namespace fs = std::filesystem;
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
struct Fixture {
    fs::path root = fs::temp_directory_path() /
        ("fdkss-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fdkss_runtime *runtime{};
    std::vector<std::string> logs;
    ~Fixture() {
        if (runtime) fdkss_destroy(runtime);
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }
    void script(const char *name, const char *source, bool enabled = true) {
        const auto directory = root / name;
        fs::create_directories(directory / "Scripts");
        std::ofstream(directory / "Scripts" / "main.lua") << source;
        if (enabled) std::ofstream(directory / "enabled.txt") << "1";
    }
    static void FDKSS_CALL logger(void *user, const char *mod, const char *message) {
        static_cast<Fixture *>(user)->logs.push_back(std::string(mod) + ":" + message);
    }
    static int FDKSS_CALL native(void *user, const char *input, char *out, size_t capacity) {
        auto *fixture = static_cast<Fixture *>(user);
        require(fdkss_tick(fixture->runtime, 0.1) == FDKSS_REENTRANT, "native reentry was allowed");
        const int count = std::snprintf(out, capacity, "echo:%s", input);
        return count < 0 || static_cast<size_t>(count) >= capacity;
    }
    void create() {
        fs::create_directories(root);
        const auto path = root.u8string();
        require(fdkss_create(reinterpret_cast<const char *>(path.c_str()), logger, this, &runtime) == FDKSS_OK,
                "create failed");
    }
    size_t matches(const std::string &message) const {
        return static_cast<size_t>(std::count(logs.begin(), logs.end(), message));
    }
};
void test_lifecycle_and_reload() {
    Fixture f;
    f.script("Alpha", R"(
        shared = 'alpha'
        local helper = require('helper')
        fdk.on('start', function() print('started', helper); fdk.log(fdk.call('test.echo', 'hi')) end)
        fdk.on('update', function(dt) assert(dt == 0.25); fdk.log('old') end)
        fdk.on('map.loaded', function(name) fdk.log(name) end)
        fdk.on('stop', function() fdk.log('stopped') end)
    )");
    std::ofstream(f.root / "Alpha" / "Scripts" / "helper.lua") << "return 'module'";
    f.script("Beta", "assert(shared == nil); fdk.on('update', function() fdk.log('beta') end)");
    f.script("Disabled", "error('disabled script ran')", false);
    f.script("SyntaxError", "this is not Lua !");
    f.create();
    require(fdkss_register_native(f.runtime, "test.echo", Fixture::native, &f) == FDKSS_OK, "register failed");
    require(fdkss_register_native(f.runtime, "test.echo", Fixture::native, &f) == FDKSS_ERROR, "duplicate native allowed");
    require(fdkss_tick(f.runtime, 0) == FDKSS_ERROR, "tick before start allowed");
    require(fdkss_start(f.runtime) == FDKSS_OK, "start failed");
    size_t count = 0;
    require(fdkss_mod_count(f.runtime, &count) == FDKSS_OK && count == 2, "bad mod count");
    require(f.matches("Alpha:started\tmodule") == 1, "print/module failed");
    require(f.matches("Alpha:echo:hi") == 1, "native result missing");
    require(fdkss_start(f.runtime) == FDKSS_ERROR, "double start allowed");
    require(fdkss_tick(f.runtime, -1) == FDKSS_INVALID_ARGUMENT, "negative delta allowed");
    require(fdkss_tick(f.runtime, 0.25) == FDKSS_OK, "tick failed");
    require(f.matches("Alpha:old") == 1 && f.matches("Beta:beta") == 1, "callbacks failed");
    require(fdkss_emit(f.runtime, "map.loaded", "Rolent") == FDKSS_OK, "event failed");
    require(f.matches("Alpha:Rolent") == 1, "payload missing");
    require(fdkss_emit(f.runtime, "stop", "") == FDKSS_INVALID_ARGUMENT, "reserved event allowed");
    f.script("Alpha", "bad Lua !!!");
    require(fdkss_reload(f.runtime, "Alpha") == FDKSS_ERROR, "invalid reload accepted");
    require(std::string(fdkss_last_error(f.runtime)).find("retained") != std::string::npos, "reload error missing");
    fdkss_tick(f.runtime, 0.25);
    require(f.matches("Alpha:old") == 2 && f.matches("Alpha:stopped") == 0, "failed reload lost old mod");
    f.script("Alpha", "fdk.on('start', function() error('startup failure') end)");
    require(fdkss_reload(f.runtime, "Alpha") == FDKSS_ERROR, "failed startup accepted");
    f.script("Alpha", "fdk.on('update', function() fdk.log('new') end); fdk.on('stop', function() fdk.log('newstop') end)");
    require(fdkss_reload(f.runtime, "Alpha") == FDKSS_OK, "reload failed");
    require(f.matches("Alpha:stopped") == 1, "old mod was not stopped");
    fdkss_tick(f.runtime, 0.25);
    require(f.matches("Alpha:new") == 1, "new callback not active");
    require(fdkss_reload(f.runtime, "../escape") == FDKSS_INVALID_ARGUMENT, "traversal accepted");
    fdkss_status wrong_thread = FDKSS_OK;
    std::thread thread([&] { wrong_thread = fdkss_tick(f.runtime, 0.25); }); thread.join();
    require(wrong_thread == FDKSS_WRONG_THREAD, "wrong thread accepted");
    require(fdkss_destroy(f.runtime) == FDKSS_OK, "destroy failed"); f.runtime = nullptr;
    require(f.matches("Alpha:newstop") == 1, "stop failed");
}
void test_error_isolation() {
    Fixture f;
    f.script("Broken", R"(
        fdk.on('update', function() error('deliberate failure') end)
        fdk.on('update', function() fdk.log('healthy') end)
        fdk.on('custom', function() fdk.on('update', function() end) end)
    )");
    f.script("Healthy", "fdk.on('update', function() fdk.log('running') end)");
    f.create(); fdkss_start(f.runtime);
    fdkss_tick(f.runtime, 0.1); fdkss_tick(f.runtime, 0.1);
    require(f.matches("Broken:healthy") == 2 && f.matches("Healthy:running") == 2, "error killed other callbacks");
    size_t failures = 0;
    for (const auto &line : f.logs) if (line.find("deliberate failure") != std::string::npos) ++failures;
    require(failures == 1, "failing callback was not disabled");
    fdkss_emit(f.runtime, "custom", "");
    fdkss_tick(f.runtime, 0.1);
    require(f.matches("Broken:healthy") == 3, "registration during callback corrupted handlers");
}
void test_invalid_inputs() {
    require(fdkss_abi_version() == 1, "bad ABI version");
    require(fdkss_tick(nullptr, 0) == FDKSS_INVALID_ARGUMENT, "null runtime accepted");
    fdkss_runtime *runtime = reinterpret_cast<fdkss_runtime *>(1);
    require(fdkss_create("", nullptr, nullptr, &runtime) == FDKSS_INVALID_ARGUMENT && !runtime, "bad create output");
    Fixture f; f.create();
    require(fdkss_start(f.runtime) == FDKSS_OK, "empty start failed");
    size_t count = 99; fdkss_mod_count(f.runtime, &count); require(count == 0, "empty mod count");
#if defined(_WIN32)
    auto attach = reinterpret_cast<DWORD (WINAPI *)(void *)>(GetProcAddress(GetModuleHandleW(L"fdkss.dll"), "FDKssAttach"));
    require(attach && attach(nullptr) == ERROR_BAD_EXE_FORMAT, "unsupported executable was not rejected");
#endif
}
int FDKSS_CALL typed_echo(void *user, const fdkss_value *args, size_t count,
    fdkss_result_fn emit, void *sink, char *, size_t) {
    auto &f = *static_cast<Fixture *>(user);
    require(fdkss_tick(f.runtime, 0) == FDKSS_REENTRANT, "typed native allowed reentry");
    for (size_t i = 0; i < count; ++i) if (emit(sink, &args[i])) return 1;
    return 0;
}
int FDKSS_CALL typed_invalid(void *, const fdkss_value *, size_t,
    fdkss_result_fn emit, void *sink, char *, size_t) {
    fdkss_value value{}; value.type = FDKSS_STRING; value.string_size = 1;
    emit(sink, &value); // Deliberately ignore sink failure: runtime must still reject.
    return 0;
}
int FDKSS_CALL typed_overflow(void *, const fdkss_value *, size_t,
    fdkss_result_fn emit, void *sink, char *, size_t) {
    fdkss_value value{};
    for (int i = 0; i < 65; ++i) emit(sink, &value);
    return 0;
}
int FDKSS_CALL typed_throw(void *, const fdkss_value *, size_t,
    fdkss_result_fn, void *, char *, size_t) { throw std::runtime_error("test"); }
void test_typed_natives() {
    Fixture f;
    f.script("Typed", R"(
        local binary = 'a\0b'
        local a,b,c,d,e = fdk.invoke('typed.echo', nil, true, 9223372036854775807, 1.25, binary)
        assert(a == nil and b == true and c == 9223372036854775807 and math.type(c) == 'integer')
        assert(d == 1.25 and e == binary and #e == 3)
        assert(select('#', fdk.invoke('typed.echo', nil)) == 1)
        assert(select('#', fdk.invoke('typed.echo')) == 0)
        assert(not pcall(fdk.invoke, 'typed.echo', {}))
        assert(not pcall(fdk.invoke, 'typed.echo', 0/0))
        assert(not pcall(fdk.invoke, 'typed.echo', math.huge))
        assert(not pcall(fdk.invoke, 'typed.missing'))
        assert(not pcall(fdk.invoke, 'typed.invalid'))
        assert(not pcall(fdk.invoke, 'typed.overflow'))
        assert(not pcall(fdk.invoke, 'typed.throw'))
        assert(not pcall(fdk.invoke, 'typed.echo', table.unpack({}, 1, 65)))
        local values = table.pack(fdk.invoke('typed.echo', table.unpack({}, 1, 64)))
        assert(values.n == 64)
        fdk.log('typed passed')
    )");
    f.create();
    require(fdkss_register_typed_native(f.runtime, "typed.echo", typed_echo, &f) == FDKSS_OK, "typed registration failed");
    require(fdkss_register_native(f.runtime, "typed.echo", Fixture::native, &f) == FDKSS_ERROR, "cross registry duplicate allowed");
    require(fdkss_register_typed_native(f.runtime, "typed.echo", typed_echo, &f) == FDKSS_ERROR, "typed duplicate allowed");
    fdkss_register_typed_native(f.runtime, "typed.invalid", typed_invalid, nullptr);
    fdkss_register_typed_native(f.runtime, "typed.overflow", typed_overflow, nullptr);
    fdkss_register_typed_native(f.runtime, "typed.throw", typed_throw, nullptr);
    require(fdkss_start(f.runtime) == FDKSS_OK, "typed start failed");
    require(f.matches("Typed:typed passed") == 1, "typed conversions/failure isolation failed");
    require(fdkss_register_typed_native(f.runtime, "typed.late", typed_echo, &f) == FDKSS_ERROR, "late registration allowed");
}
void test_console() {
    Fixture f;
    f.script("Console", "fdk.on('update', function() fdk.log('alive') end)");
    f.create(); fdkss_start(f.runtime);
    char result[256]{};
    require(fdkss_execute(f.runtime, "Console", "counter = 4; return counter, true, nil", result, sizeof(result)) == FDKSS_OK &&
        std::string(result) == "4\ttrue\tnil", "console results failed");
    require(fdkss_execute(f.runtime, "Console", "counter = counter + 1; return counter", result, sizeof(result)) == FDKSS_OK &&
        std::string(result) == "5", "console did not persist globals");
    require(fdkss_execute(f.runtime, "Console", "return 'a\\0b'", result, sizeof(result)) == FDKSS_OK &&
        std::string(result) == "a\\0b", "console truncated binary string");
    char large[13000]{};
    require(fdkss_execute(f.runtime, "Console", "return string.rep('x', 9000), 42", large, sizeof(large)) == FDKSS_OK &&
        std::string(large) == std::string(9000, 'x') + "\t42", "large console result corrupted Lua buffer");
    require(fdkss_execute(f.runtime, "Console", "print(string.rep('q', 9000)); return 'printed'", result, sizeof(result)) == FDKSS_OK &&
        f.matches("Console:" + std::string(9000, 'q')) == 1, "large print corrupted Lua buffer");
    require(fdkss_execute(f.runtime, "Console", "error('console failure')", result, sizeof(result)) == FDKSS_ERROR,
        "console runtime error allowed");
    require(fdkss_execute(f.runtime, "Console", "syntax !", result, sizeof(result)) == FDKSS_ERROR, "console syntax error allowed");
    require(fdkss_execute(f.runtime, "Console", "return setmetatable({}, {__tostring=function() error('format failure') end})", result, sizeof(result)) == FDKSS_ERROR,
        "console formatting error escaped protection");
    require(fdkss_execute(f.runtime, "Console", "fdk.on('update', function() end)", result, sizeof(result)) == FDKSS_ERROR,
        "console registered callbacks outside initialization");
    require(fdkss_execute(f.runtime, "Console", "return 'too long'", result, 2) == FDKSS_ERROR, "console result overflow allowed");
    require(fdkss_execute(f.runtime, "Missing", "return 1", result, sizeof(result)) == FDKSS_ERROR, "missing console mod allowed");
    require(fdkss_execute(f.runtime, "Console", "return 42", result, sizeof(result)) == FDKSS_OK && std::string(result) == "42",
        "console did not recover after failures");
    fdkss_tick(f.runtime, 0.1);
    require(f.matches("Console:alive") == 1, "console failure killed update callbacks");
}
void test_typed_events_and_removal() {
    Fixture f;
    f.script("Events", R"(
        local token
        token = fdk.on('typed.event', function(n, b, s, empty)
            assert(n == 42 and b == true and s == 'a\0b' and empty == nil)
            assert(fdk.off(token))
            fdk.log('once')
        end)
        local removed = fdk.on('typed.event', function() error('removed callback ran') end)
        assert(fdk.off(removed) and not fdk.off(removed))
        fdk.on('typed.event', function() fdk.log('persistent') end)
        assert(not fdk.off(removed)) -- Registry reference reuse must not reuse tokens.
    )");
    f.create(); fdkss_start(f.runtime);
    fdkss_value values[4]{};
    values[0].type = FDKSS_INTEGER; values[0].integer = 42;
    values[1].type = FDKSS_BOOLEAN; values[1].boolean = 1;
    values[2].type = FDKSS_STRING; values[2].string = "a\0b"; values[2].string_size = 3;
    require(fdkss_emit_values(f.runtime, "typed.event", values, 4) == FDKSS_OK, "typed event failed");
    fdkss_emit_values(f.runtime, "typed.event", values, 4);
    require(f.matches("Events:once") == 1 && f.matches("Events:persistent") == 2, "callback removal failed");
    require(fdkss_emit_values(f.runtime, "update", values, 4) == FDKSS_INVALID_ARGUMENT, "reserved typed event allowed");
    require(fdkss_emit_values(f.runtime, "typed.event", nullptr, 1) == FDKSS_INVALID_ARGUMENT, "null arguments allowed");
    values[2].string = nullptr;
    require(fdkss_emit_values(f.runtime, "typed.event", values, 4) == FDKSS_INVALID_ARGUMENT, "invalid string allowed");
}
void test_hook_replacements() {
    Fixture f;
    f.script("Alpha", R"(
        fdk.on('filter.number', function(n) return n / 2 end)
        fdk.on('filter.integer', function(n) return n + 1 end)
        fdk.on('filter.boolean', function(b) return not b end)
        fdk.on('filter.many', function(...) assert(select('#', ...) == 64); return select(64, ...) + 1 end)
    )");
    f.script("Broken", R"(
        fdk.on('filter.number', function() return math.huge end)
        fdk.on('filter.integer', function() return 1.5 end)
        fdk.on('filter.boolean', function() return 1 end)
    )");
    f.script("Zeta", R"(
        fdk.on('filter.number', function(n) assert(n == 0.25); return n + 0.1 end)
        fdk.on('filter.integer', function(n) assert(n == 43); return n + 1 end)
        fdk.on('filter.boolean', function(b) assert(b == false) end)
        fdk.on('update', function() fdk.log('alive') end)
    )");
    f.create(); fdkss_start(f.runtime);
    fdkss_value value{}; value.type = FDKSS_NUMBER;
    for (int i = 0; i < 2; ++i) {
        value.number = 0.5;
        require(fdkss_filter_values(f.runtime, "filter.number", &value, 1, 0, 0, 1) == FDKSS_OK &&
            std::abs(value.number - 0.35) < 1e-12, "numeric hook replacement chain failed");
    }
    value.type = FDKSS_INTEGER; value.integer = 42;
    require(fdkss_filter_values(f.runtime, "filter.integer", &value, 1, 0, 0, 0) == FDKSS_OK && value.integer == 44,
        "integer hook replacement failed");
    value.type = FDKSS_BOOLEAN; value.boolean = 1;
    require(fdkss_filter_values(f.runtime, "filter.boolean", &value, 1, 0, 0, 0) == FDKSS_OK && !value.boolean,
        "boolean hook replacement failed");
    require(f.matches("Broken:hook returned invalid replacement; disabled") == 3, "invalid replacement callbacks were not isolated");
    fdkss_value many[64]{};
    for (int i = 0; i < 64; ++i) { many[i].type = FDKSS_INTEGER; many[i].integer = i + 1; }
    require(fdkss_filter_values(f.runtime, "filter.many", many, 64, 63, 0, 0) == FDKSS_OK && many[63].integer == 65,
        "large hook argument stack failed");
    require(fdkss_filter_values(f.runtime, "filter.many", many, 64, 64, 0, 0) == FDKSS_INVALID_ARGUMENT, "invalid writable index allowed");
    value.type = FDKSS_NUMBER; value.number = -1;
    require(fdkss_filter_values(f.runtime, "filter.number", &value, 1, 0, 0, 1) == FDKSS_INVALID_ARGUMENT, "invalid initial bounds allowed");
    fdkss_tick(f.runtime, 0.1);
    require(f.matches("Zeta:alive") == 1, "hook replacement errors killed updates");
}
#if defined(FDKSS_NATIVE_V1)
void test_native_plugins() {
    Fixture f;
    auto native = [&](const char *name, const fs::path &library) {
        const auto directory = f.root / name / "Native";
        fs::create_directories(directory);
        std::ofstream(f.root / name / "enabled.txt") << '1';
        fs::copy_file(library, directory / "main.dll", fs::copy_options::overwrite_existing);
    };
    f.script("AlphaNative", "fdk.on('update', function() fdk.log('lua-update') end)");
    f.script("Isolated", "assert(not pcall(fdk.invoke, 'native.elapsed'))");
    f.script("UpdateFailure", "fdk.on('update', function() fdk.log('lua-update') end)");
    for (const auto *name : {"AlphaNative", "NativeOnly", "StartFailure", "UpdateFailure", "EventFailure", "FilterNative", "FilterFailure"}) native(name, FDKSS_NATIVE_V1);
    native("LegacyNative", FDKSS_NATIVE_LEGACY);
    native("BadABI", FDKSS_NATIVE_BAD);
    f.create();
    fdkss_register_native(f.runtime, "test.echo", Fixture::native, &f);
    fdkss_register_typed_native(f.runtime, "typed.echo", typed_echo, &f);
    require(fdkss_start(f.runtime) == FDKSS_OK, "native load broke runtime startup");
    size_t count = 0; fdkss_mod_count(f.runtime, &count);
    require(count == 8, "native-only/legacy/failed native mod count wrong");
    require(f.matches("StartFailure:native-stop") == 1, "partial native start was not cleaned up");
    char result[128]{};
    auto execute = [&](const char *source) { return fdkss_execute(f.runtime, "AlphaNative", source, result, sizeof(result)); };
    require(execute("return fdk.call('native.version'), fdk.invoke('native.elapsed')") == FDKSS_OK &&
        std::string(result) == "build-1\t0.0", "native Lua bindings failed");
    fdkss_tick(f.runtime, 0.25); fdkss_tick(f.runtime, 0.25);
    require(f.matches("UpdateFailure:native-update") == 1 && f.matches("UpdateFailure:lua-update") == 2,
        "native update errors were not isolated");
    fdkss_value value{}; value.type = FDKSS_INTEGER; value.integer = 42;
    fdkss_emit_values(f.runtime, "custom.native", &value, 1); fdkss_emit_values(f.runtime, "custom.native", &value, 1);
    require(f.matches("AlphaNative:native-event") == 2 && f.matches("EventFailure:native-event") == 1,
        "native events/errors failed");
    value.integer = 42;
    require(fdkss_filter_values(f.runtime, "custom.filter", &value, 1, 0, 0, 0) == FDKSS_OK && value.integer == 52,
        "native hook replacement failed");
    value.integer = 42; fdkss_filter_values(f.runtime, "custom.filter", &value, 1, 0, 0, 0);
    require(value.integer == 52 && f.matches("FilterFailure:native filter returned invalid replacement; disabled") == 1,
        "invalid native filter was not isolated");
    native("AlphaNative", FDKSS_NATIVE_V2); // Original DLL must remain editable while a shadow is loaded.
    f.script("AlphaNative", "error('failed reload candidate')");
    require(fdkss_reload(f.runtime, "AlphaNative") == FDKSS_ERROR, "failed native candidate accepted");
    require(execute("return fdk.call('native.version'), fdk.invoke('native.elapsed')") == FDKSS_OK &&
        std::string(result) == "build-1\t0.5", "failed reload replaced old native bindings/state");
    f.script("AlphaNative", "assert(fdk.call('native.version') == 'build-2')");
    require(fdkss_reload(f.runtime, "AlphaNative") == FDKSS_OK, "native DLL hot reload failed");
    require(execute("return fdk.call('native.version'), fdk.invoke('native.elapsed')") == FDKSS_OK &&
        std::string(result) == "build-2\t0.0", "new native code did not replace old code");
    require(fdkss_unload(f.runtime, "NativeOnly") == FDKSS_OK && f.matches("NativeOnly:native-stop") == 1,
        "native-only unload did not stop the plugin");
    require(fdkss_destroy(f.runtime) == FDKSS_OK, "native runtime shutdown failed"); f.runtime = nullptr;
    require(f.matches("AlphaNative:native-stop") == 3 && f.matches("NativeOnly:native-stop") == 1,
        "native shutdown/reload cleanup failed");
    const auto cache = f.root / ".fdkss-native-cache";
    require(fs::exists(cache) && fs::is_empty(cache), "native shadow DLLs were not cleaned up");
}
#endif
void test_content_only_mod() {
    Fixture f;
    fs::create_directories(f.root / "AssetsOnly" / "Assets");
    std::ofstream(f.root / "AssetsOnly" / "enabled.txt") << '1';
    f.create();
    require(fdkss_start(f.runtime) == FDKSS_OK, "content-only runtime start failed");
    size_t count = 0;
    fdkss_mod_count(f.runtime, &count);
    require(count == 1, "content-only mod was not accepted");
    char result[64]{};
    require(fdkss_execute(f.runtime, "AssetsOnly", "return fdk.version", result, sizeof(result)) == FDKSS_OK,
        "content-only mod has no console state");
    require(fdkss_reload(f.runtime, "AssetsOnly") == FDKSS_OK, "content-only reload failed");
}
int FDKSS_CALL script_fixture(void *user, const fdkss_value *args, size_t count,
    fdkss_result_fn emit, void *sink, char *, size_t) {
    const auto kind = reinterpret_cast<std::uintptr_t>(user);
    auto number = [&](std::int64_t n) { fdkss_value v{}; v.type = FDKSS_INTEGER; v.integer = n; return emit(sink, &v); };
    if (kind == 8) {
        require(count == 5 && args[2].type == FDKSS_INTEGER && args[2].integer == -7 &&
            args[3].type == FDKSS_NIL && args[4].type == FDKSS_NUMBER && args[4].number == 1.5,
            "pure call lost scalar argument order or nil");
        return number(-7);
    }
    if (kind >= 2 && kind != 5) {
        require(count == (kind == 2 ? 1 : kind == 7 ? 3 : 2), "script info arguments wrong");
        if (std::string_view(args[0].string, args[0].string_size) == "missing") { fdkss_value v{}; return emit(sink, &v); }
        if (kind == 6) {
            if (number(0) || number(4294967295LL) || number(0)) return 1;
            fdkss_value value{}; return emit(sink, &value);
        }
        if (kind == 7) return number(args[2].integer);
        if (kind == 2) { for (auto n : {2048, 35, 1, 0}) if (number(n)) return 1; }
        else {
            if (kind == 4) { fdkss_value v{}; v.type = FDKSS_STRING; v.string="function4"; v.string_size=9; if (emit(sink, &v)) return 1; }
            for (auto n : {4, 2, 1, 0, 0, 123, 64}) if (number(n)) return 1;
        }
        return 0;
    }
    require(count == 3, "script enumeration arguments wrong");
    const auto offset = args[kind == 0 ? 0 : 1].integer;
    const auto limit = args[kind == 0 ? 1 : 2].integer;
    const auto end = std::min<std::int64_t>(35, offset + limit);
    if (number(end < 35 ? end : -1)) return 1;
    for (auto i = offset; i < end; ++i) {
        const auto name = std::string(kind == 0 ? "script" : kind == 5 ? "variable" : "function") + std::to_string(i);
        fdkss_value v{}; v.type = FDKSS_STRING; v.string = name.data(); v.string_size = name.size();
        if (emit(sink, &v)) return 1;
    }
    return 0;
}
void test_script_references() {
    Fixture f;
    f.script("Scripts", R"(
        local programs = fdk.find_scripts()
        assert(#programs == 35 and programs[35]:info().function_count == 35)
        assert(programs[1]:info().name == 'script0')
        local functions = programs[1]:functions()
        assert(#functions == 35 and functions[35] == 'function34')
        local fn = programs[1]:function_info('function4')
        assert(fn.index == 4 and fn.input_count == 2 and fn.default_count == 1 and fn.code_offset == 64)
        assert(programs[1]:function_at(4).name == 'function4')
        local variables = programs[1]:variables()
        assert(#variables == 35 and variables[35] == 'variable34')
        local variable = programs[1]:variable_info('variable0')
        assert(variable.name == 'variable0' and variable.index == 0 and variable.raw_metadata == 4294967295)
        assert(variable.raw_value == 0 and variable.value == nil)
        assert(programs[1]:set_variable('variable0', 42) == 42)
        assert(programs[1]:call_pure('identity', -7, nil, 1.5) == -7)
        assert(programs[1]:preview_pure('identity', -7, nil, 1.5) == -7)
        assert(fdk.script('missing'):variable_info('none') == nil)
        assert(fdk.script('missing'):info() == nil and fdk.script('missing'):function_info('none') == nil)
        assert(not pcall(function() programs[1].name = 'other' end))
        fdk.log('script-references-passed')
    )");
    f.create();
    const char *names[]{"engine.script_names", "engine.script_functions", "engine.script_info", "engine.script_function_info", "engine.script_function_at", "engine.script_variables", "engine.script_variable_info", "engine.script_variable_set", "engine.script_pure"};
    for (std::uintptr_t i = 0; i < 9; ++i)
        fdkss_register_typed_native(f.runtime, names[i], script_fixture, reinterpret_cast<void *>(i));
    fdkss_register_typed_native(f.runtime, "engine.script_pure_preview", script_fixture, reinterpret_cast<void *>(8));
    require(fdkss_start(f.runtime) == FDKSS_OK && f.matches("Scripts:script-references-passed") == 1,
        "script helper pagination/info failed");
}
void test_mod_unload() {
    Fixture f;
    f.script("Alpha", "fdk.on('update', function() fdk.log('update') end); fdk.on('stop', function() fdk.log('stop') end)");
    f.script("Beta", "value=11; fdk.on('update', function() fdk.log('update') end)");
    f.create(); require(fdkss_start(f.runtime) == FDKSS_OK, "unload start failed");
    char name[64]{};
    require(fdkss_mod_name(f.runtime, 0, name, sizeof(name)) == FDKSS_OK && std::string(name) == "Alpha", "mod name lookup failed");
    require(fdkss_mod_name(f.runtime, 0, name, 2) == FDKSS_ERROR && !*name, "short mod-name buffer accepted");
    require(fdkss_unload(f.runtime, "../Alpha") == FDKSS_INVALID_ARGUMENT, "unsafe mod name accepted");
    require(fdkss_unload(f.runtime, "Alpha") == FDKSS_OK && f.matches("Alpha:stop") == 1, "unload did not run stop");
    require(fs::exists(f.root / "Alpha" / "enabled.txt"), "unload edited the marker");
    require(fdkss_unload(f.runtime, "Alpha") == FDKSS_ERROR, "missing mod unload accepted");
    fdkss_tick(f.runtime, 0.1);
    require(f.matches("Alpha:update") == 0 && f.matches("Beta:update") == 1, "unload affected another mod's callbacks");
    require(fdkss_execute(f.runtime, "Beta", "return value", name, sizeof(name)) == FDKSS_OK && std::string(name) == "11", "unload changed another Lua state");
    require(fdkss_reload(f.runtime, "Alpha") == FDKSS_OK, "unloaded mod could not be loaded again");
    fdkss_tick(f.runtime, 0.1); require(f.matches("Alpha:update") == 1, "re-enabled callbacks missing");
}
void test_timers() {
    Fixture f;
    f.script("Timers", R"(
        events = {}
        local function record(value) events[#events + 1] = value end
        assert(not pcall(fdk.delay, 0/0, function() end))
        assert(not pcall(fdk.delay, math.huge, function() end))
        assert(not pcall(fdk.every, 0, function() end))
        local cancelled = fdk.delay(0, function() error('cancelled timer ran') end)
        assert(fdk.cancel_timer(cancelled) and not fdk.cancel_timer(cancelled))
        fdk.delay(0, function(a, b, c)
            assert(a == 'first' and b == nil and c == 7)
            record(a)
            fdk.delay(0, record, 'nested')
        end, 'first', nil, 7)
        fdk.delay(0.5, record, 'delayed')
        repeating = fdk.every(0.25, record, 'repeat')
        fdk.every(0.25, function() error('timer-probe-failure') end)
        local later
        fdk.delay(0.25, function() assert(fdk.cancel_timer(later)); record('cancel-later') end)
        later = fdk.delay(0.25, function() error('later timer ran') end)
    )");
    f.create(); require(fdkss_start(f.runtime) == FDKSS_OK, "timer runtime start failed");
    auto check = [&](const char *expected) {
        char result[1024]{};
        require(fdkss_execute(f.runtime, "Timers", "return table.concat(events, ',')", result, sizeof(result)) == FDKSS_OK,
            "timer state query failed");
        require(std::string(result) == expected, "timer order/count wrong");
    };
    check("");
    fdkss_tick(f.runtime, 0); check("first");
    fdkss_tick(f.runtime, 0.25); check("first,repeat,cancel-later,nested");
    fdkss_tick(f.runtime, 1); check("first,repeat,cancel-later,nested,delayed,repeat");
    require(std::count_if(f.logs.begin(), f.logs.end(), [](const auto &log) {
        return log.starts_with("Timers:timer callback failed:");
    }) == 1, "failing timer was not disabled independently");
    char result[128]{};
    require(fdkss_execute(f.runtime, "Timers", "return fdk.cancel_timer(repeating)", result, sizeof(result)) == FDKSS_OK &&
        std::string(result) == "true", "repeating timer cancellation failed");
    fdkss_tick(f.runtime, 10); check("first,repeat,cancel-later,nested,delayed,repeat");
    f.script("Timers", "events={}; fdk.delay(0, function() events[1]='new-state' end)");
    require(fdkss_reload(f.runtime, "Timers") == FDKSS_OK, "timer mod reload failed");
    fdkss_tick(f.runtime, 0); check("new-state");
}
void test_key_bindings() {
    Fixture f;
    f.script("Keys", R"(
        presses=0
        token=fdk.bind_key(65, function(key, modifiers) assert(key==65 and modifiers==2); presses=presses+1 end, 2)
        assert(not pcall(fdk.bind_key, 0x78, function() end))
        assert(not pcall(fdk.bind_key, 0x79, function() end))
        assert(not pcall(fdk.bind_key, 0x7a, function() end))
        assert(not pcall(fdk.bind_key, 65, function() end, 8))
        assert(not pcall(fdk.bind_key, 65.5, function() end))
        fdk.bind_key(65, function() error('key-probe-failure') end, 2)
    )");
    f.create(); require(fdkss_start(f.runtime) == FDKSS_OK, "key runtime start failed");
    auto emit = [&](int key, bool down, int modifiers) {
        fdkss_value values[3]{};
        values[0].type=FDKSS_INTEGER; values[0].integer=key;
        values[1].type=FDKSS_BOOLEAN; values[1].boolean=down;
        values[2].type=FDKSS_INTEGER; values[2].integer=modifiers;
        fdkss_emit_values(f.runtime, "input.key", values, 3);
    };
    emit(65, true, 0); emit(65, false, 2); emit(66, true, 2);
    emit(65, true, 2); emit(65, true, 2);
    char result[128]{};
    require(fdkss_execute(f.runtime, "Keys", "return presses, fdk.unbind_key(token)", result, sizeof(result)) == FDKSS_OK &&
        std::string(result) == "2\ttrue", "key filtering or cancellation failed");
    emit(65, true, 2);
    require(fdkss_execute(f.runtime, "Keys", "return presses", result, sizeof(result)) == FDKSS_OK &&
        std::string(result) == "2", "unbound key callback ran");
    require(std::count_if(f.logs.begin(), f.logs.end(), [](const auto &log) {
        return log.find("key-probe-failure") != std::string::npos;
    }) == 1, "failing key binding was not isolated");
}
void test_named_object_search() {
    struct Search { int pages = 0, reads = 0; std::string filter; } search;
    Fixture f; f.script("Search", ""); f.create();
    auto list = +[](void *user, const fdkss_value *args, size_t count, fdkss_result_fn emit, void *sink, char *, size_t) -> int {
        auto &state = *static_cast<Search *>(user);
        require(count == 3 && args[0].type == FDKSS_INTEGER && args[2].type == FDKSS_STRING, "search page arguments wrong");
        ++state.pages; state.filter.assign(args[2].string, args[2].string_size);
        const bool first = args[0].integer == 0;
        fdkss_value result{}; result.type = FDKSS_INTEGER; result.integer = first ? 2 : -1;
        if (emit(sink, &result)) return 1;
        for (const char *token : first ? std::vector<const char *>{"@a", "@b"} : std::vector<const char *>{"@c"}) {
            result.type = FDKSS_STRING; result.string = token; result.string_size = 2;
            if (emit(sink, &result)) return 1;
        }
        return 0;
    };
    auto get = +[](void *user, const fdkss_value *args, size_t count, fdkss_result_fn emit, void *sink, char *, size_t) -> int {
        auto &state = *static_cast<Search *>(user); ++state.reads;
        require(count == 2 && std::string_view(args[1].string, args[1].string_size) == "name", "search read wrong property");
        const std::string_view root(args[0].string, args[0].string_size);
        const std::string_view name = root == "@a" ? "door[0]" : root == "@b" ? "door.1" : "shop door";
        fdkss_value result{}; result.type = FDKSS_STRING; result.string = name.data(); result.string_size = name.size();
        return emit(sink, &result);
    };
    auto metadata=+[](void *,const fdkss_value *args,size_t count,fdkss_result_fn emit,void *sink,char *,size_t)->int {
        require(count==2 && args[0].type==FDKSS_STRING && args[1].type==FDKSS_INTEGER,"search metadata arguments");
        const bool other=std::string_view(args[0].string,args[0].string_size)=="@c";
        const char *name=other ? ".?AVOther@@" : ".?AVDerived@@";
        for(const char *text : {name,args[1].integer==0 ? name : ".?AVBase@@"}) {
            fdkss_value v{};v.type=FDKSS_STRING;v.string=text;v.string_size=std::strlen(text);if(emit(sink,&v))return 1;
        }
        for(const int64_t value : {int64_t(2),int64_t(0),int64_t(0),int64_t(0),int64_t(args[1].integer?0:1),int64_t(0),int64_t(-1),int64_t(0),int64_t(0)}) {
            fdkss_value v{};v.type=FDKSS_INTEGER;v.integer=value;if(emit(sink,&v))return 1;
        }
        return 0;
    };
    require(fdkss_register_typed_native(f.runtime, "engine.list", list, &search) == FDKSS_OK &&
        fdkss_register_typed_native(f.runtime, "engine.get", get, &search) == FDKSS_OK &&
        fdkss_register_typed_native(f.runtime, "engine.class_info", metadata, nullptr) == FDKSS_OK && fdkss_start(f.runtime) == FDKSS_OK, "search fixture failed");
    char result[256]{};
    require(fdkss_execute(f.runtime, "Search", "return #fdk.find_objects()", result, sizeof(result)) == FDKSS_OK &&
        std::string(result) == "3" && search.pages == 2 && search.reads == 0, "unfiltered search added property reads");
    search.pages = search.reads = 0;
    require(fdkss_execute(f.runtime, "Search", "return fdk.find_object('ObjectActor', 'door.').name", result, sizeof(result)) == FDKSS_OK &&
        std::string(result) == "door.1" && search.pages == 1 && search.filter == "ObjectActor", "first search failed to stop early or pass class filter");
    require(fdkss_execute(f.runtime, "Search", "local a=fdk.find_objects(nil,'[0]');return #a,a[1].name", result, sizeof(result)) == FDKSS_OK &&
        std::string(result) == "1\tdoor[0]", "name substring interpreted as a Lua pattern");
    require(fdkss_execute(f.runtime, "Search", "return fdk.find_object(nil,'missing')", result, sizeof(result)) == FDKSS_OK &&
        std::string(result) == "nil", "missing first search did not return nil");
    require(fdkss_execute(f.runtime, "Search", "return pcall(fdk.find_objects,nil,42)", result, sizeof(result)) == FDKSS_OK &&
        std::string(result).starts_with("false\t"), "invalid name filter accepted");
    search.pages=search.reads=0;
    require(fdkss_execute(f.runtime,"Search","return #fdk.find_objects('.?AVBase@@',nil,true)",result,sizeof(result))==FDKSS_OK &&
        std::string(result)=="3" && search.filter.empty() && search.reads==0,"inherited search native prefilter");
    require(fdkss_execute(f.runtime,"Search","return #fdk.find_objects('.?AVDerived@@',nil,true)",result,sizeof(result))==FDKSS_OK &&
        std::string(result)=="2","concrete inherited search");
    search.pages=search.reads=0;
    require(fdkss_execute(f.runtime,"Search","return fdk.find_object('.?AVBase@@','door.',true).name",result,sizeof(result))==FDKSS_OK &&
        std::string(result)=="door.1" && search.pages==1,"inherited first-match stop/name filter");
    require(fdkss_execute(f.runtime,"Search","return fdk.find_object('Base',nil,true)",result,sizeof(result))==FDKSS_OK && std::string(result)=="nil","inherited exact match");
    for(const char *lua : {"return pcall(fdk.find_objects,nil,nil,true)","return pcall(fdk.find_objects,'',nil,true)","return pcall(fdk.find_objects,'Actor',nil,1)"})
        require(fdkss_execute(f.runtime,"Search",lua,result,sizeof(result))==FDKSS_OK && std::string(result).starts_with("false\t"),"inherited invalid arguments");
}
void test_actor_notifications() {
    struct Actors { std::vector<std::string> tokens; unsigned pages=0, reads=0; bool unavailable=false, malformed=false; } actors;
    for (unsigned i=0;i<40;++i) actors.tokens.push_back("@1:1:"+std::to_string(i+1)+":abc:123");
    Fixture f; f.script("Discovery", R"(
        received, filtered, failures, cancelled = 0, 0, 0, 0
        all=fdk.notify_on_actor_discovered(nil,function(actor) assert(actor.class);received=received+1 end)
        matching=fdk.notify_on_actor_discovered('Actor[',function() filtered=filtered+1 end)
        fdk.notify_on_actor_discovered(nil,function() failures=failures+1;error('discovery-failure') end)
        local later
        fdk.notify_on_actor_discovered(nil,function() fdk.cancel_actor_notification(later) end)
        later=fdk.notify_on_actor_discovered(nil,function() cancelled=cancelled+1 end)
        assert(not pcall(fdk.notify_on_actor_discovered, false, function() end))
        assert(not pcall(fdk.notify_on_actor_discovered, nil, 42))
    )"); f.create();
    auto list=+[](void *user,const fdkss_value *args,size_t count,fdkss_result_fn emit,void *sink,char *error,size_t size)->int {
        auto &state=*static_cast<Actors *>(user);++state.pages;
        require(count==3 && args[1].integer==32 && args[2].string_size==0,"notification page was not shared/bounded");
        if(state.unavailable) {std::snprintf(error,size,"scene unavailable");return 1;}
        const auto start=static_cast<std::size_t>(args[0].integer);
        const auto end=(std::min)(start+32,state.tokens.size());
        fdkss_value value{};value.type=FDKSS_INTEGER;
        value.integer=state.malformed ? static_cast<std::int64_t>(start) : end<state.tokens.size() ? static_cast<std::int64_t>(end) : -1;
        if(emit(sink,&value))return 1;
        for(auto i=start;i<end;++i) {value.type=FDKSS_STRING;value.string=state.tokens[i].data();value.string_size=state.tokens[i].size();if(emit(sink,&value))return 1;}
        return 0;
    };
    auto get=+[](void *user,const fdkss_value *args,size_t count,fdkss_result_fn emit,void *sink,char *,size_t)->int {
        auto &state=*static_cast<Actors *>(user);++state.reads;
        require(count==2 && std::string_view(args[1].string,args[1].string_size)=="class","notification inspected an unexpected property");
        const std::string_view token(args[0].string,args[0].string_size);
        fdkss_value value{};
        if(std::find(state.tokens.begin(),state.tokens.end(),token)!=state.tokens.end()) {
            value.type=FDKSS_STRING;value.string="Actor[fixture]";value.string_size=14;
        }
        return emit(sink,&value);
    };
    require(fdkss_register_typed_native(f.runtime,"engine.list",list,&actors)==FDKSS_OK &&
        fdkss_register_typed_native(f.runtime,"engine.get",get,&actors)==FDKSS_OK && fdkss_start(f.runtime)==FDKSS_OK,"notification fixture start failed");
    auto tick=[&] {require(fdkss_tick(f.runtime,0.01)==FDKSS_OK,"notification tick failed");};
    char result[256]{};
    auto check=[&](const char *expected) {
        require(fdkss_execute(f.runtime,"Discovery","return received,filtered,failures,cancelled",result,sizeof(result))==FDKSS_OK && std::string(result)==expected,"notification counts incorrect");
    };
    tick();check("32\t32\t1\t0");require(actors.pages==1,"more than one discovery page per update");
    tick();check("40\t40\t1\t0");
    for(unsigned i=0;i<4;++i)tick();check("40\t40\t1\t0");
    require(std::count_if(f.logs.begin(),f.logs.end(),[](const auto &log){return log.starts_with("Discovery:actor notification callback failed:");})==1,"discovery callback error was not isolated");
    actors.tokens.push_back("@1:1:41:abc:123");tick();tick();check("41\t41\t1\t0");
    actors.tokens.erase(actors.tokens.begin());tick();tick();
    actors.tokens.insert(actors.tokens.begin(),"@1:1:99:abc:123");tick();tick();check("42\t42\t1\t0");
    // A scene change midway through a scan restarts from page zero before notifying.
    tick();
    actors.tokens={"@2:1:1:abc:123","@2:1:2:abc:123"};tick();tick();check("44\t44\t1\t0");
    actors.unavailable=true;tick();check("44\t44\t1\t0");actors.unavailable=false;
    actors.malformed=true;tick();check("44\t44\t1\t0");actors.malformed=false;tick();check("44\t44\t1\t0");
    require(fdkss_execute(f.runtime,"Discovery","return fdk.cancel_actor_notification(all),fdk.cancel_actor_notification(all)",result,sizeof(result))==FDKSS_OK && std::string(result)=="true\tfalse","notification cancellation failed");
    actors.tokens.push_back("@2:1:3:abc:123");tick();check("44\t45\t1\t0");
    f.script("Discovery","received=0;token=fdk.notify_on_actor_discovered(nil,function() received=received+1 end)");
    require(fdkss_reload(f.runtime,"Discovery")==FDKSS_OK,"notification reload failed");tick();
    require(fdkss_execute(f.runtime,"Discovery","return received, fdk.cancel_actor_notification(token)",result,sizeof(result))==FDKSS_OK && std::string(result)=="3\ttrue","notification state survived reload");
    const auto pages=actors.pages;tick();require(actors.pages==pages,"cancelled final subscription continued polling");
    require(fdkss_execute(f.runtime,"Discovery",R"(local ids={};for i=1,64 do ids[i]=fdk.notify_on_actor_discovered(nil,function() end) end;assert(not pcall(fdk.notify_on_actor_discovered,nil,function() end));for _,id in ipairs(ids) do assert(fdk.cancel_actor_notification(id)) end)",result,sizeof(result))==FDKSS_OK,"notification subscription bounds failed");
}
void test_command_group_discovery() {
    Fixture f;f.script("Groups","");f.create();
    struct State {bool missing=false;unsigned reads=0;} state;
    auto get=+[](void *user,const fdkss_value *args,size_t count,fdkss_result_fn emit,void *sink,char *error,size_t capacity)->int {
        auto &s=*static_cast<State *>(user);++s.reads;
        require(count==2 && args[0].type==FDKSS_STRING && args[1].type==FDKSS_STRING,"group arguments");
        const std::string root(args[0].string,args[0].string_size);require(root.starts_with("script_group:"),"group root");
        const auto group=std::stoi(root.substr(13));require(group>=0 && group<28,"group range");
        if(s.missing && group==7){std::snprintf(error,capacity,"group unavailable");return 1;}
        fdkss_value v{};
        if(std::string_view(args[1].string,args[1].string_size)=="class") {
            v.type=FDKSS_STRING;v.string=group==0 ? ".?AVCommandSys@@" : ".?AVCommandOther@@";v.string_size=std::strlen(v.string);
        }else{v.type=FDKSS_INTEGER;v.integer=group;}
        return emit(sink,&v);
    };
    require(fdkss_register_typed_native(f.runtime,"engine.get",get,&state)==FDKSS_OK && fdkss_start(f.runtime)==FDKSS_OK,"group setup");
    char result[256]{};
    auto run=[&](const char *lua){require(fdkss_execute(f.runtime,"Groups",lua,result,sizeof(result))==FDKSS_OK,"group Lua");return std::string(result);};
    require(run("local g=fdk.find_command_groups();return #g,g[1].group,g[28].group")=="28\t0\t27" && state.reads==30,"group ordered discovery/read validation");
    require(run("local g=fdk.find_command_groups('CommandSys');return #g,g[1].group")=="1\t0","group class filter");
    require(run("return #fdk.find_command_groups('[Command')")=="0","group literal filter");
    for(const char *lua : {"return pcall(fdk.command_group,-1)","return pcall(fdk.command_group,28)","return pcall(fdk.command_group,0.5)","return pcall(fdk.command_group,'0')","return pcall(fdk.find_command_groups,1)"})
        require(run(lua).starts_with("false\t"),"group invalid arguments");
    state.missing=true;require(run("return pcall(fdk.find_command_groups)").starts_with("false\t"),"missing group partial discovery");
}
void test_class_metadata() {
    Fixture f;f.script("Classes","");f.create();
    struct State {bool changed=false,invalid=false,expired=false;int calls=0;} state;
    auto inspect=+[](void *user,const fdkss_value *args,size_t count,fdkss_result_fn emit,void *sink,char *error,size_t capacity)->int {
        auto &s=*static_cast<State *>(user);++s.calls;
        require(count==2 && args[0].type==FDKSS_STRING && args[1].type==FDKSS_INTEGER,"class arguments");
        if(s.expired){std::snprintf(error,capacity,"expired");return 1;}
        const auto index=args[1].integer;require(index>=0 && index<2,"class index");
        auto text=[&](const char *str){fdkss_value v{};v.type=FDKSS_STRING;v.string=str;v.string_size=std::strlen(str);return emit(sink,&v);};
        if(text(s.changed && index==1 ? ".?AVChanged@@" : ".?AVDerived@@") || text(index ? ".?AVBase@@" : ".?AVDerived@@"))return 1;
        for(const int64_t i : {s.invalid?int64_t(1025):int64_t(2),int64_t(0),int64_t(0),int64_t(0),int64_t(index?0:1),int64_t(0),int64_t(-1),int64_t(0),int64_t(0)}) {
            fdkss_value v{};v.type=FDKSS_INTEGER;v.integer=i;if(emit(sink,&v))return 1;
        }
        return 0;
    };
    require(fdkss_register_typed_native(f.runtime,"engine.class_info",inspect,&state)==FDKSS_OK && fdkss_start(f.runtime)==FDKSS_OK,"class setup");
    char result[256]{};
    auto run=[&](const char *lua){require(fdkss_execute(f.runtime,"Classes",lua,result,sizeof(result))==FDKSS_OK,"class Lua");return std::string(result);};
    require(run("local o=fdk.object('player');local m=o:class_info();return m.name,#m.bases,m.bases[2].vbtable_offset,o:is_a('.?AVBase@@'),o:is_a('.?AVDerived@@'),o:is_a('Base')")==".?AVDerived@@\t2\t-1\ttrue\ttrue\tfalse","class copy and exact ancestry");
    require(run("return pcall(function() return fdk.engine.player:is_a(1) end)").starts_with("false\t"),"class invalid name");
    state.changed=true;require(run("return pcall(function() return fdk.engine.player:class_info() end)").starts_with("false\t"),"class changed snapshot");state.changed=false;
    state.invalid=true;require(run("return pcall(function() return fdk.engine.player:class_info() end)").starts_with("false\t"),"class count bound");state.invalid=false;
    state.expired=true;require(run("return pcall(function() return fdk.object('@expired'):is_a('.?AVBase@@') end)").starts_with("false\t"),"expired class handle");
}
void test_player_condition_copies() {
    Fixture f;f.script("Conditions","");f.create();
    struct State {bool absent=false,malformed=false;std::array<char,144> bytes{};} state;
    const std::uint32_t first=27,second=22;std::memcpy(state.bytes.data(),&first,4);std::memcpy(state.bytes.data()+72,&second,4);
    auto snapshot=+[](void *user,const fdkss_value *args,size_t count,fdkss_result_fn emit,void *sink,char *,size_t)->int {
        require(count==0 || (count==1 && args[0].type==FDKSS_STRING && std::string_view(args[0].string,args[0].string_size)=="@fixture"),"condition helper reference routing");auto &s=*static_cast<State *>(user);fdkss_value v{};
        if(s.absent)return emit(sink,&v);
        v.type=FDKSS_INTEGER;v.integer=2;if(emit(sink,&v))return 1;
        v.type=FDKSS_STRING;v.string=s.bytes.data();v.string_size=s.malformed ? 71 : s.bytes.size();return emit(sink,&v);
    };
    auto resources=+[](void *,const fdkss_value *args,size_t count,fdkss_result_fn emit,void *sink,char *,size_t)->int {
        require(count==1 && args[0].type==FDKSS_STRING && std::string_view(args[0].string,args[0].string_size)=="@fixture","resource helper reference routing");
        fdkss_value v{};v.type=FDKSS_INTEGER;
        for(auto n:{7,10,4,20,99,200}){v.integer=n;if(emit(sink,&v))return 1;}return 0;
    };
    auto status_value=+[](void *,const fdkss_value *args,size_t count,fdkss_result_fn emit,void *sink,char *,size_t)->int {
        require(count==2 && args[0].type==FDKSS_STRING && std::string_view(args[0].string,args[0].string_size)=="@fixture" && args[1].type==FDKSS_INTEGER && args[1].integer==80,"status helper reference/selector routing");
        fdkss_value v{};v.type=FDKSS_INTEGER;v.integer=4294967295ll;return emit(sink,&v);
    };
    require(fdkss_register_typed_native(f.runtime,"engine.player_conditions",snapshot,&state)==FDKSS_OK &&
        fdkss_register_typed_native(f.runtime,"engine.character_conditions",snapshot,&state)==FDKSS_OK &&
        fdkss_register_typed_native(f.runtime,"engine.character_resources",resources,nullptr)==FDKSS_OK &&
        fdkss_register_typed_native(f.runtime,"engine.character_status_value",status_value,nullptr)==FDKSS_OK &&
        fdkss_start(f.runtime)==FDKSS_OK,"condition copy setup");
    char result[256]{};
    auto run=[&](const char *lua){require(fdkss_execute(f.runtime,"Conditions",lua,result,sizeof(result))==FDKSS_OK,"condition copy Lua");return std::string(result);};
    require(run("local r=fdk.player_conditions();cached=r;return #r,r[1].raw_key,r[2].raw_key,#r[1].raw_bytes")=="2\t27\t22\t72","populated condition copy");
    require(run("local c=fdk.object('@fixture');local r=c:conditions();local s=c:resources();return #r,r[1].raw_key,s.hp,s.cp")=="2\t27\t7\t99","reference snapshot helpers");
    require(run("return fdk.object('@fixture'):status_value(80)")=="4294967295","status helper unsigned copied result");
    state.bytes[0]=1;
    require(run("local r=fdk.player_conditions();return cached[1].raw_key,r[1].raw_key,cached[1].raw_bytes==r[1].raw_bytes")=="27\t1\tfalse","condition bytes retained across source change");
    state.absent=true;require(run("return fdk.object('@fixture'):conditions()")=="nil","reference missing manager");state.absent=false;
    state.absent=true;require(run("return fdk.player_conditions()")=="nil","condition absent");state.absent=false;
    state.malformed=true;require(run("return pcall(fdk.player_conditions)").starts_with("false\t"),"condition malformed snapshot accepted");
}
void test_battle_copies() {
    Fixture f;f.script("Battle","");f.create();
    struct State {bool absent=false,malformed=false;std::array<std::uint32_t,8> row{60000,45,1813,12186,1000,1000,199,200};};State state;
    const auto snapshot=+[](void *opaque,const fdkss_value *,size_t count,fdkss_result_fn emit,void *sink,char *,size_t)->int {
        require(count==0,"battle arguments reached native");const auto &state=*static_cast<State *>(opaque);
        fdkss_value value{};if(state.absent)return emit(sink,&value);
        value.type=FDKSS_INTEGER;value.integer=1;if(emit(sink,&value))return 1;
        value={};value.type=FDKSS_STRING;value.string=reinterpret_cast<const char *>(state.row.data());
        value.string_size=state.malformed ? 31 : sizeof(state.row);return emit(sink,&value);
    };
    require(fdkss_register_typed_native(f.runtime,"engine.battle_snapshot",snapshot,&state)==FDKSS_OK && fdkss_start(f.runtime)==FDKSS_OK,"battle setup");
    char result[256]{};
    auto run=[&](const char *lua){require(fdkss_execute(f.runtime,"Battle",lua,result,sizeof(result))==FDKSS_OK,"battle Lua copy");return std::string(result);};
    require(run("held=fdk.battle_snapshot();return held.count,held.records[1].index,held.records[1].status_word,held.records[1].hp,held.atomic")=="1\t0\t60000\t1813\tfalse","battle populated decode");
    state.row[0]=60001;state.row[2]=900;
    require(run("local current=fdk.battle_snapshot();return held.records[1].hp,current.records[1].hp,held.records[1].status_word,current.records[1].status_word")=="1813\t900\t60000\t60001","battle replacement changed held copy");
    require(run("held.records[1].hp=7;return fdk.battle_snapshot().records[1].hp")=="900","battle local edit wrote source");
    state.absent=true;require(run("return fdk.battle_snapshot()")=="nil","battle absent");state.absent=false;
    state.malformed=true;require(run("return pcall(fdk.battle_snapshot)").starts_with("false\t"),"battle truncated packet accepted");state.malformed=false;
    state.row[2]=state.row[3]+1;require(run("return pcall(fdk.battle_snapshot)").starts_with("false\t"),"battle invalid resources accepted");
    require(run("return pcall(fdk.battle_snapshot,1)").starts_with("false\t"),"battle arguments ignored");
}
void test_character_model_copy() {
    Fixture f;f.script("Models","");f.create();
    struct State {std::string name="hero";bool absent=false,malformed=false;int version=4;};State state;
    const auto model=+[](void *opaque,const fdkss_value *args,size_t count,fdkss_result_fn emit,void *sink,char *,size_t)->int {
        require(count==1 && args[0].type==FDKSS_STRING && std::string_view(args[0].string,args[0].string_size)=="@fixture","model helper reference routing");
        const auto &state=*static_cast<State *>(opaque);fdkss_value value{};
        if(state.absent)return emit(sink,&value);
        value.type=FDKSS_STRING;value.string=state.name.data();value.string_size=state.name.size();if(emit(sink,&value))return 1;
        for(const auto number:{state.version,3,247}){value={};value.type=state.malformed ? FDKSS_NUMBER : FDKSS_INTEGER;value.integer=number;if(emit(sink,&value))return 1;}
        return 0;
    };
    require(fdkss_register_typed_native(f.runtime,"engine.actor_model",model,&state)==FDKSS_OK && fdkss_start(f.runtime)==FDKSS_OK,"model setup");
    char result[256]{};
    const auto run=[&](const char *lua){require(fdkss_execute(f.runtime,"Models",lua,result,sizeof(result))==FDKSS_OK,"model helper Lua");return std::string(result);};
    require(run("cached=fdk.object('@fixture'):model_info();return cached.name,cached.format_version,cached.mesh_count,cached.node_count")=="hero\t4\t3\t247","copied model fields");
    state.name="changed";require(run("local m=fdk.object('@fixture'):model_info();return cached.name,m.name,cached==m")=="hero\tchanged\tfalse","model copied ownership");
    require(run("return pcall(function()return fdk.object('@fixture'):model_info(1)end)").starts_with("false\t"),"model extra argument refused");
    state.absent=true;require(run("return fdk.object('@fixture'):model_info()")=="nil","model absent");state.absent=false;
    state.version=5;require(run("return fdk.object('@fixture'):model_info().format_version")=="5","model version5 copied");
    state.version=6;require(run("return pcall(function()return fdk.object('@fixture'):model_info()end)").starts_with("false\t"),"unknown model version refused");state.version=4;
    state.malformed=true;require(run("return pcall(function()return fdk.object('@fixture'):model_info()end)").starts_with("false\t"),"model malformed numbers refused");state.malformed=false;
    state.name=std::string("a\0b",3);require(run("return pcall(function()return fdk.object('@fixture'):model_info()end)").starts_with("false\t"),"model embedded nul refused");
    state.name.assign(64,'x');require(run("return pcall(function()return fdk.object('@fixture'):model_info()end)").starts_with("false\t"),"model oversized name refused");
}
void test_native_caller_identity() {
    Fixture f; f.script("Owner", ""); f.script("Other", ""); f.create();
    auto identity=+[](void *user,const fdkss_value *,size_t,fdkss_result_fn emit,void *sink,char *,size_t)->int {
        auto *r=static_cast<fdkss_runtime *>(user); const auto id=fdkss_native_caller(r);
        require(id!=0 && !fdkss_mod_alive(r,id),"caller scope/liveness reentry");
        fdkss_value value{}; value.type=FDKSS_INTEGER; value.integer=static_cast<int64_t>(id); return emit(sink,&value);
    };
    require(fdkss_register_typed_native(f.runtime,"test.identity",identity,f.runtime)==FDKSS_OK && fdkss_start(f.runtime)==FDKSS_OK,"identity setup");
    auto get=[&](const char *name) {
        char result[64]{}; require(fdkss_execute(f.runtime,name,"return fdk.invoke('test.identity')",result,sizeof(result))==FDKSS_OK,"identity invoke");
        require(!fdkss_native_caller(f.runtime),"caller scope leaked"); return std::stoull(result);
    };
    const auto old=get("Owner"), other=get("Other");
    require(old!=other && fdkss_mod_alive(f.runtime,old),"mod identities");
    f.script("Owner","invalid Lua !"); require(fdkss_reload(f.runtime,"Owner")==FDKSS_ERROR,"failed reload");
    require(get("Owner")==old && fdkss_mod_alive(f.runtime,old),"failed reload changed identity");
    f.script("Owner",""); require(fdkss_reload(f.runtime,"Owner")==FDKSS_OK,"identity reload");
    const auto fresh=get("Owner"); require(fresh!=old && !fdkss_mod_alive(f.runtime,old) && fdkss_mod_alive(f.runtime,fresh),"reload identity reuse");
    require(fdkss_unload(f.runtime,"Owner")==FDKSS_OK && !fdkss_mod_alive(f.runtime,fresh) && fdkss_mod_alive(f.runtime,other),"unload identity");
    bool foreign=false; std::thread worker([&]{foreign=!fdkss_native_caller(f.runtime) && !fdkss_mod_alive(f.runtime,other);}); worker.join();
    require(foreign,"foreign identity access");
}
int main() {
    try {
        test_lifecycle_and_reload(); test_error_isolation(); test_invalid_inputs(); test_typed_natives(); test_console(); test_typed_events_and_removal(); test_hook_replacements(); test_content_only_mod(); test_script_references(); test_mod_unload(); test_timers(); test_key_bindings(); test_named_object_search(); test_actor_notifications(); test_class_metadata(); test_command_group_discovery(); test_player_condition_copies(); test_battle_copies(); test_character_model_copy(); test_native_caller_identity();
#if defined(FDKSS_NATIVE_V1)
        test_native_plugins();
#endif
        std::cout << "Runtime tests passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr << "FAILED: " << e.what() << '\n'; return 1; }
}
