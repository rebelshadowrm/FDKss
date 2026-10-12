#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include "overlay.h"
#include "console_input.h"
#include "../console_jobs.h"
#include "../browser.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

#if defined(FDKSS_TEST_RENDER_PROFILE)
extern std::uintptr_t render_test_base();
#endif
namespace developer_overlay {
namespace {
using Microsoft::WRL::ComPtr;
using Present = std::uint32_t (__fastcall *)(void *, bool);
struct Profile { std::uintptr_t global, table, present, swapchain; };
constexpr Profile sky1{0x8ff010, 0x805d18, 0x50bbe0, 0xea820};
constexpr Profile sky2{0xc60ea0, 0xb1e138, 0x640700, 0x3f8};
Profile profile{};
void **slot = nullptr;
std::atomic<Present> original{nullptr};
std::atomic<bool> enabled{false}, shown{false};
std::atomic<std::uint64_t> presents{0}, draws{0};
std::atomic<DWORD> render_thread{0};
bool installed = false;
std::mutex mutex;
Snapshot snapshot{};
console_jobs::Queue jobs;
object_browser::Browser browser;
char browser_root[256] = "player", class_filter[256]{}, edit_field[256] = "position_x";
object_browser::NumericEditor property_editor;
std::int64_t browser_ui_request = 0, browser_write_request = 0;
char browser_submission[128]{};
struct ConsoleEntry { std::int64_t id; std::string mod, source, output; int status; };
std::deque<ConsoleEntry> history;
std::deque<std::string> logs;
char target_mod[256] = "HelloFDK";
char source[8193] = "return fdk.engine.scene.map_id";
char submission_error[256]{};
bool previously_editing = false, console_hotkey_down = false, console_focused = false;
std::atomic<bool> focus_input{false};
std::atomic<bool> select_browser{false};
std::atomic<bool> select_calls{false};
bool focus_browser_pending = false; // Render thread only; survives tab activation's deferred frame.
std::atomic<bool> select_console{false};
char diagnostic[128]{};
unsigned last_width = 0, last_height = 0; // Last successful draw, protected by mutex.
ImGuiContext *gui = nullptr;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext> context;
IDXGISwapChain *current_swapchain = nullptr; // Compared only; not retained across resize.
auto last_frame = std::chrono::steady_clock::now();
std::uintptr_t base() {
#if defined(FDKSS_TEST_RENDER_PROFILE)
    return render_test_base();
#else
    return reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
#endif
}
bool memory(std::uintptr_t address, void *out, std::size_t size) {
    SIZE_T count = 0;
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void *>(address), out, size, &count) && count == size;
}
std::uintptr_t pointer(std::uintptr_t address) {
    std::uintptr_t value = 0; memory(address, &value, sizeof(value)); return value;
}
void shutdown_gui() {
    if (gui) {
        const auto previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(gui);
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext(gui);
        ImGui::SetCurrentContext(previous == gui ? nullptr : previous);
        gui = nullptr;
    }
    context.Reset(); device.Reset(); current_swapchain = nullptr;
}
bool initialize(IDXGISwapChain *chain) {
    DXGI_SWAP_CHAIN_DESC description{};
    if (FAILED(chain->GetDesc(&description))) return false;
    DWORD pid = 0; GetWindowThreadProcessId(description.OutputWindow, &pid);
    if (pid != GetCurrentProcessId()) return false;
    console_input::attach_window(description.OutputWindow);
    ComPtr<ID3D11Device> candidate;
    if (FAILED(chain->GetDevice(IID_PPV_ARGS(candidate.GetAddressOf())))) return false;
    if (gui && (current_swapchain != chain || candidate.Get() != device.Get())) shutdown_gui();
    if (!gui) {
        device = candidate; device->GetImmediateContext(context.GetAddressOf());
        if (!context) return false;
        const auto previous = ImGui::GetCurrentContext();
        gui = ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::GetIO().LogFilename = nullptr;
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::GetStyle().FontScaleMain = 1.2f;
        const bool initialized = ImGui_ImplDX11_Init(device.Get(), context.Get());
        if (!initialized) { ImGui::DestroyContext(gui); gui = nullptr; }
        ImGui::SetCurrentContext(previous);
        if (!initialized) { context.Reset(); device.Reset(); return false; }
        current_swapchain = chain;
        last_frame = std::chrono::steady_clock::now();
    }
    return true;
}
void draw(void *renderer) {
    auto *chain = reinterpret_cast<IDXGISwapChain *>(pointer(reinterpret_cast<std::uintptr_t>(renderer) + profile.swapchain));
    if (!chain || !initialize(chain)) { std::snprintf(diagnostic, sizeof(diagnostic), "D3D11 initialization unavailable"); return; }
    ComPtr<ID3D11Texture2D> buffer;
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(buffer.GetAddressOf())))) return;
    D3D11_TEXTURE2D_DESC desc{}; buffer->GetDesc(&desc);
    ComPtr<ID3D11RenderTargetView> target;
    if (FAILED(device->CreateRenderTargetView(buffer.Get(), nullptr, target.GetAddressOf()))) return;
    const auto previous_gui = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(gui);
    struct RestoreGui { ImGuiContext *previous; ~RestoreGui() { ImGui::SetCurrentContext(previous); } } restore_gui{previous_gui};
    auto &io = ImGui::GetIO();
    console_input::feed(io);
    io.DisplaySize = ImVec2(static_cast<float>(desc.Width), static_cast<float>(desc.Height));
    const auto now = std::chrono::steady_clock::now();
    io.DeltaTime = std::clamp(std::chrono::duration<float>(now - last_frame).count(), 0.001f, 0.1f);
    last_frame = now;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    if (console_input::editing() && io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_1)) { select_console.store(true); focus_input.store(true); }
        if (ImGui::IsKeyPressed(ImGuiKey_2)) select_browser.store(true);
        if (ImGui::IsKeyPressed(ImGuiKey_3)) select_calls.store(true);
    }
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Always);
    const bool editing = console_input::editing();
    ImGui::SetNextWindowSize(ImVec2(std::min(600.0f, std::max(200.0f, io.DisplaySize.x - 40)),
        std::min(editing ? 900.0f : 760.0f, std::max(200.0f, io.DisplaySize.y - 40))), ImGuiCond_Always);
    ImGui::Begin("FDKss developer console", nullptr,
        (editing ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoInputs) |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    if (editing) {
        ImGui::TextUnformatted("Ctrl+1 console | Ctrl+2 objects | Ctrl+3 calls");
        if (!previously_editing) ImGui::SetWindowFocus();
    } else if (console_input::requested()) ImGui::TextUnformatted("Release held keys and mouse buttons to enter the console.");
    else ImGui::TextWrapped("F9 opens the interactive console. Controller input remains with the game.");
    ImGui::Text("Map: %s", *snapshot.map ? snapshot.map : "unavailable");
    if (snapshot.player_available) ImGui::Text("Player: %.3f, %.3f, %.3f", snapshot.position[0], snapshot.position[1], snapshot.position[2]);
    else ImGui::TextUnformatted("Player: unavailable during loading");
    ImGui::Text("Time scale: %.3f", snapshot.time_scale);
    ImGui::Text("Engine updates: %llu", static_cast<unsigned long long>(snapshot.updates));
    ImGui::Text("Update thread: %lu   Render thread: %lu", static_cast<unsigned long>(snapshot.owner_thread), static_cast<unsigned long>(GetCurrentThreadId()));
    ImGui::Separator();
    if (ImGui::BeginTabBar("developer tools")) {
    if (ImGui::BeginTabItem("Console and log", nullptr, select_console.exchange(false) ? ImGuiTabItemFlags_SetSelected : 0)) {
    if (editing) {
        ImGui::TextUnformatted("Enter runs Lua; Ctrl+Enter inserts a line.");
        ImGui::InputText("Target mod", target_mod, sizeof(target_mod));
        const bool focus = focus_input.exchange(false);
        if (!previously_editing || focus) ImGui::SetKeyboardFocusHere();
        const bool run = ImGui::InputTextMultiline("Lua code", source, sizeof(source), ImVec2(-1, 90),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
        if (run) {
            try { jobs.submit(target_mod, source); *submission_error = 0; }
            catch (const std::exception &failure) { std::snprintf(submission_error, sizeof(submission_error), "%s", failure.what()); }
            focus_input.store(true);
        }
        if (*submission_error) ImGui::TextUnformatted(submission_error);
    }
    ImGui::TextUnformatted("Returned values appear below under Result.");
    ImGui::TextUnformatted("Console results");
    ImGui::BeginChild("results", ImVec2(0, 230));
    ImGui::PushTextWrapPos(0);
    if (history.empty()) ImGui::TextUnformatted("No queued requests yet.");
    for (const auto &entry : history) {
        ImGui::TextColored(entry.status == FDKSS_OK ? ImVec4(0.4f, 1, 0.5f, 1) : ImVec4(1, 0.4f, 0.4f, 1),
            "Request #%lld: %s (%s)", static_cast<long long>(entry.id), entry.status == FDKSS_OK ? "Succeeded" : "Lua error", entry.mod.c_str());
        ImGui::TextDisabled("Executed code:");
        ImGui::TextUnformatted(entry.source.c_str());
        ImGui::TextUnformatted(entry.status == FDKSS_OK ? "Result:" : "Error details:");
        ImGui::TextUnformatted(entry.output.empty() ? "Completed with empty output." : entry.output.c_str());
        ImGui::Separator();
    }
    ImGui::PopTextWrapPos();
    ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::TextUnformatted("Mod output log");
    ImGui::BeginChild("logs", ImVec2(0, 0));
    ImGui::PushTextWrapPos(0);
    for (const auto &line : logs) ImGui::TextUnformatted(line.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::EndTabItem();
    }
    const bool enter_browser = select_browser.exchange(false);
    if (enter_browser && editing) focus_browser_pending = true;
    if (ImGui::BeginTabItem("Objects and properties", nullptr, enter_browser ? ImGuiTabItemFlags_SetSelected : 0)) {
        auto submit = [](object_browser::Request request) {
            const bool write = request.kind == object_browser::Request::write;
            const auto id = browser.submit(std::move(request));
            if (!id) std::snprintf(browser_submission, sizeof(browser_submission), "Browser request queue is full or request is invalid");
            else { *browser_submission = 0; browser_ui_request = id; if (write) browser_write_request = id; }
        };
        const auto view = browser.snapshot();
        if (editing && focus_browser_pending) { ImGui::SetKeyboardFocusHere(); focus_browser_pending = false; }
        ImGui::InputText("Class filter", class_filter, sizeof(class_filter));
        if (ImGui::Button("First page")) submit({object_browser::Request::page, class_filter});
        ImGui::SameLine();
        ImGui::BeginDisabled(view.next < 0);
        if (ImGui::Button("Next page")) submit({object_browser::Request::page, class_filter, {}, view.next});
        ImGui::EndDisabled();
        ImGui::BeginChild("actor page", ImVec2(0, 140), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
        for (const auto &row : view.rows) {
            const auto label = row.name + "  " + row.type;
            ImGui::PushID(row.token.c_str());
            if (ImGui::Selectable(label.c_str())) {
                std::snprintf(browser_root, sizeof(browser_root), "%s", row.token.c_str());
                submit({object_browser::Request::inspect, row.token});
            }
            if (ImGui::IsItemFocused()) ImGui::SetTooltip("Address: %s", row.address.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (ImGui::InputText("Object/root", browser_root, sizeof(browser_root))) property_editor.ready = false;
        if (ImGui::Button("Read properties")) submit({object_browser::Request::inspect, browser_root});
        if(ImGui::BeginCombo("Command group",std::string_view(browser_root).starts_with("script_group:") ? browser_root : "Select group")) {
            for(unsigned group=0;group<28;++group) {
                char root[32]{},label[32]{};std::snprintf(root,sizeof(root),"script_group:%u",group);std::snprintf(label,sizeof(label),"Group %u",group);
                if(ImGui::Selectable(label,!std::strcmp(browser_root,root))) {
                    std::snprintf(browser_root,sizeof(browser_root),"%s",root);property_editor.ready=false;
                    std::snprintf(edit_field,sizeof(edit_field),"slot_count");
                    submit({object_browser::Request::inspect,root});
                }
            }
            ImGui::EndCombo();
        }
        if (*browser_submission) ImGui::TextWrapped("%s", browser_submission);
        if (!view.error.empty()) ImGui::TextWrapped("%s", view.error.c_str());
        if (browser_ui_request) {
            const auto completion = browser.lookup(browser_ui_request);
            const auto phase = completion.phase;
            const char *status = phase == object_browser::Phase::pending ? "Pending" :
                phase == object_browser::Phase::succeeded ? "Succeeded" :
                phase == object_browser::Phase::failed ? "Failed" :
                phase == object_browser::Phase::cancelled ? "Cancelled" : "Result expired";
            ImGui::Text("Browser request #%lld: %s", static_cast<long long>(browser_ui_request), status);
            if (!completion.error.empty()) ImGui::TextWrapped("%s", completion.error.c_str());
        }
        const bool group_view=std::string_view(view.root).starts_with("script_group:");
        ImGui::BeginChild("property values", ImVec2(0, group_view ? 150.0f : editing ? 130.0f : 100.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
        float value_column=250.0f;
        for(const auto &property : view.properties) {
            const auto label=property.name+(property.writable ? " (writable)" : " (read-only)");
            value_column=std::max(value_column,ImGui::CalcTextSize(label.c_str()).x+20.0f);
        }
        ImGui::TextWrapped("Snapshot of %s", view.root.c_str());
        for (const auto &property : view.properties) {
            const auto &value = property.value;
            ImGui::PushID(property.name.c_str());
            const auto label = property.name + (property.writable ? " (writable)" : " (read-only)");
            if (ImGui::Selectable(label.c_str(), property_editor.root == view.root && property_editor.field == property.name,
                ImGuiSelectableFlags_None, ImVec2(value_column-15.0f, 0))) {
                property_editor.select(view.root, property);
                std::snprintf(browser_root, sizeof(browser_root), "%s", view.root.c_str());
                std::snprintf(edit_field, sizeof(edit_field), "%s", property.name.c_str());
            }
            ImGui::SameLine();
            if (!property.error.empty()) ImGui::TextWrapped("unavailable: %s", property.error.c_str());
            else switch (value.type) {
            case FDKSS_NIL: ImGui::TextUnformatted("unavailable"); break;
            case FDKSS_BOOLEAN: ImGui::TextUnformatted(value.boolean ? "true" : "false"); break;
            case FDKSS_INTEGER: ImGui::Text("%lld", static_cast<long long>(value.integer)); break;
            case FDKSS_NUMBER: ImGui::Text("%.9g", value.number); break;
            case FDKSS_STRING: ImGui::TextWrapped("%s",value.text.c_str()); break;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        if (editing) {
            if (ImGui::InputText("Property field", edit_field, sizeof(edit_field))) property_editor.ready = false;
            if (ImGui::Button("Watch property")) submit({object_browser::Request::watch, browser_root, edit_field});
            ImGui::InputDouble("New value", &property_editor.number, 0, 0, "%.17g");
            const bool pending_write = browser_write_request && browser.lookup(browser_write_request).phase == object_browser::Phase::pending;
            ImGui::BeginDisabled(pending_write || !property_editor.can_apply(view, browser_root, edit_field));
            if (ImGui::Button("Apply value")) submit({object_browser::Request::write, browser_root, edit_field, 0, property_editor.number});
            ImGui::EndDisabled();
            if (pending_write) ImGui::TextUnformatted("Waiting for the engine to apply the value.");
            else if (!property_editor.can_apply(view, browser_root, edit_field))
                ImGui::TextWrapped("Select a writable numeric property above to fill the editor. Tab navigates; Enter selects or applies.");
        }
        ImGui::BeginChild("property watches", ImVec2(0, 100), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened);
        ImGui::TextUnformatted("Live watches (4 samples/second; expired references pause)");
        for (const auto &watch : view.watches) {
            ImGui::PushID(watch.root.c_str()); ImGui::PushID(watch.field.c_str());
            if (editing && ImGui::SmallButton("Remove")) submit({object_browser::Request::unwatch, watch.root, watch.field});
            if (editing) ImGui::SameLine();
            ImGui::Text("%s.%s:", watch.root.c_str(), watch.field.c_str()); ImGui::SameLine();
            if (watch.paused) ImGui::TextWrapped("Paused: %s", watch.error.c_str());
            else switch (watch.value.type) {
            case FDKSS_NIL: ImGui::TextUnformatted("nil"); break;
            case FDKSS_BOOLEAN: ImGui::TextUnformatted(watch.value.boolean ? "true" : "false"); break;
            case FDKSS_INTEGER: ImGui::Text("%lld", static_cast<long long>(watch.value.integer)); break;
            case FDKSS_NUMBER: ImGui::Text("%.9g", watch.value.number); break;
            case FDKSS_STRING: ImGui::TextUnformatted(watch.value.text.c_str()); break;
            }
            ImGui::PopID(); ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
    }
    const bool enter_calls=select_calls.exchange(false);
    static bool focus_calls_pending=false;
    if (enter_calls && editing) focus_calls_pending=true;
    if (ImGui::BeginTabItem("Actor calls",nullptr,enter_calls ? ImGuiTabItemFlags_SetSelected : 0)) {
        static int selected_method=0;
        static std::int64_t call_request=0;
        static char call_submission[128]{};
        constexpr const char *methods[]{"type_name","type_id","type_flags","get_transform"};
        ImGui::TextUnformatted("Read-only verified actor getters");
        if (focus_calls_pending && editing) {ImGui::SetKeyboardFocusHere();focus_calls_pending=false;}
        ImGui::InputText("Object/root##call",browser_root,sizeof(browser_root));
        ImGui::Combo("Method",&selected_method,methods,4);
        ImGui::BeginDisabled(browser.lookup(call_request).phase==object_browser::Phase::pending || !*browser_root);
        if (ImGui::Button("Invoke getter")) {
            call_request=browser.submit({object_browser::Request::invoke,browser_root,methods[selected_method]});
            if (!call_request) std::snprintf(call_submission,sizeof(call_submission),"Call queue full or request invalid");
            else *call_submission=0;
        }
        ImGui::EndDisabled();
        if (*call_submission) ImGui::TextWrapped("%s",call_submission);
        if (call_request) {
            const auto completion=browser.lookup(call_request);
            const char *phase=completion.phase==object_browser::Phase::pending ? "Pending" :
                completion.phase==object_browser::Phase::succeeded ? "Succeeded" :
                completion.phase==object_browser::Phase::failed ? "Failed" :
                completion.phase==object_browser::Phase::cancelled ? "Cancelled" : "Result expired";
            ImGui::Text("Call request #%lld: %s",static_cast<long long>(call_request),phase);
            if (!completion.error.empty()) ImGui::TextWrapped("%s",completion.error.c_str());
        }
        const auto view=browser.snapshot();
        if (!view.method_name.empty()) ImGui::TextWrapped("Last call: %s : %s",view.method_root.c_str(),view.method_name.c_str());
        if (!view.method_error.empty()) ImGui::TextWrapped("%s",view.method_error.c_str());
        if (view.method_name=="get_transform" && view.method_values.size()==16 && ImGui::BeginTable("Copied transform",4,ImGuiTableFlags_Borders)) {
            for (unsigned i=0;i<16;++i) {
                ImGui::TableNextColumn();ImGui::TextDisabled("m%u%u",i/4,i%4);
                ImGui::Text("%.7g",view.method_values[i].number);
            }
            ImGui::EndTable();
        } else {
            for (const auto &value : view.method_values) {
                if (value.type==FDKSS_STRING) ImGui::TextWrapped("%s",value.text.c_str());
                else if (value.type==FDKSS_INTEGER) ImGui::Text("%lld",static_cast<long long>(value.integer));
                else if (value.type==FDKSS_NUMBER) ImGui::Text("%.9g",value.number);
            }
        }
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    }
    previously_editing = editing;
    ImGui::End();
    ImGui::Render();
    std::array<ID3D11RenderTargetView *, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> previous_targets{};
    ComPtr<ID3D11DepthStencilView> previous_depth;
    context->OMGetRenderTargets(static_cast<UINT>(previous_targets.size()), previous_targets.data(), previous_depth.GetAddressOf());
    struct RestoreTargets {
        ID3D11DeviceContext *context;
        decltype(previous_targets) &targets;
        ID3D11DepthStencilView *depth;
        ~RestoreTargets() {
            context->OMSetRenderTargets(static_cast<UINT>(targets.size()), targets.data(), depth);
            for (auto *view : targets) if (view) view->Release();
        }
    } restore_targets{context.Get(), previous_targets, previous_depth.Get()};
    auto *view = target.Get(); context->OMSetRenderTargets(1, &view, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    last_width = desc.Width; last_height = desc.Height;
    draws.fetch_add(1, std::memory_order_relaxed);
    *diagnostic = 0;
    // All temporary backbuffer/view references and bindings release before Present.
}
std::uint32_t __fastcall present(void *renderer, bool sync) {
    presents.fetch_add(1, std::memory_order_relaxed);
    if (enabled.load(std::memory_order_acquire) && shown.load(std::memory_order_relaxed)) {
        std::lock_guard lock(mutex);
        if (enabled.load(std::memory_order_acquire) && shown.load(std::memory_order_relaxed)) {
            const DWORD thread = GetCurrentThreadId(); DWORD expected = 0;
            render_thread.compare_exchange_strong(expected, thread);
            if (render_thread.load() == thread && pointer(reinterpret_cast<std::uintptr_t>(renderer)) == base() + profile.table) {
                try { draw(renderer); }
                catch (...) { shown.store(false); console_input::request(false); std::snprintf(diagnostic, sizeof(diagnostic), "Overlay exception; hidden"); }
            }
        }
    }
    return original.load(std::memory_order_acquire)(renderer, sync);
}
}
bool start(bool second) {
    std::lock_guard lock(mutex);
    if (installed) return false;
    profile = second ? sky2 : sky1;
    const auto renderer = pointer(base() + profile.global);
    slot = reinterpret_cast<void **>(base() + profile.table + 28 * sizeof(void *));
    auto *target = reinterpret_cast<void *>(base() + profile.present);
    constexpr unsigned char expected[]{0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9};
    unsigned char actual[sizeof(expected)]{};
    if (!renderer || pointer(renderer) != base() + profile.table || pointer(reinterpret_cast<std::uintptr_t>(slot)) != reinterpret_cast<std::uintptr_t>(target) ||
        !memory(base() + profile.present, actual, sizeof(actual)) || std::memcmp(actual, expected, sizeof(expected))) return false;
    original.store(reinterpret_cast<Present>(target), std::memory_order_release);
    DWORD protection = 0, ignored = 0;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protection)) return false;
    enabled.store(true, std::memory_order_release);
    installed = InterlockedCompareExchangePointer(reinterpret_cast<void *volatile *>(slot), reinterpret_cast<void *>(&present), target) == target;
    VirtualProtect(slot, sizeof(void *), protection, &ignored);
    if (!installed) enabled.store(false, std::memory_order_release);
    if (installed) console_input::start(second);
    return installed;
}
bool stop() {
    jobs.close(FDKSS_ERROR, "Runtime stopped before this request executed");
    browser.close();
    std::lock_guard lock(mutex);
    enabled.store(false, std::memory_order_release); shown.store(false);
    if (!console_input::stop()) return false;
    if (installed) {
        DWORD protection = 0, ignored = 0;
        if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &protection)) return false;
        InterlockedCompareExchangePointer(reinterpret_cast<void *volatile *>(slot), reinterpret_cast<void *>(original.load()), reinterpret_cast<void *>(&present));
        const bool restored = VirtualProtect(slot, sizeof(void *), protection, &ignored) != FALSE;
        if (!restored) return false;
        installed = false;
    }
    shutdown_gui();
    last_width = last_height = 0;
    return true;
}
bool visible() { return shown.load(std::memory_order_relaxed); }
bool captures_keyboard() { return console_input::requested(); }
void bind_browser(bool second, fdkss_typed_native_fn get, fdkss_typed_native_fn list, fdkss_typed_native_fn set, fdkss_typed_native_fn invoke) {
    browser.bind(second, get, list, set, invoke);
}
void publish(const Snapshot &value) { std::lock_guard lock(mutex); snapshot = value; snapshot.map[sizeof(snapshot.map) - 1] = 0; }
void log(const char *mod, const char *message) {
    std::string line = "[" + std::string(mod).substr(0, 255) + "] " + std::string(message).substr(0, 1024);
    std::lock_guard lock(mutex);
    logs.push_back(std::move(line));
    if (logs.size() > 64) logs.pop_front();
}
void service(fdkss_runtime *runtime) {
    if (!runtime) return;
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    const bool focused = pid == GetCurrentProcessId();
    std::array<bool, 256> keys{};
    if (focused) for (int key = 1; key < 255; ++key) keys[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
    const bool down = focused && keys[VK_F9];
    if (focused && console_focused && down && !console_hotkey_down && console_input::hooked() &&
        !keys[VK_SHIFT] && !keys[VK_CONTROL] && !keys[VK_MENU]) {
        const bool edit = !console_input::requested();
        shown.store(edit); console_input::request(edit); if (edit) { focus_input.store(true); select_console.store(true); }
    }
    console_hotkey_down = down; console_focused = focused;
    console_input::poll(focused, keys);
    if (console_input::take_close()) shown.store(false);
    browser.service();
    const auto job = jobs.take();
    if (!job) return;
    char output[16384]{};
    const auto status = fdkss_execute(runtime, job->mod.c_str(), job->source.c_str(), output, sizeof(output));
    if (status != FDKSS_OK && !*output) std::snprintf(output, sizeof(output), "%s", fdkss_last_error(runtime));
    jobs.finish(job->id, status, output);
    std::lock_guard lock(mutex);
    history.push_back({job->id, job->mod, job->source.substr(0, 1024), std::string(output).substr(0, 4096), status});
    if (history.size() > 8) history.pop_front();
}
int FDKSS_CALL enqueue(void *, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if (count != 2 || args[0].type != FDKSS_STRING || args[1].type != FDKSS_STRING) {
        std::snprintf(error, capacity, "expected target mod and Lua source strings"); return 1;
    }
    try {
        fdkss_value value{}; value.type = FDKSS_INTEGER;
        value.integer = jobs.submit(std::string(args[0].string, args[0].string_size), std::string(args[1].string, args[1].string_size));
        return emit(sink, &value);
    } catch (const std::exception &failure) { std::snprintf(error, capacity, "%s", failure.what()); return 1; }
}
int FDKSS_CALL result(void *, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if (count != 1 || args[0].type != FDKSS_INTEGER || args[0].integer <= 0) {
        std::snprintf(error, capacity, "expected positive console request identifier"); return 1;
    }
    const auto saved = jobs.lookup(args[0].integer);
    if (saved.phase == console_jobs::Phase::unknown) { std::snprintf(error, capacity, "unknown console request identifier"); return 1; }
    fdkss_value value{}; value.type = FDKSS_INTEGER; value.integer = static_cast<int>(saved.phase);
    if (emit(sink, &value)) return 1;
    value.integer = saved.status; if (emit(sink, &value)) return 1;
    value.type = FDKSS_STRING; value.string = saved.text.data(); value.string_size = saved.text.size(); return emit(sink, &value);
}
int FDKSS_CALL browser_request(void *, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if ((count < 1 || count > 3) || args[0].type != FDKSS_STRING || args[0].string_size > 255 ||
        (!args[0].string && args[0].string_size) || (count == 2 && args[1].type != FDKSS_INTEGER) ||
        (count == 3 && (args[1].type != FDKSS_STRING || args[1].string_size > 255 ||
            (!args[1].string && args[1].string_size) || (args[2].type != FDKSS_NUMBER && args[2].type != FDKSS_INTEGER)))) {
        std::snprintf(error, capacity, "expected object/root; filter and offset; or object, field and numeric value"); return 1;
    }
    object_browser::Request request{count == 1 ? object_browser::Request::inspect : count == 2 ? object_browser::Request::page : object_browser::Request::write,
        args[0].string_size ? std::string(args[0].string, args[0].string_size) : std::string{}, {}, count == 2 ? args[1].integer : 0};
    if (count == 3) {
        if (args[1].string_size) request.field.assign(args[1].string, args[1].string_size);
        request.number = args[2].type == FDKSS_NUMBER ? args[2].number : static_cast<double>(args[2].integer);
    }
    const auto id = browser.submit(std::move(request));
    if (!id) { std::snprintf(error, capacity, "browser request queue is full or invalid"); return 1; }
    fdkss_value value{}; value.type = FDKSS_BOOLEAN; value.boolean = 1; if (emit(sink, &value)) return 1;
    value.type = FDKSS_INTEGER; value.integer = id; return emit(sink, &value);
}
int FDKSS_CALL browser_call(void *, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if (count!=2 || args[0].type!=FDKSS_STRING || args[1].type!=FDKSS_STRING ||
        !args[0].string || !args[1].string || !args[0].string_size || !args[1].string_size ||
        args[0].string_size>255 || args[1].string_size>255) {
        std::snprintf(error,capacity,"expected object/root and verified getter name");return 1;
    }
    const auto id=browser.submit({object_browser::Request::invoke,
        std::string(args[0].string,args[0].string_size),std::string(args[1].string,args[1].string_size)});
    if (!id) {std::snprintf(error,capacity,"browser queue full or getter unsupported");return 1;}
    fdkss_value value{};value.type=FDKSS_INTEGER;value.integer=id;return emit(sink,&value);
}
int FDKSS_CALL browser_result(void *, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if (count != 1 || args[0].type != FDKSS_INTEGER || args[0].integer <= 0) {
        std::snprintf(error, capacity, "expected positive browser request identifier"); return 1;
    }
    const auto saved = browser.lookup(args[0].integer);
    const char *phase = saved.phase == object_browser::Phase::pending ? "pending" :
        saved.phase == object_browser::Phase::succeeded ? "succeeded" :
        saved.phase == object_browser::Phase::failed ? "failed" :
        saved.phase == object_browser::Phase::cancelled ? "cancelled" :
        saved.phase == object_browser::Phase::expired ? "expired" : "unknown";
    fdkss_value value{}; value.type = FDKSS_STRING; value.string = phase; value.string_size = std::strlen(phase);
    if (emit(sink, &value)) return 1;
    value.string = saved.error.data(); value.string_size = saved.error.size(); if (emit(sink,&value)) return 1;
    for (const auto &result : saved.values) {
        value={};value.type=result.type;value.integer=result.integer;value.number=result.number;value.boolean=result.boolean;
        value.string=result.text.data();value.string_size=result.text.size();if (emit(sink,&value)) return 1;
    }
    return 0;
}
int FDKSS_CALL browser_watch(void *, const fdkss_value *args, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if ((count != 2 && count != 3) || args[0].type != FDKSS_STRING || args[1].type != FDKSS_STRING ||
        !args[0].string || !args[1].string || args[0].string_size == 0 || args[1].string_size == 0 ||
        args[0].string_size > 255 || args[1].string_size > 255 || (count == 3 && args[2].type != FDKSS_BOOLEAN)) {
        std::snprintf(error, capacity, "expected object/root, field, and optional enable boolean"); return 1;
    }
    const std::string root(args[0].string, args[0].string_size), field(args[1].string, args[1].string_size);
    fdkss_value value{};
    if (count == 3) {
        const auto id = browser.submit({args[2].boolean ? object_browser::Request::watch : object_browser::Request::unwatch, root, field});
        if (!id) {
            std::snprintf(error, capacity, "browser request queue is full or invalid"); return 1;
        }
        value.type = FDKSS_BOOLEAN; value.boolean = 1; if (emit(sink, &value)) return 1;
        value.type = FDKSS_INTEGER; value.integer = id; return emit(sink, &value);
    }
    const auto view = browser.snapshot();
    for (const auto &watch : view.watches) if (watch.root == root && watch.field == field) {
        value.type = FDKSS_INTEGER; value.integer = static_cast<std::int64_t>(watch.samples); if (emit(sink, &value)) return 1;
        value.type = FDKSS_BOOLEAN; value.boolean = watch.paused; if (emit(sink, &value)) return 1;
        value = {}; value.type = watch.value.type; value.integer = watch.value.integer;
        value.number = watch.value.number; value.boolean = watch.value.boolean;
        value.string = watch.value.text.data(); value.string_size = watch.value.text.size(); if (emit(sink, &value)) return 1;
        value = {}; value.type = FDKSS_STRING; value.string = watch.error.data(); value.string_size = watch.error.size(); return emit(sink, &value);
    }
    return emit(sink, &value);
}
int FDKSS_CALL browser_info(void *, const fdkss_value *, std::size_t count, fdkss_result_fn emit, void *sink, char *error, std::size_t capacity) {
    if (count) { std::snprintf(error, capacity, "expected no arguments"); return 1; }
    const auto view = browser.snapshot();
    fdkss_value value{}; value.type = FDKSS_INTEGER;
    for (const auto number : {static_cast<std::int64_t>(view.revision), static_cast<std::int64_t>(view.rows.size()),
            static_cast<std::int64_t>(view.properties.size()), view.next}) {
        value.integer = number; if (emit(sink, &value)) return 1;
    }
    value.type = FDKSS_STRING; value.string = view.error.data(); value.string_size = view.error.size(); return emit(sink, &value);
}
int FDKSS_CALL command(void *, const char *input, char *output, std::size_t capacity) {
    std::lock_guard lock(mutex);
    if (!std::strcmp(input, "show")) {
        if (!installed) { std::snprintf(output, capacity, "Renderer hook unavailable"); return 1; }
        shown.store(true);
        console_input::request(false);
        select_console.store(true);
    } else if (!std::strcmp(input, "edit")) {
        if (!installed || !console_input::hooked()) { std::snprintf(output, capacity, "Keyboard console unavailable"); return 1; }
        shown.store(true); console_input::request(true); focus_input.store(true); select_console.store(true);
    } else if (!std::strcmp(input, "objects")) {
        if (!installed) { std::snprintf(output, capacity, "Renderer hook unavailable"); return 1; }
        shown.store(true); console_input::request(false); select_browser.store(true);
    } else if (!std::strcmp(input, "calls")) {
        if (!installed) {std::snprintf(output,capacity,"Renderer hook unavailable");return 1;}
        shown.store(true);console_input::request(false);select_calls.store(true);
    } else if (!std::strcmp(input, "hide")) { shown.store(false); console_input::request(false); }
    else if (*input) { std::snprintf(output, capacity, "expected show, edit, objects, calls, hide or empty status query"); return 1; }
    const int length = std::snprintf(output, capacity,
        "{\"hooked\":%s,\"visible\":%s,\"presents\":%llu,\"draws\":%llu,\"render_thread\":%lu,\"buffer_width\":%u,\"buffer_height\":%u,\"input_hooked\":%s,\"editing\":%s,\"suppressed_keys\":%llu,\"error\":\"%s\"}",
        installed ? "true" : "false", shown.load() ? "true" : "false",
        static_cast<unsigned long long>(presents.load()), static_cast<unsigned long long>(draws.load()),
        static_cast<unsigned long>(render_thread.load()), last_width, last_height, console_input::hooked() ? "true" : "false",
        console_input::editing() ? "true" : "false", static_cast<unsigned long long>(console_input::suppressed()), diagnostic);
    return length < 0 || static_cast<std::size_t>(length) >= capacity;
}
}
