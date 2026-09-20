[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-StrictMode -Version Latest
$dependency = (Get-Content -Raw (Join-Path $repoRoot 'dependencies.json') | ConvertFrom-Json).'inno-setup'
$translation = (Get-Content -Raw (Join-Path $repoRoot 'dependencies.json') | ConvertFrom-Json).'inno-setup-chinese-simplified'
$root = Join-Path $repoRoot 'build/deps'
$destination = Join-Path $root $dependency.directory
$archive = Join-Path $root "innosetup-$($dependency.version).exe"
New-Item -ItemType Directory -Force -Path $root | Out-Null
if (-not (Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest -Uri $dependency.url -OutFile ($archive + '.partial')
    if ((Get-FileHash -LiteralPath ($archive + '.partial') -Algorithm SHA256).Hash -ne $dependency.sha256) { throw 'Inno Setup download SHA256 mismatch.' }
    Move-Item -LiteralPath ($archive + '.partial') -Destination $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $dependency.sha256) { throw 'Cached Inno Setup SHA256 mismatch.' }
$compiler = Join-Path $destination 'ISCC.exe'
if (-not (Test-Path -LiteralPath $compiler)) {
    # Official portable mode disables registry integration, shortcuts and uninstall registration.
    $process = Start-Process -FilePath $archive -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART','/CURRENTUSER','/PORTABLE=1',('/DIR="' + $destination + '"')) -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $compiler)) { throw "Inno Setup portable extraction failed: $($process.ExitCode)" }
}
# Inno Setup ships no Simplified-Chinese message file, so the wizard language is a
# separate pinned download that the script compiler reads from its own Languages folder.
$translationCache = Join-Path $root $translation.file
if (-not (Test-Path -LiteralPath $translationCache)) {
    Invoke-WebRequest -Uri $translation.url -OutFile ($translationCache + '.partial')
    if ((Get-FileHash -LiteralPath ($translationCache + '.partial') -Algorithm SHA256).Hash -ne $translation.sha256) { throw 'Installer translation download SHA256 mismatch.' }
    Move-Item -LiteralPath ($translationCache + '.partial') -Destination $translationCache
}
if ((Get-FileHash -LiteralPath $translationCache -Algorithm SHA256).Hash -ne $translation.sha256) { throw 'Cached installer translation SHA256 mismatch.' }
$translationTarget = Join-Path $destination ('Languages\' + $translation.file)
if (-not (Test-Path -LiteralPath $translationTarget) `
    -or (Get-FileHash -LiteralPath $translationTarget -Algorithm SHA256).Hash -ne $translation.sha256) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $translationTarget) | Out-Null
    Copy-Item -LiteralPath $translationCache -Destination $translationTarget -Force
}
Write-Output $compiler
