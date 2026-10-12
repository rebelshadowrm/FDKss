#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "console_input.h"
#include "imgui.h"
#include "MinHook.h"
#include <atomic>
#include <array>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <intrin.h>

#if defined(FDKSS_TEST_CONSOLE_INPUT)
extern std::uintptr_t console_input_test_base();
extern SHORT WINAPI console_input_test_mouse_state(int);
extern BOOL WINAPI console_input_test_cursor(POINT *);
extern bool console_input_test_focused();
#endif
namespace console_input {
namespace {
struct Profile { std::uintptr_t global, table, handler, mouse_iat, cursor_iat, cursor_call; std::array<std::uintptr_t, 5> mouse_calls; };
constexpr Profile sky1{0x8ff058, 0x806810, 0x521a80, 0x773498, 0x773470, 0x568785, {0x5687c5,0x568801,0x56883e,0x56887b,0x5688c9}};
constexpr Profile sky2{0xc60ee8, 0xb1edc8, 0x65a120, 0x8bb5d8, 0x8bb5f0, 0x6a43a5, {0x6a43e5,0x6a4421,0x6a445e,0x6a449b,0x6a44e9}};
Profile profile{};
using Raw = void (__fastcall *)(void *, const RAWKEYBOARD *);
Raw original = nullptr;
void *target = nullptr;
std::array<unsigned char, 16> patch{};
std::atomic<bool> enabled{false}, wanted{false}, active{false}, close_requested{false};
std::atomic<std::uint64_t> blocked{0};
std::array<std::atomic<bool>, 256> held_by_console{};
using MouseState = SHORT (WINAPI *)(int);
MouseState mouse_original = nullptr;
using Cursor = BOOL (WINAPI *)(POINT *);
Cursor cursor_original = nullptr;
std::atomic<LONG> capture_x{0}, capture_y{0};
constexpr std::array<int, 5> mouse_keys{VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2};
std::array<std::atomic<bool>, 5> held_mouse{};
std::atomic<HWND> window{nullptr};
std::atomic<WNDPROC> previous{nullptr};
std::mutex mutex;
struct Event { UINT message; WPARAM key; LPARAM flags; int modifiers; };
std::array<Event, 256> events{};
std::size_t count = 0;
bool reset = true;
std::uintptr_t base() {
#if defined(FDKSS_TEST_CONSOLE_INPUT)
    return console_input_test_base();
#else
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
#endif
}
std::uintptr_t pointer(std::uintptr_t address) {
    std::uintptr_t value = 0; SIZE_T size = 0;
    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(address), &value, sizeof(value), &size);
    return size == sizeof(value) ? value : 0;
}
void clear() { std::lock_guard lock(mutex); count = 0; reset = true; }
bool game_focused() {
#if defined(FDKSS_TEST_CONSOLE_INPUT)
    return console_input_test_focused();
#else
    const auto foreground = GetForegroundWindow();
    if (const auto game_window = window.load(std::memory_order_acquire)) return foreground == game_window;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
#endif
}
SHORT WINAPI mouse_state(int key) {
    const auto value = mouse_original(key);
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (!enabled.load(std::memory_order_acquire)) return value;
    for (std::size_t index = 0; index < mouse_keys.size(); ++index) {
        if (key != mouse_keys[index] || caller != base() + profile.mouse_calls[index] + 6) continue;
        // BackgroundUpdates keeps the game ticking; its desktop-wide button
        // polling must not turn clicks in another application into game input.
        if (!game_focused()) {
            held_mouse[index].store((value & 0x8000) != 0);
            if (value) blocked.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        if (!(value & 0x8000)) { held_mouse[index].store(false); return value; }
        if (active.load(std::memory_order_acquire) || held_mouse[index].load()) {
            held_mouse[index].store(true); blocked.fetch_add(1, std::memory_order_relaxed); return 0;
        }
    }
    return value;
}
BOOL WINAPI mouse_cursor(POINT *point) {
    const auto result = cursor_original(point);
    if (result && point && enabled.load(std::memory_order_acquire) && active.load(std::memory_order_acquire) &&
        reinterpret_cast<std::uintptr_t>(_ReturnAddress()) == base() + profile.cursor_call + 6) {
        point->x = capture_x.load(); point->y = capture_y.load();
    }
    return result;
}
bool replace_mouse_api(std::uintptr_t rva, void *expected, void *replacement) {
    auto **slot = reinterpret_cast<void **>(base() + rva);
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protection)) return false;
    const auto old = InterlockedCompareExchangePointer(slot, replacement, expected);
    DWORD ignored = 0;
    const bool restored = VirtualProtect(slot, sizeof(void *), protection, &ignored) != 0;
    return old == expected && restored;
}
bool mouse_profile() {
#if defined(FDKSS_TEST_CONSOLE_INPUT)
    const auto expected = reinterpret_cast<std::uintptr_t>(&console_input_test_mouse_state);
    const auto expected_cursor = reinterpret_cast<std::uintptr_t>(&console_input_test_cursor);
#else
    const auto expected = reinterpret_cast<std::uintptr_t>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetAsyncKeyState"));
    const auto expected_cursor = reinterpret_cast<std::uintptr_t>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetCursorPos"));
#endif
    if (!expected || !expected_cursor || pointer(base() + profile.mouse_iat) != expected || pointer(base() + profile.cursor_iat) != expected_cursor) return false;
    std::array<std::uintptr_t, 6> calls{};
    std::copy(profile.mouse_calls.begin(), profile.mouse_calls.end(), calls.begin()); calls.back() = profile.cursor_call;
    for (const auto call : calls) {
        unsigned char bytes[6]{}; SIZE_T size = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(base() + call), bytes, sizeof(bytes), &size) || size != sizeof(bytes) || bytes[0] != 0xff || bytes[1] != 0x15) return false;
        std::int32_t displacement = 0; std::memcpy(&displacement, bytes + 2, 4);
        const auto iat = call == profile.cursor_call ? profile.cursor_iat : profile.mouse_iat;
        if (static_cast<std::intptr_t>(call + 6) + displacement != static_cast<std::intptr_t>(iat)) return false;
    }
    mouse_original = reinterpret_cast<MouseState>(expected);
    cursor_original = reinterpret_cast<Cursor>(expected_cursor); return true;
}
void __fastcall raw(void *input, const RAWKEYBOARD *key) {
    // Never swallow releases: a key held before console activation still reaches
    // the engine's existing release path. No engine keyboard arrays are written.
    if (enabled.load(std::memory_order_acquire) && key && key->VKey < held_by_console.size() &&
        reinterpret_cast<std::uintptr_t>(input) == pointer(base() + profile.global) &&
        pointer(reinterpret_cast<std::uintptr_t>(input)) == base() + profile.table) {
        auto &held = held_by_console[key->VKey];
        if (key->Flags & RI_KEY_BREAK) held.store(false);
        else if (active.load(std::memory_order_acquire) || held.load()) {
            held.store(true); blocked.fetch_add(1, std::memory_order_relaxed); return;
        }
    }
    original(input, key);
}
void enqueue(UINT message, WPARAM key, LPARAM flags, int modifiers = 0) {
    bool overflow = false;
    {
        std::lock_guard lock(mutex);
        if (count < events.size()) events[count++] = {message, key, flags, modifiers};
        else { count = 0; reset = true; overflow = true; }
    }
    if (overflow) { request(false); close_requested.store(true); }
}
int mouse_button(UINT message, WPARAM key) {
    switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: return 1;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: return 2;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        return HIWORD(key) == XBUTTON1 ? 3 : HIWORD(key) == XBUTTON2 ? 4 : -1;
    default: return -1;
    }
}
bool mouse_up(UINT message) {
    return message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP || message == WM_XBUTTONUP;
}
LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM key, LPARAM flags) {
    try {
        if (message == WM_KILLFOCUS || message == WM_NCDESTROY) {
            request(false); close_requested.store(true);
        }
        if (enabled.load(std::memory_order_acquire) && active.load(std::memory_order_acquire)) {
            if (message == WM_INPUT) {
                RAWINPUT input{}; UINT size = sizeof(input);
                const auto copied = GetRawInputData(reinterpret_cast<HRAWINPUT>(flags), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER));
                if (copied != static_cast<UINT>(-1) && copied >= sizeof(RAWINPUTHEADER) + sizeof(RAWMOUSE) &&
                    copied <= sizeof(input) && input.header.dwSize == copied && input.header.dwType == RIM_TYPEMOUSE) {
                    blocked.fetch_add(1, std::memory_order_relaxed);
                    // Foreground raw handles still require Windows cleanup. Keyboard
                    // and other raw devices retain the original FDK WndProc path.
                    return DefWindowProcW(hwnd, message, key, flags);
                }
            }
            const auto button = mouse_button(message, key);
            if (message == WM_MOUSEMOVE || message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL || button >= 0) {
                if (button >= 0 && !mouse_up(message)) held_mouse[button].store(true);
                enqueue(message, key, flags);
                return message == WM_XBUTTONDOWN || message == WM_XBUTTONUP || message == WM_XBUTTONDBLCLK ? TRUE : 0;
            }
            if (message == WM_KEYDOWN && key == VK_ESCAPE) {
                held_by_console[VK_ESCAPE].store(true);
                request(false); close_requested.store(true); return 0;
            }
            if (message == WM_KEYDOWN || message == WM_KEYUP || message == WM_CHAR) {
                if (message == WM_KEYDOWN && key < held_by_console.size()) held_by_console[key].store(true);
                const int modifiers = ((GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0) |
                    ((GetKeyState(VK_CONTROL) & 0x8000) ? 2 : 0) | ((GetKeyState(VK_MENU) & 0x8000) ? 4 : 0);
                enqueue(message, key, flags, modifiers);
                return 0;
            }
        }
    } catch (...) { active.store(false); wanted.store(false); close_requested.store(true); }
    // Uncaptured raw input retains the engine's original cleanup and consumers.
    return CallWindowProcW(previous.load(std::memory_order_acquire), hwnd, message, key, flags);
}
ImGuiKey translate(WPARAM key, LPARAM flags) {
    if (key >= 'A' && key <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + key - 'A');
    if (key >= '0' && key <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + key - '0');
    if (key >= VK_F1 && key <= VK_F12) return static_cast<ImGuiKey>(ImGuiKey_F1 + key - VK_F1);
    switch (key) {
    case VK_TAB: return ImGuiKey_Tab; case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow; case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow; case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown; case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End; case VK_INSERT: return ImGuiKey_Insert;
    case VK_DELETE: return ImGuiKey_Delete; case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space; case VK_RETURN: return (flags & (1LL << 24)) ? ImGuiKey_KeypadEnter : ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;
    default: return ImGuiKey_None;
    }
}
}
bool start(bool second) {
    if (original) return false;
    profile = second ? sky2 : sky1;
    const auto input = pointer(base() + profile.global);
    if (!input || pointer(input) != base() + profile.table) return false;
    if (!mouse_profile()) return false;
    target = reinterpret_cast<void *>(base() + profile.handler);
    constexpr unsigned char first[]{0x80,0xb9,0x92,0x7b,0,0,0};
    constexpr unsigned char tail1[]{0x4c,0x8b,0xca,0x4c,0x8b,0xd1};
    constexpr unsigned char tail2[]{0x4c,0x8b,0xd2,0x4c,0x8b,0xc9};
    unsigned char bytes[13]{}; SIZE_T size = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), target, bytes, sizeof(bytes), &size) || size != sizeof(bytes) ||
        std::memcmp(bytes, first, sizeof(first)) || std::memcmp(bytes + 7, second ? tail2 : tail1, 6)) return false;
    const auto initialization = MH_Initialize();
    if (initialization != MH_OK && initialization != MH_ERROR_ALREADY_INITIALIZED) return false;
    if (MH_CreateHook(target, reinterpret_cast<void *>(&raw), reinterpret_cast<void **>(&original)) != MH_OK) return false;
    enabled.store(true, std::memory_order_release);
    if (MH_EnableHook(target) != MH_OK) { enabled.store(false); return false; }
    std::memcpy(patch.data(), target, patch.size());
    if (!replace_mouse_api(profile.mouse_iat, reinterpret_cast<void *>(mouse_original), reinterpret_cast<void *>(&mouse_state))) {
        enabled.store(false); MH_DisableHook(target); return false;
    }
    if (!replace_mouse_api(profile.cursor_iat, reinterpret_cast<void *>(cursor_original), reinterpret_cast<void *>(&mouse_cursor))) {
        enabled.store(false); replace_mouse_api(profile.mouse_iat, reinterpret_cast<void *>(&mouse_state), reinterpret_cast<void *>(mouse_original));
        MH_DisableHook(target); return false;
    }
    return true;
}
bool stop() {
    request(false); enabled.store(false, std::memory_order_release);
    if (mouse_original && pointer(base() + profile.mouse_iat) == reinterpret_cast<std::uintptr_t>(&mouse_state) &&
        !replace_mouse_api(profile.mouse_iat, reinterpret_cast<void *>(&mouse_state), reinterpret_cast<void *>(mouse_original))) return false;
    if (cursor_original && pointer(base() + profile.cursor_iat) == reinterpret_cast<std::uintptr_t>(&mouse_cursor) &&
        !replace_mouse_api(profile.cursor_iat, reinterpret_cast<void *>(&mouse_cursor), reinterpret_cast<void *>(cursor_original))) return false;
    const auto hwnd = window.load();
    if (hwnd && previous.load() && IsWindow(hwnd) &&
        GetWindowLongPtrW(hwnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&procedure)) {
        SetLastError(0);
        const auto replaced = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous.load()));
        if (!replaced && GetLastError()) return false;
    }
    if (original && !std::memcmp(target, patch.data(), patch.size())) {
        const auto result = MH_DisableHook(target);
        if (result != MH_OK && result != MH_ERROR_DISABLED) return false;
    }
    // Keep previous WndProc and trampoline callable if another mod chains us.
    return true;
}
bool attach_window(void *value) {
    if (!enabled.load()) return false;
    const auto hwnd = static_cast<HWND>(value);
    DWORD pid = 0;
    if (GetWindowThreadProcessId(hwnd, &pid) != GetCurrentThreadId() || pid != GetCurrentProcessId()) return false;
    if (window.load()) return window.load() == hwnd && previous.load() != nullptr;
    const auto candidate = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(hwnd, GWLP_WNDPROC));
    if (!candidate) return false;
    previous.store(candidate, std::memory_order_release); window = hwnd;
    SetLastError(0);
    const auto replaced = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&procedure));
    if (!replaced && GetLastError()) { window = nullptr; previous.store(nullptr); return false; }
    previous.store(reinterpret_cast<WNDPROC>(replaced), std::memory_order_release);
    return true;
}
void request(bool edit) {
    wanted.store(edit && enabled.load(), std::memory_order_release);
    if (!edit) { active.store(false, std::memory_order_release); clear(); }
}
void poll(bool focused, const std::array<bool, 256> &keys) {
#if !defined(FDKSS_TEST_CONSOLE_INPUT)
    if (window.load() && GetForegroundWindow() != window.load()) focused = false;
#endif
    if (!focused) { if (wanted.load()) close_requested.store(true); request(false); return; }
    if (!wanted.load() || !previous.load() || !enabled.load()) return;
    if (!active.load()) {
        // Wait for all keys, including the opening hotkey, to be released. This
        // avoids inheriting movement held by FDK before the console opened.
        for (int key = 1; key < 255; ++key) if (keys[key]) return;
        POINT point{};
        if (!cursor_original || !cursor_original(&point)) return;
        capture_x.store(point.x); capture_y.store(point.y);
        clear(); active.store(true, std::memory_order_release);
    }
}
bool requested() { return wanted.load(); }
bool editing() { return active.load(); }
bool hooked() { return enabled.load(); }
bool take_close() { return close_requested.exchange(false); }
std::uint64_t suppressed() { return blocked.load(); }
void feed(ImGuiIO &io) {
    std::lock_guard lock(mutex);
    if (reset) { io.AddInputCharacterUTF16(0); io.ClearEventsQueue(); io.ClearInputKeys(); io.ClearInputMouse(); reset = false; }
    for (std::size_t index = 0; index < count; ++index) {
        const auto &event = events[index];
        if (event.message == WM_KEYDOWN || event.message == WM_KEYUP || event.message == WM_CHAR) {
            io.AddKeyEvent(ImGuiMod_Shift, (event.modifiers & 1) != 0);
            io.AddKeyEvent(ImGuiMod_Ctrl, (event.modifiers & 2) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (event.modifiers & 4) != 0);
        }
        const auto button = mouse_button(event.message, event.key);
        if (event.message == WM_MOUSEMOVE) io.AddMousePosEvent(static_cast<float>(static_cast<short>(LOWORD(event.flags))), static_cast<float>(static_cast<short>(HIWORD(event.flags))));
        else if (button >= 0) io.AddMouseButtonEvent(button, !mouse_up(event.message));
        else if (event.message == WM_MOUSEWHEEL || event.message == WM_MOUSEHWHEEL) {
            const auto amount = static_cast<float>(static_cast<short>(HIWORD(event.key))) / WHEEL_DELTA;
            io.AddMouseWheelEvent(event.message == WM_MOUSEHWHEEL ? -amount : 0, event.message == WM_MOUSEWHEEL ? amount : 0);
        }
        else if (event.message == WM_CHAR) io.AddInputCharacterUTF16(static_cast<ImWchar16>(event.key));
        else {
            const auto key = translate(event.key, event.flags);
            if (key != ImGuiKey_None) io.AddKeyEvent(key, event.message == WM_KEYDOWN);
        }
    }
    count = 0;
    io.MouseDrawCursor = active.load(std::memory_order_acquire);
    if (io.MouseDrawCursor) {
        POINT point{};
        if (GetCursorPos(&point) && ScreenToClient(window.load(), &point)) io.AddMousePosEvent(static_cast<float>(point.x), static_cast<float>(point.y));
        for (std::size_t index = 0; index < mouse_keys.size(); ++index) {
#if defined(FDKSS_TEST_CONSOLE_INPUT)
            const auto state = console_input_test_mouse_state(mouse_keys[index]);
#else
            const auto state = GetAsyncKeyState(mouse_keys[index]);
#endif
            io.AddMouseButtonEvent(static_cast<int>(index), (state & 0x8000) != 0);
        }
    }
}
}
