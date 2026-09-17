$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repoRoot 'tools/benchmark/TaxiCamBenchmark.ps1')
$checks = 0
function Require-Benchmark([bool]$Condition, [string]$Message) {
    $script:checks++
    if (-not $Condition) { throw $Message }
}

$baselineSamples = ConvertFrom-TaxiBridgeLog (Join-Path $PSScriptRoot 'fixtures/bridge-098.txt')
$candidateSamples = ConvertFrom-TaxiBridgeLog (Join-Path $PSScriptRoot 'fixtures/bridge-candidate.txt')
Require-Benchmark ($baselineSamples.Count -eq 5) 'Parser kept a non-timing line or dropped a probe_ms snapshot'
Require-Benchmark ($candidateSamples.Count -eq 5) 'Candidate fixture parse count changed'
Require-Benchmark ($baselineSamples[0].tick_ms -eq 100000 -and $baselineSamples[0].probe_ms -eq 0.52) 'First baseline sample fields changed'
Require-Benchmark ($baselineSamples[-1].captured -eq 320 -and $baselineSamples[-1].composed -eq 256) 'Counter fields were not parsed'

$baseline = Get-TaxiBridgeLogSummary $baselineSamples 'baseline'
$candidate = Get-TaxiBridgeLogSummary $candidateSamples 'candidate'
Require-Benchmark ($baseline.probe_ms.median -eq 0.52) 'Baseline probe median is not the middle sample'
Require-Benchmark ($candidate.probe_ms.median -eq 0.42) 'Candidate probe median is not the middle sample'
Require-Benchmark ([Math]::Abs($baseline.throughput.composed_per_s - 12) -lt 0.001) 'Baseline composed rate is not 240 frames / 20 s'
Require-Benchmark ([Math]::Abs($candidate.throughput.composed_per_s - 12) -lt 0.001) 'Candidate composed rate changed while counters matched'

$logs = Compare-TaxiBridgeLogSummaries $baseline $candidate
Require-Benchmark ($logs.probe_ms.median_delta -lt 0) 'Synthetic candidate was not reported as a lower observer time'
Require-Benchmark ([Math]::Abs($logs.throughput.composed_pct) -lt 0.001) 'Identical counter slopes reported a throughput change'

$same = Compare-TaxiBridgeLogSummaries $baseline $baseline
Require-Benchmark ($same.probe_ms.median_delta -eq 0 -and $same.probe_ms.median_pct -eq 0) 'Identical logs must compare as zero delta'

$timing = ConvertFrom-TaxiIsolatedTimingOutput @'
Own-process public query wrappers: queries_each=14336 VirtualQuery_ms=0.111111 VirtualQueryEx_ms=0.222222 per28; live benefit unmeasured.
Own-memory stage: 196 exact reads unchanged, VirtualQuery 196 -> 2, 1.250 -> 0.400 ms incl endpoint check.
Descending own-allocation graph: queries_per_stage=2 exact_reads=28 bytes=224 stage_ms=0.010000 query_ms=0.002000
Descending 512 KiB own-allocation graph: queries_per_stage=9 (previously 16), exact_reads=16 bytes=128 stage_ms=0.020000 query_ms=0.004000; live benefit unmeasured.
Own-memory 32768-byte field + reread: 8192 scalar read requests vs 2 batched, 3.100 ms vs 0.200 ms per pair.
{"checks":12,"barriersPerBatch":10171,"batches":100,"oldLookups":3051300,"batchedLookups":100,"oldMs":40.000,"batchedMs":5.000}
'@
Require-Benchmark ($timing.cached_stage_ms -eq 0.4 -and $timing.metadata_batched_ms -eq 5) 'Isolated timing parser missed known stdout fields'

$faster = ConvertFrom-TaxiIsolatedTimingOutput 'Own-memory stage: 196 exact reads unchanged, VirtualQuery 196 -> 2, 1.000 -> 0.300 ms incl endpoint check.'
$isolated = Compare-TaxiIsolatedTimings $timing $faster
$cached = @($isolated.metrics | Where-Object { $_.name -eq 'cached_stage_ms' })[0]
Require-Benchmark ($cached.pct -lt 0) 'Isolated comparison did not treat a lower cached stage as an improvement'

$identity = Get-TaxiSourceHotPathIdentity -Repository $repoRoot -Baseline 'v0.9.8-build.38' -Candidate 'HEAD'
Require-Benchmark $identity.performanceOrientedRuntimeChanges 'performance-improvements should differ on hook/capture/composition paths versus 0.9.8'
Require-Benchmark (-not $identity.establishedFrameHotPathsUnchanged) 'Idle-bypass and list-cache files should count as established-frame hot-path changes'
$changed = @($identity.changed | ForEach-Object { $_.path })
$unchanged = @($identity.unchanged | ForEach-Object { $_.path })
foreach ($path in @('src/bridge/d3d12_bridge.cpp', 'src/bridge/bridge_main.cpp', 'src/graphics/scene_capture_manager.cpp', 'src/graphics/scene_runtime.cpp', 'src/app/companion.cpp')) {
    Require-Benchmark ($changed -contains $path) "Expected performance file missing from the 0.9.8 identity diff: $path"
}
foreach ($path in @('src/camera/local_memory.cpp', 'src/camera/body_pose_provider.cpp', 'src/graphics/scene_capture_d3d12.cpp', 'src/graphics/pfd_stamp_d3d12.cpp')) {
    Require-Benchmark ($unchanged -contains $path) "Unrelated hot path unexpectedly changed versus 0.9.8: $path"
}

$report = New-TaxiVersionBenchmarkReport -Repository $repoRoot -BaselineRef 'v0.9.8-build.38' -CandidateRef 'HEAD'
Require-Benchmark ($report.verdict.localBenefit -eq 'source-delta-present') 'Source-identity verdict should be source-delta-present versus 0.9.8'
Require-Benchmark ($report.verdict.liveSimulator -eq 'unmeasured') 'Report claimed a live simulator measurement'
$head = (& git -C $repoRoot rev-parse HEAD).Trim()
Require-Benchmark ($report.baseline.commit.StartsWith('f84be75bfb6a625ae11547beef2caf3b45ec2d59') -and $report.candidate.commit -eq $head) 'Resolved 0.9.8 or HEAD commit changed'

Write-Output "PASS: $checks version-benchmark parser and 0.9.8 source-identity checks."
