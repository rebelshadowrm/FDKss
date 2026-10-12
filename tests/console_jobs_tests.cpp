#include "console_jobs.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using console_jobs::Phase;
void check(bool value) { if (!value) throw std::runtime_error("console queue check failed"); }
template<class Work> void rejects(Work work) {
    bool rejected = false; try { work(); } catch (const std::exception &) { rejected = true; } check(rejected);
}
int main() {
    try {
        console_jobs::Queue queue;
        rejects([&] { queue.submit("", "return 1"); });
        rejects([&] { queue.submit("Test", std::string(8193, 'x')); });
        rejects([&] { queue.submit("Test", std::string("return\0x", 8)); });
        std::array<std::int64_t, 8> ids{};
        std::vector<std::thread> producers;
        for (std::size_t i = 0; i < ids.size(); ++i) producers.emplace_back([&, i] { ids[i] = queue.submit("Test", "return 1"); });
        for (auto &thread : producers) thread.join();
        std::sort(ids.begin(), ids.end());
        for (std::size_t i = 0; i < ids.size(); ++i) check(ids[i] == static_cast<std::int64_t>(i + 1));
        rejects([&] { queue.submit("Test", "return 2"); });
        for (auto id : ids) {
            check(queue.lookup(id).phase == Phase::pending);
            const auto job = queue.take(); check(job && job->id == id && job->source == "return 1");
            check(!queue.take() && queue.lookup(id).phase == Phase::pending);
            rejects([&] { queue.finish(id + 1, 0, "wrong request"); });
            queue.finish(id, 0, "1");
            const auto result = queue.lookup(id); check(result.phase == Phase::complete && result.status == 0 && result.text == "1");
        }
        for (int i = 0; i < 16; ++i) {
            const auto id = queue.submit("Test", "return 3");
            check(queue.take()->id == id); queue.finish(id, 1, std::string(20000, 'x'));
            check(queue.lookup(id).text.size() == 16384);
        }
        check(queue.lookup(1).phase == Phase::expired && queue.lookup(100).phase == Phase::unknown);
        const auto stopped = queue.submit("Test", "return 4");
        queue.close(1, "stopped");
        check(!queue.take() && queue.lookup(stopped).text == "stopped" && queue.lookup(stopped).status == 1);
        rejects([&] { queue.submit("Test", "return 5"); });
        std::cout << "Console queue concurrency/FIFO/limits/cancellation checks passed\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
