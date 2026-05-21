# Elgato 4K S HDR tonemap control

A technical note on why NitLink uses two different code paths to control
the hardware HDR-to-SDR tonemap on Elgato capture cards, and how the 4K S
path was implemented.

---

## 1. Problem

NitLink originally treated all Elgato capture cards uniformly. On the 4K
Pro PCIe, the standard pipeline worked: disable the card's internal
HDR-to-SDR tonemap via Elgato's DirectShow custom property, negotiate
P010 from Media Foundation when the source is HDR10, render BT.2020 PQ
directly to an HDR10 swap chain. The card delivers raw HDR10 codes and
NitLink renders them.

The same code applied to the 4K S USB did not produce the same result.
With the 4K S in HDR mode against a PS5 HDR source, the picture had
correct deep blacks and apparent dynamic range, but bright reds and
yellows clipped severely. None of the obvious shader-side hypotheses
(range expansion, matrix swap, luminance scaling, per-channel descaling,
knee tonemap) fixed the appearance.

Querying the 4K Pro property path on the 4K S confirmed why:

```
[NitLink/ElgatoHDR] Matched device: Elgato 4K S
[NitLink/ElgatoHDR] QuerySupported(tonemap toggle) hr=0x80070492 supportFlags=0x00000000
[NitLink/ElgatoHDR] Set(tonemap=OFF) hr=0x80070492
```

`0x80070492` is `E_PROP_SET_UNSUPPORTED`. The driver does not recognize
the entire property GUID Elgato uses for the 4K Pro, not just the
specific property ID. The 4K S does not expose its tonemap control
through `IKsPropertySet` at all.

---

## 2. Key observation

The 4K S exposes its tonemap control through a different surface: a
vendor HID collection on USB interface 7 (alongside the card's UVC video
interfaces on the same composite USB device). Sending a specific 255-byte
HID Output Report to that collection tells the card's MCU to switch its
HDR-to-SDR tonemap on or off.

This is the same protocol Elgato's own Studio application uses on
Windows. It is not exposed in any public Elgato SDK.

---

## 3. Protocol reference

The byte layout of the HID Output Report is publicly documented in the
`elgato4k-linux` project:

- Repository: https://github.com/13bm/elgato4k-linux
- Reference files in that project: `src/protocol.rs`, `src/settings.rs`

That project derived the protocol through Ghidra decompilation of the
official Elgato Windows DLLs and the 4K S MCU firmware
(`FW_4K_S_MCU.bin`, ARM Cortex-M0).

NitLink used this project as a protocol reference only. No source code
from `elgato4k-linux` is included in NitLink. NitLink's Windows HID
implementation in `src/capture/elgato_hid_4ks.cpp` is independent and
written from scratch against the Win32 SetupAPI and HID Class API
(`hid.lib`, `setupapi.lib`).

For the full attribution (including upstream license) see
[`../ACKNOWLEDGMENTS.md`](../ACKNOWLEDGMENTS.md).

Byte values used:

| Field | Value | Purpose |
|---|---|---|
| USB VID:PID | `0x0FD9 : 0x00AF` (USB 3) or `0x00AE` (USB 2) | Device identification |
| HID Report ID | `0x06` | First byte of the Output Report buffer |
| Header bytes | `06 06 06 55 02` | Bytes 0..4 (report ID + signature + trigger) |
| Sub-command | `0x0A` | Byte 5 (`SUBCMD_HDR_TONEMAPPING`) |
| Payload OFF | `0x00` | Byte 6 (passthrough raw HDR10) |
| Payload ON | `0x01` | Byte 6 (internal HDR-to-SDR tonemap) |
| Padding | `0x00` | Bytes 7..254 (zero-padded to 255 total) |

The report is sent through `HidD_SetOutputReport`. When that fails the
implementation falls back to `WriteFile` to cover HID driver variants
that route OUT reports through the interrupt endpoint rather than the
control endpoint. The transfer is a single packet; no commit / confirm
step is needed. (An earlier reverse-engineering attempt suggested a
`0x13`-prefixed commit packet was required; the elgato4k-linux project
confirmed from disassembled firmware that sending such a packet crashes
the 4K S MCU into a watchdog reset, so it is not used.)

Device path selection: the 4K S enumerates multiple HID collections from
the same composite USB device. The vendor command collection is
identified by the `mi_07` substring (USB multi-interface index 7) in the
Windows device-interface path. The implementation prefers `mi_07`
matches and falls back to any matching VID/PID instance if `mi_07` is
not visible in the path string on a given driver version.

---

## 4. Implemented behavior

NitLink pairs the HID tonemap state with the Media Foundation capture
format on both the initial open and every reconcile (Alt+H toggle,
source HDR auto-detect transition, format-change recovery).

### HDR / P010 path (4K S delivers raw HDR10)

1. Send HID Output Report with `SUBCMD_HDR_TONEMAPPING = 0x00`
   (tonemap OFF, raw passthrough)
2. `CaptureDevice::Open` requests `MFVideoFormat_P010` at 1920x1080
3. Renderer binds the HDR10 PQ BT.2020 swap chain
4. P010 frames arrive as raw BT.2020 PQ codes; the renderer's BT.2020
   YUV→RGB matrix runs without any tonemap and writes results directly
   to the HDR10 swap chain. The Windows DWM hands the PQ-encoded BT.2020
   codes to the display unchanged.

### SDR / NV12 path (4K S delivers tonemapped SDR)

1. Send HID Output Report with `SUBCMD_HDR_TONEMAPPING = 0x01`
   (tonemap ON, internal HDR-to-SDR conversion engaged)
2. `CaptureDevice::Open` requests `MFVideoFormat_NV12` at 3840x2160
3. Renderer binds the SDR BGRA swap chain
4. NV12 frames arrive as BT.709 SDR even when the HDMI source is
   HDR10, because the card has done the conversion internally. The
   renderer's BT.709 YUV→RGB matrix runs and writes to the SDR swap
   chain.

The same `Set4KSTonemap(bool enableTonemap)` helper is used for both
directions. The Application layer chooses the value based on the
upcoming capture format (`enableTonemap = !useP010`). The retry path
(when `MFVideoFormat_P010` negotiation fails at the MF level and the
pipeline falls back to NV12) re-sends `enableTonemap = true` before the
second `Open()` so the card state and the capture format stay paired
even in the failure case.

---

## 5. Result

- 4K S HDR (P010) capture renders correctly to an HDR display.
- 4K S SDR (NV12) capture renders correctly to an SDR display, and to
  an HDR display through Windows' standard SDR-in-HDR composition path.
- Alt+H toggling between HDR and SDR re-paires the card state and the
  capture format on every reconcile; no restart is needed.
- 4K Pro behavior is preserved exactly. The `IKsPropertySet` property
  call is the canonical path on 4K Pro and is still made on every
  Initialize and Reconcile. The 4K S HID path is invoked unconditionally
  alongside it; on the 4K Pro it returns false silently because no
  matching VID/PID is found.

This is why NitLink carries two card-specific control paths
(`src/capture/elgato_hdr_control.cpp` for the 4K Pro `IKsPropertySet`
path and `src/capture/elgato_hid_4ks.cpp` for the 4K S HID path) rather
than one unified call: the two cards genuinely use different protocols.

---

## 6. Limitations

- **4K S HDR capture is 1080p at 60 fps.** The 4K S driver only
  publishes `MFVideoFormat_P010` at 1080p and 720p. This matches
  Elgato's documented USB-bandwidth ceiling for HDR10 over USB 3 on the
  4K S; 4K60 HDR10 requires the PCIe headroom that only the 4K Pro has.
- **4K60 capture on the 4K S is SDR (NV12).** This is the
  standard SDR pipeline; the HID tonemap is engaged so HDR-source HDMI
  signals are tonemapped to SDR by the card.
- **The 4K S does not expose HDR source auto-detection.** Unlike the
  4K Pro, the 4K S has no `IKsPropertySet` property that returns the
  current HDR InfoFrame. NitLink uses the `hdr_enabled` setting in
  `nitlink.json` (toggled by Alt+H at runtime) to decide whether to
  negotiate P010, and pairs the HID tonemap state with that decision.
- **Brief visual artifact during mid-session PS5 HDR setting changes.**
  When the PS5 HDR mode flips, the HDMI link renegotiates, the capture
  worker exits, and NitLink reconciles to the new format. A frame or
  two during the handoff window may render with a transient green band
  on the left edge before the next reconcile completes. The picture
  self-corrects within about 100 ms. Restarting NitLink avoids it
  entirely if the PS5 HDR mode change is known in advance.
