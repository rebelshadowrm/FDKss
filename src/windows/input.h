#pragma once
#include <array>
#include <utility>

namespace keyboard_input {
// Engine-thread state. Focus changes establish a baseline instead of delivering
// held desktop keys to a newly focused game.
class State {
    bool focused_ = false;
    std::array<bool, 256> previous_{};
public:
    template<class Emit> void sample(bool focused, const std::array<bool, 256> &keys, Emit emit) {
        if (!focused) { focused_ = false; previous_.fill(false); return; }
        if (!focused_) { focused_ = true; previous_ = keys; return; }
        const int modifiers = (keys[0x10] ? 1 : 0) | (keys[0x11] ? 2 : 0) | (keys[0x12] ? 4 : 0);
        const auto previous = std::exchange(previous_, keys);
        for (int key = 8; key < 255; ++key) {
            // F9/F10/F11 are reserved for console/reload/stop controls.
            if (key != 0x78 && key != 0x79 && key != 0x7a && keys[key] != previous[key]) emit(key, keys[key], modifiers);
        }
    }
};
}
