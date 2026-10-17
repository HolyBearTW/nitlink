param(
    [string]$BuildDirectory = 'build/holy-bear',
    [string]$OutputDirectory = 'build/holy-bear-dist',
    [string]$Tag,
    [switch]$RequireTag
)
. "$PSScriptRoot/holy-bear-common.ps1"
. "$PSScriptRoot/holy-bear-release-notes.ps1"
$info = Get-HolyBearVersion
$commit = Get-HolyBearCommit
if (-not $Tag) { $Tag = $info.Tag }
if ($Tag -ne $info.Tag) { throw 'Requested package tag does not match VERSION.' }
if ($RequireTag) { Assert-HolyBearSignedTag $Tag $commit }
$status = & git -C $HolyBearRoot status --porcelain=v1
if ($LASTEXITCODE -ne 0 -or $status) { throw 'Package source checkout must be clean and committed.' }
$build = [IO.Path]::GetFullPath((Join-Path $HolyBearRoot $BuildDirectory))
$output = [IO.Path]::GetFullPath((Join-Path $HolyBearRoot $OutputDirectory))
if (Test-Path -LiteralPath $output) { throw 'Output directory already exists; refusing to overwrite a package.' }
$summary = Get-Content -LiteralPath (Join-Path $build 'test-summary.json') -Raw | ConvertFrom-Json
if ($summary.skipped -ne 0 -or $summary.tests -lt 1 -or $summary.failures -notin @(0, 1)) { throw 'Missing validated test summary.' }
Initialize-HolyBearToolchain
New-Item -ItemType Directory -Path $output | Out-Null
$install = Join-Path $output 'install'
& cmake --install $build --config Release --prefix $install
if ($LASTEXITCODE -ne 0) { throw 'Release staging failed.' }
$stage = Join-Path $install 'bin'
Copy-Item -LiteralPath (Join-Path $HolyBearRoot 'README-HOLYBEAR.txt') -Destination (Join-Path $stage 'README.txt')
Copy-Item -LiteralPath (Join-Path $HolyBearRoot 'HOLYBEAR-SOURCES.json') -Destination $stage
Copy-Item -LiteralPath (Join-Path $HolyBearRoot 'docs/4ks-hdr-tonemap.md') -Destination (Join-Path $stage 'docs')
Copy-Item -LiteralPath (Join-Path $HolyBearRoot 'docs/HOLYBEAR-RELEASE-SIGNING.md') -Destination (Join-Path $stage 'docs')
Copy-Item -LiteralPath (Join-Path $HolyBearRoot 'docs/HOLYBEAR-UPDATES.md') -Destination (Join-Path $stage 'docs')
New-Item -ItemType Directory -Path (Join-Path $stage 'licenses') | Out-Null
Copy-Item -LiteralPath (Join-Path $HolyBearRoot 'licenses/MSVC-RUNTIME.txt') -Destination (Join-Path $stage 'licenses')
Copy-Item -LiteralPath (Join-Path $HolyBearRoot 'licenses/NSIS.txt') -Destination (Join-Path $stage 'licenses')
Copy-Item -LiteralPath (Join-Path $HolyBearRoot 'licenses/HOLYBEAR-RELEASE-PUBLIC-KEY.asc') -Destination (Join-Path $stage 'licenses')
$notes = Get-Content -LiteralPath $info.Notes -Raw
$repo = Get-HolyBearRepositoryName
$actionsUrl = if ($env:GITHUB_ACTIONS -eq 'true') { "https://github.com/$repo/actions/runs/$env:GITHUB_RUN_ID" } else { '' }
$notes = Get-HolyBearReleaseNotes -Notes $notes -Title $info.Title -Commit $commit -Tag $Tag -Summary $summary -TagSourceVerified $RequireTag.IsPresent -ActionsUrl $actionsUrl
Set-Content -LiteralPath (Join-Path $stage 'RELEASE-NOTES.md') -Value $notes -Encoding utf8
Copy-Item -LiteralPath (Join-Path $stage 'RELEASE-NOTES.md') -Destination $output
$sources = Get-Content -LiteralPath (Join-Path $HolyBearRoot 'HOLYBEAR-SOURCES.json') -Raw | ConvertFrom-Json
$identity = @(
    "version=$($info.Version)", "variant=$($info.Title)", "commit=$commit", "tag=$Tag",
    "tag_source_verified=$($RequireTag.IsPresent.ToString().ToLowerInvariant())",
    "source_repository=$repo", "upstream_version=$($sources.upstream.version)",
    "upstream_commit=$($sources.upstream.commit)", "pr31_commit=$($sources.pr31.commit)",
    "pr23_commit=$($sources.pr23.commit)", "audio_version=$($sources.audio.tag)",
    "audio_commit=$($sources.audio.commit)", "audio_author=$($sources.audio.author)",
    "built_utc=$([DateTime]::UtcNow.ToString('o'))", "ctest=$($summary.passed)/$($summary.tests)",
    "known_baseline_failures=$($summary.failures)"
)
Set-Content -LiteralPath (Join-Path $stage 'BUILD.txt') -Value $identity -Encoding utf8
$crt = Join-Path $env:VCToolsRedistDir 'x64/Microsoft.VC143.CRT'
if (-not (Test-Path -LiteralPath $crt)) { throw 'Licensed release VC143 CRT directory is missing.' }
# Copy the runtime import closure from the redistributable directory, never System32/debug.
$pending = [Collections.Generic.Queue[string]]::new()
$pending.Enqueue((Join-Path $stage 'NitLink.exe'))
$copied = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
while ($pending.Count) {
    $imports = & dumpbin /DEPENDENTS $pending.Dequeue()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect runtime imports.' }
    foreach ($line in $imports) {
        if ($line -match '^\s+((?:msvcp|vcruntime|concrt|vccorlib)[a-z0-9_]*\.dll)\s*$') {
            $dll = $Matches[1].ToLowerInvariant()
            if ($copied.Add($dll)) {
                Copy-Item -LiteralPath (Join-Path $crt $dll) -Destination $stage
                $pending.Enqueue((Join-Path $stage $dll))
            }
        }
    }
}
& "$PSScriptRoot/validate-package.ps1" -Path $stage -HolyBear
Assert-HolyBearExecutable (Join-Path $stage 'NitLink.exe')
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = Join-Path $output $info.ZipName
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip, [IO.Compression.CompressionLevel]::Optimal, $false)
& "$PSScriptRoot/verify-holybear-package.ps1" -Zip $zip -ExpectedCommit $commit -ExpectedTag $Tag -BuildDirectory $BuildDirectory
& "$PSScriptRoot/build-installer.ps1" -PackageDirectory $stage -OutputDirectory $output
$outerHashes = foreach ($name in @($info.ZipName, $info.SetupName, 'RELEASE-NOTES.md')) {
    '{0}  {1}' -f (Get-FileHash -LiteralPath (Join-Path $output $name) -Algorithm SHA256).Hash.ToLowerInvariant(), $name
}
Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Value $outerHashes -Encoding ascii
Write-Output "Portable ZIP and Setup verified: $($info.ZipName) / $($info.SetupName)"
$global:LASTEXITCODE = 0
