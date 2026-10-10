# Third-Party Software Used in NitLink

HolyBear Edition additionally incorporates CN_115's MIT-licensed audio and
Simplified Chinese source from `CN115/nitlink-lowlatency` v0.1.2. Original
commit authorship and MIT notice are preserved in `docs/cn115` and `LICENSE`.
Bundled Microsoft VC++ runtime DLLs are proprietary and retain Microsoft's
terms; see `licenses/MSVC-RUNTIME.txt`. They are not relicensed under MIT.

This file lists all third-party software bundled or linked by NitLink,
with their original copyright and license terms preserved.

This file (together with the individual LICENSE files preserved under
the bundled vendor directories) constitutes attribution as required by
the included licenses.

External work that informed NitLink's development but is not bundled,
linked, or redistributed (for example, protocol research references) is
acknowledged separately in [`ACKNOWLEDGMENTS.md`](ACKNOWLEDGMENTS.md).

---

## Archivo

- **Location:** `assets/menu/Archivo-400.ttf`, `Archivo-500.ttf`, `Archivo-600.ttf`
- **Source:** https://fonts.google.com/specimen/Archivo
- **Copyright:** 2020 The Archivo Project Authors (https://github.com/Omnibus-Type/Archivo)
- **License:** SIL Open Font License 1.1, preserved at `assets/menu/Archivo-OFL.txt`
- **Used for:** Settings menu typography. The unmodified Google Fonts assets are bundled for offline loading.

## Ko-fi brand icon

- **Location:** `assets/menu/kofi-cup.png`
- **Source:** https://storage.ko-fi.com/cdn/cup-border.png
- **Used for:** The existing Ko-fi support link. This is a Ko-fi brand asset, not covered by NitLink's MIT license.
- **Brand usage guidance:** https://help.ko-fi.com/hc/en-us/articles/360007169493-Can-I-use-the-Ko-fi-logo-to-promote-my-page

---

## NVIDIA Image Scaling SDK (NIS)

- **Location:** `third_party/nis/`
- **Source:** https://github.com/NVIDIAGameWorks/NVIDIAImageScaling
- **Version:** 1.0.3
- **License:** MIT (full text preserved at `third_party/nis/LICENSE.txt`)
- **Used for:** Real-time spatial upscaling of captured video frames
- **Files included unmodified:**
  - `NIS_Config.h`: host-side coefficient arrays and config struct
  - `NIS_Scaler.h`: the compute shader algorithm (HLSL/GLSL via macros)
  - `NIS_Main.hlsl`: compute shader entry point

NitLink compiles `NIS_Main.hlsl` at runtime via `D3DCompile` and uploads
NVIDIA's coefficient arrays into GPU textures as documented in the SDK.
No modifications are made to NVIDIA's source files; integration glue lives
entirely in `src/upscale/`.

---

## Catmull-Rom Bicubic Sampler (Matt Pettineo)

- **Source:** https://gist.github.com/TheRealMJP/c83b8c0f46b63f3a88a5986f4fa982b1
- **Author:** Matt Pettineo (TheRealMJP)
- **License:** MIT
- **Location:** Inline in `src/renderer/dx11_renderer.cpp` (function
  `sampleCatmullRom`, both BGRA and HDR shader paths)

The 9-tap Catmull-Rom bicubic implementation used in NitLink's resampling
shaders is adapted directly from MJP's public reference. Comments above
the function block in the shader source preserve the link to the original
gist.

Optimizations contributed by Aras Pranckevičius (aras-p) and pixelmager
in the gist comments are also included.

License text:

```
The MIT License (MIT)

Copyright (c) 2016 MJP

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
```

---

## Microsoft Windows Implementation Library (WIL)

- **Location:** `third_party/wil/`
- **Source:** https://github.com/microsoft/wil
- **License:** MIT
- **Used for:** RAII smart-pointer helpers (`wil::com_ptr`,
  `wil::unique_cotaskmem_string`) consumed by the WebView2 host code in
  `src/app/WebViewSettings.cpp`.
- **Distribution:** header-only; included unmodified.

License text:

```
The MIT License (MIT)

Copyright (c) Microsoft Corporation

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```

---

## Microsoft Edge WebView2 SDK

- **Location:** `third_party/webview2/`
- **Source:** https://www.nuget.org/packages/Microsoft.Web.WebView2
  (distributed as a NuGet package; headers vendored under
  `third_party/webview2/include/` and the static loader stub under
  `third_party/webview2/lib/x64/WebView2LoaderStatic.lib`)
- **License:** Microsoft Software License Terms for Microsoft Edge
  WebView2 SDK (proprietary but redistributable for app packaging).
  See https://aka.ms/webview2-sdk-license for the current SDK license.
- **Used for:** Embedded HTML/CSS/JS settings panel rendered as a child
  window of the main HWND (`src/app/WebViewSettings.cpp`).

The runtime Edge WebView2 component itself is not bundled; NitLink
relies on Microsoft's pre-installed WebView2 Runtime (shipped with
Windows 11 and via Evergreen Bootstrapper on Windows 10).

---

## Elgato capture-device-support (GUID + property IDs only)

- **Source:** https://github.com/elgatosf/capture-device-support
- **License:** MIT (the upstream repository)
- **Location:** Used by reference at `src/capture/elgato_hdr_control.cpp`
  / `.h`.

NitLink uses the published Elgato custom-property GUID
(`0xD1E5209F-68FD-4529-BEE0-5E7A1F479226`) and property IDs (720, 721,
722) to disable the capture card's hardware HDR-to-SDR tonemapper and to
read the upstream CEA-861 Dynamic Range InfoFrame. Only the constants
are referenced; no Elgato source code is compiled into NitLink.

Constants (numeric GUID values and integer property IDs) are not
copyrightable, but Elgato's MIT license applies to the upstream
repository from which the values were taken.

---

## Discord IPC client

- **No third-party code bundled.** NitLink contains a from-scratch
  Win32-pipe Discord Rich Presence client in `src/discord/discord_rpc.cpp`
  written against Discord's public RPC protocol spec
  (https://discord.com/developers/docs/topics/rpc).
- The Discord RPC SDK and Discord Game SDK are **not** included.

---

## Windows platform APIs (system, no attribution required)

For completeness, NitLink links against the following Windows system
APIs, all provided by Windows itself and not subject to third-party
attribution requirements:

- DirectX 11 / DXGI / Direct3D 11 (`d3d11`, `dxgi`, `d3dcompiler`,
  `dxguid`)
- Direct2D / DirectWrite (`d2d1`, `dwrite`)
- Media Foundation (`mf`, `mfplat`, `mfreadwrite`, `mfuuid`)
- DirectShow / Kernel Streaming (`strmiids`): for the IKsPropertySet
  path used by `elgato_hdr_control.cpp`
- HID Class API + Setup API (`hid`, `setupapi`): for the vendor HID
  Output Report path used by `elgato_hid_4ks.cpp` (4K S tonemap toggle)
- WASAPI / Core Audio (`avrt`)
- WinHTTP (`winhttp`) and Windows Runtime JSON (`windowsapp`): optional HolyBear update checks
- DWM (`dwmapi`), Windows Imaging Component (`windowscodecs`),
  shell helpers (`shlwapi`, `shcore`), COM (`ole32`, `oleaut32`,
  `propsys`, `uuid`), and `winmm`.

---

## Microsoft Visual C++ runtime

- **Location:** `msvcp140.dll`, `vcruntime140.dll` and `vcruntime140_1.dll`
  beside the executable in the release package.
- **Source:** The x64 redistributable runtime directory supplied with the
  matching Microsoft Visual C++ toolset.
- **Copyright:** Microsoft Corporation.
- **License:** Microsoft Visual Studio software license terms. These runtime
  binaries are not covered by NitLink's MIT license.
- **Used for:** C++ standard-library and runtime support. Windows supplies
  the Universal C Runtime; the Edge WebView2 runtime is still separate.

## NSIS installer

- **Source:** https://nsis.sourceforge.io/ and the pinned NSIS 3.12 compiler.
- **Copyright:** NSIS Contributors; preserved upstream notices in `licenses/NSIS.txt`.
- **License:** NSIS components' zlib, bzip2 and Common Public License terms with
  the stated LZMA exception. The generated installer contains NSIS runtime code;
  the compiler is not distributed in the application package.
- **Used for:** HolyBear Edition's branded multilingual Windows installer.
