param()
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
& (Join-Path $PSScriptRoot 'benchmark_compare_test.ps1')
. (Join-Path $repoRoot 'ci/toolchain.ps1')
$discoveryRoot = $PSScriptRoot
$nativeRoot = $repoRoot
$compiler = Join-Path (Get-TaxiToolchain $repoRoot) 'clang++.exe'
if (-not (Test-Path -LiteralPath $compiler)) {
  throw 'Run ./bootstrap.ps1 to install the pinned compiler.'
}
$outputDirectory = Join-Path $repoRoot 'build/tools/diagnostics'
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$common = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-DNOMINMAX', '-D_WIN32_WINNT=0x0A00', '-static')
Write-TaxiLlvmConfig -Repository $repoRoot -OutputDirectory $outputDirectory
$common += @('-I', $outputDirectory)
$testExecutable = Join-Path $outputDirectory 'inventory-test.exe'
& $compiler @common (Join-Path $repoRoot 'tools/diagnostics/image_inventory.cpp') (Join-Path $repoRoot 'tools/diagnostics/reference_inventory.cpp') (Join-Path $repoRoot 'tests/diagnostics/inventory_test.cpp') '-o' $testExecutable
if ($LASTEXITCODE -ne 0) { throw 'Synthetic inventory tests failed to compile.' }
& $testExecutable
if ($LASTEXITCODE -ne 0) { throw 'Synthetic inventory tests failed.' }
$pointerTestExecutable = Join-Path $outputDirectory 'pointer-inventory-test.exe'
$callTestExecutable = Join-Path $outputDirectory 'call-reference-test.exe'
& $compiler @common (Join-Path $repoRoot 'tools/diagnostics/image_inventory.cpp') (Join-Path $repoRoot 'tools/diagnostics/reference_inventory.cpp') (Join-Path $repoRoot 'tests/diagnostics/call_reference_test.cpp') '-o' $callTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Direct-call reference tests failed to compile.' }
& $callTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Direct-call reference tests failed.' }
& $compiler @common (Join-Path $repoRoot 'tools/diagnostics/image_inventory.cpp') (Join-Path $repoRoot 'tools/diagnostics/pointer_inventory.cpp') (Join-Path $repoRoot 'tests/diagnostics/pointer_inventory_test.cpp') '-o' $pointerTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Synthetic pointer-table tests failed to compile.' }
& $pointerTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Synthetic pointer-table tests failed.' }
$guardTestExecutable = Join-Path $outputDirectory 'guard-inventory-test.exe'
& $compiler @common (Join-Path $repoRoot 'tools/diagnostics/image_inventory.cpp') (Join-Path $repoRoot 'tools/diagnostics/guard_inventory.cpp') (Join-Path $repoRoot 'tests/diagnostics/guard_inventory_test.cpp') '-o' $guardTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Synthetic CFG metadata tests failed to compile.' }
& $guardTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Synthetic CFG metadata tests failed.' }
$inventoryExecutable = Join-Path $outputDirectory 'camera-interface-inventory.exe'
$commandListTestExecutable = Join-Path $outputDirectory 'command-list-type-test.exe'
& $compiler @common (Join-Path $repoRoot 'tools/diagnostics/command_list_inventory.cpp') (Join-Path $repoRoot 'tests/diagnostics/command_list_inventory_test.cpp') '-o' $commandListTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Fixed command-list type tests failed to compile.' }
& $commandListTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Fixed command-list type tests failed.' }
$calleeTestExecutable = Join-Path $outputDirectory 'callee-prefix-test.exe'
& $compiler @common (Join-Path $repoRoot 'tools/diagnostics/callee_prefix.cpp') (Join-Path $repoRoot 'tests/diagnostics/callee_prefix_test.cpp') '-o' $calleeTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Proven-callee prefix tests failed to compile.' }
& $calleeTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Proven-callee prefix tests failed.' }
$serviceTestExecutable = Join-Path $outputDirectory 'service-inventory-test.exe'
& $compiler @common (Join-Path $repoRoot 'tools/diagnostics/service_inventory.cpp') (Join-Path $repoRoot 'tests/diagnostics/service_inventory_test.cpp') '-o' $serviceTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Service pointer metadata tests failed to compile.' }
& $serviceTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Service pointer metadata tests failed.' }
$literalTestExecutable = Join-Path $outputDirectory 'static-literals-test.exe'
& $compiler @common (Join-Path $repoRoot 'tools/diagnostics/static_literals.cpp') (Join-Path $repoRoot 'tests/diagnostics/static_literals_test.cpp') '-o' $literalTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Static literal tests failed to compile.' }
& $literalTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Static literal tests failed.' }
$numericTestExecutable = Join-Path $outputDirectory 'static-numeric-test.exe'
& $compiler @common (Join-Path $repoRoot 'tools/diagnostics/static_numeric.cpp') (Join-Path $repoRoot 'tests/diagnostics/static_numeric_test.cpp') '-o' $numericTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Static numeric tests failed to compile.' }
& $numericTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Static numeric tests failed.' }
$codeTestExecutable = Join-Path $outputDirectory 'code-contract-test.exe'
& $compiler @common (Join-Path $nativeRoot 'src/camera/code_contract.cpp') (Join-Path $nativeRoot 'tests/camera/code_contract_test.cpp') '-o' $codeTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Loaded-code contract tests failed to compile.' }
& $codeTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Loaded-code contract tests failed.' }
$aircraftTestExecutable = Join-Path $outputDirectory 'aircraft-inventory-test.exe'
& $compiler @common (Join-Path $repoRoot 'src/camera/aircraft_inventory.cpp') (Join-Path $repoRoot 'tests/camera/aircraft_inventory_test.cpp') '-o' $aircraftTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Aircraft metadata tests failed to compile.' }
& $aircraftTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Aircraft metadata tests failed.' }
$slotTestExecutable = Join-Path $outputDirectory 'slot-prefix-test.exe'
& $compiler @common (Join-Path $repoRoot 'tools/diagnostics/slot_prefix.cpp') (Join-Path $repoRoot 'tests/diagnostics/slot_prefix_test.cpp') '-o' $slotTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Static-slot prefix tests failed to compile.' }
& $slotTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Static-slot prefix tests failed.' }
$importTestExecutable = Join-Path $outputDirectory 'import-slot-test.exe'
& $compiler @common (Join-Path $repoRoot 'tools/diagnostics/import_slot.cpp') (Join-Path $repoRoot 'tests/diagnostics/import_slot_test.cpp') '-o' $importTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Import-slot metadata tests failed to compile.' }
& $importTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Import-slot metadata tests failed.' }
$rttiTestExecutable = Join-Path $outputDirectory 'rtti-metadata-test.exe'
& $compiler @common (Join-Path $repoRoot 'tools/diagnostics/rtti_metadata.cpp') (Join-Path $repoRoot 'tests/diagnostics/rtti_metadata_test.cpp') '-o' $rttiTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Numeric RTTI metadata tests failed to compile.' }
& $rttiTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Numeric RTTI metadata tests failed.' }
& $compiler @common (Join-Path $repoRoot 'tools/diagnostics/image_inventory.cpp') (Join-Path $repoRoot 'tools/diagnostics/reference_inventory.cpp') (Join-Path $repoRoot 'tools/diagnostics/pointer_inventory.cpp') (Join-Path $repoRoot 'tools/diagnostics/guard_inventory.cpp') (Join-Path $repoRoot 'tools/diagnostics/static_literals.cpp') (Join-Path $repoRoot 'tools/diagnostics/service_inventory.cpp') (Join-Path $repoRoot 'src/camera/aircraft_inventory.cpp') (Join-Path $repoRoot 'tools/diagnostics/slot_prefix.cpp') (Join-Path $repoRoot 'tools/diagnostics/callee_prefix.cpp') (Join-Path $repoRoot 'tools/diagnostics/import_slot.cpp') (Join-Path $repoRoot 'tools/diagnostics/rtti_metadata.cpp') (Join-Path $repoRoot 'tools/diagnostics/command_list_inventory.cpp') (Join-Path $repoRoot 'tools/diagnostics/llvm_decoder.cpp') (Join-Path $repoRoot 'tools/diagnostics/static_numeric.cpp') (Join-Path $nativeRoot 'src/camera/code_contract.cpp') (Join-Path $nativeRoot 'src/camera/activation_mask.cpp') (Join-Path $nativeRoot 'src/camera/verified_profile.cpp') (Join-Path $nativeRoot 'src/camera/source_view.cpp') (Join-Path $repoRoot 'tools/diagnostics/main_module_inventory.cpp') (Join-Path $repoRoot 'tools/diagnostics/main.cpp') '-ladvapi32' '-o' $inventoryExecutable
if ($LASTEXITCODE -ne 0) { throw 'Native inventory helper failed to compile.' }
& $inventoryExecutable '--decoder-self-test'
if ($LASTEXITCODE -ne 0) { throw 'The real LLVM C decoder self-test failed.' }
$selfOutput = & $inventoryExecutable '--self'
if ($LASTEXITCODE -ne 0) { throw 'Reading the helper own main image failed.' }
$selfResult = ($selfOutput -join [Environment]::NewLine) | ConvertFrom-Json
if (-not $selfResult.main_module_verified -or -not $selfResult.valid_image -or $selfResult.module_name -ne 'camera-interface-inventory.exe') {
  throw 'Self-mode module identity validation failed.'
}
$rejectedOutput = & $inventoryExecutable '--pid' "$PID"
if ($LASTEXITCODE -ne 1) { throw 'The PID executable-name restriction did not reject the PowerShell test host.' }
$rejectedResult = ($rejectedOutput -join [Environment]::NewLine) | ConvertFrom-Json
if ($rejectedResult.main_module_verified -or $rejectedResult.error -ne 'The selected PID is not FlightSimulator2024.exe.') {
  throw 'Unexpected result from the process-name restriction test.'
}
foreach ($cameraMode in @('--camera-context', '--camera-methods', '--camera-view-setup', '--camera-view-state', '--camera-service', '--camera-leaves', '--camera-aircraft', '--camera-command-list')) {
  $contextRejectedOutput = & $inventoryExecutable '--pid' "$PID" $cameraMode
  if ($LASTEXITCODE -ne 1) { throw "$cameraMode did not reject the PowerShell test host." }
  $contextRejectedResult = ($contextRejectedOutput -join [Environment]::NewLine) | ConvertFrom-Json
  if ($contextRejectedResult.main_module_verified -or -not $contextRejectedResult.functions_requested -or
      $contextRejectedResult.error -ne 'The selected PID is not FlightSimulator2024.exe.' -or
      $contextRejectedResult.function_contexts.functions.Count -ne 0) {
    throw "Unexpected result from the $cameraMode process-name restriction test."
  }
  if ($cameraMode -eq '--camera-aircraft') {
    $pose = $contextRejectedResult.source_pose
    if ($pose.complete -or $pose.status -ne 'not_inspected' -or $pose.read_bytes -ne 0 -or
        $contextRejectedResult.source_pose_code_contract.valid -or
        @($pose.PSObject.Properties.Name | Where-Object { $_ -match 'address|pointer' }).Count -ne 0) {
      throw 'A rejected PID exposed source-pose addresses or performed source-pose reads.'
    }
  }
  if ($cameraMode -eq '--camera-command-list') {
    $type = $contextRejectedResult.command_list_type
    if ($type.valid -or $type.available -or $type.image_bytes -ne 0 -or $type.object_bytes -ne 0 -or
        @($type.PSObject.Properties.Name | Where-Object { $_ -match 'address|pointer' }).Count -ne 0) {
      throw 'A rejected PID exposed command-list addresses or performed type-field reads.'
    }
  }
  & $inventoryExecutable '--self' $cameraMode 2>$null
  if ($LASTEXITCODE -ne 2) { throw "$cameraMode must require an explicit simulator PID." }
}
Write-Output "PASS: native self-mode ($($selfResult.scanned_bytes) bytes, $($selfResult.elapsed_ms) ms), JSON output and explicit-PID process-name restriction."
$callRejectedOutput = & $inventoryExecutable '--pid' "$PID" '--camera-callers'
if ($LASTEXITCODE -ne 1) { throw 'Caller mode did not reject the PowerShell test host.' }
$callRejectedResult = ($callRejectedOutput -join [Environment]::NewLine) | ConvertFrom-Json
if ($callRejectedResult.main_module_verified -or -not $callRejectedResult.callers_requested -or
    $callRejectedResult.error -ne 'The selected PID is not FlightSimulator2024.exe.' -or
    $callRejectedResult.reference_scan.references.Count -ne 0) { throw 'Unexpected caller-mode refusal result.' }
& $inventoryExecutable '--self' '--camera-callers' 2>$null
if ($LASTEXITCODE -ne 2) { throw 'Caller mode must require an explicit simulator PID.' }
Write-Output "Built: $inventoryExecutable"
