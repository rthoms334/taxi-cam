$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repoRoot 'ci/toolchain.ps1')
Set-StrictMode -Version Latest
$nativeRoot = $repoRoot
$compiler = Join-Path (Get-TaxiToolchain $repoRoot) 'clang++.exe'
$outputDirectory = Join-Path $repoRoot 'build/tests/camera'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$common = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DNOMINMAX', '-D_WIN32_WINNT=0x0A00',
            '-mno-avx', '-mno-avx2', '-mno-avx512f', '-static')
foreach ($component in @('local_memory', 'code_contract', 'source_view', 'activation_mask', 'view_resize', 'view_aa')) {
    $output = Join-Path $outputDirectory ($component + '-test.exe')
    $testSource = $component + '_test.cpp'
    & $compiler @common (Join-Path $repoRoot ('src/camera/' + $component + '.cpp')) `
        (Join-Path $PSScriptRoot $testSource) '-o' $output
    if ($LASTEXITCODE -ne 0) { throw "Native camera component compilation failed: $component" }
    & $output
    if ($LASTEXITCODE -ne 0) { throw "Native camera component validation failed: $component" }
}
& (Join-Path $repoRoot 'tests/camera/ownership_test.ps1')
& (Join-Path $repoRoot 'tests/camera/validate-abi.ps1')
$scheduleTest = Join-Path $outputDirectory 'render-schedule-test.exe'
$controlTest = Join-Path $outputDirectory 'control-policy-test.exe'
& $compiler @common (Join-Path $repoRoot 'tests/camera/control_policy_test.cpp') `
    (Join-Path $nativeRoot 'src/graphics/scene_source_state.cpp') '-o' $controlTest
if ($LASTEXITCODE -ne 0) { throw 'Control policy test compilation failed.' }
& $controlTest
if ($LASTEXITCODE -ne 0) { throw 'Control policy test failed.' }
& $compiler @common (Join-Path $repoRoot 'tests/camera/render_schedule_test.cpp') '-o' $scheduleTest
if ($LASTEXITCODE -ne 0) { throw 'Render schedule test compilation failed.' }
& $scheduleTest
if ($LASTEXITCODE -ne 0) { throw 'Render schedule test failed.' }
$idleTest = Join-Path $outputDirectory 'probe-inspection-gate-test.exe'
& $compiler @common (Join-Path $PSScriptRoot 'probe_inspection_gate_test.cpp') '-o' $idleTest
if ($LASTEXITCODE -ne 0) { throw 'Probe idle inspection gate compilation failed.' }
& $idleTest
if ($LASTEXITCODE -ne 0) { throw 'Probe idle inspection gate validation failed.' }
$recoveryTest = Join-Path $outputDirectory 'scene-recovery-test.exe'
& $compiler @common (Join-Path $repoRoot 'tests/camera/scene_recovery_test.cpp') `
    (Join-Path $nativeRoot 'src/camera/entry_pair.cpp') '-o' $recoveryTest
if ($LASTEXITCODE -ne 0) { throw 'Scene recovery test compilation failed.' }
& $recoveryTest
if ($LASTEXITCODE -ne 0) { throw 'Scene recovery test failed.' }
$retainedResizeTest = Join-Path $outputDirectory 'view-resize-recovery-test.exe'
& $compiler @common (Join-Path $PSScriptRoot 'view_resize_recovery_test.cpp') `
    (Join-Path $nativeRoot 'src/camera/entry_pair.cpp') '-o' $retainedResizeTest
if ($LASTEXITCODE -ne 0) { throw 'Retained resolution recovery compilation failed.' }
& $retainedResizeTest
if ($LASTEXITCODE -ne 0) { throw 'Retained resolution recovery validation failed.' }
$retainedProfileTest = Join-Path $outputDirectory 'retained-profile-test.exe'
& $compiler @common (Join-Path $PSScriptRoot 'retained_profile_test.cpp') `
    (Join-Path $nativeRoot 'src/camera/entry_pair.cpp') '-o' $retainedProfileTest
if ($LASTEXITCODE -ne 0) { throw 'Retained profile transition compilation failed.' }
& $retainedProfileTest
if ($LASTEXITCODE -ne 0) { throw 'Retained profile transition validation failed.' }
$mountTest = Join-Path $outputDirectory 'aircraft-mounts-test.exe'
& $compiler @common (Join-Path $repoRoot 'tests/camera/aircraft_mounts_test.cpp') '-o' $mountTest
if ($LASTEXITCODE -ne 0) { throw 'Aircraft mount test compilation failed.' }
& $mountTest
if ($LASTEXITCODE -ne 0) { throw 'Aircraft mount test failed.' }
$scenePoseTest = Join-Path $outputDirectory 'aircraft-scene-pose-test.exe'
& $compiler @common (Join-Path $PSScriptRoot 'aircraft_scene_pose_test.cpp') '-o' $scenePoseTest
if ($LASTEXITCODE -ne 0) { throw 'Aircraft scene pose test compilation failed.' }
& $scenePoseTest
if ($LASTEXITCODE -ne 0) { throw 'Aircraft scene pose test failed.' }
$mountConfigTest = Join-Path $outputDirectory 'mount-config-test.exe'
& $compiler @common '-fno-exceptions' '-fno-rtti' (Join-Path $repoRoot 'src/camera/mount_config.cpp') `
    (Join-Path $repoRoot 'tests/camera/mount_config_test.cpp') '-o' $mountConfigTest
if ($LASTEXITCODE -ne 0) { throw 'Mount configuration test compilation failed.' }
& $mountConfigTest
if ($LASTEXITCODE -ne 0) { throw 'Mount configuration test failed.' }
$bodyProviderTest = Join-Path $outputDirectory 'body-pose-provider-test.exe'
& $compiler @common '-fno-exceptions' '-fno-rtti' '-DTAXI_BODY_POSE_PROVIDER_TESTING' `
    (Join-Path $repoRoot 'src/camera/body_pose_provider.cpp') (Join-Path $repoRoot 'tests/camera/body_pose_provider_test.cpp') '-o' $bodyProviderTest
if ($LASTEXITCODE -ne 0) { throw 'SimConnect telemetry test compilation failed.' }
& $bodyProviderTest --self-test
if ($LASTEXITCODE -ne 0) { throw 'SimConnect telemetry test failed.' }
$bodyMathTest = Join-Path $outputDirectory 'body-pose-math-test.exe'
& $compiler @common (Join-Path $repoRoot 'tests/camera/body_pose_math_test.cpp') '-o' $bodyMathTest
if ($LASTEXITCODE -ne 0) { throw 'Aircraft body pose math test compilation failed.' }
& $bodyMathTest
if ($LASTEXITCODE -ne 0) { throw 'Aircraft body pose math test failed.' }
$runtimeSources = @(
    'src/camera/probe.cpp',
    'src/camera/body_pose_provider.cpp',
    'src/camera/local_memory.cpp',
    'src/camera/code_contract.cpp',
    'src/camera/source_view.cpp',
    'src/camera/activation_mask.cpp',
    'src/camera/verified_profile.cpp',
    'src/camera/view_resize.cpp',
    'src/camera/view_aa.cpp',
    'src/camera/aircraft_inventory.cpp',
    'src/graphics/scene_handoff.cpp',
    'src/camera/entry_pair.cpp',
    'src/camera/owned_entry_inventory.cpp',
    'src/camera/owned_view.cpp',
    'src/camera/view_pool.cpp'
) | ForEach-Object { Join-Path $repoRoot $_ }
$runtimeTest = Join-Path $outputDirectory 'runtime-test.exe'
& $compiler @common (Join-Path $repoRoot 'tests/camera/runtime_test.cpp') @runtimeSources `
    (Join-Path $nativeRoot 'build/tests/hooks/engine-hook.a') '-o' $runtimeTest
if ($LASTEXITCODE -ne 0) { throw 'Native runtime integration test compilation failed.' }
& $runtimeTest
if ($LASTEXITCODE -ne 0) { throw 'Native runtime wrong-host guard failed.' }
