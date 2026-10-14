#pragma once

#include <chrono>

namespace NitLink {

// Start the next interval at the actual wait completion, not at an earlier
// scheduled slot. Even a small overshoot must not shorten the next interval
// to compensate for previous lateness. Render work still belongs inside the
// interval rather than adding another interval after Present returns.
// The caller supplies a positive interval from the active cap. Off and
// VSync bypass this clock; Source/Capture pacing skip the refresh wait.
// Work after the wait can vary, so this is not a minimum DXGI Present interval.
inline std::chrono::steady_clock::time_point AdvancePresentCapDeadline(
    std::chrono::steady_clock::time_point deadline,
    std::chrono::steady_clock::time_point waitCompletedAt,
    std::chrono::steady_clock::duration interval)
{
    // A caller reporting an early completion cannot advance the schedule
    // before the pending deadline. The renderer calls this after waiting.
    const auto release = deadline == std::chrono::steady_clock::time_point{}
        || waitCompletedAt >= deadline ? waitCompletedAt : deadline;
    return release + interval;
}

} // namespace NitLink
