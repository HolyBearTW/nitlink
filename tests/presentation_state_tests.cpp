#include "app/presentation_state.h"
#include "capture/placeholder_detector.h"

#include <iostream>

using NitLink::DecidePresentation;
using NitLink::CapturePresentationReady;
using NitLink::PresentationState;
using NitLink::PlaceholderDetector;

int main()
{
    const char* transitions[] = { "NV12 -> P010", "P010 -> NV12" };
    for (const char* transition : transitions) {
        (void)transition;
        // An already visible NitLink page remains authoritative while the
        // capture session is rebuilt in either HDR direction.
        if (DecidePresentation(true, false, false, true) != PresentationState::NoSignal) return 1;
        if (DecidePresentation(true, false, false, false) != PresentationState::NoSignal) return 2;
        if (DecidePresentation(true, false, true, false) != PresentationState::NoSignal) return 3;
    }

    // A newly accepted real frame is the only event that clears the latch;
    // after that, the normal capture decision is allowed again.
    if (DecidePresentation(false, false, true, false) != PresentationState::Capture) return 4;
    if (DecidePresentation(false, false, false, true) != PresentationState::Transition) return 5;
    if (DecidePresentation(false, false, false, false) != PresentationState::WaitingForCapture) return 6;

    // A Real classification alone cannot bypass startup waiting while the
    // renderer still has no uploaded capture texture.
    if (CapturePresentationReady(true, false)) return 10;
    if (DecidePresentation(false, false,
                           CapturePresentationReady(true, false), false) !=
        PresentationState::WaitingForCapture) return 11;
    if (CapturePresentationReady(false, true)) return 12;
    if (DecidePresentation(false, false,
                           CapturePresentationReady(false, true), false) !=
        PresentationState::WaitingForCapture) return 13;

    // The first Real frame becomes Capture only after both application
    // acceptance and renderer upload are true.
    if (!CapturePresentationReady(true, true) ||
        DecidePresentation(false, false,
                           CapturePresentationReady(true, true), false) !=
            PresentationState::Capture) return 14;

    // Invalid transitional zero samples are intentionally not accepted as a
    // capture frame. They therefore cannot clear either the startup waiting
    // state or an already latched No Signal presentation.
    if (!PlaceholderDetector::ShouldUploadCaptureFrame(
            PlaceholderDetector::FrameClassification::InvalidTransitionalFrame) &&
        DecidePresentation(false, false, false, false) != PresentationState::WaitingForCapture) {
        return 7;
    }
    if (!PlaceholderDetector::ShouldUploadCaptureFrame(
            PlaceholderDetector::FrameClassification::InvalidTransitionalFrame) &&
        DecidePresentation(true, false, false, false) != PresentationState::NoSignal) {
        return 8;
    }

    // The first genuine Real frame still transitions immediately to capture.
    if (PlaceholderDetector::ShouldUploadCaptureFrame(
            PlaceholderDetector::FrameClassification::Real) &&
        DecidePresentation(false, false, true, false) != PresentationState::Capture) {
        return 9;
    }

    std::cout << "presentation state tests passed\n";
    return 0;
}
