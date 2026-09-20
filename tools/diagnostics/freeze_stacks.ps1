#requires -Version 5.1
# Captures every thread's state and call stack from a frozen MSFS 2024 process,
# symbolises the graphics-bridge frames from the DLL's own symbol table and
# copies the current log tails next to the capture. Run it while the simulator
# is frozen, before killing it. Nothing is written into the simulator.
[CmdletBinding()]
param(
    [int]$SimulatorProcessId = 0,
    [string]$OutputDirectory,
    [switch]$AllowAnyImage  # self-test only: capture a process other than FlightSimulator2024.exe
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repository 'ci/toolchain.ps1')
$toolchain = Get-TaxiToolchain $repository
$tool = Join-Path $repository 'build/tools/diagnostics/freeze-stacks.exe'
$source = Join-Path $repository 'tools/diagnostics/freeze_stacks.cpp'
if (-not (Test-Path -LiteralPath $tool) -or (Get-Item -LiteralPath $tool).LastWriteTimeUtc -lt (Get-Item -LiteralPath $source).LastWriteTimeUtc) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $tool) | Out-Null
    & (Join-Path $toolchain 'clang++.exe') '-std=c++20' '-O2' '-Wall' '-Wextra' '-Werror' '-fms-extensions' '-static' '-municode' `
        '-DNOMINMAX' '-D_WIN32_WINNT=0x0A00' '-mno-avx' '-mno-avx2' $source '-ldbghelp' '-lpsapi' '-o' $tool
    if ($LASTEXITCODE -ne 0) { throw 'freeze-stacks build failed.' }
}
if (-not $SimulatorProcessId) {
    $candidates = @(Get-Process -Name FlightSimulator2024 -ErrorAction SilentlyContinue)
    if ($candidates.Count -ne 1) { throw "Expected exactly one FlightSimulator2024 process, found $($candidates.Count). Pass -SimulatorProcessId." }
    $SimulatorProcessId = $candidates[0].Id
}
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repository ('build/diagnostics/freeze-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$raw = Join-Path $OutputDirectory 'stacks.txt'
Write-Output "Capturing thread stacks of PID $SimulatorProcessId ..."
$arguments = @('--pid', $SimulatorProcessId)
if ($AllowAnyImage) { $arguments += '--allow-any-image' }
$lines = @(& $tool @arguments)
$exit = $LASTEXITCODE
[IO.File]::WriteAllLines($raw, $lines)
if ($exit -ne 0) { throw "freeze-stacks failed with exit code $exit; see $raw" }

# Symbolise bridge frames from the loaded DLL's COFF symbol table.
$moduleLine = $lines | Where-Object { $_ -like 'module taxi-camera-bridge.dll *' } | Select-Object -First 1
$symbolised = @()
if ($moduleLine -and $moduleLine -match ' path (.+)$') {
    $dllPath = $Matches[1]
    $nm = & (Join-Path $toolchain 'llvm-nm.exe') '--numeric-sort' $dllPath 2>$null
    $symbols = New-Object System.Collections.Generic.List[object]
    foreach ($entry in $nm) {
        if ($entry -match '^([0-9a-f]+) [tT] (.+)$') {
            $symbols.Add([pscustomobject]@{ Address = [uint64]::Parse($Matches[1], 'AllowHexSpecifier'); Name = $Matches[2] })
        }
    }
    $imageBase = [uint64]0x180000000
    $hash = (Get-FileHash -LiteralPath $dllPath).Hash
    $built = Join-Path $repository 'build/native/taxi-camera-bridge.dll'
    $sameAsBuild = (Test-Path -LiteralPath $built) -and ((Get-FileHash -LiteralPath $built).Hash -eq $hash)
    $symbolised += "bridge $dllPath sha256 $hash same_as_local_build $sameAsBuild symbols $($symbols.Count)"
    $cxxfilt = Join-Path $toolchain 'llvm-cxxfilt.exe'
    foreach ($line in $lines) {
        if ($line -match '^(\s*\d+\s+)taxi-camera-bridge\.dll\+0x([0-9a-f]+)$') {
            $rva = [uint64]::Parse($Matches[2], 'AllowHexSpecifier')
            $target = $imageBase + $rva
            $best = $null
            foreach ($symbol in $symbols) { if ($symbol.Address -le $target) { $best = $symbol } else { break } }
            if ($best) {
                $name = (& $cxxfilt $best.Name 2>$null | Select-Object -First 1)
                if (-not $name) { $name = $best.Name }
                $symbolised += ($Matches[1] + 'taxi-camera-bridge.dll!' + $name + '+0x' + ('{0:x}' -f ($target - $best.Address)) + '  [rva 0x' + ('{0:x}' -f $rva) + ']')
                continue
            }
        }
        $symbolised += $line
    }
} else {
    $symbolised += '(taxi-camera-bridge.dll is not loaded in this process)'
    $symbolised += $lines
}
[IO.File]::WriteAllLines((Join-Path $OutputDirectory 'stacks-symbolized.txt'), $symbolised)
$settings = Join-Path $env:LOCALAPPDATA 'Taxi Cam'
foreach ($log in @('bridge.log', 'launcher.log')) {
    $path = Join-Path $settings $log
    if (Test-Path -LiteralPath $path) { Get-Content -LiteralPath $path -Tail 400 | Set-Content -LiteralPath (Join-Path $OutputDirectory ($log + '.tail')) -Encoding utf8 }
}
$bridgeThreads = @($symbolised | Where-Object { $_ -like '*taxi-camera-bridge.dll!*' }).Count
Write-Output "Capture written to $OutputDirectory ($bridgeThreads bridge frames symbolised)."
