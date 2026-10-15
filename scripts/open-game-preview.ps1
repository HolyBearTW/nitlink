$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskHealthUrl = 'http://127.0.0.1:8765/preview/bridge.js?operation=artworkCatalog&locale=zh-TW&bridgeId=1'
$taskRunning = $false
try {
    $taskResponse = Invoke-WebRequest -Uri $taskHealthUrl -TimeoutSec 2 -UseBasicParsing
    $taskRunning = $taskResponse.Content.Contains('Official Nintendo Taiwan')
} catch {}
if (-not $taskRunning) {
    $taskPython = (Get-Command python -ErrorAction Stop).Source
    $taskServerScript = Join-Path $PSScriptRoot 'serve-game-preview.py'
    Start-Process -FilePath $taskPython -ArgumentList ('"' + $taskServerScript + '"') -WorkingDirectory $taskRoot -WindowStyle Hidden
    for ($taskAttempt = 0; $taskAttempt -lt 20; ++$taskAttempt) {
        Start-Sleep -Milliseconds 100
        try {
            $taskResponse = Invoke-WebRequest -Uri $taskHealthUrl -TimeoutSec 1 -UseBasicParsing
            if ($taskResponse.Content.Contains('Official Nintendo Taiwan')) { $taskRunning = $true; break }
        } catch {}
    }
}
if (-not $taskRunning) { throw 'The preview server could not start on 127.0.0.1:8765.' }
$taskNode = (Get-Command node -ErrorAction Stop).Source
$taskBrowserScript = Join-Path $PSScriptRoot 'preview-browser.cjs'
if (-not (Test-Path -LiteralPath (Join-Path $taskRoot 'out/ui-preview-runtime/node_modules/playwright-core'))) {
    throw 'Preview runtime missing. Run: npm install --prefix out/ui-preview-runtime --no-save playwright-core'
}
# The visible Chrome/Edge window is the requested interactive review tool.
# Its hidden helper supplies the exact source HTML without changing AV settings.
Start-Process -FilePath $taskNode -ArgumentList ('"' + $taskBrowserScript + '"') -WorkingDirectory $taskRoot -WindowStyle Hidden
