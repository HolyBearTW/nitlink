#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace NitLink {

// PlaceholderDetector: recognizes Elgato NO SIGNAL placeholder frames by
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
//   A new fingerprint matches a known one if (a) the known entry was
//   captured in the same capture format as the current frame (BGRA vs NV12
//   vs P010 have different luma encodings, so cross-format comparisons are
//   meaningless and were a real source of false positives on the 4K S NV12
//   SDR path) and (b) every zone differs by at most kZoneTolerance (8/255,
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
// Adding more variants: each Elgato firmware / resolution / capture-
// format combination encodes the NO SIGNAL placeholder differently
// because BGRA, NV12, and P010 each read luma through a different path.
// The shipping list covers BGRA (4K Pro SDR) and P010 (4K Pro HDR). To
// capture a new variant on hardware, press the Ctrl+F5 debug hotkey in
// Application while the placeholder is on screen: LogFingerprint prints
// the nine-byte fingerprint to OutputDebugString. Paste the result into
// kKnownPlaceholders inside placeholder_detector.cpp alongside the
// CaptureFormatKind that was active when the fingerprint was taken; the
// runtime matcher only considers entries whose format matches the
// current capture format.
class PlaceholderDetector {
public:
    using Fingerprint = std::array<uint8_t, 9>;

    // Pixel format passed in from the application layer so the fingerprint
    // computation knows how to extract luma from the raw byte stream. This
    // header deliberately doesn't pull in <mfapi.h>: Application maps the
    // negotiated MF subtype GUID to this enum.
    enum class CaptureFormatKind { BGRA, NV12, P010 };

    // Per-frame classification returned by Process().
    //   Real                  : no match against any known placeholder.
    //                           Caller should upload, bump last-good-frame
    //                           timer, run FrameDiffer normally.
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
    enum class FrameClassification { Real, CandidatePlaceholder, ConfirmedPlaceholder };

    PlaceholderDetector() = default;

    // Pure function. Computes the 9-byte fingerprint of one frame. Safe to
    // call on any frame regardless of detector state.
    static Fingerprint Compute(const uint8_t* data, uint32_t size,
                                uint32_t width, uint32_t height,
                                CaptureFormatKind format);

    // Update the detector with the latest frame's fingerprint and return
    // the classification. The format parameter identifies which capture
    // format the fingerprint was computed against; only known fingerprints
    // captured in the same format are eligible to match. See
    // FrameClassification above for the full state-machine semantics.
    FrameClassification Process(const Fingerprint& fp, CaptureFormatKind format);

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
    bool MatchesAnyKnown(const Fingerprint& fp, CaptureFormatKind format) const;

    // How close two zone bytes have to be to "match": 8/255, about 3% tolerance.
    // Tight enough that real content (with varied zone luma) doesn't match
    // by accident, loose enough that capture noise / firmware jitter on the
    // Elgato placeholder doesn't cause spurious misses.
    static constexpr uint8_t kZoneTolerance = 8;

    // How close two consecutive frame fingerprints have to be for the
    // frame to count as "temporally stable" within a placeholder streak.
    // Tighter than kZoneTolerance because the Elgato placeholder is
    // close-to-static across frames, but with headroom for real capture-
    // pipeline noise: USB transmission jitter on the 4K S, P010 encoding
    // variance at high producer rates, and reused-buffer race windows
    // can push consecutive fingerprints apart by a few luma steps even
    // on a strictly static placeholder image. Set high enough to absorb
    // that noise, low enough that real animated content (intro logos,
    // slow fades) still drifts past it and resets the streak.
    static constexpr uint8_t kStableTolerance = 6;

    // Consecutive matches required before declaring placeholder state.
    // 5 frames is about 83 ms at 60 Hz. Short enough to feel responsive on
    // a real HDMI unplug, long enough that a single false-match on a real
    // frame can't flip the latch alone.
    static constexpr int kRequiredConsecutiveMatches = 5;

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
