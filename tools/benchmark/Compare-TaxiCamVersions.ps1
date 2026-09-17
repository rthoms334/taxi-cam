[CmdletBinding()]
param(
    [string]$BaselineRef = 'v0.9.8-build.38',
    [string]$CandidateRef = 'HEAD',
    [string]$BaselineLog,
    [string]$CandidateLog,
    [switch]$RunIsolated,
    [string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $PSScriptRoot 'TaxiCamBenchmark.ps1')

if (-not $OutputDirectory) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $OutputDirectory = Join-Path $repoRoot ('build/benchmark/' + $stamp)
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$logs = $null
if ($BaselineLog -or $CandidateLog) {
    if (-not $BaselineLog -or -not $CandidateLog) { throw 'Both -BaselineLog and -CandidateLog are required for a log comparison.' }
    $logs = Compare-TaxiBridgeLogSummaries `
        (Get-TaxiBridgeLogSummary (ConvertFrom-TaxiBridgeLog $BaselineLog) 'baseline') `
        (Get-TaxiBridgeLogSummary (ConvertFrom-TaxiBridgeLog $CandidateLog) 'candidate')
}

$isolatedTimings = $null
if ($RunIsolated) {
    $isolatedRoot = Join-Path $OutputDirectory 'isolated'
    $baselineTimings = Invoke-TaxiIsolatedRevision -Repository $repoRoot -Revision $BaselineRef -OutputDirectory (Join-Path $isolatedRoot 'baseline')
    $candidateTimings = Invoke-TaxiIsolatedRevision -Repository $repoRoot -Revision $CandidateRef -OutputDirectory (Join-Path $isolatedRoot 'candidate')
    $isolatedTimings = Compare-TaxiIsolatedTimings $baselineTimings $candidateTimings
}

$report = New-TaxiVersionBenchmarkReport -Repository $repoRoot -BaselineRef $BaselineRef -CandidateRef $CandidateRef -BridgeLogs $logs -IsolatedTimings $isolatedTimings
$jsonPath = Join-Path $OutputDirectory 'comparison.json'
$report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $jsonPath -Encoding utf8
Write-Output $jsonPath
$report | ConvertTo-Json -Depth 8
