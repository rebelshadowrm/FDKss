#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "console_input.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <source_location>
#include <string>
namespace {
std::uintptr_t image = 0;
unsigned legacy_characters = 0;
std::array<SHORT, 256> physical_mouse{};
POINT physical_cursor{55, 66};
bool physical_focus = true;
void check(bool value, const std::source_location &where = std::source_location::current()) { if (!value) throw std::runtime_error("console input isolation check failed at line " + std::to_string(where.line())); }
template<class T> void store(std::uintptr_t address, T value) { std::memcpy(reinterpret_cast<void *>(address), &value, sizeof(value)); }
LRESULT CALLBACK original_window(HWND hwnd, UINT message, WPARAM key, LPARAM flags) {
    if (message == WM_CHAR) ++legacy_characters;
    return DefWindowProcW(hwnd, message, key, flags);
}
LRESULT CALLBACK replacement(HWND hwnd, UINT message, WPARAM key, LPARAM flags) { return original_window(hwnd, message, key, flags); }
}
std::uintptr_t console_input_test_base() { return image; }
SHORT WINAPI console_input_test_mouse_state(int key) { return key >= 0 && key < 256 ? physical_mouse[key] : 0; }
BOOL WINAPI console_input_test_cursor(POINT *point) { if (!point) return FALSE; *point = physical_cursor; return TRUE; }
bool console_input_test_focused() { return physical_focus; }
int main(int argc, char **) {
    try {
        image = reinterpret_cast<std::uintptr_t>(VirtualAlloc(nullptr, 0xc61000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        check(image != 0);
        const bool second = argc == 1;
        const auto global = second ? 0xc60ee8 : 0x8ff058;
        const auto table = second ? 0xb1edc8 : 0x806810;
        const auto rva = second ? 0x65a120 : 0x521a80;
        const auto mouse_iat = second ? 0x8bb5d8 : 0x773498;
        const auto cursor_iat = second ? 0x8bb5f0 : 0x773470;
        const auto cursor_call = second ? 0x6a43a5 : 0x568785;
        const std::array<std::uintptr_t, 5> mouse_calls = second ?
            std::array<std::uintptr_t, 5>{0x6a43e5,0x6a4421,0x6a445e,0x6a449b,0x6a44e9} :
            std::array<std::uintptr_t, 5>{0x5687c5,0x568801,0x56883e,0x56887b,0x5688c9};
        const std::array<int, 5> mouse_keys{VK_LBUTTON,VK_RBUTTON,VK_MBUTTON,VK_XBUTTON1,VK_XBUTTON2};
        check(!console_input::start(second));
        const auto input = image + 0x3000;
        store(image + global, input); store(input, image + table);
        // The import must still resolve to the expected API, and every pinned
        // caller must contain the independently checked FF15 import call.
        check(!console_input::start(second));
        store(image + mouse_iat, reinterpret_cast<std::uintptr_t>(&console_input_test_mouse_state));
        store(image + cursor_iat, reinterpret_cast<std::uintptr_t>(&console_input_test_cursor));
        unsigned char cursor_caller[]{0x48,0x83,0xec,0x28,0x90,0x90,0x90,0x90,0x90,0xff,0x15,0,0,0,0,0x48,0x83,0xc4,0x28,0xc3};
        const auto cursor_relative = static_cast<std::int32_t>(cursor_iat - (cursor_call + 6));
        std::memcpy(cursor_caller + 11, &cursor_relative, 4);
        std::memcpy(reinterpret_cast<void *>(image + cursor_call - 9), cursor_caller, sizeof(cursor_caller));
        using PollCursor = BOOL (*)(POINT *);
        const auto poll_cursor = reinterpret_cast<PollCursor>(image + cursor_call - 9);
        using PollMouse = SHORT (*)();
        std::array<PollMouse, 5> poll_mouse{};
        for (std::size_t index = 0; index < mouse_calls.size(); ++index) {
            const auto call = mouse_calls[index];
            unsigned char caller[]{0x48,0x83,0xec,0x28,0xb9,0,0,0,0,0xff,0x15,0,0,0,0,0x48,0x83,0xc4,0x28,0xc3};
            const auto key = mouse_keys[index]; std::memcpy(caller + 5, &key, 4);
            const auto relative = static_cast<std::int32_t>(mouse_iat - (call + 6));
            std::memcpy(caller + 11, &relative, 4);
            std::memcpy(reinterpret_cast<void *>(image + call - 9), caller, sizeof(caller));
            poll_mouse[index] = reinterpret_cast<PollMouse>(image + call - 9);
        }
        std::array<unsigned char, 20> code{0x80,0xb9,0x92,0x7b,0,0,0,0x4c,0x8b,0xd2,0x4c,0x8b,0xc9,0xff,0x05,0,0,0,0,0xc3};
        if (!second) { code[9] = 0xca; code[12] = 0xd1; }
        const auto displacement = static_cast<std::int32_t>(0x2000 - (rva + 19));
        std::memcpy(code.data() + 15, &displacement, sizeof(displacement));
        std::memcpy(reinterpret_cast<void *>(image + rva), code.data(), code.size());
        DWORD protection = 0;
        check(VirtualProtect(reinterpret_cast<void *>(image + cursor_call - 9), mouse_calls.back() - cursor_call + 20, PAGE_EXECUTE_READ, &protection));
        check(VirtualProtect(reinterpret_cast<void *>(image + rva), code.size(), PAGE_EXECUTE_READ, &protection));
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(image + rva), code.size());
        check(console_input::start(second));
        using Raw = void (__fastcall *)(void *, const RAWKEYBOARD *);
        const auto raw = reinterpret_cast<Raw>(image + rva);
        auto &calls = *reinterpret_cast<unsigned *>(image + 0x2000);
        RAWKEYBOARD key{}; key.VKey = 'W';
        raw(reinterpret_cast<void *>(input), &key); check(calls == 1);
        WNDCLASSW klass{}; klass.hInstance = GetModuleHandleW(nullptr); klass.lpfnWndProc = original_window; klass.lpszClassName = L"FDKssConsoleInputFixture";
        check(RegisterClassW(&klass) != 0);
        const auto window = CreateWindowExW(0, klass.lpszClassName, L"Hidden input fixture", 0, 0, 0, 100, 100, nullptr, nullptr, klass.hInstance, nullptr);
        check(window != nullptr && console_input::attach_window(window));
        // Exact game call sites suppress desktop clicks while unfocused. Other
        // consumers of the imported API retain the physical state.
        using BackgroundMouseApi = SHORT (WINAPI *)(int);
        const auto background_mouse = reinterpret_cast<BackgroundMouseApi>(*reinterpret_cast<std::uintptr_t *>(image + mouse_iat));
        for (std::size_t index = 0; index < mouse_keys.size(); ++index) {
            physical_focus = false;
            physical_mouse[mouse_keys[index]] = static_cast<SHORT>(0x8001);
            check(poll_mouse[index]() == 0);
            check(background_mouse(mouse_keys[index]) == static_cast<SHORT>(0x8001));
            physical_focus = true;
            check(poll_mouse[index]() == 0); // Switching back while held is suppressed.
            physical_mouse[mouse_keys[index]] = 0; check(poll_mouse[index]() == 0);
            physical_mouse[mouse_keys[index]] = static_cast<SHORT>(0x8000);
            check(poll_mouse[index]() == static_cast<SHORT>(0x8000));
            physical_focus = false;
            physical_mouse[mouse_keys[index]] = 1; check(poll_mouse[index]() == 0);
            physical_mouse[mouse_keys[index]] = 0; check(poll_mouse[index]() == 0);
            physical_focus = true;
        }
        const auto background_suppressed = console_input::suppressed();
        SendMessageW(window, WM_CHAR, 'x', 0); check(legacy_characters == 1);
        std::array<bool, 256> keys{}; keys['W'] = true;
        console_input::request(true); console_input::poll(true, keys); check(!console_input::editing());
        raw(reinterpret_cast<void *>(input), &key); check(calls == 2);
        keys.fill(false); console_input::poll(true, keys); check(console_input::editing());
        raw(reinterpret_cast<void *>(input), &key); check(calls == 2 && console_input::suppressed() == background_suppressed + 1);
        SendMessageW(window, WM_KEYDOWN, 'A', 0); SendMessageW(window, WM_CHAR, 0x03b1, 0); SendMessageW(window, WM_KEYUP, 'A', 0);
        check(legacy_characters == 1);
        auto *gui = ImGui::CreateContext();
        console_input::feed(ImGui::GetIO());
        bool unicode = false, down = false, up = false;
        for (const auto &event : gui->InputEventsQueue) {
            if (event.Type == ImGuiInputEventType_Text && event.Text.Char == 0x03b1) unicode = true;
            if (event.Type == ImGuiInputEventType_Key && event.Key.Key == ImGuiKey_A) {
                if (event.Key.Down) down = true; else up = true;
            }
        }
        check(unicode && down && up);
        physical_cursor = {100, 200}; POINT cursor{};
        check(poll_cursor(&cursor) && cursor.x == 55 && cursor.y == 66);
        using CursorApi = BOOL (WINAPI *)(POINT *);
        const auto imported_cursor = reinterpret_cast<CursorApi>(*reinterpret_cast<std::uintptr_t *>(image + cursor_iat));
        check(imported_cursor(&cursor) && cursor.x == 100 && cursor.y == 200);
        for (std::size_t index = 0; index < mouse_keys.size(); ++index) {
            physical_mouse[mouse_keys[index]] = static_cast<SHORT>(0x8000);
            check(poll_mouse[index]() == 0);
        }
        // Only the exact callsites are masked; unrelated callers still see the
        // physical state through this same import.
        using MouseApi = SHORT (WINAPI *)(int);
        auto imported_mouse = reinterpret_cast<MouseApi>(*reinterpret_cast<std::uintptr_t *>(image + mouse_iat));
        check(imported_mouse(VK_LBUTTON) == static_cast<SHORT>(0x8000));
        SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(12, 34));
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(12, 34));
        SendMessageW(window, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
        console_input::feed(ImGui::GetIO());
        bool mouse_position = false, mouse_down = false, wheel = false;
        for (const auto &event : gui->InputEventsQueue) {
            if (event.Type == ImGuiInputEventType_MousePos && event.MousePos.PosX == 12 && event.MousePos.PosY == 34) mouse_position = true;
            if (event.Type == ImGuiInputEventType_MouseButton && event.MouseButton.Button == 0 && event.MouseButton.Down) mouse_down = true;
            if (event.Type == ImGuiInputEventType_MouseWheel && event.MouseWheel.WheelY == 1) wheel = true;
        }
        check(mouse_position && mouse_down && wheel && ImGui::GetIO().MouseDrawCursor);
        console_input::request(false);
        check(poll_cursor(&cursor) && cursor.x == 100 && cursor.y == 200);
        for (std::size_t index = 0; index < mouse_keys.size(); ++index) {
            check(poll_mouse[index]() == 0); // Closing while held cannot start a game click.
            physical_mouse[mouse_keys[index]] = 0; check(poll_mouse[index]() == 0);
            physical_mouse[mouse_keys[index]] = static_cast<SHORT>(0x8000);
            check(poll_mouse[index]() == static_cast<SHORT>(0x8000));
            physical_mouse[mouse_keys[index]] = 0;
        }
        // Verify actual ImGui click/release behavior, beyond queued event shape.
        ImGui::GetIO().DisplaySize = ImVec2(200, 200);
        ImGui::GetIO().DeltaTime = 1.0f / 60;
        ImGui::GetIO().IniFilename = nullptr;
        check(ImGui::GetIO().Fonts->Build());
        auto frame = [&] {
            console_input::feed(ImGui::GetIO());
            ImGui::GetIO().AddMousePosEvent(65, 60);
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(200, 200));
            ImGui::Begin("Mouse fixture", nullptr, ImGuiWindowFlags_NoTitleBar);
            ImGui::SetCursorScreenPos(ImVec2(50, 50));
            const bool pressed = ImGui::Button("Apply", ImVec2(80, 30));
            ImGui::End(); ImGui::Render(); return pressed;
        };
        console_input::request(true); console_input::poll(true, keys);
        check(!frame());
        check(!frame());
        physical_mouse[VK_LBUTTON] = static_cast<SHORT>(0x8000);
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(65, 60));
        check(!frame());
        physical_mouse[VK_LBUTTON] = 0;
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(65, 60));
        check(frame());
        console_input::request(false);
        // A held console key cannot start moving the game through repeat makes
        // after closing. Its release is forwarded; its next fresh press works.
        raw(reinterpret_cast<void *>(input), &key); check(calls == 2);
        key.Flags = RI_KEY_BREAK; raw(reinterpret_cast<void *>(input), &key); check(calls == 3);
        key.Flags = 0; raw(reinterpret_cast<void *>(input), &key); check(calls == 4);
        keys[VK_LBUTTON] = true;
        console_input::request(true); console_input::poll(true, keys); check(!console_input::editing());
        keys[VK_LBUTTON] = false;
        console_input::request(true); console_input::poll(true, keys);
        SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
        check(!console_input::editing() && console_input::take_close());
        key.VKey = VK_ESCAPE; raw(reinterpret_cast<void *>(input), &key); check(calls == 4);
        key.Flags = RI_KEY_BREAK; raw(reinterpret_cast<void *>(input), &key); check(calls == 5);
        console_input::request(true); console_input::poll(true, keys);
        SendMessageW(window, WM_KILLFOCUS, 0, 0);
        check(!console_input::editing() && !console_input::requested() && console_input::take_close());
        console_input::request(true); console_input::poll(true, keys);
        for (int index = 0; index < 257; ++index) SendMessageW(window, WM_CHAR, 'z', 0);
        check(!console_input::editing() && !console_input::requested() && console_input::take_close());
        console_input::feed(ImGui::GetIO()); check(gui->InputEventsQueue.empty());
        check(console_input::stop());
        check(GetWindowLongPtrW(window, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(original_window));
        check(std::memcmp(reinterpret_cast<void *>(image + rva), code.data(), code.size()) == 0);
        check(*reinterpret_cast<std::uintptr_t *>(image + mouse_iat) == reinterpret_cast<std::uintptr_t>(&console_input_test_mouse_state));
        check(*reinterpret_cast<std::uintptr_t *>(image + cursor_iat) == reinterpret_cast<std::uintptr_t>(&console_input_test_cursor));
        SendMessageW(window, WM_CHAR, 'y', 0); check(legacy_characters == 2);
        // Renderer may keep calling attach_window after a normal stop; it must
        // not reinstall a window subclass while the raw hook is disabled.
        check(!console_input::attach_window(window));
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(replacement));
        check(console_input::stop());
        check(GetWindowLongPtrW(window, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(replacement));
        DestroyWindow(window); UnregisterClassW(klass.lpszClassName, klass.hInstance);
        ImGui::DestroyContext(gui); VirtualFree(reinterpret_cast<void *>(image), 0, MEM_RELEASE);
        std::cout << "Console keyboard capture/release/focus/Unicode/restoration passed\n";
        return 0;
    } catch (const std::exception &failure) { std::cerr << failure.what() << '\n'; return 1; }
}
