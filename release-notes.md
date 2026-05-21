# NitLink 1.0.0-rc1

First public release candidate. Built and hardware-tested on the Elgato 4K Pro (PCIe) and Elgato 4K S (USB) against PS5 on an LG C3 OLED desktop setup and an Acer Nitro 5 laptop.

## Highlights

- **Real HDR10 capture**: native 10-bit P010 (BT.2020 PQ) through an R10G10B10A2 HDR10 swap chain. Zero color conversion. 4K Pro auto-detects HDR via the Elgato InfoFrame property; 4K S uses the `hdr_enabled` config.
- **4K S real HDR**: reverse-engineered vendor HID tone-map control, paired with the negotiated capture format (OFF for P010, ON for NV12).
- **Low end-to-end latency**, measured photon-to-photon: 19.6 ms ± 0.9 ms on the 4K Pro, 33.2 ms ± 0.5 ms on the 4K S. NitLink itself contributes ~1 ms (PresentMon-verified).
- **VRR Present Pacing**: G-Sync / FreeSync follows the source's real unique-frame rate. Off by default; opt in from the F1 settings panel on a VRR display.
- **Multi-device source picker** in the F1 sidebar — switch capture devices at runtime without restarting. Strict explicit switching with safe rollback on failure.
- **Smart signal handling**: format-tagged placeholder fingerprints with temporal-stability gating. Brief HDMI handshake windows (PS5 boot logo, source switch, SDR ↔ HDR) hold the last good frame instead of flashing the card's NO SIGNAL placeholder.
- **NVIDIA Image Scaling** (compute-shader spatial upscale + sharpening).
- **WASAPI audio routing**, **borderless fullscreen + picture-in-picture**, **Discord Rich Presence**.

## Install

1. Download `NitLink-1.0.0-rc1-win64.zip` below.
2. Extract anywhere.
3. Double-click `NitLink.exe`. Windows SmartScreen may warn (binary is not yet code-signed); click "More info" → "Run anyway".

Requirements: Windows 10 (1809+) or Windows 11, DX11-capable GPU, Microsoft Edge WebView2 runtime, VC++ 2015-2022 redistributable, an Elgato 4K Pro or 4K S.

## Feedback

In-app "Send feedback" link in the F1 settings panel opens a Tally form. Tested reports route to a Discord webhook for fast turnaround.

## Attribution

- **Matt Pettineo (TheRealMJP)**: Catmull-Rom resampling reference (MIT).
- **Brandon (13bm), [elgato4k-linux](https://github.com/13bm/elgato4k-linux)**: 4K S HID protocol reference. NitLink's HID implementation in `src/capture/elgato_hid_4ks.{h,cpp}` is independent, written from scratch against the Win32 SetupAPI / HID Class API. See `ACKNOWLEDGMENTS.md` and `docs/4ks-hdr-tonemap.md`.

Full third-party licenses in `LICENSES.md`.
