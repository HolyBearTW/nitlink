param([string]$BuildDirectory = 'build/holy-bear')
. "$PSScriptRoot/holy-bear-common.ps1"
$info = Get-HolyBearVersion
$build = [IO.Path]::GetFullPath((Join-Path $HolyBearRoot $BuildDirectory))
New-Item -ItemType Directory -Path $build -Force | Out-Null
$summaryPath = Join-Path $build 'test-summary.json'
if (Test-Path -LiteralPath $summaryPath) { Remove-Item -LiteralPath $summaryPath }
Initialize-HolyBearToolchain
& node (Join-Path $PSScriptRoot 'validate-locales.js')
if ($LASTEXITCODE -ne 0) { throw 'Localization validation failed.' }
& cmake -S $HolyBearRoot -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DNITLINK_TEST_WEBVIEW_RUNTIME=ON
if ($LASTEXITCODE -ne 0) { throw 'Release configure failed.' }
& cmake --build $build --clean-first --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Release build failed.' }
Assert-HolyBearExecutable (Join-Path $build 'NitLink.exe')
$discovered = & ctest --test-dir $build -C Release --show-only=json-v1
if ($LASTEXITCODE -ne 0) { throw 'CTest discovery failed.' }
$expected = ($discovered -join "`n" | ConvertFrom-Json).tests.Count
& ctest --test-dir $build -C Release --output-on-failure --timeout 90 --output-junit (Join-Path $build 'ctest-results.xml') 2>&1 |
    Tee-Object -FilePath (Join-Path $build 'ctest-output.log')
$ctestCode = $LASTEXITCODE
& "$PSScriptRoot/validate-holybear-tests.ps1" -Report (Join-Path $build 'ctest-results.xml') -ExpectedTests $expected -CtestExitCode $ctestCode -Summary (Join-Path $build 'test-summary.json')
Write-Output "Release build and test policy passed for $($info.Version)."
# The raw nonzero CTest result has been recorded and strictly classified above.
$global:LASTEXITCODE = 0
