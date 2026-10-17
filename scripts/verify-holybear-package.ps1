param(
    [Parameter(Mandatory)][string]$Zip,
    [Parameter(Mandatory)][string]$ExpectedCommit,
    [Parameter(Mandatory)][string]$ExpectedTag,
    [string]$BuildDirectory,
    [switch]$RequireTag
)
. "$PSScriptRoot/holy-bear-common.ps1"
$info = Get-HolyBearVersion
if ($ExpectedCommit -notmatch '^[0-9a-f]{40}$' -or $ExpectedTag -ne $info.Tag) { throw 'Invalid package source identity.' }
$zipPath = (Resolve-Path -LiteralPath $Zip).ProviderPath
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $archive.Entries) {
        $relative = $entry.FullName.Replace('\', '/')
        if ($relative.StartsWith('/') -or $relative.Contains(':') -or $relative.Split('/') -contains '..' -or
            -not $seen.Add($relative)) { throw 'Unsafe or duplicate ZIP entry.' }
    }
} finally { $archive.Dispose() }
$fresh = Join-Path (Split-Path $zipPath -Parent) ('verify-' + [guid]::NewGuid().ToString('N'))
Expand-Archive -LiteralPath $zipPath -DestinationPath $fresh
$manifest = Join-Path $fresh 'SHA256SUMS.txt'
$checksums = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($line in Get-Content -LiteralPath $manifest) {
    if ($line -notmatch '^([0-9a-f]{64})  (.+)$') { throw 'Invalid package checksum record.' }
    $hash = $Matches[1]; $relative = $Matches[2]
    if ([IO.Path]::IsPathRooted($relative) -or $relative.Contains(':') -or $relative.Split('/') -contains '..' -or
        $checksums.ContainsKey($relative)) { throw 'Unsafe or duplicate checksum path.' }
    $checksums.Add($relative, $hash)
    $actual = (Get-FileHash -LiteralPath (Join-Path $fresh $relative) -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -cne $hash) { throw "Extracted checksum mismatch: $relative" }
}
$files = @(Get-ChildItem -LiteralPath $fresh -Recurse -File)
if ($files.Count -ne $checksums.Count + 1) { throw 'ZIP file set does not match the checksum manifest.' }
# Reuse the strict whitelist/signature validator only AFTER verifying its original manifest.
& "$PSScriptRoot/validate-package.ps1" -Path $fresh -HolyBear
$buildInfo = ConvertFrom-StringData (Get-Content -LiteralPath (Join-Path $fresh 'BUILD.txt') -Raw)
if ($buildInfo.commit -ne $ExpectedCommit -or $buildInfo.tag -ne $ExpectedTag -or
    $buildInfo.version -ne $info.Version -or -not (Test-HolyBearRepositoryName $buildInfo.source_repository)) { throw 'BUILD.txt source mismatch.' }
if ($env:GITHUB_ACTIONS -ceq 'true') {
    Assert-HolyBearRepositoryIdentity
    if ($buildInfo.source_repository -cne $env:GITHUB_REPOSITORY) { throw 'BUILD.txt repository differs from the verified Actions repository.' }
}
if ($RequireTag -and $buildInfo.tag_source_verified -ne 'true') { throw 'Package was not built from the required source tag.' }
$source = Get-Content -LiteralPath (Join-Path $HolyBearRoot 'HOLYBEAR-SOURCES.json') -Raw | ConvertFrom-Json
if ($buildInfo.upstream_commit -ne $source.upstream.commit -or $buildInfo.pr31_commit -ne $source.pr31.commit -or
    $buildInfo.pr23_commit -ne $source.pr23.commit -or $buildInfo.audio_commit -ne $source.audio.commit) { throw 'Integration source mismatch.' }
Assert-HolyBearExecutable (Join-Path $fresh 'NitLink.exe')
foreach ($file in $files) {
    if ($file.Extension -in @('.txt', '.md', '.json', '.tsv', '.html', '.js')) {
        $text = Get-Content -LiteralPath $file.FullName -Raw
        if ($text -match '(?<![a-zA-Z])[a-zA-Z]:[\\/]|/home/runner/|/Users/|gh[pousr]_[A-Za-z0-9]{20,}') {
            throw "Local path or credential-like content in package: $($file.Name)"
        }
    }
}
if ($BuildDirectory) {
    $buildPath = [IO.Path]::GetFullPath((Join-Path $HolyBearRoot $BuildDirectory))
    $matches = @('NitLink.exe', 'nitlink-menu.html', 'assets/menu/menu.js', 'locales/en-US.js',
        'locales/zh-TW.js', 'locales/zh-CN.js', 'locales/de-DE.js', 'assets/menu/Archivo-400.ttf',
        'assets/menu/Archivo-500.ttf', 'assets/menu/Archivo-600.ttf', 'third_party/nis/NIS_Scaler.h')
    foreach ($relative in $matches) {
        if ((Get-FileHash -LiteralPath (Join-Path $buildPath $relative)).Hash -ne
            (Get-FileHash -LiteralPath (Join-Path $fresh $relative)).Hash) { throw "Build-output mismatch: $relative" }
    }
}
Write-Output "Fresh ZIP verification passed for $ExpectedTag / $ExpectedCommit ($($files.Count) files)."
$global:LASTEXITCODE = 0
