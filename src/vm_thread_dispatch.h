#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <algorithm>
namespace budget_vm {
// Independently traced primary Thread prefix in both pinned executables:
// 0 destructor, 1 Start, 2 named Start, 3 Update, 4 command dispatch,
// 5 external program invocation, 6 completion, 7 diagnostic reporting.
using ThreadMethods = std::array<std::uintptr_t,8>;
// Only checkpoints or the pinned private Sys.wait_timer may suspend a job.
// Wait retries consume no new bytecode and run once per application update.
inline bool private_suspend(std::uint32_t flags, unsigned group, unsigned command,
    unsigned remaining, unsigned steps, unsigned quantum, bool timer) {
    if (!(flags&1) || !(flags&4) || remaining>quantum || steps>quantum) return false;
    if (group==28 && command==0) return !remaining && steps==quantum;
    return timer && group==0 && command==30;
}
inline bool private_dispatch(std::span<const std::uintptr_t> original,
    std::uintptr_t engine_dispatch, ThreadMethods &out) {
    if (original.size()!=out.size() || !engine_dispatch ||
        std::any_of(original.begin(),original.end(),[](auto address){return !address;})) return false;
    std::copy(original.begin(),original.end(),out.begin()); out[4]=engine_dispatch; return true;
}
}
