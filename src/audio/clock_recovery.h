#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace NitLink {
// Regress device progress against the timestamp associated with that position,
// NOT the thread wake time. Render positions use IAudioClock::GetFrequency;
// capture positions use the capture sample rate. Both timestamps are 100 ns.
class AudioClockRate {
public:
    void Reset() { *this = AudioClockRate{}; }
    void Sample(uint64_t position, uint64_t qpc, double frequency, double now) {
        if (!qpc || !std::isfinite(frequency) || frequency <= 0 || !std::isfinite(now)) {
            Reset(); return;
        }
        if (started_ && (position < lastPosition_ || qpc < lastQpc_ || frequency != frequency_)) Reset();
        if (!started_) {
            started_ = true; frequency_ = frequency;
            originPosition_ = lastPosition_ = position; originQpc_ = lastQpc_ = qpc;
            count_ = 1; lastSeen_ = now; return;
        }
        // Repeated/stalled positions are not fresh clock measurements.
        if (position == lastPosition_ || qpc == lastQpc_) return;
        const double elapsed = double(qpc - lastQpc_) / 1e7;
        const double progress = double(position - lastPosition_) / frequency;
        if (elapsed > 3 || progress / elapsed < 0.5 || progress / elapsed > 1.5) {
            Reset(); return;
        }
        if (elapsed < 0.01) return; // bound regression weight independently of engine period
        lastPosition_ = position; lastQpc_ = qpc; lastSeen_ = now;
        const double x = double(qpc - originQpc_) / 1e7;
        const double y = double(position - originPosition_) / frequency;
        ++count_; sx_ += x; sy_ += y; sxx_ += x*x; sxy_ += x*y;
        if (x < 2) return;
        const double denominator = count_*sxx_ - sx_*sx_;
        const double rate = denominator > 0 ? (count_*sxy_ - sx_*sy_) / denominator : 0;
        if (count_ >= 20 && std::isfinite(rate) && std::abs(rate - 1) <= 0.01) {
            rate_ = valid_ ? rate_ + 0.25*(rate - rate_) : rate;
            valid_ = true; lastEstimate_ = now;
        } else valid_ = false;
        originPosition_ = position; originQpc_ = qpc;
        count_ = 1; sx_ = sy_ = sxx_ = sxy_ = 0;
    }
    bool Valid(double now) const {
        return valid_ && now >= lastSeen_ && now-lastSeen_ < 0.5 && now-lastEstimate_ < 3;
    }
    double Rate() const { return rate_; }
private:
    bool started_ = false, valid_ = false;
    uint64_t originPosition_ = 0, originQpc_ = 0, lastPosition_ = 0, lastQpc_ = 0;
    double frequency_ = 0, lastSeen_ = 0, lastEstimate_ = 0, rate_ = 1;
    double count_ = 0, sx_ = 0, sy_ = 0, sxx_ = 0, sxy_ = 0;
};

// Worker-owned recovery policy. No setting or buffer target is changed.
// One reload for >=20 ms missing audio or 3 underruns within 2 seconds,
// or a >=20 ms (at least two output blocks) queue error lasting 250 ms.
// Cooldown survives endpoint rebuilds and only clears the observation window.
class AudioRecoveryGuard {
public:
    void ClearWindow() { lastObservation_ = windowStart_ = severeStart_ = -1; missing_ = 0; events_ = 0; }
    bool Observe(double now, double error, double missingSeconds, double blockSeconds) {
        if (!std::isfinite(now) || !std::isfinite(error) || !std::isfinite(missingSeconds) ||
            !std::isfinite(blockSeconds) || missingSeconds < 0 || blockSeconds < 0) return false;
        if (now < nextReload_) { ClearWindow(); return false; }
        if (lastObservation_ >= 0 && now-lastObservation_ > std::max(0.1, 2*blockSeconds)) severeStart_ = -1;
        lastObservation_ = now;
        if (windowStart_ < 0 || now < windowStart_ || now-windowStart_ > 2) {
            windowStart_ = now; missing_ = 0; events_ = 0;
        }
        if (missingSeconds > 0) { missing_ += missingSeconds; ++events_; }
        if (std::abs(error) >= std::max(0.020, 2*blockSeconds)) {
            if (severeStart_ < 0) severeStart_ = now;
        } else severeStart_ = -1;
        if (missing_ >= 0.020 || events_ >= 3 || (severeStart_ >= 0 && now-severeStart_ >= 0.250)) {
            nextReload_ = now+10; ClearWindow(); return true;
        }
        return false;
    }
private:
    double lastObservation_ = -1, windowStart_ = -1, severeStart_ = -1, missing_ = 0, nextReload_ = -1;
    unsigned events_ = 0;
};
}
