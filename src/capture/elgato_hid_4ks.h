#pragma once

#include <string>

namespace NitLink {

// Send the Elgato 4K S vendor HID command that selects the card's HDR
// tonemap mode. Application pairs this with the upcoming Media Foundation
// capture format:
//   enableTonemap = false (0x00) -> raw HDR10 PQ passthrough. Use this
//     before opening MF as P010 so the card's MCU stops applying its
//     internal HDR-to-SDR conversion and the BT.2020 PQ codes from the
//     PS5 reach NitLink's HDR10 shader untouched.
//   enableTonemap = true  (0x01) -> internal HDR-to-SDR tonemap engaged.
//     Use this before opening MF as NV12 / BGRA so an HDR HDMI source
//     gets cleanly converted to SDR by the card; without this, the NV12
//     frames carry HDR-shaped data that looks washed when rendered as
//     SDR.
//
// Why this exists separately from the 4K Pro path:
//   The 4K Pro accepts a DirectShow IKsPropertySet::Set() call against
//   GUID D1E5209F-68FD-4529-BEE0-5E7A1F479226 property 722 to toggle its
//   hardware tonemap. The 4K S does NOT recognize that GUID at all
//   (verified: QuerySupported/Set both return E_PROP_SET_UNSUPPORTED on
//   the 4K S). It uses a completely different control surface: a vendor
//   HID collection on USB interface 7 (alongside its video-capture
//   interfaces). Sending a 255-byte HID Output Report with a specific
//   header + sub-command byte tells the card's MCU to switch its HDR
//   processing mode.
//
// Protocol reference: 13bm/elgato4k-linux. No source code copied;
// protocol reference only. See ACKNOWLEDGMENTS.md for full attribution
// and docs/4ks-hdr-tonemap.md for the engineering context (including
// the no-commit-packet detail).
//
// Returns true if a 4K S was found AND the report write returned success
// from either HidD_SetOutputReport or the WriteFile fallback. Returns
// false if no 4K S is connected (the silent common case on 4K Pro, no
// device, or other vendor) or if both write paths failed (logged via
// OutputDebugString under the [NitLink/Elgato4KS-HID] tag).
//
// Safe to call before every CaptureDevice::Open(). On the 4K Pro and
// other devices the function returns false quickly without affecting
// anything: it just doesn't find a matching VID/PID.
bool Set4KSTonemap(bool enableTonemap);

// HDMI source identifier readout. Reads the 4K S's HDMI Source Product
// Descriptor InfoFrame via the vendor query channel (sub_cmd 0x14 on
// channel 0x07/0x55/0x01) and decodes known source-vendor markers into
// a human-readable label.
//
// Wire format: HidD_SetOutputReport(rid=0x06, 255 bytes) with header
// {0x06, 0x06, 0x07, 0x55, 0x01, 0x14, 0x20} zero-padded, then
// HidD_GetInputReport(rid=0x05, buffer length 2047). Buffer length is
// the on-wire report length, not the max-across-rids descriptor value.
//
// Currently decodes:
//   _SCEI + PS5 marker      -> "PlayStation 5"
//   _SCEI + non-PS5 (e.g.
//   older PS3/PS4)          -> "PlayStation (unknown model)"
//
// Returns detected=false on any of: no 4K S present, HID open failed,
// vendor query failed, no recognized marker in the response. The raw
// response bytes are logged via OutputDebugString under the
// [NitLink/Elgato4KS-HID] tag so unknown sources can have markers added.
//
// Costs exactly two HID transactions per call (1 SET_REPORT + 1
// GET_REPORT). Stays under the firmware fragility ceiling of ~3-5
// ops/session, but DO NOT chain multiple Detect4KS* calls in one
// process without unplug-replug recovery.
struct HdmiSourceInfo {
    bool         detected;
    std::wstring label;
};
HdmiSourceInfo Detect4KSHdmiSource();

// Direct HDR signal-state readout from the 4K S vendor HID.
//
// Triggers an MCU refresh of its cached HDR Metadata register, then
// queries the register and reports whether the current HDMI source is
// transmitting a CTA-861-G HDR Static Metadata InfoFrame (type 0x07).
//
// The 4K S MCU caches its last-parsed HDR Metadata InfoFrame in
// sub_cmd 0x09's register and does NOT auto-refresh when the source's
// HDR state changes. Reading 0x09 directly returns the stale cache
// from whenever HDR was last seen in the current USB session. Writing
// SET sub_cmd 0x13 = 0x01 tells the MCU to re-parse the InfoFrame
// from the current HDMI source state; after a ~1.3s settling time,
// reading sub_cmd 0x09 returns the fresh InfoFrame bytes (or all
// zeros when no HDR InfoFrame is on the wire).
//
// Wire format:
//   Write 1 (refresh trigger):
//     06 06 06 55 02 13 01 zero-padded to 255 bytes
//     (channel 0x06 set, magic 0x55, op 0x02 write, sub_cmd 0x13,
//      param 0x01)
//   Wait 1500ms for MCU re-parse.
//   Write 2 (HDR Metadata query):
//     06 06 07 55 01 09 20 zero-padded to 255 bytes
//     (channel 0x07 query, magic 0x55, op 0x01 read, sub_cmd 0x09,
//      param 0x20 length hint)
//   Read: 33 bytes from rid=0x05 via HidD_GetInputReport buffer=2047.
//
// Response layout (33 bytes, status + 32 data):
//   [0]    status (0x00 = success)
//   [1]    InfoFrame type with extension flag:
//            0x87 = HDR Static Metadata InfoFrame present (HDR active)
//            0x00 = no InfoFrame parsed (SDR)
//   [2]    version (0x01)
//   [3]    length (0x1a = 26 bytes per CTA-861-G spec)
//   [4]    checksum
//   [5]    EOTF when InfoFrame present:
//            0x00 = traditional gamma SDR
//            0x01 = traditional gamma HDR
//            0x02 = SMPTE ST 2084 (PQ)
//            0x03 = HLG
//   [6..30] mastering display metadata when InfoFrame present
//   [31..32] suffix bytes (00 01 observed)
//
// Costs three HID transactions per call (2 SET_REPORT + 1 GET_REPORT)
// plus a 1.5s mid-probe sleep, so total wall-clock is ~1.6s. Combined
// with Detect4KSHdmiSource (2 ops) and Set4KSTonemap (1 op) at startup
// the total is 6 HID ops per process lifetime, which the firmware
// tolerates because the probe's required 1.5s mid-sleep provides
// natural pacing between bursts.
//
// On the 4K S path this is the authoritative HDR signal-state input,
// replacing the source-ID heuristic in Is4KSHdmiSourceHdrCapable. The
// heuristic remains as a fallback when this probe fails (HID error,
// MCU non-responsive, no 4K S connected).
struct HdrMetadataProbeResult {
    bool          queryOk;    // true if both HID transactions returned success
    bool          hdrActive;  // true if response[1] == 0x87
    unsigned char eotf;       // response[5]; only meaningful when hdrActive == true
    unsigned char raw[33];    // full 33-byte response for inspection / logging
};
HdrMetadataProbeResult Probe4KSHdrMetadata();

// HDR-capability heuristic for known HDMI sources. Returns true when
// `label` matches a console known to output HDR by default (PS5 today;
// extend as Xbox/Switch markers get added to Detect4KSHdmiSource).
// Drives the source-ID-based auto-tonemap path: when the 4K S cannot
// directly report HDR signal state via its vendor HID, the connected
// source identifier becomes the proxy for "user probably wants HDR".
//
// Label strings MUST match exactly what Detect4KSHdmiSource() returns.
// New markers added to Detect4KSHdmiSource also need a corresponding
// entry here for the heuristic to recognize them as HDR-capable.
//
// Pure function. Safe to call freely with no HID side effects.
bool Is4KSHdmiSourceHdrCapable(const std::wstring& label);

} // namespace NitLink
