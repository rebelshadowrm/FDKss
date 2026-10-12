#include "plugin.h"
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace fs = std::filesystem;
namespace {
struct Shadow {
    fs::path root, directory;
    explicit Shadow(const fs::path &library) {
        static std::atomic<unsigned long long> sequence{0};
        root = fs::absolute(library.parent_path().parent_path().parent_path() / ".fdkss-native-cache");
        fs::create_directories(root);
        const auto serial = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = root / (library.parent_path().parent_path().filename().string() + "-" +
            std::to_string(serial) + "-" + std::to_string(sequence.fetch_add(1)));
        if (!fs::create_directory(directory)) throw std::runtime_error("could not create native mod shadow directory");
    }
    ~Shadow() {
        // Only remove the unique directory we created within the dedicated cache root.
        std::error_code error;
        if (!directory.empty() && directory.is_absolute() && directory.parent_path() == root)
            fs::remove_all(directory, error);
    }
};
struct Library {
#if defined(_WIN32)
    HMODULE module = nullptr;
    ~Library() { if (module) FreeLibrary(module); }
#endif
};
}
struct NativePlugin::Impl {
    Shadow shadow;
    Library library;
    fdkss_plugin_host host;
    fdkss_plugin_exports exports{};
    void *state = nullptr;
    bool start_attempted = false, updates_enabled = true, events_enabled = true, filters_enabled = true;
    Impl(const fs::path &path, const fdkss_plugin_host &api) : shadow(path), host(api) {
#if defined(_WIN32)
        for (const auto &entry : fs::directory_iterator(path.parent_path())) {
            if (entry.is_regular_file() && _wcsicmp(entry.path().extension().c_str(), L".dll") == 0)
                fs::copy_file(entry.path(), shadow.directory / entry.path().filename());
        }
        const auto copy = shadow.directory / path.filename();
        library.module = LoadLibraryExW(copy.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!library.module) throw std::runtime_error("native DLL load failed, Windows error " + std::to_string(GetLastError()));
        const auto query = reinterpret_cast<fdkss_plugin_query_fn>(GetProcAddress(library.module, "FDKssPluginQuery"));
        if (!query) throw std::runtime_error("native plugin query export missing");
        int status = query(FDKSS_PLUGIN_ABI_VERSION, &exports, sizeof(exports));
        if (status) {
            exports = {};
            status = query(FDKSS_PLUGIN_ABI_VERSION, &exports, FDKSS_PLUGIN_EXPORTS_BASE_SIZE);
        }
        if (status || (exports.size != sizeof(exports) && exports.size != FDKSS_PLUGIN_EXPORTS_BASE_SIZE) ||
            exports.abi_version != FDKSS_PLUGIN_ABI_VERSION ||
            !exports.start || !exports.stop)
            throw std::runtime_error("native plugin ABI/query exports are invalid");
        if (exports.size == FDKSS_PLUGIN_EXPORTS_BASE_SIZE) exports.filter = nullptr;
#else
        (void)path;
        throw std::runtime_error("native DLL mods currently require Windows");
#endif
    }
    ~Impl() {
        if (start_attempted) {
            try { exports.stop(state); }
            catch (...) { host.log(host.context, "native plugin stop threw; plugin must clean up its own resources"); }
        }
    }
    template<class Operation>
    bool invoke(const char *phase, Operation operation) {
        char error[1024]{};
        try {
            if (operation(error, sizeof(error)) == 0) return true;
            error[sizeof(error) - 1] = '\0';
            const std::string message = std::string("native ") + phase + " failed: " + error;
            host.log(host.context, message.c_str());
        } catch (const std::exception &exception) {
            const std::string message = std::string("native ") + phase + " threw: " + exception.what();
            host.log(host.context, message.c_str());
        } catch (...) { host.log(host.context, "native lifecycle callback threw"); }
        return false;
    }
};
NativePlugin::NativePlugin(const fs::path &path, const fdkss_plugin_host &host) : impl(std::make_unique<Impl>(path, host)) {}
NativePlugin::~NativePlugin() = default;
bool NativePlugin::start() {
    impl->start_attempted = true;
    return impl->invoke("start", [&](char *error, size_t capacity) { return impl->exports.start(&impl->host, &impl->state, error, capacity); });
}
bool NativePlugin::update(double delta) {
    if (!impl->exports.update || !impl->updates_enabled) return true;
    if (impl->invoke("update", [&](char *error, size_t capacity) { return impl->exports.update(impl->state, delta, error, capacity); })) return true;
    impl->updates_enabled = false;
    return false;
}
bool NativePlugin::event(const char *name, const fdkss_value *values, size_t count) {
    if (!impl->exports.event || !impl->events_enabled) return true;
    if (impl->invoke("event", [&](char *error, size_t capacity) { return impl->exports.event(impl->state, name, values, count, error, capacity); })) return true;
    impl->events_enabled = false;
    return false;
}
bool NativePlugin::filter(const char *name, const fdkss_value *values, size_t count, size_t index, fdkss_value &value) {
    if (!impl->exports.filter || !impl->filters_enabled) return true;
    if (impl->invoke("filter", [&](char *error, size_t capacity) {
        return impl->exports.filter(impl->state, name, values, count, index, &value, error, capacity);
    })) return true;
    impl->filters_enabled = false;
    return false;
}
void NativePlugin::reject_filter() { impl->filters_enabled = false; }
