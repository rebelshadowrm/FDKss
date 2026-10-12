#pragma once
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace console_jobs {
struct Job { std::int64_t id; std::string mod, source; };
enum class Phase { unknown, pending, complete, expired };
struct Result { Phase phase = Phase::unknown; int status = 0; std::string text; };
class Queue {
    mutable std::mutex mutex_;
    std::deque<Job> pending_;
    std::deque<std::pair<std::int64_t, Result>> results_;
    std::int64_t last_id_ = 0, running_ = 0;
    bool closed_ = false;
    void remember(std::int64_t id, int status, std::string text) {
        if (text.size() > 16384) text.resize(16384);
        results_.emplace_back(id, Result{Phase::complete, status, std::move(text)});
        if (results_.size() > 16) results_.pop_front();
    }
public:
    std::int64_t submit(std::string mod, std::string source) {
        if (mod.empty() || mod.size() > 255 || mod.find('\0') != mod.npos ||
            source.empty() || source.size() > 8192 || source.find('\0') != source.npos)
            throw std::invalid_argument("console request requires mod name (1..255 bytes) and Lua source (1..8192 bytes), without NUL");
        std::lock_guard lock(mutex_);
        if (closed_) throw std::runtime_error("console queue is stopped");
        if (pending_.size() >= 8) throw std::runtime_error("console queue is full");
        if (last_id_ == std::numeric_limits<std::int64_t>::max()) throw std::runtime_error("console request identifiers exhausted");
        const auto id = last_id_ + 1;
        pending_.push_back({id, std::move(mod), std::move(source)});
        last_id_ = id;
        return id;
    }
    std::optional<Job> take() {
        std::lock_guard lock(mutex_);
        if (running_ || pending_.empty()) return {};
        Job result = std::move(pending_.front()); pending_.pop_front(); running_ = result.id;
        return result;
    }
    void finish(std::int64_t id, int status, std::string text) {
        std::lock_guard lock(mutex_);
        if (running_ != id) throw std::logic_error("console completion does not match the running request");
        remember(id, status, std::move(text)); running_ = 0;
    }
    Result lookup(std::int64_t id) const {
        std::lock_guard lock(mutex_);
        for (const auto &[saved, result] : results_) if (saved == id) return result;
        if (id > 0 && (id == running_ || [&] { for (const auto &job : pending_) if (job.id == id) return true; return false; }()))
            return {Phase::pending};
        return {id > 0 && id <= last_id_ ? Phase::expired : Phase::unknown};
    }
    void close(int status, const char *reason) {
        std::lock_guard lock(mutex_); closed_ = true;
        for (const auto &job : pending_) remember(job.id, status, reason);
        pending_.clear();
    }
};
}
