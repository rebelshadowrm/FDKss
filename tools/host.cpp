#include "fdkss/api.h"
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>

static void FDKSS_CALL logger(void *, const char *mod, const char *message) {
    std::cout << '[' << mod << "] " << message << '\n';
}
static int FDKSS_CALL echo(void *, const char *input, char *output, size_t capacity) {
    const int count = std::snprintf(output, capacity, "%s", input);
    return count < 0 || static_cast<size_t>(count) >= capacity ? 1 : 0;
}
int main(int argc, char **argv) {
    if (argc != 2) { std::cerr << "Usage: fdkss_host <Mods directory>\n"; return 2; }
    fdkss_runtime *runtime = nullptr;
    if (fdkss_create(argv[1], logger, nullptr, &runtime) != FDKSS_OK) {
        std::cerr << "Could not create runtime; check Mods directory.\n"; return 1;
    }
    auto check = [&](fdkss_status status) {
        if (status != FDKSS_OK) std::cerr << "Status " << status << ": " << fdkss_last_error(runtime) << '\n';
        return status == FDKSS_OK;
    };
    if (!check(fdkss_register_native(runtime, "host.echo", echo, nullptr)) ||
        !check(fdkss_start(runtime))) { fdkss_destroy(runtime); return 1; }
    std::cout << "Commands: tick, reload <mod>, emit <event> <payload>, exec <mod> <Lua>, quit\n";
    auto last = std::chrono::steady_clock::now();
    std::string line;
    while (std::getline(std::cin, line) && line != "quit") {
        if (line == "tick") {
            const auto now = std::chrono::steady_clock::now();
            check(fdkss_tick(runtime, std::chrono::duration<double>(now - last).count()));
            last = now;
        } else if (line.starts_with("reload ")) check(fdkss_reload(runtime, line.c_str() + 7));
        else if (line.starts_with("exec ")) {
            const auto split = line.find(' ', 5);
            if (split != std::string::npos) {
                char result[16384]{};
                if (check(fdkss_execute(runtime, line.substr(5, split - 5).c_str(), line.c_str() + split + 1, result, sizeof(result))))
                    std::cout << result << '\n';
            } else std::cerr << "Usage: exec <mod> <Lua>\n";
        } else if (line.starts_with("emit ")) {
            const auto split = line.find(' ', 5);
            if (split != std::string::npos)
                check(fdkss_emit(runtime, line.substr(5, split - 5).c_str(), line.c_str() + split + 1));
            else std::cerr << "Usage: emit <event> <payload>\n";
        } else std::cerr << "Unknown command\n";
    }
    return fdkss_destroy(runtime) == FDKSS_OK ? 0 : 1;
}
