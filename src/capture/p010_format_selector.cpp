#include "p010_format_selector.h"

#include <cmath>
#include <limits>

namespace NitLink {

static uint64_t Area(const P010Candidate& candidate)
{
    return static_cast<uint64_t>(candidate.width) * candidate.height;
}

static long double Rate(const P010Candidate& candidate)
{
    if (candidate.fpsDenominator == 0) return 0.0L;
    return static_cast<long double>(candidate.fpsNumerator) /
           static_cast<long double>(candidate.fpsDenominator);
}

static bool RateEquals(const P010Candidate& candidate, uint32_t targetFps)
{
    return candidate.fpsDenominator > 0 &&
           static_cast<uint64_t>(candidate.fpsNumerator) ==
               static_cast<uint64_t>(targetFps) * candidate.fpsDenominator;
}

static bool RateAtLeast(const P010Candidate& candidate, uint32_t targetFps)
{
    return candidate.fpsDenominator > 0 &&
           static_cast<uint64_t>(candidate.fpsNumerator) >=
               static_cast<uint64_t>(targetFps) * candidate.fpsDenominator;
}

static bool RateWithinOneBelow(const P010Candidate& candidate,
                               uint32_t targetFps)
{
    return candidate.fpsDenominator > 0 &&
           static_cast<uint64_t>(candidate.fpsNumerator) +
                   static_cast<uint64_t>(candidate.fpsDenominator) * 1 >=
               static_cast<uint64_t>(targetFps) * candidate.fpsDenominator;
}

static bool RateGreater(const P010Candidate& lhs, const P010Candidate& rhs)
{
    return Rate(lhs) > Rate(rhs);
}

P010SelectionResult SelectGamingP010Candidate(
    const std::vector<P010Candidate>& candidates,
    uint32_t targetWidth,
    uint32_t targetHeight,
    uint32_t targetFps)
{
    P010SelectionResult result;
    if (candidates.empty()) return result;

    const bool targetDimensionsKnown = targetWidth > 0 && targetHeight > 0;
    if (targetDimensionsKnown) {
        if (targetFps > 0) {
            for (size_t i = 0; i < candidates.size(); ++i) {
                const auto& candidate = candidates[i];
                if (candidate.width == targetWidth &&
                    candidate.height == targetHeight &&
                    RateEquals(candidate, targetFps)) {
                    result.index = i;
                    result.reason = P010SelectionReason::ExactTarget;
                    return result;
                }
            }
        }

        // An unspecified FPS must not reuse a previous negotiated rate. Prefer
        // a >=60 FPS mode at the requested resolution when one exists.
        size_t bestResolution = static_cast<size_t>(-1);
        if (targetFps == 0) {
            for (size_t i = 0; i < candidates.size(); ++i) {
                const auto& candidate = candidates[i];
                if (candidate.width != targetWidth ||
                    candidate.height != targetHeight ||
                    !RateAtLeast(candidate, 60)) {
                    continue;
                }
                if (bestResolution == static_cast<size_t>(-1) ||
                    RateGreater(candidate, candidates[bestResolution])) {
                    bestResolution = i;
                }
            }
            if (bestResolution != static_cast<size_t>(-1)) {
                result.index = bestResolution;
                result.reason = P010SelectionReason::TargetResolution;
                return result;
            }
        }

        // With an explicit target FPS, only use the target resolution when it
        // can satisfy the requested rate. This prevents a 30 FPS 1440p mode
        // from outranking a suitable 60 FPS 1080p mode.
        if (targetFps > 0) {
            long double bestFpsDistance = std::numeric_limits<long double>::max();
            for (size_t i = 0; i < candidates.size(); ++i) {
                const auto& candidate = candidates[i];
                if (candidate.width != targetWidth ||
                    candidate.height != targetHeight ||
                    !RateWithinOneBelow(candidate, targetFps)) {
                    continue;
                }
                const long double distance = std::abs(
                    Rate(candidate) - static_cast<long double>(targetFps));
                if (bestResolution == static_cast<size_t>(-1) ||
                    distance < bestFpsDistance ||
                    (distance == bestFpsDistance &&
                     RateGreater(candidate, candidates[bestResolution]))) {
                    bestResolution = i;
                    bestFpsDistance = distance;
                }
            }
            if (bestResolution != static_cast<size_t>(-1)) {
                result.index = bestResolution;
                result.reason = P010SelectionReason::TargetResolution;
                return result;
            }
        }
    }

    const uint32_t preferredFps = targetFps > 0 ? targetFps : 60;
    bool hasPreferredTier = false;
    for (const auto& candidate : candidates) {
        if (RateAtLeast(candidate, preferredFps)) {
            hasPreferredTier = true;
            break;
        }
    }

    size_t best = static_cast<size_t>(-1);
    if (hasPreferredTier) {
        result.usedHighFpsTier = targetFps == 0 || targetFps >= 60;
        for (size_t i = 0; i < candidates.size(); ++i) {
            const auto& candidate = candidates[i];
            if (!RateAtLeast(candidate, preferredFps)) continue;
            if (best == static_cast<size_t>(-1) ||
                Area(candidate) > Area(candidates[best]) ||
                (Area(candidate) == Area(candidates[best]) &&
                 RateGreater(candidate, candidates[best]))) {
                best = i;
            }
        }
    } else if (targetFps > 0) {
        long double bestFpsDistance = std::numeric_limits<long double>::max();
        for (size_t i = 0; i < candidates.size(); ++i) {
            const auto& candidate = candidates[i];
            const long double distance = std::abs(
                Rate(candidate) - static_cast<long double>(targetFps));
            if (best == static_cast<size_t>(-1) ||
                distance < bestFpsDistance ||
                (distance == bestFpsDistance &&
                 (RateGreater(candidate, candidates[best]) ||
                  (Rate(candidate) == Rate(candidates[best]) &&
                   Area(candidate) > Area(candidates[best]))))) {
                best = i;
                bestFpsDistance = distance;
            }
        }
    } else if (targetFps == 0) {
        // If no >=60 mode exists, prefer the highest available rate before
        // resolution. A 59.94 FPS mode is a better deterministic fallback
        // than an enumeration-first 30 FPS mode at a larger resolution.
        for (size_t i = 0; i < candidates.size(); ++i) {
            const auto& candidate = candidates[i];
            if (best == static_cast<size_t>(-1) ||
                RateGreater(candidate, candidates[best]) ||
                (Rate(candidate) == Rate(candidates[best]) &&
                 Area(candidate) > Area(candidates[best]))) {
                best = i;
            }
        }
    } else {
        for (size_t i = 0; i < candidates.size(); ++i) {
            const auto& candidate = candidates[i];
            if (best == static_cast<size_t>(-1) ||
                Area(candidate) > Area(candidates[best]) ||
                (Area(candidate) == Area(candidates[best]) &&
                 RateGreater(candidate, candidates[best]))) {
                best = i;
            }
        }
    }

    result.index = best;
    result.reason = hasPreferredTier
        ? P010SelectionReason::HighFpsTier
        : P010SelectionReason::HighestResolutionFallback;
    return result;
}

const wchar_t* P010SelectionReasonText(P010SelectionReason reason)
{
    switch (reason) {
    case P010SelectionReason::ExactTarget:
        return L"exact target resolution/FPS";
    case P010SelectionReason::TargetResolution:
        return L"target resolution with closest native FPS";
    case P010SelectionReason::HighFpsTier:
        return L"native P010 >=60 FPS tier, then highest resolution";
    case P010SelectionReason::HighestResolutionFallback:
        return L"no native P010 >=60 FPS mode, highest resolution fallback";
    case P010SelectionReason::None:
        return L"no native P010 candidate";
    }
    return L"no native P010 candidate";
}

} // namespace NitLink
