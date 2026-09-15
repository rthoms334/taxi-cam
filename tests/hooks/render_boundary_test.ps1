[CmdletBinding()]
param([string]$Compiler)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$taskRoot = $repoRoot
$taskDependencies = Get-Content -Raw -LiteralPath (Join-Path $taskRoot 'dependencies.json') | ConvertFrom-Json
if (-not $Compiler) { $Compiler = Join-Path $taskRoot ('build/deps/' + $taskDependencies.'llvm-mingw'.directory + '/bin/clang++.exe') }
$taskOutput = Join-Path $taskRoot 'build/render-boundary-validation'
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
$taskExecutable = Join-Path $taskOutput 'render-boundary-observer-test.exe'
$taskCommon = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Werror', '-mno-avx', '-mno-avx2', '-mno-avx512f')
& $Compiler @taskCommon '-DTAXI_RENDER_BOUNDARY_VALIDATION' (Join-Path $taskRoot 'tests/hooks/render_boundary_observer_test.cpp') `
  (Join-Path $taskRoot 'src/hooks/render_boundary_observer.cpp') '-static' '-o' $taskExecutable
if ($LASTEXITCODE -ne 0) { throw 'Native render boundary observer strict compilation failed.' }
$taskResult = & $taskExecutable
if ($LASTEXITCODE -ne 0) { throw 'Native render boundary observer refusal/lifecycle tests failed.' }
$taskResult | Write-Output
$taskReceipt = [ordered]@{
  binarySha256 = (Get-FileHash -LiteralPath $taskExecutable -Algorithm SHA256).Hash
  result = ($taskResult | ConvertFrom-Json)
  limitation = 'Synthetic public COM calls in an isolated process; actual hardware/WARP capture ordering is tested separately by scene_capture_manager_test.ps1 --boundary-observer modes.'
}
$taskReceipt | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $taskOutput 'result.json') -Encoding utf8

$selectedExecutable = Join-Path $taskOutput 'selected-boundary-proof.exe'
& $Compiler @taskCommon '-DTAXI_RENDER_BOUNDARY_VALIDATION' '-DTAXI_SELECTED_BARRIER_FIX' `
  (Join-Path $taskRoot 'tests/graphics/selected_boundary_proof.cpp') `
  (Join-Path $taskRoot 'src/hooks/render_boundary_observer.cpp') '-static' '-o' $selectedExecutable
if ($LASTEXITCODE -ne 0) { throw 'Selected PFD boundary proof compilation failed.' }
$selectedResult = & $selectedExecutable
if ($LASTEXITCODE -ne 0) { throw 'Selected PFD boundary refusal/large-batch proof failed.' }
$selectedResult | Write-Output
[ordered]@{
  binarySha256 = (Get-FileHash -LiteralPath $selectedExecutable -Algorithm SHA256).Hash
  result = ($selectedResult | ConvertFrom-Json)
  limitation = 'Isolated COM fixture; selected PFD copies are also tested on hardware and WARP.'
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $taskOutput 'selected-result.json') -Encoding utf8
