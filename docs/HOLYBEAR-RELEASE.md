# HolyBear Edition releases

This is the independent `holybear/integrated` branch in `HolyBearTW/nitlink`.
It preserves the validated main + #31 + #23 + selected CN_115 v0.1.2 source
snapshot. It does not merge the entire audio fork or duplicate #25's manual
HDMI identity implementation. No upstream PR branch is changed by this flow.

## Preparing a version

Update VERSION, CMake's numeric version, app.rc's four-part numeric/string
versions, src/app/holybear_version.h, the existing HUD version tag, the HTML nitlink-version metadata, README-HOLYBEAR.txt
and `release-notes-<VERSION>.md`. The package script checks their agreement.
Keep original source attribution in HOLYBEAR-SOURCES.json and docs/cn115.

### Release note writing style

Use the maintainer-edited [1.2.4.1 Release](https://github.com/HolyBearTW/nitlink/releases/tag/v1.2.4.1-holybear)
as the writing/style reference for future versions:

- Write primarily in Traditional Chinese, with a short English Summary.
- Open the body with `**非官方社群整合版（Unofficial Community Edition）**`.
- Explain changes in concise, user-facing language, grouped under the existing
  update, compatibility, validation, known limitations and attribution sections.
- Credit people/projects with `@nitlink-dev`, `@CN115` and `@HolyBearTW`, while
  retaining direct links to the actual source repositories, PRs, licenses and notices.
- Describe only changes since the previous release and actual validation results;
  do not present existing features as new or old hardware evidence as a new test.

The source `release-notes-<VERSION>.md` must still begin with the exact
`# NitLink <numeric version> HolyBear Edition` heading required by packaging.
The generator validates that heading, then omits it from the final body so the
GitHub Release title is not repeated. ZIP, Setup and GitHub all receive the same
generated `RELEASE-NOTES.md`; the dynamic validation footer and strict body
comparison remain unchanged. Do not revise published artifacts or overwrite
maintainer-edited release text to apply this style retroactively.

From PowerShell 7 on a Windows x64 development machine with Visual Studio
2022 C++ tools, CMake, Ninja, Node, Python, 7-Zip and WebView2 Runtime:

```powershell
./scripts/build-holybear.ps1
```

Commit the reviewed source and push only `holybear/integrated`. Its Actions
run builds/tests/packages and uploads review artifacts, without publishing a
Release. After reviewing that run, create a new matching tag and push it:

```powershell
. ./scripts/holy-bear-common.ps1
$info = Get-HolyBearVersion
$commit = Get-HolyBearCommit
git tag -s -u 'B96A785DE4C5083A0DFF15EA8925D54ECCF3EADF!' -m $info.Title $info.Tag $commit
if ($LASTEXITCODE -ne 0) { throw 'Tag signing failed; do not push.' }
Assert-HolyBearSignedTag $info.Tag $commit
git -c push.followTags=false push origin "refs/tags/$($info.Tag):refs/tags/$($info.Tag)"
if ($LASTEXITCODE -ne 0) { throw 'Tag push failed; inspect before retrying.' }
```

Tags matching `v*-holybear*` trigger a fresh build from the tag's commit and
automatic **official Latest Release** publication (`prerelease=false`). Future
versions, including four numeric components, need corresponding source version
updates; a mismatched tag or a commit outside the integration branch is rejected.
Formal sources must be annotated OpenPGP-signed tags authorized by
`HOLYBEAR-TAG-SIGNING.json`. `Assert-HolyBearSignedTag` checks the exact VERSION,
direct built Commit, reviewed integration ancestry, cryptographic signature,
approved signing/primary fingerprints, key validity and SHA-2 digest. It reads
the public trust policy/key from the resolved `origin/holybear/integrated` tree
and uses an isolated public-only keyring. The tag checkout and global keyring
cannot supply a different trusted signer.

The formal build job checks this before building; RequireTag packaging, the
protected signing job before Secrets, and the publisher before creating a
draft each repeat the check. A lightweight/unsigned/invalid/unapproved tag,
missing object/tool/public key or unreviewed Commit stops the flow. GitHub's
Verified badge and `gh release create --verify-tag` are not signature gates.

Workflow Dispatch is restricted to `holybear/integrated`: it is build-only by
default, or signs/verifies review artifacts when `signing_validation=true`.
Neither manual mode creates tags, Releases or public Release attachments.
Do not overwrite an existing tag or Release.

Before relying on Workflow Dispatch, confirm that GitHub accepts the existing
workflow on the reviewed integration ref. Registration alone is not proof of
dispatch availability when the workflow is absent from the default branch.
If dispatch is unavailable, record the outstanding protected signing check;
do not modify main, create a validation tag or bypass Environment review.

The commands above are formal publication steps and require explicit release
authorization. A branch preview does not create a tag: its notes identify the
planned version tag and BUILD.txt keeps tag_source_verified=false. Only a
package that passed RequireTag identifies a verified formal source tag.

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

After Windows validation, one protected Windows signing/publication job creates three mandatory
GPG detached signatures over the final ZIP, Setup and outer SHA256SUMS.txt.
See [signing key, Secrets and verification](HOLYBEAR-RELEASE-SIGNING.md). No
private material enters artifacts; missing Secrets/signature/hash failures block
publication. Normal branch previews are unsigned and cannot be published.
For a real Environment signing test without a Release, use the documented
Workflow Dispatch on `holybear/integrated` with `signing_validation=true`.
It produces signed Actions artifacts while the publication step stays skipped.
Validation tags are not supported. Remove duplicate Repository GPG Secrets
before this test so missing Environment Secrets cannot resolve from another scope.

Only the Environment-protected signing/publication stage has contents:write; it uses GITHUB_TOKEN and is
restricted to HolyBearTW/nitlink. It verifies downloaded CI artifacts, refuses
an existing Release, creates a draft with all assets, verifies the uploaded
draft bytes and all signatures, then publishes it as an official Latest Release.
It checks the public downloads and /releases/latest afterward. Failures stop the job. Existing
versions are never clobbered. A rerun encountering an existing draft fails for
review rather than overwriting it. Logs are separate Actions artifacts.
