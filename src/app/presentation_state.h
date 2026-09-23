#pragma once

namespace NitLink {

// Single render decision used by both SDR and HDR paths. The no-signal latch
// deliberately has priority over transition and capture readiness so a
// format reconcile cannot expose an invalidated capture surface.
// Waiting and NoSignal are independent presentations; they do not require a
// fresh capture frame.
enum class PresentationState {
    WaitingForCapture,
    Capture,
    NoSignal,
    Transition,
};

// A classified Real frame is not presentable until the renderer has accepted
// it into the current GPU capture texture. This prevents the render loop from
// selecting Capture while DrawCaptureFrame still has no usable surface.
// This applies to the current capture session.
constexpr bool CapturePresentationReady(bool acceptedReal,
                                        bool rendererHasFrame) noexcept
{
    return acceptedReal && rendererHasFrame;
}

constexpr PresentationState DecidePresentation(bool noSignalLatched,
                                                bool showNoSignalNow,
                                                bool captureReady,
                                                bool transitionActive) noexcept
{
    if (noSignalLatched || showNoSignalNow) return PresentationState::NoSignal;
    if (transitionActive) return PresentationState::Transition;
    if (captureReady) return PresentationState::Capture;
    return PresentationState::WaitingForCapture;
}

// A zero-zone GC553Pro frame can be legal FULL-range black content. Keep it
// accepted as Real; this state only controls a temporary presentation hint.
// The hint ends when visible content arrives or the known placeholder is
// confirmed, and does not affect source liveness or detector classification.
struct StartupBlackFrameHint {
    bool eligible = true;
    bool visible = false;

    constexpr void ObserveFrame(bool acceptedReal, bool zeroLumaZones) noexcept {
        if (!eligible || !acceptedReal) return;
        if (zeroLumaZones) {
            visible = true;
        } else {
            eligible = false;
            visible = false;
        }
    }

    constexpr void ConfirmPlaceholder() noexcept {
        eligible = false;
        visible = false;
    }

    constexpr void Reset() noexcept {
        eligible = true;
        visible = false;
    }
};

constexpr bool ShouldDrawStartupBlackFrameHint(bool isGc553Pro,
                                               PresentationState state,
                                               const StartupBlackFrameHint& hint) noexcept
{
    return isGc553Pro && state == PresentationState::Capture && hint.visible;
}

} // namespace NitLink
