# NitLink 1.2.1

Fixes laggy pause menus and sudden frame-rate drops when using Lossless Scaling with Source frame rate pacing.

## Fixed

- **Pause menus no longer drop to about 4 fps.** In Source frame rate mode, a still picture previously reduced presents to one every 250 ms. Lossless Scaling saw that as a 4 fps input, and navigating a menu could feel delayed. NitLink now remembers the measured source cadence and keeps presenting at that cadence while the picture is still.
- **The HUD shows the presentation rate in Source frame rate mode.** It previously fell back to the capture rate on still images, hiding the drop that Lossless Scaling was receiving.

The frame-difference classifier and the Display refresh and Capture rate modes are unchanged.

## Testing

The beta was tested by Rawbowke on an Elgato 4K Pro with separate AMD GPUs for rendering and Lossless Scaling. The tester reported that lag spikes were fixed, NitLink's frame rate matched Lossless Scaling's base frame rate, and Gran Turismo 7 was smooth. Thanks for the testing and feedback.

## Install

Download `NitLink-1.2.1-win64.zip`, extract it into a new folder and run `NitLink.exe`. Copy your existing `nitlink.json` into that folder to keep your settings.

For this fix, select **F1 > Video > Present Pacing > Source frame rate**.
