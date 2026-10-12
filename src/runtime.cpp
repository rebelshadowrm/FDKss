#include "actor_notifications.h"
#include "fdkss/api.h"
#include "plugin.h"
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
struct Mod;
struct Native { fdkss_native_fn callback; void *user; };
struct TypedNative { fdkss_typed_native_fn callback; void *user; };
struct fdkss_runtime {
    fs::path root;
    fdkss_log_fn logger{};
    void *log_user{};
    std::thread::id owner = std::this_thread::get_id();
    bool busy = false;
    bool started = false;
    std::uint64_t next_identity = 0, native_caller = 0;
    std::string error;
    std::unordered_map<std::string, Native> natives;
    std::unordered_map<std::string, TypedNative> typed_natives;
    std::vector<std::unique_ptr<Mod>> mods;
    void log(const char *mod, const char *message) noexcept {
        if (logger) { try { logger(log_user, mod, message); } catch (...) {} }
    }
};
struct Mod {
    fdkss_runtime *runtime;
    std::string name;
    std::uint64_t identity;
    lua_State *state{};
    struct Handler { int reference; std::int64_t token; };
    std::unordered_map<std::string, std::vector<Handler>> handlers;
    std::int64_t next_token = 1;
    bool dispatching = false;
    bool registering_plugin = false;
    std::unordered_map<std::string, Native> local_natives;
    std::unordered_map<std::string, TypedNative> local_typed_natives;
    std::unique_ptr<NativePlugin> plugin;
    Mod(fdkss_runtime *r, std::string n) : runtime(r), name(std::move(n)), identity(0) {
        if (r->next_identity >= static_cast<std::uint64_t>(INT64_MAX)) throw std::runtime_error("mod identities exhausted");
        identity=++r->next_identity;
    }
    ~Mod() { if (state) lua_close(state); plugin.reset(); }
};

namespace {
struct NativeCaller {
    fdkss_runtime &runtime; std::uint64_t previous;
    explicit NativeCaller(Mod &mod) : runtime(*mod.runtime), previous(runtime.native_caller) { runtime.native_caller = mod.identity; }
    ~NativeCaller() { runtime.native_caller = previous; }
};
bool plugin_owner(Mod *mod) { return mod && mod->runtime->owner == std::this_thread::get_id(); }
void FDKSS_CALL plugin_log(void *context, const char *message) {
    auto *mod = static_cast<Mod *>(context);
    if (plugin_owner(mod) && message) mod->runtime->log(mod->name.c_str(), message);
}
bool binding_available(Mod &mod, const char *name) {
    return name && *name && !mod.local_natives.contains(name) && !mod.local_typed_natives.contains(name) &&
        !mod.runtime->natives.contains(name) && !mod.runtime->typed_natives.contains(name);
}
fdkss_status FDKSS_CALL plugin_register_string(void *context, const char *name, fdkss_native_fn callback, void *user) {
    auto *mod = static_cast<Mod *>(context);
    if (!plugin_owner(mod)) return FDKSS_WRONG_THREAD;
    if (!callback || !name || !*name) return FDKSS_INVALID_ARGUMENT;
    try {
        if (!mod->registering_plugin || !binding_available(*mod, name)) return FDKSS_ERROR;
        mod->local_natives.emplace(name, Native{callback, user}); return FDKSS_OK;
    }
    catch (...) { return FDKSS_ERROR; }
}
fdkss_status FDKSS_CALL plugin_register_typed(void *context, const char *name, fdkss_typed_native_fn callback, void *user) {
    auto *mod = static_cast<Mod *>(context);
    if (!plugin_owner(mod)) return FDKSS_WRONG_THREAD;
    if (!callback || !name || !*name) return FDKSS_INVALID_ARGUMENT;
    try {
        if (!mod->registering_plugin || !binding_available(*mod, name)) return FDKSS_ERROR;
        mod->local_typed_natives.emplace(name, TypedNative{callback, user}); return FDKSS_OK;
    }
    catch (...) { return FDKSS_ERROR; }
}
fdkss_status FDKSS_CALL plugin_call_string(void *context, const char *name, const char *input, char *output, size_t capacity) {
    auto *mod = static_cast<Mod *>(context);
    if (!plugin_owner(mod)) return FDKSS_WRONG_THREAD;
    if (!name || !input || !output || !capacity || capacity > 16384) return FDKSS_INVALID_ARGUMENT;
    std::fill_n(output, capacity, '\0');
    const auto found = mod->runtime->natives.find(name);
    if (found == mod->runtime->natives.end()) return FDKSS_INVALID_ARGUMENT;
    try {
        const int status = found->second.callback(found->second.user, input, output, capacity);
        if (output[capacity - 1] != 0) { output[capacity - 1] = 0; return FDKSS_ERROR; }
        return status ? FDKSS_ERROR : FDKSS_OK;
    } catch (...) { std::snprintf(output, capacity, "native function threw"); return FDKSS_ERROR; }
}
fdkss_status FDKSS_CALL plugin_invoke(void *context, const char *name, const fdkss_value *args, size_t count,
    fdkss_result_fn emit, void *sink, char *error, size_t capacity) {
    auto *mod = static_cast<Mod *>(context);
    if (!plugin_owner(mod)) return FDKSS_WRONG_THREAD;
    if (!name || (!args && count) || count > 64 || !emit || !error || !capacity) return FDKSS_INVALID_ARGUMENT;
    error[0] = 0;
    error[capacity - 1] = 0;
    for (size_t i = 0; i < count; ++i) {
        if (args[i].type < FDKSS_NIL || args[i].type > FDKSS_STRING ||
            (args[i].type == FDKSS_NUMBER && !std::isfinite(args[i].number)) ||
            (args[i].type == FDKSS_STRING && !args[i].string && args[i].string_size)) return FDKSS_INVALID_ARGUMENT;
    }
    const auto found = mod->runtime->typed_natives.find(name);
    if (found == mod->runtime->typed_natives.end()) {
        std::snprintf(error, capacity, "typed native function unavailable"); return FDKSS_INVALID_ARGUMENT;
    }
    try {
        NativeCaller caller(*mod);
        const int status = found->second.callback(found->second.user, args, count, emit, sink, error, capacity);
        if (error[capacity - 1] != 0) { error[capacity - 1] = 0; return FDKSS_ERROR; }
        return status ? FDKSS_ERROR : FDKSS_OK;
    } catch (...) { std::snprintf(error, capacity, "typed native function threw"); return FDKSS_ERROR; }
}
Mod *context(lua_State *L) {
    return static_cast<Mod *>(lua_touserdata(L, lua_upvalueindex(1)));
}
int on(lua_State *L) {
    auto *mod = context(L);
    const char *event = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!*event) return luaL_error(L, "event name cannot be empty");
    if (mod->dispatching) return luaL_error(L, "register callbacks during script initialization only");
    lua_pushvalue(L, 2);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    bool failed = false;
    const auto token = mod->next_token++;
    try { mod->handlers[event].push_back({ref, token}); } catch (...) { failed = true; }
    if (failed) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return luaL_error(L, "could not register callback");
    }
    lua_pushinteger(L, token);
    return 1;
}
int off(lua_State *L) {
    auto *mod = context(L);
    const auto token = luaL_checkinteger(L, 1);
    for (auto &[event, handlers] : mod->handlers) {
        (void)event;
        for (auto &handler : handlers) {
            if (handler.token == token && handler.reference != LUA_NOREF) {
                luaL_unref(L, LUA_REGISTRYINDEX, handler.reference);
                handler.reference = LUA_NOREF;
                lua_pushboolean(L, true); return 1;
            }
        }
    }
    lua_pushboolean(L, false); return 1;
}
int log(lua_State *L) {
    const char *message = luaL_checkstring(L, 1);
    auto *mod = context(L);
    mod->runtime->log(mod->name.c_str(), message);
    return 0;
}
int escape_nuls(lua_State *L) {
    size_t length = 0;
    const char *text = lua_tolstring(L, 1, &length);
    luaL_Buffer buffer;
    luaL_buffinit(L, &buffer);
    for (size_t i = 0; i < length; ++i) {
        if (text[i] == '\0') luaL_addstring(&buffer, "\\0");
        else luaL_addchar(&buffer, text[i]);
    }
    luaL_pushresult(&buffer);
    return 1;
}
void append_tostring(lua_State *L, luaL_Buffer *buffer, int index) {
    size_t length = 0;
    const char *text = luaL_tolstring(L, index, &length);
    if (std::memchr(text, 0, length)) {
        lua_pushcfunction(L, escape_nuls);
        lua_insert(L, -2);
        lua_call(L, 1, 1);
    }
    luaL_addvalue(buffer);
}
int print(lua_State *L) {
    const int count = lua_gettop(L);
    luaL_Buffer buffer;
    luaL_buffinit(L, &buffer);
    for (int index = 1; index <= count; ++index) {
        if (index > 1) luaL_addchar(&buffer, '\t');
        append_tostring(L, &buffer, index);
    }
    luaL_pushresult(&buffer);
    auto *mod = context(L);
    mod->runtime->log(mod->name.c_str(), lua_tostring(L, -1));
    return 0;
}
int native_call(lua_State *L) {
    auto *mod = context(L);
    const char *name = luaL_checkstring(L, 1);
    const char *input = luaL_optstring(L, 2, "");
    // No nontrivial automatic objects may remain alive across luaL_error's longjmp.
    fdkss_native_fn callback = nullptr;
    void *user = nullptr;
    {
        const auto local = mod->local_natives.find(name);
        if (local != mod->local_natives.end()) { callback = local->second.callback; user = local->second.user; }
        const auto it = mod->runtime->natives.find(name);
        if (!callback && it != mod->runtime->natives.end()) {
            callback = it->second.callback;
            user = it->second.user;
        }
    }
    if (!callback) return luaL_error(L, "native function unavailable: %s", name);
    char output[16384]{};
    int status = -1;
    bool threw = false;
    try { status = callback(user, input, output, sizeof(output)); }
    catch (...) { threw = true; }
    if (threw) return luaL_error(L, "native function threw: %s", name);
    if (output[sizeof(output) - 1] != '\0')
        return luaL_error(L, "native output exceeded capacity: %s", name);
    if (status != 0) return luaL_error(L, "native %s failed: %s", name, output);
    lua_pushstring(L, output);
    return 1;
}
struct TypedResults {
    std::array<fdkss_value, 64> values{};
    std::array<char, 16384> strings{};
    size_t count = 0, used = 0;
    bool failed = false;
};
int FDKSS_CALL collect_result(void *sink, const fdkss_value *value) {
    auto &results = *static_cast<TypedResults *>(sink);
    if (results.failed) return 1;
    if (!value || results.count == results.values.size() ||
        value->type < FDKSS_NIL || value->type > FDKSS_STRING ||
        (value->type == FDKSS_NUMBER && !std::isfinite(value->number)) ||
        (value->type == FDKSS_STRING && ((!value->string && value->string_size) ||
            value->string_size > results.strings.size() - results.used))) {
        results.failed = true;
        return 1;
    }
    auto &copy = results.values[results.count++];
    copy = *value;
    if (value->type == FDKSS_STRING) {
        copy.string = results.strings.data() + results.used;
        if (value->string_size) std::copy_n(value->string, value->string_size,
            results.strings.data() + results.used);
        results.used += value->string_size;
    }
    return 0;
}
int typed_call(lua_State *L) {
    auto *mod = context(L);
    const char *name = luaL_checkstring(L, 1);
    const int count = lua_gettop(L) - 1;
    if (count > 64) return luaL_error(L, "typed native accepts at most 64 arguments");
    std::array<fdkss_value, 64> arguments{};
    for (int i = 0; i < count; ++i) {
        auto &value = arguments[i];
        const int index = i + 2;
        switch (lua_type(L, index)) {
        case LUA_TNIL: value.type = FDKSS_NIL; break;
        case LUA_TBOOLEAN: value.type = FDKSS_BOOLEAN; value.boolean = lua_toboolean(L, index); break;
        case LUA_TNUMBER:
            if (lua_isinteger(L, index)) { value.type = FDKSS_INTEGER; value.integer = lua_tointeger(L, index); }
            else {
                value.type = FDKSS_NUMBER; value.number = lua_tonumber(L, index);
                if (!std::isfinite(value.number)) return luaL_error(L, "argument %d must be finite", i + 1);
            }
            break;
        case LUA_TSTRING:
            value.type = FDKSS_STRING;
            value.string = lua_tolstring(L, index, &value.string_size);
            break;
        default: return luaL_error(L, "argument %d must be nil, boolean, number or string", i + 1);
        }
    }
    fdkss_typed_native_fn callback = nullptr;
    void *user = nullptr;
    {
        const auto local = mod->local_typed_natives.find(name);
        if (local != mod->local_typed_natives.end()) { callback = local->second.callback; user = local->second.user; }
        const auto it = mod->runtime->typed_natives.find(name);
        if (!callback && it != mod->runtime->typed_natives.end()) { callback = it->second.callback; user = it->second.user; }
    }
    if (!callback) return luaL_error(L, "typed native unavailable: %s", name);
    TypedResults results;
    char error[1024]{};
    int status = -1;
    bool threw = false;
    try { NativeCaller caller(*mod); status = callback(user, arguments.data(), count, collect_result, &results, error, sizeof(error)); }
    catch (...) { threw = true; }
    error[sizeof(error) - 1] = '\0';
    if (threw) return luaL_error(L, "typed native threw: %s", name);
    if (results.failed) return luaL_error(L, "typed native emitted invalid or excessive results: %s", name);
    if (status) return luaL_error(L, "typed native %s failed: %s", name, error);
    if (!lua_checkstack(L, static_cast<int>(results.count))) return luaL_error(L, "cannot allocate result stack");
    for (size_t i = 0; i < results.count; ++i) {
        const auto &value = results.values[i];
        switch (value.type) {
        case FDKSS_NIL: lua_pushnil(L); break;
        case FDKSS_BOOLEAN: lua_pushboolean(L, value.boolean); break;
        case FDKSS_INTEGER: lua_pushinteger(L, value.integer); break;
        case FDKSS_NUMBER: lua_pushnumber(L, value.number); break;
        case FDKSS_STRING: lua_pushlstring(L, value.string, value.string_size); break;
        }
    }
    return static_cast<int>(results.count);
}
int traceback(lua_State *L) {
    const char *message = lua_tostring(L, 1);
    luaL_traceback(L, L, message ? message : "non-string Lua error", 1);
    return 1;
}
int format_results(lua_State *L) {
    const int count = lua_gettop(L);
    luaL_Buffer buffer;
    luaL_buffinit(L, &buffer);
    for (int index = 1; index <= count; ++index) {
        if (index > 1) luaL_addchar(&buffer, '\t');
        append_tostring(L, &buffer, index);
    }
    luaL_pushresult(&buffer);
    return 1;
}
bool protected_call(Mod &mod, int arguments, int returns = 0) {
    auto *L = mod.state;
    const int function_index = lua_gettop(L) - arguments;
    lua_pushcfunction(L, traceback);
    lua_insert(L, function_index);
    const int result = lua_pcall(L, arguments, returns, function_index);
    if (result != LUA_OK) {
        const char *message = lua_tostring(L, -1);
        mod.runtime->log(mod.name.c_str(), message ? message : "Lua error");
        lua_pop(L, 1);
    }
    lua_remove(L, function_index);
    return result == LUA_OK;
}
void push_value(lua_State *L, const fdkss_value &value) {
    switch (value.type) {
    case FDKSS_NIL: lua_pushnil(L); break;
    case FDKSS_BOOLEAN: lua_pushboolean(L, value.boolean); break;
    case FDKSS_INTEGER: lua_pushinteger(L, value.integer); break;
    case FDKSS_NUMBER: lua_pushnumber(L, value.number); break;
    case FDKSS_STRING: lua_pushlstring(L, value.string ? value.string : "", value.string_size); break;
    }
}
bool valid_replacement(const fdkss_value &candidate, const fdkss_value &original, double minimum, double maximum) {
    if (candidate.type != original.type) return false;
    if (candidate.type == FDKSS_NUMBER)
        return std::isfinite(candidate.number) && candidate.number >= minimum && candidate.number <= maximum;
    if (candidate.type == FDKSS_INTEGER && minimum < maximum)
        return static_cast<long double>(candidate.integer) >= minimum && static_cast<long double>(candidate.integer) <= maximum;
    return true;
}
void dispatch_filter(Mod &mod, const char *event, fdkss_value *values, size_t count,
    size_t index, double minimum, double maximum) {
    if (mod.plugin) {
        auto candidate = values[index];
        if (mod.plugin->filter(event, values, count, index, candidate)) {
            if (valid_replacement(candidate, values[index], minimum, maximum)) values[index] = candidate;
            else {
                mod.runtime->log(mod.name.c_str(), "native filter returned invalid replacement; disabled");
                mod.plugin->reject_filter();
            }
        }
        mod.plugin->event(event, values, count);
    }
    const auto it = mod.handlers.find(event);
    if (it == mod.handlers.end()) return;
    if (!lua_checkstack(mod.state, static_cast<int>(count) + 8)) {
        mod.runtime->log(mod.name.c_str(), "could not allocate hook argument stack"); return;
    }
    mod.dispatching = true;
    for (auto &handler : it->second) {
        int &ref = handler.reference;
        if (ref == LUA_NOREF) continue;
        lua_rawgeti(mod.state, LUA_REGISTRYINDEX, ref);
        for (size_t i = 0; i < count; ++i) push_value(mod.state, values[i]);
        if (!protected_call(mod, static_cast<int>(count), 1)) {
            luaL_unref(mod.state, LUA_REGISTRYINDEX, ref); ref = LUA_NOREF; continue;
        }
        bool valid = true;
        if (!lua_isnil(mod.state, -1)) {
            auto candidate = values[index];
            if (candidate.type == FDKSS_NUMBER) {
                valid = lua_type(mod.state, -1) == LUA_TNUMBER;
                if (valid) candidate.number = lua_tonumber(mod.state, -1);
            } else if (candidate.type == FDKSS_INTEGER) {
                valid = lua_isinteger(mod.state, -1) != 0;
                if (valid) candidate.integer = lua_tointeger(mod.state, -1);
            } else {
                valid = lua_type(mod.state, -1) == LUA_TBOOLEAN;
                if (valid) candidate.boolean = lua_toboolean(mod.state, -1);
            }
            valid = valid && valid_replacement(candidate, values[index], minimum, maximum);
            if (valid) values[index] = candidate;
        }
        lua_pop(mod.state, 1);
        if (!valid) {
            mod.runtime->log(mod.name.c_str(), "hook returned invalid replacement; disabled");
            luaL_unref(mod.state, LUA_REGISTRYINDEX, ref); ref = LUA_NOREF;
        }
    }
    mod.dispatching = false;
}
bool dispatch(Mod &mod, const char *event, const char *payload = nullptr,
              const double *delta = nullptr, const fdkss_value *values = nullptr, size_t count = 0) {
    bool ok = true;
    if (mod.plugin) {
        if (delta) ok = mod.plugin->update(*delta);
        else if (std::string_view(event) != "start" && std::string_view(event) != "stop") {
            fdkss_value argument{};
            if (payload) {
                argument.type = FDKSS_STRING; argument.string = payload; argument.string_size = std::char_traits<char>::length(payload);
                ok = mod.plugin->event(event, &argument, 1);
            } else ok = mod.plugin->event(event, values, count);
        }
    }
    const auto it = mod.handlers.find(event);
    if (it == mod.handlers.end()) return ok;
    if (!lua_checkstack(mod.state, static_cast<int>(count) + 8)) {
        mod.runtime->log(mod.name.c_str(), "could not allocate event argument stack");
        return false;
    }
    mod.dispatching = true;
    for (auto &handler : it->second) {
        int &ref = handler.reference;
        if (ref == LUA_NOREF) continue;
        lua_rawgeti(mod.state, LUA_REGISTRYINDEX, ref);
        int nargs = 0;
        if (values) {
            for (size_t i = 0; i < count; ++i) {
                const auto &value = values[i];
                switch (value.type) {
                case FDKSS_NIL: lua_pushnil(mod.state); break;
                case FDKSS_BOOLEAN: lua_pushboolean(mod.state, value.boolean); break;
                case FDKSS_INTEGER: lua_pushinteger(mod.state, value.integer); break;
                case FDKSS_NUMBER: lua_pushnumber(mod.state, value.number); break;
                case FDKSS_STRING: lua_pushlstring(mod.state, value.string ? value.string : "", value.string_size); break;
                }
            }
            nargs = static_cast<int>(count);
        } else if (delta) { lua_pushnumber(mod.state, *delta); nargs = 1; }
        else if (payload) { lua_pushstring(mod.state, payload); nargs = 1; }
        if (!protected_call(mod, nargs)) {
            luaL_unref(mod.state, LUA_REGISTRYINDEX, ref);
            ref = LUA_NOREF;
            ok = false;
        }
    }
    mod.dispatching = false;
    return ok;
}
std::unique_ptr<Mod> load(fdkss_runtime &runtime, const fs::path &directory) {
    auto mod = std::make_unique<Mod>(&runtime, directory.filename().string());
    const auto native = directory / "Native" / "main.dll";
    if (fs::is_regular_file(native)) {
        fdkss_plugin_host host{};
        host.size = sizeof(host); host.abi_version = FDKSS_PLUGIN_ABI_VERSION;
        host.context = mod.get(); host.mod_name = mod->name.c_str(); host.log = plugin_log;
        host.register_string = plugin_register_string; host.register_typed = plugin_register_typed;
        host.call_string = plugin_call_string; host.invoke = plugin_invoke;
        mod->plugin = std::make_unique<NativePlugin>(native, host);
        mod->registering_plugin = true;
        const bool started = mod->plugin->start();
        mod->registering_plugin = false;
        if (!started) return nullptr;
    }
    mod->state = luaL_newstate();
    if (!mod->state) throw std::runtime_error("could not allocate Lua state");
    auto *L = mod->state;
    // Trusted local mods: standard libraries are deliberately available.
    luaL_openlibs(L);
    lua_pushlightuserdata(L, mod.get());
    lua_pushcclosure(L, print, 1);
    lua_setglobal(L, "print");
    lua_newtable(L);
    for (const auto &binding : {std::pair{"on", on}, {"off", off}, {"log", log}, {"call", native_call}, {"invoke", typed_call}}) {
        lua_pushlightuserdata(L, mod.get());
        lua_pushcclosure(L, binding.second, 1);
        lua_setfield(L, -2, binding.first);
    }
    lua_pushstring(L, mod->name.c_str()); lua_setfield(L, -2, "mod_name");
    lua_pushstring(L, "0.3.0"); lua_setfield(L, -2, "version");
    lua_setglobal(L, "fdk");
    // Root references resolve on every access; they never cache engine pointers.
    constexpr char object_api[] = R"lua(
        local function resource_copy(hp, max_hp, ep, max_ep, cp, max_cp)
            if hp == nil then return nil end
            return {hp=hp, max_hp=max_hp, ep=ep, max_ep=max_ep, cp=cp, max_cp=max_cp}
        end
        function fdk.player_resources()
            return resource_copy(fdk.invoke('engine.player_resources'))
        end
        function fdk.battle_snapshot(...)
            assert(select('#',...)==0,'battle_snapshot expects no arguments')
            local count,bytes=fdk.invoke('engine.battle_snapshot')
            if count==nil then return nil end
            assert(math.type(count)=='integer' and count>=0 and count<=70 and
                   type(bytes)=='string' and #bytes==count*32,'invalid battle snapshot')
            local records={}
            for i=1,count do
                local word,level,hp,max_hp,ep,max_ep,cp,max_cp=string.unpack('<I4I4I4I4I4I4I4I4',bytes,(i-1)*32+1)
                assert(hp<=max_hp and ep<=max_ep and cp<=max_cp and
                       max_hp<=2147483647 and max_ep<=2147483647 and max_cp<=2147483647,'invalid battle resources')
                records[i]={index=i-1,status_word=word,level=level,
                    hp=hp,max_hp=max_hp,ep=ep,max_ep=max_ep,cp=cp,max_cp=max_cp}
            end
            return {count=count,records=records,atomic=false}
        end
        function fdk.set_player_resource(name, value)
            return fdk.invoke('engine.set_player_resource', name, value)
        end
        local function condition_copy(count, bytes)
            if count == nil then return nil end
            assert(math.type(count)=='integer' and count>=0 and count<=128 and
                   type(bytes)=='string' and #bytes==count*72,'invalid condition snapshot')
            local records={}
            for i=1,count do
                local raw=bytes:sub((i-1)*72+1,i*72)
                records[i]={raw_key=string.unpack('<I4',raw),raw_bytes=raw}
            end
            return records
        end
        function fdk.player_conditions()
            return condition_copy(fdk.invoke('engine.player_conditions'))
        end
        local function model_copy(name,version,meshes,nodes)
            if name==nil then return nil end
            assert(type(name)=='string' and #name<=63 and not name:find('\0',1,true) and
                math.type(version)=='integer' and version>=4 and version<=5 and
                math.type(meshes)=='integer' and meshes>=0 and meshes<=10000 and
                math.type(nodes)=='integer' and nodes>=0 and nodes<=10000,'invalid model snapshot')
            return {name=name,format_version=version,mesh_count=meshes,node_count=nodes}
        end
    )lua" R"lua(
        function fdk.object(root)
            assert(type(root) == 'string', 'engine root must be a string')
            return setmetatable({}, {
                __index = function(_, key)
                    if key == 'status_value' then
                        return function(_, selector) return fdk.invoke('engine.character_status_value',root,selector) end
                    end
                    if key == 'model_info' then
                        return function(_, ...)
                            assert(select('#',...)==0,'model_info expects no arguments')
                            return model_copy(fdk.invoke('engine.actor_model',root))
                        end
                    end
                    if key == 'resources' then
                        return function(_) return resource_copy(fdk.invoke('engine.character_resources',root)) end
                    end
                    if key == 'conditions' then
                        return function(_) return condition_copy(fdk.invoke('engine.character_conditions',root)) end
                    end
                    if key == 'call' then
                        return function(_, name, ...) return fdk.invoke('engine.call', root, name, ...) end
                    end
                    if key == 'class_info' or key == 'is_a' then
                        return function(_, expected)
                            if key == 'is_a' then assert(type(expected)=='string', 'class name must be a string') end
                            local info
                            for index=0,1023 do
                                local name, base, count, flags, offset, constructor, contained, member, vbtable, displacement, attributes =
                                    fdk.invoke('engine.class_info',root,index)
                                assert(type(name)=='string' and type(base)=='string' and math.type(count)=='integer' and count>=1 and count<=1024,
                                    'invalid class metadata')
                                if not info then info={name=name, flags=flags, object_offset=offset, constructor_offset=constructor, bases={}} end
                                assert(name==info.name and count==(info.count or count), 'class metadata changed during inspection')
                                info.count=count
                                info.bases[#info.bases+1]={name=base,contained_bases=contained,member_offset=member,
                                    vbtable_offset=vbtable,virtual_displacement=displacement,attributes=attributes}
                                if index+1==count then break end
                            end
                            if key=='class_info' then return info end
                            for _, base in ipairs(info.bases) do if base.name==expected then return true end end
                            return false
                        end
                    end
                    return fdk.invoke('engine.get', root, key)
                end,
                __newindex = function(_, key, value) fdk.invoke('engine.set', root, key, value) end,
                __metatable = 'FDK root reference',
                __tostring = function() return 'FDK root: ' .. root end
            })
        end
        fdk.engine = {}
        function fdk.command_group(group)
            assert(math.type(group)=='integer' and group>=0 and group<28, 'command group must be 0..27')
            return fdk.object('script_group:'..group)
        end
        function fdk.find_command_groups(class_filter)
            assert(class_filter==nil or type(class_filter)=='string', 'class filter must be a string')
            local result={}
            for group=0,27 do
                local object=fdk.command_group(group)
                local class=object.class
                assert(type(class)=='string', 'command group unavailable')
                if class_filter==nil or class:find(class_filter,1,true) then result[#result+1]=object end
            end
            return result
        end
        local function find_scene_objects(class_filter, name_filter, first_only, include_derived)
            assert(class_filter == nil or type(class_filter) == 'string', 'class filter must be a string')
            assert(name_filter == nil or type(name_filter) == 'string', 'name filter must be a string')
            assert(include_derived == nil or type(include_derived)=='boolean', 'include_derived must be a boolean')
            assert(not include_derived or (type(class_filter)=='string' and #class_filter>0), 'inherited search requires a class name')
            local objects, offset = {}, 0
            repeat
                local page = table.pack(fdk.invoke('engine.list', offset, 32, include_derived and '' or class_filter or ''))
                local next_offset = page[1]
                assert(next_offset == -1 or next_offset > offset, 'invalid object enumeration cursor')
                for i = 2, page.n do
                    local object = fdk.object(page[i])
                    if (name_filter == nil or object.name:find(name_filter, 1, true)) and
                        (not include_derived or object:is_a(class_filter)) then
                        if first_only then return object end
                        objects[#objects + 1] = object
                    end
                end
                offset = next_offset
            until offset == -1
            if first_only then return nil end
            return objects
        end
        function fdk.find_objects(class_filter, name_filter, include_derived)
            return find_scene_objects(class_filter, name_filter, false, include_derived)
        end
        function fdk.find_object(class_filter, name_filter, include_derived)
            return find_scene_objects(class_filter, name_filter, true, include_derived)
        end
        function fdk.hook(name, phase, callback)
            assert(phase == 'pre' or phase == 'post', 'hook phase must be pre or post')
            assert(type(callback) == 'function', 'hook callback must be a function')
            assert(fdk.invoke('engine.hook_supported', name), 'unsupported engine function hook')
            if name == 'script.command.argument' then
                assert(phase == 'pre', 'command argument filters support pre phase only')
                return fdk.on('hook.' .. name .. '.' .. phase, callback)
            end
            if name == 'script.command' or name == 'actor.type_name' or name == 'actor.type_id' or name == 'actor.type_flags' or name == 'actor.get_transform' or name=='actor.set_placement' or name=='status.resource_change' then return fdk.on('hook.' .. name .. '.' .. phase, callback) end
            return fdk.on('hook.' .. name .. '.' .. phase, function(root, ...)
                return callback(fdk.object(root), ...)
            end)
        end
        fdk.unhook = fdk.off
        function fdk.bind_key(key, callback, modifiers)
            assert(math.type(key) == 'integer' and key >= 8 and key < 255 and key ~= 0x78 and key ~= 0x79 and key ~= 0x7a,
                'key must be a Windows keyboard virtual-key code; F9/F10/F11 are reserved')
            modifiers = modifiers or 0
            assert(math.type(modifiers) == 'integer' and modifiers >= 0 and modifiers <= 7,
                'key modifiers must be a bitmask: Shift=1, Ctrl=2, Alt=4')
            assert(type(callback) == 'function', 'key callback must be a function')
            return fdk.on('input.key', function(code, pressed, held)
                if code == key and pressed and held == modifiers then callback(code, held) end
            end)
        end
        fdk.unbind_key = fdk.off
        -- One permanent update handler lets timers be scheduled from callbacks without
        -- mutating the C++ handler vector during dispatch.
        local timers, timer_count, next_timer = {}, 0, 0
        local function schedule(seconds, callback, repeating, ...)
            assert(type(seconds) == 'number' and seconds == seconds and seconds >= 0 and seconds <= 31536000,
                'timer delay must be finite and between zero and one year')
            assert(not repeating or seconds >= 0.000001, 'timer interval must be at least one microsecond')
            assert(type(callback) == 'function', 'timer callback must be a function')
            assert(timer_count < 1024, 'mod timer limit reached')
            assert(next_timer < math.maxinteger, 'timer identifiers exhausted')
            next_timer = next_timer + 1
            timers[next_timer] = {remaining=seconds, interval=repeating and seconds, callback=callback, args=table.pack(...)}
            timer_count = timer_count + 1
            return next_timer
        end
        function fdk.delay(seconds, callback, ...) return schedule(seconds, callback, false, ...) end
        function fdk.every(seconds, callback, ...) return schedule(seconds, callback, true, ...) end
        function fdk.cancel_timer(token)
            if timers[token] == nil then return false end
            timers[token] = nil
            timer_count = timer_count - 1
            return true
        end
        fdk.on('update', function(dt)
            local snapshot = {}
            for token in pairs(timers) do snapshot[#snapshot + 1] = token end
            table.sort(snapshot)
            for _, token in ipairs(snapshot) do
                local timer = timers[token]
                if timer then
                    timer.remaining = timer.remaining - dt
                    if timer.remaining <= 0 then
                        if timer.interval then
                            -- Coalesce missed periods to one callback; keep the next phase.
                            timer.remaining = timer.interval - ((-timer.remaining) % timer.interval)
                        else
                            fdk.cancel_timer(token)
                        end
                        local ok, failure = xpcall(timer.callback, debug.traceback, table.unpack(timer.args, 1, timer.args.n))
                        if not ok then
                            fdk.cancel_timer(token)
                            fdk.log('timer callback failed: ' .. tostring(failure))
                        end
                    end
                end
            end
        end)
        local function enumerate(binding, ...)
            local result, offset, args = {}, 0, table.pack(...)
            repeat
                local page
                if binding == 'engine.script_names' then
                    page = table.pack(fdk.invoke(binding, offset, 32, args[1]))
                else
                    page = table.pack(fdk.invoke(binding, args[1], offset, 32))
                end
                for i = 2, page.n do result[#result + 1] = page[i] end
                assert(page[1] == -1 or page[1] > offset, 'invalid script enumeration cursor')
                offset = page[1]
            until offset == -1
            return result
        end
        function fdk.script(name)
            assert(type(name) == 'string', 'script name must be a string')
            return setmetatable({}, {
                __index = {
                    info = function()
                        local size, count, inputs, outputs = fdk.invoke('engine.script_info', name)
                        if size == nil then return nil end
                        return {name=name, size_bytes=size, function_count=count, input_variables=inputs, output_variables=outputs}
                    end,
                    functions = function() return enumerate('engine.script_functions', name) end,
                    variables = function() return enumerate('engine.script_variables', name) end,
                    set_variable = function(_, variable, value) return fdk.invoke('engine.script_variable_set', name, variable, value) end,
                    variable_info = function(_, variable)
                        local index, metadata, raw, value = fdk.invoke('engine.script_variable_info', name, variable)
                        if index == nil then return nil end
                        return {name=variable, index=index, raw_metadata=metadata, raw_value=raw, value=value}
                    end,
                    call_constant = function(_, fn) return fdk.invoke('engine.script_constant', name, fn) end,
                    call_pure = function(_, fn, ...) return fdk.invoke('engine.script_pure', name, fn, ...) end,
                    call_pure_async = function(_, fn, quantum, ...) return fdk.invoke('engine.script_pure_async', quantum, name, fn, ...) end,
                    preview_pure = function(_, fn, ...) return fdk.invoke('engine.script_pure_preview', name, fn, ...) end,
                    function_at = function(_, index)
                        local fn, index, inputs, outputs, flags0, flags1, checksum, code = fdk.invoke('engine.script_function_at', name, index)
                        if fn == nil then return nil end
                        return {name=fn, index=index, input_count=inputs, default_count=outputs, flags0=flags0,
                            flags1=flags1, checksum=checksum, code_offset=code}
                    end,
                    function_info = function(_, fn)
                        local index, inputs, outputs, flags0, flags1, checksum, code = fdk.invoke('engine.script_function_info', name, fn)
                        if index == nil then return nil end
                        return {name=fn, index=index, input_count=inputs, default_count=outputs, flags0=flags0,
                            flags1=flags1, checksum=checksum, code_offset=code}
                    end
                },
                __newindex = function() error('script references are read-only') end,
                __metatable = 'FDK script reference',
                __tostring = function() return 'FDK script: ' .. name end
            })
        end
        function fdk.script_wait(seconds, quantum) return fdk.invoke('engine.vm_wait', seconds, quantum == nil and 16 or quantum) end
        function fdk.call_compiled_async(bytes, fn, quantum, ...) return fdk.invoke('engine.script_compiled_async', bytes, fn, quantum == nil and 16 or quantum, ...) end
        function fdk.call_compiled(bytes, fn, ...) return fdk.invoke('engine.script_compiled', bytes, fn, ...) end
        function fdk.preview_compiled(bytes, fn, ...) return fdk.invoke('engine.script_compiled_preview', bytes, fn, ...) end
        function fdk.find_scripts(name_filter)
            local result = {}
            for _, name in ipairs(enumerate('engine.script_names', name_filter or '')) do
                result[#result + 1] = fdk.script(name)
            end
            return result
        end
        for _, root in ipairs({'application', 'framework', 'fieldmap', 'scene', 'player_controller', 'player', 'script_manager', 'player_model'}) do
            fdk.engine[root] = fdk.object(root)
        end
    )lua";
    if (luaL_loadbufferx(L, object_api, sizeof(object_api) - 1, "@FDKss/object-api", "t") != LUA_OK ||
        !protected_call(*mod, 0)) throw std::runtime_error("could not initialize object API");
    if (luaL_loadbufferx(L, actor_notifications::lua, sizeof(actor_notifications::lua)-1,
        "@FDKss/actor-notifications", "t") != LUA_OK || !protected_call(*mod, 0))
        throw std::runtime_error("could not initialize actor notifications");
    lua_getglobal(L, "package");
    const std::string scripts = (directory / "Scripts").generic_string();
    const std::string search = scripts + "/?.lua;" + scripts + "/?/init.lua";
    lua_pushstring(L, search.c_str()); lua_setfield(L, -2, "path");
    // Binary Lua extensions must be exposed through the adapter's native registry.
    lua_pushliteral(L, ""); lua_setfield(L, -2, "cpath");
    lua_pop(L, 1);
    const auto entry = directory / "Scripts" / "main.lua";
    std::ifstream file(entry, std::ios::binary);
    if (!file) {
        if (mod->plugin && !fs::exists(entry)) {
            runtime.log(mod->name.c_str(), "loaded native-only mod");
            return mod;
        }
        if (!fs::exists(entry) && fs::is_directory(directory / "Assets")) {
            runtime.log(mod->name.c_str(), "loaded content-only mod; content requires a supporting adapter");
            return mod;
        }
        runtime.log(mod->name.c_str(), "could not open Scripts/main.lua");
        return nullptr;
    }
    const std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (file.bad()) throw std::runtime_error("could not read mod script");
    const auto entry_utf8 = entry.u8string();
    const std::string chunk_name = "@" + std::string(reinterpret_cast<const char *>(entry_utf8.data()), entry_utf8.size());
    if (luaL_loadbufferx(L, source.data(), source.size(), chunk_name.c_str(), "t") != LUA_OK) {
        runtime.log(mod->name.c_str(), lua_tostring(L, -1));
        return nullptr;
    }
    if (!protected_call(*mod, 0)) return nullptr;
    if (!dispatch(*mod, "start")) {
        dispatch(*mod, "stop");
        return nullptr;
    }
    runtime.log(mod->name.c_str(), "loaded");
    return mod;
}
bool valid_name(const char *name) {
    if (!name || !*name) return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(name); *p; ++p)
        if (!( (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
               (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    return true;
}
template<class Operation>
fdkss_status guarded(fdkss_runtime *runtime, Operation operation) noexcept {
    if (!runtime) return FDKSS_INVALID_ARGUMENT;
    if (runtime->owner != std::this_thread::get_id()) return FDKSS_WRONG_THREAD;
    if (runtime->busy) return FDKSS_REENTRANT;
    runtime->busy = true;
    struct Reset { bool &busy; ~Reset() { busy = false; } } reset{runtime->busy};
    try {
        runtime->error.clear();
        return operation();
    } catch (const std::exception &e) {
        try { runtime->error = e.what(); } catch (...) {}
    } catch (...) {
        try { runtime->error = "unknown runtime error"; } catch (...) {}
    }
    return FDKSS_ERROR;
}
void require_started(fdkss_runtime *r) {
    if (!r->started) throw std::runtime_error("runtime has not started");
}
}

extern "C" {
uint32_t FDKSS_CALL fdkss_abi_version(void) { return FDKSS_ABI_VERSION; }
fdkss_status FDKSS_CALL fdkss_create(const char *root, fdkss_log_fn logger,
                                   void *user, fdkss_runtime **out) {
    if (!out) return FDKSS_INVALID_ARGUMENT;
    *out = nullptr;
    if (!root || !*root) return FDKSS_INVALID_ARGUMENT;
    try {
        auto runtime = std::make_unique<fdkss_runtime>();
        runtime->root = fs::absolute(fs::path(std::u8string(reinterpret_cast<const char8_t *>(root))));
        if (!fs::is_directory(runtime->root)) return FDKSS_INVALID_ARGUMENT;
        runtime->logger = logger;
        runtime->log_user = user;
        *out = runtime.release();
        return FDKSS_OK;
    } catch (...) { return FDKSS_ERROR; }
}
fdkss_status FDKSS_CALL fdkss_register_native(fdkss_runtime *r, const char *name,
                                            fdkss_native_fn callback, void *user) {
    return guarded(r, [&] {
        if (!name || !*name || !callback) return FDKSS_INVALID_ARGUMENT;
        if (r->started) throw std::runtime_error("register native functions before starting");
        if (r->typed_natives.contains(name) || !r->natives.emplace(name, Native{callback, user}).second)
            throw std::runtime_error("native function already registered");
        return FDKSS_OK;
    });
}
fdkss_status FDKSS_CALL fdkss_register_typed_native(fdkss_runtime *r, const char *name,
                                                  fdkss_typed_native_fn callback, void *user) {
    return guarded(r, [&] {
        if (!name || !*name || !callback) return FDKSS_INVALID_ARGUMENT;
        if (r->started) throw std::runtime_error("register native functions before starting");
        if (r->natives.contains(name) || !r->typed_natives.emplace(name, TypedNative{callback, user}).second)
            throw std::runtime_error("native function already registered");
        return FDKSS_OK;
    });
}
fdkss_status FDKSS_CALL fdkss_start(fdkss_runtime *r) {
    return guarded(r, [&] {
        if (r->started) throw std::runtime_error("runtime already started");
        std::vector<fs::path> directories;
        for (const auto &entry : fs::directory_iterator(r->root)) {
            if (entry.is_directory() && fs::is_regular_file(entry.path() / "enabled.txt")) {
                const auto name = entry.path().filename().string();
                if (valid_name(name.c_str())) directories.push_back(entry.path());
                else r->log(name.c_str(), "skipped: use letters, digits, underscores or hyphens in mod names");
            }
        }
        std::sort(directories.begin(), directories.end());
        r->mods.reserve(directories.size());
        r->started = true;
        for (const auto &directory : directories) {
            try {
                auto mod = load(*r, directory);
                if (mod) r->mods.push_back(std::move(mod));
            } catch (const std::exception &error) { r->log(directory.filename().string().c_str(), error.what()); }
            catch (...) { r->log(directory.filename().string().c_str(), "mod load failed"); }
        }
        return FDKSS_OK;
    });
}
fdkss_status FDKSS_CALL fdkss_tick(fdkss_runtime *r, double delta) {
    return guarded(r, [&] {
        if (!std::isfinite(delta) || delta < 0) return FDKSS_INVALID_ARGUMENT;
        require_started(r);
        for (auto &mod : r->mods) dispatch(*mod, "update", nullptr, &delta);
        return FDKSS_OK;
    });
}
fdkss_status FDKSS_CALL fdkss_emit(fdkss_runtime *r, const char *event, const char *payload) {
    return guarded(r, [&] {
        if (!event || !*event || !payload) return FDKSS_INVALID_ARGUMENT;
        if (std::string(event) == "start" || std::string(event) == "update" ||
            std::string(event) == "stop") return FDKSS_INVALID_ARGUMENT;
        require_started(r);
        for (auto &mod : r->mods) dispatch(*mod, event, payload);
        return FDKSS_OK;
    });
}
fdkss_status FDKSS_CALL fdkss_emit_values(fdkss_runtime *r, const char *event,
    const fdkss_value *values, size_t count) {
    return guarded(r, [&] {
        if (!event || !*event || count > 64 || (!values && count)) return FDKSS_INVALID_ARGUMENT;
        if (std::string(event) == "start" || std::string(event) == "update" || std::string(event) == "stop")
            return FDKSS_INVALID_ARGUMENT;
        for (size_t i = 0; i < count; ++i) {
            const auto &value = values[i];
            if (value.type < FDKSS_NIL || value.type > FDKSS_STRING ||
                (value.type == FDKSS_NUMBER && !std::isfinite(value.number)) ||
                (value.type == FDKSS_STRING && !value.string && value.string_size)) return FDKSS_INVALID_ARGUMENT;
        }
        require_started(r);
        for (auto &mod : r->mods) dispatch(*mod, event, nullptr, nullptr, values, count);
        return FDKSS_OK;
    });
}
fdkss_status FDKSS_CALL fdkss_filter_values(fdkss_runtime *r, const char *event,
    fdkss_value *values, size_t count, size_t index, double minimum, double maximum) {
    return guarded(r, [&] {
        if (!event || !*event || !values || count > 64 || index >= count ||
            values[index].type < FDKSS_BOOLEAN || values[index].type > FDKSS_NUMBER ||
            !std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum) return FDKSS_INVALID_ARGUMENT;
        if (std::string_view(event) == "start" || std::string_view(event) == "update" || std::string_view(event) == "stop")
            return FDKSS_INVALID_ARGUMENT;
        for (size_t i = 0; i < count; ++i) {
            const auto &value = values[i];
            if (value.type < FDKSS_NIL || value.type > FDKSS_STRING ||
                (value.type == FDKSS_NUMBER && !std::isfinite(value.number)) ||
                (value.type == FDKSS_STRING && !value.string && value.string_size)) return FDKSS_INVALID_ARGUMENT;
        }
        if (!valid_replacement(values[index], values[index], minimum, maximum)) return FDKSS_INVALID_ARGUMENT;
        require_started(r);
        for (auto &mod : r->mods) dispatch_filter(*mod, event, values, count, index, minimum, maximum);
        return FDKSS_OK;
    });
}
fdkss_status FDKSS_CALL fdkss_reload(fdkss_runtime *r, const char *name) {
    return guarded(r, [&] {
        if (!valid_name(name)) return FDKSS_INVALID_ARGUMENT;
        require_started(r);
        const auto directory = r->root / name;
        if (!fs::is_regular_file(directory / "enabled.txt"))
            throw std::runtime_error("mod is not enabled");
        std::unique_ptr<Mod> candidate;
        try { candidate = load(*r, directory); }
        catch (const std::exception &error) { throw std::runtime_error(std::string("reload failed; previous mod retained: ") + error.what()); }
        if (!candidate) throw std::runtime_error("reload failed; previous mod retained (see log)");
        for (auto &mod : r->mods) {
            if (mod->name == name) {
                dispatch(*mod, "stop");
                mod = std::move(candidate);
                return FDKSS_OK;
            }
        }
        r->mods.push_back(std::move(candidate));
        return FDKSS_OK;
    });
}
fdkss_status FDKSS_CALL fdkss_mod_count(fdkss_runtime *r, size_t *out) {
    return guarded(r, [&] {
        if (!out) return FDKSS_INVALID_ARGUMENT;
        *out = r->mods.size();
        return FDKSS_OK;
    });
}
fdkss_status FDKSS_CALL fdkss_unload(fdkss_runtime *r, const char *name) {
    return guarded(r, [&] {
        if (!valid_name(name)) return FDKSS_INVALID_ARGUMENT;
        require_started(r);
        const auto it = std::find_if(r->mods.begin(), r->mods.end(), [&](const auto &mod) { return mod->name == name; });
        if (it == r->mods.end()) throw std::runtime_error("mod is not loaded");
        dispatch(**it, "stop");
        r->mods.erase(it);
        return FDKSS_OK;
    });
}
fdkss_status FDKSS_CALL fdkss_mod_name(fdkss_runtime *r, size_t index, char *output, size_t capacity) {
    if (output && capacity) output[0] = '\0';
    return guarded(r, [&] {
        if (!output || !capacity) return FDKSS_INVALID_ARGUMENT;
        if (index >= r->mods.size()) return FDKSS_INVALID_ARGUMENT;
        const auto &name = r->mods[index]->name;
        if (name.size() >= capacity) throw std::runtime_error("mod name exceeds output capacity");
        std::copy(name.begin(), name.end(), output); output[name.size()] = '\0';
        return FDKSS_OK;
    });
}
uint64_t FDKSS_CALL fdkss_native_caller(const fdkss_runtime *r) {
    return r && r->owner == std::this_thread::get_id() ? r->native_caller : 0;
}
int FDKSS_CALL fdkss_mod_alive(const fdkss_runtime *r, uint64_t identity) {
    if (!r || !identity || r->owner != std::this_thread::get_id() || r->busy) return 0;
    for (const auto &mod : r->mods) if (mod->identity == identity) return 1;
    return 0;
}
fdkss_status FDKSS_CALL fdkss_execute(fdkss_runtime *r, const char *name,
    const char *source, char *output, size_t capacity) {
    if (output && capacity) output[0] = '\0';
    return guarded(r, [&] {
        if (!valid_name(name) || !source || !output || !capacity) return FDKSS_INVALID_ARGUMENT;
        require_started(r);
        Mod *mod = nullptr;
        for (auto &candidate : r->mods) if (candidate->name == name) mod = candidate.get();
        if (!mod) throw std::runtime_error("mod is not loaded");
        auto *L = mod->state;
        const int top = lua_gettop(L);
        struct Restore {
            lua_State *L; int top; Mod *mod;
            ~Restore() { lua_settop(L, top); mod->dispatching = false; }
        } restore{L, top, mod};
        mod->dispatching = true;
        lua_pushcfunction(L, traceback);
        const int handler = lua_gettop(L);
        int status = luaL_loadbufferx(L, source, std::char_traits<char>::length(source), "@FDKss/console", "t");
        if (status == LUA_OK) status = lua_pcall(L, 0, LUA_MULTRET, handler);
        if (status != LUA_OK) throw std::runtime_error(lua_tostring(L, -1) ? lua_tostring(L, -1) : "Lua error");
        lua_remove(L, handler);
        const int results = lua_gettop(L) - top;
        lua_pushcfunction(L, format_results);
        lua_insert(L, top + 1);
        if (lua_pcall(L, results, 1, 0) != LUA_OK)
            throw std::runtime_error(lua_tostring(L, -1) ? lua_tostring(L, -1) : "result formatting failed");
        size_t length = 0;
        const char *text = lua_tolstring(L, -1, &length);
        if (length >= capacity) throw std::runtime_error("console result exceeded output capacity");
        if (length) std::copy_n(text, length, output);
        output[length] = '\0';
        return FDKSS_OK;
    });
}
const char *FDKSS_CALL fdkss_last_error(const fdkss_runtime *r) {
    if (!r) return "invalid runtime";
    if (r->owner != std::this_thread::get_id()) return "wrong thread";
    return r->error.c_str();
}
fdkss_status FDKSS_CALL fdkss_destroy(fdkss_runtime *r) {
    if (!r) return FDKSS_INVALID_ARGUMENT;
    const auto status = guarded(r, [&] {
        for (auto it = r->mods.rbegin(); it != r->mods.rend(); ++it) dispatch(**it, "stop");
        r->mods.clear();
        return FDKSS_OK;
    });
    if (status == FDKSS_OK) delete r;
    return status;
}
}
