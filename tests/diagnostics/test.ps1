param()
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repoRoot 'ci/toolchain.ps1')
$discoveryRoot = $PSScriptRoot
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
$calleeTestExecutable = Join-Path $outputDirectory 'callee-prefix-test.exe'
& $compiler @common (Join-Path $repoRoot 'tools/diagnostics/callee_prefix.cpp') (Join-Path $repoRoot 'tests/diagnostics/callee_prefix_test.cpp') '-o' $calleeTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Proven-callee prefix tests failed to compile.' }
& $calleeTestExecutable
if ($LASTEXITCODE -ne 0) { throw 'Proven-callee prefix tests failed.' }
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
& $compiler @common (Join-Path $repoRoot 'tools/diagnostics/image_inventory.cpp') (Join-Path $repoRoot 'tools/diagnostics/llvm_decoder.cpp') (Join-Path $repoRoot 'tools/diagnostics/main_module_inventory.cpp') (Join-Path $repoRoot 'tools/diagnostics/main.cpp') '-ladvapi32' '-o' $inventoryExecutable
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
Write-Output "PASS: native self-mode ($($selfResult.scanned_bytes) bytes, $($selfResult.elapsed_ms) ms), JSON output and explicit-PID process-name restriction."
# Windows PowerShell turns redirected native stderr into an error under Stop.
$ErrorActionPreference = 'Continue'
& $inventoryExecutable '--pid' "$PID" '--camera-context' 2>$null
$removedMode = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($removedMode -ne 2) { throw 'Removed build-specific modes must be rejected as invalid arguments.' }
Write-Output "Built: $inventoryExecutable"
