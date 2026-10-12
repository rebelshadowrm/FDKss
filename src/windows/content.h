#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace content_overlay {
// Verified FileStream::Open ABI, including its fifth, stack-passed argument.
using Open = int (__fastcall *)(void *, const char *, std::uint32_t, std::uint32_t, std::uint16_t);
struct Request { std::string path; std::uint32_t mode, flags; bool redirected; int result; };
struct Status { bool enabled; std::size_t files; std::uint64_t opens, redirects, dropped; };
bool start(void *target, const std::filesystem::path &mods);
bool refresh(const std::filesystem::path &mods);
bool stop();
Status status();
std::vector<Request> drain(std::size_t limit = 16);
std::string error();
std::string describe(bool requests);
}
