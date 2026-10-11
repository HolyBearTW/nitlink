#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace NitLink {

struct FrameRateCounters {
    uint64_t captured = 0;
    uint64_t content = 0;
    uint64_t presented = 0;
};

// Independent measurements over the same wall-clock interval. Content is an
// estimate from the GPU differ on frames consumed by the render loop; it does
// not measure the game's renderer. Present includes cadence holds/keepalives.
struct FrameRateStats {
    uint32_t captureFps = 0;
    uint32_t contentFps = 0;
    uint32_t presentFps = 0;
    bool contentAvailable = false;

    // The primary HUD metric estimates observed content changes, not the
    // game's renderer or the display's scanout. Unknown and measured zero
    // remain distinct; neither falls back to capture or presentation.
    uint32_t HudFps() const { return contentFps; }
    bool HudFpsAvailable() const { return contentAvailable; }
};

inline FrameRateStats SampleFrameRates(const FrameRateCounters& current,
                                      const FrameRateCounters& previous,
                                      double elapsedSeconds,
                                      bool contentAvailable)
{
    FrameRateStats stats;
    if (!(elapsedSeconds > 0) || !std::isfinite(elapsedSeconds)) return stats;
    stats.contentAvailable = contentAvailable && current.content >= previous.content;

    auto rate = [elapsedSeconds](uint64_t count, uint64_t before) -> uint32_t {
        // Capture buffers are rebuilt on device/format changes. A reset must
        // never wrap an unsigned delta into a huge FPS reading.
        if (count < before) return 0;
        return static_cast<uint32_t>(std::min(
            (count - before) / elapsedSeconds,
            static_cast<double>(std::numeric_limits<uint32_t>::max())));
    };
    stats.captureFps = rate(current.captured, previous.captured);
    stats.contentFps = stats.contentAvailable ? rate(current.content, previous.content) : 0;
    stats.presentFps = rate(current.presented, previous.presented);
    return stats;
}

} // namespace NitLink
