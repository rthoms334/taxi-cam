#requires -Version 5.1
[CmdletBinding()]
param([switch]$Test)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repository 'ci/toolchain.ps1')
. (Join-Path $repository 'ci/version.ps1')
$toolchain = Get-TaxiToolchain $repository
$output = Join-Path $repository 'build/tools/performance'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$version = (Get-TaxiVersion $repository).Version
@('#pragma once', ('#define TAXI_CAM_VERSION "' + $version + '"')) |
    Set-Content -LiteralPath (Join-Path $output 'taxi-cam-version.hpp') -Encoding ASCII
$common = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-static', '-municode',
    '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-D_WIN32_WINNT=0x0A00', '-mno-avx', '-mno-avx2', '-mno-avx512f', '-I', $output)
$executable = Join-Path $output 'taxi-performance-sampler.exe'
& (Join-Path $toolchain 'clang++.exe') @common (Join-Path $PSScriptRoot 'sampler.cpp') '-lbcrypt' '-o' $executable
if ($LASTEXITCODE -ne 0) { throw 'Performance sampler compilation failed.' }
$poller = Join-Path $output 'ipc-poll.exe'
& (Join-Path $toolchain 'clang++.exe') @common (Join-Path $PSScriptRoot 'ipc-poll.cpp') '-o' $poller
if ($LASTEXITCODE -ne 0) { throw 'IPC poller compilation failed.' }
if ($Test) {
    & (Join-Path $repository 'tests/performance/test.ps1')
}
$sources = @('tools/performance/sampler.cpp', 'src/shared/protocol.hpp', 'src/shared/sim_messages.hpp', 'src/shared/camera_rate.hpp', 'src/shared/exposure_settings.hpp', 'src/profiles/catalog.hpp', 'src/shared/version.hpp', 'changelog.json')
$manifest = @($sources | ForEach-Object {
    [ordered]@{path=$_; sha256=(Get-FileHash -LiteralPath (Join-Path $repository $_) -Algorithm SHA256).Hash}
})
[ordered]@{createdUtc=[DateTime]::UtcNow.ToString('o'); compiler=(Get-FileHash -LiteralPath (Join-Path $toolchain 'clang++.exe')).Hash;
    executableSha256=(Get-FileHash -LiteralPath $executable).Hash; selfTestsRun=[bool]$Test; sources=$manifest} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'build.json') -Encoding UTF8
Write-Output "Sampler ready: $executable"
