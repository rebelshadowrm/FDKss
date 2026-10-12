#pragma once
#include "fdkss/plugin.h"
#include <filesystem>
#include <memory>
class NativePlugin {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    NativePlugin(const std::filesystem::path &library, const fdkss_plugin_host &);
    ~NativePlugin();
    NativePlugin(const NativePlugin &) = delete;
    NativePlugin &operator=(const NativePlugin &) = delete;
    bool start();
    bool update(double delta);
    bool event(const char *name, const fdkss_value *, size_t count);
    bool filter(const char *name, const fdkss_value *, size_t count, size_t index, fdkss_value &replacement);
    void reject_filter();
};
