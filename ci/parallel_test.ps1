$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'parallel.ps1')
$checks = 0
function Assert-That([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:checks++
}
foreach ($case in @(
    @('abc', 'abc'), @('', '""'), @('a b', '"a b"'), @('a"b', '"a\"b"'), @('a\"b', '"a\\\"b"'),
    @('C:\with space\', '"C:\with space\\"'), @('C:\no\space\', 'C:\no\space\'), @("tab`there", "`"tab`there`""))) {
    $actual = ConvertTo-TaxiProcessArgument $case[0]
    Assert-That ($actual -ceq $case[1]) "Argument [$($case[0])] quoted as [$actual], expected [$($case[1])]."
}
$job = New-TaxiJob -File 'C:\tools\probe.exe' -Arguments (@('--warp') + $null + @()) -Failure 'probe failed.'
Assert-That ($job.Arguments.Count -eq 1 -and $job.Arguments[0] -ceq '--warp') 'Null elements became process arguments.'
Assert-That ($job.Name -ceq 'probe.exe --warp') "Default job name was [$($job.Name)]."
$cmd = Join-Path $env:SystemRoot 'System32\cmd.exe'
function New-ExitJob([string]$Name, [int]$Code, [int[]]$Ok = @(0)) {
    New-TaxiJob -Name $Name -File $cmd -Arguments @('/d', '/c', "echo $Name& exit $Code") -Failure "$Name failed." -Ok $Ok
}
# Passing jobs: exit codes recorded, accepted non-zero codes pass, output kept per job.
$jobs = @((New-ExitJob 'first' 0), (New-ExitJob 'skipped' 77 @(0, 77)), (New-ExitJob 'third' 0))
$output = @(Invoke-TaxiJobs $jobs 2)
Assert-That ($jobs[0].ExitCode -eq 0 -and $jobs[1].ExitCode -eq 77 -and $jobs[2].ExitCode -eq 0) 'Exit codes were not recorded.'
Assert-That (($output -join ',') -match 'first' -and ($output -join ',') -match 'skipped' -and ($output -join ',') -match 'third') 'Job output was not printed.'
Assert-That (-not ($output -match 'FAILED')) 'Accepted exit code was reported as a failure.'
# A failure stops new jobs, lets running ones finish and throws every collected message.
$jobs = @((New-ExitJob 'broken' 3), (New-ExitJob 'never' 0))
$message = $null
try { $output = @(Invoke-TaxiJobs $jobs 1) } catch { $message = $_.Exception.Message }
Assert-That ($message -ceq 'broken failed.') "Failure message was [$message]."
Assert-That ($jobs[0].ExitCode -eq 3 -and $null -eq $jobs[1].ExitCode) 'A job started after an earlier failure.'
$jobs = @((New-ExitJob 'left' 4), (New-ExitJob 'right' 5))
$message = $null
try { $output = @(Invoke-TaxiJobs $jobs 2) } catch { $message = $_.Exception.Message }
Assert-That ($message -match 'left failed\.' -and $message -match 'right failed\.') "Concurrent failures were not all reported: [$message]."
# A missing executable is a job failure, not a crash of the pool.
$message = $null
try { Invoke-TaxiJobs @(New-TaxiJob -Name 'missing' -File (Join-Path $env:TEMP ([Guid]::NewGuid().ToString('N') + '.exe')) -Failure 'missing failed.') 1 | Out-Null } catch { $message = $_.Exception.Message }
Assert-That ($message -like 'missing failed.*') "Missing executable was reported as [$message]."
$message = $null
try { Invoke-TaxiJobs @() 0 } catch { $message = $_.Exception.Message }
Assert-That ($null -ne $message) 'A zero throttle was accepted.'
Write-Output "PASS parallel job runner: $checks quoting, exit-code, output, fail-fast and missing-executable checks."
