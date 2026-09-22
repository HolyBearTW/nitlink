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

} // namespace NitLink
