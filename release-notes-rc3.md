# NitLink 1.0.0-rc3

Third public release candidate. Manual capture format override, false-positive fixes, and quality-of-life polish since rc2.

## Bug fixes

- **Placeholder false positives on dark intro cards.** The "no signal" detector matched dark studio-logo intro cards (Star Wars Jedi: Lucasfilm card and similar) whose 9-zone luma signature is pixel-identical to the baked Elgato placeholder. Tightened the per-zone tolerance from 8 to 4 and the streak length from 5 to 15 frames, then added a motion-recency gate that suppresses confirmation when the source has produced fresh content or motion in the last 5 seconds. Real placeholder is preceded by silence; intro cards are preceded by animation.

- **1080p override silent downgrade on Elgato 4K Pro.** The 4K Pro's Media Foundation driver accepts a 1920x1080 RGB32 `SetCurrentMediaType` request with S_OK and then silently delivers 4K BGRA. The override appeared to take effect but the user's resolution choice was discarded. A post-negotiation guard now reads back the actual dimensions and fps, and rejects the attempt if the driver substituted different values. The fallback path activates and a toast surfaces the format change. Verified on hardware via a device-switch repro (4K S 1080p override carried over to 4K Pro).

- **Cascade dropdowns offered framerates not valid for the selected resolution.** `uniqueFps()` previously returned the union of every framerate across the whole device when Resolution was Auto, which surfaced unreachable combinations like 4K@120fps on the 4K Pro. The filter is now strict: framerate options only appear when a resolution is selected, and only contain framerates that have a renderable format at that exact resolution. The same strict filter applies to the format dropdown.

- **Cascade could send malformed `0x0 @ Nfps` overrides to the backend.** Resolution=Auto with a specific framerate is rarely achievable on real hardware. The handler now resets the whole override to Auto when Resolution is set to Auto. Switching to a new resolution validates persisted framerate and format against the new resolution, resetting either axis if the prior choice is no longer valid.

- **VRR pacing log: spurious fps spike after HDR/SDR reconcile.** `hdmiFps` could read ~4.2 billion in one sample immediately after an HDR/SDR reconcile, caused by unsigned-subtract underflow when the FrameBuffer's monotonic frame counter restarted from zero. The sample now treats counter resets as "no useful delta this window" and writes 0 instead.

## New

- **F1 Source picker: manual capture format override.** Cascade dropdowns for Resolution, Framerate, and Format inside the Source popover. Each dropdown is data-driven from the live device's media-type list and filtered to renderable formats (P010, NV12, BGRA). Picking Auto on any axis preserves automatic negotiation for that dimension. If the chosen combination is not achievable on the live source, the pipeline falls back to automatic negotiation and surfaces a toast: "Capture format unavailable, reverted to automatic." Persisted across launches in `nitlink.json`.

- **Screenshot saved toast.** Ctrl+S now displays a toast showing the saved filename. Click the filename to open the containing folder in Windows Explorer with the file pre-selected. Auto-dismisses after 3.5 seconds.

- **HDR auto-detect availability notice.** The F1 Source picker shows a note under the HDR toggle when the active card does not expose Elgato's HDR InfoFrame property (4K S and generic Media Foundation devices): "HDR auto-detect unavailable on this card. Toggle manually with Alt+H." Makes it clear that the HDR toggle is the only way to switch the pipeline on those cards.

## Install

1. Download `NitLink-1.0.0-rc3-win64.zip` below.
2. Extract anywhere.
3. Double-click `NitLink.exe`. Windows SmartScreen may warn (binary is not yet code-signed); click "More info" then "Run anyway".

Requirements: Windows 10 (1809+) or Windows 11, DX11-capable GPU, Microsoft Edge WebView2 runtime, VC++ 2015-2022 redistributable, an Elgato 4K Pro, 4K S, or 4K X (4K X uses generic Media Foundation path; vendor controls land in 1.1).

## What's still coming in 1.1

- VRR Present Pacing differ rework with Media Foundation timestamp as a second signal. Addresses [#1](https://github.com/nitlink-dev/nitlink/issues/1).
- Native 4K X support via Elgato's UVC Extension Unit protocol. Addresses [#2](https://github.com/nitlink-dev/nitlink/issues/2).
- Cam Link 4K validation pass.

## Feedback

In-app "Send feedback" link in the F1 settings panel opens a Tally form. Tested reports route to a Discord webhook for fast turnaround.

## Attribution

Full third-party licenses in `LICENSES.md`. External research and protocol references in `ACKNOWLEDGMENTS.md`.
