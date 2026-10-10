# HolyBear release signatures

Official GitHub Releases of this **Unofficial Community Edition** are published
only in [HolyBearTW/nitlink](https://github.com/HolyBearTW/nitlink/releases).
`prerelease=false` is GitHub release metadata; the `-holybear` suffix identifies
the edition, rather than an opt-in to GitHub pre-release updates.

## Trusted public key

The maintainer approved this existing signing subkey for HolyBear releases:

```text
Signing subkey: B96A785DE4C5083A0DFF15EA8925D54ECCF3EADF
Primary key:    5DA247A5EFA2AACB4EB1A3A2D278ED91B6F2C0BA
```

Obtain the public key from
[`licenses/HOLYBEAR-RELEASE-PUBLIC-KEY.asc`](../licenses/HOLYBEAR-RELEASE-PUBLIC-KEY.asc).
Cross-check the full fingerprints through the maintainer's previously trusted
[GitHub profile public keys](https://github.com/HolyBearTW.gpg), separately from
the Release's attachments. That profile currently includes this primary key
and subkey. A copy bundled beside a download is not, by itself, an independent
proof of publisher identity. If you do not already trust this identity, confirm
the fingerprint with the maintainer through a separate trusted channel first.
Do not rely on an 8- or 16-character key ID.

Install GnuPG, then use PowerShell to inspect and import the public key:

```powershell
gpg --show-keys --with-subkey-fingerprint ./HOLYBEAR-RELEASE-PUBLIC-KEY.asc
gpg --import ./HOLYBEAR-RELEASE-PUBLIC-KEY.asc
```

## Verify downloaded artifacts

Download ZIP, Setup.exe, SHA256SUMS.txt and each matching `.asc` from the same
Release. Each `.asc` is an ASCII-armored **detached OpenPGP signature** over the
exact final file. It is not a renamed checksum or an embedded Windows signature.

```powershell
gpg --verify `
  NitLink-1.2.4-HolyBear-Setup-x64.exe.asc `
  NitLink-1.2.4-HolyBear-Setup-x64.exe
if ($LASTEXITCODE -ne 0) { throw 'Setup signature failed' }

gpg --verify `
  NitLink-1.2.4-HolyBear-Win64.zip.asc `
  NitLink-1.2.4-HolyBear-Win64.zip
if ($LASTEXITCODE -ne 0) { throw 'ZIP signature failed' }

gpg --verify SHA256SUMS.txt.asc SHA256SUMS.txt
if ($LASTEXITCODE -ne 0) { throw 'Checksum signature failed' }

Get-FileHash NitLink-1.2.4-HolyBear-Setup-x64.exe -Algorithm SHA256
Get-FileHash NitLink-1.2.4-HolyBear-Win64.zip -Algorithm SHA256
```

Compare the hashes against the **verified** SHA256SUMS.txt. Its records cover
the ZIP, Setup and release notes. GnuPG may report a valid signature but warn
that the key's owner is not trusted; matching a trusted full fingerprint is
still necessary. For maintainers, the repository provides an automated public
key verifier that also checks every release checksum:

```powershell
python ./scripts/release-signing.py verify --assets ./downloads --tag v1.2.4-holybear
```

The inner ZIP/installed file manifest checks individual payload files. It does
not replace authentication of the downloaded ZIP and installer.

## Maintainer secrets and failure policy

Set these Actions Secrets in HolyBearTW/nitlink's **`release` Environment**:

| Secret | Required value |
| --- | --- |
| `HOLYBEAR_GPG_PRIVATE_KEY` | ASCII-armored export of only the approved secret subkey, with a dummy primary secret key |
| `HOLYBEAR_GPG_PASSPHRASE` | The subkey's passphrase; enter through GitHub Secrets, never chat or a command argument |
| `HOLYBEAR_GPG_FINGERPRINT` | `B96A785DE4C5083A0DFF15EA8925D54ECCF3EADF` |

No master secret key or other secret subkey is accepted. The script checks the
fingerprints, signing capability, revocation/expiry and selected-secret-only
export, and uses `--local-user <fingerprint>!` to prevent GPG choosing another
key. The passphrase uses batch loopback mode and stdin (`--passphrase-fd 0`).
No secrets are included in command arguments, diagnostics or build artifacts.

Windows builds/tests/verifies the final ZIP and Setup. A protected Windows job downloads
those same bytes, verifies SHA-256, imports the secret into an isolated temporary
GNUPGHOME, signs ZIP, Setup and SHA256SUMS, and immediately verifies all three
in a second temporary keyring containing **only the pinned public key**. It
kills the temporary GPG agent and removes key material even when signing fails.
Missing secrets, a bad signature, or a hash mismatch stops publication.

Only trusted HolyBear version/validation tags in this repository, or an explicit
manual validation request on the integration branch, invoke signing. Source
must belong to `holy-bear/integrated`. Normal branch/manual runs build previews
without secrets. External pull requests never receive signing secrets.
The `sign` job declares `environment: release` and contains signing, verification
and the optional publication step. Combining them means only one protected job
needs approval. Any required reviewer must approve the pending deployment before
it runs; the workflow never bypasses or changes Environment protection rules.
Only that protected stage can write Releases; it checks signatures again, creates
a new draft, downloads and verifies uploaded bytes, then publishes an official
Latest Release and verifies the downloadable attachments. Existing Tags,
Releases and attachments are never overwritten automatically.

## Manual Environment signing check without a Release

The current `release` Environment permits `v*-holybear*` **tags**, not arbitrary
branches. Use a new, signed **validation tag** from the reviewed integration
commit, such as `v1.2.4-holybear-signing-check-20261011010000`. The final digits
must be unique. Do not create the formal version tag as a test and do not
overwrite any existing tag.

```powershell
git switch holy-bear/integrated
$checkTag = 'v1.2.4-holybear-signing-check-' + [DateTime]::UtcNow.ToString('yyyyMMddHHmmss')
git tag -s -u 'B96A785DE4C5083A0DFF15EA8925D54ECCF3EADF!' $checkTag -m 'HolyBear Environment signing validation only; no Release'
git verify-tag $checkTag
git push holy-bear $checkTag
```

This is a deliberate manual validation invocation that respects the existing
tag-only Environment policy. It builds/tests/packages, then actually unlocks
the approved subkey, signs the three files and verifies them with only the public
key. It uploads `holy-bear-signed` plus `SIGNING-VERIFICATION.txt`. The publication
step is **skipped**, and the script rejects this tag as a formal release source.
`v1.2.4-holybear` remains untouched. The signed files are validation artifacts,
not an official release; a later formal tag produces fresh files and signatures.

An explicit `workflow_dispatch` input `signing_validation=true` provides the
same no-publication mode. GitHub's dispatch/UI availability and Environment
branch/tag permissions still apply; the workflow does not add the integration
branch to allowed deployment sources or modify the default branch. The separate
validation-tag path works while the workflow exists only on this feature branch.

Environment-level same-name Secrets take precedence over Repository Secrets.
GitHub does not expose a separate Environment-only secret-expression namespace:
if an Environment secret is removed while the duplicate Repository name remains,
the platform can resolve that Repository value. No fallback is coded here.
Before validation, independently confirm all three names exist in `release`:

```powershell
gh secret list --repo HolyBearTW/nitlink --env release
```

After successful **actual** Environment signing, the maintainer should remove
the three Repository-level duplicates before the formal release. This eliminates
GitHub's implicit same-name fallback; do not treat `environment: release` alone
as proof that duplicate names cannot resolve from another scope. No private key,
passphrase, or sensitive input is included in the verification report.

## GPG, Authenticode and Git tags

GPG authenticates artifact bytes to holders of a trusted public key. Windows
does not treat these detached signatures as Authenticode publisher certificates.
The current executable/installer has no Authenticode publisher signature. GPG
does **not** remove SmartScreen warnings or prevent Defender false positives.
No self-signed Windows certificate is presented as a trusted publisher.

If trusted Authenticode signing is added later, finish it **before** computing
release hashes and GPG signatures. Any later ZIP/EXE change invalidates the GPG
signature. Artifact signatures also do not make a Git tag “Verified”: that
requires a separately signed Git tag and actual verification of that tag.
BUILD.txt's `tag_source_verified` records source/tag agreement only.
