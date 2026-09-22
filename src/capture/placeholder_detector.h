#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace NitLink {

// PlaceholderDetector: recognizes known capture-device NO SIGNAL placeholder frames by
// content fingerprint, so the run loop can suppress them and let the
// in-app no-signal debounce take over.
//
// Why content-based, not motion-based:
//   A naive "frame has not changed for N seconds" heuristic false-positives
//   on real static content (paused games, static menus, black scene
//   transitions, splash screens). The Elgato placeholder is a specific
//   image and can be discriminated by its actual zone-luma pattern, which
//   real content essentially never matches across all nine zones at once.
//
// Fingerprint:
//   9 bytes of zone-average luma. The frame is split into a 3x3 grid; each
//   zone gets 16 evenly-distributed sample points, each converted to
//   8-bit luma (format-aware: BT.709 weights for BGRA, direct Y byte for
//   NV12, top-8-of-16 Y byte for P010), then averaged per zone. The result
//   fits in 9 bytes and is robust to small noise (the average smooths
//   per-pixel jitter) while precise enough to discriminate a specific
//   placeholder layout from arbitrary content.
//
// Matching:
//   A new fingerprint matches a known one if (a) the known entry belongs to
//   the same device family, (b) it was captured in the same capture format as
//   the current frame (BGRA vs NV12 vs P010 have different luma encodings, so
//   cross-format comparisons are meaningless), and (c) every zone differs by
//   at most kZoneTolerance (8/255,
//   about 3%). Process() additionally requires:
//     - kRequiredConsecutiveMatches matching frames in a row, AND
//     - each frame in the streak is temporally STABLE relative to the
//       previous frame: its zone fingerprint must be within
//       kStableTolerance of the previous fingerprint. The real Elgato
//       placeholder is bit-identical across frames, so consecutive
//       fingerprints differ by 0. Real content (intro logos, slow fades,
//       splash screens) has subtle frame-to-frame variation from capture
//       noise, fade animation, or compression that produces fingerprint
//       drifts larger than kStableTolerance, so the streak resets before
//       confirmation. Together these two requirements stop a low-motion
//       game intro that happens to fall within zone tolerance of a baked
//       placeholder from being confirmed.
//
// Adding more variants: each device family / resolution / capture-format
// combination can encode the NO SIGNAL placeholder differently. To capture
// a new variant on hardware, press the Ctrl+F5 debug hotkey in Application
// while the placeholder is on screen. The diagnostic prints the device
// family, negotiated subtype, size, fps, and nine-byte fingerprint to
// OutputDebugString. Add a new entry only after the hardware and format are
// verified; the runtime matcher requires both the family and format to match.
class PlaceholderDetector {
public:
    using Fingerprint = std::array<uint8_t, 9>;

    // Pixel format passed in from the application layer so the fingerprint
    // computation knows how to extract luma from the raw byte stream. This
    // header deliberately doesn't pull in <mfapi.h>: Application maps the
    // negotiated MF subtype GUID to this enum.
    enum class CaptureFormatKind { BGRA, NV12, P010 };

    // Kept local to the detector to avoid making this capture-independent
    // component include capture-device policy headers. Application maps the
    // selected device policy to this small enum before calling Process().
    enum class PlaceholderDeviceFamily {
        Unknown,
        Elgato,
        AverMediaGC553Pro,
    };

    // Per-frame classification returned by Process() or the application-side
    // raw-sample gate. InvalidTransitionalFrame is deliberately not produced
    // by Process(): it is identified before Process() so it cannot alter the
    // placeholder detector's temporal streak.
    //   Real                  : no match against any known placeholder.
    //                           Caller should upload, bump last-good-frame
    //                           timer, run FrameDiffer normally. A known
    //                           match whose first frame is not stable against
    //                           the previous real frame is still a candidate
    //                           and is quarantined until the streak settles.
    //   CandidatePlaceholder  : matched a known placeholder, but the
    //                           kRequiredConsecutiveMatches streak hasn't
    //                           been reached yet. Caller should SUPPRESS
    //                           upload immediately (so even the first
    //                           matching frame doesn't pollute the
    //                           renderer's texture) but should NOT yet log
    //                           "placeholder detected": the streak might
    //                           be a coincidence and break on the next
    //                           frame.
    //   ConfirmedPlaceholder  : streak reached. Same suppression as
    //                           CandidatePlaceholder, plus the caller may
    //                           now log the transition and arm the shorter
    //                           placeholder-specific no-signal grace.
    //
    // The first non-matching frame after any candidate / confirmed streak
    // resets both the streak counter and the latch and returns Real, so
    // recovery is one-frame fast.
    enum class FrameClassification {
        Real,
        CandidatePlaceholder,
        ConfirmedPlaceholder,
        InvalidTransitionalFrame,
    };

    PlaceholderDetector() = default;

    // Pure function. Computes the 9-byte fingerprint of one frame. Safe to
    // call on any frame regardless of detector state.
    static Fingerprint Compute(const uint8_t* data, uint32_t size,
                               uint32_t width, uint32_t height,
                               CaptureFormatKind format);

    // Detect only a structurally invalid transitional sample: deterministic
    // samples from both the luma and chroma planes are raw zero. This is not a
    // black-frame detector. A legitimate black NV12/P010 frame has neutral
    // chroma (128 / 512 respectively) and therefore does not match. The
    // fixed sample set keeps the startup hot path bounded even at 4K.
    static bool IsInvalidTransitionalFrame(const uint8_t* data, uint32_t size,
                                           uint32_t width, uint32_t height,
                                           CaptureFormatKind format);

    // GC553Pro startup-specific transitional sample gate. This is deliberately
    // narrower than IsInvalidTransitionalFrame(): it only recognizes the
    // hardware's pre-first-real-frame pattern (zero luma plus neutral chroma).
    // NV12 accepts the verified structure for either MF range during startup;
    // P010 remains LIMITED-only so its legal limited-black representation is
    // not changed. Once the current capture session has accepted a genuine
    // Real frame the rule is disabled.
    // The raw sample predicate itself is deliberately stricter than a
    // fingerprint match: a known device placeholder has non-zero luma in its
    // verified zones and cannot satisfy the zero-luma/neutral-chroma rule.
    static bool IsGc553ProNeutralTransitionalFrame(
        const uint8_t* data, uint32_t size, uint32_t width, uint32_t height,
        CaptureFormatKind format, PlaceholderDeviceFamily deviceFamily,
        bool limitedRange, bool hasAcceptedRealFrame);

    // Update the detector with the latest frame's fingerprint and return
    // the classification. The format parameter identifies which capture
    // format the fingerprint was computed against; only known fingerprints
    // captured in the same format are eligible to match. See
    // FrameClassification above for the full state-machine semantics.
    FrameClassification Process(
        const Fingerprint& fp,
        CaptureFormatKind format,
        PlaceholderDeviceFamily deviceFamily = PlaceholderDeviceFamily::Unknown);

    // A placeholder candidate is already unsafe to upload: the caller must
    // suppress it immediately, before the temporal confirmation gate finishes.
    // InvalidTransitionalFrame is likewise never uploadable.
    static bool ShouldUploadCaptureFrame(FrameClassification classification) {
        return classification == FrameClassification::Real;
    }

    // Read-only diagnostic query using the same family/format-scoped table as
    // Process(). This does not alter detector state.
    bool IsKnownPlaceholder(const Fingerprint& fp,
                            CaptureFormatKind format,
                            PlaceholderDeviceFamily deviceFamily) const {
        return MatchesAnyKnown(fp, format, deviceFamily);
    }

    // Native capability query backed by the same table as Process().
    bool HasKnownPlaceholder(PlaceholderDeviceFamily deviceFamily,
                             CaptureFormatKind format) const;

    // Drop only unchanged fingerprints at the producer. A luma change must
    // reach the detector's stability check so returning source content can
    // release the placeholder state.
    static bool Matches(const Fingerprint& a, const Fingerprint& b) {
        return a == b;
    }

    // Drop detector state. Call from ReconcileCaptureFormat after the
    // FrameBuffer is rebuilt: the new capture format may encode luma
    // differently, so any in-flight match streak is meaningless.
    void Reset();

    // True iff the last Process() call left the detector in the
    // confirmed-placeholder state. Read-only: exists so debug/diagnostic
    // code can query without calling Process again.
    bool IsCurrentlyPlaceholder() const { return m_inPlaceholder; }

    // Debug helper. Formats `fp` as a hex byte array and routes it to the
    // caller-provided log sink. Used by the Ctrl+F5 hotkey to dump the
    // current frame's fingerprint into the OutputDebugString channel so
    // the developer can copy it into kKnownPlaceholders below.
    static void LogFingerprint(const Fingerprint& fp,
                                const std::function<void(const std::wstring&)>& log);

private:
    static bool ZoneMatch(const Fingerprint& a, const Fingerprint& b, uint8_t tolerance);
    bool MatchesAnyKnown(const Fingerprint& fp, CaptureFormatKind format,
                         PlaceholderDeviceFamily deviceFamily) const;

    // How close two zone bytes have to be to "match": 4/255, about 1.5%
    // tolerance. Real Elgato P010 placeholder fingerprints captured via
    // Ctrl+F5 show zero frame-to-frame jitter (bit-identical across
    // consecutive captures), so the placeholder side of the gap is exact;
    // the tolerance only has to cover hardware noise. A looser tolerance
    // admits false positives from dark game-intro content: measured
    // minimum luma distance from the baked P010 placeholder to Star Wars
    // Jedi studio-logo frames was 6 steps, so 4 sits safely in the gap
    // between the placeholder's bit-static reality and the closest
    // false-positive content.
    static constexpr uint8_t kZoneTolerance = 4;

    // How close two consecutive frame fingerprints have to be for the
    // frame to count as "temporally stable" within a placeholder streak.
    // Real Elgato P010 placeholders are bit-identical across consecutive
    // Ctrl+F5 captures (zero drift), while real game intro logos animate
    // through the center zone (Star Wars Jedi logos shifted center luma
    // through 0x12, 0x1B, 0x1D, 0x25, 0x53 across frames). A tight
    // stability window collapses the streak as soon as that animation
    // appears; 2 leaves room for USB transmission jitter and P010
    // encoding variance on the real placeholder without admitting
    // legitimate animated content.
    static constexpr uint8_t kStableTolerance = 2;

    // Consecutive matches required before declaring placeholder state.
    // Defense in depth against the intro-logo false-positive pattern.
    // 15 frames is about 250 ms at 60 Hz: still fast enough to feel
    // responsive on a real HDMI unplug (the Elgato placeholder typically
    // displays for many seconds during a handshake gap, easily satisfying
    // a 250 ms streak requirement), but long enough that a coincidental
    // short match on real content cannot flip the latch. Paired with the
    // tight zone and stability tolerances above, the three gates together
    // make a false confirmation on animated content extremely unlikely.
    static constexpr int kRequiredConsecutiveMatches = 15;

    int  m_consecutiveMatches = 0;
    bool m_inPlaceholder      = false;

    // Previous frame's fingerprint, used by the temporal-stability check.
    // m_havePrevFp distinguishes "first call after Reset" (no baseline yet)
    // from "second call onward". On the first call, stability is treated
    // as vacuously satisfied so the streak can start; from the second
    // call on, the streak only advances when the current fingerprint is
    // within kStableTolerance of m_prevFp in every zone.
    Fingerprint m_prevFp{};
    bool        m_havePrevFp = false;
};

} // namespace NitLink
