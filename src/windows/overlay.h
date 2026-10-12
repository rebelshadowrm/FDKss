#pragma once
#include <cstddef>
#include <cstdint>
#include "fdkss/api.h"

namespace developer_overlay {
struct Snapshot {
    char map[64]{};
    double position[3]{};
    bool player_available = false;
    double time_scale = 1;
    std::uint64_t updates = 0;
    std::uint32_t owner_thread = 0;
};
bool start(bool sky2);
bool stop();
bool visible();
bool captures_keyboard();
void bind_browser(bool sky2, fdkss_typed_native_fn get, fdkss_typed_native_fn list, fdkss_typed_native_fn set, fdkss_typed_native_fn invoke = nullptr);
void publish(const Snapshot &);
void log(const char *mod, const char *message);
void service(fdkss_runtime *);
int FDKSS_CALL enqueue(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL result(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL browser_request(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL browser_call(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL browser_info(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL browser_result(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL browser_watch(void *, const fdkss_value *, std::size_t, fdkss_result_fn, void *, char *, std::size_t);
int FDKSS_CALL command(void *, const char *, char *, std::size_t);
}
