$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repoRoot 'ci/toolchain.ps1')
Set-StrictMode -Version Latest
$compiler = Join-Path (Get-TaxiToolchain $repoRoot) 'clang++.exe'
$output = Join-Path $repoRoot 'build/tools/diagnostics'
$standIn = Join-Path $output 'display-identity-fake'
New-Item -ItemType Directory -Force -Path $standIn | Out-Null
$flags = @('-std=c++20','-O2','-Wall','-Wextra','-Werror','-mno-avx','-mno-avx2','-D_WIN32_WINNT=0x0A00','-static')
$scanner = Join-Path $output 'display-identity-scan.exe'
& $compiler @flags (Join-Path $PSScriptRoot 'display_identity_scan.cpp') '-lpsapi' '-ladvapi32' '-o' $scanner
if ($LASTEXITCODE -ne 0) { throw 'Display identity scanner compile failed.' }
# The stand-in must carry the simulator's basename to pass the scanner's process check.
$fake = Join-Path $standIn 'FlightSimulator2024.exe'
& $compiler @flags (Join-Path $repoRoot 'tests/diagnostics/display_identity_fake.cpp') '-o' $fake
if ($LASTEXITCODE -ne 0) { throw 'Display identity stand-in compile failed.' }

$wrong = & $scanner "$PID" (Join-Path $standIn 'none.txt') 'DUS'
if ($LASTEXITCODE -ne 1 -or ($wrong -join "`n") -notmatch 'error=wrong_process') { throw 'Wrong-process refusal failed.' }

$candidates = Join-Path $standIn 'display-candidates.txt'
Remove-Item -LiteralPath $candidates, "$candidates.done" -ErrorAction SilentlyContinue
$info = New-Object System.Diagnostics.ProcessStartInfo $fake, "`"$candidates`""
$info.UseShellExecute = $false
$info.RedirectStandardOutput = $true
$info.CreateNoWindow = $true
$target = [System.Diagnostics.Process]::Start($info)
try {
  $line = $target.StandardOutput.ReadLine()
  if ($line -notmatch '^material=([0-9a-f]+)$') { throw "Stand-in did not start: $line" }
  $material = [Convert]::ToUInt64($Matches[1], 16)
  $report = & $scanner "$($target.Id)" $candidates 'DUS'
  if ($LASTEXITCODE -ne 0) { throw 'Scanner failed on the stand-in.' }
} finally {
  New-Item -ItemType File -Force -Path "$candidates.done" | Out-Null
  $target.WaitForExit(10000) | Out-Null
}
$nameRef = '{0:x}' -f ($material + 8)
$text = $report -join "`n"
if ($text -notmatch '=== panels: skipped \(untested build') { throw 'The build-specific panel walk ran on an untested image.' }
if ($text -notmatch "(?m)^name ref $nameRef heap -> name DUS\+0\r?\n\s+\+80: [0-9a-f]+ -> holder [0-9a-f]+ base-48") {
  throw "Scanner did not tie the planted name to its texture chain (material $('{0:x}' -f $material))."
}
Write-Output 'PASS: display identity scanner refuses another process and ties a planted name to its native texture chain.'
