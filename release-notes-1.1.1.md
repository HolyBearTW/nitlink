# NitLink 1.1.1

Maintenance release: the USB audio stutter fix promised in 1.1.0, an aspect-ratio option, and a clear answer when Windows blocks the card's audio.

## Fixed

- **Audio stutter on USB cards.** The audio router now runs event-driven on both WASAPI endpoints and keeps a small buffer between the capture card's clock and the playback device's clock, steering it with single-frame corrections that are inaudible. The previous pump polled on a timer, discarded the tail of any capture packet that did not fit the render buffer, and had no drift handling, which surfaced as periodic stutter, most audibly on the 4K S. Routing health (buffer fill, underruns, overruns, corrections) is logged every five seconds for diagnosis.

- **Silent NitLink when Windows blocks microphone access.** The Windows Microphone privacy switch also blocks capture-card audio for desktop apps and reports it as a plain access denial. NitLink now names the switch in an on-screen notice instead of staying quiet.

## New

- **Aspect ratio option.** `Alt+A`, the F1 panel, or `aspect_ratio` in `nitlink.json`. Auto shows the source at the ratio the card reports; 4:3, 16:9, 16:10, 21:9, or any custom `W:H` squeezes or letterboxes the picture, which restores 4:3 consoles that a card delivers stretched inside a 16:9 frame; Stretch fills the window. Requested in [#4](https://github.com/nitlink-dev/nitlink/issues/4).

## Known issues

- **The stutter fix is verified by counters on a 4K Pro and awaits confirmation on a 4K S.** If you own a 4K S, the five-second `[NitLink/Audio] stats` lines in DebugView are the thing to report: underrun and overrun should stay at 0 and the slip counts should stay small.

## Install

1. Download `NitLink-1.1.1-win64.zip` below.
2. Extract anywhere.
3. Double-click `NitLink.exe`. Windows SmartScreen may warn (the binary is not yet code-signed); click "More info" then "Run anyway".

Requirements: Windows 10 (1809+) or Windows 11, a DirectX 11 capable GPU, the Microsoft Edge WebView2 runtime, the VC++ 2015-2022 redistributable, and an Elgato 4K Pro, 4K S, or 4K X.

## Attribution

Full third-party licenses in `LICENSES.md`. External research, protocol references, and code contributors in `ACKNOWLEDGMENTS.md`.
