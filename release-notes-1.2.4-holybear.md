# NitLink 1.2.4 HolyBear Edition

**Unofficial Community Edition — independent community integration build.**
Based on upstream NitLink 1.2.3. Not an official nitlink-dev release.

## What's Included

- Selected HDMI source metadata, VRR and signal resync improvements from [#23](https://github.com/nitlink-dev/nitlink/pull/23).
- FPS telemetry, actual negotiated capture rates and automatic Present Cap fixes from [#31](https://github.com/nitlink-dev/nitlink/pull/31).
- Low-latency audio enhancements from [CN_115's fork, v0.1.2](https://github.com/CN115/nitlink-lowlatency/tree/v0.1.2): adjustable FIFO/render targets, WASAPI exclusive output with focus-loss shared fallback, native-format conversion, adaptive clock drift correction and recovery diagnostics.
- English, Traditional Chinese and Simplified Chinese support. About lists contributors by name, including CN_115, with GitHub links and without language labels.
- Manual HDMI identity and Discord functionality shared with #25 is included once.
- Portable ZIP and a branded Windows x64 Setup installer, with English/Traditional Chinese/Simplified Chinese installer and uninstall pages, Windows UI language detection and a manual language selector.
- Optional native HTTPS update checks in F1 → About → Software Updates, exclusively for HolyBearTW/nitlink's formal Releases. Automatic checks start disabled; enabled checks are limited to once per 24 hours. No automatic download or installation.
- ASCII-armored GPG detached signatures for the final ZIP, Setup and SHA256SUMS.txt, verified before publication and after upload. See [verification instructions and full key fingerprints](https://github.com/HolyBearTW/nitlink/blob/holy-bear/integrated/docs/HOLYBEAR-RELEASE-SIGNING.md).

HUD Present FPS counts successful DXGI submissions, not game FPS or physical display updates. Content FPS is a FrameDiffer estimate. Capture FPS reports observed capture delivery, separate from the negotiated MF rate and Windows monitor refresh.

## Compatibility

- Windows 10 (1809 or later) / Windows 11, x64; DirectX 11 GPU and appropriate capture-card/audio drivers.
- Uses the upstream capture paths, including Elgato devices and GC553Pro. Automated compatibility checks do not establish support for every capture card or sound device.
- Microsoft Edge WebView2 Evergreen Runtime is required for F1 settings and is installed separately. Current Windows 11 installations normally include it.
- Required release VC++ runtime DLLs are bundled unmodified from the Visual Studio redistributable directory, with Microsoft notices. The ZIP includes HTML, JavaScript, fonts, all three locales, NIS resources, licenses, source provenance and BUILD.txt.
- Extract the complete ZIP into a writable folder and run NitLink.exe. No user configuration or device-specific preset is bundled. Exclusive audio starts disabled; buffering defaults to 12 ms FIFO / 10 ms render target.
- Alternatively, run Setup to install into the standard per-user application directory. Start menu shortcuts are created; desktop shortcut and launching NitLink are optional. Upgrades/uninstall retain generated settings and other user files. Original NitLink installations are not migrated.

## Testing

- The initial combined source candidate passed a clean x64 Release build, 14/14 targeted regressions and 35/36 full CTest tests.
- The sole local baseline failure was `webview_runtime_security`: `FAIL restarted host reloads the packaged menu`, also reproduced on clean upstream/main. Its original assertion is retained.
- Coverage includes FPS statistics, 1152 Present Cap decisions, both renderer wait paths, actual EndFrame/receipt integration, signal resync, MF/native P010 negotiation, HDR/placeholder handling, source metadata, audio conversion, clock recovery, packetized drift and mock WASAPI focus/exclusive behavior.
- The release workflow runs the complete test suite with WebView runtime tests enabled. It permits only that exact single baseline assertion failure, rejects skipped/missing tests and any new failure, and records the actual CI result below.
- Each release is rebuilt from its tag. The workflow checks EXE version/architecture, bundled files, Microsoft runtime signatures, source/tag identity and file hashes after extracting the ZIP into a fresh directory.
- Native update regressions use mock responses for numeric version ordering, typed formal-release validation, malformed JSON, timeout/403/429/500, opt-in, 24-hour interval and cancellation. WebView tests cover update state in all three languages without external network access.
- CI verifies Setup payload bytes, all three installer languages, install/upgrade/uninstall metadata, shortcuts and retained settings. An isolated signing job verifies all three GPG signatures with only the pinned public key and rechecks SHA-256; publication fails if any required check fails.

## Known Limitations

- This combined edition has not completed a full GC553Pro hardware validation run. HDR/SDR transitions, No Signal handling and input-to-photon latency remain insufficiently verified on the combined build.
- Long-run audio drift, audio/video synchronization, exclusive/shared focus switching and sound quality still need hardware validation. Low buffer targets are not suitable for every capture card or sound device.
- The existing WebView restart test issue is retained and reported. A passing CI run on another locale/runtime does not establish that the local issue is fixed.
- The previously observed buffer overwrites with Low Latency off remain unexplained. FrameDiffer estimates may differ from actual game cadence.
- No precise CPU/GPU improvement or latency reduction is claimed. Audio queue diagnostics are not end-to-end latency measurements.
- The app and installer have no trusted Windows Authenticode publisher signature. GPG verification does not eliminate SmartScreen warnings or Defender false positives. Long-term upgrades and every interactive installer page still warrant broader user testing.

## Credits

- Original NitLink project: [nitlink-dev/nitlink](https://github.com/nitlink-dev/nitlink), NitLink Contributors.
- Low-latency audio and Simplified Chinese: [CN_115](https://github.com/CN115), [CN115/nitlink-lowlatency v0.1.2](https://github.com/CN115/nitlink-lowlatency/tree/v0.1.2).
- HolyBear Edition integration and release automation: [HolyBearTW](https://github.com/HolyBearTW).

Original MIT and third-party notices are preserved. See HOLYBEAR-SOURCES.json and docs/cn115 for exact original source commits and authorship.
