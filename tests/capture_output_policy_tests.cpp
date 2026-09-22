#include "app/capture_output_policy.h"

#include <iostream>

using NitLink::CaptureFormatPreference;
using NitLink::DecideGc553ProOutputPolicy;
using NitLink::ApplyManualFormatPreferenceToNonGcPolicy;
using NitLink::ScopedNegotiatedCaptureFormat;
using NitLink::ShouldReopenNonGcCapture;
using NitLink::SetIntegerFrameRateFields;
using NitLink::NegotiatedCaptureFormatKind;

int main()
{
    const auto nv12HdrOff = DecideGc553ProOutputPolicy(
        NegotiatedCaptureFormatKind::NV12, false);
    if (nv12HdrOff.desiredCaptureIsP010 || nv12HdrOff.reopenCapture ||
        nv12HdrOff.sourceIsHDR10 || nv12HdrOff.sdrFromHdrTonemap) return 1;

    const auto nv12HdrOn = DecideGc553ProOutputPolicy(
        NegotiatedCaptureFormatKind::NV12, true);
    if (!nv12HdrOn.desiredCaptureIsP010 || !nv12HdrOn.reopenCapture ||
        nv12HdrOn.sourceIsHDR10 || nv12HdrOn.sdrFromHdrTonemap) return 2;

    const auto p010HdrOff = DecideGc553ProOutputPolicy(
        NegotiatedCaptureFormatKind::P010, false);
    if (!p010HdrOff.desiredCaptureIsP010 || p010HdrOff.reopenCapture ||
        !p010HdrOff.sourceIsHDR10 || !p010HdrOff.sdrFromHdrTonemap) return 3;

    const auto p010HdrOn = DecideGc553ProOutputPolicy(
        NegotiatedCaptureFormatKind::P010, true);
    if (!p010HdrOn.desiredCaptureIsP010 || p010HdrOn.reopenCapture ||
        !p010HdrOn.sourceIsHDR10 || p010HdrOn.sdrFromHdrTonemap) return 4;

    // Actual negotiated NV12 wins over a stale/requested P010 preference.
    const auto requestedP010ButNegotiatedNV12 = DecideGc553ProOutputPolicy(
        NegotiatedCaptureFormatKind::NV12, true);
    if (!requestedP010ButNegotiatedNV12.reopenCapture ||
        requestedP010ButNegotiatedNV12.sourceIsHDR10) return 5;

    const auto manualNv12HdrOn = DecideGc553ProOutputPolicy(
        NegotiatedCaptureFormatKind::NV12, true,
        CaptureFormatPreference::ManualNV12);
    if (manualNv12HdrOn.desiredCaptureIsP010 ||
        manualNv12HdrOn.reopenCapture || !manualNv12HdrOn.hdrRejected) return 6;

    const auto manualNv12HdrOff = DecideGc553ProOutputPolicy(
        NegotiatedCaptureFormatKind::NV12, false,
        CaptureFormatPreference::ManualNV12);
    if (manualNv12HdrOff.desiredCaptureIsP010 ||
        manualNv12HdrOff.reopenCapture || manualNv12HdrOff.hdrRejected) return 7;

    const auto manualP010HdrOff = DecideGc553ProOutputPolicy(
        NegotiatedCaptureFormatKind::P010, false,
        CaptureFormatPreference::ManualP010);
    if (!manualP010HdrOff.desiredCaptureIsP010 ||
        manualP010HdrOff.reopenCapture || !manualP010HdrOff.sourceIsHDR10 ||
        !manualP010HdrOff.sdrFromHdrTonemap) return 8;

    const auto manualP010HdrOn = DecideGc553ProOutputPolicy(
        NegotiatedCaptureFormatKind::P010, true,
        CaptureFormatPreference::ManualP010);
    if (!manualP010HdrOn.desiredCaptureIsP010 ||
        manualP010HdrOn.reopenCapture || !manualP010HdrOn.sourceIsHDR10 ||
        manualP010HdrOn.sdrFromHdrTonemap) return 9;

    const auto autoP010HdrOff = DecideGc553ProOutputPolicy(
        NegotiatedCaptureFormatKind::P010, false,
        CaptureFormatPreference::Auto);
    if (!autoP010HdrOff.desiredCaptureIsP010 ||
        autoP010HdrOff.reopenCapture || !autoP010HdrOff.sourceIsHDR10 ||
        !autoP010HdrOff.sdrFromHdrTonemap) return 10;

    // Auto P010 negotiation failing to NV12 remains a genuine P010 retry
    // decision, not the manual-NV12 policy rejection.
    const auto autoP010NegotiatedAsNv12 = DecideGc553ProOutputPolicy(
        NegotiatedCaptureFormatKind::NV12, true,
        CaptureFormatPreference::Auto);
    if (!autoP010NegotiatedAsNv12.reopenCapture ||
        autoP010NegotiatedAsNv12.hdrRejected) return 11;

    // Non-GC553Pro source detection must not override an accepted manual SDR
    // format and create an HDR/P010 reopen loop.
    if (ApplyManualFormatPreferenceToNonGcPolicy(
            true, CaptureFormatPreference::ManualNV12)) return 12;
    if (!ApplyManualFormatPreferenceToNonGcPolicy(
            false, CaptureFormatPreference::ManualP010)) return 13;
    if (!ApplyManualFormatPreferenceToNonGcPolicy(
            true, CaptureFormatPreference::Auto)) return 14;

    // A negotiated subtype from a previous device is not eligible for the
    // current policy; a same-device session may retain it.
    if (ScopedNegotiatedCaptureFormat(false,
                                      NegotiatedCaptureFormatKind::P010) !=
        NegotiatedCaptureFormatKind::Other) return 15;
    if (ScopedNegotiatedCaptureFormat(true,
                                      NegotiatedCaptureFormatKind::P010) !=
        NegotiatedCaptureFormatKind::P010) return 16;

    // Resolution/FPS-only override with Format=Auto may accept an SDR
    // fallback. That concrete negotiated fallback must remain stable instead
    // of reopening on every render-loop reconcile.
    if (ShouldReopenNonGcCapture(
            true, NegotiatedCaptureFormatKind::NV12, true,
            CaptureFormatPreference::Auto, true)) return 17;

    // Full Auto remains source-policy driven, so HDR preference vs NV12 still
    // requests one format transition.
    if (!ShouldReopenNonGcCapture(
            true, NegotiatedCaptureFormatKind::NV12, true,
            CaptureFormatPreference::Auto, false)) return 18;

    // Manual formats retain their explicit subtype semantics.
    if (ShouldReopenNonGcCapture(
            false, NegotiatedCaptureFormatKind::NV12, true,
            CaptureFormatPreference::ManualNV12, true)) return 19;
    if (!ShouldReopenNonGcCapture(
            true, NegotiatedCaptureFormatKind::Other, false,
            CaptureFormatPreference::Auto, true)) return 20;
    if (ShouldReopenNonGcCapture(
            true, NegotiatedCaptureFormatKind::Other, true,
            CaptureFormatPreference::Auto, true)) return 21;

    // The 4K X HDR clamp and follow-source paths both write integer rates.
    // A clamp must replace all three fields so an old 60/1 rational cannot
    // survive beside fps=30 and accidentally request 60 FPS.
    uint32_t logicalFps = 60;
    uint32_t logicalNumerator = 60000;
    uint32_t logicalDenominator = 1001;
    SetIntegerFrameRateFields(
        logicalFps, logicalNumerator, logicalDenominator, 30);
    if (logicalFps != 30 || logicalNumerator != 30 ||
        logicalDenominator != 1) return 22;

    std::cout << "capture output policy tests passed\n";
    return 0;
}
