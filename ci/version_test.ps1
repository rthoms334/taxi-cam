$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'version.ps1')
$repo = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path $repo ('build/version-tests/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $fixture | Out-Null
$checks = 0
function Write-Changelog([string[]]$Versions) {
    $releases = @($Versions | ForEach-Object { @{version=$_; changes=@('Change.')} })
    @{releases=$releases} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $fixture 'changelog.json') -Encoding utf8
}
function Assert-Version([string]$Expected, [int]$BuildNumber = 10) {
    $actual = Get-TaxiVersion $fixture $BuildNumber
    if ($actual.Version -cne $Expected -or $actual.BuildNumber -ne $BuildNumber) { throw "Expected $Expected, got $($actual.Version)." }
    $script:checks++
}
function Reject-Version([string]$Name) {
    $rejected = $false
    try { Get-TaxiVersion $fixture | Out-Null } catch { $rejected = $true }
    if (-not $rejected) { throw "Invalid changelog was accepted: $Name." }
    $script:checks++
}
# The release version is the newest changelog entry, independent of Git
# history, tags or how many commits have landed since it was added.
Write-Changelog @('0.9.50')
Assert-Version '0.9.50' 0
Assert-Version '0.9.50' 99
Write-Changelog @('0.9.51','0.9.50','0.9.47')
Assert-Version '0.9.51'
Write-Changelog @('1.0.0','0.9.51')
$actual = Get-TaxiVersion $fixture
if ($actual.Major -ne 1 -or $actual.Minor -ne 0 -or $actual.Patch -ne 0) { throw 'Version components were not split.' }
$checks++
Write-Changelog @('0.0.65535')
Assert-Version '0.0.65535'
foreach ($invalid in @('01.2.3','1.2','1.2.3-preview','1.2.3+build.4','1.2.-1','1.2.65536','999999999999.0.0','')) {
    Write-Changelog @($invalid)
    Reject-Version "version '$invalid'"
}
Write-Changelog @('0.9.50','0.9.51')
Reject-Version 'ascending versions'
Write-Changelog @('0.9.50','0.9.50')
Reject-Version 'duplicate versions'
'{"releases":[]}' | Set-Content -LiteralPath (Join-Path $fixture 'changelog.json') -Encoding ascii
Reject-Version 'empty releases'
'{"notes":[]}' | Set-Content -LiteralPath (Join-Path $fixture 'changelog.json') -Encoding ascii
Reject-Version 'missing releases'
'{"releases":[{"changes":["Change."]}]}' | Set-Content -LiteralPath (Join-Path $fixture 'changelog.json') -Encoding ascii
Reject-Version 'missing version'
Remove-Item -LiteralPath (Join-Path $fixture 'changelog.json')
Reject-Version 'missing changelog.json'
$repositoryVersion = Get-TaxiVersion $repo
$repositoryTop = @((Get-Content -Raw -LiteralPath (Join-Path $repo 'changelog.json') | ConvertFrom-Json).releases)[0].version
if ($repositoryVersion.Version -cne $repositoryTop) { throw 'The repository build version differs from its newest changelog entry.' }
$checks++
function Assert-ReleaseBuildNumber([int]$Expected, [hashtable]$Context, [string]$Name) {
    $actual = Get-TaxiReleaseBuildNumber @Context
    if ($actual -ne $Expected) { throw "Expected release build $Expected for $Name, got $actual." }
    $script:checks++
}
Assert-ReleaseBuildNumber 0 @{GitHubActions=''; EventName=''; Ref=''; RunNumber=''} 'omitted GitHub context is a local build'
Assert-ReleaseBuildNumber 0 @{GitHubActions='true'; EventName='pull_request'; Ref='refs/pull/12/merge'; RunNumber='99'} 'pull_request keeps build 0'
Assert-ReleaseBuildNumber 0 @{GitHubActions='true'; EventName='pull_request_target'; Ref='refs/heads/main'; RunNumber='99'} 'pull_request_target cannot inherit main run numbers'
Assert-ReleaseBuildNumber 0 @{GitHubActions='true'; EventName='push'; Ref='refs/heads/feature/pr-ci'; RunNumber='99'} 'non-main refs keep build 0'
Assert-ReleaseBuildNumber 0 @{GitHubActions='true'; EventName='push'; Ref='refs/heads/main'; RunNumber=''} 'main without a run number stays 0'
Assert-ReleaseBuildNumber 42 @{GitHubActions='true'; EventName='push'; Ref='refs/heads/main'; RunNumber='42'} 'main push stamps the workflow run number'
Assert-ReleaseBuildNumber 7 @{GitHubActions='true'; EventName='workflow_dispatch'; Ref='refs/heads/main'; RunNumber='7'} 'main workflow_dispatch stamps the workflow run number'
$rejected = $false
try { Get-TaxiReleaseBuildNumber -GitHubActions 'true' -EventName 'push' -Ref 'refs/heads/main' -RunNumber '01' | Out-Null } catch { $rejected = $true }
if (-not $rejected) { throw 'Leading-zero run numbers were accepted.' }
$script:checks++
Write-Output "PASS release versions: $checks checks for changelog-sourced versions, ordering, Windows limits and publish-only build numbers."
