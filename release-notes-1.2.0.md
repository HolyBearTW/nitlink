# NitLink 1.2.0

The USB audio stutter fix promised in 1.1.0, a settings panel that no longer covers the picture, an aspect-ratio option, and present pacing that can run the picture at the source's real frame rate.

This release supersedes the 1.1.1 pre-releases. Those were release candidates for what became this version; the present-pacing fix below is not in them.

## Fixed

- Elgato Game Capture 4K60 Pro MK.2 is now recognized; HDR auto-detect works and colors match Elgato Studio (limited-range chroma decode).

- **Audio stutter on USB cards.** The audio router now runs event-driven on both WASAPI endpoints and keeps a small buffer between the capture card's clock and the playback device's clock, steering it with single-frame corrections that are inaudible. The previous pump polled on a timer, discarded the tail of any capture packet that did not fit the render buffer, and had no drift handling, which surfaced as periodic stutter, most audibly on the 4K S. Routing health (buffer fill, underruns, overruns, corrections) is logged every five seconds for diagnosis.

- **Audio drift correction now applies in both directions.** The below-band correction reserved no output frame, so a requested repeat consumed the entire render request and never repeated. A capture clock running slower than the playback device therefore drained the buffer toward its floor instead of recovering, leaving almost no headroom against jitter. Only the 1.1.1 pre-releases carried this. A one-hour simulation at -100 ppm now holds inside the intended band with no underruns or overruns.

- **Silent NitLink when Windows blocks microphone access.** The Windows Microphone privacy switch also blocks capture-card audio for desktop apps and reports it as a plain access denial. NitLink now names the switch in an on-screen notice instead of staying quiet.

- **HUD visibility is remembered.** Startup respects `show_overlay` in `nitlink.json`, and `Ctrl+F3` saves the choice immediately. Rebuilding the graphics renderer also preserves the preference.

- **Discord Rich Presence never connected.** The connection was marked running only after the handshake completed, but `ReadFrame` uses that same state to decide whether to keep waiting. A valid READY reply was therefore rejected before the worker could start, so the feature failed for every user who enabled it. The state is now set before the handshake payload is read, and cleared again if the handshake fails.

- **A capture card never recovered after being unplugged.** A failed reopen consumed the retry flag and nothing re-armed it, so a card removed and reconnected stayed dark until NitLink was restarted. Failed opens now retry against a one-second deadline and wait for the selected device to reappear.

- **Closing NitLink could hang.** Shutdown joined the capture thread while that thread was blocked inside a synchronous read, so a card that stopped delivering samples held the process open. The reader is now asynchronous with shared callback state, so `StopCapture` wakes the worker, joins it, and flushes pending requests afterwards. Late callbacks touch only their own shared state, and a fatal reader flag suppresses further calls.

- **The VRR present pacing setting never took effect.** Low-Latency Mode forced a present every loop iteration after the pacing decision had already been made, and Low-Latency Mode is on by default, so the toggle shipped in 1.1.0 did nothing for almost every user. The override is gone. Low-Latency Mode now controls only where the swap-chain wait happens, which is what it documents, and pacing is chosen explicitly on the new Present Pacing row.

- **Placeholder frames flooded the ring buffer.** With the console off, a 4K Pro streams its own NO SIGNAL card as a valid frame stream at over 200 fps, and every one of those 12 MB frames was copied into the ring buffer to display a static image. The capture thread now fingerprints each raw frame against the fingerprint confirmed at detection time and drops exact matches before the write. The real placeholder is bit-identical between frames, so an exact match is the correct test: any variation at all belongs to returning source content and must reach the detector.

- **Incomplete frames could reach the screen.** Uploads shorter than a complete frame are rejected, with bounded padding still allowed for contiguous buffers, and the last valid texture is retained instead of uploading stale rows from the tail of the frame buffer.

- **The first frame after a stream reset could be suppressed.** Pending frame-differ readbacks are invalidated on reset, so a classification computed against the previous stream can no longer hide the first frame of the new one in Source frame rate mode.

- **Switching cards left the previous card's state behind.** Card-specific format policy, source state and pollers are rebuilt on a device change, with rollback if the switch fails, and 4K S HID commands are sent only when a 4K S is selected.

- **HDR detection stopped after an initial SDR result.** Source polling continues after any successful query rather than settling on the first answer, so a console that switches into HDR after NitLink is already running is picked up.

- **The 4K Pro window title stayed wrong after a no-signal launch.** Source timing is refreshed when the first real frame arrives after signal loss, so a session started before the console was on now reports the correct mode once the picture appears.

- **The settings panel could act on a destroyed host.** Callbacks from expired WebView initializations are rejected and late controllers are closed without touching the host. Pending Show requests are honored and focus moves when controller creation completes.

- **Dependent format lists went stale.** Source selectors refresh without rebuilding the focused control, so a list no longer keeps the previous card's options while it has focus.

- **Large and small hotkey steps fired together.** Modifiers are now matched exactly across all 37 registered combinations, so a Shift-modified PiP nudge no longer triggers the unmodified step as well.

- **PiP position was lost on multi-monitor setups.** Saved negative desktop coordinates are preserved, with only (-1, -1) reserved to mean automatic placement, so a window parked on a monitor left of or above the primary display returns there.

- **HDR diagnostic patches were written in the wrong color space.** They are converted from linear scRGB to BT.2020 and PQ before reaching the HDR10 target.

- **A CMake install produced an executable that could not run.** The settings HTML, the NIS runtime shader include and the license notices are installed alongside the binary.

- **The 4K S link specification was wrong in the documentation.** It is USB 3.2 Gen 1 at 5 Gbps, and native 4K60 MJPEG is now distinguished from native 4K NV12 at up to 30 fps, with the vendor format table linked as the reference.

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
