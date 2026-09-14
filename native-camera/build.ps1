$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$nativeRoot = Split-Path -Parent $PSScriptRoot
$compiler = Join-Path $nativeRoot 'build/deps/llvm-mingw/llvm-mingw-20260908-ucrt-x86_64/bin/clang++.exe'
$outputDirectory = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$common = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DNOMINMAX', '-D_WIN32_WINNT=0x0A00',
            '-mno-avx', '-mno-avx2', '-mno-avx512f', '-static')
foreach ($component in @('local_memory', 'code_contract', 'source_view', 'activation_mask', 'view_resize')) {
    $output = Join-Path $outputDirectory ($component + '-test.exe')
    $testSource = if ($component -eq 'local_memory') { 'local_memory.test.cpp' } else { $component + '_test.cpp' }
    & $compiler @common (Join-Path $PSScriptRoot ($component + '.cpp')) `
        (Join-Path $PSScriptRoot $testSource) '-o' $output
    if ($LASTEXITCODE -ne 0) { throw "Native camera component compilation failed: $component" }
    & $output
    if ($LASTEXITCODE -ne 0) { throw "Native camera component validation failed: $component" }
}
& (Join-Path $PSScriptRoot 'validate-abi.ps1')
$scheduleTest = Join-Path $outputDirectory 'render-schedule-test.exe'
$controlTest = Join-Path $outputDirectory 'control-policy-test.exe'
& $compiler @common (Join-Path $PSScriptRoot 'control_policy_test.cpp') `
    (Join-Path $nativeRoot 'src/scene_source_state.cpp') '-o' $controlTest
if ($LASTEXITCODE -ne 0) { throw 'Control policy test compilation failed.' }
& $controlTest
if ($LASTEXITCODE -ne 0) { throw 'Control policy test failed.' }
& $compiler @common (Join-Path $PSScriptRoot 'render_schedule_test.cpp') '-o' $scheduleTest
if ($LASTEXITCODE -ne 0) { throw 'Render schedule test compilation failed.' }
& $scheduleTest
if ($LASTEXITCODE -ne 0) { throw 'Render schedule test failed.' }
$idleTest = Join-Path $outputDirectory 'probe-inspection-gate-test.exe'
& $compiler @common (Join-Path $PSScriptRoot 'probe_inspection_gate_test.cpp') '-o' $idleTest
if ($LASTEXITCODE -ne 0) { throw 'Probe idle inspection gate compilation failed.' }
& $idleTest
if ($LASTEXITCODE -ne 0) { throw 'Probe idle inspection gate validation failed.' }
$recoveryTest = Join-Path $outputDirectory 'scene-recovery-test.exe'
& $compiler @common (Join-Path $PSScriptRoot 'scene_recovery_test.cpp') `
    (Join-Path $nativeRoot 'engine-camera/entry_pair.cpp') '-o' $recoveryTest
if ($LASTEXITCODE -ne 0) { throw 'Scene recovery test compilation failed.' }
& $recoveryTest
if ($LASTEXITCODE -ne 0) { throw 'Scene recovery test failed.' }
$retainedResizeTest = Join-Path $outputDirectory 'view-resize-recovery-test.exe'
& $compiler @common (Join-Path $PSScriptRoot 'view_resize_recovery_test.cpp') `
    (Join-Path $nativeRoot 'engine-camera/entry_pair.cpp') '-o' $retainedResizeTest
if ($LASTEXITCODE -ne 0) { throw 'Retained resolution recovery compilation failed.' }
& $retainedResizeTest
if ($LASTEXITCODE -ne 0) { throw 'Retained resolution recovery validation failed.' }
$retainedProfileTest = Join-Path $outputDirectory 'retained-profile-test.exe'
& $compiler @common (Join-Path $PSScriptRoot 'retained_profile_test.cpp') `
    (Join-Path $nativeRoot 'engine-camera/entry_pair.cpp') '-o' $retainedProfileTest
if ($LASTEXITCODE -ne 0) { throw 'Retained profile transition compilation failed.' }
& $retainedProfileTest
if ($LASTEXITCODE -ne 0) { throw 'Retained profile transition validation failed.' }
$mountTest = Join-Path $outputDirectory 'aircraft-mounts-test.exe'
& $compiler @common (Join-Path $PSScriptRoot 'aircraft_mounts_test.cpp') '-o' $mountTest
if ($LASTEXITCODE -ne 0) { throw 'Aircraft mount test compilation failed.' }
& $mountTest
if ($LASTEXITCODE -ne 0) { throw 'Aircraft mount test failed.' }
$mountConfigTest = Join-Path $outputDirectory 'mount-config-test.exe'
& $compiler @common '-fno-exceptions' '-fno-rtti' (Join-Path $PSScriptRoot 'mount_config.cpp') `
    (Join-Path $PSScriptRoot 'mount_config_test.cpp') '-o' $mountConfigTest
if ($LASTEXITCODE -ne 0) { throw 'Mount configuration test compilation failed.' }
& $mountConfigTest
if ($LASTEXITCODE -ne 0) { throw 'Mount configuration test failed.' }
$bodyProviderTest = Join-Path $outputDirectory 'body-pose-provider-test.exe'
& $compiler @common '-fno-exceptions' '-fno-rtti' '-DTAXI_BODY_POSE_PROVIDER_TESTING' `
    (Join-Path $PSScriptRoot 'body_pose_provider.cpp') (Join-Path $PSScriptRoot 'body_pose_provider_test.cpp') '-o' $bodyProviderTest
if ($LASTEXITCODE -ne 0) { throw 'SimConnect telemetry test compilation failed.' }
& $bodyProviderTest --self-test
if ($LASTEXITCODE -ne 0) { throw 'SimConnect telemetry test failed.' }
$bodyMathTest = Join-Path $outputDirectory 'body-pose-math-test.exe'
& $compiler @common (Join-Path $PSScriptRoot 'body_pose_math_test.cpp') '-o' $bodyMathTest
if ($LASTEXITCODE -ne 0) { throw 'Aircraft body pose math test compilation failed.' }
& $bodyMathTest
if ($LASTEXITCODE -ne 0) { throw 'Aircraft body pose math test failed.' }
$runtimeSources = @(
    'probe.cpp', 'body_pose_provider.cpp', 'local_memory.cpp', 'code_contract.cpp', 'source_view.cpp', 'activation_mask.cpp', 'verified_profile.cpp', 'view_resize.cpp',
    '../discovery/aircraft_inventory.cpp', '../src/scene_handoff.cpp', '../engine-camera/entry_pair.cpp',
    '../engine-camera/owned_entry_inventory.cpp', '../engine-camera/owned_view.cpp', '../engine-camera/view_pool.cpp'
) | ForEach-Object { Join-Path $PSScriptRoot $_ }
$runtimeTest = Join-Path $outputDirectory 'runtime-test.exe'
& $compiler @common (Join-Path $PSScriptRoot 'runtime_test.cpp') @runtimeSources `
    (Join-Path $nativeRoot 'engine-hook/build/engine-hook.a') '-o' $runtimeTest
if ($LASTEXITCODE -ne 0) { throw 'Native runtime integration test compilation failed.' }
& $runtimeTest
if ($LASTEXITCODE -ne 0) { throw 'Native runtime wrong-host guard failed.' }
