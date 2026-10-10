@echo off
setlocal
cd /d "%~dp0"
if errorlevel 1 exit /b 1
REM HolyBear Edition: build with scripts/build-holybear.ps1 before packaging.
REM The source checkout must be committed. Existing packages are never overwritten.
if "%~1"=="" (
  pwsh -NoProfile -File scripts\package-holybear.ps1
) else (
  pwsh -NoProfile -File scripts\package-holybear.ps1 -BuildDirectory "%~1"
)
exit /b %errorlevel%
