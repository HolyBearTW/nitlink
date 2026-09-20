#pragma once

namespace NitLink {

// RequestP010() describes a future Open() attempt. The negotiated subtype
// returned by CaptureDevice::GetOutputFormat() describes the stream that is
// actually running and is therefore the only input to this runtime policy.
enum class NegotiatedCaptureFormatKind {
    Other,
    NV12,
    P010,
};

// The user's format preference is kept separate from the negotiated stream.
// An empty format override is Auto, even when dimensions or FPS are pinned.
enum class CaptureFormatPreference {
    Auto,
    ManualNV12,
    ManualP010,
    ManualOther,
};

struct Gc553ProOutputPolicy {
    bool desiredCaptureIsP010 = false;
    bool reopenCapture = false;
    bool sourceIsHDR10 = false;
    bool sdrFromHdrTonemap = false;
    bool hdrRejected = false;
};

// GC553Pro does not expose a supported vendor HDR source-state interface. This
// policy only decouples an already negotiated capture stream from the output
// preference; it does not claim that an SDR source is safe to reinterpret as
// P010.
constexpr Gc553ProOutputPolicy DecideGc553ProOutputPolicy(
    NegotiatedCaptureFormatKind actualFormat,
    bool hdrOutputEnabled,
    CaptureFormatPreference preference = CaptureFormatPreference::Auto) noexcept
{
    const bool actualP010 = actualFormat == NegotiatedCaptureFormatKind::P010;

    // A manually selected non-P010 format is a hard constraint. HDR output is
    // rejected by the caller instead of triggering a P010 request/reopen.
    if (preference == CaptureFormatPreference::ManualNV12 ||
        preference == CaptureFormatPreference::ManualOther) {
        return {
            false,
            false,
            actualP010,
            actualP010 && !hdrOutputEnabled,
            hdrOutputEnabled,
        };
    }

    // Manual P010 remains P010 for both output modes. If a previous attempt
    // did not actually negotiate P010, do not turn an output toggle into an
    // automatic retry loop; an explicit override/reopen can retry it.
    if (preference == CaptureFormatPreference::ManualP010) {
        return {
            true,
            false,
            actualP010,
            actualP010 && !hdrOutputEnabled,
            false,
        };
    }

    // Auto promotes NV12 to P010 only when HDR output is requested. Once P010
    // is actually running, retain it for SDR tone mapping as well.
    const bool desiredP010 = actualP010 || hdrOutputEnabled;
    return {
        desiredP010,
        desiredP010 != actualP010,
        actualP010,
        actualP010 && !hdrOutputEnabled,
        false,
    };
}

} // namespace NitLink
