#include "actor_construction.h"
#include "framework_bridge.h"
#include "framework_profiles.h"
#include "actor_hooks.h"
#include "placement_hooks.h"
#include "resource_hooks.h"
#include "native_math.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include "fdkss/api.h"
#include "sky1.h"
#include "sky2.h"
#include "console.h"
#include "content.h"
#include "program_lifetimes.h"
#include "commands.h"
#include "scripts.h"
#include "vm_hooks.h"
#include "input.h"
#include "overlay.h"
#include <atomic>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#if defined(FDKSS_TEST_PROFILE)
#include "test_profile.h"
#endif

namespace {
enum class Driver { steam_callbacks, application_update, framework_frame };
enum class Extension { none, sky_first, sky_second };
using FrameContract = bool (*)(framework_bridge::Profile &);
bool frame_contract(framework_bridge::Profile &contract, std::uint32_t image_size,
                    std::uint32_t frame, std::uint32_t root, std::uint32_t table,
                    std::uint32_t thread, std::span<const unsigned char> body) {
    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!image) return false;
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000) return false;
    const auto *pe = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(image + dos->e_lfanew);
    if (pe->Signature != IMAGE_NT_SIGNATURE || pe->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        pe->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        pe->OptionalHeader.SizeOfImage != image_size) return false;
    // Independently verified Framework root, primary table, main-thread ID and
    // float passed to Application phases. Actor worker identity is a different field.
    contract = {image, image_size, frame, root, table, thread, 0x3c, body};
    return true;
}
bool pn_frame_contract(framework_bridge::Profile &contract) {
    return frame_contract(contract, framework_profiles::pn_image_size,
                          0x761520, 0xe26950, 0xb70230, 0xe260f8, framework_profiles::pn_frame_body);
}
bool ysx_frame_contract(framework_bridge::Profile &contract) {
    return frame_contract(contract, framework_profiles::ysx_image_size,
                          0x6a8cf0, 0xd0bb90, 0xa86988, 0xd0b658, framework_profiles::ysx_frame_body);
}
#if defined(FDKSS_TEST_FRAME_PROFILE)
bool fixture_frame_contract(framework_bridge::Profile &contract) {
    using Provider = const framework_bridge::Profile *(__cdecl *)();
    const auto provider = reinterpret_cast<Provider>(GetProcAddress(GetModuleHandleW(nullptr), "TestFrameworkProfile"));
    const auto value = provider ? provider() : nullptr;
    if (!value) return false;
    contract = *value;
    return true;
}
#endif
struct Profile {
    const wchar_t *file;
    const char *name;
    const char *sha256;
    Driver driver = Driver::steam_callbacks;
    Extension extension = Extension::none;
    FrameContract frame_contract = nullptr;
    bool sky_extensions() const { return extension != Extension::none; }
    bool second_sky() const { return extension == Extension::sky_second; }
    const char *source() const {
        switch (driver) {
        case Driver::application_update: return "fdk.application.update";
        case Driver::framework_frame: return "fdk.framework.frame";
        default: return "steam.callbacks";
        }
    }
};
constexpr Profile profiles[] = {
#if defined(FDKSS_TEST_PROFILE)
#if defined(FDKSS_TEST_FRAME_PROFILE)
    {L"fdkss_framework_platform_fixture.exe", "FDKss framework platform test host", FDKSS_TEST_SHA256,
     Driver::framework_frame, Extension::none, fixture_frame_contract
    },
#else
    {L"fdkss_adapter_fixture.exe", "FDKss adapter test host", FDKSS_TEST_SHA256,
#if defined(FDKSS_TEST_ENGINE_PROFILE)
     Driver::application_update, Extension::sky_first
#else
     Driver::steam_callbacks, Extension::none
#endif
    },
#endif
#endif
    {L"sora_1st.exe", "Trails in the Sky 1st Chapter",
     "4f84cb3be200cfd0485d9d6b2fec0ba70a63d73d3fb8a29a515e3a7abdbf13cf", Driver::application_update, Extension::sky_first},
    {L"sora_2nd.exe", "Trails in the Sky 2nd Chapter",
     "d8b2911d1576216bdc22d070550e4f531e105de7ed2981885849669f4acf8aaf", Driver::application_update, Extension::sky_second},
    {L"ysx_pn.exe", "Ys X - Proud Nordics",
     "34badc3071d5c571524a2234ad64a4c4251214de578eaa18c04b96432501765e",
     Driver::framework_frame, Extension::none, pn_frame_contract},
    {L"ysx.exe", "Ys X - Nordics",
     "224df0db3e0867cc83d4f71b2e3e78f71f47c430899c76b9beb12da5403ae246",
     Driver::framework_frame, Extension::none, ysx_frame_contract}
};
using Pump = void (__cdecl *)(void);
using Update = std::uintptr_t (__fastcall *)(void *, float);
std::atomic<Pump> original{nullptr};
std::atomic<Update> original_update{nullptr};
std::atomic<DWORD> owner{0};
std::atomic<bool> installed{false};
std::atomic<unsigned> commands{0};
constexpr unsigned command_stop = 1, command_reload = 2, command_detach = 4;
void **hook_slot = nullptr;
std::mutex attachment_mutex;
std::ofstream logfile;
std::filesystem::path mods_directory;
const Profile *profile = nullptr;
fdkss_runtime *runtime = nullptr;
std::vector<std::string> loaded_mod_names;
bool attempted = false;
bool vm_installed = false;
sky1_scripts::CatalogProfile sky2_catalog{0xc60e58, 0xb12fd0, 0xc60ec0, 0xb1ad08, 0xa0};
bool reload_down = false;
bool stop_down = false;
auto last_tick = std::chrono::steady_clock::now();
std::uint64_t update_count = 0;
double current_delta = 0;
thread_local bool in_callback = false;

void FDKSS_CALL logger(void *, const char *mod, const char *message) {
    logfile << '[' << mod << "] " << message << '\n';
    logfile.flush();
    developer_overlay::log(mod, message);
}
int FDKSS_CALL game_name(void *, const char *, char *out, size_t capacity) {
    const int count = std::snprintf(out, capacity, "%s", profile->name);
    return count < 0 || static_cast<size_t>(count) >= capacity;
}
int FDKSS_CALL build_hash(void *, const char *, char *out, size_t capacity) {
    const int count = std::snprintf(out, capacity, "%s", profile->sha256);
    return count < 0 || static_cast<size_t>(count) >= capacity;
}
int FDKSS_CALL update_info(void *, const char *, char *out, size_t capacity) {
    const int count = std::snprintf(out, capacity,
        "{\"source\":\"%s\",\"thread\":%lu,\"count\":%llu,\"delta_seconds\":%.9g}",
        profile->source(),
        static_cast<unsigned long>(owner.load()), static_cast<unsigned long long>(update_count), current_delta);
    return count < 0 || static_cast<size_t>(count) >= capacity;
}
int FDKSS_CALL capability_info(void *, const char *, char *out, size_t capacity) {
    // Registration scope; each operation retains its own live availability guards.
    const int count = std::snprintf(out, capacity,
        "{\"update_source\":\"%s\",\"engine_properties\":%s,\"actor_api\":%s,\"vm_api\":%s}",
        profile->source(), profile->sky_extensions() ? "true" : "false",
        profile->sky_extensions() ? "true" : "false", profile->sky_extensions() ? "true" : "false");
    return count < 0 || static_cast<size_t>(count) >= capacity;
}
int FDKSS_CALL inspect_framework(void *, const char *, char *out, size_t capacity) {
    return profile->second_sky() ? sky2::describe("framework", out, capacity) : sky1::describe("framework", out, capacity);
}
int FDKSS_CALL inspect_objects(void *, const char *, char *out, size_t capacity) {
    return profile->second_sky() ? sky2::describe("objects", out, capacity) : sky1::describe("objects", out, capacity);
}
int FDKSS_CALL inspect_scene(void *, const char *, char *out, size_t capacity) {
    return profile->second_sky() ? sky2::describe("scene", out, capacity) : sky1::describe("scene", out, capacity);
}
int FDKSS_CALL inspect_player(void *, const char *, char *out, size_t capacity) {
    return profile->second_sky() ? sky2::describe("player", out, capacity) : sky1::describe("player", out, capacity);
}
int FDKSS_CALL inspect_content(void *, const char *, char *out, size_t capacity) {
    const auto value = content_overlay::describe(false);
    if (value.size() >= capacity) return 1;
    std::memcpy(out, value.c_str(), value.size() + 1); return 0;
}
int FDKSS_CALL inspect_file_requests(void *, const char *, char *out, size_t capacity) {
    const auto value = content_overlay::describe(true);
    if (value.size() >= capacity) return 1;
    std::memcpy(out, value.c_str(), value.size() + 1); return 0;
}
int FDKSS_CALL change_time_scale(void *, const char *input, char *out, size_t capacity) {
    return sky1::set_time_scale(input, out, capacity);
}
int FDKSS_CALL mod_names(void *, const char *, char *out, size_t capacity) {
    std::string names = "[";
    for (const auto &name : loaded_mod_names) { if (names.size() > 1) names += ','; names += '"' + name + '"'; }
    names += ']';
    const auto count = std::snprintf(out, capacity, "%s", names.c_str());
    return count < 0 || static_cast<size_t>(count) >= capacity;
}
int FDKSS_CALL mod_directory(void *, const char *, char *out, size_t capacity) {
    const auto path = mods_directory.generic_u8string();
    const auto count = std::snprintf(out, capacity, "%s", reinterpret_cast<const char *>(path.c_str()));
    return count < 0 || static_cast<size_t>(count) >= capacity;
}
int FDKSS_CALL request_reload(void *, const char *, char *out, size_t capacity) {
    commands.fetch_or(command_reload);
    const auto count = std::snprintf(out, capacity, "queued");
    return count < 0 || static_cast<size_t>(count) >= capacity;
}
void publish_mod_names() {
    loaded_mod_names.clear();
    size_t count = 0;
    if (!runtime || fdkss_mod_count(runtime, &count) != FDKSS_OK) return;
    for (size_t i = 0; i < count; ++i) {
        char name[256]{};
        if (fdkss_mod_name(runtime, i, name, sizeof(name)) == FDKSS_OK) loaded_mod_names.emplace_back(name);
    }
}
int FDKSS_CALL hook_supported(void *, const fdkss_value *args, size_t count,
    fdkss_result_fn emit, void *sink, char *error, size_t capacity) {
    if (count != 1 || args[0].type != FDKSS_STRING) {
        std::snprintf(error, capacity, "expected engine function name string"); return 1;
    }
    fdkss_value value{}; value.type = FDKSS_BOOLEAN;
    const std::string_view name(args[0].string, args[0].string_size);
    value.boolean = profile->sky_extensions() && (name == "application.update" ||
        ((name == "script.command" || name == "script.command.argument") && vm_installed) ||
        ((name == "actor.type_name" || name == "actor.type_id" || name == "actor.type_flags" || name == "actor.get_transform") && actor_hooks::active()) ||
        (name=="actor.set_placement" && placement_hooks::active()) ||
        (name=="status.resource_change" && resource_hooks::active()));
    return emit(sink, &value);
}
void update_event(const char *phase, double &delta, std::uintptr_t &result) {
    if (!runtime) return;
    fdkss_value values[3]{};
    values[0].type = FDKSS_STRING; values[0].string = "application"; values[0].string_size = 11;
    values[1].type = FDKSS_NUMBER; values[1].number = delta;
    values[2].type = FDKSS_INTEGER; values[2].integer = static_cast<std::int64_t>(result);
    const bool pre = std::strcmp(phase, "hook.application.update.pre") == 0;
    fdkss_filter_values(runtime, phase, values, pre ? 2 : 3, pre ? 1 : 2, 0, pre ? 1 : 0);
    if (pre) delta = values[1].number;
    else result = static_cast<std::uintptr_t>(values[2].integer);
}
void reload_mods() {
    if (!runtime) return;
    std::unordered_set<std::string> enabled_mods;
    for (const auto &entry : std::filesystem::directory_iterator(mods_directory)) {
        if (entry.is_directory() && std::filesystem::is_regular_file(entry.path() / "enabled.txt")) {
            const auto name = entry.path().filename().string();
            enabled_mods.insert(name);
            if (fdkss_reload(runtime, name.c_str()) != FDKSS_OK)
                logger(nullptr, name.c_str(), fdkss_last_error(runtime));
        }
    }
    std::vector<std::string> disabled_mods;
    std::size_t count = 0;
    if (fdkss_mod_count(runtime, &count) == FDKSS_OK) {
        for (std::size_t index = 0; index < count; ++index) {
            char name[256]{};
            if (fdkss_mod_name(runtime, index, name, sizeof(name)) == FDKSS_OK && !enabled_mods.contains(name))
                disabled_mods.emplace_back(name);
        }
    }
    for (const auto &name : disabled_mods) {
        if (fdkss_unload(runtime, name.c_str()) != FDKSS_OK) logger(nullptr, name.c_str(), fdkss_last_error(runtime));
        else logger(nullptr, name.c_str(), "disabled and unloaded");
    }
    publish_mod_names();
    if (profile->sky_extensions() && !content_overlay::refresh(mods_directory))
        logger(nullptr, "FDKss", content_overlay::error().c_str());
}
bool stop_scripts() {
    if (profile->sky_extensions()) sky1_scripts::stop_jobs();
    if (profile->sky_extensions() && !program_lifetimes::stop()) {
        logger(nullptr, "FDKss", "Script descriptor hook still active; retry stop/detach"); return false;
    }
    if (!developer_overlay::stop()) {
        logger(nullptr, "FDKss", "Could not restore renderer hook; retry stop/detach"); return false;
    }
    if (profile->sky_extensions() && !content_overlay::stop()) {
        logger(nullptr, "FDKss", "Could not restore content hook; retry stop/detach"); return false;
    }
    if (profile->sky_extensions() && !vm_hooks::stop()) {
        logger(nullptr, "FDKss", "Could not restore VM command slots; retry stop/detach"); return false;
    }
    vm_installed = false;
    if (profile->sky_extensions() && !actor_construction::stop()) {
        logger(nullptr, "FDKss", "Could not restore Actor constructor/base destructor; retry stop/detach"); return false;
    }
    if (profile->sky_extensions() && !actor_hooks::stop()) {
        logger(nullptr, "FDKss", "Could not restore actor getter slots; retry stop/detach"); return false;
    }
    if(profile->sky_extensions() && !resource_hooks::stop()) {logger(nullptr,"FDKss","Could not restore native resource hook; retry stop/detach");return false;}
    if(profile->sky_extensions() && !placement_hooks::stop()) {logger(nullptr,"FDKss","Could not restore character placement hook; retry stop/detach");return false;}
    if (runtime) {
        if (fdkss_destroy(runtime) != FDKSS_OK) return false;
        runtime = nullptr;
        loaded_mod_names.clear();
        logger(nullptr, "FDKss", "Lua stopped for this session");
    }
    if (profile->sky_extensions() && !(profile->second_sky() ? sky2::restore_changes() : sky1::restore_changes())) {
        logger(nullptr, "FDKss", "Could not restore engine changes; retry stop/detach before ending the session");
        return false;
    }
    return true;
}
void restore_hook();
std::string utf8(const std::filesystem::path &path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}
std::uintptr_t run_scripts(double delta, bool wall_clock = false, std::uintptr_t result = 0) {
    if (!installed.load(std::memory_order_acquire)) return result;
    DWORD expected = 0;
    const DWORD thread = GetCurrentThreadId();
    owner.compare_exchange_strong(expected, thread);
    if (owner.load() != thread) return result;
    if (profile->second_sky() && !sky2::owner_thread()) return result;
    if (in_callback) return result;
    in_callback = true;
    try {
        if (wall_clock) {
            const auto now = std::chrono::steady_clock::now();
            delta = attempted ? std::chrono::duration<double>(now - last_tick).count() : 0.0;
            last_tick = now;
        }
        if (!std::isfinite(delta) || delta < 0) {
            in_callback = false;
            return result;
        }
        ++update_count;
        current_delta = delta;
        if (profile->sky_extensions()) { if (profile->second_sky()) sky2::refresh_scene(); else sky1::refresh_scene(); }
        if (!attempted) {
            attempted = true;
            if (profile->sky_extensions() && !(profile->second_sky() ? sky2::start_lifetimes() : sky1::start_lifetimes()))
                throw std::runtime_error("Actor destructor slots changed; refusing unsafe object references");
#if !defined(FDKSS_TEST_PROFILE)
            if (profile->sky_extensions()) {
                auto *target = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr)) +
                    (profile->second_sky() ? 0x654640 : 0x51bf50);
                constexpr unsigned char prologue[]{0x48, 0x89, 0x5c, 0x24, 0x20, 0x55, 0x56, 0x57};
                if (std::memcmp(target, prologue, sizeof(prologue)) != 0)
                    logger(nullptr, "FDKss", "FileStream open prologue changed; content overlay unavailable");
                else if (!content_overlay::start(target, mods_directory))
                    logger(nullptr, "FDKss", content_overlay::error().c_str());
                if (!program_lifetimes::start(profile->second_sky(), thread))
                    logger(nullptr, "FDKss", "Script descriptor destructor changed; unload tracking unavailable");
            }
#endif
            char thread_message[128]{};
            std::snprintf(thread_message, sizeof(thread_message), "Initializing Lua on thread %lu (%s)",
                static_cast<unsigned long>(thread), profile->source());
            logger(nullptr, "FDKss", thread_message);
            const auto root = utf8(mods_directory);
            if (fdkss_create(root.c_str(), logger, nullptr, &runtime) == FDKSS_OK) {
                fdkss_register_native(runtime, "game.name", game_name, nullptr);
                fdkss_register_native(runtime, "game.build_sha256", build_hash, nullptr);
                fdkss_register_native(runtime, "game.update_info", update_info, nullptr);
                fdkss_register_native(runtime, "game.capabilities", capability_info, nullptr);
                fdkss_register_native(runtime, "game.mod_names", mod_names, nullptr);
                fdkss_register_native(runtime, "game.mod_directory", mod_directory, nullptr);
                fdkss_register_native(runtime, "game.request_reload", request_reload, nullptr);
                if (profile->driver != Driver::framework_frame)
                    fdkss_register_native(runtime, "game.overlay", developer_overlay::command, nullptr);
                if (profile->sky_extensions()) {
                    static native_math::Context native_math_context{reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)),profile->second_sky()};
                    fdkss_register_typed_native(runtime,"engine.native_abs",native_math::absolute,&native_math_context);
                    fdkss_register_typed_native(runtime,"engine.native_min",native_math::minimum,&native_math_context);
                    fdkss_register_typed_native(runtime,"engine.native_max",native_math::maximum,&native_math_context);
                    fdkss_register_typed_native(runtime,"engine.native_sqrt",native_math::square_root,&native_math_context);
                    fdkss_register_typed_native(runtime, "engine.actor_hook_info", actor_hooks::info, nullptr);
                    fdkss_register_typed_native(runtime, "engine.actor_hook_probe", actor_hooks::request_probe, nullptr);
                    fdkss_register_typed_native(runtime, "engine.actor_hook_probe_result", actor_hooks::probe_result, nullptr);
                    fdkss_register_typed_native(runtime,"engine.placement_hook_info",placement_hooks::info,nullptr);
                    fdkss_register_typed_native(runtime,"engine.resource_hook_info",resource_hooks::info,nullptr);
                    fdkss_register_typed_native(runtime,"engine.placement_hook_probe",placement_hooks::request_probe,nullptr);
                    fdkss_register_typed_native(runtime,"engine.placement_hook_probe_result",placement_hooks::probe_result,nullptr);
                    fdkss_register_typed_native(runtime, "engine.actor_constructor_info", actor_construction::info, nullptr);
                    fdkss_register_typed_native(runtime, "engine.actor_constructor_self_test", actor_construction::self_test, nullptr);
                    sky1_scripts::bind_jobs(runtime, profile->second_sky() ? &sky2_catalog : nullptr);
                    fdkss_register_native(runtime, "game.script_lifetimes", program_lifetimes::info, nullptr);
                    fdkss_register_typed_native(runtime, "engine.vm_program_lifetime_self_test", program_lifetimes::self_test, nullptr);
                }
                fdkss_register_typed_native(runtime, "game.console_enqueue", developer_overlay::enqueue, nullptr);
                fdkss_register_typed_native(runtime, "game.console_result", developer_overlay::result, nullptr);
                if (profile->driver != Driver::framework_frame) {
                    fdkss_register_typed_native(runtime, "game.browser_request", developer_overlay::browser_request, nullptr);
                    fdkss_register_typed_native(runtime, "game.browser_info", developer_overlay::browser_info, nullptr);
                    fdkss_register_typed_native(runtime, "game.browser_result", developer_overlay::browser_result, nullptr);
                    fdkss_register_typed_native(runtime, "game.browser_call", developer_overlay::browser_call, nullptr);
                    fdkss_register_typed_native(runtime, "game.browser_watch", developer_overlay::browser_watch, nullptr);
                }
                fdkss_register_typed_native(runtime, "engine.hook_supported", hook_supported, nullptr);
                if (profile->sky_extensions()) {
                    fdkss_register_native(runtime, "game.framework", inspect_framework, nullptr);
                    fdkss_register_native(runtime, "game.objects", inspect_objects, nullptr);
                    fdkss_register_native(runtime, "game.scene", inspect_scene, nullptr);
                    fdkss_register_native(runtime, "game.player", inspect_player, nullptr);
                }
                if (profile->sky_extensions() && !profile->second_sky()) {
                    fdkss_register_native(runtime, "game.content", inspect_content, nullptr);
                    fdkss_register_native(runtime, "game.file_requests", inspect_file_requests, nullptr);
                    fdkss_register_native(runtime, "game.set_time_scale", change_time_scale, nullptr);
                    fdkss_register_typed_native(runtime, "engine.get", sky1::get_property, nullptr);
                    fdkss_register_typed_native(runtime, "engine.player_resources", sky1::player_resources, nullptr);
                    fdkss_register_typed_native(runtime, "engine.battle_snapshot", sky1::battle_roster, nullptr);
                    fdkss_register_typed_native(runtime, "engine.player_conditions", sky1::player_conditions, nullptr);
                    fdkss_register_typed_native(runtime, "engine.character_resources", sky1::character_resources, nullptr);
                    fdkss_register_typed_native(runtime, "engine.character_model", sky1::character_model, nullptr);
                    fdkss_register_typed_native(runtime, "engine.actor_model", sky1::actor_model, nullptr);
                    fdkss_register_typed_native(runtime, "engine.character_status_value", sky1::character_status_value, nullptr);
                    fdkss_register_typed_native(runtime, "engine.character_conditions", sky1::character_conditions, nullptr);
                    fdkss_register_typed_native(runtime, "engine.set_player_resource", sky1::set_player_resource, nullptr);
                    fdkss_register_typed_native(runtime, "engine.set", sky1::set_property, nullptr);
                    fdkss_register_typed_native(runtime, "engine.list", sky1::list_objects, nullptr);
                    fdkss_register_typed_native(runtime, "engine.class_info", sky1::class_info, nullptr);
                    fdkss_register_typed_native(runtime, "engine.call", sky1::call_method, nullptr);
                    fdkss_register_typed_native(runtime, "engine.script_command_groups", native_commands::groups, nullptr);
                    fdkss_register_typed_native(runtime, "engine.script_command_info", native_commands::info, nullptr);
                    fdkss_register_typed_native(runtime, "engine.script_command_page", native_commands::page, nullptr);
                    fdkss_register_typed_native(runtime, "engine.script_names", sky1_scripts::names, nullptr);
                    fdkss_register_typed_native(runtime, "engine.script_info", sky1_scripts::info, nullptr);
                    fdkss_register_typed_native(runtime, "engine.script_functions", sky1_scripts::functions, nullptr);
                    fdkss_register_typed_native(runtime, "engine.script_function_info", sky1_scripts::function_info, nullptr);
                    fdkss_register_typed_native(runtime, "engine.script_function_at", sky1_scripts::function_at, nullptr);
                    fdkss_register_typed_native(runtime, "engine.script_variables", sky1_scripts::variables, nullptr);
                    fdkss_register_typed_native(runtime, "engine.script_variable_info", sky1_scripts::variable_info, nullptr);
                    fdkss_register_typed_native(runtime, "engine.script_variable_set", sky1_scripts::set_variable, nullptr);
                    fdkss_register_typed_native(runtime, "engine.vm_self_test", sky1_scripts::self_test, nullptr);
                    fdkss_register_typed_native(runtime, "engine.script_constant", sky1_scripts::call_constant, nullptr);
                    fdkss_register_typed_native(runtime, "engine.script_pure", sky1_scripts::call_pure, nullptr);
                    fdkss_register_typed_native(runtime, "engine.script_pure_preview", sky1_scripts::preview_pure, nullptr);
                    fdkss_register_typed_native(runtime, "engine.vm_pure_self_test", sky1_scripts::pure_self_test, nullptr);
                    fdkss_register_typed_native(runtime, "engine.vm_arithmetic_self_test", sky1_scripts::arithmetic_self_test, nullptr);
                    fdkss_register_typed_native(runtime, "engine.vm_float_self_test", sky1_scripts::float_self_test, nullptr);
                    fdkss_register_typed_native(runtime,"engine.vm_native_math_self_test",sky1_scripts::native_math_self_test,nullptr);
                    fdkss_register_typed_native(runtime, "engine.vm_branch_self_test", sky1_scripts::branch_self_test, nullptr);
                    fdkss_register_typed_native(runtime, "engine.vm_loop_self_test", sky1_scripts::loop_self_test, nullptr);
                    fdkss_register_typed_native(runtime, "engine.vm_budget_self_test", sky1_scripts::budget_self_test, nullptr);
                    fdkss_register_typed_native(runtime, "engine.vm_lease_self_test", sky1_scripts::lease_self_test, nullptr);
                    fdkss_register_typed_native(runtime, "engine.vm_global_self_test", sky1_scripts::global_self_test, nullptr);
                    fdkss_register_typed_native(runtime, "engine.vm_nested_self_test", sky1_scripts::nested_self_test, nullptr);
                    fdkss_register_typed_native(runtime, "engine.vm_comparison_self_test", sky1_scripts::comparison_self_test, nullptr);
                    fdkss_register_typed_native(runtime, "engine.vm_default_self_test", sky1_scripts::default_self_test, nullptr);
                    fdkss_register_typed_native(runtime, "engine.resource_read", sky1::read_resource, nullptr);
#if !defined(FDKSS_TEST_PROFILE)
                    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
                    void **slots[]{reinterpret_cast<void **>(image + 0x7fc550 + 0x20),
                        reinterpret_cast<void **>(image + 0x8041e8 + 0x20)};
                    vm_installed = vm_hooks::start(slots, 2, reinterpret_cast<void *>(image + 0x4b29f0), runtime, thread);
                    if (!vm_installed) logger(nullptr, "FDKss", "VM command slots changed; VM hooks unavailable");
#endif
                }
                if (profile->second_sky()) {
                    fdkss_register_native(runtime, "game.content", inspect_content, nullptr);
                    fdkss_register_native(runtime, "game.file_requests", inspect_file_requests, nullptr);
                    fdkss_register_typed_native(runtime, "engine.get", sky2::get_property, nullptr);
                    fdkss_register_typed_native(runtime, "engine.player_resources", sky2::player_resources, nullptr);
                    fdkss_register_typed_native(runtime, "engine.battle_snapshot", sky2::battle_roster, nullptr);
                    fdkss_register_typed_native(runtime, "engine.player_conditions", sky2::player_conditions, nullptr);
                    fdkss_register_typed_native(runtime, "engine.character_resources", sky2::character_resources, nullptr);
                    fdkss_register_typed_native(runtime, "engine.character_model", sky2::character_model, nullptr);
                    fdkss_register_typed_native(runtime, "engine.actor_model", sky2::actor_model, nullptr);
                    fdkss_register_typed_native(runtime, "engine.character_status_value", sky2::character_status_value, nullptr);
                    fdkss_register_typed_native(runtime, "engine.character_conditions", sky2::character_conditions, nullptr);
                    fdkss_register_typed_native(runtime, "engine.set_player_resource", sky2::set_player_resource, nullptr);
                    fdkss_register_typed_native(runtime, "engine.set", sky2::set_property, nullptr);
                    fdkss_register_typed_native(runtime, "engine.list", sky2::list_objects, nullptr);
                    fdkss_register_typed_native(runtime, "engine.class_info", sky2::class_info, nullptr);
                    fdkss_register_typed_native(runtime, "engine.call", sky2::call_method, nullptr);
                    fdkss_register_typed_native(runtime, "engine.script_command_groups", native_commands::groups, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.script_command_info", native_commands::info, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.script_command_page", native_commands::page, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.script_names", sky1_scripts::names, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.script_info", sky1_scripts::info, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.script_functions", sky1_scripts::functions, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.script_function_info", sky1_scripts::function_info, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.script_function_at", sky1_scripts::function_at, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.script_variables", sky1_scripts::variables, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.script_variable_info", sky1_scripts::variable_info, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.script_variable_set", sky1_scripts::set_variable, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.vm_self_test", sky1_scripts::self_test, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.script_constant", sky1_scripts::call_constant, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.script_pure", sky1_scripts::call_pure, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.script_pure_preview", sky1_scripts::preview_pure, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.vm_pure_self_test", sky1_scripts::pure_self_test, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.vm_arithmetic_self_test", sky1_scripts::arithmetic_self_test, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.vm_float_self_test", sky1_scripts::float_self_test, &sky2_catalog);
                    fdkss_register_typed_native(runtime,"engine.vm_native_math_self_test",sky1_scripts::native_math_self_test,&sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.vm_branch_self_test", sky1_scripts::branch_self_test, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.vm_loop_self_test", sky1_scripts::loop_self_test, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.vm_budget_self_test", sky1_scripts::budget_self_test, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.vm_lease_self_test", sky1_scripts::lease_self_test, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.vm_global_self_test", sky1_scripts::global_self_test, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.vm_nested_self_test", sky1_scripts::nested_self_test, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.vm_comparison_self_test", sky1_scripts::comparison_self_test, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.vm_default_self_test", sky1_scripts::default_self_test, &sky2_catalog);
                    fdkss_register_typed_native(runtime, "engine.content_self_test", sky2::content_self_test, nullptr);
                    fdkss_register_typed_native(runtime, "engine.resource_read", sky2::read_resource, nullptr);
#if !defined(FDKSS_TEST_PROFILE)
                    // Independently traced Sky 2 Thread dispatch: RCX=this,
                    // EDX=group, R8D=command. Both primary tables share it.
                    const auto image = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
                    void **slots[]{reinterpret_cast<void **>(image + 0xb13128 + 0x20),
                        reinterpret_cast<void **>(image + 0xb1c318 + 0x20)};
                    vm_installed = vm_hooks::start(slots, 2, reinterpret_cast<void *>(image + 0x5e63c0), runtime, thread);
                    if (!vm_installed) logger(nullptr, "FDKss", "Sky 2 VM command slots changed; VM hooks unavailable");
#endif
                }
#if !defined(FDKSS_TEST_PROFILE)
                if (profile->sky_extensions() && !actor_construction::start(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)), profile->second_sky(), thread))
                    logger(nullptr, "FDKss", "Actor constructor profile changed; constructor tracking unavailable");
                if (profile->sky_extensions() && !actor_hooks::start(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)), profile->second_sky(), runtime, thread))
                    logger(nullptr, "FDKss", "Actor getter slots changed; actor hooks unavailable");
                if(profile->sky_extensions() && !resource_hooks::start(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)),profile->second_sky(),runtime,thread))
                    logger(nullptr,"FDKss","Native resource setter profile changed; resource hooks unavailable");
                if(profile->sky_extensions() && !placement_hooks::start(reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)),profile->second_sky(),runtime,thread))
                    logger(nullptr,"FDKss","Character placement profile changed; placement hooks unavailable");
#endif
                if (fdkss_start(runtime) != FDKSS_OK) {
                    logger(nullptr, "FDKss", fdkss_last_error(runtime));
                    actor_construction::stop();
                    actor_hooks::stop();
                    resource_hooks::stop();
                    placement_hooks::stop();
                    vm_hooks::stop(); vm_installed = false;
                    fdkss_destroy(runtime);
                    runtime = nullptr;
                }
                publish_mod_names();
#if !defined(FDKSS_TEST_PROFILE)
                if (runtime && profile->sky_extensions()) developer_overlay::bind_browser(profile->second_sky(),
                    profile->second_sky() ? sky2::get_property : sky1::get_property,
                    profile->second_sky() ? sky2::list_objects : sky1::list_objects,
                    profile->second_sky() ? sky2::set_property : sky1::set_property,
                    profile->second_sky() ? sky2::call_method : sky1::call_method);
                if (runtime && profile->sky_extensions() && !developer_overlay::start(profile->second_sky()))
                    logger(nullptr, "FDKss", "Renderer profile changed; developer overlay unavailable");
#endif
            } else logger(nullptr, "FDKss", "Could not initialize Mods directory");
            last_tick = std::chrono::steady_clock::now();
        }
        if (runtime) {
            if(profile->sky_extensions())placement_hooks::service_probe();
            if (profile->sky_extensions()) sky1_scripts::service_jobs(runtime,static_cast<float>(delta));
            if (profile->sky_extensions()) program_lifetimes::emit_events(runtime);
            if (profile->sky_extensions()) actor_construction::emit_events(runtime);
            if (profile->sky_extensions()) { if (profile->second_sky()) sky2::emit_lifetime_events(runtime); else sky1::emit_lifetime_events(runtime); }
            if (profile->sky_extensions() && actor_hooks::probe_pending()) {
                fdkss_value args[2]{};for(auto &arg : args)arg.type=FDKSS_STRING;
                args[0].string="player";args[0].string_size=6;args[1].string="address";args[1].string_size=7;
                std::uintptr_t actor=0;char error[128]{};
                const auto getter=profile->second_sky() ? sky2::get_property : sky1::get_property;
                getter(nullptr,args,2,+[](void *out,const fdkss_value *value)->int {
                    if(value->type!=FDKSS_STRING || value->string_size<3 || value->string_size>32 ||
                        value->string[0]!='0' || value->string[1]!='x')return 1;
                    const auto end=value->string+value->string_size;
                    const auto parsed=std::from_chars(value->string+2,end,*static_cast<std::uintptr_t *>(out),16);
                    return parsed.ec!=std::errc{} || parsed.ptr!=end;
                },&actor,error,sizeof(error));
                actor_hooks::service_probe(actor);
            }
            fdkss_tick(runtime, delta);
            developer_overlay::service(runtime);
            if (profile->sky_extensions() && developer_overlay::visible()) {
                developer_overlay::Snapshot snapshot{};
                snapshot.updates = update_count; snapshot.owner_thread = thread;
                const auto getter = profile->second_sky() ? sky2::get_property : sky1::get_property;
                const auto number = +[](void *sink, const fdkss_value *value) -> int {
                    if (value->type == FDKSS_NUMBER) *static_cast<double *>(sink) = value->number;
                    else if (value->type == FDKSS_INTEGER) *static_cast<double *>(sink) = static_cast<double>(value->integer);
                    else return 1;
                    return 0;
                };
                fdkss_value args[2]{};
                for (auto &arg : args) arg.type = FDKSS_STRING;
                args[0].string = "scene"; args[0].string_size = 5;
                args[1].string = "map_id"; args[1].string_size = 6;
                char error[128]{};
                getter(nullptr, args, 2, +[](void *sink, const fdkss_value *value) -> int {
                    if (value->type != FDKSS_STRING) return 1;
                    auto *snapshot = static_cast<developer_overlay::Snapshot *>(sink);
                    const auto size = std::min(value->string_size, sizeof(snapshot->map) - 1);
                    std::memcpy(snapshot->map, value->string, size); return 0;
                }, &snapshot, error, sizeof(error));
                args[0].string = "player"; args[0].string_size = 6;
                snapshot.player_available = true;
                const char *names[]{"position_x", "position_y", "position_z"};
                for (int axis = 0; axis < 3; ++axis) {
                    args[1].string = names[axis]; args[1].string_size = 10;
                    if (getter(nullptr, args, 2, number, &snapshot.position[axis], error, sizeof(error))) snapshot.player_available = false;
                }
                args[0].string = "framework"; args[0].string_size = 9;
                args[1].string = "time_scale"; args[1].string_size = 10;
                getter(nullptr, args, 2, number, &snapshot.time_scale, error, sizeof(error));
                developer_overlay::publish(snapshot);
            }
            if (profile->sky_extensions()) update_event("hook.application.update.post", delta, result);
            // Reload enabled mods at a pump boundary, only while the game has focus.
            DWORD foreground_pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
            const bool down = foreground_pid == GetCurrentProcessId() &&
                              (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
            if (down && !reload_down) {
                reload_mods();
            }
            reload_down = down;
            const bool stop = foreground_pid == GetCurrentProcessId() &&
                              (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
            if (stop && !stop_down) {
                stop_scripts();
            }
            stop_down = stop;
            if (runtime) {
                static keyboard_input::State keyboard;
                std::array<bool, 256> keys{};
                const bool focused = foreground_pid == GetCurrentProcessId();
                if (focused) for (int key = 8; key < 255; ++key) keys[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
                keyboard.sample(focused && !developer_overlay::captures_keyboard(), keys, [](int key, bool pressed, int modifiers) {
                    fdkss_value values[3]{};
                    values[0].type = FDKSS_INTEGER; values[0].integer = key;
                    values[1].type = FDKSS_BOOLEAN; values[1].boolean = pressed;
                    values[2].type = FDKSS_INTEGER; values[2].integer = modifiers;
                    fdkss_emit_values(runtime, "input.key", values, 3);
                });
            }
        }
        const unsigned requested = commands.exchange(0);
        live_console::pump(runtime);
        if (requested & command_detach) {
            if (stop_scripts()) restore_hook();
        } else if (requested & command_stop) stop_scripts();
        else if (requested & command_reload) reload_mods();
    } catch (const std::exception &e) { logger(nullptr, "FDKss", e.what()); }
    catch (...) { logger(nullptr, "FDKss", "Unexpected adapter error"); }
    in_callback = false;
    return result;
}
void __cdecl pump() {
    original.load(std::memory_order_acquire)();
    run_scripts(0.0, true);
}
void framework_frame_post(double delta, std::uintptr_t result) {
    run_scripts(delta, false, result);
}
std::uintptr_t __fastcall application_update(void *application, float delta) {
    if (installed.load(std::memory_order_acquire) && owner.load() == GetCurrentThreadId() && runtime &&
        !in_callback && std::isfinite(delta) && delta >= 0) {
        in_callback = true;
        try {
            if (profile->second_sky()) sky2::refresh_scene(); else sky1::refresh_scene();
            double filtered = delta;
            std::uintptr_t unused = 0;
            update_event("hook.application.update.pre", filtered, unused);
            delta = static_cast<float>(filtered);
        } catch (const std::exception &e) { logger(nullptr, "FDKss", e.what()); }
        catch (...) { logger(nullptr, "FDKss", "Unexpected pre-update hook error"); }
        in_callback = false;
    }
    const auto result = original_update.load(std::memory_order_acquire)(application, delta);
    // The engine update method releases its internal lock before returning.
    // Run Lua afterward, before Framework signals the main thread that this update ended.
    return run_scripts(static_cast<double>(delta), false, result);
}
void restore_hook() {
    if (profile->driver == Driver::framework_frame) {
        const bool restored = framework_bridge::stop();
        installed.store(false, std::memory_order_release);
        live_console::stop();
        logger(nullptr, "FDKss", restored ? "Framework frame detached; complete original body verified; DLL remains pinned" :
            "Framework frame restoration refused; callback dispatch disabled; foreign bytes and pinned trampoline retained");
        return;
    }
    const auto replacement = profile->sky_extensions() ? reinterpret_cast<void *>(&application_update) : reinterpret_cast<void *>(&pump);
    const auto target = profile->sky_extensions() ? reinterpret_cast<void *>(original_update.load()) : reinterpret_cast<void *>(original.load());
    DWORD protection = 0, ignored = 0;
    if (!VirtualProtect(hook_slot, sizeof(void *), PAGE_READWRITE, &protection)) {
        logger(nullptr, "FDKss", "Hook restoration failed; request detach again");
        return;
    }
    void *previous = InterlockedCompareExchangePointer(reinterpret_cast<void *volatile *>(hook_slot), target, replacement);
    if (!VirtualProtect(hook_slot, sizeof(void *), protection, &ignored))
        logger(nullptr, "FDKss", "Could not restore hook-page protection");
    installed.store(false, std::memory_order_release);
    live_console::stop();
    logger(nullptr, "FDKss", previous == replacement ? "Hook detached; DLL remains loaded" :
        "Hook slot changed externally; left it untouched and disabled Lua");
}
std::filesystem::path module_path(HMODULE module) {
    wchar_t path[32768]{};
    const DWORD length = GetModuleFileNameW(module, path, 32768);
    if (!length || length >= 32768) throw std::runtime_error("could not resolve module path");
    return std::filesystem::path(path);
}
std::string hash_file(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("could not open game executable");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    struct Cleanup {
        BCRYPT_ALG_HANDLE &a; BCRYPT_HASH_HANDLE &h;
        ~Cleanup() { if (h) BCryptDestroyHash(h); if (a) BCryptCloseAlgorithmProvider(a, 0); }
    } cleanup{algorithm, hash};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("could not initialize SHA-256");
    char buffer[65536];
    while (stream) {
        stream.read(buffer, sizeof(buffer));
        if (stream.gcount() && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer),
            static_cast<ULONG>(stream.gcount()), 0) < 0) throw std::runtime_error("SHA-256 failed");
    }
    if (!stream.eof()) throw std::runtime_error("could not read game executable");
    unsigned char digest[32]{};
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0) throw std::runtime_error("SHA-256 failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
void **find_pump_slot(HMODULE module) {
    auto *base = reinterpret_cast<unsigned char *>(module);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return nullptr;
    const auto &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return nullptr;
    auto *entry = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + directory.VirtualAddress);
    for (; entry->Name; ++entry) {
        if (_stricmp(reinterpret_cast<char *>(base + entry->Name), "steam_api64.dll") != 0) continue;
        if (!entry->OriginalFirstThunk) return nullptr;
        auto *names = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + entry->OriginalFirstThunk);
        auto *slots = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + entry->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto *import = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<char *>(import->Name), "SteamAPI_RunCallbacks") == 0)
                return reinterpret_cast<void **>(&slots->u1.Function);
        }
    }
    return nullptr;
}
}

// Called explicitly by the injector after LoadLibrary has completed.
// No initialization under loader lock; the module remains loaded until process exit.
extern "C" __declspec(dllexport) DWORD WINAPI FDKssAttach(void *) {
    try {
        std::lock_guard lock(attachment_mutex);
        if (installed.load()) return ERROR_ALREADY_EXISTS;
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&FDKssAttach), &self)) return GetLastError();
        const auto directory = module_path(self).parent_path();
        logfile.open(directory / "FDKss.log", std::ios::app);
        if (!logfile) return ERROR_ACCESS_DENIED;
        const auto game = module_path(nullptr);
        const auto fingerprint = hash_file(game);
        for (const auto &candidate : profiles) {
            if (_wcsicmp(game.filename().c_str(), candidate.file) == 0 && fingerprint == candidate.sha256)
                profile = &candidate;
        }
        if (!profile) {
            logger(nullptr, "FDKss", "Unsupported executable/build; refusing to install hook");
            logfile.close();
            return ERROR_BAD_EXE_FORMAT;
        }
        mods_directory = directory / "Mods";
        std::filesystem::create_directories(mods_directory);
        if (profile->driver == Driver::framework_frame) {
            framework_bridge::Profile contract{};
            if (profile->sky_extensions() || !profile->frame_contract || !profile->frame_contract(contract) ||
                !framework_bridge::install(contract, framework_frame_post)) return ERROR_INVALID_DATA;
            logger(nullptr, "FDKss", "Attached framework frame bridge; core mod platform only; waiting for the proved owner");
            installed.store(true, std::memory_order_release);
            if (!live_console::start()) logger(nullptr, "FDKss", "Live console unavailable; frame bridge and mods remain active");
            return ERROR_SUCCESS;
        }
        if (profile->driver == Driver::application_update && !profile->sky_extensions()) return ERROR_INVALID_DATA;
        void **slot = profile->sky_extensions() ? (profile->second_sky() ? sky2::update_slot() : sky1::update_slot()) :
            find_pump_slot(GetModuleHandleW(nullptr));
#if defined(FDKSS_TEST_ENGINE_PROFILE)
        if (_wcsicmp(game.filename().c_str(), L"fdkss_adapter_fixture.exe") == 0) {
            using FixtureSlot = void **(__cdecl *)(void);
            const auto fixture_slot = reinterpret_cast<FixtureSlot>(GetProcAddress(GetModuleHandleW(nullptr), "TestUpdateSlot"));
            slot = fixture_slot ? fixture_slot() : nullptr;
        }
#endif
        if (!slot || !*slot) return ERROR_PROC_NOT_FOUND;
        hook_slot = slot;
        // Pin the DLL: unloading it while the IAT references pump would be unsafe.
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&FDKssAttach), &self)) return GetLastError();
        DWORD protection = 0;
        if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protection)) return GetLastError();
        void *replacement = nullptr;
        if (profile->sky_extensions()) {
            original_update.store(reinterpret_cast<Update>(*slot), std::memory_order_release);
            replacement = reinterpret_cast<void *>(&application_update);
        } else {
            original.store(reinterpret_cast<Pump>(*slot), std::memory_order_release);
            replacement = reinterpret_cast<void *>(&pump);
        }
        InterlockedExchangePointer(reinterpret_cast<void *volatile *>(slot), replacement);
        DWORD ignored = 0;
        if (!VirtualProtect(slot, sizeof(void *), protection, &ignored))
            logger(nullptr, "FDKss", "Warning: could not restore IAT page protection");
        logger(nullptr, "FDKss", profile->sky_extensions() ?
            "Attached; waiting for FDK application update to initialize Lua" :
            "Attached; waiting for Steam callback pump to initialize Lua");
        installed.store(true, std::memory_order_release);
        if (!live_console::start()) logger(nullptr, "FDKss", "Live console unavailable; update hook and mods remain active");
        return ERROR_SUCCESS;
    } catch (...) { return ERROR_DLL_INIT_FAILED; }
}
extern "C" __declspec(dllexport) DWORD WINAPI FDKssRequestReload(void *) {
    if (!installed.load(std::memory_order_acquire)) return ERROR_INVALID_STATE;
    commands.fetch_or(command_reload);
    return ERROR_SUCCESS;
}
extern "C" __declspec(dllexport) DWORD WINAPI FDKssRequestStop(void *) {
    if (!installed.load(std::memory_order_acquire)) return ERROR_INVALID_STATE;
    commands.fetch_or(command_stop);
    return ERROR_SUCCESS;
}
extern "C" __declspec(dllexport) DWORD WINAPI FDKssRequestDetach(void *) {
    if (!installed.load(std::memory_order_acquire)) return ERROR_INVALID_STATE;
    commands.fetch_or(command_detach);
    return ERROR_SUCCESS;
}
