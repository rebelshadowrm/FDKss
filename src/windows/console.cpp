#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include "console.h"
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace {
HANDLE stopping = nullptr;
// Intentionally not destroyed under DLL/process teardown. Explicit detach joins it.
std::thread *worker = nullptr;
std::mutex mutex;
std::condition_variable completed;
std::string request, response;
bool pending = false, ready = false, quitting = false;

struct Handle {
    HANDLE value;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool finish(HANDLE pipe, OVERLAPPED &operation, BOOL immediate, DWORD &bytes) {
    if (immediate) return GetOverlappedResult(pipe, &operation, &bytes, FALSE) != FALSE;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    HANDLE events[] = {stopping, operation.hEvent};
    const auto state = WaitForMultipleObjects(2, events, FALSE, 10000);
    if (state != WAIT_OBJECT_0 + 1) {
        CancelIoEx(pipe, &operation);
        GetOverlappedResult(pipe, &operation, &bytes, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &operation, &bytes, FALSE) != FALSE;
}
void serve(HANDLE pipe) noexcept {
    try {
        Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!event.value) return;
        while (WaitForSingleObject(stopping, 0) == WAIT_TIMEOUT) {
            OVERLAPPED connect{}; connect.hEvent = event.value;
            ResetEvent(event.value);
            DWORD ignored = 0;
            const BOOL connected = ConnectNamedPipe(pipe, &connect);
            const auto error = connected ? ERROR_SUCCESS : GetLastError();
            if (error != ERROR_PIPE_CONNECTED && !finish(pipe, connect, connected, ignored)) {
                DisconnectNamedPipe(pipe); continue;
            }
            char data[65536]{};
            DWORD received = 0;
            OVERLAPPED read{}; read.hEvent = event.value;
            ResetEvent(event.value);
            if (!finish(pipe, read, ReadFile(pipe, data, sizeof(data), &received, &read), received) || !received) {
                DisconnectNamedPipe(pipe); continue;
            }
            std::string result;
            if (std::string_view(data, received).find('\0') != std::string_view::npos) {
                result = "ERROR\nrequest contains NUL bytes";
            } else {
                std::unique_lock lock(mutex);
                request.assign(data, received); pending = true; ready = false;
                completed.wait(lock, [] { return ready || quitting; });
                if (quitting) break;
                result = std::move(response); ready = false;
            }
            DWORD sent = 0;
            OVERLAPPED write{}; write.hEvent = event.value;
            ResetEvent(event.value);
            if (finish(pipe, write, WriteFile(pipe, result.data(), static_cast<DWORD>(result.size()), &sent, &write), sent)) {
                // A buffered write may finish before the client reads it. Disconnecting
                // immediately would discard unread bytes. Await acknowledgment or close.
                char acknowledgment = 0;
                DWORD count = 0;
                OVERLAPPED ack{}; ack.hEvent = event.value;
                ResetEvent(event.value);
                finish(pipe, ack, ReadFile(pipe, &acknowledgment, 1, &count, &ack), count);
            }
            DisconnectNamedPipe(pipe);
        }
    } catch (...) {}
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
}
}
namespace live_console {
bool start() {
    if (worker) return false;
    // Restrict this local pipe to the game process's user; reject remote clients.
    Handle token{nullptr};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return false;
    DWORD needed = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &needed);
    std::string user(needed, '\0');
    if (!needed || !GetTokenInformation(token.value, TokenUser, user.data(), needed, &needed)) return false;
    wchar_t *sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid, &sid)) return false;
    const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const std::wstring name = L"\\\\.\\pipe\\FDKss-" + std::to_wstring(GetCurrentProcessId());
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 65536, 65536, 0, &security);
    LocalFree(descriptor);
    if (pipe == INVALID_HANDLE_VALUE) return false;
    stopping = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopping) { CloseHandle(pipe); return false; }
    { std::lock_guard lock(mutex); pending = false; ready = false; quitting = false; request.clear(); response.clear(); }
    try { worker = new std::thread(serve, pipe); }
    catch (...) { CloseHandle(pipe); CloseHandle(stopping); stopping = nullptr; return false; }
    return true;
}
void pump(fdkss_runtime *runtime) {
    std::string input;
    {
        std::lock_guard lock(mutex);
        if (!pending) return;
        input = std::move(request); pending = false;
    }
    std::string output;
    const auto split = input.find('\n');
    if (!runtime) output = "ERROR\nLua runtime is stopped or unavailable";
    else if (split == input.npos) output = "ERROR\nexpected mod name, newline, Lua source";
    else {
        char result[16384]{};
        const auto name = input.substr(0, split);
        const auto status = fdkss_execute(runtime, name.c_str(), input.c_str() + split + 1, result, sizeof(result));
        output = status == FDKSS_OK ? std::string("OK\n") + result :
            std::string("ERROR\n") + fdkss_last_error(runtime);
    }
    { std::lock_guard lock(mutex); response = std::move(output); ready = true; }
    completed.notify_one();
}
void stop() {
    if (!worker) return;
    { std::lock_guard lock(mutex); quitting = true; }
    SetEvent(stopping); completed.notify_one(); worker->join(); delete worker; worker = nullptr;
    CloseHandle(stopping); stopping = nullptr;
}
}
