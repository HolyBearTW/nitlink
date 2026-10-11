Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$HolyBearRoot = Split-Path $PSScriptRoot -Parent

function Get-HolyBearVersion {
    $value = (Get-Content -LiteralPath (Join-Path $HolyBearRoot 'VERSION') -Raw).Trim()
    if ($value -notmatch '^((?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:\.(?:0|[1-9][0-9]*))?)-holybear$') {
        throw 'VERSION must contain a numeric version followed by -holybear.'
    }
    $numeric = $Matches[1]
    $cmake = Get-Content -LiteralPath (Join-Path $HolyBearRoot 'CMakeLists.txt') -Raw
    $resource = Get-Content -LiteralPath (Join-Path $HolyBearRoot 'src/app.rc') -Raw
    $numbers = $numeric.Split('.')
    if (@($numbers | Where-Object { [int]$_ -gt 65535 }).Count) { throw 'Invalid Windows version.' }
    $windowsParts = @($numbers)
    if ($windowsParts.Count -eq 3) { $windowsParts += '0' }
    $fourPart = $windowsParts -join ','
    $title = "NitLink $numeric HolyBear Edition"
    if (-not $cmake.Contains("project(NitLink VERSION $numeric LANGUAGES CXX)")) { throw 'CMake version mismatch.' }
    foreach ($field in @('FILEVERSION', 'PRODUCTVERSION')) {
        if (-not $resource.Contains("$field $fourPart")) { throw "Resource $field mismatch." }
    }
    foreach ($field in @('FileVersion', 'ProductVersion')) {
        if (-not $resource.Contains(('VALUE "{0}", "{1}"' -f $field, $value))) { throw "Resource $field mismatch." }
    }
    $menu = Get-Content -LiteralPath (Join-Path $HolyBearRoot 'nitlink-menu.html') -Raw
    $menuScript = Get-Content -LiteralPath (Join-Path $HolyBearRoot 'assets/menu/menu.js') -Raw
    $overlay = Get-Content -LiteralPath (Join-Path $HolyBearRoot 'src/overlay/overlay.cpp') -Raw
    $native = Get-Content -LiteralPath (Join-Path $HolyBearRoot 'src/app/holybear_version.h') -Raw
    if (-not $menu.Contains('id="holy-bear-product-title"') -or
        -not $menu.Contains('id="holy-bear-last-check"') -or
        -not $menuScript.Contains("const currentTag = typeof s.current") -or
        -not $overlay.Contains(('L"{0}"' -f $value)) -or
        -not $native.Contains(('L"{0}"' -f $value))) { throw 'About/HUD/native update version mismatch.' }
    if (-not $resource.Contains(('VALUE "ProductName", "{0}"' -f $title)) -or
        (Get-Content -LiteralPath (Join-Path $HolyBearRoot 'README-HOLYBEAR.txt') -First 1) -cne "$title ($value)") {
        throw 'Product title/README version mismatch.'
    }
    [pscustomobject]@{
        Version = $value; Numeric = $numeric; Tag = "v$value"
        WindowsVersion = $windowsParts -join '.'
        Title = $title
        ZipName = "NitLink-$numeric-HolyBear-Win64.zip"
        SetupName = "NitLink-$numeric-HolyBear-Setup-x64.exe"
        Notes = Join-Path $HolyBearRoot "release-notes-$value.md"
    }
}

function Get-HolyBearCommit {
    $sha = (& git -C $HolyBearRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $sha -notmatch '^[0-9a-f]{40}$') { throw 'Cannot resolve source commit.' }
    return $sha
}

function Assert-HolyBearTag([string]$Tag, [string]$Commit) {
    $info = Get-HolyBearVersion
    if ($Tag -ne $info.Tag) { throw 'Tag does not match VERSION.' }
    $resolved = & git -C $HolyBearRoot rev-parse --verify "refs/tags/$Tag^{commit}" 2>$null
    if ($LASTEXITCODE -ne 0 -or $resolved -ne $Commit) { throw 'Tag does not resolve to the built source commit.' }
}

function Initialize-HolyBearToolchain {
    # Process environment only; do not change the machine's PATH or settings.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $installation = & $vswhere -latest -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'Visual Studio 2022 C++ tools are required.' }
    $init = Join-Path $installation 'Common7/Tools/VsDevCmd.bat'
    $env:VSLANG = '1033'
    $command = '"{0}" -arch=x64 -host_arch=x64 >nul && set' -f $init
    $lines = & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw 'VsDevCmd failed.' }
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
    }
    $env:VSLANG = '1033'
}

function Assert-HolyBearExecutable([string]$Path) {
    $info = Get-HolyBearVersion
    $file = Get-Item -LiteralPath $Path
    if ($file.VersionInfo.FileVersion -ne $info.Version -or
        $file.VersionInfo.ProductVersion -ne $info.Version -or
        $file.VersionInfo.ProductName -ne $info.Title -or
        $file.VersionInfo.FileDescription -ne 'NitLink (HolyBear Edition)' -or
        $file.VersionInfo.OriginalFilename -ne 'NitLink.exe' -or
        $file.VersionInfo.LegalCopyright -ne 'Copyright (c) 2026 NitLink Contributors') {
        throw 'EXE version, description or original copyright mismatch.'
    }
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) { throw 'Invalid executable.' }
    $pe = [BitConverter]::ToInt32($bytes, 0x3c)
    if ($pe -lt 64 -or $pe -gt $bytes.Length - 6 -or
        [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550 -or
        [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) { throw 'Executable must be Windows AMD64 PE.' }
}
