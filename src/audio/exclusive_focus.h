#pragma once
#include <chrono>

namespace NitLink {
// Lose access immediately; require a stable foreground before taking it back.
// Keeps transient activation gaps from repeatedly reopening the endpoint.
class ExclusiveFocusGate {
public:
    using Clock = std::chrono::steady_clock;
    bool Update(bool foreground, Clock::time_point now) {
        if (!foreground) { tracking_ = allowed_ = false; return false; }
        if (!tracking_) { tracking_ = true; since_ = now; }
        allowed_ = now - since_ >= std::chrono::milliseconds(200);
        return allowed_;
    }
private:
    bool tracking_ = false, allowed_ = false;
    Clock::time_point since_{};
};
}
