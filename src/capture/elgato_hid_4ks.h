#pragma once

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

} // namespace NitLink
