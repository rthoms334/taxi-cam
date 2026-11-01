[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-StrictMode -Version Latest
$dependencies = Get-Content -Raw (Join-Path $repoRoot 'dependencies.json') | ConvertFrom-Json
$dependency = $dependencies.'inno-setup'
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
# Message files for Setup languages that Inno does not ship. Each is pinned by
# hash, cached with the other dependencies and never installed.
foreach ($translation in @($dependencies.'inno-setup-translations')) {
    $cache = Join-Path $root $translation.file
    if (-not (Test-Path -LiteralPath $cache)) {
        Invoke-WebRequest -Uri $translation.url -OutFile ($cache + '.partial')
        if ((Get-FileHash -LiteralPath ($cache + '.partial') -Algorithm SHA256).Hash -ne $translation.sha256) { throw "Setup translation download SHA256 mismatch: $($translation.file)" }
        Move-Item -LiteralPath ($cache + '.partial') -Destination $cache
    }
    if ((Get-FileHash -LiteralPath $cache -Algorithm SHA256).Hash -ne $translation.sha256) { throw "Cached Setup translation SHA256 mismatch: $($translation.file)" }
    $target = Join-Path (Join-Path $destination 'Languages') $translation.file
    if (-not (Test-Path -LiteralPath $target) -or (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $translation.sha256) {
        Copy-Item -LiteralPath $cache -Destination $target -Force
    }
}
Write-Output $compiler
