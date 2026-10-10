param([Parameter(Mandatory)][string]$PackageDirectory, [Parameter(Mandatory)][string]$OutputDirectory)
. "$PSScriptRoot/holy-bear-common.ps1"
. "$PSScriptRoot/get-nsis.ps1"
$info = Get-HolyBearVersion
$stage = (Resolve-Path -LiteralPath $PackageDirectory).ProviderPath
$output = (Resolve-Path -LiteralPath $OutputDirectory).ProviderPath
& "$PSScriptRoot/validate-package.ps1" -Path $stage -HolyBear
$compiler = Get-HolyBearNSIS
$generated = Join-Path $output 'installer-source'
New-Item -ItemType Directory -Path $generated | Out-Null
$licenseText = Get-Content -LiteralPath (Join-Path $stage 'LICENSE') -Raw -Encoding utf8
$rtf = [Text.StringBuilder]::new()
[void]$rtf.Append('{\rtf1\ansi\ansicpg1252\deff0{\fonttbl{\f0\fnil Segoe UI;}}\viewkind4\uc1\pard\f0\fs20\sl260\slmult1\sa80 ')
foreach ($character in $licenseText.ToCharArray()) {
    $code = [int]$character
    switch ($character) {
        '\' { [void]$rtf.Append('\\') }
        '{'  { [void]$rtf.Append('\{') }
        '}'  { [void]$rtf.Append('\}') }
        "`r" { }
        "`n" { [void]$rtf.Append('\line ') }
        default {
            if ($code -lt 32) { [void]$rtf.Append('?') }
            elseif ($code -gt 127) {
                $signedCode = if ($code -gt 32767) { $code - 65536 } else { $code }
                [void]$rtf.Append("\u$signedCode?")
            } else { [void]$rtf.Append($character) }
        }
    }
}
[void]$rtf.Append('}')
$licenseRtf = Join-Path $generated 'LICENSE.rtf'
[IO.File]::WriteAllText($licenseRtf, $rtf.ToString(), [Text.UTF8Encoding]::new($false))
$installLines = @(); $uninstallLines = @(); $folders = [Collections.Generic.HashSet[string]]::new()
$size = 0L
foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName) {
    $relative = $file.FullName.Substring($stage.Length + 1)
    if ($relative.Contains('$') -or $relative.Contains('"')) { throw 'Unsafe installer filename.' }
    $parent = [IO.Path]::GetDirectoryName($relative)
    $installLines += 'SetOutPath "$INSTDIR' + $(if($parent){'\' + $parent}else{''}) + '"'
    $installLines += 'File "' + $file.FullName + '"'
    $uninstallLines += 'Delete "$INSTDIR\' + $relative + '"'
    while ($parent) { [void]$folders.Add($parent); $parent = [IO.Path]::GetDirectoryName($parent) }
    $size += $file.Length
}
foreach ($folder in $folders | Sort-Object Length -Descending) { $uninstallLines += 'RMDir "$INSTDIR\' + $folder + '"' }
$installInclude = Join-Path $generated 'install-files.nsh'
$uninstallInclude = Join-Path $generated 'uninstall-files.nsh'
Set-Content -LiteralPath $installInclude -Value $installLines -Encoding utf8
Set-Content -LiteralPath $uninstallInclude -Value $uninstallLines -Encoding utf8
$setup = Join-Path $output $info.SetupName
if (Test-Path -LiteralPath $setup) { throw 'Installer already exists; refusing to overwrite.' }
& $compiler /V3 /WX /INPUTCHARSET UTF8 "/DVERSION=$($info.Version)" "/DNUMERIC_VERSION=$($info.Numeric)" "/DWINDOWS_VERSION=$($info.WindowsVersion)" "/DPACKAGE_KB=$([int][Math]::Ceiling($size / 1KB))" "/DPACKAGE_DIR=$stage" "/DLICENSE_RTF=$licenseRtf" "/DOUTPUT_FILE=$setup" "/DINSTALL_FILES=$installInclude" "/DUNINSTALL_FILES=$uninstallInclude" (Join-Path $HolyBearRoot 'installer/holy-bear.nsi')
if ($LASTEXITCODE -ne 0) { throw 'NSIS Setup build failed.' }
& "$PSScriptRoot/verify-installer.ps1" -Setup $setup -PackageDirectory $stage
Write-Output "Branded multilingual installer verified: $($info.SetupName)"
$global:LASTEXITCODE = 0
