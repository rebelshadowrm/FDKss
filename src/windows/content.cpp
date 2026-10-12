#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include "content.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace content_overlay {
namespace {
using Files = std::unordered_map<std::string, std::string>;
std::atomic<std::shared_ptr<const Files>> files;
std::atomic<bool> enabled{false};
std::atomic<std::uint64_t> opens{0}, redirects{0};
Open original = nullptr;
void *target = nullptr;
bool attempted = false;
std::array<unsigned char, 5> installed_patch{};
std::mutex diagnostics;
std::string last_error;
struct Trace { std::array<char, 512> path{}; std::uint32_t mode, flags; bool redirected; int result; };
std::array<Trace, 512> traces{};
std::size_t trace_count = 0;
std::uint64_t dropped = 0;
thread_local bool reentrant = false;

std::optional<std::string> normalize(std::string_view path) {
    if (path.empty() || path.size() >= 512) return {};
    std::string result;
    for (const unsigned char ch : path) {
        if (ch < 32 || ch >= 127 || ch == ':') return {};
        result += ch == '\\' ? '/' : ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + 32) : static_cast<char>(ch);
    }
    if (result.front() == '/' || result.back() == '/') return {};
    std::size_t begin = 0;
    while (begin < result.size()) {
        const auto end = result.find('/', begin);
        const auto part = result.substr(begin, end == result.npos ? end : end - begin);
        if (part.empty() || part == "." || part == "..") return {};
        begin = end == result.npos ? result.size() : end + 1;
    }
    return result;
}
std::string utf8(const std::filesystem::path &path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}
void failure(const std::string &message) { std::lock_guard guard(diagnostics); last_error = message; }
bool mh(MH_STATUS result) {
    if (result == MH_OK) return true;
    failure(MH_StatusToString(result)); return false;
}
int __fastcall open(void *stream, const char *path, std::uint32_t mode, std::uint32_t flags, std::uint16_t extra) {
    if (!enabled.load(std::memory_order_acquire) || reentrant) return original(stream, path, mode, flags, extra);
    reentrant = true;
    struct Reset { ~Reset() { reentrant = false; } } reset;
    ++opens;
    std::array<char, 512> text{};
    // Engine paths are valid C strings; bound our own copy and preserve unknown paths.
    std::size_t length = 0;
    if (path) while (length < text.size() - 1 && path[length]) { text[length] = path[length]; ++length; }
    bool redirected = false;
    int result = 0;
    const auto snapshot = files.load(std::memory_order_acquire);
    const std::string *replacement = nullptr;
    try {
        if (snapshot && path && length < text.size() - 1 && (mode == 0 || mode == 2) && !(flags & 4)) {
            if (auto key = normalize(std::string_view(text.data(), length))) {
                if (const auto found = snapshot->find(*key); found != snapshot->end()) replacement = &found->second;
            }
        }
    } catch (...) { replacement = nullptr; } // Allocation failure preserves the original engine request.
    if (replacement) {
        // Absolute-path flag bypasses FDK language/path mapping; disable its two cache paths.
        redirected = true; ++redirects;
        result = original(stream, replacement->c_str(), mode, (flags | 4u) & ~3u, extra);
    } else result = original(stream, path, mode, flags, extra);
    {
        std::lock_guard guard(diagnostics);
        if (trace_count < traces.size()) traces[trace_count++] = {text, mode, flags, redirected, result};
        else ++dropped;
    }
    return result;
}
}
bool refresh(const std::filesystem::path &mods) {
    try {
        auto candidate = std::make_shared<Files>();
        std::vector<std::filesystem::path> roots;
        for (const auto &entry : std::filesystem::directory_iterator(mods)) {
            if (entry.is_directory() && std::filesystem::is_regular_file(entry.path() / "enabled.txt") &&
                std::filesystem::is_directory(entry.path() / "Assets")) roots.push_back(entry.path());
        }
        std::sort(roots.begin(), roots.end());
        for (const auto &root : roots) {
            const auto assets = std::filesystem::canonical(root / "Assets");
            for (const auto &entry : std::filesystem::recursive_directory_iterator(assets)) {
                if (!entry.is_regular_file()) continue;
                if (candidate->size() >= 65536) throw std::runtime_error("content overlay exceeds 65536 files");
                const auto key = normalize(utf8(entry.path().lexically_relative(assets)));
                if (!key) throw std::runtime_error("asset keys must be printable ASCII relative paths without dot components");
                const auto absolute = std::filesystem::canonical(entry.path());
                const auto relative = absolute.lexically_relative(assets);
                if (relative.empty() || *relative.begin() == L"..") throw std::runtime_error("asset resolves outside its mod Assets directory");
                const auto destination = utf8(absolute);
                if (destination.size() >= 512) throw std::runtime_error("asset absolute path exceeds FDK's 511-byte path limit");
                (*candidate)[*key] = destination;
            }
        }
        files.store(std::move(candidate), std::memory_order_release);
        failure(""); return true;
    } catch (const std::exception &exception) { failure(exception.what()); return false; }
}
bool start(void *address, const std::filesystem::path &mods) {
    if (attempted || !address) return false;
    if (!refresh(mods)) return false;
    attempted = true; target = address;
    if (!mh(MH_Initialize())) return false;
    if (!mh(MH_CreateHook(target, reinterpret_cast<void *>(&open), reinterpret_cast<void **>(&original)))) return false;
    enabled.store(true, std::memory_order_release);
    if (!mh(MH_EnableHook(target))) { enabled.store(false, std::memory_order_release); return false; }
    std::memcpy(installed_patch.data(), target, installed_patch.size());
    return true;
}
bool stop() {
    enabled.store(false, std::memory_order_release);
    if (original) {
        // A later mod may have replaced or chained our detour. Preserve its patch.
        if (std::memcmp(target, installed_patch.data(), installed_patch.size()) != 0) return true;
        const auto result = MH_DisableHook(target);
        if (result != MH_OK && result != MH_ERROR_DISABLED) return mh(result);
    }
    // Retain trampoline storage for calls that were already in flight. DLL is pinned.
    return true;
}
Status status() {
    const auto snapshot = files.load(std::memory_order_acquire);
    std::lock_guard guard(diagnostics);
    return {enabled.load(), snapshot ? snapshot->size() : 0, opens.load(), redirects.load(), dropped};
}
std::vector<Request> drain(std::size_t limit) {
    std::lock_guard guard(diagnostics);
    const auto count = std::min(limit, trace_count);
    std::vector<Request> result; result.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto &trace = traces[index];
        result.push_back({trace.path.data(), trace.mode, trace.flags, trace.redirected, trace.result});
    }
    std::copy(traces.begin() + count, traces.begin() + trace_count, traces.begin());
    trace_count -= count;
    return result;
}
std::string error() { std::lock_guard guard(diagnostics); return last_error; }
std::string describe(bool requests) {
    if (!requests) {
        const auto state = status();
        return std::string("{\"enabled\":") + (state.enabled ? "true" : "false") +
            ",\"files\":" + std::to_string(state.files) + ",\"opens\":" + std::to_string(state.opens) +
            ",\"redirects\":" + std::to_string(state.redirects) + ",\"dropped_trace\":" + std::to_string(state.dropped) + "}";
    }
    auto quote = [](std::string_view value) {
        constexpr char hex[] = "0123456789abcdef";
        std::string result = "\"";
        for (const unsigned char ch : value) {
            if (ch == '"' || ch == '\\') { result += '\\'; result += ch; }
            else if (ch < 32) { result += "\\u00"; result += hex[ch >> 4]; result += hex[ch & 15]; }
            else result += ch;
        }
        return result + '"';
    };
    std::string result = "[";
    for (const auto &request : drain()) {
        if (result.size() > 1) result += ',';
        result += "{\"path\":" + quote(request.path) + ",\"mode\":" + std::to_string(request.mode) +
            ",\"flags\":" + std::to_string(request.flags) + ",\"redirected\":" +
            (request.redirected ? "true" : "false") + ",\"result\":" + std::to_string(request.result) + "}";
    }
    return result + "]";
}
}
