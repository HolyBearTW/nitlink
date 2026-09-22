#include "capture/placeholder_detector.h"

#include <iostream>
#include <algorithm>
#include <cstdint>
#include <vector>

using NitLink::PlaceholderDetector;

namespace {

bool Confirm(PlaceholderDetector::Fingerprint fp,
             PlaceholderDetector::CaptureFormatKind format,
             PlaceholderDetector::PlaceholderDeviceFamily family)
{
    PlaceholderDetector detector;
    for (int i = 0; i < 15; ++i) {
        const auto classification = detector.Process(fp, format, family);
        if (i < 14 && classification != PlaceholderDetector::FrameClassification::CandidatePlaceholder) {
            return false;
        }
        if (i == 14 && classification != PlaceholderDetector::FrameClassification::ConfirmedPlaceholder) {
            return false;
        }
    }
    return detector.IsCurrentlyPlaceholder();
}

} // anonymous namespace

uint16_t ReadU16(const std::vector<uint8_t>& bytes, size_t offset)
{
    return static_cast<uint16_t>(bytes[offset]) |
           (static_cast<uint16_t>(bytes[offset + 1]) << 8);
}

void WriteU16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value)
{
    bytes[offset] = static_cast<uint8_t>(value & 0xFF);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

bool TestTransitionalSamples()
{
    constexpr uint32_t width = 16;
    constexpr uint32_t height = 16;

    // A zero-filled NV12 sample is invalid because both Y and UV are zero.
    const size_t nv12YBytes = static_cast<size_t>(width) * height;
    std::vector<uint8_t> nv12(nv12YBytes + width * (height / 2), 0);
    if (!PlaceholderDetector::IsInvalidTransitionalFrame(
            nv12.data(), static_cast<uint32_t>(nv12.size()), width, height,
            PlaceholderDetector::CaptureFormatKind::NV12)) {
        return false;
    }

    // Legal limited-range black has Y=16 and neutral Cb/Cr=128.
    std::fill(nv12.begin(), nv12.begin() + nv12YBytes, uint8_t{16});
    std::fill(nv12.begin() + nv12YBytes, nv12.end(), uint8_t{128});
    if (PlaceholderDetector::IsInvalidTransitionalFrame(
            nv12.data(), static_cast<uint32_t>(nv12.size()), width, height,
            PlaceholderDetector::CaptureFormatKind::NV12)) {
        return false;
    }

    // A zero-filled P010 sample is invalid for the same structural reason.
    const size_t p010YBytes = static_cast<size_t>(width) * height * 2;
    std::vector<uint8_t> p010(p010YBytes + width * (height / 2) * 2, 0);
    if (!PlaceholderDetector::IsInvalidTransitionalFrame(
            p010.data(), static_cast<uint32_t>(p010.size()), width, height,
            PlaceholderDetector::CaptureFormatKind::P010)) {
        return false;
    }

    // Legal full-range black has Y=0 and neutral 10-bit Cb/Cr=512.
    for (size_t offset = p010YBytes; offset < p010.size(); offset += 2) {
        WriteU16(p010, offset, static_cast<uint16_t>(512u << 6));
    }
    if (PlaceholderDetector::IsInvalidTransitionalFrame(
            p010.data(), static_cast<uint32_t>(p010.size()), width, height,
            PlaceholderDetector::CaptureFormatKind::P010)) {
        return false;
    }
    if (ReadU16(p010, p010YBytes) != static_cast<uint16_t>(512u << 6)) {
        return false;
    }

    // The gate is intentionally YUV-only; an all-zero BGRA frame is not
    // reinterpreted as an invalid transitional sample.
    std::vector<uint8_t> bgra(width * height * 4, 0);
    if (PlaceholderDetector::IsInvalidTransitionalFrame(
            bgra.data(), static_cast<uint32_t>(bgra.size()), width, height,
            PlaceholderDetector::CaptureFormatKind::BGRA)) {
        return false;
    }
    return true;
}

bool TestGc553ProNeutralTransitionalSamples()
{
    constexpr uint32_t width = 16;
    constexpr uint32_t height = 16;
    constexpr auto family =
        PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro;

    const size_t p010YBytes = static_cast<size_t>(width) * height * 2;
    std::vector<uint8_t> p010(p010YBytes + width * (height / 2) * 2, 0);

    // This is the verified GC553Pro pre-first-Real P010 LIMITED pattern:
    // raw Y=0x0000 and neutral raw UV=0x8000/0x8000.
    for (size_t offset = p010YBytes; offset < p010.size(); offset += 2) {
        WriteU16(p010, offset, 0x8000);
    }
    if (!PlaceholderDetector::IsGc553ProNeutralTransitionalFrame(
            p010.data(), static_cast<uint32_t>(p010.size()), width, height,
            PlaceholderDetector::CaptureFormatKind::P010, family,
            true, false)) {
        return false;
    }

    // Legal P010 LIMITED black is luma code 64, represented as raw 0x1000.
    for (size_t offset = 0; offset < p010YBytes; offset += 2) {
        WriteU16(p010, offset, 0x1000);
    }
    if (PlaceholderDetector::IsGc553ProNeutralTransitionalFrame(
            p010.data(), static_cast<uint32_t>(p010.size()), width, height,
            PlaceholderDetector::CaptureFormatKind::P010, family,
            true, false)) {
        return false;
    }

    // Y=0x0000 with neutral chroma is legal full-range black, so the
    // LIMITED-only startup rule must not classify it as transitional.
    std::fill(p010.begin(), p010.begin() + p010YBytes, uint8_t{0});
    if (PlaceholderDetector::IsGc553ProNeutralTransitionalFrame(
            p010.data(), static_cast<uint32_t>(p010.size()), width, height,
            PlaceholderDetector::CaptureFormatKind::P010, family,
            false, false)) {
        return false;
    }

    // The same raw pattern after a genuine Real frame has been accepted is
    // ordinary source data for this startup-only gate.
    if (PlaceholderDetector::IsGc553ProNeutralTransitionalFrame(
            p010.data(), static_cast<uint32_t>(p010.size()), width, height,
            PlaceholderDetector::CaptureFormatKind::P010, family,
            true, true)) {
        return false;
    }

    // The optional symmetric NV12 rule is equally narrow: only GC553Pro,
    // LIMITED range, and before the first accepted Real frame.
    const size_t nv12YBytes = static_cast<size_t>(width) * height;
    std::vector<uint8_t> nv12(nv12YBytes + width * (height / 2), 0);
    std::fill(nv12.begin() + nv12YBytes, nv12.end(), uint8_t{0x80});
    if (!PlaceholderDetector::IsGc553ProNeutralTransitionalFrame(
            nv12.data(), static_cast<uint32_t>(nv12.size()), width, height,
            PlaceholderDetector::CaptureFormatKind::NV12, family,
            true, false)) {
        return false;
    }

    // FULL-range legal black remains valid before the first accepted frame.
    if (PlaceholderDetector::IsGc553ProNeutralTransitionalFrame(
            nv12.data(), static_cast<uint32_t>(nv12.size()), width, height,
            PlaceholderDetector::CaptureFormatKind::NV12, family,
            false, false)) {
        return false;
    }
    if (PlaceholderDetector::IsGc553ProNeutralTransitionalFrame(
            nv12.data(), static_cast<uint32_t>(nv12.size()), width, height,
            PlaceholderDetector::CaptureFormatKind::NV12, family,
            false, true)) {
        return false;
    }
    if (PlaceholderDetector::IsGc553ProNeutralTransitionalFrame(
            nv12.data(), static_cast<uint32_t>(nv12.size()), width, height,
            PlaceholderDetector::CaptureFormatKind::NV12,
            PlaceholderDetector::PlaceholderDeviceFamily::Elgato,
            false, false) ||
        PlaceholderDetector::IsGc553ProNeutralTransitionalFrame(
            nv12.data(), static_cast<uint32_t>(nv12.size()), width, height,
            PlaceholderDetector::CaptureFormatKind::NV12,
            PlaceholderDetector::PlaceholderDeviceFamily::Unknown,
            false, false)) {
        return false;
    }

    // The structural transitional predicate intentionally has its own fixed
    // sample points. A non-zero pixel at a fingerprint-only point must not
    // make the startup frame uploadable; the old application-side all-zero
    // fingerprint gate incorrectly allowed that case through.
    const size_t yOffsetOutsideStructuralSamples = 1u * width + 1u;
    // Compute() averages 16 samples per zone; use a saturated value so this
    // single fingerprint-only point remains non-zero after integer division.
    nv12[yOffsetOutsideStructuralSamples] = 0xFF;
    // A tiny loading logo outside every structural sample must also remain
    // valid in FULL range; the range gate protects it, not a lucky sample.
    if (PlaceholderDetector::IsGc553ProNeutralTransitionalFrame(
            nv12.data(), static_cast<uint32_t>(nv12.size()), width, height,
            PlaceholderDetector::CaptureFormatKind::NV12, family,
            false, false)) return false;
    const auto nonZeroFingerprint = PlaceholderDetector::Compute(
        nv12.data(), static_cast<uint32_t>(nv12.size()), width, height,
        PlaceholderDetector::CaptureFormatKind::NV12);
    bool fingerprintHasNonZero = false;
    for (const uint8_t value : nonZeroFingerprint) {
        if (value != 0) {
            fingerprintHasNonZero = true;
            break;
        }
    }
    if (!fingerprintHasNonZero ||
        !PlaceholderDetector::IsGc553ProNeutralTransitionalFrame(
            nv12.data(), static_cast<uint32_t>(nv12.size()), width, height,
            PlaceholderDetector::CaptureFormatKind::NV12, family,
            true, false)) {
        return false;
    }
    return true;
}

int main()
{
    if (!TestTransitionalSamples()) return 24;
    if (!TestGc553ProNeutralTransitionalSamples()) return 29;

    const PlaceholderDetector::Fingerprint elgato{
        0x10, 0x10, 0x10, 0x10, 0x1D, 0x10, 0x10, 0x10, 0x10};
    const PlaceholderDetector::Fingerprint gc553ProNV12VariantA{
        0x00, 0x0C, 0x00, 0x04, 0x05, 0x03, 0x2D, 0x1C, 0x15};
    const PlaceholderDetector::Fingerprint gc553Pro{
        0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x43, 0x2F, 0x24};
    const PlaceholderDetector::Fingerprint gc553ProP010{
        0x00, 0x0D, 0x00, 0x03, 0x04, 0x02, 0x2E, 0x20, 0x18};

    if (!Confirm(elgato, PlaceholderDetector::CaptureFormatKind::P010,
                 PlaceholderDetector::PlaceholderDeviceFamily::Elgato)) return 1;
    if (Confirm(elgato, PlaceholderDetector::CaptureFormatKind::P010,
                PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro)) return 2;
    if (!Confirm(gc553Pro, PlaceholderDetector::CaptureFormatKind::NV12,
                 PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro)) return 3;
    if (!Confirm(gc553ProNV12VariantA, PlaceholderDetector::CaptureFormatKind::NV12,
                 PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro)) return 30;
    if (!Confirm(gc553ProP010, PlaceholderDetector::CaptureFormatKind::P010,
                 PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro)) return 4;
    if (!PlaceholderDetector::ShouldUploadCaptureFrame(
            PlaceholderDetector::FrameClassification::Real)) return 5;
    if (PlaceholderDetector::ShouldUploadCaptureFrame(
            PlaceholderDetector::FrameClassification::CandidatePlaceholder)) return 6;
    if (PlaceholderDetector::ShouldUploadCaptureFrame(
            PlaceholderDetector::FrameClassification::ConfirmedPlaceholder)) return 7;
    if (Confirm(gc553Pro, PlaceholderDetector::CaptureFormatKind::NV12,
                PlaceholderDetector::PlaceholderDeviceFamily::Elgato)) return 8;
    if (Confirm(gc553Pro, PlaceholderDetector::CaptureFormatKind::P010,
                PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro)) return 9;
    if (Confirm(gc553Pro, PlaceholderDetector::CaptureFormatKind::BGRA,
                PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro)) return 10;
    if (Confirm(gc553Pro, PlaceholderDetector::CaptureFormatKind::NV12,
                PlaceholderDetector::PlaceholderDeviceFamily::Unknown)) return 11;

    PlaceholderDetector capabilityDetector;
    if (!capabilityDetector.HasKnownPlaceholder(
            PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro,
            PlaceholderDetector::CaptureFormatKind::NV12)) return 12;
    if (!capabilityDetector.HasKnownPlaceholder(
            PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro,
            PlaceholderDetector::CaptureFormatKind::P010)) return 13;
    if (capabilityDetector.HasKnownPlaceholder(
            PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro,
            PlaceholderDetector::CaptureFormatKind::BGRA)) return 14;
    if (capabilityDetector.HasKnownPlaceholder(
            PlaceholderDetector::PlaceholderDeviceFamily::Unknown,
            PlaceholderDetector::CaptureFormatKind::NV12)) return 15;

    PlaceholderDetector detector;
    for (int i = 0; i < 14; ++i) {
        if (detector.Process(gc553Pro, PlaceholderDetector::CaptureFormatKind::NV12,
                             PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro) !=
            PlaceholderDetector::FrameClassification::CandidatePlaceholder) return 16;
    }

    // An invalid transitional sample is skipped by the application-side gate,
    // so it neither uploads nor consumes a placeholder streak slot. The next
    // matching frame must still be the 15th match and confirm normally.
    PlaceholderDetector streakAfterInvalid;
    for (int i = 0; i < 14; ++i) {
        if (streakAfterInvalid.Process(
                gc553Pro, PlaceholderDetector::CaptureFormatKind::NV12,
                PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro) !=
            PlaceholderDetector::FrameClassification::CandidatePlaceholder) return 25;
    }
    const std::vector<uint8_t> zeroNv12(16 * 16 + 16 * 8, 0);
    if (!PlaceholderDetector::IsInvalidTransitionalFrame(
            zeroNv12.data(), static_cast<uint32_t>(zeroNv12.size()), 16, 16,
            PlaceholderDetector::CaptureFormatKind::NV12)) return 26;
    if (PlaceholderDetector::ShouldUploadCaptureFrame(
            PlaceholderDetector::FrameClassification::InvalidTransitionalFrame)) return 27;
    if (streakAfterInvalid.Process(
            gc553Pro, PlaceholderDetector::CaptureFormatKind::NV12,
            PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro) !=
        PlaceholderDetector::FrameClassification::ConfirmedPlaceholder) return 28;
    auto drifting = gc553Pro;
    drifting[6] = 0x46; // within match tolerance, outside temporal stability
    if (detector.Process(drifting, PlaceholderDetector::CaptureFormatKind::NV12,
                         PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro) !=
        PlaceholderDetector::FrameClassification::Real) return 17;
    auto changed = gc553Pro;
    changed[6] = 0x60;
    if (detector.Process(changed, PlaceholderDetector::CaptureFormatKind::NV12,
                         PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro) !=
        PlaceholderDetector::FrameClassification::Real) return 18;
    if (detector.IsCurrentlyPlaceholder()) return 19;

    for (int i = 0; i < 15; ++i) {
        detector.Process(gc553Pro, PlaceholderDetector::CaptureFormatKind::NV12,
                         PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro);
    }
    detector.Reset();
    if (detector.IsCurrentlyPlaceholder()) return 20;
    if (detector.Process(gc553Pro, PlaceholderDetector::CaptureFormatKind::NV12,
                         PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro) !=
        PlaceholderDetector::FrameClassification::CandidatePlaceholder) return 21;

    // A known match after unrelated real content is initially unstable.
    // Only subsequent stable frames can start the 15-frame candidate streak.
    PlaceholderDetector transitionDetector;
    const PlaceholderDetector::Fingerprint realFrame{
        0x58, 0x62, 0x71, 0x44, 0x39, 0x67, 0x52, 0x6A, 0x5D};
    if (transitionDetector.Process(
            realFrame, PlaceholderDetector::CaptureFormatKind::NV12,
            PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro) !=
        PlaceholderDetector::FrameClassification::Real) return 22;
    if (transitionDetector.Process(
            gc553Pro, PlaceholderDetector::CaptureFormatKind::NV12,
            PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro) !=
        PlaceholderDetector::FrameClassification::Real) return 23;
    for (int i = 1; i <= 15; ++i) {
        const auto expected = i < 15
            ? PlaceholderDetector::FrameClassification::CandidatePlaceholder
            : PlaceholderDetector::FrameClassification::ConfirmedPlaceholder;
        if (transitionDetector.Process(gc553Pro,
                PlaceholderDetector::CaptureFormatKind::NV12,
                PlaceholderDetector::PlaceholderDeviceFamily::AverMediaGC553Pro)
            != expected) return 31;
    }

    // Both signatures match the Elgato zone tolerance (4), but their temporal
    // difference (3) exceeds stability tolerance (2). Moving content must
    // clear a confirmed latch and cannot remain indefinitely quarantined.
    PlaceholderDetector elgatoDetector;
    for (int i = 0; i < 15; ++i) {
        elgatoDetector.Process(elgato, PlaceholderDetector::CaptureFormatKind::P010,
            PlaceholderDetector::PlaceholderDeviceFamily::Elgato);
    }
    if (!elgatoDetector.IsCurrentlyPlaceholder()) return 32;
    auto movingElgato = elgato;
    movingElgato[4] += 3;
    for (int i = 0; i < 40; ++i) {
        const auto& fp = i % 2 == 0 ? movingElgato : elgato;
        if (elgatoDetector.Process(fp, PlaceholderDetector::CaptureFormatKind::P010,
                PlaceholderDetector::PlaceholderDeviceFamily::Elgato) !=
            PlaceholderDetector::FrameClassification::Real ||
            elgatoDetector.IsCurrentlyPlaceholder()) return 33;
    }
    for (int i = 1; i <= 15; ++i) {
        const auto expected = i < 15
            ? PlaceholderDetector::FrameClassification::CandidatePlaceholder
            : PlaceholderDetector::FrameClassification::ConfirmedPlaceholder;
        if (elgatoDetector.Process(elgato, PlaceholderDetector::CaptureFormatKind::P010,
                PlaceholderDetector::PlaceholderDeviceFamily::Elgato) != expected)
            return 34;
    }

    std::cout << "placeholder detector tests passed\n";
    return 0;
}
