#pragma once
// Adaptive duplicate floor for the frame differ.
//
// Some capture cards do not repeat a held frame pixel-exactly: the Elgato 4K Pro at 2560x1440 delivers repeats with a
// frame-global mean difference of 0.00049-0.00069 (measured), and a UGREEN 1080p60 card on a PS5 Pro showed the same
// class of behavior (a 30 fps game read as 60 unique frames per second).
// Against the fixed 0.0005 threshold those repeats count as picture changes, so "Source frame rate" pacing presents
// every repeat and an external frame generator receives identical pairs.
//
// A 30 fps source captured at 60 Hz alternates strictly: change, repeat, change, repeat. This estimator looks at the
// last 16 differences and, when they split into two bands that alternate, remembers the low band as the card's noise
// floor. The differ then classifies against bounds raised above that floor. Real 60 fps content (no alternation),
// a static picture (no separation) and slow motion (bands too close) leave the last learned floor untouched, and the
// caller's base thresholds apply until a floor has been learned.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace NitLink {

class NoiseFloorEstimator {
public:
    static constexpr int kWindow = 16;
    static constexpr float kMaxFloor = 0.002f;      // a repeat band above this is motion, not capture noise
    static constexpr float kSeparation = 2.5f;      // high band median over low band maximum
    static constexpr float kBandSpread = 1.6f;      // low band maximum over low band minimum
    static constexpr int kMinTransitions = 13;      // of 15 steps in the window must alternate bands
    static constexpr float kLowerFactor = 1.4f;     // lower bound above the floor
    static constexpr float kUpperFactor = 2.0f;     // upper bound above the floor

    struct Bounds {
        float lower = 0.0f;
        float upper = 0.0f;
        bool adaptive = false;
    };

    void Observe(float diff) {
        if (!(diff >= 0.0f) || !std::isfinite(diff)) return;
        m_ring[m_idx] = diff;
        m_idx = (m_idx + 1) % kWindow;
        if (m_count < kWindow) ++m_count;
        if (m_count == kWindow && ++m_sinceUpdate >= kWindow / 2) {
            m_sinceUpdate = 0;
            Update();
        }
    }

    Bounds Compute(float baseUpper, float baseLower) const {
        Bounds b{baseLower, baseUpper, false};
        if (!m_valid) return b;
        b.lower = (std::max)(baseLower, m_floor * kLowerFactor);
        b.upper = (std::max)(baseUpper, m_floor * kUpperFactor);
        if (b.upper <= b.lower) b.upper = b.lower * 1.25f;
        b.adaptive = true;
        return b;
    }

    bool Valid() const { return m_valid; }
    float Floor() const { return m_floor; }
    uint32_t Updates() const { return m_updates; }

    void Reset() {
        m_idx = m_count = m_sinceUpdate = 0;
        m_floor = 0.0f;
        m_valid = false;
    }

private:
    void Update() {
        float ordered[kWindow];
        float sorted[kWindow];
        for (int i = 0; i < kWindow; ++i) {
            ordered[i] = m_ring[(m_idx + i) % kWindow];   // oldest first
            sorted[i] = ordered[i];
        }
        std::sort(sorted, sorted + kWindow);
        const float lowMin = sorted[0], lowMed = sorted[kWindow / 4], lowMax = sorted[kWindow / 2 - 1];
        const float highMed = sorted[kWindow / 2 + kWindow / 4];
        if (lowMed <= 0.0f || lowMed > kMaxFloor) return;
        if (highMed < lowMax * kSeparation) return;
        if (lowMax > (std::max)(lowMin, 1e-6f) * kBandSpread) return;
        const float split = std::sqrt(lowMed * highMed);
        int transitions = 0;
        for (int i = 1; i < kWindow; ++i)
            transitions += (ordered[i] >= split) != (ordered[i - 1] >= split);
        if (transitions < kMinTransitions) return;
        m_floor = m_valid ? 0.5f * (m_floor + lowMed) : lowMed;
        m_valid = true;
        ++m_updates;
    }

    float m_ring[kWindow] = {};
    int m_idx = 0, m_count = 0, m_sinceUpdate = 0;
    float m_floor = 0.0f;
    bool m_valid = false;
    uint32_t m_updates = 0;
};

}
