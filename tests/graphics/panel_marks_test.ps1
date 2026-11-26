param([Parameter(Mandatory=$true)][string]$PanelMarksTest)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) {
    throw 'Node.js is required to validate panel-marks JS placement.'
}

& $node.Source (Join-Path $PSScriptRoot 'panel_marks_test.js')
if ($LASTEXITCODE -ne 0) { throw 'panel_marks_test.js failed.' }

$outDir = Join-Path $repoRoot 'build/tests/panel-marks'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$helper = Join-Path $repoRoot 'src/panel_marks/taxi-cam-marks.js'
$stub = Join-Path $repoRoot 'tools/panel-marks/stub-page.js'
$vc03Raw = Join-Path $outDir 'efis_stacked_vc03.raw'
$vc04Raw = Join-Path $outDir 'efis_stacked_vc04.raw'
$cornerRaw = Join-Path $outDir 'corner_guard.raw'

$vc03Out = & $node.Source $stub $helper (Join-Path $repoRoot 'tools/panel-marks/fixtures/efis_stacked_vc03.json') $vc03Raw 1 4
if ($LASTEXITCODE -ne 0) { throw 'stub-page.js failed for efis_stacked_vc03.' }
if ($vc03Out -notmatch 'blocks drawn=2 .* at=1606,2 size=36x\d+ at=1568,2 size=36x\d+') {
    throw "Unexpected VCockpit03 stacked placement: $vc03Out"
}

$vc04Out = & $node.Source $stub $helper (Join-Path $repoRoot 'tools/panel-marks/fixtures/efis_stacked_vc04.json') $vc04Raw 1 4
if ($LASTEXITCODE -ne 0) { throw 'stub-page.js failed for efis_stacked_vc04.' }
if ($vc04Out -notmatch 'blocks drawn=2 .* at=1606,2 size=36x\d+ at=1568,2 size=36x\d+') {
    throw "Unexpected VCockpit04 stacked placement: $vc04Out"
}

$cornerOut = & $node.Source $stub $helper (Join-Path $repoRoot 'tools/panel-marks/fixtures/corner_guard.json') $cornerRaw 1 4
if ($LASTEXITCODE -ne 0) { throw 'stub-page.js failed for corner_guard.' }
if ($cornerOut -notmatch 'blocks drawn=2 .* at=1606,12 size=36x\d+ at=1556,2 size=36x\d+') {
    throw "Unexpected corner-guard placement: $cornerOut"
}

& $PanelMarksTest --stacked $vc03Raw $vc04Raw 1644 1024
if ($LASTEXITCODE -ne 0) { throw 'C++ decode failed on JS-generated stacked panel marks.' }

& $PanelMarksTest --clear-corner $cornerRaw 1644 1024 1594 10 4
if ($LASTEXITCODE -ne 0) { throw 'C++ decode/corner check failed on JS-generated corner-guard panel marks.' }
