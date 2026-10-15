# NitLink 1.2.4

Low-latency audio with drift correction, a Simplified Chinese interface, an adaptive duplicate floor for noisy capture cards, clean SDR from an Elgato 4K X at 4K120 HDR, and a settings panel that closes cleanly in a window.

## Changes

- **Duplicate detection learns the capture card.** Cards that do not repeat a held frame pixel-exactly made a 30 fps game read as 60 unique frames, so Source frame rate pacing presented every repeat and external frame generators got identical pairs. The differ now learns the card's repeat band. Cards with exact repeats behave as before.
- **Content FPS in the HUD, exact present pacing.** The HUD headline is the game's own frame rate. Automatic present pacing uses the display's exact refresh rate (59.94, 119.88), so the present cap no longer runs uncapped when the capture rate is close to it. By HolyBearTW.
- **Low-latency audio.** Configurable buffering (3 to 100 ms), device-clock drift correction, native playback formats, optional exclusive output, automatic recovery after underruns. Defaults: 40 ms FIFO, 30 ms queue. Lower values are in the settings panel; what your card and driver tolerate varies. By CN_115.
- **Simplified Chinese interface** with automatic system-language selection. By CN_115.
- **Elgato 4K X with an HDR console at 4K120.** The card delivers 4K120 as 8-bit; NitLink now keeps its HDR-to-SDR conversion on for that path, so the picture is clean SDR instead of raw HDR codes. 4K HDR on the 4K X is 30 fps.
- **Wide settings layout.** With Position set to Full, the F1 panel covers the window with a dedicated wide layout. By HolyBearTW.
- **About panel** links the Steam version.
- **The F1 panel closes cleanly in a window.** It could leave a frozen picture of itself over the video until the window moved. Fullscreen was not affected.

## Credits

- HolyBearTW: wide Full-mode settings layout (#26), FPS telemetry and automatic present pacing (#31).
- CN_115 (github.com/CN115): low-latency audio, drift correction, Simplified Chinese.

## Install

Unzip and run NitLink.exe from the new NitLink-1.2.4-win64 folder. To keep your settings, copy nitlink.json from your old NitLink folder into it.

A NitLink version with experimental frame generation (30 fps console games at 60 fps) is in closed beta and coming soon on Steam: https://store.steampowered.com/app/5346980. This free viewer stays free.
