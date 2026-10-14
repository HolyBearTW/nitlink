#pragma once
#include <algorithm>
#include <cmath>
#include <limits>

namespace NitLink {
// All inputs are seconds. Integrate by elapsed time, never by wake count.
// A one-second low-pass rejects USB packet jitter; anti-windup and slew
// limits keep the correction bounded during scheduler stalls.
class DriftController {
public:
    void Reset() { clock_ = filtered_ = integral_ = ppm_ = 0; Reprime(); }
    // Preserve device-clock feed-forward and protective negative feedback across
    // a re-prime, but discard positive feedback that could repeat starvation.
    void Reprime() {
        filtered_ = 0; integral_ = std::min(integral_,0.0); ppm_ = std::min(ppm_,clock_);
        windowSeconds_ = 0; minimum_ = previousMinimum_ = Infinity;
        errorMin_ = previousErrorMin_ = Infinity; errorMax_ = previousErrorMax_ = -Infinity;
    }
    // headroomSeconds is source audio left after a nominal output block,
    // excluding FIR history/lookahead. It is NOT the configured FIFO target.
    double Update(double errorSeconds, double dt, double clockPpm = 0,
                  double headroomSeconds = Infinity) {
        if (!std::isfinite(errorSeconds) || !std::isfinite(dt) || !std::isfinite(clockPpm) || dt <= 0) return ppm_;
        dt = std::min(dt, 0.1);
        clockPpm = std::clamp(clockPpm, -2000.0, 2000.0);
        // Do not count the same learned drift twice when the clock fit arrives.
        const double clockChange = clockPpm-clock_;
        if (clockChange*integral_>0)
            integral_ -= std::copysign(std::min(std::abs(clockChange),std::abs(integral_)),integral_);
        clock_ = clockPpm;
        // Correct only errors that stay on the same side of the target across
        // a packet window. An ordinary sawtooth crossing the target is neutral.
        errorMin_ = std::min(errorMin_,errorSeconds); errorMax_ = std::max(errorMax_,errorSeconds);
        const double minError=std::min(errorMin_,previousErrorMin_);
        const double maxError=std::max(errorMax_,previousErrorMax_);
        const double sustainedError=minError>0 ? minError : (maxError<0 ? maxError : 0);
        filtered_ += (1 - std::exp(-dt)) * (sustainedError - filtered_);
        // Packet peaks must not authorize draining through the following trough.
        // Two adjacent 250 ms windows remember troughs across window boundaries.
        if (std::isfinite(headroomSeconds)) minimum_ = std::min(minimum_,headroomSeconds);
        const double low = std::min(minimum_,previousMinimum_);
        windowSeconds_ += dt;
        if (windowSeconds_ >= .25) {
            previousMinimum_ = minimum_; minimum_ = Infinity; windowSeconds_ = 0;
            previousErrorMin_ = errorMin_; previousErrorMax_ = errorMax_;
            errorMin_ = Infinity; errorMax_ = -Infinity;
        }
        const double error = std::min(filtered_,low-.0005);
        // A safety brake can remove stale positive feedback immediately; normal
        // tracking still slews. This does not change either user buffer target.
        const double ceiling = low < .001
            ? std::clamp(clockPpm+(low-.001)*2000000,-2000.0,2000.0) : 2000.0;
        if (low <= .0005) integral_ = std::min(integral_,0.0);
        // Back-calculate the integrator as well, so it cannot immediately undo
        // the brake when the low-water observation expires.
        if (ppm_ > ceiling)
            integral_ = std::min(integral_,std::clamp(ceiling-clockPpm,-1500.0,1500.0));
        ppm_ = std::min(ppm_,ceiling);
        const double next = std::clamp(integral_ + error * dt * 20000, -1500.0, 1500.0);
        const double raw = clockPpm + error * 250000 + next;
        if (std::abs(raw) < 2000 || raw * error < 0) integral_ = next;
        const double target = std::clamp(clockPpm + error * 250000 + integral_, -2000.0, ceiling);
        ppm_ += std::clamp(target - ppm_, -400 * dt, 400 * dt);
        return ppm_;
    }
private:
    static constexpr double Infinity = std::numeric_limits<double>::infinity();
    double filtered_ = 0, integral_ = 0, ppm_ = 0;
    double clock_ = 0, windowSeconds_ = 0, minimum_ = Infinity, previousMinimum_ = Infinity;
    double errorMin_ = Infinity, previousErrorMin_ = Infinity;
    double errorMax_ = -Infinity, previousErrorMax_ = -Infinity;
};
// Four-point cubic interpolation. For drift correction only (near unity),
// not a general sample-rate converter. Windows handles nominal rate changes.
inline double DriftCubic(double a, double b, double c, double d, double t) {
    return b + 0.5 * t * (c - a + t * (2*a - 5*b + 4*c - d + t * (3*(b-c) + d-a)));
}
}
