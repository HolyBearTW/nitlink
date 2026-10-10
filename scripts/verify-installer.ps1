param([Parameter(Mandatory)][string]$Setup, [Parameter(Mandatory)][string]$PackageDirectory)
. "$PSScriptRoot/holy-bear-common.ps1"
. "$PSScriptRoot/get-nsis.ps1"
$info = Get-HolyBearVersion
$setupPath = (Resolve-Path -LiteralPath $Setup).ProviderPath
$version = (Get-Item -LiteralPath $setupPath).VersionInfo
if ($version.FileVersion -ne $info.Version -or $version.ProductName -ne $info.Title -or
    $version.FileDescription -ne "$($info.Title) Setup (x64)") { throw 'Installer product/version mismatch.' }
$fresh = Join-Path (Split-Path $setupPath -Parent) ('verify-setup-' + [guid]::NewGuid().ToString('N'))
& (Get-HolyBear7Zip) x $setupPath "-o$fresh" -y -bso0 -bsp0
if ($LASTEXITCODE -ne 0) { throw 'Cannot extract installer payload for verification.' }
$stage = (Resolve-Path -LiteralPath $PackageDirectory).ProviderPath
foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File) {
    $relative = $file.FullName.Substring($stage.Length + 1)
    $extracted = Join-Path $fresh $relative
    if (-not (Test-Path -LiteralPath $extracted) -or
        (Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath $extracted).Hash) {
        throw "Installer payload differs from validated portable files: $relative"
    }
}
if (@(Get-ChildItem -LiteralPath $fresh -Recurse -File -Filter nitlink.json).Count) { throw 'Installer contains user configuration.' }
Write-Output 'Setup payload matches every validated portable file; no user configuration.'
$global:LASTEXITCODE = 0
