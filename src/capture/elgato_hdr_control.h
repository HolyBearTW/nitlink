#pragma once

// Elgato capture card HDR control via the MK.2 SDK custom property set.
//
// Background: Elgato capture cards (4K Pro, MK.2, 4K S) by default
// tonemap incoming HDR10 internally and expose SDR-encoded bytes to
// Media Foundation. This destroys the 10-bit precision of the source
// and bakes in Elgato's tonemap curve.
//
// The official Elgato Game Capture SDK exposes a custom property GUID
// that toggles this internal tonemap OFF. Once OFF, the card
// passes raw HDR10 (PQ-encoded BT.2020) to the OS, which is then
// negotiated as P010 via MF and decoded properly in the shader.
//
// The property is documented on the MK.2 SDK page at
// github.com/elgatosf/capture-device-support and was confirmed to work
// on the newer 4K Pro card (DEV_0710 SUBSYS_00121CFA).
//
// Implementation note: this property has to go via DirectShow's
// IKsPropertySet on the device filter; it isn't reachable through
// Media Foundation. So the code opens a transient DirectShow graph, sends
// the command, tears it down, and lets the normal MF capture session take
// over. The driver retains the OFF state across the DS->MF transition
// (verified experimentally; if this regresses on a future driver, the
// fallback is graceful: only 10-bit precision is lost, not a crash).

#include <string>
#include <cstdint>

namespace NitLink {

// Send "disable hardware tonemap" to an Elgato capture device.
//
// deviceName: friendly name of the device, e.g. L"Elgato 4K Pro".
//             Matched case-insensitively as a substring.
//
// Returns true if a matching device was found AND the property Set
// succeeded. Returns false in all other cases (no Elgato present,
// older non-SDK Elgato model, property unsupported on this driver,
// COM init failure, etc.).
//
// SAFE to call unconditionally at app init: no side effects on
// non-Elgato devices, no exceptions, no crashes.
bool DisableElgatoTonemap(const std::wstring& deviceName);

// Re-enable the hardware tonemap. Used when the user's NitLink output is
// SDR but the HDMI source is still sending HDR10 (e.g. user pressed Alt+H
// to disable HDR rendering while a PS5 HDR game is running). Without this
// the card would still pass raw HDR10 PQ codes that the NV12 shader
// interprets as sRGB BT.709, producing a strong green cast.
//
// Same safety contract as DisableElgatoTonemap: best-effort, no crashes,
// returns false on non-Elgato or unsupported devices.
bool EnableElgatoTonemap(const std::wstring& deviceName);

// Unified setter: true = ON (card tonemaps internally to SDR bytes out),
// false = OFF (card passes raw HDR10 PQ). Exists so reconcile code can
// pass a computed flag (`!userWantsHDR && sourceIsHDR10`) without an if.
bool SetElgatoTonemap(const std::wstring& deviceName, bool enable);

// What the HDMI source attached to the Elgato is currently sending,
// based on the CEA-861 Dynamic Range InfoFrame the card reports.
//
// The 4K Pro (and MK.2) expose the latest HDR InfoFrame as a 32-byte
// blob via two property IDs (720/721: split across two 16-byte reads).
// The EOTF field (byte[4] in the structured packet) is decoded into a
// human-meaningful state.
//
// This is the basis for auto-detection: if the source is HDR10, the
// pipeline negotiates P010 from Media Foundation; if it's SDR, it
// stays on BGRA.
//
// Why a struct and not a single enum? The caller needs to know BOTH
// what the source is doing AND whether the property was even readable.
// On non-Elgato cards, propertyAccessible is false and the caller
// should fall back to "treat as SDR" without surfacing an error to
// the user.
struct HDRSourceInfo {
    bool propertyAccessible = false; // True if the InfoFrame was read OK
    bool isHDR10 = false;            // ST.2084 / PQ (the PS5's HDR10 mode)
    bool isHLG   = false;            // HLG (rare on PS5 but possible)
    uint8_t rawEotf = 0;             // Raw EOTF byte for debugging
};

// Read the current HDR source state from the Elgato. Best-effort:
// returns a default-constructed HDRSourceInfo on any failure (non-
// Elgato device, property unsupported, etc.).
//
// The `quiet` parameter suppresses the routine per-call diagnostics
// (matched device name, raw 32-byte InfoFrame hex dump, decoded EOTF
// line). When true, only error and unusual conditions log. The HDR
// source poller passes `quiet=true` because it runs once per second
// and would otherwise drown the debug output. Initialize and other
// one-shot callers leave it false to retain the diagnostic trail.
HDRSourceInfo ReadElgatoHDRSource(const std::wstring& deviceName,
                                  bool quiet = false);

} // namespace NitLink
