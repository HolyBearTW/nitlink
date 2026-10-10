function Get-HolyBearNSIS {
    $version = '3.12'
    $expectedHash = '56581f90db321581c5381193d796fffcf2d24b2f8fed2160a6c6a3baa67f2c4f'
    $tools = Join-Path $HolyBearRoot 'build/installer-tools'
    $archive = Join-Path $tools "nsis-$version.zip"
    $compiler = Join-Path $tools "nsis-$version/makensis.exe"
    New-Item -ItemType Directory -Path $tools -Force | Out-Null
    if (-not (Test-Path -LiteralPath $archive)) {
        & curl.exe --fail --location --silent --show-error --max-time 60 "https://downloads.sourceforge.net/project/nsis/NSIS%203/$version/nsis-$version.zip" --output $archive
        if ($LASTEXITCODE -ne 0) { throw 'NSIS portable compiler download failed.' }
    }
    if ((Get-FileHash -LiteralPath $archive).Hash.ToLowerInvariant() -cne $expectedHash) { throw 'NSIS compiler checksum mismatch.' }
    if (-not (Test-Path -LiteralPath $compiler)) {
        [IO.Compression.ZipFile]::ExtractToDirectory($archive, $tools)
    }
    $actual = & $compiler /VERSION
    if ($LASTEXITCODE -ne 0 -or $actual -ne 'v3.12') { throw 'Unexpected NSIS compiler version.' }
    return $compiler
}

function Get-HolyBear7Zip {
    $path = Join-Path $env:ProgramFiles '7-Zip/7z.exe'
    if (-not (Test-Path -LiteralPath $path)) { throw '7-Zip is required to verify the installer payload.' }
    return $path
}
