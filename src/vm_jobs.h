#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <deque>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
namespace vm_jobs {
enum class State { queued, running, complete, cancelled, failed };
inline const char *name(State state) {
    switch (state) {
    case State::queued: return "queued"; case State::running: return "running";
    case State::complete: return "complete"; case State::cancelled: return "cancelled";
    default: return "failed";
    }
}
struct Task {
    virtual ~Task() = default;
    virtual bool step(unsigned quantum, float delta) = 0;
};
struct Job {
    std::uint64_t id, owner;
    State state = State::queued;
    unsigned quantum, slices = 0;
    std::size_t bytes;
    std::uint32_t raw;
    std::string text, error;
    std::unique_ptr<Task> task;
};
struct Stats { std::size_t active=0, retained=0, bytes=0; };
class Queue {
    std::deque<Job> jobs;
    std::uint64_t serial = 0;
    void trim() {
        while (std::count_if(jobs.begin(),jobs.end(),[](const auto &j){return !j.task;}) > 16) {
            const auto oldest = std::find_if(jobs.begin(),jobs.end(),[](const auto &j){return !j.task;});
            jobs.erase(oldest);
        }
    }
public:
    Stats stats() const {
        Stats result;
        for (const auto &j : jobs) {
            if (j.task) { ++result.active; result.bytes+=j.bytes; }
            else ++result.retained;
        }
        return result;
    }
    bool available(std::size_t bytes) const {
        std::size_t used = 0, active = 0;
        for (const auto &j : jobs) if (j.task) { used += j.bytes; ++active; }
        return active < 8 && bytes <= 32*1024*1024 && used <= 32*1024*1024-bytes && serial < INT64_MAX;
    }
    std::uint64_t submit(std::uint64_t owner, unsigned quantum, std::size_t bytes,
        std::uint32_t raw, std::string text, std::unique_ptr<Task> task) {
        if (!owner || !quantum || quantum>128 || !task || !available(bytes)) throw std::runtime_error("VM job capacity exceeded");
        jobs.push_back({++serial,owner,State::queued,quantum,0,bytes,raw,std::move(text),{},std::move(task)});
        return serial;
    }
    const Job *find(std::uint64_t owner, std::uint64_t id) const {
        for (const auto &j : jobs) if (j.owner==owner && j.id==id) return &j;
        return nullptr;
    }
    bool cancel(std::uint64_t owner, std::uint64_t id) {
        for (auto &j : jobs) if (j.owner==owner && j.id==id && j.task) {
            j.task.reset(); j.state=State::cancelled; trim(); return true;
        }
        return false;
    }
    template<class Alive> void service(Alive alive, float delta=0.f) {
        if (!std::isfinite(delta) || delta<0) throw std::invalid_argument("invalid VM job update delta");
        std::erase_if(jobs,[&](const auto &j){return !alive(j.owner);});
        for (auto &j : jobs) if (j.task) {
            try {
                ++j.slices;
                if (j.task->step(j.quantum,delta)) { j.task.reset(); j.state=State::complete; }
                else if (j.slices>=2048) { j.task.reset(); j.state=State::failed; j.error="VM job slice limit exceeded"; }
                else j.state=State::running;
            } catch (const std::exception &e) { j.task.reset(); j.state=State::failed; j.error=e.what(); }
            catch (...) { j.task.reset(); j.state=State::failed; j.error="VM job execution failed"; }
        }
        trim();
    }
    void clear() { jobs.clear(); }
    std::size_t size() const { return jobs.size(); }
};
}
