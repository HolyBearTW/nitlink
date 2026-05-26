# NitLink 1.0.0-rc4

Fourth public release candidate. Direct HDR auto-detection on the 4K S, live latency measurement replaces baked HUD constants, window title now surfaces source + resolution + effective HDR state.

## New

- **4K S HDR auto-detection.** Reads the live HDR Static Metadata InfoFrame directly from the card's MCU via vendor HID. When the source flips HDR on or off (e.g. PS5 Settings → Screen and Video → HDR), NitLink picks up the change on the next reconcile and swaps capture format accordingly. The 4K Pro's existing IKsPropertySet detection is unchanged.

- **4K S Alt+H is now a real capture-format swap.** With HDR engaged the 4K S captures at 1080p P010 (BT.2020 PQ); with HDR off it captures at the user's preferred resolution as NV12 with the card's internal HDR-to-SDR tonemap. The USB bandwidth can't fit 4K HDR, so the trade-off is explicit and runtime-flippable.

- **Source identifier in the window title.**
  - 4K S: `NitLink - PlayStation 5 [HDR]` / `[SDR]` (HDMI Source Product Descriptor InfoFrame read from the card).
  - 4K Pro: `NitLink - 3840x2160 @ 60Hz [HDR]` / `[SDR]` (resolution + fps from the Elgato custom property set).
  - The HDR/SDR suffix reflects the renderer's effective mode, not the capture format: honest about what the user actually sees.

- **HUD "APP INGEST" replaces baked-constant TOTAL LATENCY.** The HUD's latency field used to display `16 + frame_capture + 8` where `16` and `8` were tuned constants from a single test rig. The new field measures actual card-driver-to-app delivery time via `MFSampleExtension_DeviceTimestamp`. When the driver doesn't populate it (some non-Elgato cards), the field reads 0. End-to-end photon-to-photon latency stays out of NitLink's measurement scope; "APP INGEST" is what NitLink can measure honestly.

- **HDR-capable source heuristic as fallback.** When direct HDR signal detection isn't available (HID error, MCU non-responsive, non-Elgato cards), NitLink falls back to a source-identifier lookup: known HDR-capable consoles (PS5) default the pipeline to HDR. User can override with Alt+H. Disable with `hdr_auto_from_source = false` in `nitlink.json`.

## Install

1. Download `NitLink-1.0.0-rc4-win64.zip` below.
2. Extract anywhere.
3. Double-click `NitLink.exe`. Windows SmartScreen may warn (binary is not yet code-signed); click "More info" then "Run anyway".

Requirements: Windows 10 (1809+) or Windows 11, DX11-capable GPU, Microsoft Edge WebView2 runtime, VC++ 2015-2022 redistributable, an Elgato 4K Pro, 4K S, or 4K X (4K X uses generic Media Foundation path; vendor controls land in 1.1).

## What's still coming in 1.1

- VRR Present Pacing differ rework with Media Foundation timestamp as a second signal. Addresses [#1](https://github.com/nitlink-dev/nitlink/issues/1).
- Native 4K X support via Elgato's UVC Extension Unit protocol. Addresses [#2](https://github.com/nitlink-dev/nitlink/issues/2).
- Source identifier coverage expansion (Xbox Series X|S, Switch 2, PC).
- Audio endpoint matching with Wave Link active.

## Feedback

In-app "Send feedback" link in the F1 settings panel opens a Tally form. Tested reports route to a Discord webhook for fast turnaround.

## Attribution

Full third-party licenses in `LICENSES.md`. External research and protocol references in `ACKNOWLEDGMENTS.md`.
