#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
std::string sha256(const std::filesystem::path &path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    struct Cleanup {
        BCRYPT_ALG_HANDLE &a; BCRYPT_HASH_HANDLE &h;
        ~Cleanup() { if (h) BCryptDestroyHash(h); if (a) BCryptCloseAlgorithmProvider(a, 0); }
    } cleanup{algorithm, hash};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("could not initialize executable hash");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw std::runtime_error("could not read target executable");
    char buffer[65536];
    while (stream.read(buffer, sizeof(buffer)) || stream.gcount()) {
        if (BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer),
                           static_cast<ULONG>(stream.gcount()), 0) < 0)
            throw std::runtime_error("could not hash target executable");
    }
    if (!stream.eof()) throw std::runtime_error("target executable read failed");
    unsigned char digest[32];
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0)
        throw std::runtime_error("could not finish executable hash");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
struct Handle {
    HANDLE value;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
uintptr_t remote_module(DWORD pid, const std::filesystem::path &path, bool full_path) {
    Handle snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)};
    if (snapshot.value == INVALID_HANDLE_VALUE) throw std::runtime_error("could not enumerate target modules");
    MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot.value, &entry)) do {
        const auto candidate = full_path ? std::filesystem::path(entry.szExePath) : std::filesystem::path(entry.szModule);
        if (_wcsicmp(candidate.c_str(), path.c_str()) == 0)
            return reinterpret_cast<uintptr_t>(entry.modBaseAddr);
    } while (Module32NextW(snapshot.value, &entry));
    return 0;
}
DWORD run_remote(HANDLE process, uintptr_t address, void *argument) {
    Handle thread{CreateRemoteThread(process, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(address), argument, 0, nullptr)};
    if (!thread.value) throw std::runtime_error("could not start remote loader thread");
    // A timeout must not free memory that an unfinished loader thread still uses.
    if (WaitForSingleObject(thread.value, 15000) != WAIT_OBJECT_0)
        throw std::runtime_error("remote thread timed out; buffer retained; restart target before retrying");
    DWORD code = 0;
    if (!GetExitCodeThread(thread.value, &code)) throw std::runtime_error("could not get remote thread result");
    return code;
}
}
int wmain(int argc, wchar_t **argv) {
    if (argc != 3 && argc != 4) {
        std::cerr << "Usage: fdkss_inject <PID> <absolute path to fdkss.dll> [--reload|--stop|--detach]\n";
        return 2;
    }
    try {
        const char *export_name = "FDKssAttach";
        if (argc == 4) {
            const std::wstring command = argv[3];
            if (command == L"--reload") export_name = "FDKssRequestReload";
            else if (command == L"--stop") export_name = "FDKssRequestStop";
            else if (command == L"--detach") export_name = "FDKssRequestDetach";
            else throw std::runtime_error("unknown control command");
        }
        size_t consumed = 0;
        const auto raw_pid = std::stoul(argv[1], &consumed);
        if (!raw_pid || consumed != std::wcslen(argv[1]) || raw_pid > MAXDWORD)
            throw std::runtime_error("invalid PID");
        const DWORD pid = static_cast<DWORD>(raw_pid);
        const auto dll = std::filesystem::canonical(argv[2]);
        Handle process{OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid)};
        if (!process.value) throw std::runtime_error("could not open target process");
        wchar_t target_path[32768]{};
        DWORD target_length = 32768;
        if (!QueryFullProcessImageNameW(process.value, 0, target_path, &target_length))
            throw std::runtime_error("could not identify target executable");
        const auto target_name = std::filesystem::path(target_path).filename();
        const char *expected_hash = nullptr;
        if (_wcsicmp(target_name.c_str(), L"sora_1st.exe") == 0)
            expected_hash = "4f84cb3be200cfd0485d9d6b2fec0ba70a63d73d3fb8a29a515e3a7abdbf13cf";
        else if (_wcsicmp(target_name.c_str(), L"sora_2nd.exe") == 0)
            expected_hash = "d8b2911d1576216bdc22d070550e4f531e105de7ed2981885849669f4acf8aaf";
        else if (_wcsicmp(target_name.c_str(), L"ysx_pn.exe") == 0)
            expected_hash = "34badc3071d5c571524a2234ad64a4c4251214de578eaa18c04b96432501765e";
        else if (_wcsicmp(target_name.c_str(), L"ysx.exe") == 0)
            expected_hash = "224df0db3e0867cc83d4f71b2e3e78f71f47c430899c76b9beb12da5403ae246";
        if (!expected_hash) throw std::runtime_error("target has no supported FDKss adapter");
        if (sha256(target_path) != expected_hash)
            throw std::runtime_error("target executable does not match its pinned adapter build");
        if (_wcsicmp(dll.filename().c_str(), L"fdkss.dll") != 0)
            throw std::runtime_error("DLL must be named fdkss.dll");
        USHORT target_machine = 0, native_machine = 0;
        if (!IsWow64Process2(process.value, &target_machine, &native_machine) ||
            target_machine != IMAGE_FILE_MACHINE_UNKNOWN || native_machine != IMAGE_FILE_MACHINE_AMD64)
            throw std::runtime_error("target must be native x64");
        // Resolve LoadLibraryW relative to its actual owner (often KernelBase),
        // then locate that module in the target rather than assuming equal ASLR bases.
        const auto loader = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HMODULE owner = nullptr;
        if (!loader || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(loader), &owner))
            throw std::runtime_error("could not resolve loader");
        wchar_t owner_path[32768]{};
        if (!GetModuleFileNameW(owner, owner_path, 32768)) throw std::runtime_error("could not resolve loader module");
        const auto remote_owner = remote_module(pid, std::filesystem::path(owner_path).filename(), false);
        if (!remote_owner) throw std::runtime_error("target loader module not found");
        const auto remote_loader = remote_owner + reinterpret_cast<uintptr_t>(loader) - reinterpret_cast<uintptr_t>(owner);
        auto remote_dll = remote_module(pid, dll, true);
        if (argc == 4 && !remote_dll) throw std::runtime_error("FDKss is not loaded in the target");
        if (!remote_dll) {
            const auto path = dll.wstring();
            const size_t bytes = (path.size() + 1) * sizeof(wchar_t);
            void *buffer = VirtualAllocEx(process.value, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!buffer) throw std::runtime_error("could not allocate loader argument");
            SIZE_T written = 0;
            if (!WriteProcessMemory(process.value, buffer, path.c_str(), bytes, &written) || written != bytes) {
                VirtualFreeEx(process.value, buffer, 0, MEM_RELEASE);
                throw std::runtime_error("could not write loader argument");
            }
            run_remote(process.value, remote_loader, buffer);
            VirtualFreeEx(process.value, buffer, 0, MEM_RELEASE);
            remote_dll = remote_module(pid, dll, true);
            if (!remote_dll) throw std::runtime_error("DLL failed to load (check architecture and dependencies)");
        }
        HMODULE local = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!local) throw std::runtime_error("could not inspect DLL exports");
        const auto attach = GetProcAddress(local, export_name);
        const auto offset = reinterpret_cast<uintptr_t>(attach) - reinterpret_cast<uintptr_t>(local);
        FreeLibrary(local);
        if (!attach) throw std::runtime_error("requested FDKss export is missing");
        const DWORD status = run_remote(process.value, remote_dll + offset, nullptr);
        if (status != ERROR_SUCCESS && status != ERROR_ALREADY_EXISTS) {
            std::cerr << "Attach failed with Windows error " << status << "; see FDKss.log beside the DLL.\n";
            return 1;
        }
        std::cout << (argc == 4 ? "FDKss command queued for the owning thread; see FDKss.log.\n" :
            "FDKss attached; see FDKss.log for Lua startup.\n");
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << " (Windows error " << GetLastError() << ")\n"; return 1; }
}
