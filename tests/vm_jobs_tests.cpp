#include "vm_jobs.h"
#include <iostream>
using namespace vm_jobs;
void require(bool okay, const char *why) { if (!okay) throw std::runtime_error(why); }
struct Driver final : Task {
    unsigned remaining; unsigned &destroyed, &steps; bool fail;
    Driver(unsigned n, unsigned &d, unsigned &s, bool f=false) : remaining(n),destroyed(d),steps(s),fail(f) {}
    ~Driver() override { ++destroyed; }
    bool step(unsigned quantum, float) override {
        ++steps; if (fail) throw std::runtime_error("fixture failure");
        if (remaining<=quantum) return true; remaining-=quantum; return false;
    }
};
struct ClockDriver final : Task {
    float &elapsed; unsigned &destroyed;
    ClockDriver(float &e,unsigned &d) : elapsed(e),destroyed(d) {}
    ~ClockDriver() override { ++destroyed; }
    bool step(unsigned, float delta) override { elapsed+=delta; return elapsed>=.15f; }
};
int main() {
    try {
        Queue queue; unsigned destroyed=0,steps=0;
        auto add=[&](unsigned owner, unsigned n, unsigned quantum=1, std::size_t bytes=1, bool fail=false) {
            return queue.submit(owner,quantum,bytes,0x4000002a,"",std::make_unique<Driver>(n,destroyed,steps,fail));
        };
        const auto a=add(1,3), b=add(2,5);
        require(queue.stats().active==2 && queue.stats().retained==0 && queue.stats().bytes==2,"active accounting");
        require(!queue.find(2,a) && !queue.cancel(2,a),"cross mod ownership");
        queue.service([](auto){return true;});
        require(queue.find(1,a)->state==State::running && queue.find(1,a)->slices==1 && steps==2,"one slice per active job");
        require(queue.cancel(2,b) && !queue.cancel(2,b) && destroyed==1,"cancellation exactly once");
        require(queue.stats().active==1 && queue.stats().retained==1 && queue.stats().bytes==1,"cancel frees payload");
        queue.service([](auto){return true;}); queue.service([](auto){return true;});
        require(queue.find(1,a)->state==State::complete && destroyed==2,"completion cleanup");
        add(3,100); queue.service([](auto owner){return owner!=3;}); require(destroyed==3,"unload before execution");
        const auto broken=add(1,10,1,1,true); queue.service([](auto){return true;});
        require(queue.find(1,broken)->state==State::failed && queue.find(1,broken)->error=="fixture failure","failure isolation");
        const auto infinite=add(1,10000);
        for (unsigned i=0;i<2048;++i) queue.service([](auto){return true;});
        require(queue.find(1,infinite)->state==State::failed && queue.find(1,infinite)->slices==2048,"slice limit");
        queue.clear();
        for (unsigned i=0;i<8;++i) add(1,100);
        require(!queue.available(0),"active bound");
        bool rejected=false; try { add(1,100); } catch (...) { rejected=true; } require(rejected,"ninth task accepted");
        queue.clear(); require(queue.available(32*1024*1024) && !queue.available(32*1024*1024+1),"memory bound");
        add(1,100,1,32*1024*1024); require(!queue.available(1),"aggregate memory"); queue.clear();
        const auto oldest=add(1,1); queue.service([](auto){return true;});
        for (unsigned i=0;i<16;++i) { add(1,1); queue.service([](auto){return true;}); }
        require(queue.size()==16 && !queue.find(1,oldest),"result expiry");
        queue.service([](auto){return false;}); require(queue.size()==0,"reload results cleanup");
        float elapsed=0; const auto clock=queue.submit(1,1,1,0,"",std::make_unique<ClockDriver>(elapsed,destroyed));
        const auto before_steps=steps, before_destroyed=destroyed;
        for (float delta : {-1.f,std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
            rejected=false; try { queue.service([](auto){return true;},delta); } catch (const std::invalid_argument &) { rejected=true; }
            require(rejected && queue.find(1,clock)->slices==0 && elapsed==0,"invalid delta advanced a task");
        }
        queue.service([](auto){return true;},0.f);
        require(elapsed==0 && queue.find(1,clock)->state==State::running,"zero delta must stall timer");
        queue.service([](auto){return true;},.1f);
        require(queue.find(1,clock)->state==State::running,"timer completed early");
        queue.service([](auto){return true;},.1f);
        require(queue.find(1,clock)->state==State::complete && destroyed==before_destroyed+1,"timer delta or completion cleanup");
        elapsed=0; const auto cancel_clock=queue.submit(1,1,1,0,"",std::make_unique<ClockDriver>(elapsed,destroyed));
        queue.service([](auto){return true;},.1f); require(queue.cancel(1,cancel_clock),"running timer cancellation");
        queue.service([](auto){return true;},.1f);
        require(elapsed==.1f && steps==before_steps && destroyed==before_destroyed+2,"cancelled timer advanced");
        std::cout << "VM job ownership, fairness, cancellation, limits and cleanup passed\n"; return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
