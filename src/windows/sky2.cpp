#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "sky2.h"
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
#include <array>
#include "lifetimes.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <string>
#include <vector>
#include <unordered_set>
#include <charconv>
#if defined(FDKSS_TEST_SKY2_LAYOUT)
extern std::uintptr_t sky2_test_base();
#endif
namespace {
#define FDKSS_ACTOR_METHOD(name, slot, rva) constexpr std::uintptr_t actor_transform_rva = rva;
#include "fdkss/sky2_actor_methods.inc"
#undef FDKSS_ACTOR_METHOD
#define FDKSS_PLACEMENT(name, rva) constexpr std::uintptr_t placement_rva = rva;
#include "fdkss/sky2_placement.inc"
#undef FDKSS_PLACEMENT
std::uintptr_t base() {
#if defined(FDKSS_TEST_SKY2_LAYOUT)
    return sky2_test_base();
#else
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
#endif
}
template<class T> bool read(std::uintptr_t address, T &value) {
    SIZE_T count = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(address),
        &value, sizeof(value), &count) && count == sizeof(value);
}
std::uintptr_t pointer(std::uintptr_t address) {
    std::uintptr_t value = 0; read(address, value); return value;
}
bool checked_class(std::uintptr_t object, std::uintptr_t table_rva, const char *expected) {
    const auto table = pointer(object);
    if (table != base() + table_rva) return false;
    const auto locator = pointer(table - 8);
    struct Locator { std::uint32_t signature, offset, constructor, type, hierarchy, self; } data{};
    char name[128]{};
    return read(locator, data) && data.signature == 1 && data.offset == 0 &&
        locator >= data.self && locator - data.self == base() &&
        read(base() + data.type + 16, name) && std::memchr(name, 0, sizeof(name)) &&
        std::strcmp(name, expected) == 0;
}
std::uintptr_t root(std::string_view name) {
    if (name == "application") {
        const auto value = pointer(base() + 0xc5d7a0);
        return checked_class(value, 0xae7ab0, ".?AVApplication@sora@@") ? value : 0;
    }
    if (name == "framework") {
        const auto value = pointer(base() + 0xc60ec0);
        return checked_class(value, 0xb1ad08, ".?AVFramework@fdk@@") ? value : 0;
    }
    if (name == "script_manager") {
        const auto value = pointer(base() + 0xc60e58);
        return checked_class(value, 0xb12fd0, ".?AVManager@script@sora@@") ? value : 0;
    }
    const auto manager = pointer(base() + 0xc60e08);
    if (!checked_class(manager, 0xb03a98, ".?AVManager@fieldmap@sora@@")) return 0;
    if (name == "fieldmap") return manager;
    if (name == "scene") {
        const auto value = pointer(manager + 0x108);
        return checked_class(value, 0xb06730, ".?AVScene@fieldmap@sora@@") ? value : 0;
    }
    const auto controller = pointer(manager + 0x660);
    if (!checked_class(controller, 0xb05cb8, ".?AVPlayer@fieldmap@sora@@")) return 0;
    if (name == "player_controller") return controller;
    if (name == "player" || name == "player_model") {
        const auto value = pointer(controller + 0x60);
        if (!checked_class(value, 0xafdec8, ".?AVActor@chara@sora@@")) return 0;
        if (name=="player") return value;
        const auto model=pointer(value+0x2e0);
        std::uint32_t magic=0,version=0;std::uint64_t meshes=0,nodes=0;
        return checked_class(model,0xb18c80,".?AVModel@fdk@@") &&
            read(model+0x10,magic) && magic==0x204c444d && read(model+0x14,version) && (version==4 || version==5) &&
            read(model+0x258,meshes) && meshes<=10000 && read(model+0x288,nodes) && nodes<=10000 ? model : 0;
    }
    return 0;
}
const char *root_type(std::string_view name) {
    if (name == "application") return ".?AVApplication@sora@@";
    if (name == "framework") return ".?AVFramework@fdk@@";
    if (name == "fieldmap") return ".?AVManager@fieldmap@sora@@";
    if (name == "scene") return ".?AVScene@fieldmap@sora@@";
    if (name == "player_controller") return ".?AVPlayer@fieldmap@sora@@";
    if (name == "player") return ".?AVActor@chara@sora@@";
    if (name == "player_model") return ".?AVModel@fdk@@";
    if (name == "script_manager") return ".?AVManager@script@sora@@";
    return nullptr;
}
int fail(const char *message, char *error, std::size_t capacity) {
    std::snprintf(error, capacity, "%s", message); return 1;
}
struct ActorDescriptor { std::uintptr_t table, destructor; const char *name; };
constexpr ActorDescriptor actor_descriptors[]{
#define FDKSS_ACTOR(table, destructor, name) {table, destructor, name},
#include "sky2_lifetimes.inc"
#undef FDKSS_ACTOR
};
const ActorDescriptor *actor_type(std::uintptr_t address) {
    const auto table = pointer(address);
    for (const auto &entry : actor_descriptors) if (table == base() + entry.table) return &entry;
    return nullptr;
}
std::uint64_t scene_epoch = 0;
std::uintptr_t tracked_scene = 0, tracked_player = 0;
std::string tracked_map, previous_map;
bool scene_changed = false, collection_cached = false;
std::vector<std::uintptr_t> cached_actors;
std::unordered_set<std::uintptr_t> cached_members;
bool actor_collection() {
    if (collection_cached) return true;
    cached_actors.clear(); cached_members.clear();
    const auto scene = root("scene");
    if (!scene) { collection_cached = true; return true; }
    std::vector<std::uintptr_t> pending{scene};
    std::unordered_set<std::uintptr_t> scenes;
    struct Vector { std::uintptr_t data, count, capacity; };
    const auto vector = [](std::uintptr_t address, std::vector<std::uintptr_t> &items) {
        Vector value{}; SIZE_T count = 0;
        if (!read(address, value) || value.count > value.capacity || value.capacity > 16384 || (value.count && !value.data)) return false;
        items.resize(static_cast<std::size_t>(value.count));
        return items.empty() || (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(value.data),
            items.data(), items.size() * sizeof(std::uintptr_t), &count) && count == items.size() * sizeof(std::uintptr_t));
    };
    while (!pending.empty()) {
        const auto current = pending.back(); pending.pop_back();
        if (!scenes.insert(current).second || scenes.size() > 256 ||
            !checked_class(current, 0xb06730, ".?AVScene@fieldmap@sora@@")) return false;
        std::vector<std::uintptr_t> actors, children;
        if (!vector(current + 0x158, actors) || !vector(current + 0x170, children)) return false;
        for (const auto actor : actors) {
            // Only expose classes with independently verified destructor coverage.
            if (actor && actor_type(actor) && cached_members.insert(actor).second) cached_actors.push_back(actor);
        }
        if (cached_actors.size() > 16384 || pending.size() + children.size() > 256) return false;
        pending.insert(pending.end(), children.begin(), children.end());
    }
    collection_cached = true; return true;
}
std::uintptr_t resolve_actor(std::string_view token) {
    if (token.empty() || token[0] != '@') return 0;
    std::uint64_t fields[5]{};
    std::size_t begin = 1;
    for (std::size_t index = 0; index < 5; ++index) {
        const auto end = token.find(':', begin);
        if ((index < 4 && end == token.npos) || (index == 4 && end != token.npos)) return 0;
        const auto part = token.substr(begin, end == token.npos ? end : end - begin);
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), fields[index], index < 3 ? 10 : 16);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size()) return 0;
        begin = end == token.npos ? token.size() : end + 1;
    }
    const auto address = static_cast<std::uintptr_t>(fields[3]);
    if (fields[0] != scene_epoch || !address || !actor_lifetimes::valid(address, {fields[1], fields[2]}) ||
        !actor_collection() || !cached_members.contains(address) || pointer(address) != fields[4] || !actor_type(address)) return 0;
    return address;
}
struct Property { const char *root, *name; std::size_t offset; fdkss_value_type type; bool byte_integer,wide_integer,wide_string; };
constexpr Property properties[]{
#define FDKSS_PROPERTY(root, name, offset, type, storage, writable) {#root, #name, offset, type, std::string_view(#storage) == "uint8_t",std::string_view(#storage)=="uint64_t",std::string_view(#storage)=="char64"},
#include "fdkss/sky2_properties.inc"
#undef FDKSS_PROPERTY
};
bool changed = false;
std::uintptr_t changed_framework = 0;
float original_scale = 1, last_scale = 1;
bool write_scale(std::uintptr_t framework, float value) {
    SIZE_T count = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(framework + 0x40),
        &value, sizeof(value), &count) && count == sizeof(value);
}
}
namespace sky2 {
int describe(const char *kind, char *buffer, std::size_t capacity) {
    if (!buffer || !capacity) return 1;
    if (!owner_thread()) return fail("snapshots require the owning update thread", buffer, capacity);
    const auto output = [&](const std::string &value) {
        if (value.size() >= capacity) return fail("snapshot buffer too small", buffer, capacity);
        std::memcpy(buffer, value.c_str(), value.size() + 1); return 0;
    };
    const auto quoted = [](std::string_view text) {
        std::string value = "\"";
        for (const unsigned char byte : text) {
            if (byte == '"' || byte == '\\') { value += '\\'; value += static_cast<char>(byte); }
            else if (byte < 0x20 || byte >= 0x7f) {
                char escaped[7]{}; std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(byte));
                value += escaped;
            } else value += static_cast<char>(byte);
        }
        return value + '"';
    };
    const auto identity = [&](std::uintptr_t address, const char *type) {
        if (!address) return std::string("null");
        char text[32]{}; std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(address));
        return "{\"address\":" + quoted(text) + ",\"class\":" + quoted(type) + "}";
    };
    if (!kind) return fail("unknown snapshot", buffer, capacity);
    const std::string_view name(kind);
    if (name == "scene") {
        const auto scene = root("scene");
        char map[32]{};
        if (!scene || !read(scene + 0x30, map) || !std::memchr(map, 0, sizeof(map)) || !*map)
            return output("{\"available\":false}");
        if (root("scene") != scene) return fail("scene changed during snapshot", buffer, capacity);
        return output("{\"available\":true,\"map_id\":" + quoted(map) +
            ",\"object\":" + identity(scene, root_type("scene")) + "}");
    }
    if (name == "player") {
        const auto player = root("player");
        std::array<float, 3> position{};
        if (!player || !read(player + 0xd8, position) ||
            !std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2]))
            return output("{\"available\":false}");
        if (root("player") != player) return fail("player changed during snapshot", buffer, capacity);
        char coordinates[256]{};
        std::snprintf(coordinates, sizeof(coordinates), "{\"x\":%.9g,\"y\":%.9g,\"z\":%.9g}",
            static_cast<double>(position[0]), static_cast<double>(position[1]), static_cast<double>(position[2]));
        return output("{\"available\":true,\"actor\":" + identity(player, root_type("player")) +
            ",\"position\":" + coordinates + "}");
    }
    if (name == "framework") {
        const auto framework = root("framework");
        float fps = 0, scale = 0, delta = 0; DWORD thread = 0;
        if (!framework || !read(framework + 0x30, fps) || !read(framework + 0x40, scale) ||
            !read(framework + 0x38, delta) || !read(framework + 0xa0, thread) ||
            !std::isfinite(fps) || !std::isfinite(scale) || !std::isfinite(delta))
            return fail("framework unavailable", buffer, capacity);
        if (root("framework") != framework) return fail("framework changed during snapshot", buffer, capacity);
        char values[256]{};
        std::snprintf(values, sizeof(values), "{\"fps\":%.9g,\"time_scale\":%.9g,\"delta_seconds\":%.9g,\"update_thread\":%lu}",
            static_cast<double>(fps), static_cast<double>(scale), static_cast<double>(delta), static_cast<unsigned long>(thread));
        return output(values);
    }
    if (name == "objects") {
        constexpr const char *names[]{"application", "framework", "fieldmap", "scene", "player_controller"};
        std::array<std::uintptr_t, 5> addresses{};
        std::string value = "{";
        for (std::size_t i = 0; i < addresses.size(); ++i) {
            addresses[i] = root(names[i]);
            if (i) value += ',';
            value += quoted(names[i]) + ':' + identity(addresses[i], root_type(names[i]));
        }
        for (std::size_t i = 0; i < addresses.size(); ++i)
            if (root(names[i]) != addresses[i]) return fail("objects changed during snapshot", buffer, capacity);
        return output(value + '}');
    }
    return fail("unknown snapshot", buffer, capacity);
}
int FDKSS_CALL class_info(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if (!owner_thread() || count!=2 || args[0].type!=FDKSS_STRING)
        return fail("class metadata requires the owning update thread and root/index arguments",error,capacity);
    const std::string_view name(args[0].string,args[0].string_size);
    command_objects::Object group;
    const auto address=command_objects::named(name) ? (command_objects::resolve(base(),true,name,group) ? group.address : 0) :
        !name.empty() && name[0]=='@' ? resolve_actor(name) : root(name);
    return live_class_metadata::inspect(base(),address,args,count,emit,sink,error,capacity);
}
int FDKSS_CALL read_resource(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    return resource_stream::read(base(), resource_stream::sky2, args, count, emit, sink, error, capacity);
}
int FDKSS_CALL content_self_test(void *, const fdkss_value *, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if (count) return fail("content self test takes no arguments", error, capacity);
    fdkss_value args[3]{}; args[0].type = FDKSS_STRING; args[0].string = "fdkss/probe.txt"; args[0].string_size = 15;
    args[1].type = args[2].type = FDKSS_INTEGER; args[2].integer = 128;
    struct Output { fdkss_result_fn emit; void *sink; } output{emit, sink};
    const auto first = [](void *context, const fdkss_value *value) -> int {
        const auto &output = *static_cast<Output *>(context);
        return value->type == FDKSS_STRING ? output.emit(output.sink, value) : 0;
    };
    return read_resource(nullptr, args, 3, first, &output, error, capacity);
}
int FDKSS_CALL call_method(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if (!owner_thread()) return fail("Sky 2 engine access requires the verified update thread", error, capacity);
    if (count < 2 || args[0].type != FDKSS_STRING || args[1].type != FDKSS_STRING)
        return fail("expected root and method strings", error, capacity);
    const std::string_view root(args[0].string, args[0].string_size), name(args[1].string, args[1].string_size);
    const auto actor = !root.empty() && root[0] == '@' ? resolve_actor(root) : ::root(root);
    if (name == "get_transform" || name == "type_name" || name == "type_id" || name == "type_flags") {
        if (count != 2 || !actor || !actor_type(actor)) return fail("getter requires a current covered actor and no additional arguments", error, capacity);
        if (name != "get_transform") return actor_methods::type(reinterpret_cast<void *>(actor), base(), true, name, emit, sink, error, capacity);
        return actor_methods::transform(reinterpret_cast<void *>(actor), base() + actor_transform_rva, emit, sink, error, capacity);
    }
    if (!actor || !checked_class(actor, 0xafdec8, ".?AVActor@chara@sora@@")) {
        return fail("method requires a current character actor", error, capacity);
    }
    if (name == "set_placement")
        return character_placement::invoke(reinterpret_cast<void *>(actor), character_placement::checked_target(base() + placement_rva),
            args + 2, count - 2, emit, sink, error, capacity);
    if(name=="set_native_state_byte") {
        const auto discard=+[](void *,const fdkss_value *)->int{return 0;};
        if(actor_methods::state_byte(reinterpret_cast<void *>(actor),base(),true,args+2,count-2,discard,nullptr,error,capacity))return 1;
        fdkss_value result{};return emit(sink,&result);
    }
    return fail("unknown character method", error, capacity);
}
void **update_slot() {
    const auto application = root("application"), framework = root("framework");
    if (!application || !framework) return nullptr;
    const auto slot = base() + 0xae7ab0 + 0x38;
    DWORD worker = 0;
    if (pointer(slot) != base() + 0x311f0 || !read(framework + 0xa0, worker) || !worker) return nullptr;
    return reinterpret_cast<void **>(slot);
}
bool owner_thread() {
    const auto framework = root("framework"); DWORD worker = 0;
    return framework && read(framework + 0xa0, worker) && worker == GetCurrentThreadId();
}
int model_snapshot(const fdkss_value *args,std::size_t count,
    fdkss_result_fn emit,void *sink,char *error,std::size_t capacity,bool objects) {
    if(!owner_thread())return fail("character model snapshots require the owning update thread",error,capacity);
    return character_models::snapshot(base(),character_models::second,
        [](std::string_view reference){return reference=="player" ? root("player") : resolve_actor(reference);},
        ::player_resources::read,[](std::uintptr_t model){return checked_class(model,0xb18c80,".?AVModel@fdk@@");},
        args,count,emit,sink,error,capacity,objects ? &character_models::object_second : nullptr);
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
    if(!owner_thread())return fail("native status reads require the owning update thread",error,capacity);
    return status_getter::invoke(base(),::player_resources::sky2,
        [](std::string_view reference){return reference=="player" ? root("player") : resolve_actor(reference);},
        args,count,emit,sink,error,capacity);
}
int FDKSS_CALL character_resources(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if(!owner_thread())return fail("character snapshots require the owning update thread",error,capacity);
    return character_snapshots::snapshot(base(),::player_resources::sky2,::player_conditions::sky2,false,
        [](std::string_view reference){return reference=="player" ? root("player") : resolve_actor(reference);},
        args,count,emit,sink,error,capacity);
}
int FDKSS_CALL character_conditions(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if(!owner_thread())return fail("character snapshots require the owning update thread",error,capacity);
    return character_snapshots::snapshot(base(),::player_resources::sky2,::player_conditions::sky2,true,
        [](std::string_view reference){return reference=="player" ? root("player") : resolve_actor(reference);},
        args,count,emit,sink,error,capacity);
}
int FDKSS_CALL player_conditions(void *, const fdkss_value *, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if(!owner_thread())return fail("player conditions require the owning update thread",error,capacity);
    return ::player_conditions::snapshot(base(),::player_resources::sky2,::player_conditions::sky2,
        []{return root("player");},count,emit,sink,error,capacity);
}
int FDKSS_CALL set_player_resource(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if(!owner_thread())return fail("player resource editing requires the owning update thread",error,capacity);
    return ::player_resources::edit(base(),::player_resources::sky2,::player_resources::sky2_setter,
        []{return root("player");},args,count,emit,sink,error,capacity,resource_hooks::checked_target);
}
int FDKSS_CALL battle_roster(void *,const fdkss_value *,std::size_t count,
    fdkss_result_fn emit,void *sink,char *error,std::size_t capacity) {
    return battle_snapshots::snapshot(base(),true,owner_thread,count,emit,sink,error,capacity);
}
int FDKSS_CALL player_resources(void *, const fdkss_value *, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if(!owner_thread())return fail("player resources require the owning update thread",error,capacity);
    return ::player_resources::snapshot(base(),::player_resources::sky2,
        []{return root("player");},count,emit,sink,error,capacity);
}
int FDKSS_CALL get_property(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if (!owner_thread()) return fail("Sky 2 engine access requires the verified update thread", error, capacity);
    if (count != 2 || args[0].type != FDKSS_STRING || args[1].type != FDKSS_STRING)
        return fail("expected root and property strings", error, capacity);
    std::string_view name(args[0].string, args[0].string_size);
    if(command_objects::named(name))return command_objects::get(base(),true,args,count,emit,sink,error,capacity);
    const std::string_view field(args[1].string, args[1].string_size);
    if(const auto resource=resource_properties::index(field);resource>=0)
        return resource_properties::get(character_resources,args,static_cast<unsigned>(resource),emit,sink,error,capacity);
    const bool handle = !name.empty() && name[0] == '@';
    const auto object = handle ? resolve_actor(name) : root(name);
    if (handle && !object) return fail("scene actor reference expired", error, capacity);
    if (handle) name = checked_class(object, 0xafdec8, ".?AVActor@chara@sora@@") ? "player" : "actor";
    if (!handle && !root_type(name)) return fail("unknown Sky 2 engine root", error, capacity);
    const Property *property = nullptr;
    for (const auto &entry : properties) if ((name == entry.root || (std::string_view(entry.root) == "actor" && name == "player")) && field == entry.name) property = &entry;
    if (!property && field != "class" && field != "address") return fail("unknown property for this Sky 2 build", error, capacity);
    fdkss_value value{};
    if (!object) return emit(sink, &value);
    if (field == "yaw_radians")
        return character_placement::yaw(reinterpret_cast<void *>(object), emit, sink, error, capacity);
    char text[32]{};
    if (field == "address") {
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(object));
        value.type = FDKSS_STRING; value.string = text; value.string_size = std::strlen(text);
    } else if (field == "class") {
        value.type = FDKSS_STRING;
        const auto *current = handle ? actor_type(object) : nullptr;
        if (handle && !current) return fail("scene actor reference expired", error, capacity);
        value.string = handle ? current->name : root_type(name);
        value.string_size = std::strlen(value.string);
    } else if (property->type == FDKSS_STRING) {
        if (property->wide_string) {
            char wide_text[64]{};
            if (!read(object+property->offset,wide_text) || !std::memchr(wide_text,0,sizeof(wide_text)))
                return fail("property could not be read",error,capacity);
            value.type=FDKSS_STRING;value.string=wide_text;value.string_size=std::strlen(wide_text);return emit(sink,&value);
        }
        if (!read(object + property->offset, text) || !std::memchr(text, 0, sizeof(text)))
            return fail("property could not be read", error, capacity);
        value.type = FDKSS_STRING; value.string = text; value.string_size = std::strlen(text);
    } else if (property->type == FDKSS_BOOLEAN) {
        unsigned char boolean = 0;
        if (!read(object + property->offset, boolean) || boolean > 1) return fail("property could not be read", error, capacity);
        value.type = FDKSS_BOOLEAN; value.boolean = boolean;
    } else if (property->type == FDKSS_INTEGER) {
        value.type = FDKSS_INTEGER;
        if (property->byte_integer) {
            unsigned char integer = 0;
            if (!read(object + property->offset, integer)) return fail("property could not be read", error, capacity);
            value.integer = integer;
        } else if (property->wide_integer) {
            std::uint64_t integer = 0;
            if (!read(object + property->offset, integer) || integer > INT64_MAX) return fail("property could not be read", error, capacity);
            value.integer = static_cast<std::int64_t>(integer);
        } else {
            DWORD integer = 0;
            if (!read(object + property->offset, integer)) return fail("property could not be read", error, capacity);
            value.integer = integer;
        }
    } else {
        float number = 0;
        if (!read(object + property->offset, number) || !std::isfinite(number)) return fail("property could not be read", error, capacity);
        value.type = FDKSS_NUMBER; value.number = number;
    }
    return emit(sink, &value);
}
int FDKSS_CALL set_property(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if (!owner_thread()) return fail("Sky 2 engine access requires the verified update thread", error, capacity);
    if (count != 3 || args[0].type != FDKSS_STRING || args[1].type != FDKSS_STRING)
        return fail("expected root, property and value", error, capacity);
    const std::string_view root_name(args[0].string, args[0].string_size), property(args[1].string, args[1].string_size);
    if(resource_properties::index(property)>=0) {

        return resource_properties::set(base(),::player_resources::sky2,::player_resources::sky2_setter,
            [](std::string_view reference){return reference=="player" ? root("player") : resolve_actor(reference);},
            args,emit,sink,error,capacity,resource_hooks::checked_target);
    }
    if(property=="native_state_byte") {
        const auto actor=!root_name.empty() && root_name[0]=='@' ? resolve_actor(root_name) : root(root_name);
        if(!actor || !checked_class(actor,0xafdec8,".?AVActor@chara@sora@@"))return fail("native state editing requires a current character actor",error,capacity);
        return actor_methods::state_byte(reinterpret_cast<void *>(actor),base(),true,args+2,1,emit,sink,error,capacity);
    }
    if (property == "position_x" || property == "position_y" || property == "position_z" || property == "yaw_radians") {
        const auto actor = !root_name.empty() && root_name[0] == '@' ? resolve_actor(root_name) : root(root_name);
        if (!actor || !checked_class(actor, 0xafdec8, ".?AVActor@chara@sora@@")) { std::snprintf(error, capacity, "coordinate editing requires a current character actor"); return 1; }
        return character_placement::coordinate(reinterpret_cast<void *>(actor), character_placement::checked_target(base() + placement_rva),
            property, args[2], emit, sink, error, capacity);
    }
    if (count != 3 || args[0].type != FDKSS_STRING || args[1].type != FDKSS_STRING ||
        std::string_view(args[0].string, args[0].string_size) != "framework" ||
        std::string_view(args[1].string, args[1].string_size) != "time_scale")
        return fail("writable fields are framework.time_scale and character position_x/y/z/yaw_radians", error, capacity);
    if (args[2].type != FDKSS_INTEGER && args[2].type != FDKSS_NUMBER)
        return fail("time_scale requires a number", error, capacity);
    const double number = args[2].type == FDKSS_INTEGER ? static_cast<double>(args[2].integer) : args[2].number;
    if (!std::isfinite(number) || number < 0.25 || number > 2)
        return fail("time_scale must be finite and between 0.25 and 2", error, capacity);
    const auto framework = root("framework"); float previous = 0;
    if (!framework || (changed && framework != changed_framework) ||
        !read(framework + 0x40, previous) || !std::isfinite(previous))
        return fail("framework unavailable or identity changed", error, capacity);
    const auto replacement = static_cast<float>(number);
    if (!write_scale(framework, replacement)) return fail("could not write time_scale", error, capacity);
    if (!changed) { changed = true; changed_framework = framework; original_scale = previous; }
    last_scale = replacement;
    fdkss_value value{}; value.type = FDKSS_NUMBER; value.number = replacement; return emit(sink, &value);
}
bool restore_changes() {
    if (!actor_lifetimes::stop()) return false;
    if (!changed) return true;
    const auto framework = root("framework"); float current = 0;
    // Never overwrite a new framework or a later change made by another mod.
    if (framework == changed_framework) {
        if (!read(framework + 0x40, current)) return false;
        if (current == last_scale && !write_scale(framework, original_scale)) return false;
    }
    changed = false; return true;
}
int FDKSS_CALL list_objects(void *, const fdkss_value *args, std::size_t count,
    fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if (!owner_thread()) return fail("Sky 2 engine access requires the verified update thread", error, capacity);
    if (count != 3 || args[0].type != FDKSS_INTEGER || args[0].integer < 0 || args[0].integer > 16384 ||
        args[1].type != FDKSS_INTEGER || args[1].integer < 1 || args[1].integer > 63 || args[2].type != FDKSS_STRING)
        return fail("expected offset, limit and class filter string", error, capacity);
    if (!actor_collection()) return fail("scene hierarchy unavailable, cyclic or out of bounds", error, capacity);
    const std::string_view filter(args[2].string, args[2].string_size);
    std::vector<std::string> tokens;
    auto index = static_cast<std::size_t>(args[0].integer);
    for (; index < cached_actors.size() && tokens.size() < static_cast<std::size_t>(args[1].integer); ++index) {
        const auto address = cached_actors[index];
        const auto *type = actor_type(address);
        if (!type || std::string_view(type->name).find(filter) == std::string_view::npos) continue;
        const auto stamp = actor_lifetimes::observe(address);
        char token[144]{};
        std::snprintf(token, sizeof(token), "@%llu:%llu:%llu:%llx:%llx",
            static_cast<unsigned long long>(scene_epoch), static_cast<unsigned long long>(stamp.epoch),
            static_cast<unsigned long long>(stamp.generation), static_cast<unsigned long long>(address),
            static_cast<unsigned long long>(pointer(address)));
        tokens.emplace_back(token);
    }
    fdkss_value value{}; value.type = FDKSS_INTEGER;
    value.integer = index < cached_actors.size() ? static_cast<std::int64_t>(index) : -1;
    if (emit(sink, &value)) return 1;
    for (const auto &token : tokens) {
        value.type = FDKSS_STRING; value.string = token.data(); value.string_size = token.size();
        if (emit(sink, &value)) return 1;
    }
    return 0;
}
bool start_lifetimes() {
    std::vector<actor_lifetimes::Hook> hooks;
    for (const auto &entry : actor_descriptors)
        hooks.push_back({reinterpret_cast<void **>(base() + entry.table),
            reinterpret_cast<actor_lifetimes::Destructor>(base() + entry.destructor), entry.name});
    return actor_lifetimes::install(hooks);
}
void refresh_scene() {
    collection_cached = false;
    const auto scene = root("scene"), player = root("player");
    char map[32]{};
    if (!scene || !read(scene + 0x30, map) || !std::memchr(map, 0, sizeof(map))) map[0] = 0;
    if (scene != tracked_scene || player != tracked_player || tracked_map != map) {
        if (!scene_changed) previous_map = tracked_map;
        scene_changed = true; ++scene_epoch;
        tracked_scene = scene; tracked_player = player; tracked_map = map;
    }
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

}
