// CPU tests for the adaptive duplicate floor (src/capture/noise_floor.h), plus an optional replay of a recorded
// per-frame trace (a CSV with a "difference" column holding the differ's frame-global mean) that prints the learned
// floor and the resulting unique-frame rate through the differ's stage-1 hysteresis.
#include "noise_floor.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <limits>

using NitLink::NoiseFloorEstimator;

static int checks = 0;
static void Require(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}

// Stage-1 hysteresis of FrameDiffer::Process with the estimator's bounds; returns the committed "new" votes.
static std::vector<bool> Classify(const std::vector<float>& diffs, NoiseFloorEstimator& est, float baseUpper = 0.0005f, float baseLower = 0.0002f) {
    std::vector<bool> votes;
    bool wasNew = true;
    for (float d : diffs) {
        est.Observe(d);
        const auto b = est.Compute(baseUpper, baseLower);
        bool raw = d >= b.upper ? true : (d < b.lower ? false : wasNew);
        wasNew = raw;
        votes.push_back(raw);
    }
    return votes;
}

static std::vector<float> Alternating(float low, float high, int n, float jitter = 0.0f) {
    std::vector<float> v;
    for (int i = 0; i < n; ++i) {
        const float j = jitter * ((i * 7919) % 13 - 6) / 6.0f;
        v.push_back(i % 2 ? low * (1.0f + j) : high * (1.0f + j));
    }
    return v;
}

int main(int argc, char** argv) {
    {   // 4K Pro at 1440p: repeats 0.00066, slow pan 0.0034 -> floor learned, repeats rejected, motion kept
        NoiseFloorEstimator e;
        const auto votes = Classify(Alternating(0.00066f, 0.0034f, 120, 0.03f), e);
        Require(e.Valid() && e.Floor() > 0.0005f && e.Floor() < 0.0009f, "1440p floor learned near 0.00066");
        int news = 0; for (size_t i = 60; i < votes.size(); ++i) news += votes[i];
        Require(news == 30, "after learning, exactly half of the frames are new (30 of 60)");
        const auto b = e.Compute(0.0005f, 0.0002f);
        Require(b.adaptive && b.lower > 0.0008f && b.upper >= 0.0012f && b.upper < 0.0019f, "bounds sit above the noise and under the smallest measured motion");
    }
    {   // the same card during a slow pan at 0.00187 (ratio 2.8): floor still learned (separation 2.5), repeats rejected
        NoiseFloorEstimator e;
        const auto votes = Classify(Alternating(0.00066f, 0.00187f, 120, 0.02f), e);
        Require(e.Valid(), "slow-pan window still learns the floor");
        int news = 0; for (size_t i = 60; i < votes.size(); ++i) news += votes[i];
        Require(news == 30, "slow pan: half new");
    }
    {   // UGREEN-like: noisier repeats 0.0012, 30 fps game 0.03
        NoiseFloorEstimator e;
        const auto votes = Classify(Alternating(0.0012f, 0.03f, 120, 0.05f), e);
        Require(e.Valid() && e.Floor() > 0.001f && e.Floor() < 0.0014f, "noisier card floor learned");
        int news = 0; for (size_t i = 60; i < votes.size(); ++i) news += votes[i];
        Require(news == 30, "noisier card: half new");
    }
    {   // exact repeats (1080p/4K): low band 0 -> no floor, the base thresholds apply unchanged
        NoiseFloorEstimator e;
        const auto votes = Classify(Alternating(0.0f, 0.02f, 120), e);
        Require(!e.Valid(), "exact repeats learn no floor");
        int news = 0; for (size_t i = 60; i < votes.size(); ++i) news += votes[i];
        Require(news == 30, "exact repeats: half new with base thresholds");
    }
    {   // real 60 fps content with mixed motion: no alternation -> no floor, every frame new
        NoiseFloorEstimator e;
        std::vector<float> v;
        for (int i = 0; i < 120; ++i) v.push_back(0.002f + 0.03f * ((i * 7919) % 17) / 17.0f);
        const auto votes = Classify(v, e);
        Require(!e.Valid(), "60 fps mixed motion learns no floor");
        int news = 0; for (bool b : votes) news += b;
        Require(news == 120, "60 fps content stays all new");
    }
    {   // static picture with noise 0.0006 everywhere: no separation -> no floor (the base rule decides)
        NoiseFloorEstimator e;
        std::vector<float> v(120, 0.0006f);
        Classify(v, e);
        Require(!e.Valid(), "static noise learns no floor");
    }
    {   // a learned floor survives a 60 fps or static stretch and a slow pan; Reset forgets it
        NoiseFloorEstimator e;
        Classify(Alternating(0.00066f, 0.0034f, 64, 0.02f), e);
        Require(e.Valid(), "learned");
        std::vector<float> v(64, 0.02f);
        Classify(v, e);
        Require(e.Valid() && e.Floor() > 0.0005f, "floor kept through 60 fps content");
        e.Reset();
        Require(!e.Valid() && e.Floor() == 0.0f, "reset forgets the floor");
    }
    {   // a repeat band above kMaxFloor is motion, never a floor (a 60 fps game alternating 0.003/0.03 must not lose frames)
        NoiseFloorEstimator e;
        const auto votes = Classify(Alternating(0.003f, 0.03f, 120), e);
        Require(!e.Valid(), "bands above the noise ceiling are not a floor");
        int news = 0; for (bool b : votes) news += b;
        Require(news == 120, "alternating real motion keeps every frame");
    }
    {   // Do not infer repeats from two bands without temporal alternation.
        NoiseFloorEstimator e;
        std::vector<float> blocks;
        for (int i = 0; i < 128; ++i) blocks.push_back(i % 16 < 8 ? 0.00065f : 0.02f);
        Classify(blocks, e);
        Require(!e.Valid(), "grouped small and large motion is not a repeat cadence");
        Classify(Alternating(0.0006f, 0.001f, 128), e);
        Require(!e.Valid(), "nearby slow-motion bands cannot establish a floor");
    }
    {   // A broad low band and invalid GPU values cannot train the estimator.
        NoiseFloorEstimator e;
        auto diffs = Alternating(0.00065f, 0.02f, 128);
        for (size_t i = 1; i < diffs.size(); i += 4) diffs[i] *= 0.4f;
        Classify(diffs, e);
        Require(!e.Valid(), "broad low band rejected");
        for (int i = 0; i < 64; ++i) {
            e.Observe(-1.0f);
            e.Observe(std::numeric_limits<float>::quiet_NaN());
            e.Observe(std::numeric_limits<float>::infinity());
        }
        Require(!e.Valid(), "invalid samples never learn a floor");
        Classify(Alternating(0.00065f, 0.02f, 128), e);
        Require(e.Valid(), "valid observations recover after invalid samples");
        const auto tuned = e.Compute(0.01f, 0.003f);
        Require(tuned.upper == 0.01f && tuned.lower == 0.003f,
                "adaptive bounds never lower explicitly tuned thresholds");
        e.Reset();
        for (int i = 0; i < 15; ++i) e.Observe(i % 2 ? 0.00065f : 0.02f);
        Require(!e.Valid(), "capture reset must collect a fresh complete window");
    }
    if (argc > 1) {   // replay a recorded trace: print floor and unique rate per second
        std::ifstream in(argv[1]);
        std::string line; std::getline(in, line);
        int col = -1; { std::stringstream h(line); std::string c; int i = 0; while (std::getline(h, c, ',')) { if (c == "difference") col = i; ++i; } }
        Require(col >= 0, "difference column present");
        std::vector<float> diffs;
        while (std::getline(in, line)) {
            std::stringstream s(line); std::string c; int i = 0;
            while (std::getline(s, c, ',')) { if (i == col) diffs.push_back(std::strtof(c.c_str(), nullptr)); ++i; }
        }
        NoiseFloorEstimator e;
        const auto votes = Classify(diffs, e);
        std::printf("replay %s: frames %zu floor %.5f valid %d updates %u\n", argv[1], diffs.size(), e.Floor(), e.Valid(), e.Updates());
        for (size_t start = 0; start + 60 <= votes.size(); start += 600) {
            int news = 0; for (size_t i = start; i < start + 60; ++i) news += votes[i];
            std::printf("  frames %5zu-%5zu: %d new of 60\n", start, start + 60, news);
        }
    }
    std::printf("{\"status\":\"NOISE_FLOOR_PASS\",\"checks\":%d}\n", checks);
    return 0;
}
