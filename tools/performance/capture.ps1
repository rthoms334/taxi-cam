#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateRange(1,2147483647)][int]$SimulatorProcessId,
    [Parameter(Mandatory=$true)][ValidateRange(1,4)][int]$ExpectedProfile,
    [Parameter(Mandatory=$true)][ValidateRange(0,3)][int]$ExpectedMask,
    [Parameter(Mandatory=$true)][ValidatePattern('^[a-zA-Z0-9_-]{1,64}$')][string]$Phase,
    [ValidateRange(1,120)][int]$Seconds = 30,
    [ValidateRange(100,1000)][int]$IntervalMilliseconds = 250,
    [ValidateRange(0,30)][int]$DelaySeconds = 5,
    [string]$OutputDirectory,
    [string]$PresentMonCsv,
    [switch]$AllowUnloaded
)
if ($AllowUnloaded -and $ExpectedMask -ne 0) { throw '-AllowUnloaded requires -ExpectedMask 0.' }
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$sampler = Join-Path $repository 'build/tools/performance/taxi-performance-sampler.exe'
$buildReceipt = Join-Path $repository 'build/tools/performance/build.json'
if (-not (Test-Path -LiteralPath $sampler -PathType Leaf) -or -not (Test-Path -LiteralPath $buildReceipt -PathType Leaf)) {
    throw 'Build the helper first: ./tools/performance/build.ps1 -Test'
}
$receipt = Get-Content -Raw -LiteralPath $buildReceipt | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $sampler).Hash -ne $receipt.executableSha256) { throw 'Sampler differs from its build receipt.' }
foreach ($source in $receipt.sources) {
    if ((Get-FileHash -LiteralPath (Join-Path $repository $source.path)).Hash -ne $source.sha256) {
        throw 'Sampler source/protocol changed. Rebuild the helper before capturing.'
    }
}
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repository ('build/performance-captures/' + $Phase + '-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'OutputDirectory must be new; previous evidence is never overwritten.' }
if ($PresentMonCsv -and -not (Test-Path -LiteralPath $PresentMonCsv -PathType Leaf)) { throw 'PresentMonCsv must name an existing external capture.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
Copy-Item -LiteralPath $buildReceipt -Destination (Join-Path $OutputDirectory 'sampler-build.json')
Write-Output "Capture starts in $DelaySeconds seconds. Keep the simulator foreground, with the same aircraft, cockpit view and scene. Output: $OutputDirectory"
if ($DelaySeconds) { Start-Sleep -Seconds $DelaySeconds }
& $sampler '--pid' $SimulatorProcessId '--profile' $ExpectedProfile '--mask' $ExpectedMask '--phase' $Phase `
    '--duration-ms' ($Seconds * 1000) '--interval-ms' $IntervalMilliseconds '--output' $OutputDirectory `
    '--allow-unloaded' $(if ($AllowUnloaded) { 1 } else { 0 })
$result = $LASTEXITCODE
$external = $null
if ($PresentMonCsv) {
    $before = (Get-FileHash -LiteralPath $PresentMonCsv -Algorithm SHA256).Hash
    $destination = Join-Path $OutputDirectory 'presentmon.csv'
    Copy-Item -LiteralPath $PresentMonCsv -Destination $destination
    $copied = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
    $after = (Get-FileHash -LiteralPath $PresentMonCsv -Algorithm SHA256).Hash
    $external = [ordered]@{file='presentmon.csv'; sha256=$copied; unchangedDuringCopy=($before -eq $copied -and $after -eq $copied);
        scope='External data only; PID, time overlap, swapchain selection, dropped events and frame interpretation must be checked separately.'}
}
[ordered]@{createdUtc=[DateTime]::UtcNow.ToString('o'); samplerExitCode=$result; simulatorPid=$SimulatorProcessId;
    phase=$Phase; requestedSeconds=$Seconds; expectedProfile=$ExpectedProfile; expectedMask=$ExpectedMask; unloaded=[bool]$AllowUnloaded;
    externalPresentMon=$external;
    liveSceneComparability='Requires operator confirmation; no screenshot or IPC field establishes identical scene/GPU load.'} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'capture.json') -Encoding UTF8
if ($result -ne 0) { throw "Capture refused or invalid (exit $result). Preserve evidence; inspect summary.json when present." }
