#include "lifetimes.h"
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace actor_lifetimes;
namespace {
std::atomic<unsigned> calls{0}, last_flags{0};
Stamp old{};
void require(bool condition) { if (!condition) throw std::runtime_error("lifetime assertion failed"); }
void *__fastcall original(void *object, unsigned flags) {
    require(!valid(reinterpret_cast<std::uintptr_t>(object), old));
    ++calls; last_flags = flags;
    return object;
}
void *__fastcall other(void *object, unsigned flags) {
    ++calls; last_flags = flags;
    return reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(object) ^ flags);
}
}
int main() {
    try {
        void *slot = reinterpret_cast<void *>(&original), *second = reinterpret_cast<void *>(&other);
        Hook bad{&slot, &other, "Wrong"};
        require(!install(std::span(&bad, 1)) && slot == reinterpret_cast<void *>(&original));
        Hook hooks[]{{&slot, &original, "Actor"}, {&second, &other, "OtherActor"}};
        require(install(hooks));
        auto thunk = reinterpret_cast<Destructor>(slot);
        auto second_thunk = reinterpret_cast<Destructor>(second);
        void *object = reinterpret_cast<void *>(0x12345678000ull);
        const auto address = reinterpret_cast<std::uintptr_t>(object);
        old = observe(address);
        require(valid(address, old));
        require(thunk(object, 0x87654321) == object && calls == 1 && last_flags == 0x87654321);
        require(!valid(address, old));
        auto events = drain();
        require(events.destroyed.size() == 1 && events.destroyed[0].address == address &&
            std::string(events.destroyed[0].class_name) == "Actor" && !events.dropped);
        const auto reused = observe(address);
        require(valid(address, reused) && !valid(address, old));
        std::thread worker([&] { second_thunk(object, 7); }); worker.join();
        require(!valid(address, reused) && calls == 2 && last_flags == 7);
        require(second_thunk(object, 3) == reinterpret_cast<void *>(address ^ 3));
        drain();
        for (unsigned index = 0; index < event_queue_capacity+2; ++index) second_thunk(object, 1);
        events = drain();
        require(events.destroyed.size() == event_queue_capacity && events.dropped == 2);
        auto queue=queue_info();require(queue.pending==0 && queue.peak==event_queue_capacity && queue.pending_dropped==0 && queue.total_dropped==2);
        // A direct entry following a vtable thunk must not notify/invalidate twice.
        auto direct_stamp=observe(address);
        second_thunk(object,0);
        destructing(address,"BaseActor");
        events=drain();
        require(!valid(address,direct_stamp) && events.destroyed.size()==1 &&
            std::string(events.destroyed[0].class_name)=="OtherActor");
        // A direct entry invalidates and notifies a tracked live reference once,
        // but does not insert an untracked address or revive a dead reference.
        direct_stamp=observe(address);
        destructing(address,"BaseActor");destructing(address,"BaseActor");
        destructing(address+8,"Untracked");
        events=drain();
        require(!valid(address,direct_stamp) && events.destroyed.size()==1 &&
            std::string(events.destroyed[0].class_name)=="BaseActor");
        for(unsigned index=0;index<event_queue_capacity+2;++index) {observe(address);destructing(address,"BaseActor");}
        queue=queue_info();require(queue.pending==event_queue_capacity && queue.pending_dropped==2 && queue.total_dropped==4);
        require(!valid(address,direct_stamp));
        events=drain();require(events.destroyed.size()==event_queue_capacity && events.dropped==2);
        // The observed save/map teardown overflow was 995 beyond the old 2048
        // queue. This bounded representative burst must preserve every event.
        for(std::uintptr_t index=0;index<3043;++index) {
            auto current=reinterpret_cast<void *>(0x20000000000ull+index*16);
            old=observe(reinterpret_cast<std::uintptr_t>(current));
            require(thunk(current,0)==current && !valid(reinterpret_cast<std::uintptr_t>(current),old));
        }
        events=drain();require(events.destroyed.size()==3043 && !events.dropped);
        for(std::uintptr_t index=0;index<events.destroyed.size();++index)
            require(events.destroyed[index].address==0x20000000000ull+index*16);
        // A complete maximum-size accepted scene fits as well, with every
        // reference invalidated and every ordered diagnostic preserved.
        for(std::uintptr_t index=0;index<16384;++index) {
            auto current=reinterpret_cast<void *>(0x30000000000ull+index*16);
            old=observe(reinterpret_cast<std::uintptr_t>(current));
            require(thunk(current,0)==current && !valid(reinterpret_cast<std::uintptr_t>(current),old));
        }
        events=drain();require(events.destroyed.size()==16384 && !events.dropped);
        for(std::uintptr_t index=0;index<events.destroyed.size();++index)
            require(events.destroyed[index].address==0x30000000000ull+index*16);
        // Bounded address history invalidates every previous stamp on capacity rollover.
        const auto before_capacity = observe(address);
        for (std::uintptr_t index = 1; index <= 65536; ++index) observe(index * 16);
        require(!valid(address, before_capacity));
        // Respect another hook's replacement and preserve cached call-through thunks.
        second = reinterpret_cast<void *>(&original);
        require(stop() && slot == reinterpret_cast<void *>(&original) && second == reinterpret_cast<void *>(&original));
        queue=queue_info();require(queue.pending==0 && queue.peak==0 && queue.pending_dropped==0 && queue.total_dropped==0);
        require(thunk(object, 42) == object && last_flags == 42);
        require(drain().destroyed.empty() && !install(hooks));
        std::cout << "Lifetime ABI, address reuse, foreign thread, overflow and restoration passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
