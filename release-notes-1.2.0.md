# NitLink 1.2.0

The USB audio stutter fix promised in 1.1.0, a settings panel that no longer covers the picture, an aspect-ratio option, and present pacing that can run the picture at the source's real frame rate.

This release supersedes the 1.1.1 pre-releases. Those were release candidates for what became this version; the present-pacing fix below is not in them.

## Fixed

- **Audio stutter on USB cards.** The audio router now runs event-driven on both WASAPI endpoints and keeps a small buffer between the capture card's clock and the playback device's clock, steering it with single-frame corrections that are inaudible. The previous pump polled on a timer, discarded the tail of any capture packet that did not fit the render buffer, and had no drift handling, which surfaced as periodic stutter, most audibly on the 4K S. Routing health (buffer fill, underruns, overruns, corrections) is logged every five seconds for diagnosis.

- **Audio drift correction now applies in both directions.** The below-band correction reserved no output frame, so a requested repeat consumed the entire render request and never repeated. A capture clock running slower than the playback device therefore drained the buffer toward its floor instead of recovering, leaving almost no headroom against jitter. Only the 1.1.1 pre-releases carried this. A one-hour simulation at -100 ppm now holds inside the intended band with no underruns or overruns.

- **Silent NitLink when Windows blocks microphone access.** The Windows Microphone privacy switch also blocks capture-card audio for desktop apps and reports it as a plain access denial. NitLink now names the switch in an on-screen notice instead of staying quiet.

- **HUD visibility is remembered.** Startup respects `show_overlay` in `nitlink.json`, and `Ctrl+F3` saves the choice immediately. Rebuilding the graphics renderer also preserves the preference.

## New

- **The F1 panel is now a sidebar.** It opens as a translucent strip on the right and the picture keeps playing beside it, so a setting can be changed while watching the result. The Position row on the Video tab cycles Right, Left, and Full, the previous whole-window panel; `panel_side` and `panel_width` in `nitlink.json` store the choice. While a panel is open the desktop compositor handles the window (about one frame of extra latency, VRR paused), which is why the earlier panel covered the picture; closing it restores the direct path. The Ctrl+F3 HUD is redrawn in the same style: title band, plain-ink metrics that only take color when something degrades, amber sparklines, and the pipeline badges in the footer.

- **Present pacing, and VRR pacing that actually runs.** A single Present Pacing row on the Video tab replaces the old VRR toggle and picks how often the picture is presented: **Display refresh** (every frame the monitor draws, lowest latency, the default), **Capture rate** (once per frame the card delivers), or **Source frame rate** (only when the picture changes). The old VRR toggle never took effect while low-latency mode was on, and low-latency mode is on by default, so it did nothing for almost everyone; that override is gone. Source frame rate is what a G-Sync or FreeSync display needs in order to follow the game instead of the card's constant delivery rate, what an external frame-generation tool such as Lossless Scaling needs in order to read the real frame rate, and what stops 30 fps content juddering against a present rate that is not a multiple of it. Measured on a 4K Pro with a 30 fps source on a 120 Hz display: presents drop from 100 a second to 32, matching the content, and Lossless Scaling reads 30 in and 60 out. Both paced modes add up to one capture interval of latency and turn the present cap off. `present_pacing` in `nitlink.json`; an existing `vrr_present_pacing = true` is migrated to Source frame rate.

- **Aspect ratio option.** `Alt+A`, the F1 panel, or `aspect_ratio` in `nitlink.json`. Auto shows the source at the ratio the card reports; 4:3, 16:9, 16:10, 21:9, or any custom `W:H` squeezes or letterboxes the picture, which restores 4:3 consoles that a card delivers stretched inside a 16:9 frame; Stretch fills the window. Requested in [#4](https://github.com/nitlink-dev/nitlink/issues/4).

- **Resizable picture-in-picture.** Drag an edge to resize one dimension or a corner to scale both together. `Ctrl+Alt+Arrows` adjusts width and height; `Alt+Up/Down` scales proportionally. Add `Shift` for larger steps, or hold the keys to repeat. Resizing keeps PiP on its current monitor, and its size and position are saved on exit.

- **PiP opacity control.** Adjust opacity from 10% to 100% in F1 > Video > Picture-in-picture, or use `Alt+Left/Right` in 5-point steps (`Shift` for 10 points). The keys repeat when held, and the choice is saved on exit.

- **F11 fullscreen shortcut.** `F11` now toggles fullscreen alongside `Alt+Enter`.

## Changed

- **PiP shortcut moved to Alt+O.** Replaces `Alt+P` to avoid a shortcut conflict observed with another application.

## Known issues

- **Source frame rate pacing needs a signal the card captures natively.** When the card has to scale, the scaler makes duplicated frames differ slightly, the frame differ counts every one of them as new content, and the mode presents at the delivery rate instead of the source rate. The case to avoid on a 4K Pro is a 4K120 source, which the card can only capture as 1080p120. A mode the card captures natively, such as 4K60, works as intended.

- **The stutter fix is verified by counters on a 4K Pro and awaits confirmation on a 4K S.** If you own a 4K S, the five-second `[NitLink/Audio] stats` lines in DebugView are the thing to report: underrun and overrun should stay at 0 and the slip counts should stay small.

## Install

1. Download `NitLink-1.2.0-win64.zip` below.
2. Extract anywhere.
3. Double-click `NitLink.exe`. Windows SmartScreen may warn (the binary is not yet code-signed); click "More info" then "Run anyway".

Requirements: Windows 10 (1809+) or Windows 11, a DirectX 11 capable GPU, the Microsoft Edge WebView2 runtime, the VC++ 2015-2022 redistributable, and an Elgato 4K Pro, 4K S, or 4K X.

## Attribution

Full third-party licenses in `LICENSES.md`. External research, protocol references, and code contributors in `ACKNOWLEDGMENTS.md`.
