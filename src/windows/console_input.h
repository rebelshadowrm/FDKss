#pragma once
#include <array>
#include <cstdint>
struct ImGuiIO;
namespace console_input {
bool start(bool sky2);
bool stop();
bool attach_window(void *window);
void request(bool edit);
void poll(bool focused, const std::array<bool, 256> &keys);
bool requested();
bool editing();
bool hooked();
bool take_close();
void feed(ImGuiIO &);
std::uint64_t suppressed();
}
