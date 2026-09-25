#include "placeholder_detector.h"

#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <vector>

namespace NitLink {

namespace {

constexpr uint32_t kTransitionalSampleCount = 4;

uint32_t SampleCoordinate(uint32_t length, uint32_t index)
{
    // 1/8, 3/8, 5/8, 7/8 of the plane dimension. These points avoid the
    // outermost row/column while covering all four quadrants deterministically.
    return (length * (2 * index + 1)) / (2 * kTransitionalSampleCount);
}

bool ReadUInt16LEIsZero(const uint8_t* data, uint64_t offset, uint32_t size)
{
    if (offset + 1 >= size) return false;
    return data[offset] == 0 && data[offset + 1] == 0;
}

bool ReadUInt16LEEquals(const uint8_t* data, uint64_t offset, uint32_t size,
                        uint16_t expected)
{
    if (offset + 1 >= size) return false;
    const uint16_t value = static_cast<uint16_t>(data[offset]) |
                           (static_cast<uint16_t>(data[offset + 1]) << 8);
    return value == expected;
}

bool HasZeroLumaAndNeutralChroma(const uint8_t* data, uint32_t size,
                                 uint32_t width, uint32_t height,
                                 PlaceholderDetector::CaptureFormatKind format)
{
    if (!data || width < 8 || height < 8) return false;
    if (format != PlaceholderDetector::CaptureFormatKind::NV12 &&
        format != PlaceholderDetector::CaptureFormatKind::P010) {
        return false;
    }

    const uint64_t pixels = static_cast<uint64_t>(width) * height;
    const uint32_t chromaWidth = width / 2;
    const uint32_t chromaHeight = height / 2;
    if (chromaWidth == 0 || chromaHeight == 0) return false;

    if (format == PlaceholderDetector::CaptureFormatKind::NV12) {
        const uint64_t yBytes = pixels;
        const uint64_t uvBytes = static_cast<uint64_t>(width) * chromaHeight;
        if (yBytes + uvBytes > size) return false;

        for (uint32_t sy = 0; sy < kTransitionalSampleCount; ++sy) {
            const uint32_t y = SampleCoordinate(height, sy);
            for (uint32_t sx = 0; sx < kTransitionalSampleCount; ++sx) {
                const uint32_t x = SampleCoordinate(width, sx);
                const uint64_t yOffset = static_cast<uint64_t>(y) * width + x;
                if (data[yOffset] != 0) return false;
            }
        }

        for (uint32_t sy = 0; sy < kTransitionalSampleCount; ++sy) {
            const uint32_t y = SampleCoordinate(chromaHeight, sy);
            for (uint32_t sx = 0; sx < kTransitionalSampleCount; ++sx) {
                const uint32_t x = SampleCoordinate(chromaWidth, sx);
                const uint64_t uvOffset = yBytes +
                    static_cast<uint64_t>(y) * width +
                    static_cast<uint64_t>(x) * 2;
                if (uvOffset + 1 >= yBytes + uvBytes ||
                    data[uvOffset] != 0x80 || data[uvOffset + 1] != 0x80) {
                    return false;
                }
            }
        }
        return true;
    }

    const uint64_t yBytes = pixels * 2;
    const uint64_t uvBytes = static_cast<uint64_t>(width) * chromaHeight * 2;
    if (yBytes + uvBytes > size) return false;

    for (uint32_t sy = 0; sy < kTransitionalSampleCount; ++sy) {
        const uint32_t y = SampleCoordinate(height, sy);
        for (uint32_t sx = 0; sx < kTransitionalSampleCount; ++sx) {
            const uint32_t x = SampleCoordinate(width, sx);
            const uint64_t yOffset = (static_cast<uint64_t>(y) * width + x) * 2;
            if (!ReadUInt16LEIsZero(data, yOffset, size)) return false;
        }
    }

    // P010's neutral 10-bit chroma code is 512, represented in the raw
    // 16-bit sample as 0x8000 (the low six bits are zero).
    for (uint32_t sy = 0; sy < kTransitionalSampleCount; ++sy) {
        const uint32_t y = SampleCoordinate(chromaHeight, sy);
        for (uint32_t sx = 0; sx < kTransitionalSampleCount; ++sx) {
            const uint32_t x = SampleCoordinate(chromaWidth, sx);
            const uint64_t uvOffset = yBytes +
                (static_cast<uint64_t>(y) * width + x * 2) * 2;
            if (!ReadUInt16LEEquals(data, uvOffset, size, 0x8000) ||
                !ReadUInt16LEEquals(data, uvOffset + 2, size, 0x8000)) {
                return false;
            }
        }
    }
    return true;
}

// Known placeholder fingerprints, one entry per verified device/format
// variant.
//
// Entries are captured with Ctrl+F5 while the Elgato NO SIGNAL placeholder
// is visible on screen. Each press prints a nine-byte [NitLink/Placeholder]
// line to the debug-output channel; paste those bytes into a new entry
// below alongside the capture format that was active when the fingerprint
// was taken. The same NO SIGNAL screen encodes to different luma
// fingerprints depending on the capture format (BGRA vs NV12 vs P010 each
// read the signal through different YUV / RGB paths), so each entry is
// tagged with its source format and the runtime matcher only considers
// entries whose format matches the current capture format. This stops a
// BGRA fingerprint from being matched against an NV12 frame (a real source
// of false positives on the 4K S NV12 SDR path before format tagging was
// added).
struct KnownPlaceholder {
    PlaceholderDetector::Fingerprint       zones;
    PlaceholderDetector::CaptureFormatKind format;
    PlaceholderDetector::PlaceholderDeviceFamily deviceFamily;
};

const std::vector<KnownPlaceholder>& KnownPlaceholders()
{
    using FmtKind = PlaceholderDetector::CaptureFormatKind;
    using Family = PlaceholderDetector::PlaceholderDeviceFamily;
    static const std::vector<KnownPlaceholder> list = {
        // Elgato NO SIGNAL placeholder, BGRA path, Elgato 4K Pro, captured
        // via Ctrl+F5 on 2026-05-19. Confirmed against the run-loop signal
        // pattern at capture time: contentFps=0 sustained for several
        // seconds, lastDiff=0 across the differ, hdmiFps drifted to ~36
        // (Elgato's no-signal output rate, distinct from the 60 Hz it
        // delivers on a live source). That pattern only happens when the
        // upstream HDMI source is gone.
        { { 0x3A, 0x30, 0x2B, 0x44, 0x3E, 0x33, 0x4F, 0x3C, 0x3A }, FmtKind::BGRA, Family::Elgato },

        // Elgato NO SIGNAL placeholder, P010 / HDR path, Elgato 4K Pro,
        // captured via Ctrl+F5 on 2026-05-19 with HDR active after HDMI
        // input was gone and the log held contentFps=0 / lastDiff=0
        // sustained. Eight zones at 0x10 (near-black P010 top-byte, about
        // 10-bit Y 64) with the center zone at 0x1D: the classic
        // dark-background-plus-centered-element shape of a NO SIGNAL screen.
        // The high uniformity across 8 zones is what makes this fingerprint
        // robust: real HDR content essentially never produces 8 zones
        // within +/-8 of 0x10 simultaneously, regardless of how dark the
        // scene is.
        { { 0x10, 0x10, 0x10, 0x10, 0x1D, 0x10, 0x10, 0x10, 0x10 }, FmtKind::P010, Family::Elgato },

        // Elgato NO SIGNAL placeholder, NV12 / SDR path, Elgato 4K Pro,
        // captured via Ctrl+F5 on 2026-05-20 with the source disconnected
        // and the capture pipeline running at 3840x2160 NV12. Bytes are
        // numerically the same as the P010 entry above because NV12 Y and
        // P010 top-byte both encode BT.709 black at 16 (0x10), and the
        // Elgato emits a visually equivalent placeholder on both paths;
        // the format-tagged matcher needs a separate NV12 entry so the
        // SDR pipeline can confirm the placeholder without falling back
        // to the cross-format comparison (which is rejected by design).
        { { 0x10, 0x10, 0x10, 0x10, 0x1D, 0x10, 0x10, 0x10, 0x10 }, FmtKind::NV12, Family::Elgato },

        // AVerMedia Live Gamer ULTRA S GC553Pro + P010 hardware NO SIGNAL
        // placeholder fingerprint. The format scope is intentional: each
        // native pixel format has its own independently captured signature.
        { { 0x00, 0x0D, 0x00, 0x03, 0x04, 0x02, 0x2E, 0x20, 0x18 },
          FmtKind::P010, Family::AverMediaGC553Pro },

        // AVerMedia Live Gamer ULTRA S GC553Pro, verified on hardware at
        // 3840x2160 @ 60 FPS NV12 with HDR output OFF after a USB reset while
        // the HDMI source was already absent. This is the reset/no-source
        // hardware NO SIGNAL variant.
        { { 0x00, 0x0C, 0x00, 0x04, 0x05, 0x03, 0x2D, 0x1C, 0x15 },
          FmtKind::NV12, Family::AverMediaGC553Pro },

        // AVerMedia Live Gamer ULTRA S GC553Pro, verified on hardware at
        // 3840x2160 @ 60 FPS NV12 with HDR output OFF after valid HDMI input
        // was established and the source later entered sleep/no-signal. This
        // variant persisted across NitLink restart while the device remained
        // connected. The comments describe observable behavior only; they do
        // not assert a firmware-level cause.
        { { 0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x43, 0x2F, 0x24 },
          FmtKind::NV12, Family::AverMediaGC553Pro },
    };
    return list;
}

} // anonymous namespace


PlaceholderDetector::Fingerprint
PlaceholderDetector::Compute(const uint8_t* data, uint32_t size,
                              uint32_t width, uint32_t height,
                              CaptureFormatKind format)
{
    Fingerprint fp{};   // all zeros by default; never matches a real placeholder
                        // because real placeholders have a structured zone pattern,
                        // not all-black-everywhere.

    if (!data || size == 0 || width < 12 || height < 12) {
        // Too small or no data: return the zero fingerprint. The detector's
        // tolerance gate (<= 8) means an all-zero fingerprint can only match
        // a known fingerprint that is itself near-black in every zone, which
        // would be a degenerate placeholder that must never be baked in.
        return fp;
    }

    constexpr int kGrid = 3;            // 3x3 zones across the frame
    constexpr int kPerAxisSamples = 4;  // 4x4 sample grid within each zone = 16 samples/zone

    const uint32_t zoneW = width  / kGrid;
    const uint32_t zoneH = height / kGrid;

    for (int gy = 0; gy < kGrid; ++gy) {
        for (int gx = 0; gx < kGrid; ++gx) {
            const uint32_t x0 = static_cast<uint32_t>(gx) * zoneW;
            const uint32_t y0 = static_cast<uint32_t>(gy) * zoneH;

            uint32_t lumaSum   = 0;
            uint32_t lumaCount = 0;

            // Sample 4x4 = 16 points inside the zone, centered in each
            // sub-cell (avoids sampling exactly on zone edges where adjacent
            // zones' luma might bleed in for slightly-mis-positioned
            // placeholders).
            for (int sy = 0; sy < kPerAxisSamples; ++sy) {
                for (int sx = 0; sx < kPerAxisSamples; ++sx) {
                    const uint32_t px = x0 + (zoneW * (2 * sx + 1)) / (2 * kPerAxisSamples);
                    const uint32_t py = y0 + (zoneH * (2 * sy + 1)) / (2 * kPerAxisSamples);
                    if (px >= width || py >= height) continue;

                    uint8_t luma = 0;
                    switch (format) {
                    case CaptureFormatKind::BGRA: {
                        // DXGI BGRA layout: byte 0=B, 1=G, 2=R, 3=A.
                        // BT.709 luma weights ((R*54 + G*183 + B*19) >> 8):
                        // integer approximation, no float math in hot path.
                        const uint64_t off =
                            static_cast<uint64_t>(py) * width * 4 + static_cast<uint64_t>(px) * 4;
                        if (off + 3 >= size) continue;
                        const uint32_t B = data[off + 0];
                        const uint32_t G = data[off + 1];
                        const uint32_t R = data[off + 2];
                        luma = static_cast<uint8_t>((R * 54 + G * 183 + B * 19) >> 8);
                        break;
                    }
                    case CaptureFormatKind::NV12: {
                        // 8-bit Y plane comes first (width*height bytes,
                        // top-down, packed by ConvertToContiguousBuffer).
                        const uint64_t off =
                            static_cast<uint64_t>(py) * width + static_cast<uint64_t>(px);
                        if (off >= size) continue;
                        luma = data[off];
                        break;
                    }
                    case CaptureFormatKind::P010: {
                        // 16-bit little-endian Y plane (top 10 bits carry
                        // data, bottom 6 zero). The top byte at offset
                        // (off + 1) carries the high 8 bits: close enough
                        // for fingerprinting.
                        const uint64_t off =
                            static_cast<uint64_t>(py) * width * 2 + static_cast<uint64_t>(px) * 2 + 1;
                        if (off >= size) continue;
                        luma = data[off];
                        break;
                    }
                    }

                    lumaSum   += luma;
                    lumaCount += 1;
                }
            }

            const int idx = gy * kGrid + gx;
            fp[idx] = (lumaCount > 0)
                ? static_cast<uint8_t>(lumaSum / lumaCount)
                : uint8_t{0};
        }
    }

    return fp;
}

bool PlaceholderDetector::IsInvalidTransitionalFrame(
    const uint8_t* data, uint32_t size, uint32_t width, uint32_t height,
    CaptureFormatKind format)
{
    if (!data || width < 8 || height < 8) return false;
    if (format != CaptureFormatKind::NV12 && format != CaptureFormatKind::P010) {
        return false;
    }

    const uint64_t pixels = static_cast<uint64_t>(width) * height;
    const uint32_t chromaWidth = width / 2;
    const uint32_t chromaHeight = height / 2;
    if (chromaWidth == 0 || chromaHeight == 0) return false;

    if (format == CaptureFormatKind::NV12) {
        const uint64_t yBytes = pixels;
        const uint64_t uvBytes = static_cast<uint64_t>(width) * chromaHeight;
        if (yBytes + uvBytes > size) return false;

        for (uint32_t sy = 0; sy < kTransitionalSampleCount; ++sy) {
            const uint32_t y = SampleCoordinate(height, sy);
            for (uint32_t sx = 0; sx < kTransitionalSampleCount; ++sx) {
                const uint32_t x = SampleCoordinate(width, sx);
                const uint64_t yOffset = static_cast<uint64_t>(y) * width + x;
                if (data[yOffset] != 0) return false;
            }
        }

        // NV12 chroma is interleaved Cb/Cr, one pair for each 2x2 luma block.
        // A raw zero UV plane is structurally invalid; neutral black is 128/128.
        for (uint32_t sy = 0; sy < kTransitionalSampleCount; ++sy) {
            const uint32_t y = SampleCoordinate(chromaHeight, sy);
            for (uint32_t sx = 0; sx < kTransitionalSampleCount; ++sx) {
                const uint32_t x = SampleCoordinate(chromaWidth, sx);
                const uint64_t uvOffset = yBytes +
                    static_cast<uint64_t>(y) * width +
                    static_cast<uint64_t>(x) * 2;
                if (uvOffset + 1 >= yBytes + uvBytes ||
                    data[uvOffset] != 0 || data[uvOffset + 1] != 0) {
                    return false;
                }
            }
        }
        return true;
    }

    // P010 stores 16-bit little-endian samples with the meaningful 10-bit
    // value in the high bits. Checking the raw 16-bit units catches a true
    // zero-filled sample while preserving legal full-range black plus neutral
    // chroma (0 / 512 in 10-bit code space).
    const uint64_t yBytes = pixels * 2;
    const uint64_t uvBytes = static_cast<uint64_t>(width) * chromaHeight * 2;
    if (yBytes + uvBytes > size) return false;

    for (uint32_t sy = 0; sy < kTransitionalSampleCount; ++sy) {
        const uint32_t y = SampleCoordinate(height, sy);
        for (uint32_t sx = 0; sx < kTransitionalSampleCount; ++sx) {
            const uint32_t x = SampleCoordinate(width, sx);
            const uint64_t yOffset = (static_cast<uint64_t>(y) * width + x) * 2;
            if (!ReadUInt16LEIsZero(data, yOffset, size)) return false;
        }
    }

    for (uint32_t sy = 0; sy < kTransitionalSampleCount; ++sy) {
        const uint32_t y = SampleCoordinate(chromaHeight, sy);
        for (uint32_t sx = 0; sx < kTransitionalSampleCount; ++sx) {
            const uint32_t x = SampleCoordinate(chromaWidth, sx);
            const uint64_t uvOffset = yBytes +
                (static_cast<uint64_t>(y) * width + x * 2) * 2;
            if (!ReadUInt16LEIsZero(data, uvOffset, size) ||
                !ReadUInt16LEIsZero(data, uvOffset + 2, size)) {
                return false;
            }
        }
    }
    return true;
}

bool PlaceholderDetector::IsGc553ProNeutralTransitionalFrame(
    const uint8_t* data, uint32_t size, uint32_t width, uint32_t height,
    CaptureFormatKind format, PlaceholderDeviceFamily deviceFamily,
    bool limitedRange, bool hasAcceptedRealFrame)
{
    // This is intentionally not a general black-frame rule. It is tied to the
    // verified GC553Pro startup behavior and is disabled after the current
    // stream has accepted its first genuine Real frame.
    if (deviceFamily != PlaceholderDeviceFamily::AverMediaGC553Pro) return false;
    if (hasAcceptedRealFrame) return false;

    // Zero luma with neutral chroma is legal FULL-range black in both
    // formats, even before the first accepted frame. Sparse sampling cannot
    // distinguish that content (or a small loading logo) from startup output.
    if (!limitedRange) return false;
    if (format != CaptureFormatKind::NV12 && format != CaptureFormatKind::P010) {
        return false;
    }
    return HasZeroLumaAndNeutralChroma(data, size, width, height, format);
}

bool PlaceholderDetector::ZoneMatch(const Fingerprint& a, const Fingerprint& b,
                                      uint8_t tolerance)
{
    for (size_t i = 0; i < a.size(); ++i) {
        const int diff = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
        if (diff > static_cast<int>(tolerance)) return false;
    }
    return true;
}

bool PlaceholderDetector::MatchesAnyKnown(const Fingerprint& fp,
                                            CaptureFormatKind format,
                                            PlaceholderDeviceFamily deviceFamily) const
{
    for (const auto& known : KnownPlaceholders()) {
        if (known.format != format) continue;
        if (known.deviceFamily != deviceFamily) continue;
        if (ZoneMatch(fp, known.zones, kZoneTolerance)) return true;
    }
    return false;
}

bool PlaceholderDetector::HasKnownPlaceholder(
    PlaceholderDeviceFamily deviceFamily, CaptureFormatKind format) const
{
    for (const auto& known : KnownPlaceholders()) {
        if (known.deviceFamily == deviceFamily && known.format == format) {
            return true;
        }
    }
    return false;
}

PlaceholderDetector::FrameClassification
PlaceholderDetector::Process(const Fingerprint& fp, CaptureFormatKind format,
                             PlaceholderDeviceFamily deviceFamily)
{
    // Temporal-stability check. The Elgato NO SIGNAL placeholder is
    // bit-identical across frames, so consecutive fingerprints land
    // exactly equal (or off by capture-pipeline noise of at most a few
    // luma steps). Real intro logos, slow fades, and splash screens
    // that happen to fall within kZoneTolerance of a baked placeholder
    // still vary subtly from frame to frame (fade animation, micro
    // animation, encoder noise, dither). Requiring the current
    // fingerprint to be within kStableTolerance of the previous one
    // collapses the streak as soon as that subtle variation appears,
    // preventing real low-motion content from being confirmed as a
    // placeholder.
    //
    // On the very first call after Reset() (or construction), there is
    // no previous fingerprint to compare against. Treat stability as
    // vacuously satisfied so a streak can start; the second call onward
    // will check stability against the just-stored fingerprint.
    const bool zoneMatches = MatchesAnyKnown(fp, format, deviceFamily);
    const bool stable      = m_havePrevFp
        ? ZoneMatch(m_prevFp, fp, kStableTolerance)
        : true;
    m_prevFp     = fp;
    m_havePrevFp = true;

    if (zoneMatches && stable) {
        if (m_consecutiveMatches < kRequiredConsecutiveMatches) {
            ++m_consecutiveMatches;
        }
        if (m_consecutiveMatches >= kRequiredConsecutiveMatches) {
            m_inPlaceholder = true;
            return FrameClassification::ConfirmedPlaceholder;
        }

        // Stable match but not yet confirmed: the caller suppresses upload.
        // Logging the transition is
        // deferred to ConfirmedPlaceholder so a single false-match does not
        // spam the debug channel.
        return FrameClassification::CandidatePlaceholder;
    }

    // A non-match or temporal instability is real source content. Preserve
    // the moving-content safeguard even when both fingerprints match a known
    // entry, and release any previous placeholder latch immediately.
    m_consecutiveMatches = 0;
    m_inPlaceholder      = false;
    return FrameClassification::Real;
}

void PlaceholderDetector::Reset()
{
    m_consecutiveMatches = 0;
    m_inPlaceholder      = false;
    // Drop the temporal-stability baseline too: after a format change
    // the new format's fingerprint encoding is different (BGRA vs NV12
    // vs P010 read luma through different paths), so comparing the
    // first post-reset frame against an old-format baseline would be
    // meaningless. Treat the first call after Reset as the start of a
    // fresh stability window.
    m_prevFp     = {};
    m_havePrevFp = false;
}

void PlaceholderDetector::LogFingerprint(
    const Fingerprint& fp,
    const std::function<void(const std::wstring&)>& log)
{
    std::wstringstream ss;
    ss << L"Placeholder fingerprint (paste into placeholder_detector.cpp): { ";
    for (size_t i = 0; i < fp.size(); ++i) {
        ss << L"0x"
           << std::hex << std::uppercase
           << std::setw(2) << std::setfill(L'0')
           << static_cast<int>(fp[i]);
        if (i + 1 < fp.size()) ss << L", ";
    }
    ss << L" }";
    log(ss.str());
}

} // namespace NitLink
