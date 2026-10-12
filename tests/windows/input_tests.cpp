#include "input.h"
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <vector>

int main() {
    try {
        keyboard_input::State state;
        std::array<bool, 256> keys{};
        std::vector<std::tuple<int, bool, int>> events;
        auto sample = [&](bool focused) { state.sample(focused, keys, [&](int k, bool down, int modifiers) {
            events.emplace_back(k, down, modifiers);
        }); };
        keys[65] = true; sample(false); sample(true);
        if (!events.empty()) throw std::runtime_error("background or initial held key emitted");
        keys[65] = false; sample(true);
        if (events != decltype(events){{65, false, 0}}) throw std::runtime_error("release missing");
        events.clear(); keys[0x11] = true; keys[65] = true; sample(true); sample(true);
        if (events != decltype(events){{0x11, true, 2}, {65, true, 2}}) throw std::runtime_error("edge/modifier/repeat handling failed");
        events.clear(); sample(false); keys[66] = true; sample(true);
        if (!events.empty()) throw std::runtime_error("focus regain delivered held keys");
        keys[0x78] = keys[0x79] = keys[0x7a] = true; sample(true);
        if (!events.empty()) throw std::runtime_error("reserved hotkeys emitted");
        keys[0x10] = keys[0x12] = true; sample(true);
        if (events.size() != 2 || std::get<2>(events[0]) != 7 || std::get<2>(events[1]) != 7)
            throw std::runtime_error("combined modifiers incorrect");
        std::cout << "Keyboard focus/edge tests passed\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
