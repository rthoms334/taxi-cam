# Shared helpers for comparing Taxi Cam revisions. Timings from own-process
# tests or parsed bridge logs are not live MSFS frame-time evidence.

Set-StrictMode -Version Latest

function Test-TaxiWindowsHost {
    return $env:OS -eq 'Windows_NT'
}

function Get-TaxiBenchmarkHotPaths {
    @(
        [ordered]@{ Path = 'src/camera/local_memory.cpp'; Band = 'observer-memory' }
        [ordered]@{ Path = 'src/camera/local_memory.hpp'; Band = 'observer-memory' }
        [ordered]@{ Path = 'src/camera/probe.cpp'; Band = 'observer' }
        [ordered]@{ Path = 'src/camera/probe.hpp'; Band = 'observer' }
        [ordered]@{ Path = 'src/camera/render_schedule.hpp'; Band = 'observer' }
        [ordered]@{ Path = 'src/camera/still_frame_hold.hpp'; Band = 'observer' }
        [ordered]@{ Path = 'src/camera/body_pose_provider.cpp'; Band = 'telemetry' }
        [ordered]@{ Path = 'src/app/companion.cpp'; Band = 'companion' }
        [ordered]@{ Path = 'src/bridge/bridge_main.cpp'; Band = 'bridge-loop' }
        [ordered]@{ Path = 'src/bridge/d3d12_bridge.cpp'; Band = 'hooks' }
        [ordered]@{ Path = 'src/bridge/d3d12_bridge.hpp'; Band = 'hooks' }
        [ordered]@{ Path = 'src/graphics/metadata_batch_cache.hpp'; Band = 'hooks' }
        [ordered]@{ Path = 'src/graphics/scene_runtime.cpp'; Band = 'composition' }
        [ordered]@{ Path = 'src/graphics/scene_capture_manager.cpp'; Band = 'capture' }
        [ordered]@{ Path = 'src/graphics/scene_capture_d3d12.cpp'; Band = 'capture' }
        [ordered]@{ Path = 'src/graphics/camera_compositor_d3d12.hpp'; Band = 'composition' }
        [ordered]@{ Path = 'src/graphics/pfd_stamp_d3d12.cpp'; Band = 'pfd' }
        [ordered]@{ Path = 'src/graphics/scene_handoff.cpp'; Band = 'handoff' }
    )
}

function Resolve-TaxiGitCommit {
    param(
        [Parameter(Mandatory)][string]$Repository,
        [Parameter(Mandatory)][string]$Revision
    )
    $hash = & git -C $Repository rev-parse $Revision
    if ($LASTEXITCODE -ne 0) { throw "Could not resolve Git revision: $Revision" }
    $short = & git -C $Repository rev-parse --short $Revision
    $subject = & git -C $Repository log -1 --format=%s $Revision
    [ordered]@{
        revision = $Revision
        commit   = $hash.Trim()
        short    = $short.Trim()
        subject  = $subject.Trim()
    }
}

function Get-TaxiSourceHotPathIdentity {
    param(
        [Parameter(Mandatory)][string]$Repository,
        [Parameter(Mandatory)][string]$Baseline,
        [Parameter(Mandatory)][string]$Candidate
    )
    $changed = @()
    $unchanged = @()
    foreach ($entry in Get-TaxiBenchmarkHotPaths) {
        $stat = & git -C $Repository diff --numstat $Baseline $Candidate -- $entry.Path
        if ($LASTEXITCODE -ne 0) { throw "Could not diff $($entry.Path) between $Baseline and $Candidate" }
        $line = @($stat | Where-Object { $_ })
        if ($line.Count -eq 0) {
            $unchanged += [ordered]@{ path = $entry.Path; band = $entry.Band }
            continue
        }
        $parts = ($line[0] -split '\s+')
        $changed += [ordered]@{
            path      = $entry.Path
            band      = $entry.Band
            additions = [int]$parts[0]
            deletions = [int]$parts[1]
        }
    }
    $runtimeChanged = @($changed | Where-Object { $_.band -in @('observer-memory', 'hooks', 'composition', 'capture', 'pfd') })
    [ordered]@{
        unchanged                            = $unchanged
        changed                              = $changed
        performanceOrientedRuntimeChanges    = [bool]$runtimeChanged
        establishedFrameHotPathsUnchanged    = (@($changed | Where-Object { $_.band -in @('observer-memory', 'hooks', 'composition', 'capture', 'pfd', 'telemetry') }).Count -eq 0)
    }
}

function Get-TaxiStatistic {
    param([double[]]$Values)
    $present = @($Values | Where-Object { $null -ne $_ })
    if ($present.Count -eq 0) { return $null }
    $sorted = @($present | Sort-Object)
    $n = $sorted.Count
    $median = if ($n % 2) { [double]$sorted[($n - 1) / 2] } else { ([double]$sorted[$n / 2 - 1] + [double]$sorted[$n / 2]) / 2 }
    $p95Index = [Math]::Min($n - 1, [int][Math]::Floor(0.95 * ($n - 1)))
    [ordered]@{
        count  = $n
        min    = [double]$sorted[0]
        median = $median
        mean   = [double]($sorted | Measure-Object -Average).Average
        p95    = [double]$sorted[$p95Index]
        max    = [double]$sorted[$n - 1]
    }
}

function ConvertFrom-TaxiBridgeLog {
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Bridge log not found: $Path" }
    $samples = @()
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -notmatch 'probe_ms=') { continue }
        $sample = [ordered]@{}
        if ($line -match '^(\d+)') { $sample.tick_ms = [uint64]$Matches[1] }
        foreach ($name in @('native', 'scene', 'mask', 'captured', 'composed', 'stamps', 'output', 'queries', 'inspections', 'updates')) {
            if ($line -match "(?:^|\s)$name=(\d+)") { $sample[$name] = [int64]$Matches[1] }
        }
        foreach ($name in @('probe_ms', 'query_ms', 'read_ms', 'aa_ms')) {
            if ($line -match "$name=([0-9]+(?:\.[0-9]+)?)") { $sample[$name] = [double]$Matches[1] }
        }
        if ($sample.Contains('probe_ms')) { $samples += [pscustomobject]$sample }
    }
    return $samples
}

function Get-TaxiBridgeLogSummary {
    param(
        [Parameter(Mandatory)]$Samples,
        [string]$Label = 'log'
    )
    $rows = @($Samples)
    $rate = $null
    if ($rows.Count -ge 2 -and $null -ne $rows[0].tick_ms -and $null -ne $rows[-1].tick_ms -and $rows[-1].tick_ms -gt $rows[0].tick_ms) {
        $seconds = ([double]$rows[-1].tick_ms - [double]$rows[0].tick_ms) / 1000.0
        $rate = [ordered]@{
            window_s      = $seconds
            captures_per_s = ([double]$rows[-1].captured - [double]$rows[0].captured) / $seconds
            composed_per_s = ([double]$rows[-1].composed - [double]$rows[0].composed) / $seconds
            stamps_per_s   = ([double]$rows[-1].stamps - [double]$rows[0].stamps) / $seconds
        }
    }
    [ordered]@{
        label       = $Label
        samples     = $rows.Count
        probe_ms    = Get-TaxiStatistic @($rows | ForEach-Object { $_.probe_ms })
        query_ms    = Get-TaxiStatistic @($rows | ForEach-Object { $_.query_ms })
        read_ms     = Get-TaxiStatistic @($rows | ForEach-Object { $_.read_ms })
        aa_ms       = Get-TaxiStatistic @($rows | ForEach-Object { $_.aa_ms })
        queries     = Get-TaxiStatistic @($rows | ForEach-Object { [double]$_.queries })
        inspections = Get-TaxiStatistic @($rows | ForEach-Object { [double]$_.inspections })
        throughput  = $rate
    }
}

function Compare-TaxiStatistic {
    param($Baseline, $Candidate)
    if ($null -eq $Baseline -or $null -eq $Candidate -or $Baseline.median -eq 0) {
        return [ordered]@{ baseline = $Baseline; candidate = $Candidate; median_delta = $null; median_pct = $null }
    }
    $delta = [double]$Candidate.median - [double]$Baseline.median
    [ordered]@{
        baseline     = $Baseline
        candidate    = $Candidate
        median_delta = $delta
        median_pct   = ($delta / [double]$Baseline.median) * 100.0
    }
}

function Compare-TaxiBridgeLogSummaries {
    param(
        [Parameter(Mandatory)]$Baseline,
        [Parameter(Mandatory)]$Candidate
    )
    $throughput = $null
    if ($null -ne $Baseline.throughput -and $null -ne $Candidate.throughput -and $Baseline.throughput.composed_per_s -ne 0) {
        $throughput = [ordered]@{
            baseline_composed_per_s  = $Baseline.throughput.composed_per_s
            candidate_composed_per_s = $Candidate.throughput.composed_per_s
            composed_pct             = (($Candidate.throughput.composed_per_s - $Baseline.throughput.composed_per_s) / $Baseline.throughput.composed_per_s) * 100.0
        }
    }
    [ordered]@{
        probe_ms    = Compare-TaxiStatistic $Baseline.probe_ms $Candidate.probe_ms
        query_ms    = Compare-TaxiStatistic $Baseline.query_ms $Candidate.query_ms
        read_ms     = Compare-TaxiStatistic $Baseline.read_ms $Candidate.read_ms
        aa_ms       = Compare-TaxiStatistic $Baseline.aa_ms $Candidate.aa_ms
        throughput  = $throughput
        note        = 'Last-callback timings and counter deltas from bridge.log; not displayed FPS or GPU occupancy.'
    }
}

function ConvertFrom-TaxiIsolatedTimingOutput {
    param([Parameter(Mandatory)][string]$Text)
    $timings = [ordered]@{}
    if ($Text -match 'VirtualQuery_ms=([0-9.]+)\s+VirtualQueryEx_ms=([0-9.]+)') {
        $timings.virtual_query_ms = [double]$Matches[1]
        $timings.virtual_query_ex_ms = [double]$Matches[2]
    }
    if ($Text -match 'VirtualQuery 196 -> 2,\s+([0-9.]+) -> ([0-9.]+) ms') {
        $timings.uncached_stage_ms = [double]$Matches[1]
        $timings.cached_stage_ms = [double]$Matches[2]
    }
    if ($Text -match 'Descending own-allocation graph:.*?stage_ms=([0-9.]+)\s+query_ms=([0-9.]+)') {
        $timings.descending_stage_ms = [double]$Matches[1]
        $timings.descending_query_ms = [double]$Matches[2]
    }
    if ($Text -match 'Descending 512 KiB own-allocation graph:.*?stage_ms=([0-9.]+)\s+query_ms=([0-9.]+)') {
        $timings.multiwindow_stage_ms = [double]$Matches[1]
        $timings.multiwindow_query_ms = [double]$Matches[2]
    }
    if ($Text -match '8192 scalar read requests vs 2 batched,\s+([0-9.]+) ms vs ([0-9.]+) ms') {
        $timings.scalar_read_ms = [double]$Matches[1]
        $timings.batched_read_ms = [double]$Matches[2]
    }
    if ($Text -match '"oldMs":([0-9.]+),"batchedMs":([0-9.]+)') {
        $timings.metadata_old_ms = [double]$Matches[1]
        $timings.metadata_batched_ms = [double]$Matches[2]
    }
    return [pscustomobject]$timings
}

function Get-TaxiNoteProperty {
    param($Object, [string]$Name)
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Compare-TaxiIsolatedTimings {
    param($Baseline, $Candidate)
    $keys = @(
        @($Baseline.PSObject.Properties.Name) + @($Candidate.PSObject.Properties.Name) |
            Select-Object -Unique
    )
    $rows = @()
    foreach ($key in $keys) {
        $left = Get-TaxiNoteProperty $Baseline $key
        $right = Get-TaxiNoteProperty $Candidate $key
        $pct = $null
        if ($null -ne $left -and $null -ne $right -and [double]$left -ne 0) {
            $pct = (([double]$right - [double]$left) / [double]$left) * 100.0
        }
        $rows += [ordered]@{ name = $key; baseline = $left; candidate = $right; pct = $pct }
    }
    [ordered]@{
        metrics = $rows
        note    = 'Own-process microbenchmarks. Query-count contracts are the regression; wall times are diagnostic only.'
    }
}

function Invoke-TaxiIsolatedRevision {
    param(
        [Parameter(Mandatory)][string]$Repository,
        [Parameter(Mandatory)][string]$Revision,
        [Parameter(Mandatory)][string]$OutputDirectory
    )
    if (-not (Test-TaxiWindowsHost)) { throw 'Isolated native timings require Windows and the pinned LLVM-MinGW compiler.' }
    . (Join-Path $Repository 'ci/toolchain.ps1')
    $compiler = Join-Path (Get-TaxiToolchain $Repository) 'clang++.exe'
    $tree = Join-Path $OutputDirectory 'tree'
    if (Test-Path -LiteralPath $tree) {
        & git -C $Repository worktree remove --force $tree
        if ($LASTEXITCODE -ne 0) { throw "Could not replace worktree $tree" }
    }
    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    & git -C $Repository worktree add --detach $tree $Revision
    if ($LASTEXITCODE -ne 0) { throw "Could not create worktree for $Revision" }
    $common = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DNOMINMAX', '-D_WIN32_WINNT=0x0A00',
        '-mno-avx', '-mno-avx2', '-mno-avx512f', '-static')
    $memoryExe = Join-Path $OutputDirectory 'local-memory-timing.exe'
    & $compiler @common (Join-Path $tree 'src/camera/local_memory.cpp') (Join-Path $tree 'tests/camera/local_memory_test.cpp') '-o' $memoryExe
    if ($LASTEXITCODE -ne 0) { throw "local_memory_test compilation failed for $Revision" }
    $memoryOut = & $memoryExe
    if ($LASTEXITCODE -ne 0) { throw "local_memory_test failed for $Revision" }
    $cacheExe = Join-Path $OutputDirectory 'metadata-batch-timing.exe'
    & $compiler @common (Join-Path $tree 'tests/graphics/metadata_batch_cache_test.cpp') '-o' $cacheExe
    if ($LASTEXITCODE -ne 0) { throw "metadata_batch_cache_test compilation failed for $Revision" }
    $cacheOut = & $cacheExe
    if ($LASTEXITCODE -ne 0) { throw "metadata_batch_cache_test failed for $Revision" }
    $text = (@($memoryOut) + @($cacheOut)) -join "`n"
    Set-Content -LiteralPath (Join-Path $OutputDirectory 'timing-stdout.txt') -Value $text -Encoding utf8
    ConvertFrom-TaxiIsolatedTimingOutput $text
}

function New-TaxiVersionBenchmarkReport {
    param(
        [Parameter(Mandatory)][string]$Repository,
        [Parameter(Mandatory)][string]$BaselineRef,
        [Parameter(Mandatory)][string]$CandidateRef,
        $BridgeLogs,
        $IsolatedTimings,
        [string]$HostName = [Environment]::MachineName
    )
    $baseline = Resolve-TaxiGitCommit -Repository $Repository -Revision $BaselineRef
    $candidate = Resolve-TaxiGitCommit -Repository $Repository -Revision $CandidateRef
    $identity = Get-TaxiSourceHotPathIdentity -Repository $Repository -Baseline $BaselineRef -Candidate $CandidateRef
    $verdict = if ($identity.establishedFrameHotPathsUnchanged -and -not $identity.performanceOrientedRuntimeChanges) {
        'none-expected'
    } else {
        'source-delta-present'
    }
    [ordered]@{
        generatedUtc = [DateTime]::UtcNow.ToString('o')
        host         = $HostName
        windows      = Test-TaxiWindowsHost
        baseline     = $baseline
        candidate    = $candidate
        source       = $identity
        bridgeLogs   = $BridgeLogs
        isolated     = $IsolatedTimings
        verdict      = [ordered]@{
            localBenefit   = $verdict
            liveSimulator  = 'unmeasured'
            summary        = if ($verdict -eq 'none-expected') {
                'Established-frame hot paths are identical. This revision has no expected FPS or observer-time benefit versus the baseline.'
            } else {
                'Hot-path files differ. Isolated timings and live Diagnostics are required before claiming a benefit.'
            }
        }
    }
}
