$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'publish-tag.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path $repo ('build/publish-tag-tests/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $fixture | Out-Null
$checks = 0
function Invoke-TestGit([string[]]$Arguments) {
    $output = @(& git -C $fixture -c user.name='Taxi Cam publish tag tests' -c user.email='publish-tag-tests@example.invalid' -c commit.gpgSign=false -c core.hooksPath=disabled-test-hooks @Arguments)
    if ($LASTEXITCODE -ne 0) { throw "Publish-tag fixture Git command failed: $($Arguments[0])" }
    return ,$output
}
function Get-FixtureCommit([string]$Rev) {
    $sha = [string](& git -C $fixture rev-parse $Rev)
    if ($LASTEXITCODE -ne 0 -or $sha -notmatch '^[0-9a-fA-F]{40}$') { throw "Fixture revision $Rev was not a commit." }
    return $sha
}
function Commit-Fixture([string]$Message) {
    $null = Invoke-TestGit @('add','--all')
    $null = Invoke-TestGit @('commit','--quiet','-m',$Message)
}
function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
function Assert-TextContains([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) { throw $Message }
    $script:checks++
}
function Assert-TextAbsent([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -match $Pattern) { throw $Message }
    $script:checks++
}

$null = Invoke-TestGit @('init','--quiet','-b','main')
$null = Invoke-TestGit @('commit','--quiet','--allow-empty','-m','Initial fixture')
$first = Get-FixtureCommit 'HEAD'
Assert-True ($null -eq (Get-TaxiLastMainReleaseTag -Repository $fixture -Commit $first)) `
    'A main history with no release tags has no last published tag.'
Assert-True ((Get-TaxiNextPublishTag -Repository $fixture -Commit $first -Version '0.9.8') -ceq 'v0.9.8-build.1') `
    'The first publish tag is build.1 when main has no release tag.'

$null = Invoke-TestGit @('tag','v0.9.8-build.38')
'change' | Set-Content -LiteralPath (Join-Path $fixture 'app.txt') -Encoding ascii
Commit-Fixture 'Merge pull request #35 from example/notes'
$unpublished = Get-FixtureCommit 'HEAD'
Assert-True ((Get-TaxiLastMainReleaseTag -Repository $fixture -Commit $unpublished) -ceq 'v0.9.8-build.38') `
    'Last tag on main is the nearest first-parent release tag.'
Assert-True ((Get-TaxiLastMainReleaseTag -Repository $fixture -Commit $unpublished -ExcludeCommit) -ceq 'v0.9.8-build.38') `
    'Notes baseline is the last tag on main before the commit being published.'
Assert-True ((Get-TaxiNextPublishTag -Repository $fixture -Commit $unpublished -Version '0.9.9') -ceq 'v0.9.9-build.39') `
    'Publish creates the next tag from the last tag on main, using this commit version.'

foreach ($stale in @('0.9.8','0.9.7','0.8.99')) {
    $rejected = $false
    try { $null = Get-TaxiNextPublishTag -Repository $fixture -Commit $unpublished -Version $stale } catch { $rejected = $true }
    Assert-True $rejected "Publishing $stale after v0.9.8-build.38 must require a higher changelog version."
}
Assert-True ((Get-TaxiNextPublishTag -Repository $fixture -Commit $unpublished -Version '0.10.0') -ceq 'v0.10.0-build.39') `
    'Versions compare numerically, so 0.10.0 is higher than 0.9.8.'

$null = Invoke-TestGit @('switch','--quiet','-c','feature')
'branch' | Set-Content -LiteralPath (Join-Path $fixture 'app.txt') -Encoding ascii
Commit-Fixture 'Feature work'
$null = Invoke-TestGit @('tag','v0.9.9-build.100')
$feature = Get-FixtureCommit 'HEAD'
$null = Invoke-TestGit @('switch','--quiet','main')
Assert-True ((Get-TaxiLastMainReleaseTag -Repository $fixture -Commit $unpublished) -ceq 'v0.9.8-build.38') `
    'A tag that is not on main first-parent history is not the last main tag.'
Assert-True ((Get-TaxiNextPublishTag -Repository $fixture -Commit $unpublished -Version '0.9.9') -ceq 'v0.9.9-build.39') `
    'Feature-branch tags do not increment the published build number.'
Assert-True ((Get-TaxiNextPublishTag -Repository $fixture -Commit $feature -Version '0.9.9') -ceq 'v0.9.9-build.100') `
    'An already-tagged commit reuses its tag instead of incrementing.'

$beforeTags = @(& git -C $fixture tag --list)
$null = Get-TaxiNextPublishTag -Repository $fixture -Commit $unpublished -Version '0.9.9'
$afterTags = @(& git -C $fixture tag --list)
Assert-True ((@($beforeTags) -join ',') -ceq (@($afterTags) -join ',')) `
    'Computing the next publish tag must not create a git tag.'

$workflow = Get-Content -Raw -LiteralPath (Join-Path $repo '.github/workflows/release.yml')
$buildJob = [regex]::Match($workflow, '(?ms)^  build:.*?(?=^  publish:)')
$publishJob = [regex]::Match($workflow, '(?ms)^  publish:.*')
Assert-True ($buildJob.Success -and $publishJob.Success) 'Release workflow must define separate build and publish jobs.'
Assert-TextContains $workflow '(?m)^permissions:\r?\n  contents: read\s*$' 'The workflow default must stay read-only.'
Assert-TextContains $publishJob.Value '(?m)^\s+environment:\r?\n\s+name: release\s*$' 'Only publish waits for the release environment.'
Assert-TextContains $publishJob.Value 'contents:\s*write' 'Only publish may write tags or releases.'
Assert-TextContains $publishJob.Value 'publish-release\.ps1' 'Publish job must run the publication script.'
Assert-TextAbsent $buildJob.Value 'publish-release\.ps1' 'Main-build job must not publish.'
Assert-TextAbsent $buildJob.Value 'git tag' 'Main-build job must not create tags.'
Assert-TextAbsent $buildJob.Value 'gh release' 'Main-build job must not create GitHub releases.'
Assert-TextAbsent $buildJob.Value 'contents:\s*write' 'Main-build job must not request write access.'

$script = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot 'publish-release.ps1')
Assert-TextContains $script 'Get-TaxiNextPublishTag' 'Publish must choose the next tag from the last tag on main.'
Assert-TextContains $script 'Get-TaxiLastMainReleaseTag' 'Release notes must start from the last tag on main.'
Assert-TextContains $script "release','create'" 'Publish creates the tag with gh release create.'
Assert-TextContains $script '--target' 'Publish must tag the exact main commit being published.'
Assert-TextAbsent $script 'v\$\(\$receipt\.version\)-build\.\$BuildNumber' 'Publish must not reuse the workflow run number as the release tag.'

$prWorkflowPath = Join-Path $repo '.github/workflows/pr-test-build.yml'
if (Test-Path -LiteralPath $prWorkflowPath -PathType Leaf) {
    $pr = Get-Content -Raw -LiteralPath $prWorkflowPath
    Assert-TextAbsent $pr 'publish-release\.ps1' 'PR CI must not publish.'
    Assert-TextAbsent $pr 'git tag' 'PR CI must not create tags.'
    Assert-TextAbsent $pr 'gh release' 'PR CI must not create GitHub releases.'
    Assert-TextAbsent $pr 'contents:\s*write' 'PR CI must stay read-only.'
    Assert-TextAbsent $pr 'environment:\s*release' 'PR CI must not use the release environment.'
}

$updater = Get-Content -Raw -LiteralPath (Join-Path $repo 'src/app/updater.cpp')
Assert-TextContains $updater 'L"https://api\.github\.com/repos/" \+ wide\(Repository\) \+ L"/releases/latest"' `
    'Auto-update must read published GitHub releases only.'
Assert-TextContains $updater 'constexpr char Repository\[\] = "rthoms334/taxi-cam";' 'Auto-update must read the project repository.'
Assert-TextAbsent $updater 'windows-pr-test-build-' 'Auto-update must not read PR artifacts.'
Assert-TextAbsent $updater 'windows-release-' 'Auto-update must not read untagged main-build artifacts.'

Write-Output "PASS publish tags on main: $checks checks for last-tag baseline, next publish tag, version advance, and no tags from PR/main-build."
