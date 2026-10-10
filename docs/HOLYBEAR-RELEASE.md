# HolyBear Edition releases

This is the independent `holy-bear/integrated` branch in `HolyBearTW/nitlink`.
It preserves the validated main + #31 + #23 + selected CN_115 v0.1.2 source
snapshot. It does not merge the entire audio fork or duplicate #25's manual
HDMI identity implementation. No upstream PR branch is changed by this flow.

## Preparing a version

Update VERSION, CMake's numeric version, app.rc's four-part numeric/string
versions, src/app/holybear_version.h, the existing HUD version tag, About build text, README-HOLYBEAR.txt
and `release-notes-<VERSION>.md`. The package script checks their agreement.
Keep original source attribution in HOLYBEAR-SOURCES.json and docs/cn115.

From PowerShell 7 on a Windows x64 development machine with Visual Studio
2022 C++ tools, CMake, Ninja, Node, Python, 7-Zip and WebView2 Runtime:

```powershell
./scripts/build-holybear.ps1
```

Commit the reviewed source and push only `holy-bear/integrated`. Its Actions
run builds/tests/packages and uploads review artifacts, without publishing a
Release. After reviewing that run, create a new matching tag and push it:

```text
git tag -s v1.2.4-holybear -m "NitLink 1.2.4 HolyBear Edition"
git verify-tag v1.2.4-holybear
git push holy-bear v1.2.4-holybear
```

Tags matching `v*-holybear*` trigger a fresh build from the tag's commit and
automatic **official Latest Release** publication (`prerelease=false`). Future
versions, including four numeric components, need corresponding source version
updates; a mismatched tag or a commit outside the integration branch is rejected.
workflow_dispatch is build-only. Do not overwrite an existing tag or Release.

## Publication checks

The complete CTest suite includes WebView runtime tests. Every discovered test
must run. The only permitted nonzero result is exactly one failed target named
webview_runtime_security with its sole `FAIL` line equal to the documented
restart assertion. Timeout, skipped/missing tests, additional failure lines
or any other failing target stop publication. Actual counts are appended to
the release notes; the assertion is never changed or skipped by the workflow.

The workflow stages a fresh CMake install and an explicit allowed file set,
adds unmodified Microsoft-signed redistributable CRT DLLs, validates EXE
version/architecture, writes BUILD.txt/file checksums, and verifies a newly
extracted ZIP against both the build output and source/tag. A pinned portable
NSIS 3.12 compiler builds a branded three-language installer from those exact
files. 7-Zip extraction verifies every installed payload byte against the ZIP.
CI tests silent install, upgrade and uninstall in all three languages, including
the normal per-user application directory, Start menu shortcut, ARP identity
and retained user settings. NitLink is never launched by the installer tests.
No private settings, logs, PDBs or test launchers are included.

The installer uses a separate per-user product identity and defaults to
`%LOCALAPPDATA%\Programs\NitLink HolyBear Edition`. Windows UI language defaults
zh-TW/HK/MO to Traditional Chinese, zh-CN/SG to Simplified Chinese and others
to English; the user can override it in the language dialog. Framework pages,
buttons, component descriptions, error messages and uninstall flow are localized.
Custom icon/sidebar/banner sources are in assets/installer; the wizard retains
standard Windows controls with a dark NitLink sidebar/header. Desktop shortcut
and launch-after-install are optional and initially unchecked. Upgrades replace
only packaged files; uninstall retains settings/screenshots/VRR_CAP and other
user-created files. No original installation is migrated or modified.

The opt-in native update checker accepts only this fork's formal Releases.
See [network behavior and tests](HOLYBEAR-UPDATES.md). Background checks default
off. No updater downloads or runs an installer.

After Windows validation, an isolated Linux signing job creates three mandatory
GPG detached signatures over the final ZIP, Setup and outer SHA256SUMS.txt.
See [signing key, Secrets and verification](HOLYBEAR-RELEASE-SIGNING.md). No
private material enters artifacts; missing Secrets/signature/hash failures block
publication. Branch previews are explicitly unsigned and cannot be published.

Only the tag-push publish job has contents:write; it uses GITHUB_TOKEN and is
restricted to HolyBearTW/nitlink. It verifies downloaded CI artifacts, refuses
an existing Release, creates a draft with all assets, verifies the uploaded
draft bytes and all signatures, then publishes it as an official Latest Release.
It checks the public downloads and /releases/latest afterward. Failures stop the job. Existing
versions are never clobbered. A rerun encountering an existing draft fails for
review rather than overwriting it. Logs are separate Actions artifacts.
