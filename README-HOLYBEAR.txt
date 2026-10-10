NitLink 1.2.4 HolyBear Edition (1.2.4-holybear)
Unofficial community integration build based on NitLink 1.2.3.
Not an official nitlink-dev release.

Extract the complete ZIP into a writable folder, then run NitLink.exe.
Keep its HTML, assets, locales and third_party directories together.
No personal nitlink.json or capture-device preset is included. Settings are
created on first launch in the working directory; launching from Explorer
uses the application's folder. No separate development checkout is required.

Alternatively run the multilingual Setup.exe. It installs under your user's
standard Programs directory, creates Start menu shortcuts, and offers optional
desktop shortcut and launch. Windows UI language selects English, Traditional
Chinese (TW/HK/MO), or Simplified Chinese (CN/SG), with a manual selector.
Upgrades and uninstall retain your settings and other generated user files.
The installer does not migrate or change an existing original NitLink installation.

Requirements: Windows 10 1809+ / Windows 11 x64, DirectX 11 GPU, suitable
capture/audio drivers, and Microsoft Edge WebView2 Evergreen Runtime.
Required release Microsoft VC++ DLLs are included with their notices.

F1 opens settings. Choose capture device/format and language there.
Ctrl+F3 toggles the FPS HUD; Alt+L toggles low latency; Alt+V toggles VSync;
F11 / Alt+Enter toggles fullscreen. Audio defaults to shared mode, drift
correction on, 12 ms FIFO and 10 ms render targets. Optional exclusive output
requests the sound device's supported minimum period; it falls back to shared
on focus loss and returns after stable foreground. Changes can briefly
restart audio. Buffer targets and diagnostics are not measured end-to-end latency.

F1 About includes Software Updates. Automatic checks are OFF by default.
Enable them to allow one background GitHub check per 24 hours, or check manually.
Only HolyBearTW/nitlink's formal Releases are queried; no token, personal settings,
device IDs or game information is sent. View/Download opens the release webpage;
there is no automatic download or installation. See docs/HOLYBEAR-UPDATES.md.

Present FPS is successful DXGI submissions, not game FPS or scan-out rate.
Content FPS is an estimate. Actual capture rate and monitor refresh are separate.

BUILD.txt records the exact build commit/tag and integration sources.
SHA256SUMS.txt records package file hashes; the separate release checksum file
records ZIP, Setup and release-note hashes. Download their GPG signatures and
verify the full trusted fingerprint before installing; see
docs/HOLYBEAR-RELEASE-SIGNING.md and licenses/HOLYBEAR-RELEASE-PUBLIC-KEY.asc.
The executable/installer is not Authenticode-signed. GPG does not remove
SmartScreen warnings or prevent Windows Defender false positives.

Read RELEASE-NOTES.md for test results and hardware limitations.
Read LICENSE, LICENSES.md, ACKNOWLEDGMENTS.md and docs/cn115 for attribution.
Original NitLink: https://github.com/nitlink-dev/nitlink
Audio and Simplified Chinese by CN_115: https://github.com/CN115/nitlink-lowlatency
HolyBear Edition releases/support: https://github.com/HolyBearTW/nitlink
