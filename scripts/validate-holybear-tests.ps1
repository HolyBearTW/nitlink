param(
    [Parameter(Mandatory)][string]$Report,
    [Parameter(Mandatory)][int]$ExpectedTests,
    [Parameter(Mandatory)][int]$CtestExitCode,
    [Parameter(Mandatory)][string]$Summary
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
[xml]$xml = Get-Content -LiteralPath $Report -Raw
$suite = $xml.testsuite
$cases = @($suite.testcase)
if ($ExpectedTests -lt 1 -or $cases.Count -ne $ExpectedTests -or
    [int]$suite.tests -ne $ExpectedTests -or [int]$suite.skipped -ne 0) {
    throw 'Incomplete/skipped CTest suite; release rejected.'
}
$failures = @()
foreach ($case in $cases) {
    if ($case.SelectSingleNode('skipped') -or $case.status -notin @('run', 'fail')) { throw "Unexecuted test: $($case.name)" }
    if ($case.SelectSingleNode('failure')) { $failures += $case }
    elseif ($case.status -ne 'run') { throw "Inconsistent test status: $($case.name)" }
}
if ([int]$suite.failures -ne $failures.Count) { throw 'Inconsistent JUnit failure count.' }
$known = 'FAIL restarted host reloads the packaged menu'
if ($failures.Count -eq 0) {
    if ($CtestExitCode -ne 0) { throw 'CTest returned a failure without a complete failure report.' }
} elseif ($failures.Count -eq 1) {
    $case = $failures[0]
    $lines = @([regex]::Matches([string]$case.'system-out', '(?m)^FAIL[^\r\n]*') | ForEach-Object Value)
    if ($CtestExitCode -ne 8 -or $case.name -ne 'webview_runtime_security' -or
        $case.failure.message -ne 'Failed' -or $lines.Count -ne 1 -or $lines[0] -cne $known) {
        throw 'Failure is not the exact permitted upstream WebView restart assertion.'
    }
    Write-Warning $known
} else { throw 'New CTest failures; release rejected.' }
$result = [ordered]@{
    tests = $ExpectedTests; passed = $ExpectedTests - $failures.Count
    failures = $failures.Count; skipped = 0
    knownFailure = if ($failures.Count) { $known } else { $null }
}
$result | ConvertTo-Json | Set-Content -LiteralPath $Summary -Encoding utf8
Write-Output "CTest: $($result.passed)/$ExpectedTests; known baseline failures=$($result.failures); skipped=0."
