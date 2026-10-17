param([Parameter(Mandatory)][string]$Assets)
. "$PSScriptRoot/holy-bear-common.ps1"
Assert-HolyBearRepositoryIdentity
if (-not $env:RUNNER_TEMP) {
    throw 'Installer execution tests are restricted to the disposable HolyBear Actions runner.'
}
$info = Get-HolyBearVersion
$assetsRoot = (Resolve-Path -LiteralPath $Assets).ProviderPath
$install = Join-Path $env:LOCALAPPDATA 'Programs/NitLink HolyBear Edition'
$regPath = 'Software\HolyBearTW\NitLink HolyBear Edition'
$arpPath = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\NitLink-HolyBear-Edition'
$registry = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, [Microsoft.Win32.RegistryView]::Registry64)
if ((Test-Path -LiteralPath $install) -or $registry.OpenSubKey($regPath) -or $registry.OpenSubKey($arpPath)) {
    throw 'Existing HolyBear installation detected; refusing to touch it.'
}
$payload = Join-Path $env:RUNNER_TEMP ('holy-bear-installer-test-' + [guid]::NewGuid().ToString('N'))
Expand-Archive -LiteralPath (Join-Path $assetsRoot $info.ZipName) -DestinationPath $payload
$settings = Join-Path $install 'nitlink.json'
$sentinelHash = $null
foreach ($language in @(1033, 1028, 2052)) {
    $process = Start-Process -FilePath (Join-Path $assetsRoot $info.SetupName) -ArgumentList '/S',"/LANG=$language" -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Installer failed: $($process.ExitCode)" }
    foreach ($file in Get-ChildItem -LiteralPath $payload -Recurse -File) {
        $relative = $file.FullName.Substring($payload.Length + 1)
        $target = Join-Path $install $relative
        if (-not (Test-Path -LiteralPath $target) -or (Get-FileHash -LiteralPath $file.FullName).Hash -ne (Get-FileHash -LiteralPath $target).Hash) {
            throw "Installed payload mismatch: $relative"
        }
    }
    $product = $registry.OpenSubKey($regPath)
    $arp = $registry.OpenSubKey($arpPath)
    if (-not $product -or -not $arp -or $product.GetValue('InstallDir') -ne $install -or
        $product.GetValue('Language') -ne "$language" -or $arp.GetValue('DisplayName') -ne $info.Title -or
        $arp.GetValue('DisplayVersion') -ne $info.Version) { throw 'Installed registry metadata is incorrect.' }
    $product.Dispose(); $arp.Dispose()
    $shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'NitLink HolyBear Edition/NitLink HolyBear Edition.lnk'
    if (-not (Test-Path -LiteralPath $shortcut)) { throw 'Start menu shortcut missing.' }
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut($shortcut)
    if ($link.TargetPath -ne (Join-Path $install 'NitLink.exe') -or $link.WorkingDirectory -ne $install) { throw 'Shortcut path/working directory is incorrect.' }
    if (Test-Path -LiteralPath (Join-Path ([Environment]::GetFolderPath('Desktop')) 'NitLink HolyBear Edition.lnk')) { throw 'Optional desktop shortcut must start unchecked.' }
    if ($sentinelHash) {
        if ((Get-FileHash -LiteralPath $settings).Hash -ne $sentinelHash) { throw 'Upgrade modified user settings.' }
    } else {
        if (Test-Path -LiteralPath $settings) { throw 'Installer bundled or generated personal settings.' }
        Set-Content -LiteralPath $settings -Value '{"private_test_sentinel":true}' -Encoding utf8
        $sentinelHash = (Get-FileHash -LiteralPath $settings).Hash
    }
    Write-Output "Installer/upgrade verified: language $language; user settings retained."
}
$uninstaller = Join-Path $install 'Uninstall.exe'
# NSIS _?= runs in place so -Wait observes the uninstaller, rather than its copy.
$process = Start-Process -FilePath $uninstaller -ArgumentList '/S',"_?=$install" -WindowStyle Hidden -Wait -PassThru
if ($process.ExitCode -ne 0) { throw 'Uninstaller failed.' }
foreach ($file in Get-ChildItem -LiteralPath $payload -Recurse -File) {
    if (Test-Path -LiteralPath (Join-Path $install $file.FullName.Substring($payload.Length + 1))) { throw 'Uninstaller left a release payload file.' }
}
if ($registry.OpenSubKey($arpPath) -or $registry.OpenSubKey($regPath) -or (Test-Path -LiteralPath $shortcut) -or
    -not (Test-Path -LiteralPath $settings) -or (Get-FileHash -LiteralPath $settings).Hash -ne $sentinelHash) {
    throw 'Uninstall metadata or retained settings are incorrect.'
}
$registry.Dispose()
Write-Output 'CI install/upgrade/uninstall passed for all three languages; NitLink was never launched.'
$global:LASTEXITCODE = 0
