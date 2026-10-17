param([Parameter(Mandatory)][string]$Tag, [Parameter(Mandatory)][string]$Assets)
. "$PSScriptRoot/holy-bear-common.ps1"
. "$PSScriptRoot/holy-bear-release-notes.ps1"
Assert-HolyBearRepositoryIdentity
if ($env:GITHUB_EVENT_NAME -cne 'push' -or $env:GITHUB_REF -cne "refs/tags/$Tag") {
    throw 'Publication is restricted to a matching tag-push Actions run.'
}
$repo = Get-HolyBearRepositoryName
$info = Get-HolyBearVersion
$commit = Get-HolyBearCommit
Assert-HolyBearSignedTag $Tag $commit
& git -C $HolyBearRoot merge-base --is-ancestor HEAD origin/holybear/integrated
if ($LASTEXITCODE -ne 0) { throw 'Release tag is not part of the reviewed integration branch.' }
$assetRoot = (Resolve-Path -LiteralPath (Join-Path $HolyBearRoot $Assets)).ProviderPath
$signingReport = Get-Content -LiteralPath (Join-Path $assetRoot 'SIGNING-VERIFICATION.txt')
if ($signingReport -cnotcontains 'validation_only=false' -or
    $signingReport -cnotcontains "source_ref=refs/tags/$Tag" -or
    $signingReport -cnotcontains 'environment=release') { throw 'Signing-validation artifacts cannot be published.' }
if (-not (Get-Command gpg -ErrorAction SilentlyContinue)) {
    $gitGpg = Join-Path $env:ProgramFiles 'Git/usr/bin/gpg.exe'
    if (-not (Test-Path -LiteralPath $gitGpg)) { throw 'GPG verification tools are missing.' }
    $env:PATH = (Split-Path $gitGpg -Parent) + ';' + $env:PATH
}
function Test-SignedRelease([string]$Folder) {
    & python "$PSScriptRoot/release-signing.py" verify --assets $Folder --tag $Tag
    if ($LASTEXITCODE -ne 0) { throw 'Signatures or SHA-256 failed; release rejected.' }
}
Test-SignedRelease $assetRoot
& "$PSScriptRoot/verify-holybear-package.ps1" -Zip (Join-Path $assetRoot $info.ZipName) -ExpectedCommit $commit -ExpectedTag $Tag -RequireTag
$portable = Join-Path $assetRoot ('publish-payload-' + [guid]::NewGuid().ToString('N'))
Expand-Archive -LiteralPath (Join-Path $assetRoot $info.ZipName) -DestinationPath $portable
& "$PSScriptRoot/verify-installer.ps1" -Setup (Join-Path $assetRoot $info.SetupName) -PackageDirectory $portable
# Listing errors are fatal; absence is never inferred from an authentication failure.
$existingJson = & gh release list --repo $repo --limit 1000 --json tagName
if ($LASTEXITCODE -ne 0) { throw 'Cannot check existing releases.' }
if (@(($existingJson -join "`n" | ConvertFrom-Json) | Where-Object tagName -eq $Tag).Count) {
    throw 'Release already exists; refusing to overwrite it.'
}
$expectedAssets = @($info.ZipName, "$($info.ZipName).asc", $info.SetupName, "$($info.SetupName).asc", 'SHA256SUMS.txt', 'SHA256SUMS.txt.asc', 'RELEASE-NOTES.md')
$payload = $expectedAssets | ForEach-Object { Join-Path $assetRoot $_ }
& gh release create $Tag @payload --repo $repo --verify-tag --draft --prerelease=false --title $info.Title --notes-file (Join-Path $assetRoot 'RELEASE-NOTES.md')
if ($LASTEXITCODE -ne 0) { throw 'Draft creation/upload failed; nothing will be published.' }
$draftJson = & gh release view $Tag --repo $repo --json isDraft,isPrerelease,assets,body
if ($LASTEXITCODE -ne 0) { throw 'Cannot verify created draft.' }
$draft = $draftJson -join "`n" | ConvertFrom-Json
$expectedNotes = Get-Content -LiteralPath (Join-Path $assetRoot 'RELEASE-NOTES.md') -Raw -Encoding utf8
Assert-HolyBearReleaseNotesMatch -Expected $expectedNotes -Actual $draft.body
$names = @($draft.assets | ForEach-Object name)
if (-not $draft.isDraft -or $draft.isPrerelease -or $names.Count -ne $expectedAssets.Count -or
    @($names | Where-Object { $_ -notin $expectedAssets }).Count -or
    @($draft.assets | Where-Object { $_.size -le 0 }).Count) { throw 'Draft has incomplete/invalid assets.' }
$download = Join-Path $env:RUNNER_TEMP ('holy-bear-upload-check-' + [guid]::NewGuid().ToString('N'))
& gh release download $Tag --repo $repo --dir $download
if ($LASTEXITCODE -ne 0) { throw 'Cannot verify uploaded draft bytes.' }
foreach ($name in $expectedAssets) {
    if ((Get-FileHash -LiteralPath (Join-Path $download $name)).Hash -ne
        (Get-FileHash -LiteralPath (Join-Path $assetRoot $name)).Hash) { throw "Uploaded draft mismatch: $name" }
}
Test-SignedRelease $download
& gh release edit $Tag --repo $repo --draft=false --prerelease=false --latest=true
if ($LASTEXITCODE -ne 0) { throw 'Verified draft publication failed.' }
$publishedJson = & gh release view $Tag --repo $repo --json isDraft,isPrerelease,tagName,url,assets,body
if ($LASTEXITCODE -ne 0) { throw 'Cannot verify published release.' }
$published = $publishedJson -join "`n" | ConvertFrom-Json
Assert-HolyBearReleaseNotesMatch -Expected $expectedNotes -Actual $published.body
if ($published.isDraft -or $published.isPrerelease -or $published.tagName -ne $Tag -or
    @($published.assets).Count -ne $expectedAssets.Count) { throw 'Published release state is invalid.' }
$latest = & gh api "repos/$repo/releases/latest" --jq .tag_name
if ($LASTEXITCODE -ne 0 -or $latest -ne $Tag) { throw 'Published Release is not Latest.' }
$publicDownload = Join-Path $env:RUNNER_TEMP ('holy-bear-public-check-' + [guid]::NewGuid().ToString('N'))
& gh release download $Tag --repo $repo --dir $publicDownload
if ($LASTEXITCODE -ne 0) { throw 'Public release attachments cannot be downloaded.' }
foreach ($name in $expectedAssets) {
    if ((Get-FileHash -LiteralPath (Join-Path $publicDownload $name)).Hash -ne
        (Get-FileHash -LiteralPath (Join-Path $assetRoot $name)).Hash) { throw "Public download mismatch: $name" }
}
Test-SignedRelease $publicDownload
Write-Output "Published verified official HolyBear Release (Latest): $($published.url)"
$global:LASTEXITCODE = 0
