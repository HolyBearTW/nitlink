. "$PSScriptRoot/holy-bear-common.ps1"
. "$PSScriptRoot/holy-bear-release-notes.ps1"
$info = Get-HolyBearVersion
$script:checks = 0
function Assert-Notes([bool]$Condition, [string]$Name) {
    if (-not $Condition) { throw "FAIL $Name" }
    $script:checks++
    Write-Output "PASS $Name"
}
function Assert-Rejected([scriptblock]$Operation, [string]$Name) {
    $rejected = $false
    try { & $Operation | Out-Null } catch { $rejected = $true }
    Assert-Notes $rejected $Name
}

# All tag/API data below are fixtures. No Git tag or GitHub Release is created.
$decoder = [Text.UTF8Encoding]::new($false, $true)
$notes = $decoder.GetString([IO.File]::ReadAllBytes($info.Notes)).TrimStart([char]0xfeff)
Assert-Rejected { $decoder.GetString([byte[]]@(0xff, 0x80)) } 'invalid UTF-8 bytes rejected'
Assert-Notes (($notes -split '\r?\n', 2)[0] -ceq "# $($info.Title)") 'exact Markdown product title'
Assert-Notes ($notes.Contains('非官方社群整合版') -and $notes.Contains('來源、授權與致謝') -and
    $notes.Contains('已知限制') -and $notes.Contains('English Summary')) 'UTF-8 Traditional Chinese notes retain required sections'
Assert-Notes ($notes -notmatch '審閱草稿|預計納入|final release validation is pending|41/41|38155376483') 'no draft or obsolete build results'
Assert-Notes ($notes.Contains('既有 1.2.4 安裝版') -and $notes.Contains('不代表 1.2.4.1 正式產物')) 'older hardware evidence is identified honestly'
Assert-Notes ($notes.Contains('自動模式無法取得更新率') -and $notes.Contains('30 Hz fallback')) 'conservative fallback is scoped to Auto'
$links = @([regex]::Matches($notes, '\[[^\]\r\n]+\]\(([^)\s]+)\)'))
Assert-Notes ($links.Count -gt 0) 'Markdown contains attribution and verification links'
foreach ($link in $links) {
    $uri = [uri]$link.Groups[1].Value
    Assert-Notes ($uri.IsAbsoluteUri -and $uri.Scheme -ceq 'https' -and $uri.Host -ceq 'github.com') 'well-formed HTTPS source link'
}
Assert-Notes (([regex]::Matches($notes, '(?m)^```').Count % 2) -eq 0) 'balanced Markdown fences'

$commit = '0123456789abcdef0123456789abcdef01234567'
$otherCommit = 'abcdef0123456789abcdef0123456789abcdef01'
$actions = 'https://github.com/HolyBearTW/nitlink/actions/runs/123456789'
$passed = [pscustomobject]@{tests = 7; passed = 7; failures = 0; skipped = 0; knownFailure = $null}
$baseline = [pscustomobject]@{tests = 7; passed = 6; failures = 1; skipped = 0; knownFailure = 'FAIL restarted host reloads the packaged menu'}
$preview = Get-HolyBearReleaseNotes -Notes $notes -Title $info.Title -Commit $commit -Tag $info.Tag -Summary $passed -ActionsUrl $actions
Assert-Notes ($preview.Contains('預定版本標籤') -and $preview.Contains('尚未驗證正式 Git 標籤') -and
    -not $preview.Contains('已驗證的正式 Git 標籤')) 'preview never claims a verified source tag'
Assert-Notes ($preview.Contains($commit) -and $preview.Contains($info.Tag) -and $preview.Contains('7/7') -and
    $preview.Contains('失敗 0 項，略過 0 項') -and $preview.Contains($actions)) 'preview retains dynamic identity, test counts and Actions URL'
$notesBody = ($notes -split '\r?\n', 2)[1] -replace '^\r?\n', ''
Assert-Notes ($preview.StartsWith($notesBody, [StringComparison]::Ordinal) -and
    -not $preview.StartsWith("# $($info.Title)", [StringComparison]::Ordinal) -and
    $preview.Contains('## 此發布套件的建置驗證結果')) 'generated notes retain the complete body without repeating the product title'
$formal = Get-HolyBearReleaseNotes -Notes $notes -Title $info.Title -Commit $otherCommit -Tag $info.Tag -Summary $baseline -TagSourceVerified $true
Assert-Notes ($formal.Contains('已驗證的正式 Git 標籤') -and -not $formal.Contains('預定版本標籤')) 'verified formal tag fixture has distinct wording'
Assert-Notes ($formal.Contains($otherCommit) -and $formal.Contains('6/7') -and $formal.Contains($baseline.knownFailure) -and
    -not $formal.Contains($actions)) 'formal fixture retains exact baseline failure and optional Actions URL'
# Exercise the structural source-tag guard using only a scoped git mock.
function Get-FormalTagFixture([string]$Tag, [string]$Resolved, [int]$ExitCode) {
    $script:fixtureResolved = $Resolved
    $script:fixtureExit = $ExitCode
    function git {
        $global:LASTEXITCODE = $script:fixtureExit
        Write-Output $script:fixtureResolved
    }
    Assert-HolyBearTag $Tag $commit
    Get-HolyBearReleaseNotes -Notes $notes -Title $info.Title -Commit $commit -Tag $Tag -Summary $passed -TagSourceVerified $true
}
$verifiedFixture = Get-FormalTagFixture $info.Tag $commit 0
Assert-Notes ($verifiedFixture.Contains('已驗證的正式 Git 標籤')) 'formal source-tag guard succeeds with a matching mock Commit'
Assert-Rejected { Get-FormalTagFixture $info.Tag $otherCommit 0 } 'formal source-tag guard rejects a different mock Commit'
Assert-Rejected { Get-FormalTagFixture $info.Tag '' 1 } 'formal source-tag guard rejects an absent mock tag'
Assert-Rejected { Get-FormalTagFixture 'v9.9.9-holybear' $commit 0 } 'formal source-tag guard rejects the wrong version tag'
foreach ($bad in @(
    [pscustomobject]@{tests = 7; passed = 6; failures = 0; skipped = 1; knownFailure = $null},
    [pscustomobject]@{tests = 7; passed = 7; failures = 1; skipped = 0; knownFailure = $baseline.knownFailure},
    [pscustomobject]@{tests = 7; passed = 5; failures = 2; skipped = 0; knownFailure = $baseline.knownFailure},
    [pscustomobject]@{tests = 7; passed = 6; failures = 1; skipped = 0; knownFailure = 'FAIL another assertion'},
    [pscustomobject]@{tests = 7; passed = 7; failures = 0; skipped = 0; knownFailure = $baseline.knownFailure},
    [pscustomobject]@{tests = 0; passed = 0; failures = 0; skipped = 0; knownFailure = $null},
    [pscustomobject]@{tests = 7; failures = 0; skipped = 0; knownFailure = $null}
)) {
    Assert-Rejected { Get-HolyBearReleaseNotes -Notes $notes -Title $info.Title -Commit $commit -Tag $info.Tag -Summary $bad } 'invalid/incomplete test summary rejected'
}
Assert-Rejected { Get-HolyBearReleaseNotes -Notes ($notes.Replace("# $($info.Title)", '# Wrong title')) -Title $info.Title -Commit $commit -Tag $info.Tag -Summary $passed } 'wrong title rejected'
Assert-Rejected { Get-HolyBearReleaseNotes -Notes $notes -Title $info.Title -Commit 'short' -Tag $info.Tag -Summary $passed } 'invalid Commit rejected'

$lf = $preview.Replace("`r`n", "`n")
foreach ($equivalent in @($preview, $lf, $lf.Replace("`n", "`r`n"), ([char]0xfeff + $lf), $lf.TrimEnd([char]10), ($lf + "`n`n"))) {
    Assert-HolyBearReleaseNotesMatch -Expected $preview -Actual $equivalent
    Assert-Notes $true 'only transport BOM/newlines are normalized'
}
foreach ($different in @(
    '', ($lf.Replace('7/7', '6/7')), ($lf.Replace($commit, $otherCommit)),
    ($lf.Replace($info.Tag, 'v9.9.9-holybear')), ($lf.Replace($actions, "$actions/other")),
    ($lf.Replace('## 本次更新', '## 本次 更新')), ($lf.Replace('English Summary', 'English summary')),
    ($lf.Replace('## 已知限制', '')), ($lf + ' '), ($lf.Replace("`n`n## 本次更新", "`n## 本次更新"))
)) {
    Assert-Rejected { Assert-HolyBearReleaseNotesMatch -Expected $preview -Actual $different } 'body, numbers, links, case and paragraph changes rejected'
}
# Mock GitHub API JSON body transport and the publication gate, never call gh.
$script:publications = 0
function Invoke-ReleaseBodyFixture([string]$Json) {
    $draft = $Json | ConvertFrom-Json
    Assert-HolyBearReleaseNotesMatch -Expected $preview -Actual $draft.body
    $script:publications++
}
Invoke-ReleaseBodyFixture ([pscustomobject]@{body = $lf; isDraft = $true} | ConvertTo-Json)
Assert-Notes ($script:publications -eq 1) 'mock API preserves the exact generated body'
Assert-Rejected { Invoke-ReleaseBodyFixture ([pscustomobject]@{body = $lf.Replace('7/7', '0/7'); isDraft = $true} | ConvertTo-Json) } 'mock API body mismatch blocks publication'
Assert-Notes ($script:publications -eq 1) 'mismatched mock never reaches publication'

$package = Get-Content -LiteralPath "$PSScriptRoot/package-holybear.ps1" -Raw
$publisher = Get-Content -LiteralPath "$PSScriptRoot/publish-holybear.ps1" -Raw
Assert-Notes ($package.IndexOf('Assert-HolyBearSignedTag $Tag $commit', [StringComparison]::Ordinal) -gt 0 -and
    $package.IndexOf('Assert-HolyBearSignedTag $Tag $commit', [StringComparison]::Ordinal) -lt
    $package.IndexOf('Get-HolyBearReleaseNotes -Notes', [StringComparison]::Ordinal) -and
    $package.Contains('-TagSourceVerified $RequireTag.IsPresent') -and
    $package.Contains('"tag_source_verified=$($RequireTag.IsPresent.ToString().ToLowerInvariant())"')) 'RequireTag guard and machine verification field remain authoritative'
Assert-Notes ($package.Contains("Copy-Item -LiteralPath (Join-Path `$stage 'RELEASE-NOTES.md') -Destination `$output") -and
    $package.Contains('CreateFromDirectory($stage') -and $package.Contains('-PackageDirectory $stage')) 'ZIP, Setup and upload notes share one generated stage file'
$draftCheck = $publisher.IndexOf('Assert-HolyBearReleaseNotesMatch -Expected $expectedNotes -Actual $draft.body', [StringComparison]::Ordinal)
$publish = $publisher.IndexOf('& gh release edit $Tag', [StringComparison]::Ordinal)
Assert-Notes ($publisher.Contains("--notes-file (Join-Path `$assetRoot 'RELEASE-NOTES.md')") -and
    $draftCheck -gt 0 -and $publish -gt $draftCheck -and
    $publisher.Contains('Assert-HolyBearReleaseNotesMatch -Expected $expectedNotes -Actual $published.body')) 'publisher checks body before exposure and verifies it afterward'
foreach ($field in @('validation_only=false', 'source_ref=refs/tags/$Tag', 'environment=release')) {
    Assert-Notes ($publisher.Contains($field)) 'publication machine safety field is unchanged'
}
$docs = Get-Content -LiteralPath (Join-Path $HolyBearRoot 'docs/HOLYBEAR-RELEASE-SIGNING.md') -Raw
Assert-Notes ($docs.Contains($info.ZipName) -and $docs.Contains($info.SetupName) -and
    $docs.Contains("--tag $($info.Tag)") -and $docs.Contains('5DA247A5EFA2AACB4EB1A3A2D278ED91B6F2C0BA') -and
    $docs.Contains('B96A785DE4C5083A0DFF15EA8925D54ECCF3EADF')) 'packaged signing instructions match the new version and pinned keys'
Write-Output "Release note regression checks passed: $script:checks. No tag or release operations performed."
