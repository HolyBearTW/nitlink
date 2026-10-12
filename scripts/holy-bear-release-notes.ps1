function Get-HolyBearReleaseNotes {
    param(
        [Parameter(Mandatory)][string]$Notes,
        [Parameter(Mandatory)][string]$Title,
        [Parameter(Mandatory)][string]$Commit,
        [Parameter(Mandatory)][string]$Tag,
        [Parameter(Mandatory)]$Summary,
        [bool]$TagSourceVerified = $false,
        [string]$ActionsUrl = ''
    )
    if (($Notes -split '\r?\n', 2)[0] -cne "# $Title") { throw 'Release note title mismatch.' }
    if ($Commit -cnotmatch '^[0-9a-f]{40}$') { throw 'Invalid release note source commit.' }
    if ($Summary.tests -lt 1 -or $Summary.skipped -ne 0 -or $Summary.failures -notin @(0, 1) -or
        $Summary.passed -lt 0 -or $Summary.passed + $Summary.failures -ne $Summary.tests -or
        ($Summary.failures -eq 1 -and $Summary.knownFailure -cne 'FAIL restarted host reloads the packaged menu') -or
        ($Summary.failures -eq 0 -and $Summary.knownFailure)) {
        throw 'Missing or inconsistent validated test summary.'
    }
    # Keep the validated title in the source Markdown for packaging checks.
    # The generated notes/body share the content below it; GitHub supplies the title.
    $body = ($Notes -split '\r?\n', 2)[1] -replace '^\r?\n', ''
    $result = $body + "`n## 此發布套件的建置驗證結果`n`n"
    $result += "- 原始碼 Commit：``$Commit``。`n"
    if ($TagSourceVerified) {
        $result += "- 已驗證的正式 Git 標籤：``$Tag``（已核對來源 Commit）。`n"
    } else {
        $result += "- 預定版本標籤：``$Tag``（preview 封裝，尚未驗證正式 Git 標籤）。`n"
    }
    $result += "- 乾淨環境 x64 Release 建置：通過。完整 CTest：$($Summary.passed)/$($Summary.tests) 通過，失敗 $($Summary.failures) 項，略過 $($Summary.skipped) 項。`n"
    if ($Summary.failures) {
        $result += "- 如實保留的上游基準失敗：``$($Summary.knownFailure)``。未接受其他測試失敗。`n"
    } else {
        $result += "- 此建置環境的所有 CTest 測試均通過；既有本機 WebView 重新啟動問題仍列為已知限制。`n"
    }
    if ($ActionsUrl) { $result += "- GitHub Actions：$ActionsUrl`n" }
    return $result
}

function Assert-HolyBearReleaseNotesMatch {
    param(
        [Parameter(Mandatory)][string]$Expected,
        [Parameter(Mandatory)][AllowEmptyString()][string]$Actual
    )
    # GitHub body/UTF-8 file transport can differ only in BOM and line endings.
    $normalize = { param([string]$Value)
        $Value.TrimStart([char]0xfeff).Replace("`r`n", "`n").TrimEnd([char]10)
    }
    if ((& $normalize $Expected) -cne (& $normalize $Actual)) {
        throw 'GitHub Release body does not match RELEASE-NOTES.md.'
    }
}
