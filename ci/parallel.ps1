# Bounded native process pool for build.ps1. Works on Windows PowerShell 5.1 and PowerShell 7:
# each job's stdout and stderr are captured and printed as one block when it exits, so parallel
# test output never interleaves.

function ConvertTo-TaxiProcessArgument([string]$Value) {
    # Quote for the Windows C runtime command-line parser used by clang and the test executables.
    if ($Value.Length -and $Value -notmatch '[\s"]') { return $Value }
    $escaped = [regex]::Replace($Value, '(\\*)"', { param($match) ($match.Groups[1].Value * 2) + '\"' })
    return '"' + [regex]::Replace($escaped, '(\\+)$', '$1$1') + '"'
}

function New-TaxiJob([string]$Name, [string]$File, [object[]]$Arguments = @(), [string]$Failure, [int[]]$Ok = @(0)) {
    if (-not $Failure) { throw "Job $File needs a failure message." }
    # Array concatenation with an empty function result adds $null elements; they are not arguments.
    $values = @($Arguments | Where-Object { $null -ne $_ } | ForEach-Object { [string]$_ })
    if (-not $Name) { $Name = (@(Split-Path -Leaf $File) + $values) -join ' ' }
    return @{Name=$Name; File=$File; Arguments=$values; Failure=$Failure; Ok=$Ok; ExitCode=$null}
}

# Runs jobs at most $Throttle at a time and records ExitCode on each job. After the first failure no
# new job starts; running jobs finish, then every failure message is thrown together.
function Invoke-TaxiJobs([object[]]$Jobs, [int]$Throttle) {
    if ($Throttle -lt 1) { throw 'Job throttle must be at least 1.' }
    $pending = New-Object System.Collections.Generic.Queue[object]
    foreach ($job in $Jobs) { $pending.Enqueue($job) }
    $running = New-Object System.Collections.Generic.List[object]
    $failures = New-Object System.Collections.Generic.List[string]
    try {
        while ($running.Count -or ($pending.Count -and -not $failures.Count)) {
            while ($pending.Count -and -not $failures.Count -and $running.Count -lt $Throttle) {
                $job = $pending.Dequeue()
                $info = New-Object System.Diagnostics.ProcessStartInfo $job.File
                $info.Arguments = @($job.Arguments | ForEach-Object { ConvertTo-TaxiProcessArgument $_ }) -join ' '
                $info.UseShellExecute = $false
                $info.CreateNoWindow = $true
                $info.RedirectStandardOutput = $true
                $info.RedirectStandardError = $true
                try { $job.Process = [System.Diagnostics.Process]::Start($info) } catch {
                    $failures.Add("$($job.Failure) ($($_.Exception.Message))")
                    continue
                }
                $job.Output = $job.Process.StandardOutput.ReadToEndAsync()
                $job.Errors = $job.Process.StandardError.ReadToEndAsync()
                $job.Clock = [System.Diagnostics.Stopwatch]::StartNew()
                $running.Add($job)
            }
            if (-not $running.Count) { continue }
            Start-Sleep -Milliseconds 10
            foreach ($job in $running.ToArray()) {
                if (-not $job.Process.HasExited) { continue }
                $job.Process.WaitForExit()
                $job.ExitCode = $job.Process.ExitCode
                # A child process that inherited the pipes can hold them open after the job exits.
                $closed = $job.Output.Wait(30000) -and $job.Errors.Wait(30000)
                $passed = $job.Ok -contains $job.ExitCode
                if (-not $passed) {
                    Write-Output ("FAILED (exit {0}, {1:N1} s): {2}" -f $job.ExitCode, $job.Clock.Elapsed.TotalSeconds, $job.Name)
                    $failures.Add($job.Failure)
                }
                if ($closed) {
                    foreach ($text in @($job.Output.Result, $job.Errors.Result)) {
                        if ($text.Trim()) { Write-Output ($text.TrimEnd() -split '\r?\n') }
                    }
                } else { Write-Output "$($job.Name): output still held open by a child process after exit." }
                $job.Process.Dispose()
                [void]$running.Remove($job)
            }
        }
    } finally {
        foreach ($job in $running) {
            if (-not $job.Process.HasExited) { $job.Process.Kill() }
        }
    }
    if ($failures.Count) { throw ($failures -join [Environment]::NewLine) }
}
