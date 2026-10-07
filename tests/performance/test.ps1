#requires -Version 5.1
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repository 'ci/toolchain.ps1')
. (Join-Path $repository 'ci/version.ps1')
$toolchain = Get-TaxiToolchain $repository
$output = Join-Path $repository ('build/tests/performance/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $output -Force | Out-Null
$version = (Get-TaxiVersion $repository).Version
@('#pragma once', ('#define TAXI_CAM_VERSION "' + $version + '"')) |
    Set-Content -LiteralPath (Join-Path $output 'taxi-cam-version.hpp') -Encoding ASCII
$test = Join-Path $output 'performance-sampler-test.exe'
& (Join-Path $toolchain 'clang++.exe') '-std=c++20' '-O2' '-Wall' '-Wextra' '-Werror' '-static' '-municode' `
    '-DNOMINMAX' '-DWIN32_LEAN_AND_MEAN' '-D_WIN32_WINNT=0x0A00' '-mno-avx' '-mno-avx2' '-mno-avx512f' '-I' $output `
    (Join-Path $PSScriptRoot 'sampler_test.cpp') '-lbcrypt' '-o' $test
if ($LASTEXITCODE -ne 0) { throw 'Performance sampler self-test compilation failed.' }
& $test $output 2>&1 | Tee-Object -FilePath (Join-Path $output 'test.log')
if ($LASTEXITCODE -ne 0) { throw 'Performance sampler self-test failed.' }
$fixture = Get-Content -Raw -LiteralPath (Join-Path $output 'ipc-fixture.json') | ConvertFrom-Json
if ($fixture.settings.camera_rate -ne 10 -or $fixture.status.active_profile -ne 4 -or $fixture.status.aircraft_path -ne 'fixture-aircraft' -or
    $fixture.settings.mounts.Count -ne 3 -or $fixture.status.stage_elapsed_ms.Count -ne 10) { throw 'Structured IPC JSON output did not round-trip.' }
foreach ($script in @('tools/performance/build.ps1', 'tools/performance/capture.ps1', 'tests/performance/test.ps1')) {
    $tokens = $null; $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile((Join-Path $repository $script), [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw "PowerShell syntax failure: $script" }
}
[ordered]@{createdUtc=[DateTime]::UtcNow.ToString('o'); result='passed'; scope='Isolated process-owned fixtures only; no live simulator capture';
    binarySha256=(Get-FileHash -LiteralPath $test).Hash; sourceSha256=(Get-FileHash -LiteralPath (Join-Path $repository 'tools/performance/sampler.cpp')).Hash} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'validation.json') -Encoding UTF8
Write-Output "Performance sampler test evidence: $output"
