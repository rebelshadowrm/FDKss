#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "sky2.h"
#include "scripts.h"
#include "resources.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <source_location>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
namespace {
std::unique_ptr<unsigned char[]> image;
std::array<unsigned char, 0x200> application{}, framework{};
void check(bool result, std::source_location where = std::source_location::current()) { if (!result) throw std::runtime_error("Sky 2 profile check failed at line " + std::to_string(where.line())); }
template<class T> void store(std::uintptr_t address, const T &value) { std::memcpy(reinterpret_cast<void *>(address), &value, sizeof(value)); }
fdkss_value string(const char *text) {
    fdkss_value value{}; value.type = FDKSS_STRING; value.string = text; value.string_size = std::strlen(text); return value;
}
int FDKSS_CALL capture(void *out, const fdkss_value *value) { *static_cast<fdkss_value *>(out) = *value; return 0; }
int FDKSS_CALL capture_text(void *out, const fdkss_value *value) {
    auto &rows = *static_cast<std::vector<std::string> *>(out);
    rows.push_back(value->type == FDKSS_STRING ? std::string(value->string, value->string_size) : std::to_string(value->integer)); return 0;
}
}
std::uintptr_t sky2_test_base() { return reinterpret_cast<std::uintptr_t>(image.get()); }
int main() {
    try {
        image = std::make_unique<unsigned char[]>(0xc61000);
        std::uint32_t raw_constant = 0;
        const unsigned char constant[]{0x26,1,0,0,4,42,0,0,0x40,0x0a,0,0x0d};
        check(sky1_scripts::constant_code(constant, sizeof(constant), raw_constant) && raw_constant == 0x4000002a);
        check(!sky1_scripts::constant_code(constant, sizeof(constant) - 1, raw_constant));
        const unsigned char command[]{0x24,0,1,0,0x0d};
        check(!sky1_scripts::constant_code(command, sizeof(command), raw_constant));
        const unsigned char loop[]{0x0e,0,0,0,0};
        check(!sky1_scripts::constant_code(loop, sizeof(loop), raw_constant));
        const unsigned char reference[]{0,4,1,0,0,0,0x0a,0,0x0d};
        check(!sky1_scripts::constant_code(reference, sizeof(reference), raw_constant));
        const auto base = sky2_test_base(), app = reinterpret_cast<std::uintptr_t>(application.data()), fw = reinterpret_cast<std::uintptr_t>(framework.data());
        auto object = [&](std::uintptr_t address, std::uintptr_t global, std::uintptr_t table, std::uint32_t locator, std::uint32_t type, const char *name) {
            store(base + global, address); store(address, base + table); store(base + table - 8, base + locator);
            const std::array<std::uint32_t, 6> col{1, 0, 0, type, 0, locator}; store(base + locator, col);
            std::memcpy(reinterpret_cast<void *>(base + type + 16), name, std::strlen(name) + 1);
        };
        object(app, 0xc5d7a0, 0xae7ab0, 0x1000, 0x2000, ".?AVApplication@sora@@");
        object(fw, 0xc60ec0, 0xb1ad08, 0x1100, 0x2100, ".?AVFramework@fdk@@");
        store(base + 0xae7ab0 + 0x38, base + 0x311f0);
        store(fw + 0xa0, GetCurrentThreadId()); store(fw + 0x40, 1.0f); store(fw + 0x38, 0.016f);
        check(sky2::update_slot() == reinterpret_cast<void **>(base + 0xae7ab0 + 0x38));
        store(base + 0xae7ab0 + 0x38, base + 0x2fb10); check(!sky2::update_slot());
        store(base + 0xae7ab0 + 0x38, base + 0x311f0);
        store(fw, base + 0x7da5a8); check(!sky2::update_slot()); store(fw, base + 0xb1ad08);
        char error[256]{}; fdkss_value result{};
        // Sky2 snapshots use its independent offsets, never Sky1's scale/worker fields.
        char snapshot[2048]{};
        store(fw + 0x30, 60.0f); store(fw + 0x3c, 0.75f); store(fw + 0x68, DWORD{123});
        check(!sky2::describe("framework", snapshot, sizeof(snapshot)));
        check(std::strstr(snapshot, "\"time_scale\":1") && std::strstr(snapshot, "\"fps\":60") &&
            std::strstr(snapshot, ("\"update_thread\":" + std::to_string(GetCurrentThreadId())).c_str()));
        check(!sky2::describe("scene", snapshot, sizeof(snapshot)) &&
            std::strcmp(snapshot, "{\"available\":false}") == 0);
        check(!sky2::describe("player", snapshot, sizeof(snapshot)) &&
            std::strcmp(snapshot, "{\"available\":false}") == 0);
        check(!sky2::describe("objects", snapshot, sizeof(snapshot)) &&
            std::strstr(snapshot, "\"scene\":null") && std::strstr(snapshot, "\"fieldmap\":null"));
        check(sky2::describe("unknown", snapshot, sizeof(snapshot)) != 0);
        check(sky2::describe(nullptr, snapshot, sizeof(snapshot)) != 0);
        check(sky2::describe("framework", nullptr, 0) != 0);
        std::array<char, 4> short_buffer{'a','b','c','d'};
        check(sky2::describe("framework", short_buffer.data(), 2) != 0 && short_buffer[2] == 'c' && short_buffer[3] == 'd');
        bool snapshot_denied = false;
        std::thread foreign_snapshot([&] { snapshot_denied = sky2::describe("framework", snapshot, sizeof(snapshot)) != 0; });
        foreign_snapshot.join(); check(snapshot_denied);
        store(fw + 0x30, std::numeric_limits<float>::quiet_NaN());
        check(sky2::describe("framework", snapshot, sizeof(snapshot)) != 0); store(fw + 0x30, 60.0f);
        check(sky2::player_resources(nullptr,nullptr,0,capture,&result,error,sizeof(error))!=0);
        // The getter body is intentionally absent here: fail before returning a
        // fabricated status snapshot, even though the owner thread is valid.
        check(std::strstr(error,"profile changed")!=nullptr);
        bool resource_denied=false;
        std::thread resource_other([&]{resource_denied=sky2::player_resources(nullptr,nullptr,0,capture,&result,error,sizeof(error))!=0;});
        resource_other.join();check(resource_denied && std::strstr(error,"owning update thread"));
        std::thread battle_other([&]{resource_denied=sky2::battle_roster(nullptr,nullptr,0,capture,&result,error,sizeof(error))!=0;});
        battle_other.join();check(resource_denied && std::strstr(error,"owning update thread"));
        std::thread resource_edit_other([&]{resource_denied=sky2::set_player_resource(nullptr,nullptr,0,capture,&result,error,sizeof(error))!=0;});
        resource_edit_other.join();check(resource_denied && std::strstr(error,"owning update thread"));
        std::thread condition_other([&]{resource_denied=sky2::player_conditions(nullptr,nullptr,0,capture,&result,error,sizeof(error))!=0;});
        condition_other.join();check(resource_denied && std::strstr(error,"owning update thread"));
        std::thread status_other([&]{resource_denied=sky2::character_status_value(nullptr,nullptr,0,capture,&result,error,sizeof(error))!=0;});
        status_other.join();check(resource_denied && std::strstr(error,"owning update thread"));
        std::thread character_other([&]{resource_denied=sky2::character_conditions(nullptr,nullptr,0,capture,&result,error,sizeof(error))!=0;});
        character_other.join();check(resource_denied && std::strstr(error,"owning update thread"));
        std::thread character_resources_other([&]{resource_denied=sky2::character_resources(nullptr,nullptr,0,capture,&result,error,sizeof(error))!=0;});
        character_resources_other.join();check(resource_denied && std::strstr(error,"owning update thread"));
        check(sky2::character_conditions(nullptr,nullptr,0,capture,&result,error,sizeof(error))!=0 && std::strstr(error,"reference"));

        fdkss_value args[]{string("framework"), string("time_scale"), {}};
        store(fw + 0x30, 85.0f); args[1] = string("fps");
        check(!sky2::get_property(nullptr, args, 2, capture, &result, error, sizeof(error)) && result.type == FDKSS_NUMBER && result.number == 85);
        args[2].type = FDKSS_NUMBER; args[2].number = 60;
        check(sky2::set_property(nullptr, args, 3, capture, &result, error, sizeof(error)) != 0);
        args[1] = string("time_scale");
        check(!sky2::get_property(nullptr, args, 2, capture, &result, error, sizeof(error)) && result.number == 1);
        args[2].type = FDKSS_NUMBER; args[2].number = 0.5;
        check(!sky2::set_property(nullptr, args, 3, capture, &result, error, sizeof(error)) && result.number == 0.5);
        args[2].number = std::numeric_limits<double>::quiet_NaN();
        check(sky2::set_property(nullptr, args, 3, capture, &result, error, sizeof(error)) != 0);
        args[2].number = 3; check(sky2::set_property(nullptr, args, 3, capture, &result, error, sizeof(error)) != 0);
        args[1] = string("position_x"); check(sky2::get_property(nullptr, args, 2, capture, &result, error, sizeof(error)) != 0);
        args[1] = string("time_scale");
        fdkss_value missing[]{string("scene"), string("map_id")};
        check(!sky2::get_property(nullptr, missing, 2, capture, &result, error, sizeof(error)) && result.type == FDKSS_NIL);
        bool denied = false;
        std::thread other([&] { denied = sky2::get_property(nullptr, args, 2, capture, &result, error, sizeof(error)) != 0; }); other.join(); check(denied);
        check(sky2::restore_changes());
        check(!sky2::get_property(nullptr, args, 2, capture, &result, error, sizeof(error)) && result.number == 1);
        args[2].number = 0.75; check(!sky2::set_property(nullptr, args, 3, capture, &result, error, sizeof(error)));
        store(fw + 0x40, 1.5f); check(sky2::restore_changes());
        check(!sky2::get_property(nullptr, args, 2, capture, &result, error, sizeof(error)) && result.number == 1.5);
        // Only the independent Sky 2 catalog root is populated. The Sky 1 root stays null.
        object(base + 0x3000, 0xc60e58, 0xb12fd0, 0x1200, 0x2200, ".?AVManager@script@sora@@");
        object(base + 0x5000, 0x9000, 0x8000, 0x1300, 0x2300, ".?AVBinaryResource@fdk@@");
        store(base + 0x3008, base + 0x4000); store(base + 0x3010, std::uint64_t{1}); store(base + 0x3018, std::uint64_t{1});
        store(base + 0x4008, base + 0x5000); store(base + 0x4010, base + 0x6000);
        std::memcpy(reinterpret_cast<void *>(base + 0x4018), "fixture", 8);
        store(base + 0x6000, base + 0x7000); store(base + 0x6008, std::uint32_t{128});
        const std::array<std::uint32_t, 6> header{0x70637323, 24, 1, 96, 0, 0}; store(base + 0x7000, header);
        const std::array<std::uint32_t, 8> function{56, 0, 0, 0, 0, 0, 0xa8814c43, 0xc0000042}; store(base + 0x7018, function);
        std::memcpy(reinterpret_cast<void *>(base + 0x7042), "FDKssConstant", 14);
        sky1_scripts::CatalogProfile profile{0xc60e58, 0xb12fd0, 0xc60ec0, 0xb1ad08, 0xa0};
        // Local synchronous authored previews need no manager-owned program lease.
        std::array<unsigned char,128> authored{};
        const std::array<std::uint32_t,6> authored_header{0x70637323,24,1,112,0,0};
        const std::array<std::uint32_t,8> authored_record{56,0,96,96,0,0,0xa8814c43,0xc0000042};
        std::memcpy(authored.data(),authored_header.data(),sizeof(authored_header));
        std::memcpy(authored.data()+24,authored_record.data(),sizeof(authored_record));
        std::memcpy(authored.data()+66,"FDKssConstant",14);
        const unsigned char authored_code[]{0,4,42,0,0,0x40,10,0,13};
        std::memcpy(authored.data()+56,authored_code,sizeof(authored_code));
        fdkss_value authored_args[]{string(""),string("FDKssConstant")};
        authored_args[0].string=reinterpret_cast<const char *>(authored.data());authored_args[0].string_size=authored.size();
        check(!sky1_scripts::preview_compiled_call(&profile,authored_args,2,capture,&result,error,sizeof(error)) && result.integer==42);
        check(sky1_scripts::call_compiled(&profile,authored_args,2,capture,&result,error,sizeof(error))!=0 && std::strstr(error,"VM profile"));
        const auto original_authored=authored;
        store(reinterpret_cast<std::uintptr_t>(authored.data()+16),std::uint32_t{1});
        check(sky1_scripts::preview_compiled_call(&profile,authored_args,2,capture,&result,error,sizeof(error))!=0);authored=original_authored;
        authored_args[0].string_size=23;
        check(sky1_scripts::preview_compiled_call(&profile,authored_args,2,capture,&result,error,sizeof(error))!=0);authored_args[0].string_size=authored.size();
        authored[0]^=1;check(sky1_scripts::preview_compiled_call(&profile,authored_args,2,capture,&result,error,sizeof(error))!=0);authored=original_authored;
        bool authored_foreign=false;
        std::thread authored_thread([&]{authored_foreign=sky1_scripts::preview_compiled_call(&profile,authored_args,2,capture,&result,error,sizeof(error))!=0;});authored_thread.join();check(authored_foreign);
        const unsigned char forever[]{11,56,0,0,0};std::memcpy(authored.data()+56,forever,sizeof(forever));
        check(sky1_scripts::preview_compiled_call(&profile,authored_args,2,capture,&result,error,sizeof(error))!=0 && std::strstr(error,"bounded pure"));
        // No executable VM table is populated in this fixture: refuse it before construction.
        check(sky1_scripts::self_test(&profile, nullptr, 0, capture, &result, error, sizeof(error)) != 0);
        check(sky1_scripts::budget_self_test(&profile, nullptr, 0, capture, &result, error, sizeof(error)) != 0 && std::strstr(error, "VM profile does not match"));
        std::thread foreign_budget([&] { denied = sky1_scripts::budget_self_test(&profile, nullptr, 0, capture, &result, error, sizeof(error)) != 0; });
        foreign_budget.join(); check(denied);
        check(sky1_scripts::lease_self_test(&profile, nullptr, 0, capture, &result, error, sizeof(error)) != 0 && std::strstr(error, "acquire failed"));
        std::uint32_t lease_references = 1;
        std::memcpy(&lease_references, reinterpret_cast<void *>(base + 0x4040), sizeof(lease_references));
        check(lease_references == 0); // Missing executable lease profile cannot change the loaded entry.
        std::thread foreign_lease([&] { denied = sky1_scripts::lease_self_test(&profile, nullptr, 0, capture, &result, error, sizeof(error)) != 0; });
        foreign_lease.join(); check(denied);

        std::thread foreign_vm([&] { denied = sky1_scripts::self_test(&profile, nullptr, 0, capture, &result, error, sizeof(error)) != 0; });
        foreign_vm.join(); check(denied);
        fdkss_value page[3]{}; page[0].type = page[1].type = FDKSS_INTEGER; page[1].integer = 32; page[2] = string("");
        fdkss_value resource_args[]{string("../resource.dat"), page[0], page[1]};
        check(sky2::read_resource(nullptr, resource_args, 3, capture, &result, error, sizeof(error)) != 0 && std::strstr(error, "dot components"));
        resource_args[0] = string("C:/resource.dat");
        check(sky2::read_resource(nullptr, resource_args, 3, capture, &result, error, sizeof(error)) != 0 && std::strstr(error, "relative paths"));
        resource_args[0] = string("resource.dat"); resource_args[2].integer = 8193;
        check(sky2::read_resource(nullptr, resource_args, 3, capture, &result, error, sizeof(error)) != 0 && std::strstr(error, "byte count"));
        resource_args[2].integer = 32;
        check(sky2::read_resource(nullptr, resource_args, 3, capture, &result, error, sizeof(error)) != 0 && std::strstr(error, "profile changed"));
        // The independent Sky 1 resource profile cannot borrow Sky 2's framework.
        check(resource_stream::read(base, resource_stream::sky1, resource_args, 3, capture, &result, error, sizeof(error)) != 0 &&
            std::strstr(error, "verified update thread"));
        store(base + 0x8ff030, fw); store(fw, base + 0x802d80); store(fw + 0x68, GetCurrentThreadId());
        resource_args[0] = string("../resource.dat");
        check(resource_stream::read(base, resource_stream::sky1, resource_args, 3, capture, &result, error, sizeof(error)) != 0 &&
            std::strstr(error, "dot components"));
        resource_args[0] = string("resource.dat");
        check(resource_stream::read(base, resource_stream::sky1, resource_args, 3, capture, &result, error, sizeof(error)) != 0 &&
            std::strstr(error, "profile changed"));
        std::thread foreign_resource([&] {
            denied = resource_stream::read(base, resource_stream::sky1, resource_args, 3, capture, &result, error, sizeof(error)) != 0;
        }); foreign_resource.join(); check(denied);
        store(fw, base + 0xb1ad08); store(base + 0x8ff030, std::uintptr_t{0});
        std::vector<std::string> rows;
        check(!sky1_scripts::names(&profile, page, 3, capture_text, &rows, error, sizeof(error)) && rows == std::vector<std::string>{"-1", "fixture"});
        rows.clear(); check(!sky1_scripts::names(nullptr, page, 3, capture_text, &rows, error, sizeof(error)) && rows == std::vector<std::string>{"-1"});
        fdkss_value function_args[]{string("fixture"), page[0]}; rows.clear();
        check(!sky1_scripts::function_at(&profile, function_args, 2, capture_text, &rows, error, sizeof(error)) && rows.size() == 8 && rows[0] == "FDKssConstant");
        fdkss_value pure_args[]{string("fixture"), string("FDKssConstant"), {}};
        auto pure_rejected = [&](std::size_t count, const char *message) {
            check(sky1_scripts::call_pure(&profile, pure_args, count, capture, &result, error, sizeof(error)) != 0 && std::strstr(error, message));
        };
        store(base + 0x7038, static_cast<unsigned char>(13));
        store(base + 0x6030, base + 0x7000); store(base + 0x6038, base + 0x7018);
        check(!sky1_scripts::preview_pure(&profile, pure_args, 2, capture, &result, error, sizeof(error)) && result.type == FDKSS_NIL);
        const unsigned char preview_constant[]{0,4,42,0,0,0x40,10,0,13};
        std::memcpy(reinterpret_cast<void *>(base + 0x7038), preview_constant, sizeof(preview_constant));
        check(!sky1_scripts::preview_pure(&profile, pure_args, 2, capture, &result, error, sizeof(error)) && result.type == FDKSS_INTEGER && result.integer == 42);
        store(base + 0x703a, std::uint32_t{0xc0000064});
        std::memcpy(reinterpret_cast<void *>(base + 0x7064), "abc", 4);
        rows.clear(); check(!sky1_scripts::preview_pure(&profile, pure_args, 2, capture_text, &rows, error, sizeof(error)) && rows == std::vector<std::string>{"abc"});
        store(base + 0x703a, std::uint32_t{0xc0000000});
        check(sky1_scripts::preview_pure(&profile, pure_args, 2, capture, &result, error, sizeof(error)) != 0 && std::strstr(error, "string offset"));
        store(base + 0x7064, std::uint32_t{0});
        store(base + 0x7038, static_cast<unsigned char>(13));
        pure_rejected(2, "profile does not match"); // All preflight checks passed, but no executable profile.
        pure_args[2].type = FDKSS_INTEGER; pure_args[2].integer = 536870912;
        pure_rejected(3, "30-bit"); pure_args[2].integer = 5;
        pure_rejected(3, "supported arity");
        store(base + 0x701c, std::uint32_t{1}); store(base + 0x7024, std::uint32_t{100});
        store(base + 0x7064, std::uint32_t{4}); pure_rejected(3, "reference parameters");
        store(base + 0x7064, std::uint32_t{0}); pure_rejected(3, "bounded pure"); // unpopped argument
        store(base + 0x701c, std::uint32_t{0x200}); pure_rejected(2, "function metadata");
        store(base + 0x701c, std::uint32_t{0}); store(base + 0x7024, std::uint32_t{0});
        store(base + 0x7038, static_cast<unsigned char>(0x24)); pure_rejected(2, "bounded pure");
        store(base + 0x7038, static_cast<unsigned char>(13));
        store(base + 0x6038, std::uintptr_t{0}); pure_rejected(2, "descriptor changed");
        store(base + 0x7018, function); // Restore the intentionally corrupted checksum from the catalog guard test.
        store(base + 0x6038, base + 0x7018);
        std::thread foreign_pure([&] { pure_rejected(2, "verified update thread"); }); foreign_pure.join();
        // Omitted optional parameters use the tail of the compiler's default table.
        store(base + 0x701c, std::uint32_t{0x01000001});
        store(base + 0x7020, std::uint32_t{100}); store(base + 0x7024, std::uint32_t{104});
        store(base + 0x7068, std::uint32_t{0}); store(base + 0x7064, std::uint32_t{0x4000002a});
        const unsigned char default_body[]{10,0,13};
        std::memcpy(reinterpret_cast<void *>(base + 0x7038), default_body, sizeof(default_body));
        check(!sky1_scripts::preview_pure(&profile, pure_args, 2, capture, &result, error, sizeof(error)) && result.integer == 42);
        check(!sky1_scripts::preview_pure(&profile, pure_args, 3, capture, &result, error, sizeof(error)) && result.integer == 5);
        store(base + 0x7064, std::uint32_t{0xc0000042}); pure_rejected(2, "defaults require");
        store(base + 0x701c, std::uint32_t{0x01000002}); pure_rejected(2, "supported arity");
        store(base + 0x7018, function); store(base + 0x7038, static_cast<unsigned char>(13));

        // Preview a terminating backward-loop path through Sky 2's own catalog.
        store(base + 0x6008, std::uint32_t{1024});
        store(base + 0x7018, std::uint32_t{512}); store(base + 0x701c, std::uint32_t{1});
        store(base + 0x7024, std::uint32_t{104}); store(base + 0x7068, std::uint32_t{0});
        unsigned char loop_body[]{10,1,0,4,0,0,0,0x40,10,0,
            9,1,15,0,0,0,0,9,0,9,1,16,10,0,9,1,0,4,1,0,0,0x40,17,10,1,11,0,0,0,0,13};
        const std::uint32_t loop_exit = 552, loop_start = 522;
        std::memcpy(loop_body + 13, &loop_exit, 4); std::memcpy(loop_body + 36, &loop_start, 4);
        std::memcpy(reinterpret_cast<void *>(base + 0x7200), loop_body, sizeof(loop_body));
        pure_args[2].type = FDKSS_INTEGER; pure_args[2].integer = 11;
        check(!sky1_scripts::preview_pure(&profile, pure_args, 3, capture, &result, error, sizeof(error)) && result.integer == 66);
        pure_rejected(3, "VM profile does not match"); // Preflight still cannot borrow an unpopulated execution profile.
        pure_args[2].integer = 12; pure_rejected(3, "bounded pure");
        check(sky1_scripts::preview_pure(&profile, pure_args, 3, capture, &result, error, sizeof(error)) != 0);
        store(base + 0x6008, std::uint32_t{128}); store(base + 0x7018, function);

        // Nested calls resolve and validate every callee's current metadata.
        store(base + 0x6008, std::uint32_t{2048}); store(base + 0x7008, std::uint32_t{2});
        const std::array<std::uint32_t, 8> caller{512,1,0,104,0,0,0xa8814c43,0xc00005dc};
        const std::array<std::uint32_t, 8> child{1024,1,0,108,0,0,0x1c8d84d2,0xc00005f0};
        store(base + 0x7018, caller); store(base + 0x7038, child);
        std::memcpy(reinterpret_cast<void *>(base + 0x75dc), "FDKssConstant", 14);
        std::memcpy(reinterpret_cast<void *>(base + 0x75f0), "Child", 6);
        store(base + 0x7068, std::uint32_t{0}); store(base + 0x706c, std::uint32_t{0});
        const unsigned char nested_body[]{10,4,0,4,0,0,0,0,0,4,19,2,0,0,9,4,12,1,0,13};
        std::memcpy(reinterpret_cast<void *>(base + 0x7200), nested_body, sizeof(nested_body));
        std::memcpy(reinterpret_cast<void *>(base + 0x7400), default_body, sizeof(default_body));
        pure_args[2].integer = 5;
        check(!sky1_scripts::preview_pure(&profile, pure_args, 3, capture, &result, error, sizeof(error)) && result.integer == 5);
        pure_rejected(3, "VM profile does not match");
        store(base + 0x703c, std::uint32_t{0x101}); // Verified diagnostic-only callee flag.
        check(!sky1_scripts::preview_pure(&profile, pure_args, 3, capture, &result, error, sizeof(error)) && result.integer == 5);
        store(base + 0x701c, std::uint32_t{0x101}); store(base + 0x7028, std::uint32_t{1}); store(base + 0x702c, std::uint32_t{1600});
        check(!sky1_scripts::preview_pure(&profile, pure_args, 3, capture, &result, error, sizeof(error)) && result.integer == 5);
        store(base + 0x702c, std::uint32_t{2044}); pure_rejected(3, "function metadata"); // Extra-record table bounds.
        store(base + 0x702c, std::uint32_t{1600});
        store(base + 0x703c, std::uint32_t{0x201}); pure_rejected(3, "bounded pure"); // Unknown callee flag.
        store(base + 0x703c, std::uint32_t{1}); store(base + 0x706c, std::uint32_t{4}); pure_rejected(3, "bounded pure"); // Reference callee.
        store(base + 0x706c, std::uint32_t{0}); store(base + 0x7400, static_cast<unsigned char>(0x24)); pure_rejected(3, "bounded pure");
        store(base + 0x7400, static_cast<unsigned char>(10)); store(base + 0x703c, std::uint32_t{2}); pure_rejected(3, "bounded pure"); // Wrong argument count.
        store(base + 0x6008, std::uint32_t{128}); store(base + 0x7008, std::uint32_t{1});
        store(base + 0x7018, function); store(base + 0x7038, static_cast<unsigned char>(13));
        std::memcpy(reinterpret_cast<void *>(base + 0x7042), "FDKssConstant", 14);

        store(base + 0x7030, std::uint32_t{0}); rows.clear();
        check(sky1_scripts::function_at(&profile, function_args, 2, capture_text, &rows, error, sizeof(error)) != 0);
        std::thread foreign([&] { denied = sky1_scripts::names(&profile, page, 3, capture_text, &rows, error, sizeof(error)) != 0; }); foreign.join(); check(denied);
        // Variable metadata is an 8-byte pair; runtime values are separate tagged DWORDs.
        store(base + 0x7010, std::uint32_t{1});
        store(base + 0x6030, base + 0x7000); store(base + 0x6040, base + 0x7060);
        store(base + 0x6048, base + 0x6100); store(base + 0x6050, std::uint32_t{1});
        store(base + 0x7060, std::array<std::uint32_t, 2>{0xc000006e, 0xffffffff});
        std::memcpy(reinterpret_cast<void *>(base + 0x706e), "TestVar", 8);
        store(base + 0x6100, std::uint32_t{0x40000005});
        fdkss_value variable_page[]{string("fixture"), page[0], page[1]}; rows.clear();
        check(!sky1_scripts::variables(&profile, variable_page, 3, capture_text, &rows, error, sizeof(error)) && rows == std::vector<std::string>{"-1", "TestVar"});
        store(base + 0x7018, function); // Restore the intentionally corrupted checksum from the catalog guard test.
        store(base + 0x6038, base + 0x7018);
        const unsigned char global_body[]{7,0,0,0,0,10,0,13};
        std::memcpy(reinterpret_cast<void *>(base + 0x7038), global_body, sizeof(global_body));
        check(!sky1_scripts::preview_pure(&profile, pure_args, 2, capture, &result, error, sizeof(error)) && result.integer == 5);
        pure_rejected(2, "VM profile does not match"); // Reaches execution only after bounded global preflight.
        store(base + 0x6100, std::uint32_t{0});
        check(!sky1_scripts::preview_pure(&profile, pure_args, 2, capture, &result, error, sizeof(error)) && result.type == FDKSS_NIL);
        store(base + 0x6100, std::uint32_t{0x8fe00000});
        check(!sky1_scripts::preview_pure(&profile, pure_args, 2, capture, &result, error, sizeof(error)) && result.number == 1.f);
        for (auto raw : {1u,0xc000006eu,0x9fe00000u}) {
            store(base + 0x6100, raw); pure_rejected(2, "bounded pure");
        }
        store(base + 0x6100, std::uint32_t{0x40000005});
        store(base + 0x7039, static_cast<unsigned char>(1)); pure_rejected(2, "bounded pure");
        store(base + 0x7039, static_cast<unsigned char>(0));
        store(base + 0x6050, std::uint32_t{2}); pure_rejected(2, "bounded pure");
        store(base + 0x6050, std::uint32_t{1});
        store(base + 0x6048, std::uintptr_t{0}); pure_rejected(2, "bounded pure");
        store(base + 0x6048, base + 0x6100);
        store(base + 0x7038, static_cast<unsigned char>(8)); pure_rejected(2, "bounded pure");
        store(base + 0x7038, static_cast<unsigned char>(13));
        fdkss_value variable_args[]{string("fixture"), string("TestVar")}; rows.clear();
        check(!sky1_scripts::variable_info(&profile, variable_args, 2, capture_text, &rows, error, sizeof(error)) &&
            rows == std::vector<std::string>{"0", "4294967295", "1073741829", "5"});
        store(base + 0x6100, std::uint32_t{0x7fffffff});
        check(!sky1_scripts::variable_info(&profile, variable_args, 2, capture, &result, error, sizeof(error)) && result.integer == -1);
        fdkss_value variable_write[]{variable_args[0], variable_args[1], {}};
        variable_write[1] = string(""); variable_write[2].type = FDKSS_INTEGER;
        check(sky1_scripts::set_variable(&profile, variable_write, 3, capture, &result, error, sizeof(error)) != 0);
        variable_write[1] = string("TestVar"); ++variable_write[1].string_size;
        check(sky1_scripts::set_variable(&profile, variable_write, 3, capture, &result, error, sizeof(error)) != 0);
        variable_write[1] = variable_args[1];
        variable_write[2].type = FDKSS_INTEGER; variable_write[2].integer = -536870912;
        check(!sky1_scripts::set_variable(&profile, variable_write, 3, capture, &result, error, sizeof(error)) && result.integer == -536870912);
        variable_write[2].integer = 536870911;
        check(!sky1_scripts::set_variable(&profile, variable_write, 3, capture, &result, error, sizeof(error)) && result.integer == 536870911);
        for (const auto invalid : {-536870913LL, 536870912LL}) {
            variable_write[2].integer = invalid;
            check(sky1_scripts::set_variable(&profile, variable_write, 3, capture, &result, error, sizeof(error)) != 0);
            check(!sky1_scripts::variable_info(&profile, variable_args, 2, capture, &result, error, sizeof(error)) && result.integer == 536870911);
        }
        variable_write[2].type = FDKSS_NUMBER; variable_write[2].number = 1;
        check(sky1_scripts::set_variable(&profile, variable_write, 3, capture, &result, error, sizeof(error)) != 0);
        store(base + 0x6100, std::uint32_t{0x8fe00000}); // VM-encoded 1.0f.
        check(!sky1_scripts::variable_info(&profile, variable_args, 2, capture, &result, error, sizeof(error)) && result.type == FDKSS_NUMBER && result.number == 1);
        variable_write[2].number = 1.23456789;
        const auto encoded_float = (std::bit_cast<std::uint32_t>(static_cast<float>(variable_write[2].number)) >> 2) | 0x80000000;
        const auto decoded_float = std::bit_cast<float>(encoded_float << 2);
        check(!sky1_scripts::set_variable(&profile, variable_write, 3, capture, &result, error, sizeof(error)) && result.number == decoded_float);
        check(!sky1_scripts::variable_info(&profile, variable_args, 2, capture, &result, error, sizeof(error)) && result.number == decoded_float);
        for (const auto invalid : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 1e300}) {
            variable_write[2].number = invalid;
            check(sky1_scripts::set_variable(&profile, variable_write, 3, capture, &result, error, sizeof(error)) != 0);
            check(!sky1_scripts::variable_info(&profile, variable_args, 2, capture, &result, error, sizeof(error)) && result.number == decoded_float);
        }
        variable_write[2].type = FDKSS_INTEGER; variable_write[2].integer = 2;
        check(!sky1_scripts::set_variable(&profile, variable_write, 3, capture, &result, error, sizeof(error)) && result.type == FDKSS_NUMBER && result.number == 2);
        store(base + 0x6100, std::uint32_t{0x9fe00000}); // Infinity is rejected before emitting partial results.
        check(sky1_scripts::variable_info(&profile, variable_args, 2, capture, &result, error, sizeof(error)) != 0);
        store(base + 0x6100, std::uint32_t{0xc000006e}); rows.clear();
        check(!sky1_scripts::variable_info(&profile, variable_args, 2, capture_text, &rows, error, sizeof(error)) && rows.back() == "TestVar");
        check(sky1_scripts::set_variable(&profile, variable_write, 3, capture, &result, error, sizeof(error)) != 0);
        store(base + 0x6100, std::uint32_t{0xc0000080});
        check(sky1_scripts::variable_info(&profile, variable_args, 2, capture, &result, error, sizeof(error)) != 0);
        store(base + 0x6100, std::uint32_t{1});
        check(sky1_scripts::variable_info(&profile, variable_args, 2, capture, &result, error, sizeof(error)) != 0);
        store(base + 0x6100, std::uint32_t{0});
        check(!sky1_scripts::variable_info(&profile, variable_args, 2, capture, &result, error, sizeof(error)) && result.type == FDKSS_NIL);
        check(sky1_scripts::set_variable(&profile, variable_write, 3, capture, &result, error, sizeof(error)) != 0);
        variable_write[1] = string("missing");
        check(sky1_scripts::set_variable(&profile, variable_write, 3, capture, &result, error, sizeof(error)) != 0);
        variable_write[1] = string("TestVar");
        variable_args[1] = string("missing");
        check(!sky1_scripts::variable_info(&profile, variable_args, 2, capture, &result, error, sizeof(error)) && result.type == FDKSS_NIL);
        variable_args[1] = string("TestVar");
        store(base + 0x6050, std::uint32_t{2});
        check(sky1_scripts::variable_info(&profile, variable_args, 2, capture, &result, error, sizeof(error)) != 0);
        store(base + 0x7010, std::uint32_t{2}); store(base + 0x7068, std::array<std::uint32_t, 2>{0xc000006e, 0});
        // Rebuild the overlapping name after adding the second metadata record.
        std::memcpy(reinterpret_cast<void *>(base + 0x7070), "TestVar", 8);
        store(base + 0x7060, std::array<std::uint32_t, 2>{0xc0000070, 0xffffffff});
        store(base + 0x7068, std::array<std::uint32_t, 2>{0xc0000070, 0});
        check(sky1_scripts::variable_info(&profile, variable_args, 2, capture, &result, error, sizeof(error)) != 0 && std::strstr(error, "ambiguous"));
        store(base + 0x7010, std::uint32_t{1}); store(base + 0x6050, std::uint32_t{1});
        std::memcpy(reinterpret_cast<void *>(base + 0x4048), reinterpret_cast<void *>(base + 0x4000), 72);
        store(base + 0x3010, std::uint64_t{2}); store(base + 0x3018, std::uint64_t{2});
        check(sky1_scripts::variables(&profile, variable_page, 3, capture, &result, error, sizeof(error)) != 0 && std::strstr(error, "ambiguous"));
        check(sky1_scripts::set_variable(&profile, variable_write, 3, capture, &result, error, sizeof(error)) != 0 && std::strstr(error, "ambiguous"));
        store(base + 0x3010, std::uint64_t{1}); store(base + 0x3018, std::uint64_t{1});
        std::thread foreign_variable([&] { denied = sky1_scripts::variable_info(&profile, variable_args, 2, capture, &result, error, sizeof(error)) != 0; });
        foreign_variable.join(); check(denied);
        std::thread foreign_variable_write([&] { denied = sky1_scripts::set_variable(&profile, variable_write, 3, capture, &result, error, sizeof(error)) != 0; });
        foreign_variable_write.join(); check(denied);
        auto *readonly_value = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        check(readonly_value != nullptr); store(reinterpret_cast<std::uintptr_t>(readonly_value), std::uint32_t{0x40000005});
        DWORD old_protection = 0; check(VirtualProtect(readonly_value, 4096, PAGE_READONLY, &old_protection));
        store(base + 0x6048, reinterpret_cast<std::uintptr_t>(readonly_value));
        check(sky1_scripts::set_variable(&profile, variable_write, 3, capture, &result, error, sizeof(error)) != 0 && std::strstr(error, "not writable"));
        check(!sky1_scripts::variable_info(&profile, variable_args, 2, capture, &result, error, sizeof(error)) && result.integer == 5);
        check(VirtualFree(readonly_value, 0, MEM_RELEASE)); store(base + 0x6048, base + 0x6100);
        check(sky1_scripts::variable_info(nullptr, variable_args, 2, capture, &result, error, sizeof(error)) != 0);
        check(sky1_scripts::set_variable(nullptr, variable_write, 3, capture, &result, error, sizeof(error)) != 0);
        store(base + 0x8fefc8, base + 0x3000); store(base + 0x8ff030, fw); store(fw, base + 0x802d80);
        check(!sky1_scripts::variable_info(nullptr, variable_args, 2, capture, &result, error, sizeof(error)) && result.type == FDKSS_NIL);
        store(base + 0x6100, std::uint32_t{0x40000005});
        check(!sky1_scripts::set_variable(nullptr, variable_write, 3, capture, &result, error, sizeof(error)) && result.integer == 2);
        std::thread foreign_first_variable([&] { denied = sky1_scripts::variable_info(nullptr, variable_args, 2, capture, &result, error, sizeof(error)) != 0; });
        foreign_first_variable.join(); check(denied);
        std::thread foreign_first_write([&] { denied = sky1_scripts::set_variable(nullptr, variable_write, 3, capture, &result, error, sizeof(error)) != 0; });
        foreign_first_write.join(); check(denied);
        store(fw, base + 0xb1ad08); store(base + 0x8ff030, std::uintptr_t{0}); store(base + 0x8fefc8, std::uintptr_t{0});
        object(base + 0xa000, 0xc60e08, 0xb03a98, 0x1400, 0x2400, ".?AVManager@fieldmap@sora@@");
        object(base + 0xb000, 0x9008, 0xb06730, 0x1500, 0x2500, ".?AVScene@fieldmap@sora@@");
        object(base + 0xc000, 0x9010, 0xb06730, 0x1500, 0x2500, ".?AVScene@fieldmap@sora@@");
        object(base + 0x10000, 0x9018, 0xafdec8, 0x1600, 0x2600, ".?AVActor@chara@sora@@");
        object(base + 0x11000, 0x9020, 0xafdec8, 0x1600, 0x2600, ".?AVActor@chara@sora@@");
        // Current model is resolved afresh through the player controller.
        object(base+0x80000,0x9030,0xb05cb8,0x1d00,0x2d00,".?AVPlayer@fieldmap@sora@@");
        object(base+0x81000,0x9038,0xb18c80,0x1e00,0x2e00,".?AVModel@fdk@@");
        store(base+0xa660,base+0x80000);store(base+0x80060,base+0x10000);store(base+0x102e0,base+0x81000);
        store(base+0x81010,std::uint32_t{0x204c444d});store(base+0x81014,std::uint32_t{4});
        store(base+0x81258,std::uint64_t{13});store(base+0x81288,std::uint64_t{247});
        std::memset(reinterpret_cast<void*>(base+0x8101c),'m',63);store(base+0x8105b,'\0');
        fdkss_value model_args[]{string("player_model"),string("name")};rows.clear();
        check(!sky2::get_property(nullptr,model_args,2,capture_text,&rows,error,sizeof(error)) && rows[0].size()==63);
        fdkss_value character_model_args[]{string("player")};rows.clear();
        check(!sky2::character_model(nullptr,character_model_args,1,capture_text,&rows,error,sizeof(error)) &&
            rows.size()==4 && rows[0].size()==63 && rows[1]=="4" && rows[2]=="13" && rows[3]=="247");
        rows.clear();check(!sky2::actor_model(nullptr,character_model_args,1,capture_text,&rows,error,sizeof(error)) && rows.size()==4);
        std::thread foreign_character_model([&]{denied=sky2::character_model(nullptr,character_model_args,1,capture,&result,error,sizeof(error))!=0;});
        foreign_character_model.join();check(denied);
        std::thread foreign_actor_model([&]{denied=sky2::actor_model(nullptr,character_model_args,1,capture,&result,error,sizeof(error))!=0;});
        foreign_actor_model.join();check(denied);
        store(base+0x8105b,'m');check(sky2::get_property(nullptr,model_args,2,capture,&result,error,sizeof(error))!=0);
        store(base+0x8105b,'\0');model_args[1]=string("mesh_count");
        check(!sky2::get_property(nullptr,model_args,2,capture,&result,error,sizeof(error)) && result.integer==13);
        model_args[1]=string("node_count");
        check(!sky2::get_property(nullptr,model_args,2,capture,&result,error,sizeof(error)) && result.integer==247);
        std::thread foreign_model([&]{denied=sky2::get_property(nullptr,model_args,2,capture,&result,error,sizeof(error))!=0;});
        foreign_model.join();check(denied);
        for (const auto &item : std::array<std::pair<std::uintptr_t,std::uint64_t>,4>{{
            {base+0x102e0,0},{base+0x81000,base+0xb05cb8},{base+0x81258,10001},{base+0x81288,0x100000001ULL}}}) {
            const auto previous=*reinterpret_cast<std::uint64_t*>(item.first);store(item.first,item.second);
            check(!sky2::get_property(nullptr,model_args,2,capture,&result,error,sizeof(error)) && result.type==FDKSS_NIL);
            store(item.first,previous);
        }
        store(base+0x81014,std::uint32_t{5});rows.clear();
        check(!sky2::character_model(nullptr,character_model_args,1,capture_text,&rows,error,sizeof(error)) && rows[1]=="5");
        model_args[1]=string("format_version");
        check(!sky2::get_property(nullptr,model_args,2,capture,&result,error,sizeof(error)) && result.integer==5);
        store(base+0x81014,std::uint32_t{6});
        check(!sky2::get_property(nullptr,model_args,2,capture,&result,error,sizeof(error)) && result.type==FDKSS_NIL);
        store(base+0x81014,std::uint32_t{4});
        fdkss_value model_write[]{model_args[0],model_args[1],{}};model_write[2].type=FDKSS_INTEGER;model_write[2].integer=0;
        check(sky2::set_property(nullptr,model_write,3,capture,&result,error,sizeof(error))!=0);
        // Current scene/controller/player identities must follow Sky2's +660/+60 chain.
        store(base + 0xa108, base + 0xb000);
        std::memcpy(reinterpret_cast<void *>(base + 0xb030), "map\"\\\n", 7);
        store(base + 0x100d8, 12.0f); store(base + 0x100dc, -3.0f); store(base + 0x100e0, 0.5f);
        check(!sky2::describe("scene", snapshot, sizeof(snapshot)) &&
            std::strstr(snapshot, "\"map_id\":\"map\\\"\\\\\\u000a\""));
        check(!sky2::describe("player", snapshot, sizeof(snapshot)) &&
            std::strstr(snapshot, "\"position\":{\"x\":12,\"y\":-3,\"z\":0.5}"));
        check(!sky2::describe("objects", snapshot, sizeof(snapshot)) &&
            std::strstr(snapshot, ".?AVPlayer@fieldmap@sora@@") && std::strstr(snapshot, ".?AVScene@fieldmap@sora@@"));
        store(base + 0x100d8, std::numeric_limits<float>::infinity());
        check(!sky2::describe("player", snapshot, sizeof(snapshot)) &&
            std::strcmp(snapshot, "{\"available\":false}") == 0); store(base + 0x100d8, 12.0f);
        std::memset(reinterpret_cast<void *>(base + 0xb030), 'x', 32);
        check(!sky2::describe("scene", snapshot, sizeof(snapshot)) &&
            std::strcmp(snapshot, "{\"available\":false}") == 0);
        std::memcpy(reinterpret_cast<void *>(base + 0xb030), "fixture", 8);
        store(base + 0x80000, base + 0xb06730);
        check(!sky2::describe("player", snapshot, sizeof(snapshot)) &&
            std::strcmp(snapshot, "{\"available\":false}") == 0); store(base + 0x80000, base + 0xb05cb8);
        store(base+0xa660,std::uintptr_t{0});
        store(base + 0xa108, base + 0xb000);
        store(base + 0xb158, base + 0xe000); store(base + 0xb160, std::uint64_t{2}); store(base + 0xb168, std::uint64_t{2});
        store(base + 0xe000, base + 0x10000); store(base + 0xe008, base + 0x10000); // Duplicate actor is deduplicated.
        store(base + 0xb170, base + 0xf000); store(base + 0xb178, std::uint64_t{1}); store(base + 0xb180, std::uint64_t{1}); store(base + 0xf000, base + 0xc000);
        store(base + 0xc158, base + 0xe010); store(base + 0xc160, std::uint64_t{1}); store(base + 0xc168, std::uint64_t{1}); store(base + 0xe010, base + 0x11000);
        store(base + 0x100d8, 12.0f);
        sky2::refresh_scene(); rows.clear();
        check(!sky2::list_objects(nullptr, page, 3, capture_text, &rows, error, sizeof(error)) && rows.size() == 3);
        const auto token = rows[1];
        std::memcpy(reinterpret_cast<void *>(base + 0x10010), "Named character", 16);
        store(base + 0x10038, std::uint32_t{0xffffffff});
        fdkss_value identity_args[]{string(token.c_str()), string("name")};
        rows.clear();
        check(!sky2::get_property(nullptr, identity_args, 2, capture_text, &rows, error, sizeof(error)) && rows == std::vector<std::string>{"Named character"});
        std::memset(reinterpret_cast<void *>(base + 0x10010), 'a', 32);
        check(sky2::get_property(nullptr, identity_args, 2, capture_text, &rows, error, sizeof(error)) != 0);
        store(base + 0x1002f, '\0'); rows.clear();
        check(!sky2::get_property(nullptr, identity_args, 2, capture_text, &rows, error, sizeof(error)) && rows[0].size() == 31);
        identity_args[1] = string("entity_id");
        check(!sky2::get_property(nullptr, identity_args, 2, capture, &result, error, sizeof(error)) && result.integer == 4294967295LL);
        identity_args[1] = string("native_state_byte");
        for (const std::uint32_t raw : {0xffffffffu, 0x12345600u, 0xabcdef7fu}) {
            store(base + 0x1016c, raw);
            check(!sky2::get_property(nullptr, identity_args, 2, capture, &result, error, sizeof(error)) &&
                result.type == FDKSS_INTEGER && result.integer == (raw & 0xff));
        }
        for (const auto &[name, offset, number] : std::array<std::tuple<const char *, std::uintptr_t, float>,3>{{
            {"scale_x", 0x138, -2.0f}, {"scale_y", 0x13c, 0.0f}, {"scale_z", 0x140, 3.0f}}}) {
            store(base + 0x10000 + offset, number); identity_args[1] = string(name);
            check(!sky2::get_property(nullptr, identity_args, 2, capture, &result, error, sizeof(error)) &&
                result.type == FDKSS_NUMBER && result.number == number);
            fdkss_value scale_write[]{identity_args[0],identity_args[1],{}};
            scale_write[2].type=FDKSS_NUMBER;scale_write[2].number=1;
            check(sky2::set_property(nullptr, scale_write, 3, capture, &result, error, sizeof(error)) != 0);
            check(*reinterpret_cast<float *>(base + 0x10000 + offset) == number);
            store(base + 0x10000 + offset, std::numeric_limits<float>::infinity());
            check(sky2::get_property(nullptr, identity_args, 2, capture, &result, error, sizeof(error)) != 0);
            store(base + 0x10000 + offset, number);
            fdkss_value root_scale[]{string("player"), string(name)};
            store(base + 0xa660, base + 0x80000);
            check(!sky2::get_property(nullptr, root_scale, 2, capture, &result, error, sizeof(error)) && result.number == number);
            store(base + 0xa660, std::uintptr_t{0});
        }
        std::thread foreign_scale([&]{denied=sky2::get_property(nullptr,identity_args,2,capture,&result,error,sizeof(error))!=0;});
        foreign_scale.join();check(denied);
        fdkss_value absent_scale[]{string("player"),string("scale_z")};
        check(!sky2::get_property(nullptr,absent_scale,2,capture,&result,error,sizeof(error)) && result.type==FDKSS_NIL);
        absent_scale[0]=string("framework");
        check(sky2::get_property(nullptr,absent_scale,2,capture,&result,error,sizeof(error))!=0);
        identity_args[1] = string("native_state_flags");store(base + 0x10170, std::uint32_t{0xffffffff});
        check(!sky2::get_property(nullptr, identity_args, 2, capture, &result, error, sizeof(error)) && result.integer == 4294967295LL);
        fdkss_value state_write[]{identity_args[0], identity_args[1], {}};state_write[2].type=FDKSS_INTEGER;
        for (const char *field : {"native_state_byte", "native_state_flags"}) {
            state_write[1]=string(field);
            check(sky2::set_property(nullptr,state_write,3,capture,&result,error,sizeof(error))!=0);
            identity_args[1]=string(field);
            std::thread foreign_state([&]{denied=sky2::get_property(nullptr,identity_args,2,capture,&result,error,sizeof(error))!=0;});
            foreign_state.join();check(denied);
        }
        identity_args[0]=string("framework");
        check(sky2::get_property(nullptr,identity_args,2,capture,&result,error,sizeof(error))!=0);
        identity_args[0]=string(token.c_str());
        fdkss_value actor_args[]{string(token.c_str()), string("position_x")};
        check(!sky2::get_property(nullptr, actor_args, 2, capture, &result, error, sizeof(error)) && result.number == 12);
        store(base + 0x10108, 0.0f); store(base + 0x1010c, 0.0f);
        store(base + 0x10110, 0.0f); store(base + 0x10114, -1.0f);
        actor_args[1] = string("yaw_radians");
        check(!sky2::get_property(nullptr, actor_args, 2, capture, &result, error, sizeof(error)) && result.type == FDKSS_NUMBER && std::abs(result.number) < 0.00001);
        store(base + 0x10108, 0.1f);
        check(sky2::get_property(nullptr, actor_args, 2, capture, &result, error, sizeof(error)) != 0);
        store(base + 0x10108, 0.0f); actor_args[1] = string("position_x");
        fdkss_value coordinate_args[]{actor_args[0], actor_args[1], {}};
        coordinate_args[2].type = FDKSS_NUMBER; coordinate_args[2].number = 13;
        std::thread foreign_write([&] { denied = sky2::set_property(nullptr, coordinate_args, 3, capture, &result, error, sizeof(error)) != 0; });
        foreign_write.join(); check(denied);
        fdkss_value transform_args[]{actor_args[0], string("get_transform")};
        std::thread foreign_transform([&] { denied = sky2::call_method(nullptr, transform_args, 2, capture, &result, error, sizeof(error)) != 0; });
        foreign_transform.join(); check(denied && std::strstr(error, "update thread"));
        // Same root, removed actor: membership refresh expires its reference.
        store(base + 0xb160, std::uint64_t{0}); sky2::refresh_scene();
        check(sky2::get_property(nullptr, identity_args, 2, capture, &result, error, sizeof(error)) != 0);
        check(sky2::get_property(nullptr, actor_args, 2, capture, &result, error, sizeof(error)) != 0);
        check(sky2::set_property(nullptr, coordinate_args, 3, capture, &result, error, sizeof(error)) != 0);
        check(sky2::call_method(nullptr, transform_args, 2, capture, &result, error, sizeof(error)) != 0);
        fdkss_value placement_args[]{actor_args[0], string("set_placement"), {}, {}, {}, {}};
        for (int index = 2; index < 6; ++index) placement_args[index].type = FDKSS_NUMBER;
        check(sky2::call_method(nullptr, placement_args, 6, capture, &result, error, sizeof(error)) != 0);
        check(sky2::character_conditions(nullptr,actor_args,1,capture,&result,error,sizeof(error))!=0 && std::strstr(error,"stale"));
        fdkss_value expired_status[]{actor_args[0],{}};expired_status[1].type=FDKSS_INTEGER;expired_status[1].integer=7;
        check(sky2::character_status_value(nullptr,expired_status,2,capture,&result,error,sizeof(error))!=0);

        check(sky2::character_resources(nullptr,actor_args,1,capture,&result,error,sizeof(error))!=0 && std::strstr(error,"stale"));
        // Restore membership, then replace the root. Scene epoch still expires the old token.
        store(base + 0xb160, std::uint64_t{2}); sky2::refresh_scene();
        store(base + 0xa108, base + 0xc000); sky2::refresh_scene();
        check(sky2::get_property(nullptr, actor_args, 2, capture, &result, error, sizeof(error)) != 0);
        // A cyclic child graph fails instead of recursing indefinitely.
        store(base + 0xc170, base + 0xf000); store(base + 0xc178, std::uint64_t{1}); store(base + 0xc180, std::uint64_t{1}); sky2::refresh_scene(); rows.clear();
        check(sky2::list_objects(nullptr, page, 3, capture_text, &rows, error, sizeof(error)) != 0 && rows.empty());
        std::puts("Sky 2 layout, thread guards, write validation and restoration passed"); return 0;
    } catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
