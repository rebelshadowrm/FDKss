#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "program_lifetimes.h"
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace program_lifetimes;
namespace {
struct Descriptor { std::array<char,88> bytes{}; };
std::atomic<unsigned> calls{0};
std::atomic<bool> bad{false};
Stamp expected;
HANDLE entered = nullptr, release = nullptr;
__declspec(noinline) void *__fastcall engine_destroy(void *address) {
    ++calls;
    if (valid(reinterpret_cast<std::uintptr_t>(address),expected)) bad = true;
    if (entered) { SetEvent(entered); WaitForSingleObject(release,5000); }
    static_cast<char *>(address)[12] = 0;
    return address;
}
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void name(Descriptor &d, const char *path) { std::memcpy(d.bytes.data()+12,path,std::strlen(path)+1); }
void write(void *address, const void *bytes, std::size_t count) {
    DWORD protection, ignored;
    require(VirtualProtect(address,count,PAGE_EXECUTE_READWRITE,&protection)!=0,"protect");
    std::memcpy(address,bytes,count); FlushInstructionCache(GetCurrentProcess(),address,count);
    require(VirtualProtect(address,count,protection,&ignored)!=0,"restore protection");
}
}
int main(int argc, char **) {
    try {
        auto *target = reinterpret_cast<void *>(&engine_destroy);
        std::array<char,16> original{}; std::memcpy(original.data(),target,original.size());
        require(install(target,GetCurrentThreadId()),"install");
        if (argc > 1) {
            const unsigned char external = 0xcc; write(target,&external,1);
            require(stop(),"external stop");
            require(*static_cast<unsigned char *>(target)==external,"external patch overwritten");
            write(target,original.data(),original.size());
            std::cout << "External patch preserved\n"; return 0;
        }
        using Destroy = void *(__fastcall *)(void *);
        Destroy volatile call = &engine_destroy;
        Descriptor d; name(d,"scena/fixture");
        const auto address = reinterpret_cast<std::uintptr_t>(&d);
        expected = observe(address);
        require(expected.generation && observe(address).generation==expected.generation,"stable stamp");
        const auto first = expected;
        require(call(&d)==&d && !bad && !valid(address,first),"invalidate before original");
        auto events = drain();
        require(events.unloaded.size()==1 && events.unloaded[0].generation==first.generation &&
            std::strcmp(events.unloaded[0].path.data(),"scena/fixture")==0,"copy before clear");
        name(d,"scena/reused"); expected = observe(address);
        require(expected.generation!=first.generation && !valid(address,first),"address reuse");
        std::thread worker([&]{call(&d);}); worker.join();
        require(!bad && !valid(address,expected) && drain().unloaded.size()==1,"foreign destruction");
        for (unsigned i=0;i<130;++i) { name(d,"overflow"); expected=observe(address); call(&d); }
        events=drain(); require(events.unloaded.size()==128 && events.dropped==2,"bounded overflow");
        std::array<Descriptor,65> many{};
        for (unsigned i=0;i<64;++i) require(observe(reinterpret_cast<std::uintptr_t>(&many[i])).generation!=0,"registry capacity");
        require(observe(reinterpret_cast<std::uintptr_t>(&many[64])).generation==0,"registry bound");
        expected = observe(reinterpret_cast<std::uintptr_t>(&many[0])); call(&many[0]);
        require(observe(reinterpret_cast<std::uintptr_t>(&many[64])).generation!=0,"registry reuse");
        drain();
        entered=CreateEventW(nullptr,TRUE,FALSE,nullptr); release=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        require(entered && release,"events");
        expected={}; std::thread blocked([&]{call(&d);});
        const bool reached = WaitForSingleObject(entered,5000)==WAIT_OBJECT_0;
        const bool stopping = stop();
        SetEvent(release); blocked.join(); CloseHandle(entered); CloseHandle(release); entered=release=nullptr;
        require(reached && !stopping && stop(),"quiescent stop retry");
        require(std::memcmp(target,original.data(),original.size())==0,"prologue restoration");
        require(!valid(address,first) && !observe(address).generation,"stopped stamps");
        const auto count=calls.load(); call(&d); require(calls==count+1 && !bad,"original exactly once");
        std::cout << "Invalidation order, reuse, foreign destruction, bounds and restoration passed\n"; return 0;
    } catch (const std::exception &e) { stop(); std::cerr << e.what() << '\n'; return 1; }
}
