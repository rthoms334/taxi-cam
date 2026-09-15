$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repoRoot 'ci/toolchain.ps1')
$managerRoot = $repoRoot
$managerBuild = Join-Path $managerRoot 'build'
$managerCompiler = Join-Path (Get-TaxiToolchain $repoRoot) 'clang++.exe'
$managerFlags = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DNOMINMAX', '-DTAXI_RENDER_BOUNDARY_STATE_VALIDATION', '-mno-avx', '-mno-avx2', '-mno-avx512f')
$managerObjects = @()
foreach ($managerSource in @('scene_capture_manager', 'scene_capture_d3d12', 'scene_handoff', 'scene_source_state')) {
  $managerObject = Join-Path $managerBuild "$managerSource-validation.o"
  $managerExceptionFlags = @('-fno-exceptions')
  if ($managerSource -eq 'scene_handoff') { $managerExceptionFlags = @() }
  & $managerCompiler @managerFlags @managerExceptionFlags -c (Join-Path $repoRoot "src/graphics/$managerSource.cpp") -o $managerObject
  if ($LASTEXITCODE -ne 0) { throw "$managerSource compile failed." }
  $managerObjects += $managerObject
}
$managerQueueObject = Join-Path $managerBuild 'queue-submit-tail-validation.o'
& $managerCompiler @managerFlags -c (Join-Path $managerRoot 'src/hooks/queue_submit_observer.cpp') -o $managerQueueObject
if ($LASTEXITCODE -ne 0) { throw 'Native queue tail observer compilation failed.' }
$managerBoundaryObject = Join-Path $managerBuild 'render-boundary-gpu-validation.o'
& $managerCompiler @managerFlags -c (Join-Path $managerRoot 'src/hooks/render_boundary_observer.cpp') -o $managerBoundaryObject
if ($LASTEXITCODE -ne 0) { throw 'Native render boundary GPU compilation failed.' }
$managerObjects += $managerBoundaryObject
$tailExecutable = Join-Path $managerBuild 'scene-queue-tail-test.exe'
& $managerCompiler @managerFlags -fno-access-control -static (Join-Path $repoRoot 'tests/graphics/scene_queue_tail_test.cpp') @managerObjects $managerQueueObject -ld3d12 -ld3dcompiler -ldxgi -ldxguid -o $tailExecutable
if ($LASTEXITCODE -ne 0) { throw 'Queue tail test compile failed.' }
foreach ($tailAdapter in @('hardware', 'warp')) {
  foreach ($tailModel in @('legacy', 'enhanced')) {
   foreach ($tailOrigin in @('transition', 'creation')) {
    $tailArguments = @()
    if ($tailAdapter -eq 'warp') { $tailArguments += '--warp' }
    if ($tailModel -eq 'enhanced') { $tailArguments += '--enhanced' }
    if ($tailOrigin -eq 'creation') { $tailArguments += '--born-render-target' }
    $tailOutput = & $tailExecutable @tailArguments
    if ($LASTEXITCODE -ne 0) { throw "Queue tail validation failed: $tailAdapter/$tailModel." }
    $tailResult = $tailOutput | ConvertFrom-Json
    # Three complete pairs at the fixture's current 736 x (251 + 496) dimensions.
    if ($tailResult.passed -ne $true -or $tailResult.checked_pixels -ne (3 * 736 * (251 + 496)) -or $tailResult.tail_captures -ne 6 -or
        $tailResult.reset_receipt_lease -ne $true -or $tailResult.stop_releases_sources -ne $true -or $tailResult.tail_device_reuse -ne $true) {
      throw 'Invalid queue tail receipt.'
    }
    $tailReceipt = "scene-queue-tail-$tailModel-$tailAdapter.json"
    if ($tailOrigin -eq 'creation') {
      if ($tailResult.born_render_target -ne $true) { throw 'Missing native creation-state proof.' }
      $tailReceipt = "scene-queue-tail-born-$tailModel-$tailAdapter.json"
    }
    $tailOutput | Set-Content -LiteralPath (Join-Path $managerBuild $tailReceipt) -Encoding utf8
    Write-Output $tailOutput
   }
  }
}
$managerExecutable = Join-Path $managerBuild 'scene-capture-manager-test.exe'
& $managerCompiler @managerFlags -static (Join-Path $repoRoot 'tests/graphics/scene_capture_manager_test.cpp') @managerObjects -ld3d12 -ld3dcompiler -ldxgi -ldxguid -o $managerExecutable
if ($LASTEXITCODE -ne 0) { throw 'Manager test compile failed.' }
foreach ($managerAdapter in @('hardware', 'warp')) {
  foreach ($managerMode in @('copy', 'render-target', 'enhanced-target', 'boundary-legacy', 'boundary-enhanced', 'boundary-pass-legacy', 'boundary-pass-enhanced', 'native-copy', 'native-texture-copy')) {
    $managerArguments = @()
    if ($managerAdapter -eq 'warp') { $managerArguments = @('--warp') }
    if ($managerMode -eq 'boundary-legacy') { $managerArguments += @('--render-target', '--boundary-observer') }
    elseif ($managerMode -eq 'boundary-enhanced') { $managerArguments += @('--enhanced-target', '--boundary-observer') }
    elseif ($managerMode -eq 'boundary-pass-legacy') { $managerArguments += @('--render-target', '--boundary-observer', '--completed-pass') }
    elseif ($managerMode -eq 'boundary-pass-enhanced') { $managerArguments += @('--enhanced-target', '--boundary-observer', '--completed-pass') }
    elseif ($managerMode -ne 'copy') { $managerArguments += "--$managerMode" }
    $managerOutput = & $managerExecutable @managerArguments
    if ($LASTEXITCODE -ne 0) { throw "Manager validation failed on $managerAdapter." }
    $managerResult = $managerOutput | ConvertFrom-Json
    if ($managerMode -in @('enhanced-target', 'boundary-enhanced', 'boundary-pass-enhanced') -and $managerResult.PSObject.Properties['skipped'] -and
        $managerResult.skipped -eq $true -and $managerResult.passed -eq $true) {
      $managerOutput | Set-Content -LiteralPath (Join-Path $managerBuild "scene-capture-manager-$managerMode-$managerAdapter.json") -Encoding utf8
      Write-Output $managerOutput
      continue
    }
    if ($managerResult.passed -ne $true -or $managerResult.checked_pixels -ne 1536) { throw 'Invalid manager receipt.' }
    if ($managerMode -notin @('copy', 'native-copy', 'native-texture-copy') -and ($managerResult.render_target_writes -ne 4 -or $managerResult.render_target_rewrites -ne 2)) {
      throw 'Invalid repeated render-target capture receipt.'
    }
    $managerReceipt = "scene-capture-manager-$managerAdapter.json"
    if ($managerMode -eq 'render-target') { $managerReceipt = "scene-capture-manager-rt-$managerAdapter.json" }
    if ($managerMode -eq 'enhanced-target') { $managerReceipt = "scene-capture-manager-enhanced-$managerAdapter.json" }
    if ($managerMode -like 'boundary-*') {
      if ($managerResult.native_boundary -ne $true -or $managerResult.boundary_captures -ne 4) { throw 'Invalid native boundary receipt.' }
      $managerReceipt = "scene-capture-manager-$managerMode-$managerAdapter.json"
      if ($managerMode -like 'boundary-pass-*' -and $managerResult.completed_pass_proof -ne $true) { throw 'Missing completed-pass proof.' }
    }
    if ($managerMode -in @('native-copy', 'native-texture-copy')) {
      if ($managerResult.native_copy -ne $true -or $managerResult.native_copy_captures -ne 4) { throw 'Invalid native copy receipt.' }
      if ($managerMode -eq 'native-texture-copy' -and $managerResult.texture_copy -ne $true) { throw 'Missing texture copy proof.' }
      $managerReceipt = "scene-capture-manager-$managerMode-$managerAdapter.json"
    }
    $managerOutput | Set-Content -LiteralPath (Join-Path $managerBuild $managerReceipt) -Encoding utf8
    Write-Output $managerOutput
  }
}
