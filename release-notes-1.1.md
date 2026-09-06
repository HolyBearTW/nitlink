# NitLink 1.1.0

First feature release after the 1.0.0 release-candidate series. Native Elgato 4K X support, real GPU timing in the HUD, a low-latency present path that matches or beats the Elgato Capture Utility on a VRR display, true-HDR screenshots, and a capture color-range override.

## New

- **Native Elgato 4K X support.** Live source detection (resolution, refresh rate, HDR state, and console identity) read from the card's UVC Extension Unit over a held-handle path that never wedges the device. The window title surfaces the detected source the same way it does on the 4K Pro and 4K S. Addresses [#2](https://github.com/nitlink-dev/nitlink/issues/2).

- **Low-latency mode (Alt+L).** A present-on-arrival path paired with an ALLOW_TEARING swap chain and a VRR-aware present cadence. On a VRR display this engages variable refresh tear-free and brings end-to-end latency to parity with (and in measurement below) the Elgato 4K Capture Utility. On by default; toggle with Alt+L or the F1 settings panel.

- **Real GPU frame time in the HUD.** A new `GPU N.N ms` field driven by Direct3D 11 timestamp queries reports actual per-frame GPU work. It reads back a few frames late so it never stalls the pipeline, and shows `GPU --` when timestamp queries are unavailable.

- **NIS fp16 half-precision path.** The NVIDIA Image Scaling pass uploads its coefficients as R16 half-precision and runs in `min16float`, lowering GPU cost on bandwidth-limited cards with no visible change in output.

- **Capture color-range override (Alt+R).** A three-state cycle: Auto / Full / Limited. Auto trusts the range Media Foundation reports, which is correct on all three Elgato cards; the manual states cover third-party cards that misreport their range. Each change shows a brief on-screen notice.

- **True-HDR screenshots.** In HDR mode, Ctrl+S writes a scRGB FP16 `.jxr` sidecar alongside the shareable tonemapped-to-SDR `.png`. The `.jxr` opens as real HDR in the Windows Photos app, preserving highlights and wide gamut.

- **VRR present-pacing rework.** The content-frame differ uses a tiled maximum-SAD detector, which correctly handles localized motion and 30fps-over-60Hz HDMI alternation that the previous whole-frame compare misread as duplicate frames. Addresses [#1](https://github.com/nitlink-dev/nitlink/issues/1).

## Changed

- HDR source color is decoded from each card's reported range rather than per-card special cases, fixing washed-out or over-saturated HDR on some sources.

- **Audio routing works across format mismatches and playback device changes.** The audio router now opens the playback endpoint through the Windows audio engine's format conversion, so a playback device with a different sample rate or channel layout (a 44.1 kHz headset, virtual surround) gets audio instead of silence. It also listens for endpoint changes: switching the default playback device, unplugging the card, or an in-place format change rebuilds the affected side on the worker thread instead of leaving NitLink mute until restart. Contributed by Nathan K. ([#5](https://github.com/nitlink-dev/nitlink/pull/5)).

## Hardening

- Capture, renderer, audio, and Discord paths received lifetime, concurrency, and error-handling work: Direct3D device-loss recovery, checked Direct3D and Media Foundation results, bounded Discord IPC reads, a tightened WebView2 navigation allow-list, and checked WASAPI results throughout the audio router.

- **Device-loss recovery retries.** If the GPU goes away mid-session (driver reset, sleep/resume, a TDR), the renderer and its dependents rebuild when the device comes back, and recovery keeps retrying while the GPU is unavailable instead of freezing the window or exiting. Swap-chain resize failures caused by a lost device route into the same recovery. Verified against forced GPU resets.

- **The present loop never waits on the GPU.** The frame differ's readback is non-blocking, so when the GPU is busy with other work (another application, or the desktop compositor) NitLink keeps its cadence instead of dropping to two thirds of the source rate.

- **Exit can no longer hang on Discord.** A stalled Discord Rich Presence connection is cancelled on shutdown so NitLink always closes promptly, and partially delivered IPC frames no longer park the worker.

- **Low-Latency Mode wording.** The F1 panel, README, and config comments now describe what turning Low-Latency Mode off actually does: frames are held for up to one refresh before present. The present path is tearing-allowed in both states, and on a VRR display neither state shows tearing.

## Known issues

- **Audio routing can stutter on USB cards.** Reported on the 4K S; the same mechanism applies to any USB card. NitLink copies capture audio to the playback device without compensating for clock drift between the two devices, which surfaces as periodic stutter, most noticeably over USB. A fix is in progress for 1.1.1. Workaround: send game audio to the TV or receiver over the card's HDMI passthrough and mute NitLink's audio; video is unaffected.

## Install

1. Download `NitLink-1.1.0-win64.zip` below.
2. Extract anywhere.
3. Double-click `NitLink.exe`. Windows SmartScreen may warn (the binary is not yet code-signed); click "More info" then "Run anyway".

Requirements: Windows 10 (1809+) or Windows 11, a DirectX 11 capable GPU, the Microsoft Edge WebView2 runtime, the VC++ 2015-2022 redistributable, and an Elgato 4K Pro, 4K S, or 4K X.

## Attribution

Full third-party licenses in `LICENSES.md`. External research, protocol references, and code contributors in `ACKNOWLEDGMENTS.md`.
