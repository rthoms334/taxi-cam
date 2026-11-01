[CmdletBinding(PositionalBinding=$false)]
param([switch]$Validate, [switch]$Bootstrap, [switch]$WarpOnly, [switch]$Serial, [int]$Throttle = 0)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($WarpOnly -and -not $Validate) { throw '-WarpOnly requires -Validate.' }
if ($Serial -and $Throttle) { throw 'Use -Serial or -Throttle, not both.' }
if ($Throttle -lt 0) { throw '-Throttle must be positive.' }
# Independent compiles and test runs use a process pool; -Serial runs one process at a time.
# Test executables get half the slots: WARP renders on several CPU threads per process, and
# GPU fixtures wait about one second for frames.
$throttleLimit = if ($Serial) { 1 } elseif ($Throttle) { $Throttle } else { [Environment]::ProcessorCount }
$runThrottle = [Math]::Max(1, [int][Math]::Floor($throttleLimit / 2))
$taskRoot = $PSScriptRoot
. (Join-Path $taskRoot 'ci/version.ps1')
. (Join-Path $taskRoot 'ci/parallel.ps1')
$buildNumber = Get-TaxiReleaseBuildNumber
$releaseVersion = Get-TaxiVersion -Repository $taskRoot -BuildNumber $buildNumber
$version = $releaseVersion.Version
Write-Output "Building Taxi Cam $version (build $buildNumber; version from changelog.json; $throttleLimit parallel compiles, $runThrottle parallel tests)."
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
if ($Validate) {
    & (Join-Path $taskRoot 'ci/parallel_test.ps1')
}
$common = @('-std=c++20','-O2','-Wall','-Wextra','-Werror','-fms-extensions','-static',
    '-DNOMINMAX','-DWIN32_LEAN_AND_MEAN','-D_WIN32_WINNT=0x0A00',
    '-mno-avx','-mno-avx2','-mno-avx512f')
$common += @('-I', $generated)
# Installer commands run as taxi-cam.exe --setup; see src/setup/setup_commands.hpp.
$setup = @('src/setup/setup_commands.cpp','src/setup/setup_common.cpp','src/setup/processes.cpp','src/setup/prerequisites.cpp',
    'src/setup/saved_settings.cpp','src/setup/startup_xml.cpp')
$graphics = @(
 'src/bridge/d3d12_bridge.cpp',
 'src/graphics/scene_handoff.cpp','src/graphics/scene_capture_d3d12.cpp','src/graphics/scene_capture_manager.cpp','src/graphics/scene_source_state.cpp',
 'src/graphics/scene_frame_output.cpp','src/graphics/scene_runtime.cpp','src/graphics/pfd_submission_pool.cpp','src/graphics/pfd_stamp_state.cpp','src/graphics/pfd_stamp_d3d12.cpp',
 'src/hooks/queue_submit_observer.cpp','src/hooks/pfd_state_observer.cpp','src/hooks/render_boundary_observer.cpp'
)
$engine = @(
 'src/camera/probe.cpp','src/camera/local_memory.cpp','src/camera/code_contract.cpp','src/camera/activation_mask.cpp',
 'src/camera/camera_contract.cpp','src/camera/camera_contract_model.cpp','src/camera/relocatable_contract.cpp','src/camera/rtti_vtables.cpp',
 'src/camera/view_resize.cpp','src/camera/view_aa.cpp','src/camera/view_clip.cpp','src/camera/source_view.cpp','src/camera/body_pose_provider.cpp','src/camera/mount_config.cpp',
 'src/camera/aircraft_inventory.cpp','src/camera/entry_pair.cpp','src/camera/display_panels.cpp','src/camera/view_cascades.cpp',
 'src/camera/owned_entry_inventory.cpp','src/camera/owned_view.cpp','src/camera/view_pool.cpp',
 'src/hooks/observer_hook.cpp','src/hooks/observer_thunk.S','tools/diagnostics/image_inventory.cpp'
)
$libs = @('-ld3d12','-ldxgi','-ldxguid','-ld3dcompiler')
$objects = @()
$objectJobs = @()
foreach ($source in @($graphics + $engine)) {
    $object = Join-Path $out (($source -replace '[/\\]','_') + '.o')
    $objectJobs += New-TaxiJob -Name (Split-Path -Leaf $object) -File $compiler -Arguments @($common + '-c' + (Join-Path $taskRoot $source) + '-o' + $object) -Failure "Native compilation failed: $source"
    $objects += $object
}
Invoke-TaxiJobs $objectJobs $throttleLimit
function Add-Compile([string]$Output, [object[]]$Arguments, [string]$Failure) {
    $script:compileJobs += New-TaxiJob -Name (Split-Path -Leaf $Output) -File $compiler -Arguments @($common + $Arguments + '-o' + $Output) -Failure $Failure
    return $Output
}
function Add-Run([string]$Executable, [object[]]$Arguments, [string]$Failure, [int[]]$Ok = @(0), [switch]$Timing) {
    $job = New-TaxiJob -File $Executable -Arguments $Arguments -Failure $Failure -Ok $Ok
    if ($Timing) { $script:timingJobs += $job } else { $script:runJobs += $job }
    return $job
}
$compileJobs = @()
$runJobs = @()
$timingJobs = @()
if (Test-Path -LiteralPath (Join-Path $taskRoot 'src/bridge/bridge_main.cpp')) {
    $compileJobs += New-TaxiJob -Name 'taxi-camera-bridge.dll' -File $compiler -Arguments @($common + '-shared' + (Join-Path $taskRoot 'src/bridge/bridge_main.cpp') + $objects + $libs + '-Wl,--no-insert-timestamp' + '-o' + (Join-Path $out 'taxi-camera-bridge.dll')) -Failure 'Native bridge link failed.'
}
$companion = Test-Path -LiteralPath (Join-Path $taskRoot 'src/app/companion.cpp')
$companionLibs = @('-lbcrypt','-lshell32','-lcomctl32','-lcomdlg32','-ladvapi32','-ldwmapi','-luxtheme','-lwinhttp','-lhid','-lole32','-loleaut32','-lshlwapi','-lmsxml6','-lwindowscodecs')
$setupSources = @($setup | ForEach-Object { Join-Path $taskRoot $_ })
if ($companion) {
    $windres = Join-Path (Split-Path -Parent $compiler) 'llvm-windres.exe'
    $resource = Join-Path $out 'app.res.o'
    & $windres '-I' $generated '-I' (Join-Path $taskRoot 'src/app') '-i' (Join-Path $taskRoot 'src/app/app.rc') '-O' 'coff' '-o' $resource
    if ($LASTEXITCODE -ne 0) { throw 'Windows application manifest compilation failed.' }
    [void](Add-Compile (Join-Path $out 'taxi-cam.exe') (@('-municode','-mwindows',(Join-Path $taskRoot 'src/app/companion.cpp'),(Join-Path $taskRoot 'src/app/updater.cpp')) + $setupSources + $resource + $companionLibs + '-Wl,--no-insert-timestamp') 'Windows companion build failed.')
}
if ($Validate) {
    $graphicsObjects = @($objects | Select-Object -First $graphics.Count)
    $metadataObjects = @($graphicsObjects | Select-Object -Skip 1)
    $runtimeContentionObjects = @($graphicsObjects | Where-Object { (Split-Path -Leaf $_) -ne 'src_graphics_scene_runtime.cpp.o' })
    $stampObjects = @('src_graphics_pfd_stamp_state.cpp.o','src_graphics_pfd_stamp_d3d12.cpp.o','src_hooks_pfd_state_observer.cpp.o') | ForEach-Object { Join-Path $out $_ }
    # The two companion-sized test compiles take longest; start them with the companion link.
    $diagnosticsTest = Add-Compile (Join-Path $out 'companion-diagnostics-test.exe') (@((Join-Path $taskRoot 'tests/app/companion_diagnostics_test.cpp'), (Join-Path $taskRoot 'src/app/updater.cpp')) + $setupSources + '-lgdi32' + $companionLibs) 'Diagnostic controls compilation failed.'
    $hotkeysTest = Add-Compile (Join-Path $out 'camera-hotkeys-test.exe') (@((Join-Path $taskRoot 'tests/app/camera_hotkeys_test.cpp'), (Join-Path $taskRoot 'src/app/updater.cpp')) + $setupSources + '-lgdi32' + $companionLibs) 'Camera shortcut controls compilation failed.'
    $deviceIdentityTest = Add-Compile (Join-Path $out 'native-device-identity-test.exe') (@((Join-Path $taskRoot 'tests/graphics/native_device_identity_test.cpp')) + $libs) 'Native device identity compilation failed.'
    $lateConnect = Add-Compile (Join-Path $out 'late-connect-rendering-test.exe') (@((Join-Path $taskRoot 'tests/graphics/late_connect_rendering_test.cpp')) + $graphicsObjects + $libs) 'Active-rendering late Connect compilation failed.'
    $iconTest = Add-Compile (Join-Path $out 'application-icon-test.exe') @('-municode', (Join-Path $taskRoot 'tests/app/application_icon_test.cpp'), '-lshell32', '-lgdi32') 'Application icon validation compilation failed.'
    $gpu = Add-Compile (Join-Path $out 'native-graphics-validation.exe') (@('-municode', (Join-Path $taskRoot 'tests/graphics/graphics_validation.cpp')) + $graphicsObjects + $libs) 'Native validation compilation failed.'
    $frameOrder = Add-Compile (Join-Path $out 'native-frame-order-validation.exe') (@('-municode', (Join-Path $taskRoot 'tests/graphics/frame_order_validation.cpp')) + $graphicsObjects + $libs) 'Frame-order validation compilation failed.'
    $metadataTest = Add-Compile (Join-Path $out 'metadata-batch-bridge-test.exe') (@((Join-Path $taskRoot 'tests/graphics/metadata_batch_bridge_test.cpp')) + $metadataObjects + $libs) 'Bridge metadata batching regression compilation failed.'
    $lateAttachTest = Add-Compile (Join-Path $out 'late-attach-test.exe') (@((Join-Path $taskRoot 'tests/graphics/late_attach_test.cpp')) + $metadataObjects + $libs) 'Late-attach regression compilation failed.'
    $sourceObservationTest = Add-Compile (Join-Path $out 'source-observation-test.exe') (@('-fno-access-control', (Join-Path $taskRoot 'tests/graphics/source_observation_test.cpp')) + $metadataObjects + $libs) 'Source observation regression compilation failed.'
    $frameOutputTest = Add-Compile (Join-Path $out 'scene-frame-output-test.exe') (@((Join-Path $taskRoot 'tests/graphics/scene_frame_output_test.cpp')) + $graphicsObjects + $libs) 'Lazy patch replay validation compilation failed.'
    $fontGpu = Add-Compile (Join-Path $out 'gs-font-validation.exe') (@('-municode', (Join-Path $taskRoot 'tests/graphics/gs_font_validation.cpp')) + $libs) 'GS font validation compilation failed.'
    $dynamicGpu = Add-Compile (Join-Path $out 'dynamic-state-validation.exe') (@((Join-Path $taskRoot 'tests/graphics/dynamic_state_validation.cpp')) + $stampObjects + $libs) 'Dynamic graphics-state validation compilation failed.'
    $sampleGpu = Add-Compile (Join-Path $out 'sample-positions-validation.exe') (@((Join-Path $taskRoot 'tests/graphics/sample_positions_validation.cpp')) + $stampObjects + $libs) 'Sample-position validation compilation failed.'
    $encodingGpu = Add-Compile (Join-Path $out 'stamp-encoding-validation.exe') (@((Join-Path $taskRoot 'tests/graphics/stamp_encoding_validation.cpp')) + $stampObjects + $libs) 'Stamp encoding validation compilation failed.'
    $tailTest = Add-Compile (Join-Path $out 'scene-queue-tail-test.exe') (@('-fno-access-control', (Join-Path $taskRoot 'tests/graphics/scene_queue_tail_test.cpp')) + $graphicsObjects + $libs) 'Live capture regression compilation failed.'
    $runtimeContention = Add-Compile (Join-Path $out 'scene-runtime-contention-test.exe') (@('-fno-access-control', (Join-Path $taskRoot 'tests/graphics/scene_runtime_contention_test.cpp')) + $runtimeContentionObjects + $libs) 'Camera service contention regression compilation failed.'
    $queuePatchTest = Add-Compile (Join-Path $out 'queue-patch-snapshot-test.exe') (@('-fno-access-control', (Join-Path $taskRoot 'tests/graphics/queue_patch_snapshot_test.cpp')) + $runtimeContentionObjects + $libs) 'Queue patch snapshot compilation failed.'
    $splitSubmissionTest = Add-Compile (Join-Path $out 'pfd-split-submission-test.exe') (@((Join-Path $taskRoot 'tests/graphics/pfd_split_submission_test.cpp')) + $graphicsObjects + $libs) 'Split-list display submission compilation failed.'
    $displayPoolTest = Add-Compile (Join-Path $out 'pfd-submission-pool-test.exe') (@((Join-Path $taskRoot 'tests/graphics/pfd_submission_pool_test.cpp'), (Join-Path $taskRoot 'src/graphics/pfd_submission_pool.cpp'), (Join-Path $taskRoot 'src/hooks/queue_submit_observer.cpp')) + $libs) 'Display submission pool compilation failed.'
    $compositor = Add-Compile (Join-Path $out 'compositor-validation.exe') (@('-municode', (Join-Path $taskRoot 'tests/graphics/compositor_main.cpp')) + $libs) 'Compositor validation compilation failed.'
    $smoke = Add-Compile (Join-Path $out 'native-smoke-validation.exe') @('-municode', (Join-Path $taskRoot 'tests/app/smoke_validation.cpp'), '-ladvapi32') 'Native smoke compilation failed.'
    $logTest = Add-Compile (Join-Path $out 'rotating-log-test.exe') @('-municode', (Join-Path $taskRoot 'tests/app/rotating_log_test.cpp')) 'Runtime log rotation compilation failed.'
    # Wall-clock budgets, lock timing and concurrent-writer tests run one at a time after the pool.
    $timingTests = @('crash-evidence','freeze-guards','hook-timing','lock-hold-stats','lock-holder-priority')
    $nativeTests = @(foreach ($entry in @(
        @{Name='native-launcher-path'; Sources=@('tests/app/launcher_path_test.cpp')},
        @{Name='connection-recoverability'; Sources=@('tests/app/connection_recoverability_test.cpp')},
        @{Name='aircraft-profiles'; Sources=@('tests/app/aircraft_profiles_test.cpp','src/camera/view_resize.cpp','src/camera/local_memory.cpp')},
        @{Name='discovered-aircraft'; Sources=@('tests/app/discovered_aircraft_test.cpp')},
        @{Name='profile-selection'; Sources=@('tests/app/profile_selection_test.cpp')},
        @{Name='waiting-page'; Sources=@('tests/app/waiting_page_test.cpp')},
        @{Name='settings-store'; Sources=@('tests/app/settings_store_test.cpp')},
        @{Name='camera-rate-migration'; Sources=@('tests/app/camera_rate_migration_test.cpp')},
        @{Name='display-snapshot'; Sources=@('tests/app/display_snapshot_test.cpp')},
        @{Name='display-identity'; Sources=@('tests/camera/display_identity_test.cpp')},
        @{Name='notification-settings'; Sources=@('tests/app/notification_settings_test.cpp')},
        @{Name='companion-control'; Sources=@('tests/app/companion_control_test.cpp')},
        @{Name='startup-state'; Sources=@('tests/app/startup_state_test.cpp')},
        @{Name='changelog'; Sources=@('tests/app/changelog_test.cpp'); Args=@((Join-Path $taskRoot 'changelog.json'))},
        @{Name='whats-new-image'; Sources=@('tests/app/whats_new_image_test.cpp'); Libs=@('-lole32','-lwindowscodecs','-lgdi32')},
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
        @{Name='pmdg-display-select'; Sources=@('tests/camera/pmdg_display_select_test.cpp')},
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
        @{Name='render-target-shapes'; Sources=@('tests/graphics/render_target_shapes_test.cpp')},
        @{Name='cloud-merge'; Sources=@('tests/graphics/cloud_merge_test.cpp')},
        @{Name='capture-phase'; Sources=@('tests/graphics/capture_phase_test.cpp')},
        @{Name='display-exposure'; Sources=@('tests/graphics/display_exposure_test.cpp')},
        @{Name='ground-speed-display'; Sources=@('tests/graphics/ground_speed_display_test.cpp')},
        @{Name='calibration'; Sources=@('tests/graphics/calibration_test.cpp')},
        @{Name='write-budget'; Sources=@('tests/graphics/write_budget_test.cpp')},
        @{Name='freeze-guards'; Sources=@('tests/graphics/freeze_guards_test.cpp')},
        @{Name='video-memory'; Sources=@('tests/graphics/video_memory_test.cpp')},
        @{Name='hook-timing'; Sources=@('tests/hooks/hook_timing_test.cpp')},
        @{Name='lock-hold-stats'; Sources=@('tests/hooks/lock_hold_stats_test.cpp')},
        @{Name='lock-holder-priority'; Sources=@('tests/hooks/lock_holder_priority_test.cpp')},
        @{Name='pose-source-cache'; Sources=@('tests/camera/pose_source_cache_test.cpp')},
        @{Name='node-mount'; Sources=@('tests/camera/node_mount_test.cpp')},
        @{Name='view-clip'; Sources=@('tests/camera/view_clip_test.cpp','src/camera/view_clip.cpp','src/camera/local_memory.cpp')},
        @{Name='sim-messages'; Sources=@('tests/app/sim_messages_test.cpp')},
        @{Name='scene-handoff'; Sources=@('tests/graphics/scene_handoff_test.cpp','src/graphics/scene_handoff.cpp')},
        @{Name='source-state'; Sources=@('tests/graphics/scene_source_state_test.cpp','src/graphics/scene_source_state.cpp')},
        @{Name='queue-submit'; Sources=@('tests/hooks/queue_submit_observer_test.cpp','src/hooks/queue_submit_observer.cpp')}
    )) {
        $sources = @($entry.Sources | ForEach-Object { Join-Path $taskRoot $_ })
        $testLibs = @(if ($entry.ContainsKey('Libs')) { $entry.Libs })
        $entry.Exe = Add-Compile (Join-Path $out ($entry.Name + '-test.exe')) ($sources + $libs + '-ladvapi32' + $testLibs) "Native test compilation failed: $($entry.Name)"
        $entry
    })
    $buttonsTest = Add-Compile (Join-Path $out 'camera-buttons-test.exe') @((Join-Path $taskRoot 'tests/app/camera_buttons_test.cpp'), '-lhid') 'Controller button bindings compilation failed.'
    $setupLibs = @('-lole32','-loleaut32','-lshlwapi','-lmsxml6','-lbcrypt')
    $startupXmlTest = Add-Compile (Join-Path $out 'startup-xml-test.exe') (@((Join-Path $taskRoot 'tests/setup/startup_xml_test.cpp'), (Join-Path $taskRoot 'src/setup/startup_xml.cpp'), (Join-Path $taskRoot 'src/setup/setup_common.cpp')) + $setupLibs) 'Startup XML test compilation failed.'
    $prerequisitesTest = Add-Compile (Join-Path $out 'prerequisites-test.exe') (@((Join-Path $taskRoot 'tests/setup/prerequisites_test.cpp'), (Join-Path $taskRoot 'src/setup/prerequisites.cpp'), (Join-Path $taskRoot 'src/setup/setup_common.cpp')) + $setupLibs) 'Prerequisite test compilation failed.'
    $setupHost = Add-Compile (Join-Path $out 'setup-host-test.exe') (@('-municode', '-DTAXI_SETUP_TEST', (Join-Path $taskRoot 'tests/setup/setup_host.cpp')) + $setupSources + $setupLibs) 'Setup command test host compilation failed.'
    $updaterTest = Add-Compile (Join-Path $out 'updater-test.exe') @((Join-Path $taskRoot 'tests/app/updater_test.cpp'), (Join-Path $taskRoot 'src/app/updater.cpp'), '-lbcrypt', '-lshell32', '-lwinhttp') 'Updater test compilation failed.'
    $observerTest = Add-Compile (Join-Path $out 'pfd-state-observer-test.exe') @('-DTAXI_PFD_STATE_OBSERVER_VALIDATION', (Join-Path $taskRoot 'tests/hooks/pfd_state_observer_test.cpp'), (Join-Path $taskRoot 'src/hooks/pfd_state_observer.cpp')) 'PFD state observer test compilation failed.'
}
Invoke-TaxiJobs $compileJobs $throttleLimit
if ($companion) {
    $binaryVersion = (Get-Item -LiteralPath (Join-Path $out 'taxi-cam.exe')).VersionInfo
    if ($binaryVersion.FileVersion -ne $version -or $binaryVersion.ProductVersion -ne $version -or
        "$($binaryVersion.FileMajorPart).$($binaryVersion.FileMinorPart).$($binaryVersion.FileBuildPart)" -ne $version) {
        throw 'Windows executable version differs from the release version.'
    }
}
if ($Validate) {
    $adapters = @(if (-not $WarpOnly) { 'hardware' }) + 'warp'
    # Always wrap calls in @(): a one-element result is returned as a bare string, and string + array concatenates text.
    function Get-AdapterArgs([string]$Adapter) { if ($Adapter -eq 'warp') { @('--warp') } else { @() } }
    [void](Add-Run $deviceIdentityTest @('--warp') 'WARP native device identity regression failed.')
    [void](Add-Run $lateConnect @('--warp') 'WARP active-rendering late Connect failed.' -Timing)
    if (-not $WarpOnly) { [void](Add-Run $lateConnect @() 'Hardware active-rendering late Connect failed.' -Timing) }
    [void](Add-Run $iconTest @((Join-Path $out 'taxi-cam.exe'), (Join-Path $out 'application-icon')) 'Application icon validation failed.')
    [void](Add-Run $frameOrder @('--warp') 'WARP frame-order validation failed.')
    if (-not $WarpOnly) { [void](Add-Run $frameOrder @() 'Hardware frame-order validation failed.') }
    if (-not $WarpOnly) {
        [void](Add-Run $gpu @() 'Hardware native graphics validation failed.')
        [void](Add-Run $gpu @('--a350') 'Hardware A350 graphics validation failed.')
        [void](Add-Run $gpu @('--ini-a380') 'Hardware ini A380 graphics validation failed.')
    } else { Write-Output 'Hardware GPU validation not run: explicit WARP-only CI mode.' }
    [void](Add-Run $gpu @('--warp') 'WARP native graphics validation failed.')
    [void](Add-Run $gpu @('--warp', '--a350') 'WARP A350 graphics validation failed.')
    [void](Add-Run $gpu @('--warp', '--ini-a380') 'WARP ini A380 graphics validation failed.')
    foreach ($queryAdapter in $adapters) {
        foreach ($queryProfile in @('a380', 'a350')) {
            $queryArgs = @('--query-fallback') + @(Get-AdapterArgs $queryAdapter) + @(if ($queryProfile -eq 'a350') { '--a350' })
            [void](Add-Run $gpu $queryArgs "Query-aware PFD regression failed: $queryAdapter / $queryProfile")
        }
    }
    foreach ($copyAdapter in $adapters) {
        foreach ($copyProfile in @('a380', 'a350')) {
            $copyArgs = @('--prefer-copy') + @(Get-AdapterArgs $copyAdapter) + @(if ($copyProfile -eq 'a350') { '--a350' })
            [void](Add-Run $gpu $copyArgs "PFD boundary-copy regression failed: $copyAdapter / $copyProfile")
        }
    }
    [void](Add-Run $metadataTest @() 'Bridge metadata batching regression failed.')
    [void](Add-Run $lateAttachTest @() 'Late-attach regression failed.')
    if (-not $WarpOnly) { [void](Add-Run $sourceObservationTest @() 'Hardware source observation regression failed.') }
    [void](Add-Run $sourceObservationTest @('--warp') 'WARP source observation regression failed.')
    foreach ($grayAdapter in $adapters) {
        [void](Add-Run $gpu (@('--textured-gray') + @(Get-AdapterArgs $grayAdapter)) "Textured gray PFD fallback regression failed: $grayAdapter")
    }
    if (-not $WarpOnly) { [void](Add-Run $frameOutputTest @() 'Hardware lazy patch replay validation failed.') }
    [void](Add-Run $frameOutputTest @('--warp') 'WARP lazy patch replay validation failed.')
    if (-not $WarpOnly) { [void](Add-Run $fontGpu @() 'Hardware GS font validation failed.') }
    [void](Add-Run $fontGpu @('--warp') 'WARP GS font validation failed.')
    $dynamicStateJobs = [ordered]@{}
    $sampleJobs = [ordered]@{}
    foreach ($adapter in $adapters) {
        $dynamicStateJobs[$adapter] = Add-Run $dynamicGpu @(Get-AdapterArgs $adapter) "Dynamic graphics-state regression failed: $adapter" -Ok @(0, 77)
        $sampleJobs[$adapter] = Add-Run $sampleGpu @(Get-AdapterArgs $adapter) "Sample-position graphics regression failed: $adapter" -Ok @(0, 77)
        [void](Add-Run $encodingGpu @(Get-AdapterArgs $adapter) "Stamp encoding validation failed: $adapter")
    }
    [void](Add-Run $tailTest @('--submission-locks') 'Submission lock ordering regression failed.' -Timing)
    foreach ($tailAdapter in $adapters) {
        foreach ($tailOrigin in @('transition', 'creation')) {
            $tailArgs = @(Get-AdapterArgs $tailAdapter) + @(if ($tailOrigin -eq 'creation') { '--born-render-target' })
            [void](Add-Run $tailTest $tailArgs "Live capture regression failed: $tailAdapter / $tailOrigin" -Timing)
        }
    }
    [void](Add-Run $runtimeContention @('--warp') 'WARP camera service contention regression failed.' -Timing)
    if (-not $WarpOnly) { [void](Add-Run $runtimeContention @() 'Hardware camera service contention regression failed.' -Timing) }
    foreach ($displayAdapter in @('warp','hardware')) {
        if ($WarpOnly -and $displayAdapter -eq 'hardware') { continue }
        $displayArgs = @(Get-AdapterArgs $displayAdapter)
        [void](Add-Run $queuePatchTest $displayArgs "Queue patch snapshot failed: $displayAdapter" -Timing)
        [void](Add-Run $queuePatchTest ($displayArgs + '--a350') "A350 queue patch snapshot failed: $displayAdapter" -Timing)
        foreach ($displayProfile in @('ini','fbw','fbw-three')) {
            foreach ($displayState in @('srv','common')) {
                foreach ($displayPlacement in @('separate','mixed-exit','first-list')) {
                    $splitArgs = $displayArgs + @(if ($displayProfile -ne 'ini') { "--$displayProfile" }) +
                        @(if ($displayState -eq 'common') { '--common' }) + @(if ($displayPlacement -ne 'separate') { "--$displayPlacement" })
                    [void](Add-Run $splitSubmissionTest $splitArgs "Display submission failed: $displayAdapter / $displayProfile / $displayState / $displayPlacement")
                }
            }
        }
        [void](Add-Run $displayPoolTest $displayArgs "Display submission pool failed: $displayAdapter")
        [void](Add-Run $displayPoolTest ($displayArgs + '--common') "COMMON display submission pool failed: $displayAdapter")
        [void](Add-Run $displayPoolTest ($displayArgs + '--before') "Before-list display submission pool failed: $displayAdapter")
    }
    if (-not $WarpOnly) { [void](Add-Run $compositor @() 'Hardware compositor validation failed.') }
    [void](Add-Run $compositor @('--warp') 'WARP compositor validation failed.')
    if (-not $WarpOnly) { [void](Add-Run $gpu @('--profile-switch') 'Hardware active-feed aircraft-switch validation failed.') }
    [void](Add-Run $gpu @('--warp', '--profile-switch') 'WARP active-feed aircraft-switch validation failed.')
    [void](Add-Run $smoke @((Join-Path $out 'taxi-camera-bridge.dll')) 'Exact native DLL smoke failed.')
    [void](Add-Run $logTest @() 'Runtime log rotation regression failed.' -Timing)
    foreach ($entry in $nativeTests) {
        $testArgs = @(if ($entry.ContainsKey('Args')) { $entry.Args })
        [void](Add-Run $entry.Exe $testArgs "Native test failed: $($entry.Name)" -Timing:($timingTests -contains $entry.Name))
        if ($entry.Name -eq 'queue-submit') { [void](Add-Run $entry.Exe @('augment') 'Queue submission augmentation failed.') }
    }
    [void](Add-Run $diagnosticsTest @() 'Diagnostic controls validation failed.')
    [void](Add-Run $hotkeysTest @() 'Camera shortcut controls validation failed.')
    [void](Add-Run $buttonsTest @() 'Controller button bindings validation failed.')
    [void](Add-Run $startupXmlTest @() 'Startup XML tests failed.')
    [void](Add-Run $prerequisitesTest @() 'Prerequisite tests failed.')
    [void](Add-Run $updaterTest @() 'Updater tests failed.')
    [void](Add-Run $observerTest @() 'PFD state observer lifecycle tests failed.')
    [void](Add-Run $observerTest @('--optional') 'PFD state observer optional callback tests failed.')
    Invoke-TaxiJobs $runJobs $runThrottle
    Invoke-TaxiJobs $timingJobs 1
    $dynamicStateResults = @(foreach ($adapter in $dynamicStateJobs.Keys) {
        [ordered]@{adapter=$adapter; result=$(if ($dynamicStateJobs[$adapter].ExitCode -eq 77) { 'unsupported; skipped' } else { 'passed' })}
    })
    $sampleResults = @(foreach ($adapter in $sampleJobs.Keys) {
        [ordered]@{adapter=$adapter; result=$(if ($sampleJobs[$adapter].ExitCode -eq 77) { 'unsupported; skipped' } else { 'passed' })}
    })
    # These scripts change process-wide state or depend on each other, so they keep their order.
    & (Join-Path $taskRoot 'tests/installer/setup_commands_test.ps1') -SetupHost $setupHost
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
    foreach ($source in @($graphics + $engine + @('src/bridge/bridge_main.cpp','src/app/companion.cpp','src/app/updater.cpp') + $setup)) {
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
        tests=@($gpuTests + @('pre-existing graphics objects','late Connect during active native rendering, Present and CPU work','exact bridge rejects D3D11On12 bootstrap and thread-suspending hook dependencies','per-profile marking colour persistence, IPC and GPU pixel isolation','launcher phase timing and distinct wait outcomes','terminal command-list PFD drawing without application root replay','textured gray alpha and mip preservation with terminal PFD draws and repeated submissions','scoped barrier metadata lookup caching','descriptor heap filtering and retained device identity','unrelated submission bypass and discovery lock ordering','per-recording PFD copy evidence and draw fallback','preferred OM/Close copy pixel and query preservation','dynamic graphics-state replay and ABI','programmable sample-pattern normalization and restoration','camera codes preserved through UNORM and sRGB display views with aircraft-UI overlay encoding','isolated graphics-state regression coverage','lazy typed patch demand, discard and cross-profile replay','fractional ground-speed truncation','camera border and inset composition','antialiased GS glyphs and two-character spacing','ClearState pipeline preservation','native render-pass state preservation','private PFD patch copies','selected PFD copies in large barrier batches','PFD exits across command lists','typed PFD view evidence','application occlusion-query preservation','query-aware PFD drawing and OM restoration','calibration OM and Close delivery','retained camera dimension recovery','retained native aircraft transitions and request tokens','current-process memory query equivalence','close-only activation inspection','pane output admission before gate opening','bounded memory query reuse','shared read-only inspection transaction with fresh endpoint validation','early camera preparation and activation timings','bounded grounded prewarm and retained foreground takeover','idle camera inspection scheduling','large barrier batches and changing camera frames','exact DLL smoke',
            'settings persistence and IPC','per-user first-launch Settings visibility, failure retry and preview isolation','per-aircraft reference-guide persistence','live reference-guide GPU updates','scene demand and retained camera ownership','companion contention and watchdog','configurable global camera shortcuts, native conflict recovery, hidden-window dispatch, preview isolation and manual-only controls','launcher file identity','native COM slots','TAXI routing','profile-switch target reacquisition','active-feed A380-A350-A380 transitions with retained sources','manual and automatic target selection','PFD detector','exposure','calibration','write budget',
            'compositor formats and exposure','scene handoff and resource state','camera ownership','bug report URL encoding, bounds and diagnostic privacy','relocatable native instruction discovery and static RTTI identity','complete camera contract on relocated synthetic PE images and refusal controls','queue submit','PFD state observer lifecycle','render boundary','engine hook','camera telemetry and lifecycle','aircraft layout compatibility','exe.xml preservation and rename migration',
            'bounded weak command-list lookup reuse and idle observation epochs','continuous source-state observation with idle capture suppression','optional fenced GPU timestamps on owned recordings','5-60 camera activation limits and low-rate scheduling',
            'bounded rotating runtime logs, oversized history and concurrent writers','bounded renderer-fault retention with active-record preservation',
            'both native release queues and first-free view retirement admission','partial camera-pair cleanup and deferred startup with request cancellation',
            'nonblocking telemetry shutdown with retained worker ownership','private composition contention retry with retained leases and GPU pixels','nonblocking helper refusal and retained GPU submission ordering',
            'same-submit iniBuilds and FBW camera/calibration pixels after barrier-only lists and before mixed exit/consumer lists including first-list insertion, replay and subsequent native overwrite','exact RT exit proof including COMMON and SRV, pass and generation refusal','bounded display copy packets with fence-held source/target lifetime and mip preservation','prepared camera/calibration patches with nonblocking snapshots and manager timeline ordering','late display submission activity with unchanged automatic side ordering',
            'native imports and header dependency closure','embedded multi-resolution application icon and Windows shell extraction','release selection, download integrity and updater handoff guards',
            'bounded simulator-thread lock budgets, deferred lifecycle ring and presentation watchdog','unordered submission notification and presentation pulse statistics','tray notification events, toast policy, edges, rate limit and IPC log','notification setting migration from [messages] in_simulator'));
        gpuValidation=[ordered]@{hardware=$(if ($WarpOnly) { 'not-run' } else { 'passed' });warp='passed'};
        simulatorVerified=$false
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'validation.json') -Encoding utf8

}
Write-Output "Native build complete: $out "
