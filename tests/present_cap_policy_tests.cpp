#include "app/present_cap_policy.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main()
{
    using namespace NitLink;
    try {
        const auto before = ChoosePresentCap({0, 120.0, 60.0});
        const auto after = ChoosePresentCap({0, 120.0, 120.0});
        Require(before.effectiveHz == 117.0, "1080p60 retains the existing automatic headroom cap");
        Require(after.effectiveHz == 120.0,
                "1080p60 -> 1080p120 must not turn Auto into uncapped Present");
        Require(ChoosePresentCap({0, 120.0, 60.0}).effectiveHz == before.effectiveHz,
                "1080p120 -> 1080p60 restores the headroom cap");

        unsigned combinations = 0;
        for (double capture : {60.0, 120.0}) {
            for (double monitor : {24.0, 30.0, 49.0, 59.94, 60.0, 119.88, 119.998, 120.0, 144.0, 180.0}) {
                for (int configured : {0, -1, 95}) {
                    for (bool vsync : {false, true}) {
                        for (bool lowLatency : {false, true}) {
                            for (int pacing : {kPacingRefresh, kPacingCaptured, kPacingUnique}) {
                                for (bool rateKnown : {false, true}) {
                                    for (bool marker : {false, true}) {
                                        PresentCapInput input{configured, monitor,
                                            rateKnown ? capture : 0.0, pacing, vsync, marker, 141.0};
                                        const auto decision = ChoosePresentCap(input);
                                        double expected = 0.0;
                                        if (!vsync && pacing == kPacingRefresh) {
                                            if (marker) expected = 141.0;
                                            else if (configured > 0) expected = configured;
                                            else if (configured == 0) {
                                                expected = !rateKnown || capture <= monitor - 3.0
                                                    ? monitor - 3.0 : monitor;
                                            }
                                        }
                                        Require(decision.effectiveHz == expected,
                                                "cap matrix must preserve explicit caps, marker priority and bypasses");
                                        const auto wait = ChoosePresentWaitPoint(pacing, lowLatency);
                                        Require(wait == (pacing != kPacingRefresh ? PresentWaitPoint::None
                                                : lowLatency ? PresentWaitPoint::BeforeCapture : PresentWaitPoint::BeforeRender),
                                                "low latency only changes the location of the same refresh-mode wait");
                                        ++combinations;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        Require(ChoosePresentCap({0, 60.0, 120.0}).effectiveHz == 60.0,
                "capture faster than the monitor still has an automatic display ceiling");
        Require(ChoosePresentCap({0, 180.0, 120.0}).effectiveHz == 177.0,
                "a real 180Hz display must retain 177Hz for a 120fps source");
        Require(ChoosePresentCap({0, 119.998, 10000000.0 / 83333.0}).effectiveHz == 119.998,
                "fractional MF and display rates must not disable Auto");
        for (const double monitor : {59.94, 60.0, 119.88, 119.998, 120.0, 144.0}) {
            const auto decision = ChoosePresentCap({0, monitor, 120.0});
            const double expected = monitor - 3.0 >= 120.0 ? monitor - 3.0 : monitor;
            Require(decision.requestedHz == expected && decision.effectiveHz == expected,
                    "fractional display ceilings retain precision and source-admissible headroom");
            Require(decision.effectiveHz <= monitor,
                    "a source faster than the physical monitor cannot force Auto above its ceiling");
        }
        Require(ChoosePresentCap({0, 59.94, 60.0}).effectiveHz == 59.94,
                "59.94Hz / 60fps uses the actual ceiling without rounding or flooring");
        Require(ChoosePresentCap({0, 119.88, 120.0}).effectiveHz == 119.88,
                "119.88Hz / 120fps uses the actual ceiling without rounding or flooring");
        Require(ChoosePresentCap({0, 120.0, 117.0}).effectiveHz == 117.0 &&
                ChoosePresentCap({0, 120.0, 117.001}).effectiveHz == 120.0,
                "the source boundary selects headroom only when it can accommodate the source");
        for (const double monitor : {1.0, 2.0, 24.0, 30.0, 49.0}) {
            Require(ChoosePresentCap({0, monitor, 60.0}).effectiveHz == monitor,
                    "a reliably queried low refresh rate retains its physical ceiling");
        }
        Require(ChoosePresentCap({0, 2.0, 0.0}).effectiveHz == 2.0,
                "unknown source cannot select nonpositive headroom on a low refresh display");
        Require(ChoosePresentCap({0, 120.0, 0.0}).effectiveHz == 117.0,
                "unknown MF rate is not the user's requested or measured capture rate");
        Require(ChoosePresentCap({0, 120.0, 120.0}).effectiveHz == 120.0,
                "a retained operational hint for missing MF metadata cannot remove Auto");
        for (double bad : {-1.0, 0.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
            const auto decision = ChoosePresentCap({0, bad, 120.0});
            Require(decision.effectiveHz == kUnknownDisplayPresentCapHz &&
                    decision.reason == PresentCapReason::UnknownDisplayFallback,
                    "unknown display refresh selects an explicit conservative fallback without unlimiting Auto");
            Require(ChoosePresentCap({95, bad, 120.0}).effectiveHz == 95.0,
                    "manual caps do not depend on querying display refresh");
            Require(ChoosePresentCap({-1, bad, 120.0}).effectiveHz == 0.0,
                    "Explicit Off still requests uncapped presentation when the display is unknown");
            for (bool vsync : {false, true}) {
                for (int pacing : {kPacingRefresh, kPacingCaptured, kPacingUnique}) {
                    const bool bypass = vsync || pacing != kPacingRefresh;
                    Require(ChoosePresentCap({0, bad, 120.0, pacing, vsync, true, 141.0}).effectiveHz ==
                            (bypass ? 0.0 : 141.0),
                            "unknown monitor preserves marker priority and VSync/source/capture bypasses");
                    Require(ChoosePresentCap({0, bad, 120.0, pacing, vsync}).effectiveHz ==
                            (bypass ? 0.0 : kUnknownDisplayPresentCapHz),
                            "conservative fallback only applies to refresh pacing with VSync off");
                }
            }
        }
        Require(ChoosePresentCap({0, 120.0, 0.0, kPacingRefresh, false, true, 117.0}).effectiveHz == 117.0,
                "the legacy marker default remains authoritative");
        std::cout << "PASS Present cap transition, " << combinations
                  << " combinations, fractional/unknown rates and wait locations\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
