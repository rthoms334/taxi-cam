[CmdletBinding()]
param([switch]$Validate, [switch]$Bootstrap, [switch]$WarpOnly)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($WarpOnly -and -not $Validate) { throw '-WarpOnly requires -Validate.' }
$taskRoot = $PSScriptRoot
. (Join-Path $taskRoot 'ci/version.ps1')
$buildNumber = Get-TaxiReleaseBuildNumber
$releaseVersion = Get-TaxiVersion -Repository $taskRoot -BuildNumber $buildNumber
$version = $releaseVersion.Version
Write-Output "Building Taxi Cam $version (build $buildNumber; $($releaseVersion.CommitsSinceBase) commits since version baseline)."
$deps = Get-Content -Raw -LiteralPath (Join-Path $taskRoot 'dependencies.json') | ConvertFrom-Json
$compiler = Join-Path $taskRoot ('build/deps/' + $deps.'llvm-mingw'.directory + '/bin/clang++.exe')
if ($Bootstrap -and -not (Test-Path -LiteralPath $compiler)) { & (Join-Path $taskRoot 'bootstrap.ps1') }
if (-not (Test-Path -LiteralPath $compiler)) { throw 'Pinned LLVM-MinGW compiler missing. Run build.ps1 -Bootstrap.' }
$out = Join-Path $taskRoot 'build/native'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$generated = Join-Path $out 'generated'
New-Item -ItemType Directory -Force -Path $generated | Out-Null
@(
    '#pragma once',
    "#define TAXI_CAM_VERSION `"$version`"",
    "#define TAXI_CAM_VERSION_MAJOR $($releaseVersion.Major)",
    "#define TAXI_CAM_VERSION_MINOR $($releaseVersion.Minor)",
    "#define TAXI_CAM_VERSION_PATCH $($releaseVersion.Patch)",
    "#define TAXI_CAM_BUILD_NUMBER $buildNumber"
) | Set-Content -LiteralPath (Join-Path $generated 'taxi-cam-version.hpp') -Encoding ascii
$manifest = Get-Content -Raw -LiteralPath (Join-Path $taskRoot 'src/app/app.manifest')
$manifest.Replace('@TAXI_CAM_VERSION@', $version) | Set-Content -LiteralPath (Join-Path $generated 'taxi-cam.manifest') -Encoding utf8
$releaseVersion | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $out 'version.json') -Encoding utf8
$receipt = Join-Path $out 'validation.json'
if (Test-Path -LiteralPath $receipt) { Remove-Item -LiteralPath $receipt }
$common = @('-std=c++20','-O2','-Wall','-Wextra','-Werror','-fms-extensions','-static',
    '-DNOMINMAX','-DWIN32_LEAN_AND_MEAN','-D_WIN32_WINNT=0x0A00',
    '-mno-avx','-mno-avx2','-mno-avx512f')
$common += @('-I', $generated)
$graphics = @(
 'src/bridge/d3d12_bridge.cpp',
 'src/graphics/scene_handoff.cpp','src/graphics/scene_capture_d3d12.cpp','src/graphics/scene_capture_manager.cpp','src/graphics/scene_source_state.cpp',
 'src/graphics/scene_frame_output.cpp','src/graphics/scene_runtime.cpp','src/graphics/pfd_submission_pool.cpp','src/graphics/pfd_stamp_state.cpp','src/graphics/pfd_stamp_d3d12.cpp',
 'src/hooks/queue_submit_observer.cpp','src/hooks/pfd_state_observer.cpp','src/hooks/render_boundary_observer.cpp'
)
$engine = @(
 'src/camera/probe.cpp','src/camera/local_memory.cpp','src/camera/code_contract.cpp','src/camera/activation_mask.cpp',
 'src/camera/camera_contract.cpp','src/camera/camera_contract_model.cpp','src/camera/relocatable_contract.cpp','src/camera/rtti_vtables.cpp',
 'src/camera/view_resize.cpp','src/camera/view_aa.cpp','src/camera/source_view.cpp','src/camera/body_pose_provider.cpp','src/camera/mount_config.cpp',
 'src/camera/aircraft_inventory.cpp','src/camera/entry_pair.cpp',
 'src/camera/owned_entry_inventory.cpp','src/camera/owned_view.cpp','src/camera/view_pool.cpp',
 'src/hooks/observer_hook.cpp','src/hooks/observer_thunk.S','tools/diagnostics/image_inventory.cpp'
)
$libs = @('-ld3d12','-ldxgi','-ldxguid','-ld3dcompiler')
$objects = @()
foreach ($source in @($graphics + $engine)) {
    $object = Join-Path $out (($source -replace '[/\\]','_') + '.o')
    & $compiler @common '-c' (Join-Path $taskRoot $source) '-o' $object
    if ($LASTEXITCODE -ne 0) { throw "Native compilation failed: $source" }
    $objects += $object
}
if (Test-Path -LiteralPath (Join-Path $taskRoot 'src/bridge/bridge_main.cpp')) {
    & $compiler @common '-shared' (Join-Path $taskRoot 'src/bridge/bridge_main.cpp') @objects @libs '-Wl,--no-insert-timestamp' '-o' (Join-Path $out 'taxi-camera-bridge.dll')
    if ($LASTEXITCODE -ne 0) { throw 'Native bridge link failed.' }
}
if (Test-Path -LiteralPath (Join-Path $taskRoot 'src/app/companion.cpp')) {
    $windres = Join-Path (Split-Path -Parent $compiler) 'llvm-windres.exe'
    $resource = Join-Path $out 'app.res.o'
    & $windres '-I' $generated '-I' (Join-Path $taskRoot 'src/app') '-i' (Join-Path $taskRoot 'src/app/app.rc') '-O' 'coff' '-o' $resource
    if ($LASTEXITCODE -ne 0) { throw 'Windows application manifest compilation failed.' }
    & $compiler @common '-municode' '-mwindows' (Join-Path $taskRoot 'src/app/companion.cpp') (Join-Path $taskRoot 'src/app/updater.cpp') $resource '-lbcrypt' '-lshell32' '-lcomctl32' '-lcomdlg32' '-ladvapi32' '-ldwmapi' '-luxtheme' '-Wl,--no-insert-timestamp' '-o' (Join-Path $out 'taxi-cam.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Windows companion build failed.' }
    $binaryVersion = (Get-Item -LiteralPath (Join-Path $out 'taxi-cam.exe')).VersionInfo
    if ($binaryVersion.FileVersion -ne $version -or $binaryVersion.ProductVersion -ne $version -or
        "$($binaryVersion.FileMajorPart).$($binaryVersion.FileMinorPart).$($binaryVersion.FileBuildPart)" -ne $version) {
        throw 'Windows executable version differs from the release version.'
    }
}
if ($Validate) {
    $deviceIdentityTest = Join-Path $out 'native-device-identity-test.exe'
    & $compiler @common (Join-Path $taskRoot 'tests/graphics/native_device_identity_test.cpp') @libs '-o' $deviceIdentityTest
    if ($LASTEXITCODE -ne 0) { throw 'Native device identity compilation failed.' }
    & $deviceIdentityTest
    if ($LASTEXITCODE -ne 0) { throw 'Native device identity regression failed.' }
    & $deviceIdentityTest --warp
    if ($LASTEXITCODE -ne 0) { throw 'WARP native device identity regression failed.' }
    $lateConnect = Join-Path $out 'late-connect-rendering-test.exe'
    & $compiler @common (Join-Path $taskRoot 'tests/graphics/late_connect_rendering_test.cpp') @($objects | Select-Object -First $graphics.Count) @libs '-o' $lateConnect
    if ($LASTEXITCODE -ne 0) { throw 'Active-rendering late Connect compilation failed.' }
    & $lateConnect --warp
    if ($LASTEXITCODE -ne 0) { throw 'WARP active-rendering late Connect failed.' }
    if (-not $WarpOnly) {
        & $lateConnect
        if ($LASTEXITCODE -ne 0) { throw 'Hardware active-rendering late Connect failed.' }
    }
    $iconTest = Join-Path $out 'application-icon-test.exe'
    & $compiler @common '-municode' (Join-Path $taskRoot 'tests/app/application_icon_test.cpp') '-lshell32' '-lgdi32' '-o' $iconTest
    if ($LASTEXITCODE -ne 0) { throw 'Application icon validation compilation failed.' }
    & $iconTest (Join-Path $out 'taxi-cam.exe') (Join-Path $out 'application-icon')
    if ($LASTEXITCODE -ne 0) { throw 'Application icon validation failed.' }
    $gpu = Join-Path $out 'native-graphics-validation.exe'
    & $compiler @common '-municode' (Join-Path $taskRoot 'tests/graphics/graphics_validation.cpp') @($objects | Select-Object -First $graphics.Count) @libs '-o' $gpu
    if ($LASTEXITCODE -ne 0) { throw 'Native validation compilation failed.' }
    $frameOrder = Join-Path $out 'native-frame-order-validation.exe'
    & $compiler @common '-municode' (Join-Path $taskRoot 'tests/graphics/frame_order_validation.cpp') @($objects | Select-Object -First $graphics.Count) @libs '-o' $frameOrder
    if ($LASTEXITCODE -ne 0) { throw 'Frame-order validation compilation failed.' }
    & $frameOrder --warp
    if ($LASTEXITCODE -ne 0) { throw 'WARP frame-order validation failed.' }
    if (-not $WarpOnly) {
        & $frameOrder
        if ($LASTEXITCODE -ne 0) { throw 'Hardware frame-order validation failed.' }
    }
    & $gpu --patch-demand
    if ($LASTEXITCODE -ne 0) { throw 'Lazy patch demand admission failed.' }
    if (-not $WarpOnly) {
        & $gpu
        if ($LASTEXITCODE -ne 0) { throw 'Hardware native graphics validation failed.' }
        & $gpu --a350
        if ($LASTEXITCODE -ne 0) { throw 'Hardware A350 graphics validation failed.' }
        & $gpu --ini-a380
        if ($LASTEXITCODE -ne 0) { throw 'Hardware ini A380 graphics validation failed.' }
    } else { Write-Output 'Hardware GPU validation not run: explicit WARP-only CI mode.' }
    & $gpu --warp
    if ($LASTEXITCODE -ne 0) { throw 'WARP native graphics validation failed.' }
    & $gpu --warp --a350
    if ($LASTEXITCODE -ne 0) { throw 'WARP A350 graphics validation failed.' }
    & $gpu --warp --ini-a380
    if ($LASTEXITCODE -ne 0) { throw 'WARP ini A380 graphics validation failed.' }
    foreach ($queryAdapter in @('hardware', 'warp')) {
        if ($WarpOnly -and $queryAdapter -eq 'hardware') { continue }
        foreach ($queryProfile in @('a380', 'a350')) {
            $queryArgs = @('--query-fallback')
            if ($queryAdapter -eq 'warp') { $queryArgs += '--warp' }
            if ($queryProfile -eq 'a350') { $queryArgs += '--a350' }
            & $gpu @queryArgs
            if ($LASTEXITCODE -ne 0) { throw "Query-aware PFD regression failed: $queryAdapter / $queryProfile" }
        }
    }
    foreach ($copyAdapter in @('hardware', 'warp')) {
        if ($WarpOnly -and $copyAdapter -eq 'hardware') { continue }
        foreach ($copyProfile in @('a380', 'a350')) {
            $copyArgs = @('--prefer-copy')
            if ($copyAdapter -eq 'warp') { $copyArgs += '--warp' }
            if ($copyProfile -eq 'a350') { $copyArgs += '--a350' }
            & $gpu @copyArgs
            if ($LASTEXITCODE -ne 0) { throw "PFD boundary-copy regression failed: $copyAdapter / $copyProfile" }
        }
    }
    $metadataTest = Join-Path $out 'metadata-batch-bridge-test.exe'
    $metadataObjects = @($objects | Select-Object -First $graphics.Count | Select-Object -Skip 1)
    & $compiler @common (Join-Path $taskRoot 'tests/graphics/metadata_batch_bridge_test.cpp') @metadataObjects @libs '-o' $metadataTest
    if ($LASTEXITCODE -ne 0) { throw 'Bridge metadata batching regression compilation failed.' }
    & $metadataTest
    if ($LASTEXITCODE -ne 0) { throw 'Bridge metadata batching regression failed.' }
    $lateAttachTest = Join-Path $out 'late-attach-test.exe'
    & $compiler @common (Join-Path $taskRoot 'tests/graphics/late_attach_test.cpp') @metadataObjects @libs '-o' $lateAttachTest
    if ($LASTEXITCODE -ne 0) { throw 'Late-attach regression compilation failed.' }
    & $lateAttachTest
    if ($LASTEXITCODE -ne 0) { throw 'Late-attach regression failed.' }
    $sourceObservationTest = Join-Path $out 'source-observation-test.exe'
    & $compiler @common '-fno-access-control' (Join-Path $taskRoot 'tests/graphics/source_observation_test.cpp') @metadataObjects @libs '-o' $sourceObservationTest
    if ($LASTEXITCODE -ne 0) { throw 'Source observation regression compilation failed.' }
    if (-not $WarpOnly) {
        & $sourceObservationTest
        if ($LASTEXITCODE -ne 0) { throw 'Hardware source observation regression failed.' }
    }
    & $sourceObservationTest --warp
    if ($LASTEXITCODE -ne 0) { throw 'WARP source observation regression failed.' }
    foreach ($grayAdapter in @('hardware', 'warp')) {
        if ($WarpOnly -and $grayAdapter -eq 'hardware') { continue }
        $grayArgs = @('--textured-gray')
        if ($grayAdapter -eq 'warp') { $grayArgs += '--warp' }
        & $gpu @grayArgs
        if ($LASTEXITCODE -ne 0) { throw "Textured gray PFD fallback regression failed: $grayAdapter" }
    }
    $frameOutputTest = Join-Path $out 'scene-frame-output-test.exe'
    & $compiler @common (Join-Path $taskRoot 'tests/graphics/scene_frame_output_test.cpp') @($objects | Select-Object -First $graphics.Count) @libs '-o' $frameOutputTest
    if ($LASTEXITCODE -ne 0) { throw 'Lazy patch replay validation compilation failed.' }
    if (-not $WarpOnly) {
        & $frameOutputTest
        if ($LASTEXITCODE -ne 0) { throw 'Hardware lazy patch replay validation failed.' }
    }
    & $frameOutputTest --warp
    if ($LASTEXITCODE -ne 0) { throw 'WARP lazy patch replay validation failed.' }
    $fontGpu = Join-Path $out 'gs-font-validation.exe'
    & $compiler @common '-municode' (Join-Path $taskRoot 'tests/graphics/gs_font_validation.cpp') @libs '-o' $fontGpu
    if ($LASTEXITCODE -ne 0) { throw 'GS font validation compilation failed.' }
    if (-not $WarpOnly) {
        & $fontGpu
        if ($LASTEXITCODE -ne 0) { throw 'Hardware GS font validation failed.' }
    }
    & $fontGpu '--warp'
    if ($LASTEXITCODE -ne 0) { throw 'WARP GS font validation failed.' }
    $dynamicGpu = Join-Path $out 'dynamic-state-validation.exe'
    & $compiler @common (Join-Path $taskRoot 'tests/graphics/dynamic_state_validation.cpp') (Join-Path $out 'src_graphics_pfd_stamp_state.cpp.o') (Join-Path $out 'src_graphics_pfd_stamp_d3d12.cpp.o') (Join-Path $out 'src_hooks_pfd_state_observer.cpp.o') @libs '-o' $dynamicGpu
    if ($LASTEXITCODE -ne 0) { throw 'Dynamic graphics-state validation compilation failed.' }
    $dynamicStateResults = @()
    foreach ($dynamicAdapter in @('hardware', 'warp')) {
        if ($WarpOnly -and $dynamicAdapter -eq 'hardware') { continue }
        $dynamicArgs = @()
        if ($dynamicAdapter -eq 'warp') { $dynamicArgs += '--warp' }
        & $dynamicGpu @dynamicArgs
        $dynamicExit = $LASTEXITCODE
        if ($dynamicExit -notin @(0, 77)) { throw "Dynamic graphics-state regression failed: $dynamicAdapter" }
        $dynamicStateResults += [ordered]@{adapter=$dynamicAdapter; result=$(if ($dynamicExit -eq 77) { 'unsupported; skipped' } else { 'passed' })}
    }
    $sampleGpu = Join-Path $out 'sample-positions-validation.exe'
    & $compiler @common (Join-Path $taskRoot 'tests/graphics/sample_positions_validation.cpp') (Join-Path $out 'src_graphics_pfd_stamp_state.cpp.o') (Join-Path $out 'src_graphics_pfd_stamp_d3d12.cpp.o') (Join-Path $out 'src_hooks_pfd_state_observer.cpp.o') @libs '-o' $sampleGpu
    if ($LASTEXITCODE -ne 0) { throw 'Sample-position validation compilation failed.' }
    $sampleResults = @()
    foreach ($sampleAdapter in @('hardware', 'warp')) {
        if ($WarpOnly -and $sampleAdapter -eq 'hardware') { continue }
        $sampleArgs = @()
        if ($sampleAdapter -eq 'warp') { $sampleArgs += '--warp' }
        & $sampleGpu @sampleArgs
        $sampleExit = $LASTEXITCODE
        if ($sampleExit -notin @(0, 77)) { throw "Sample-position graphics regression failed: $sampleAdapter" }
        $sampleResults += [ordered]@{adapter=$sampleAdapter; result=$(if ($sampleExit -eq 77) { 'unsupported; skipped' } else { 'passed' })}
    }
    $tailTest = Join-Path $out 'scene-queue-tail-test.exe'
    & $compiler @common '-fno-access-control' (Join-Path $taskRoot 'tests/graphics/scene_queue_tail_test.cpp') @($objects | Select-Object -First $graphics.Count) @libs '-o' $tailTest
    if ($LASTEXITCODE -ne 0) { throw 'Live capture regression compilation failed.' }
    & $tailTest --submission-locks
    if ($LASTEXITCODE -ne 0) { throw 'Submission lock ordering regression failed.' }
    foreach ($tailAdapter in @('hardware', 'warp')) {
        if ($WarpOnly -and $tailAdapter -eq 'hardware') { continue }
        foreach ($tailOrigin in @('transition', 'creation')) {
            $tailArgs = @()
            if ($tailAdapter -eq 'warp') { $tailArgs += '--warp' }
            if ($tailOrigin -eq 'creation') { $tailArgs += '--born-render-target' }
            & $tailTest @tailArgs
            if ($LASTEXITCODE -ne 0) { throw "Live capture regression failed: $tailAdapter / $tailOrigin" }
        }
    }

    $runtimeContention = Join-Path $out 'scene-runtime-contention-test.exe'
    $runtimeContentionObjects = @($objects | Select-Object -First $graphics.Count | Where-Object { (Split-Path -Leaf $_) -ne 'src_graphics_scene_runtime.cpp.o' })
    & $compiler @common '-fno-access-control' (Join-Path $taskRoot 'tests/graphics/scene_runtime_contention_test.cpp') @runtimeContentionObjects @libs '-o' $runtimeContention
    if ($LASTEXITCODE -ne 0) { throw 'Camera service contention regression compilation failed.' }
    & $runtimeContention --warp
    if ($LASTEXITCODE -ne 0) { throw 'WARP camera service contention regression failed.' }
    if (-not $WarpOnly) {
        & $runtimeContention
        if ($LASTEXITCODE -ne 0) { throw 'Hardware camera service contention regression failed.' }
    }
    $queuePatchTest = Join-Path $out 'queue-patch-snapshot-test.exe'
    & $compiler @common '-fno-access-control' (Join-Path $taskRoot 'tests/graphics/queue_patch_snapshot_test.cpp') @runtimeContentionObjects @libs '-o' $queuePatchTest
    if ($LASTEXITCODE -ne 0) { throw 'Queue patch snapshot compilation failed.' }
    $splitSubmissionTest = Join-Path $out 'pfd-split-submission-test.exe'
    & $compiler @common (Join-Path $taskRoot 'tests/graphics/pfd_split_submission_test.cpp') @($objects | Select-Object -First $graphics.Count) @libs '-o' $splitSubmissionTest
    if ($LASTEXITCODE -ne 0) { throw 'Split-list display submission compilation failed.' }
    $displayPoolTest = Join-Path $out 'pfd-submission-pool-test.exe'
    & $compiler @common (Join-Path $taskRoot 'tests/graphics/pfd_submission_pool_test.cpp') (Join-Path $taskRoot 'src/graphics/pfd_submission_pool.cpp') (Join-Path $taskRoot 'src/hooks/queue_submit_observer.cpp') @libs '-o' $displayPoolTest
    if ($LASTEXITCODE -ne 0) { throw 'Display submission pool compilation failed.' }
    foreach ($displayAdapter in @('warp','hardware')) {
        if ($WarpOnly -and $displayAdapter -eq 'hardware') { continue }
        $displayArgs = @()
        if ($displayAdapter -eq 'warp') { $displayArgs += '--warp' }
        & $queuePatchTest @displayArgs
        if ($LASTEXITCODE -ne 0) { throw "Queue patch snapshot failed: $displayAdapter" }
        & $queuePatchTest @displayArgs --a350
        if ($LASTEXITCODE -ne 0) { throw "A350 queue patch snapshot failed: $displayAdapter" }
        foreach ($displayProfile in @('ini','fbw','fbw-three')) {
            foreach ($displayState in @('srv','common')) {
                foreach ($displayPlacement in @('separate','mixed-exit','first-list')) {
                    $splitArgs = @($displayArgs)
                    if ($displayProfile -ne 'ini') { $splitArgs += "--$displayProfile" }
                    if ($displayState -eq 'common') { $splitArgs += '--common' }
                    if ($displayPlacement -ne 'separate') { $splitArgs += "--$displayPlacement" }
                    & $splitSubmissionTest @splitArgs
                    if ($LASTEXITCODE -ne 0) { throw "Display submission failed: $displayAdapter / $displayProfile / $displayState / $displayPlacement" }
                }
            }
        }
        & $displayPoolTest @displayArgs
        if ($LASTEXITCODE -ne 0) { throw "Display submission pool failed: $displayAdapter" }
        & $displayPoolTest @displayArgs --common
        if ($LASTEXITCODE -ne 0) { throw "COMMON display submission pool failed: $displayAdapter" }
        & $displayPoolTest @displayArgs --before
        if ($LASTEXITCODE -ne 0) { throw "Before-list display submission pool failed: $displayAdapter" }
    }

    $compositor = Join-Path $out 'compositor-validation.exe'
    & $compiler @common '-municode' (Join-Path $taskRoot 'tests/graphics/compositor_main.cpp') @libs '-o' $compositor
    if ($LASTEXITCODE -ne 0) { throw 'Compositor validation compilation failed.' }
    if (-not $WarpOnly) {
        & $compositor
        if ($LASTEXITCODE -ne 0) { throw 'Hardware compositor validation failed.' }
    }
    & $compositor --warp
    if ($LASTEXITCODE -ne 0) { throw 'WARP compositor validation failed.' }

    if (-not $WarpOnly) {
        & $gpu --profile-switch
        if ($LASTEXITCODE -ne 0) { throw 'Hardware active-feed aircraft-switch validation failed.' }
    }
    & $gpu --warp --profile-switch
    if ($LASTEXITCODE -ne 0) { throw 'WARP active-feed aircraft-switch validation failed.' }
    $smoke = Join-Path $out 'native-smoke-validation.exe'
    & $compiler @common '-municode' (Join-Path $taskRoot 'tests/app/smoke_validation.cpp') '-ladvapi32' '-o' $smoke
    if ($LASTEXITCODE -ne 0) { throw 'Native smoke compilation failed.' }
    & $smoke (Join-Path $out 'taxi-camera-bridge.dll')
    if ($LASTEXITCODE -ne 0) { throw 'Exact native DLL smoke failed.' }
    $logTest = Join-Path $out 'rotating-log-test.exe'
    & $compiler @common '-municode' (Join-Path $taskRoot 'tests/app/rotating_log_test.cpp') '-o' $logTest
    if ($LASTEXITCODE -ne 0) { throw 'Runtime log rotation compilation failed.' }
    & $logTest
    if ($LASTEXITCODE -ne 0) { throw 'Runtime log rotation regression failed.' }
    foreach ($entry in @(
        @{Name='native-launcher-path'; Sources=@('tests/app/launcher_path_test.cpp')},
        @{Name='connection-recoverability'; Sources=@('tests/app/connection_recoverability_test.cpp')},
        @{Name='process-elevation'; Sources=@('tests/app/process_elevation_test.cpp')},
        @{Name='simulator-graphics'; Sources=@('tests/app/simulator_graphics_test.cpp')},
        @{Name='aircraft-profiles'; Sources=@('tests/app/aircraft_profiles_test.cpp','src/camera/view_resize.cpp')},
        @{Name='profile-selection'; Sources=@('tests/app/profile_selection_test.cpp')},
        @{Name='night-boost-migration'; Sources=@('tests/app/night_boost_migration_test.cpp')},
        @{Name='companion-control'; Sources=@('tests/app/companion_control_test.cpp')},
        @{Name='startup-state'; Sources=@('tests/app/startup_state_test.cpp')},
        @{Name='camera-status'; Sources=@('tests/app/camera_status_test.cpp')},
        @{Name='bug-report'; Sources=@('tests/app/bug_report_test.cpp')},
        @{Name='scene-demand'; Sources=@('tests/camera/scene_demand_test.cpp','src/camera/entry_pair.cpp')},
        @{Name='scene-session-reset'; Sources=@('tests/camera/scene_session_reset_test.cpp','src/camera/entry_pair.cpp')},
        @{Name='view-creation-wait'; Sources=@('tests/camera/view_creation_wait_test.cpp','src/camera/entry_pair.cpp')},
        @{Name='view-output'; Sources=@('tests/camera/view_output_test.cpp')},
        @{Name='manager-inspection'; Sources=@('tests/camera/manager_inspection_test.cpp')},
        @{Name='relocatable-contract'; Sources=@('src/camera/relocatable_contract.cpp','tests/camera/relocatable_contract_test.cpp')},
        @{Name='rtti-vtables'; Sources=@('src/camera/rtti_vtables.cpp','tests/camera/rtti_vtables_test.cpp')},
        @{Name='camera-contract'; Sources=@('src/camera/camera_contract.cpp','src/camera/camera_contract_model.cpp','src/camera/relocatable_contract.cpp','src/camera/rtti_vtables.cpp','src/camera/code_contract.cpp','src/camera/activation_mask.cpp','tools/diagnostics/image_inventory.cpp','tests/camera/camera_contract_test.cpp')},
        @{Name='taxi-button-command'; Sources=@('tests/camera/taxi_button_command_test.cpp')},
        @{Name='aircraft-layout'; Sources=@('src/camera/aircraft_inventory.cpp','tests/camera/aircraft_inventory_test.cpp')},
        @{Name='native-slots'; Sources=@('tests/graphics/native_slots_test.cpp')},
        @{Name='crash-evidence'; Sources=@('tests/graphics/crash_evidence_test.cpp')},
        @{Name='native-root-state'; Sources=@('tests/graphics/root_state_test.cpp','src/graphics/pfd_stamp_state.cpp')},
        @{Name='dynamic-state-abi'; Sources=@('tests/graphics/dynamic_state_abi_test.cpp')},
        @{Name='sample-positions-abi'; Sources=@('tests/graphics/sample_positions_abi_test.cpp')},
        @{Name='native-query-scope'; Sources=@('tests/graphics/query_scope_test.cpp')},
        @{Name='pfd-copy-proof'; Sources=@('tests/graphics/pfd_copy_proof_test.cpp')},
        @{Name='pfd-submission-proof'; Sources=@('tests/graphics/pfd_submission_proof_test.cpp')},
        @{Name='metadata-batch-cache'; Sources=@('tests/graphics/metadata_batch_cache_test.cpp')},
        @{Name='taxi-routes'; Sources=@('tests/graphics/taxi_button_routes_test.cpp')},
        @{Name='target-assignment'; Sources=@('tests/graphics/target_assignment_test.cpp')},
        @{Name='pfd-detector'; Sources=@('tests/graphics/pfd_target_detector_test.cpp')},
        @{Name='display-exposure'; Sources=@('tests/graphics/display_exposure_test.cpp')},
        @{Name='ground-speed-display'; Sources=@('tests/graphics/ground_speed_display_test.cpp')},
        @{Name='calibration'; Sources=@('tests/graphics/calibration_test.cpp')},
        @{Name='write-budget'; Sources=@('tests/graphics/write_budget_test.cpp')},
        @{Name='scene-handoff'; Sources=@('tests/graphics/scene_handoff_test.cpp','src/graphics/scene_handoff.cpp')},
        @{Name='source-state'; Sources=@('tests/graphics/scene_source_state_test.cpp','src/graphics/scene_source_state.cpp')},
        @{Name='queue-submit'; Sources=@('tests/hooks/queue_submit_observer_test.cpp','src/hooks/queue_submit_observer.cpp')}
    )) {
        $exe = Join-Path $out ($entry.Name + '-test.exe')
        $sources = @($entry.Sources | ForEach-Object { Join-Path $taskRoot $_ })
        & $compiler @common @sources @libs '-ladvapi32' '-o' $exe
        if ($LASTEXITCODE -ne 0) { throw "Native test compilation failed: $($entry.Name)" }
        & $exe
        if ($LASTEXITCODE -ne 0) { throw "Native test failed: $($entry.Name)" }
        if ($entry.Name -eq 'queue-submit') {
            & $exe augment
            if ($LASTEXITCODE -ne 0) { throw 'Queue submission augmentation failed.' }
        }
    }
    $diagnosticsTest = Join-Path $out 'companion-diagnostics-test.exe'
    & $compiler @common (Join-Path $taskRoot 'tests/app/companion_diagnostics_test.cpp') (Join-Path $taskRoot 'src/app/updater.cpp') '-lgdi32' '-lbcrypt' '-lshell32' '-lcomctl32' '-lcomdlg32' '-ladvapi32' '-ldwmapi' '-luxtheme' '-o' $diagnosticsTest
    if ($LASTEXITCODE -ne 0) { throw 'Diagnostic controls compilation failed.' }
    & $diagnosticsTest
    if ($LASTEXITCODE -ne 0) { throw 'Diagnostic controls validation failed.' }
    $hotkeysTest = Join-Path $out 'camera-hotkeys-test.exe'
    & $compiler @common (Join-Path $taskRoot 'tests/app/camera_hotkeys_test.cpp') (Join-Path $taskRoot 'src/app/updater.cpp') '-lgdi32' '-lbcrypt' '-lshell32' '-lcomctl32' '-lcomdlg32' '-ladvapi32' '-ldwmapi' '-luxtheme' '-o' $hotkeysTest
    if ($LASTEXITCODE -ne 0) { throw 'Camera shortcut controls compilation failed.' }
    & $hotkeysTest
    if ($LASTEXITCODE -ne 0) { throw 'Camera shortcut controls validation failed.' }
    & (Join-Path $taskRoot 'tests/installer/exe_xml_test.ps1')
    $updaterTest = Join-Path $out 'updater-test.exe'
    & $compiler @common (Join-Path $taskRoot 'tests/app/updater_test.cpp') (Join-Path $taskRoot 'src/app/updater.cpp') '-lbcrypt' '-lshell32' '-o' $updaterTest
    if ($LASTEXITCODE -ne 0) { throw 'Updater test compilation failed.' }
    & $updaterTest
    if ($LASTEXITCODE -ne 0) { throw 'Updater tests failed.' }
    & (Join-Path $taskRoot 'tests/app/updater_test.ps1')
    $observerTest = Join-Path $out 'pfd-state-observer-test.exe'
    & $compiler @common '-DTAXI_PFD_STATE_OBSERVER_VALIDATION' (Join-Path $taskRoot 'tests/hooks/pfd_state_observer_test.cpp') (Join-Path $taskRoot 'src/hooks/pfd_state_observer.cpp') '-o' $observerTest
    if ($LASTEXITCODE -ne 0) { throw 'PFD state observer test compilation failed.' }
    & $observerTest
    if ($LASTEXITCODE -ne 0) { throw 'PFD state observer lifecycle tests failed.' }
    & $observerTest --optional
    if ($LASTEXITCODE -ne 0) { throw 'PFD state observer optional callback tests failed.' }
    & (Join-Path $taskRoot 'tests/hooks/render_boundary_test.ps1') -Compiler $compiler
    & (Join-Path $taskRoot 'tests/hooks/test.ps1')
    & (Join-Path $taskRoot 'tests/camera/test.ps1')
    $readobj = Join-Path (Split-Path -Parent $compiler) 'llvm-readobj.exe'
    $hashes = [ordered]@{}
    foreach ($name in @('taxi-cam.exe','taxi-camera-bridge.dll')) {
        $path = Join-Path $out $name
        $imports = & $readobj '--coff-imports' $path
        if ($LASTEXITCODE -ne 0) { throw "Import audit failed: $name" }
        if (($imports -join "`n") -match '(?i)(libc\+\+|libunwind|libwinpthread).*\.dll') {
            throw "Non-native runtime dependency found in $name."
        }
        $imports | Set-Content -LiteralPath (Join-Path $out ($name+'.imports.txt'))
        $symbols = & $readobj '--symbols' $path
        if ($LASTEXITCODE -ne 0) { throw "Symbol audit failed: $name" }
        if ((($imports + $symbols) -join "`n") -match '(?i)(SuspendThread|ResumeThread|(?:Nt|Zw)(?:Suspend|Resume)(?:Thread|Process)|D3D11On12CreateDevice|\bMH_)') {
            throw "$name includes a forbidden D3D11On12 bootstrap or thread-suspending hook dependency."
        }
        $symbols | Set-Content -LiteralPath (Join-Path $out ($name+'.symbols.txt'))
        $hashes[$name] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    }
    $closure = @()
    foreach ($source in @($graphics + $engine + @('src/bridge/bridge_main.cpp','src/app/companion.cpp','src/app/updater.cpp'))) {
        if ($source.EndsWith('.S')) { continue }
        $dependencyList = & $compiler @common '-MM' (Join-Path $taskRoot $source)
        if ($LASTEXITCODE -ne 0) { throw "Native dependency audit failed: $source" }
        $dependencyText = ($dependencyList -join ' ').Replace('\', '/')
        if ($dependencyText -match '(?i)/build/deps/') { throw "Unexpected external header dependency: $source" }
        $closure += $dependencyList
    }
    $closure | Set-Content -LiteralPath (Join-Path $out 'native-dependencies.txt')
    $gpuTests = @('WARP GPU')
    if (-not $WarpOnly) { $gpuTests = @('hardware GPU') + $gpuTests }
    [ordered]@{
        passed=$true; version=$version; buildNumber=$buildNumber; createdUtc=[DateTime]::UtcNow.ToString('o'); files=$hashes;
        dynamicGraphicsStateTests=@($dynamicStateResults)
        samplePositionTests=@($sampleResults)
        tests=@($gpuTests + @('pre-existing graphics objects','late Connect during active native rendering, Present and CPU work','exact bridge rejects D3D11On12 bootstrap and thread-suspending hook dependencies','per-profile marking colour persistence, IPC and GPU pixel isolation','launcher phase timing and distinct wait outcomes','terminal command-list PFD drawing without application root replay','textured gray alpha and mip preservation with terminal PFD draws and repeated submissions','scoped barrier metadata lookup caching','descriptor heap filtering and retained device identity','unrelated submission bypass and discovery lock ordering','per-recording PFD copy evidence and draw fallback','preferred OM/Close copy pixel and query preservation','dynamic graphics-state replay and ABI','programmable sample-pattern normalization and restoration','isolated graphics-state regression coverage','lazy typed patch demand, discard and cross-profile replay','fractional ground-speed truncation and raw-speed cutoff','camera border and inset composition','antialiased GS glyphs and two-character spacing','ClearState pipeline preservation','native render-pass state preservation','private PFD patch copies','selected PFD copies in large barrier batches','PFD exits across command lists','typed PFD view evidence','application occlusion-query preservation','query-aware PFD drawing and OM restoration','calibration OM and Close delivery','retained camera dimension recovery','retained native aircraft transitions and request tokens','current-process memory query equivalence','close-only activation inspection','pane output admission before gate opening','bounded memory query reuse','shared read-only inspection transaction with fresh endpoint validation','early camera preparation and activation timings','bounded grounded prewarm and retained foreground takeover','idle camera inspection scheduling','large barrier batches and changing camera frames','exact DLL smoke',
            'settings persistence and IPC','per-user first-launch Settings visibility, failure retry and preview isolation','per-aircraft reference-guide persistence','live reference-guide GPU updates','scene demand and retained camera ownership','companion contention and watchdog','configurable global camera shortcuts, native conflict recovery, hidden-window dispatch, preview isolation and manual-only controls','launcher file identity','native COM slots','TAXI routing','profile-switch target reacquisition','active-feed A380-A350-A380 transitions with retained sources','manual and automatic target selection','PFD detector','exposure','calibration','write budget',
            'compositor formats and exposure','scene handoff and resource state','camera ownership','bug report URL encoding, bounds and diagnostic privacy','relocatable native instruction discovery and static RTTI identity','complete camera contract on relocated synthetic PE images and refusal controls','queue submit','PFD state observer lifecycle','render boundary','engine hook','camera telemetry and lifecycle','aircraft layout compatibility','exe.xml preservation and rename migration',
            'bounded weak command-list lookup reuse and idle observation epochs','continuous source-state observation with idle capture suppression','optional fenced GPU timestamps on owned recordings','5-60 camera activation limits and low-rate scheduling',
            'bounded rotating runtime logs, oversized history and concurrent writers','bounded renderer-fault retention with active-record preservation',
            'both native release queues and first-free view retirement admission','partial camera-pair cleanup and deferred startup with request cancellation',
            'nonblocking telemetry shutdown with retained worker ownership','private composition contention retry with retained leases and GPU pixels','nonblocking helper refusal and retained GPU submission ordering',
            'same-submit iniBuilds and FBW camera/calibration pixels after barrier-only lists and before mixed exit/consumer lists including first-list insertion, replay and subsequent native overwrite','exact RT exit proof including COMMON and SRV, pass and generation refusal','bounded display copy packets with fence-held source/target lifetime and mip preservation','prepared camera/calibration patches with nonblocking snapshots and manager timeline ordering','late display submission activity with unchanged automatic side ordering',
            'native imports and header dependency closure','embedded multi-resolution application icon and Windows shell extraction','release selection, download integrity and updater handoff guards'));
        gpuValidation=[ordered]@{hardware=$(if ($WarpOnly) { 'not-run' } else { 'passed' });warp='passed'};
        simulatorVerified=$false
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'validation.json') -Encoding utf8

}
Write-Output "Native build complete: $out "
