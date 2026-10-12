#include "placement.h"
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <cstdio>
namespace {
std::array<std::byte, 0x200> actor{};
int calls = 0;
character_placement::Placement last{};
void require(bool value) { if (!value) throw std::runtime_error("placement assertion failed"); }
void __fastcall setter(void *object, const character_placement::Placement *pose) {
    require(object == actor.data()); ++calls; last = *pose;
    std::memcpy(actor.data() + 0xd8, &pose->x, 12);
    std::memcpy(actor.data() + 0xe8, &pose->x, 12);
    const float rotation[]{0, std::sin(pose->yaw_radians / 2), 0, std::cos(pose->yaw_radians / 2)};
    std::memcpy(actor.data() + 0x108, rotation, sizeof(rotation));
}
int emit(void *sink, const fdkss_value *value) { *static_cast<fdkss_value *>(sink) = *value; return 0; }
}
int main() {
    try {
        char error[256]{}; fdkss_value result{}, args[4]{};
        for (int i = 0; i < 4; ++i) { args[i].type = FDKSS_NUMBER; args[i].number = i + 1; }
        auto invoke = [&](std::size_t count) { return character_placement::invoke(actor.data(), setter, args, count, emit, &result, error, sizeof(error)); };
        require(!invoke(4) && calls == 1 && last.flags == 0x02000000 && last.yaw_radians == 4 && result.type == FDKSS_NIL);
        require(invoke(3) && calls == 1);
        args[0].type = FDKSS_STRING; require(invoke(4) && calls == 1);
        args[0].type = FDKSS_NUMBER; args[0].number = std::numeric_limits<double>::quiet_NaN(); require(invoke(4) && calls == 1);
        args[0].number = 1000001; require(invoke(4) && calls == 1);
        args[0].number = 1; args[3].number = 7; require(invoke(4) && calls == 1);
        args[3].number = 1.2; require(!invoke(4));
        fdkss_value replacement{}; replacement.type = FDKSS_INTEGER; replacement.integer = 8;
        require(!character_placement::coordinate(actor.data(), setter, "position_y", replacement, emit, &result, error, sizeof(error)));
        require(last.x == 1 && last.y == 8 && last.z == 3 && std::abs(last.yaw_radians - 1.2f) < 0.00001f && result.number == 8);
        float previous[3]{}; std::memcpy(previous, actor.data() + 0xe8, sizeof(previous)); require(previous[1] == 8);
        require(!character_placement::yaw(actor.data(), emit, &result, error, sizeof(error)) && std::abs(result.number - 1.2) < 0.00001);
        replacement.type = FDKSS_NUMBER; replacement.number = -0.75;
        require(!character_placement::coordinate(actor.data(), setter, "yaw_radians", replacement, emit, &result, error, sizeof(error)));
        require(last.x == 1 && last.y == 8 && last.z == 3 && last.yaw_radians == -0.75f && result.number == -0.75);
        require(!character_placement::yaw(actor.data(), emit, &result, error, sizeof(error)) && std::abs(result.number + 0.75) < 0.00001);
        float rotation[4]{}; std::memcpy(rotation, actor.data() + 0x108, sizeof(rotation));
        for (auto &v : rotation) v = -v;
        std::memcpy(actor.data() + 0x108, rotation, sizeof(rotation));
        require(!character_placement::yaw(actor.data(), emit, &result, error, sizeof(error)) && std::abs(result.number + 0.75) < 0.00001);
        const auto valid_calls = calls;
        replacement.number = 7;
        require(character_placement::coordinate(actor.data(), setter, "yaw_radians", replacement, emit, &result, error, sizeof(error)) && calls == valid_calls);
        replacement.number = std::numeric_limits<double>::infinity();
        require(character_placement::coordinate(actor.data(), setter, "yaw_radians", replacement, emit, &result, error, sizeof(error)) && calls == valid_calls);
        const int before = calls; float tilted = 0.1f; std::memcpy(actor.data() + 0x108, &tilted, 4);
        require(character_placement::yaw(actor.data(), emit, &result, error, sizeof(error)) && calls == before);
        require(character_placement::coordinate(actor.data(), setter, "yaw_radians", replacement, emit, &result, error, sizeof(error)) && calls == before);
        require(character_placement::coordinate(actor.data(), setter, "position_y", replacement, emit, &result, error, sizeof(error)) && calls == before);
        require(character_placement::coordinate(nullptr, setter, "position_x", replacement, emit, &result, error, sizeof(error)) && calls == before);
        std::array<unsigned char, 13> prefix{0x40,0x53,0x48,0x83,0xec,0x30,0xf6,0x42,0x03,0x01,0x48,0x8b,0xd9};
        require(character_placement::checked_target(reinterpret_cast<std::uintptr_t>(prefix.data())) != nullptr);
        prefix[0] = 0; require(!character_placement::checked_target(reinterpret_cast<std::uintptr_t>(prefix.data())));
        require(!character_placement::checked_target(1));
        puts("placement validation and pose preservation passed"); return 0;
    } catch (const std::exception &error) { puts(error.what()); return 1; }
}
