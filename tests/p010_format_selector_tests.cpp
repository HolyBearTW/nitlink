#include "capture/p010_format_selector.h"

#include <algorithm>
#include <iostream>
#include <vector>

namespace {

bool Expect(bool condition, const char* name)
{
    if (condition) return true;
    std::cerr << "FAIL: " << name << '\n';
    return false;
}

} // namespace

bool IsRate(const NitLink::P010Candidate& candidate,
            uint32_t numerator,
            uint32_t denominator)
{
    return candidate.fpsNumerator == numerator &&
           candidate.fpsDenominator == denominator;
}

int main()
{
    using namespace NitLink;
    bool pass = true;

    const std::vector<P010Candidate> gamingModes = {
        {2560, 1440, 30000, 1000},
        {1920, 1080, 60000, 1000},
        {1280, 720, 60000, 1000},
    };

    auto result = SelectGamingP010Candidate(gamingModes, 2560, 1440, 30);
    pass &= Expect(result.index == 0 &&
                   result.reason == P010SelectionReason::ExactTarget,
                   "exact target");

    result = SelectGamingP010Candidate(gamingModes, 2560, 1440, 60);
    pass &= Expect(result.index == 1 && result.usedHighFpsTier &&
                   result.reason == P010SelectionReason::HighFpsTier,
                   "target FPS outranks lower-rate target resolution");

    result = SelectGamingP010Candidate(gamingModes, 3840, 2160, 60);
    pass &= Expect(result.index == 1 && result.usedHighFpsTier &&
                   result.reason == P010SelectionReason::HighFpsTier,
                   "gaming high-FPS tier");

    const std::vector<P010Candidate> lowFpsModes = {
        {2560, 1440, 30000, 1000},
        {1920, 1080, 30000, 1000},
    };
    result = SelectGamingP010Candidate(lowFpsModes, 3840, 2160, 60);
    pass &= Expect(result.index == 0 && !result.usedHighFpsTier &&
                   result.reason ==
                       P010SelectionReason::HighestResolutionFallback,
                   "highest-resolution fallback");

    result = SelectGamingP010Candidate({}, 1920, 1080, 60);
    pass &= Expect(result.index == static_cast<size_t>(-1) &&
                   result.reason == P010SelectionReason::None,
                   "empty candidates");

    const std::vector<P010Candidate> gc553Modes = {
        {2560, 1440, 30000, 1000},
        {2560, 1440, 30000, 1001},
        {2560, 1080, 30000, 1000},
        {1920, 1080, 60000, 1000},
        {1920, 1080, 60000, 1001},
        {1920, 1080, 50000, 1000},
        {1920, 1080, 30000, 1000},
    };

    result = SelectGamingP010Candidate(gc553Modes, 1920, 1080, 60);
    pass &= Expect(result.index == 3 &&
                   IsRate(gc553Modes[result.index], 60000, 1000) &&
                   result.reason == P010SelectionReason::ExactTarget,
                   "GC553Pro target 1080p60");

    auto reversedModes = gc553Modes;
    std::reverse(reversedModes.begin(), reversedModes.end());
    result = SelectGamingP010Candidate(reversedModes, 1920, 1080, 60);
    pass &= Expect(IsRate(reversedModes[result.index], 60000, 1000) &&
                   reversedModes[result.index].width == 1920 &&
                   reversedModes[result.index].height == 1080,
                   "reversed enumeration still selects 1080p60");

    result = SelectGamingP010Candidate(gc553Modes, 2560, 1440, 0);
    pass &= Expect(IsRate(gc553Modes[result.index], 60000, 1000) &&
                   gc553Modes[result.index].width == 1920 &&
                   gc553Modes[result.index].height == 1080,
                   "unspecified FPS prefers global high-FPS mode");

    const std::vector<P010Candidate> rationalModes = {
        {2560, 1440, 30000, 1001},
        {1920, 1080, 60000, 1001},
    };
    result = SelectGamingP010Candidate(rationalModes, 1920, 1080, 60);
    pass &= Expect(result.index == 1 &&
                   IsRate(rationalModes[result.index], 60000, 1001),
                   "59.94 rational FPS remains closest to 60");

    const std::vector<P010Candidate> lowFpsRationalModes = {
        {1920, 1080, 30000, 1000},
        {1920, 1080, 29000, 1000},
    };
    result = SelectGamingP010Candidate(lowFpsRationalModes, 1920, 1080, 0);
    pass &= Expect(result.index == 0 &&
                   IsRate(lowFpsRationalModes[result.index], 30000, 1000),
                   "only 30/29 FPS falls back to 30");

    std::cout << "P010 format selector " << (pass ? "PASS" : "FAIL") << '\n';
    return pass ? 0 : 1;
}
