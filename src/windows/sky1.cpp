#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include "sky1.h"
#include "player_resources.h"
#include "resource_hooks.h"
#include "battle_snapshots.h"
#include "character_snapshots.h"
#include "character_models.h"
#include "status_getter.h"
#include "resource_properties.h"
#include "placement.h"
#include "actor_methods.h"
#include "class_metadata.h"
#include "command_objects.h"
#include "resources.h"
#include "lifetimes.h"
#include "content.h"
#include <cmath>
#include <charconv>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>

namespace {
#define FDKSS_ACTOR_METHOD(name, slot, rva) constexpr std::uintptr_t actor_transform_rva = rva;
#include "fdkss/sky1_actor_methods.inc"
#undef FDKSS_ACTOR_METHOD
#define FDKSS_PLACEMENT(name, rva) constexpr std::uintptr_t placement_rva = rva;
#include "fdkss/sky1_placement.inc"
#undef FDKSS_PLACEMENT
constexpr std::uintptr_t application_rva = 0x8fba50;
constexpr std::uintptr_t framework_rva = 0x8ff030;
constexpr std::uintptr_t fieldmap_rva = 0x8fef78;
constexpr std::uintptr_t script_manager_rva = 0x8fefc8;
constexpr std::uintptr_t application_vtable_rva = 0x7da5a8;
constexpr std::uintptr_t application_update_rva = 0x2fb10;
constexpr std::size_t scene_offset = 0x108;
constexpr std::size_t player_offset = 0x640;
bool changed_scale = false;
std::uintptr_t changed_framework = 0;
float original_scale = 1;
float last_scale = 1;
bool changed_background = false;
std::uintptr_t background_framework = 0;
unsigned char original_background = 0, last_background = 0;
std::uint64_t scene_epoch = 0;
std::uintptr_t tracked_scene = 0, tracked_player = 0;
std::string tracked_map;
bool scene_changed = false;
std::string previous_map;
bool collection_cached = false;
std::vector<std::uintptr_t> cached_actors;
std::unordered_set<std::uintptr_t> cached_members;

template<typename T>
bool read(std::uintptr_t address, T &value) {
    SIZE_T count = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(address),
        &value, sizeof(T), &count) && count == sizeof(T);
}
std::uintptr_t base() { return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)); }
std::uintptr_t pointer(std::uintptr_t address) {
    std::uintptr_t value = 0;
    read(address, value);
    return value;
}
std::string class_name(std::uintptr_t address) {
    const auto table = pointer(address);
    if (table < 8) return {};
    const auto locator = pointer(table - 8);
    struct Locator { std::uint32_t signature, offset, constructor, type, hierarchy, self; } data{};
    if (!read(locator, data) || data.signature != 1 || locator < data.self || locator - data.self != base()) return {};
    char name[256]{};
    if (!read(base() + data.type + 16, name) || !std::memchr(name, 0, sizeof(name))) return {};
    return name;
}
bool actor_layout(std::uintptr_t address) {
    // Require a nonvirtual Actor@fdk base at offset zero, not a name substring.
    const auto table = pointer(address), locator = table >= 8 ? pointer(table - 8) : 0;
    struct Locator { std::uint32_t signature, offset, constructor, type, hierarchy, self; } data{};
    if (!read(locator, data) || data.signature != 1 || data.offset ||
        locator < data.self || locator - data.self != base()) return false;
    struct Hierarchy { std::uint32_t signature, flags, count, array; } hierarchy{};
    if (!read(base() + data.hierarchy, hierarchy) || hierarchy.signature ||
        !hierarchy.count || hierarchy.count > 1024) return false;
    for (std::uint32_t index = 0; index < hierarchy.count; ++index) {
        std::uint32_t descriptor = 0;
        struct Base { std::uint32_t type, count; std::int32_t offset, vbtable, displacement; std::uint32_t flags; } entry{};
        char name[64]{};
        if (!read(base() + hierarchy.array + index * 4, descriptor) ||
            !read(base() + descriptor, entry)) return false;
        if (entry.offset || entry.vbtable != -1) continue;
        if (read(base() + entry.type + 16, name) && std::memchr(name, 0, sizeof(name)) &&
            std::strcmp(name, ".?AVActor@fdk@@") == 0) return true;
    }
    return false;
}
std::uintptr_t object(std::uintptr_t slot, const char *expected) {
    const auto address = pointer(base() + slot);
    return class_name(address) == expected ? address : 0;
}
std::string json_string(const std::string &value) {
    std::string result = "\"";
    for (const unsigned char ch : value) {
        if (ch == '"' || ch == '\\') { result += '\\'; result += static_cast<char>(ch); }
        else if (ch >= 32 && ch < 127) result += static_cast<char>(ch);
        else result += '?';
    }
    return result + '"';
}
std::string object_json(std::uintptr_t address) {
    const auto type = class_name(address);
    if (type.empty()) return "null";
    char id[32]{};
    std::snprintf(id, sizeof(id), "0x%llx", static_cast<unsigned long long>(address));
    return "{\"address\":" + json_string(id) + ",\"class\":" + json_string(type) + "}";
}
int output(const std::string &value, char *buffer, std::size_t capacity) {
    const int count = std::snprintf(buffer, capacity, "%s", value.c_str());
    return count < 0 || static_cast<std::size_t>(count) >= capacity ? 1 : 0;
}
bool write_float(std::uintptr_t address, float value) {
    SIZE_T count = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(address), &value,
        sizeof(value), &count) && count == sizeof(value);
}
bool write_byte(std::uintptr_t address, unsigned char value) {
    SIZE_T count = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(address), &value,
        sizeof(value), &count) && count == sizeof(value);
}
unsigned char foreground_active() {
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId() ? 1 : 0;
}
std::uintptr_t root_object(std::string_view root) {
    if (root == "application") return object(application_rva, ".?AVApplication@sora@@");
    if (root == "framework") return object(framework_rva, ".?AVFramework@fdk@@");
    if (root == "script_manager") return object(script_manager_rva, ".?AVManager@script@sora@@");
    const auto manager = object(fieldmap_rva, ".?AVManager@fieldmap@sora@@");
    if (root == "fieldmap") return manager;
    if (!manager) return 0;
    if (root == "scene") {
        const auto address = pointer(manager + scene_offset);
        return class_name(address) == ".?AVScene@fieldmap@sora@@" ? address : 0;
    }
    const auto controller = pointer(manager + player_offset);
    if (class_name(controller) != ".?AVPlayer@fieldmap@sora@@") return 0;
    if (root == "player_controller") return controller;
    if (root == "player" || root == "player_model") {
        const auto actor = pointer(controller + 0x40);
        if (class_name(actor) != ".?AVActor@chara@sora@@") return 0;
        if (root == "player") return actor;
        const auto model = pointer(actor + 0x2a8);
        std::uint32_t magic=0, version=0;
        std::uint64_t meshes=0, nodes=0;
        return pointer(model)==base()+0x800db0 && class_name(model)==".?AVModel@fdk@@" &&
            read(model+0x10,magic) && magic==0x204c444d && read(model+0x14,version) && version==4 &&
            read(model+0x258,meshes) && meshes<=10000 && read(model+0x288,nodes) && nodes<=10000 ? model : 0;
    }
    return 0;
}
struct Property { const char *root; const char *name; std::size_t offset; fdkss_value_type type; bool wide_integer, byte_integer, wide_string; };
constexpr Property properties[] = {
#define FDKSS_PROPERTY(root, name, offset, type, storage, writable) {#root, #name, offset, type, std::string_view(#storage) == "uint64_t", std::string_view(#storage) == "uint8_t", std::string_view(#storage) == "char64"},
#include "fdkss/sky1_properties.inc"
#undef FDKSS_PROPERTY
};
bool property_arguments(const fdkss_value *args, std::size_t count,
    std::size_t expected, char *error, std::size_t capacity) {
    if (count != expected || args[0].type != FDKSS_STRING || args[1].type != FDKSS_STRING) {
        output("expected root and property strings", error, capacity);
        return false;
    }
    return true;
}
bool actor_collection() {
    if (collection_cached) return true;
    const auto scene = root_object("scene");
    struct Collection { std::uintptr_t data, count, capacity; } collection{};
    cached_actors.clear(); cached_members.clear();
    if (!scene) { collection_cached = true; return true; }
    if (!read(scene + 0x158, collection) || collection.count > collection.capacity ||
        collection.capacity > 16384 || (collection.count && !collection.data)) return false;
    cached_actors.resize(static_cast<std::size_t>(collection.count));
    if (cached_actors.empty()) { collection_cached = true; return true; }
    SIZE_T bytes = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(collection.data),
        cached_actors.data(), cached_actors.size() * sizeof(std::uintptr_t), &bytes) ||
        bytes != cached_actors.size() * sizeof(std::uintptr_t)) return false;
    cached_members.insert(cached_actors.begin(), cached_actors.end());
    collection_cached = true;
    return true;
}
std::uintptr_t resolve_actor(std::string_view token) {
    if (token.empty() || token[0] != '@') return 0;
    std::uint64_t fields[5]{};
    const auto parse = [](std::string_view text, std::uint64_t &value, int radix) {
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value, radix);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    };
    std::size_t begin = 1;
    for (std::size_t index = 0; index < 5; ++index) {
        const auto end = token.find(':', begin);
        if ((index < 4 && end == token.npos) || (index == 4 && end != token.npos) ||
            !parse(token.substr(begin, end == token.npos ? end : end - begin), fields[index], index < 3 ? 10 : 16)) return 0;
        begin = end == token.npos ? token.size() : end + 1;
    }
    const auto address = fields[3], table = fields[4];
    if (fields[0] != scene_epoch || !address || !table ||
        !actor_lifetimes::valid(address, {fields[1], fields[2]})) return 0;
    if (!actor_collection() || !cached_members.contains(static_cast<std::uintptr_t>(address)) ||
        pointer(address) != table || class_name(address).empty()) return 0;
    return static_cast<std::uintptr_t>(address);
}
}
namespace sky1 {
int FDKSS_CALL class_info(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    DWORD worker=0;const auto framework=root_object("framework");
    if (!framework || !read(framework+0x68,worker) || worker!=GetCurrentThreadId() ||
        count!=2 || args[0].type!=FDKSS_STRING) {
        output("class metadata requires the owning update thread and root/index arguments",error,capacity);return 1;
    }
    const std::string_view name(args[0].string,args[0].string_size);
    command_objects::Object group;
    const auto address=command_objects::named(name) ? (command_objects::resolve(base(),false,name,group) ? group.address : 0) :
        !name.empty() && name[0]=='@' ? resolve_actor(name) : root_object(name);
    return live_class_metadata::inspect(base(),address,args,count,emit,sink,error,capacity);
}
bool start_lifetimes() {
#if defined(FDKSS_TEST_PROFILE)
    return true;
#else
    struct Descriptor { std::uintptr_t table, destructor; const char *name; };
    constexpr Descriptor descriptors[] = {
        {0x7eb7d0, 0x1b9160, ".?AVActor@chara@sora@@"},
        {0x801c28, 0x47ef10, ".?AVActor@fdk@@"},
        {0x806640, 0x51ffd0, ".?AVBasicActor@fdk@@"},
        {0x7ded20, 0xe5c90, ".?AVCursorActor@TargetCursor@battle@sora@@"},
        {0x805600, 0x4fc840, ".?AVDirectionalLightActor@fdk@@"},
        {0x7efce8, 0x21f8b0, ".?AVDropItem@fieldmap@sora@@"},
        {0x8062c8, 0x512da0, ".?AVEnvSoundActor@fdk@@"},
        {0x7efeb8, 0x220b50, ".?AVEventBox@fieldmap@sora@@"},
        {0x7f03e0, 0x231c80, ".?AVGodRayActor@fieldmap@sora@@"},
        {0x7f04f0, 0x232430, ".?AVLightProbeActor@fieldmap@sora@@"},
        {0x7f0620, 0x233ff0, ".?AVLookPoint@fieldmap@sora@@"},
        {0x7f1818, 0x24c410, ".?AVMonsterAreaActor@fieldmap@sora@@"},
        {0x7f1e70, 0x257ad0, ".?AVNavigationPathActor@fieldmap@sora@@"},
        {0x7f2278, 0x258440, ".?AVObjectActor@fieldmap@sora@@"},
        {0x7f2af0, 0x2626f0, ".?AVParticleFogActor@fieldmap@sora@@"},
        {0x7f2d90, 0x266a10, ".?AVPathActor@fieldmap@sora@@"},
        {0x7f3130, 0x266de0, ".?AVPlantActor@fieldmap@sora@@"},
        {0x805740, 0x4fc1d0, ".?AVPointLightActor@fdk@@"},
        {0x8056a0, 0x4fc4a0, ".?AVSpotLightActor@fdk@@"},
        {0x7f3d90, 0x288220, ".?AVTerrainActor@fieldmap@sora@@"},
        {0x7dee08, 0xec190, ".?AVTurnAreaActor@battle@sora@@"},
        {0x805898, 0x4fdae0, ".?AVVFXActor@fdk@@"},
        {0x7f3e50, 0x289880, ".?AVVolumeFogActor@fieldmap@sora@@"},
    };
    std::vector<actor_lifetimes::Hook> hooks;
    for (const auto &entry : descriptors) hooks.push_back({reinterpret_cast<void **>(base() + entry.table),
        reinterpret_cast<actor_lifetimes::Destructor>(base() + entry.destructor), entry.name});
    return actor_lifetimes::install(hooks);
#endif
}
void emit_lifetime_events(fdkss_runtime *runtime) {
    if (scene_changed) {
        fdkss_value args[2]{};
        args[0].type = args[1].type = FDKSS_STRING;
        args[0].string = previous_map.data(); args[0].string_size = previous_map.size();
        args[1].string = tracked_map.data(); args[1].string_size = tracked_map.size();
        fdkss_emit_values(runtime, "engine.scene_changed", args, 2);
        scene_changed = false;
    }
    const auto events = actor_lifetimes::drain();
    for (const auto &entry : events.destroyed) {
        char address[32]{};
        std::snprintf(address, sizeof(address), "0x%llx", static_cast<unsigned long long>(entry.address));
        fdkss_value args[2]{};
        args[0].type = args[1].type = FDKSS_STRING;
        args[0].string = address; args[0].string_size = std::strlen(address);
        args[1].string = entry.class_name; args[1].string_size = std::strlen(entry.class_name);
        fdkss_emit_values(runtime, "engine.actor_destroyed", args, 2);
    }
    if (events.dropped) {
        fdkss_value count{}; count.type = FDKSS_INTEGER; count.integer = static_cast<std::int64_t>(events.dropped);
        fdkss_emit_values(runtime, "engine.lifecycle_overflow", &count, 1);
    }
}
void refresh_scene() {
    // Called once at each owning-thread update boundary before any Lua runs.
    collection_cached = false;
    const auto scene = root_object("scene"), player = root_object("player");
    char map[32]{};
    if (!scene || !read(scene + 0x30, map) || !std::memchr(map, 0, sizeof(map))) map[0] = 0;
    if (scene != tracked_scene || player != tracked_player || tracked_map != map) {
        if (!scene_changed) previous_map = tracked_map;
        scene_changed = true;
        ++scene_epoch; tracked_scene = scene; tracked_player = player; tracked_map = map;
    }
}
int FDKSS_CALL list_objects(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if (count != 3 || args[0].type != FDKSS_INTEGER || args[1].type != FDKSS_INTEGER || args[2].type != FDKSS_STRING ||
        args[0].integer < 0 || args[0].integer > 16384 || args[1].integer < 1 || args[1].integer > 63) {
        output("expected offset (0..16384), limit (1..63), class filter string", error, capacity); return 1;
    }
    if (!actor_collection()) { output("scene actor collection unavailable or invalid", error, capacity); return 1; }
    const auto &actors = cached_actors;
    const std::string_view filter(args[2].string, args[2].string_size);
    std::vector<std::string> tokens;
    auto index = static_cast<std::size_t>(args[0].integer);
    for (; index < actors.size() && tokens.size() < static_cast<std::size_t>(args[1].integer); ++index) {
        const auto type = class_name(actors[index]);
        if (type.empty() || type.find(filter) == std::string::npos) continue;
        const auto stamp = actor_lifetimes::observe(actors[index]);
        char token[144]{};
        std::snprintf(token, sizeof(token), "@%llu:%llu:%llu:%llx:%llx", static_cast<unsigned long long>(scene_epoch),
            static_cast<unsigned long long>(stamp.epoch), static_cast<unsigned long long>(stamp.generation),
            static_cast<unsigned long long>(actors[index]), static_cast<unsigned long long>(pointer(actors[index])));
        tokens.emplace_back(token);
    }
    fdkss_value value{}; value.type = FDKSS_INTEGER;
    value.integer = index < actors.size() ? static_cast<std::int64_t>(index) : -1;
    if (emit(sink, &value)) return 1;
    for (const auto &token : tokens) {
        value.type = FDKSS_STRING; value.string = token.data(); value.string_size = token.size();
        if (emit(sink, &value)) return 1;
    }
    return 0;
}
int model_snapshot(const fdkss_value *args,std::size_t count,
    fdkss_result_fn emit,void *sink,char *error,std::size_t capacity,bool objects) {
    DWORD worker=0;const auto framework=root_object("framework");
    if(!framework || !read(framework+0x68,worker) || worker!=GetCurrentThreadId()) {
        output("character model snapshots require the owning update thread",error,capacity);return 1;
    }
    return character_models::snapshot(base(),character_models::first,
        [](std::string_view reference){return reference=="player" ? root_object("player") : resolve_actor(reference);},
        ::player_resources::read,[](std::uintptr_t model){return class_name(model)==".?AVModel@fdk@@";},
        args,count,emit,sink,error,capacity,objects ? &character_models::object_first : nullptr);
}
int FDKSS_CALL character_model(void *,const fdkss_value *args,std::size_t count,
    fdkss_result_fn emit,void *sink,char *error,std::size_t capacity) {
    return model_snapshot(args,count,emit,sink,error,capacity,false);
}
int FDKSS_CALL actor_model(void *,const fdkss_value *args,std::size_t count,
    fdkss_result_fn emit,void *sink,char *error,std::size_t capacity) {
    return model_snapshot(args,count,emit,sink,error,capacity,true);
}
int FDKSS_CALL character_status_value(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    DWORD worker=0;const auto framework=root_object("framework");
    if(!framework || !read(framework+0x68,worker) || worker!=GetCurrentThreadId()) {
        output("native status reads require the owning update thread",error,capacity);return 1;
    }
    return status_getter::invoke(base(),::player_resources::sky1,
        [](std::string_view reference){return reference=="player" ? root_object("player") : resolve_actor(reference);},
        args,count,emit,sink,error,capacity);
}
int FDKSS_CALL character_resources(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    DWORD worker=0;const auto framework=root_object("framework");
    if(!framework || !read(framework+0x68,worker) || worker!=GetCurrentThreadId()) {
        output("character snapshots require the owning update thread",error,capacity);return 1;
    }
    return character_snapshots::snapshot(base(),::player_resources::sky1,::player_conditions::sky1,false,
        [](std::string_view reference){return reference=="player" ? root_object("player") : resolve_actor(reference);},
        args,count,emit,sink,error,capacity);
}
int FDKSS_CALL character_conditions(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    DWORD worker=0;const auto framework=root_object("framework");
    if(!framework || !read(framework+0x68,worker) || worker!=GetCurrentThreadId()) {
        output("character snapshots require the owning update thread",error,capacity);return 1;
    }
    return character_snapshots::snapshot(base(),::player_resources::sky1,::player_conditions::sky1,true,
        [](std::string_view reference){return reference=="player" ? root_object("player") : resolve_actor(reference);},
        args,count,emit,sink,error,capacity);
}
int FDKSS_CALL player_conditions(void *, const fdkss_value *, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    DWORD worker=0;const auto framework=root_object("framework");
    if(!framework || !read(framework+0x68,worker) || worker!=GetCurrentThreadId()) {
        output("player conditions require the owning update thread",error,capacity);return 1;
    }
    return ::player_conditions::snapshot(base(),::player_resources::sky1,::player_conditions::sky1,
        []{return root_object("player");},count,emit,sink,error,capacity);
}
int FDKSS_CALL set_player_resource(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    DWORD worker=0;const auto framework=root_object("framework");
    if(!framework || !read(framework+0x68,worker) || worker!=GetCurrentThreadId()) {
        output("player resource editing requires the owning update thread",error,capacity);return 1;
    }
    return ::player_resources::edit(base(),::player_resources::sky1,::player_resources::sky1_setter,
        []{return root_object("player");},args,count,emit,sink,error,capacity,resource_hooks::checked_target);
}
int FDKSS_CALL battle_roster(void *,const fdkss_value *,std::size_t count,
    fdkss_result_fn emit,void *sink,char *error,std::size_t capacity) {
    const auto owner=[] {
        DWORD worker=0;const auto framework=root_object("framework");
        return framework && read(framework+0x68,worker) && worker==GetCurrentThreadId();
    };
    return battle_snapshots::snapshot(base(),false,owner,count,emit,sink,error,capacity);
}
int FDKSS_CALL player_resources(void *, const fdkss_value *, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    DWORD worker=0;const auto framework=root_object("framework");
    if(!framework || !read(framework+0x68,worker) || worker!=GetCurrentThreadId()) {
        output("player resources require the owning update thread",error,capacity);return 1;
    }
    return ::player_resources::snapshot(base(),::player_resources::sky1,
        []{return root_object("player");},count,emit,sink,error,capacity);
}
int FDKSS_CALL get_property(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if (!property_arguments(args, count, 2, error, capacity)) return 1;
    std::string_view root(args[0].string, args[0].string_size);
    if (root=="player_model") {
        DWORD worker=0;const auto framework=root_object("framework");
        if (!framework || !read(framework+0x68,worker) || worker!=GetCurrentThreadId()) {
            output("model access requires the owning update thread",error,capacity);return 1;
        }
    }
    if(command_objects::named(root))return command_objects::get(base(),false,args,count,emit,sink,error,capacity);
    const std::string_view name(args[1].string, args[1].string_size);
    if(const auto resource=resource_properties::index(name);resource>=0)
        return resource_properties::get(character_resources,args,static_cast<unsigned>(resource),emit,sink,error,capacity);
    const bool handle = !root.empty() && root[0] == '@';
    const auto address = handle ? resolve_actor(root) : root_object(root);
    if (handle) {
        if (!address) { output("scene actor reference expired", error, capacity); return 1; }
        root = class_name(address) == ".?AVActor@chara@sora@@" ? "player" : "actor";
    }
    if (!handle && root != "application" && root != "framework" && root != "fieldmap" &&
        root != "scene" && root != "player_controller" && root != "player" && root != "player_model" && root != "script_manager") {
        output("unknown engine root", error, capacity); return 1;
    }
    const Property *property = nullptr;
    for (const auto &entry : properties) if (name == entry.name &&
        (root == entry.root || (std::strcmp(entry.root, "actor") == 0 &&
            ((root == "player" && (!address || actor_layout(address))) ||
                (handle && address && actor_layout(address)))))) property = &entry;
    if (!property && name != "class" && name != "address") {
        output("unknown property; this build exposes verified fields only", error, capacity); return 1;
    }
    fdkss_value value{};
    if (!address) return emit(sink, &value);
    if (name == "address") {
        char text[32]{};
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
        value.type = FDKSS_STRING; value.string = text; value.string_size = std::strlen(text);
        return emit(sink, &value);
    }
    if (name == "class") {
        const auto type = class_name(address);
        value.type = FDKSS_STRING; value.string = type.data(); value.string_size = type.size();
        return emit(sink, &value);
    }
    if (name == "yaw_radians")
        return character_placement::yaw(reinterpret_cast<void *>(address), emit, sink, error, capacity);
    value.type = property->type;
    bool valid = false;
    char text[32]{};
    if (value.type == FDKSS_BOOLEAN) {
        unsigned char boolean = 0;
        valid = read(address + property->offset, boolean) && boolean <= 1;
        value.boolean = boolean;
    } else if (value.type == FDKSS_NUMBER) {
        float number = 0;
        valid = read(address + property->offset, number) && std::isfinite(number);
        value.number = number;
    } else if (value.type == FDKSS_INTEGER) {
        if (property->byte_integer) {
            unsigned char number = 0;
            valid = read(address + property->offset, number);
            value.integer = number;
        } else if (property->wide_integer) {
            std::uint64_t number = 0;
            valid = read(address + property->offset, number) && number <= INT64_MAX;
            value.integer = static_cast<std::int64_t>(number);
        } else {
            DWORD number = 0; valid = read(address + property->offset, number); value.integer = number;
        }
    } else {
        if (property->wide_string) {
            char wide_text[64]{};
            if (!read(address+property->offset,wide_text) || !std::memchr(wide_text,0,sizeof(wide_text))) {
                output("property could not be read",error,capacity);return 1;
            }
            value.string=wide_text;value.string_size=std::strlen(wide_text);return emit(sink,&value);
        }
        valid = read(address + property->offset, text) && std::memchr(text, 0, sizeof(text));
        if (valid) { value.string = text; value.string_size = std::strlen(text); }
    }
    if (!valid) { output("property could not be read", error, capacity); return 1; }
    return emit(sink, &value);
}
int FDKSS_CALL read_resource(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    return resource_stream::read(base(), resource_stream::sky1, args, count, emit, sink, error, capacity);
}
int FDKSS_CALL set_property(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if (!property_arguments(args, count, 3, error, capacity)) return 1;
    const std::string_view root(args[0].string, args[0].string_size), property(args[1].string, args[1].string_size);
    if(resource_properties::index(property)>=0) {
        DWORD worker=0;const auto framework=root_object("framework");
        if(!framework || !read(framework+0x68,worker) || worker!=GetCurrentThreadId()) {
            output("resource editing requires the owning update thread",error,capacity);return 1;
        }
        return resource_properties::set(base(),::player_resources::sky1,::player_resources::sky1_setter,
            [](std::string_view reference){return reference=="player" ? root_object("player") : resolve_actor(reference);},
            args,emit,sink,error,capacity,resource_hooks::checked_target);
    }
    if(property=="native_state_byte") {
        DWORD worker=0;const auto framework=root_object("framework");
        if(!framework || !read(framework+0x68,worker) || worker!=GetCurrentThreadId()){output("native state editing requires the verified update thread",error,capacity);return 1;}
        const auto actor=!root.empty() && root[0]=='@' ? resolve_actor(root) : root_object(root);
        if(!actor || class_name(actor)!=".?AVActor@chara@sora@@"){output("native state editing requires a current character actor",error,capacity);return 1;}
        return actor_methods::state_byte(reinterpret_cast<void *>(actor),base(),false,args+2,1,emit,sink,error,capacity);
    }
    if (property == "position_x" || property == "position_y" || property == "position_z" || property == "yaw_radians") {
        const auto actor = !root.empty() && root[0] == '@' ? resolve_actor(root) : root_object(root);
        if (!actor || class_name(actor) != ".?AVActor@chara@sora@@" || pointer(actor) != base() + 0x7eb7d0) { std::snprintf(error, capacity, "coordinate editing requires a current character actor"); return 1; }
        return character_placement::coordinate(reinterpret_cast<void *>(actor), character_placement::checked_target(base() + placement_rva),
            property, args[2], emit, sink, error, capacity);
    }
    if (root == "framework" && property == "run_in_background") {
        if (args[2].type != FDKSS_BOOLEAN) { output("run_in_background requires a boolean", error, capacity); return 1; }
        const auto framework = root_object("framework");
        unsigned char previous = 0;
        if (!framework || !read(framework + 0xc, previous) || previous > 1 ||
            (changed_background && framework != background_framework)) {
            output("framework unavailable or identity changed", error, capacity); return 1;
        }
        const unsigned char enabled = args[2].boolean ? 1 : 0;
        if (!write_byte(framework + 0xc, enabled)) { output("could not change background updates", error, capacity); return 1; }
        if (!changed_background) { original_background = previous; background_framework = framework; changed_background = true; }
        last_background = enabled;
        // Mirrors the game's option handler. Restore current activation on disabling.
        if (!write_byte(framework + 9, enabled ? 1 : foreground_active())) {
            output("could not update framework activation", error, capacity); return 1;
        }
        fdkss_value value{}; value.type = FDKSS_BOOLEAN; value.boolean = enabled;
        return emit(sink, &value);
    }
    if (root != "framework" || property != "time_scale") {
        output("property is read-only or unknown", error, capacity); return 1;
    }
    if (args[2].type != FDKSS_NUMBER && args[2].type != FDKSS_INTEGER) {
        output("time_scale requires a number", error, capacity); return 1;
    }
    const double number = args[2].type == FDKSS_INTEGER ? static_cast<double>(args[2].integer) : args[2].number;
    char input[64]{};
    std::snprintf(input, sizeof(input), "%.17g", number);
    // Reuse the validated write and session restoration policy.
    if (set_time_scale(input, error, capacity)) return 1;
    fdkss_value value{}; value.type = FDKSS_NUMBER; value.number = static_cast<float>(number);
    return emit(sink, &value);
}
int FDKSS_CALL call_method(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if (count < 2 || args[0].type != FDKSS_STRING || args[1].type != FDKSS_STRING) { output("expected root and method strings", error, capacity); return 1; }
    const std::string_view root(args[0].string, args[0].string_size), name(args[1].string, args[1].string_size);
    const auto actor = !root.empty() && root[0] == '@' ? resolve_actor(root) : root_object(root);
    if (name == "get_transform" || name == "type_name" || name == "type_id" || name == "type_flags") {
        DWORD worker = 0;
        const auto framework = root_object("framework");
        if (!framework || !read(framework + 0x68, worker) || worker != GetCurrentThreadId()) { output("actor transform access requires the verified update thread", error, capacity); return 1; }
        if (count != 2 || !actor || !actor_layout(actor)) { output("getter requires a current covered actor and no additional arguments", error, capacity); return 1; }
        if (name != "get_transform") return actor_methods::type(reinterpret_cast<void *>(actor), base(), false, name, emit, sink, error, capacity);
        return actor_methods::transform(reinterpret_cast<void *>(actor), base() + actor_transform_rva, emit, sink, error, capacity);
    }
    if (!actor || class_name(actor) != ".?AVActor@chara@sora@@" || pointer(actor) != base() + 0x7eb7d0) {
        output("method requires a current character actor", error, capacity); return 1;
    }
    if (name == "set_placement")
        return character_placement::invoke(reinterpret_cast<void *>(actor), character_placement::checked_target(base() + placement_rva),
            args + 2, count - 2, emit, sink, error, capacity);
    if(name=="set_native_state_byte") {
        DWORD worker=0;const auto framework=root_object("framework");
        if(!framework || !read(framework+0x68,worker) || worker!=GetCurrentThreadId()){output("native state editing requires the verified update thread",error,capacity);return 1;}
        const auto discard=+[](void *,const fdkss_value *)->int{return 0;};
        if(actor_methods::state_byte(reinterpret_cast<void *>(actor),base(),false,args+2,count-2,discard,nullptr,error,capacity))return 1;
        fdkss_value result{};return emit(sink,&result);
    }
    output("unknown character method", error, capacity); return 1;
}
void **update_slot() {
    const auto app = object(application_rva, ".?AVApplication@sora@@");
    if (!app || pointer(app) != base() + application_vtable_rva) return nullptr;
    const auto slot = base() + application_vtable_rva + 0x38;
    if (pointer(slot) != base() + application_update_rva) return nullptr;
    return reinterpret_cast<void **>(slot);
}
int describe(const char *kind, char *buffer, std::size_t capacity) {
    if (std::strcmp(kind, "content") == 0) {
        const auto state = content_overlay::status();
        char counts[256]{};
        std::snprintf(counts, sizeof(counts), "{\"enabled\":%s,\"files\":%llu,\"opens\":%llu,\"redirects\":%llu,\"dropped_trace\":%llu}",
            state.enabled ? "true" : "false", static_cast<unsigned long long>(state.files),
            static_cast<unsigned long long>(state.opens), static_cast<unsigned long long>(state.redirects),
            static_cast<unsigned long long>(state.dropped));
        return output(counts, buffer, capacity);
    }
    if (std::strcmp(kind, "file_requests") == 0) {
        std::string result = "[";
        for (const auto &request : content_overlay::drain()) {
            if (result.size() > 1) result += ',';
            result += "{\"path\":" + json_string(request.path) + ",\"mode\":" + std::to_string(request.mode) +
                ",\"flags\":" + std::to_string(request.flags) + ",\"redirected\":" +
                (request.redirected ? "true" : "false") + ",\"result\":" + std::to_string(request.result) + "}";
        }
        return output(result + "]", buffer, capacity);
    }
    if (std::strcmp(kind, "scene") == 0) {
        const auto manager = object(fieldmap_rva, ".?AVManager@fieldmap@sora@@");
        const auto scene = manager ? pointer(manager + scene_offset) : 0;
        char map[32]{};
        if (class_name(scene) != ".?AVScene@fieldmap@sora@@" || !read(scene + 0x30, map) ||
            !std::memchr(map, 0, sizeof(map)) || !*map)
            return output("{\"available\":false}", buffer, capacity);
        return output("{\"available\":true,\"map_id\":" + json_string(map) +
            ",\"object\":" + object_json(scene) + "}", buffer, capacity);
    }
    if (std::strcmp(kind, "player") == 0) {
        const auto manager = object(fieldmap_rva, ".?AVManager@fieldmap@sora@@");
        const auto controller = manager ? pointer(manager + player_offset) : 0;
        const auto actor = class_name(controller) == ".?AVPlayer@fieldmap@sora@@" ? pointer(controller + 0x40) : 0;
        struct Position { float x, y, z; } position{};
        if (class_name(actor) != ".?AVActor@chara@sora@@" || !read(actor + 0xd8, position) ||
            !std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
            return output("{\"available\":false}", buffer, capacity);
        char coordinates[256]{};
        std::snprintf(coordinates, sizeof(coordinates), "{\"x\":%.9g,\"y\":%.9g,\"z\":%.9g}",
            static_cast<double>(position.x), static_cast<double>(position.y), static_cast<double>(position.z));
        return output("{\"available\":true,\"actor\":" + object_json(actor) +
            ",\"position\":" + coordinates + "}", buffer, capacity);
    }
    if (std::strcmp(kind, "framework") == 0) {
        const auto address = object(framework_rva, ".?AVFramework@fdk@@");
        float fps = 0, scale = 0, delta = 0;
        DWORD thread = 0;
        if (!address || !read(address + 0x30, fps) || !read(address + 0x3c, scale) ||
            !read(address + 0x38, delta) || !read(address + 0x68, thread) ||
            !std::isfinite(fps) || !std::isfinite(scale) || !std::isfinite(delta))
        {
            output("framework unavailable", buffer, capacity);
            return 1;
        }
        char values[256]{};
        std::snprintf(values, sizeof(values), "{\"fps\":%.9g,\"time_scale\":%.9g,\"delta_seconds\":%.9g,\"update_thread\":%lu}",
            static_cast<double>(fps), static_cast<double>(scale), static_cast<double>(delta), static_cast<unsigned long>(thread));
        return output(values, buffer, capacity);
    }
    if (std::strcmp(kind, "objects") == 0) {
        const auto manager = object(fieldmap_rva, ".?AVManager@fieldmap@sora@@");
        const auto scene = manager ? pointer(manager + scene_offset) : 0;
        const auto player = manager ? pointer(manager + player_offset) : 0;
        return output("{\"application\":" + object_json(object(application_rva, ".?AVApplication@sora@@")) +
            ",\"framework\":" + object_json(object(framework_rva, ".?AVFramework@fdk@@")) +
            ",\"fieldmap\":" + object_json(manager) + ",\"scene\":" + object_json(scene) +
            ",\"player_controller\":" + object_json(player) + "}", buffer, capacity);
    }
    output("unknown inspection kind", buffer, capacity);
    return 1;
}
int set_time_scale(const char *input, char *buffer, std::size_t capacity) {
    float value = 0;
    const char *end = input + std::strlen(input);
    const auto parsed = std::from_chars(input, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(value) || value < 0.25f || value > 2.0f) {
        output("time scale must be a number between 0.25 and 2", buffer, capacity);
        return 1;
    }
    const auto framework = object(framework_rva, ".?AVFramework@fdk@@");
    float previous = 0;
    if (!framework || !read(framework + 0x3c, previous) || !std::isfinite(previous)) {
        output("framework unavailable", buffer, capacity);
        return 1;
    }
    if (changed_scale && framework != changed_framework) {
        output("framework identity changed; restart the session", buffer, capacity);
        return 1;
    }
    if (!write_float(framework + 0x3c, value)) {
        output("could not write time scale", buffer, capacity);
        return 1;
    }
    if (!changed_scale) { changed_scale = true; changed_framework = framework; original_scale = previous; }
    last_scale = value;
    char result[128]{};
    std::snprintf(result, sizeof(result), "{\"previous\":%.9g,\"value\":%.9g}",
        static_cast<double>(previous), static_cast<double>(value));
    return output(result, buffer, capacity);
}
bool restore_changes() {
    if (!content_overlay::stop()) return false;
    if (!actor_lifetimes::stop()) return false;
    if (changed_background) {
        const auto framework = root_object("framework");
        unsigned char current = 0;
        if (!framework || framework != background_framework || !read(framework + 0xc, current)) return false;
        if (current == last_background && (!write_byte(framework + 0xc, original_background) ||
            !write_byte(framework + 9, original_background ? 1 : foreground_active()))) return false;
        changed_background = false; background_framework = 0;
    }
    if (!changed_scale) return true;
    const auto framework = object(framework_rva, ".?AVFramework@fdk@@");
    float current = 0;
    if (!framework || framework != changed_framework || !read(framework + 0x3c, current)) return false;
    // Do not overwrite a value that another part of the game changed after our last write.
    if (std::bit_cast<std::uint32_t>(current) == std::bit_cast<std::uint32_t>(last_scale) &&
        !write_float(framework + 0x3c, original_scale)) return false;
    changed_scale = false;
    changed_framework = 0;
    return true;
}
}
