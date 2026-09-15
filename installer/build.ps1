[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Package)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-StrictMode -Version Latest
$zip = (Resolve-Path -LiteralPath $Package).Path
$base = [IO.Path]::GetFileNameWithoutExtension($zip)
if ($base -notmatch '^taxi-cam-(\d+\.\d+\.\d+)-build\.(\d+)-windows-x64$' -or [IO.Path]::GetExtension($zip) -ne '.zip') { throw 'Expected a versioned build.N Windows x64 release ZIP.' }
$version = $Matches[1]; $buildNumber = [int]$Matches[2]
$work = Join-Path $repoRoot ('build/installer/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $work | Out-Null
# Expand-Archive rejects traversal entries; use a fresh directory for each immutable input.
Expand-Archive -LiteralPath $zip -DestinationPath $work
$payload = Join-Path $work $base
if (-not (Test-Path -LiteralPath $payload -PathType Container)) { throw 'ZIP payload directory does not match its asset name.' }
. (Join-Path $repoRoot 'installer/validation_receipt.ps1')
$receipt = Assert-TaxiNativeReceipt (Join-Path $repoRoot 'build/native')
$info = Get-Content -Raw -LiteralPath ([IO.Path]::ChangeExtension($zip, '.build-info.json')) | ConvertFrom-Json
if ($receipt.version -ne $version -or $receipt.buildNumber -ne $buildNumber -or $info.buildNumber -ne $buildNumber -or $info.sourceCommit -notmatch '^[0-9a-fA-F]{40}$' -or $info.version -ne $version -or $info.build -ne "build.$buildNumber") { throw 'Package version, build number or source provenance does not match.' }
$manifest = @(Get-Content -Raw -LiteralPath ([IO.Path]::ChangeExtension($zip, '.manifest.json')) | ConvertFrom-Json)
$allowed = @('taxi-cam.exe','taxi-camera-bridge.dll','taxi-camera-mounts.cfg','LICENSE.txt','THIRD_PARTY_NOTICES.txt')
if ($manifest.Count -ne $allowed.Count) { throw 'The runtime package must contain exactly five required files, including LICENSE.txt.' }
$seen = @{}
foreach ($entry in $manifest) {
    $relative = [string]$entry.file
    if ($relative -notin $allowed) { throw "Unexpected runtime package file: $relative" }
    if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[/\\])\.\.([/\\]|$)' -or $relative.Contains(':') -or $seen.ContainsKey($relative)) { throw 'Unsafe or duplicate package manifest path.' }
    $seen[$relative] = $true
    if ((Get-FileHash -LiteralPath (Join-Path $payload $relative) -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Package manifest mismatch: $relative" }
}
foreach ($file in Get-ChildItem -LiteralPath $payload -File -Recurse) {
    $relative = $file.FullName.Substring($payload.Length + 1).Replace('\','/')
    if (-not $seen.ContainsKey($relative)) { throw "Unmanifested package file: $relative" }
}
foreach ($name in @('taxi-cam.exe','taxi-camera-bridge.dll')) {
    if ((Get-FileHash -LiteralPath (Join-Path $payload $name)).Hash -ne $receipt.files.PSObject.Properties[$name].Value) { throw "Package binary differs from validated build: $name" }
}
foreach ($name in @('installer/install.ps1','installer/uninstall.ps1','installer/exe_xml.ps1','installer/validation_receipt.ps1','installer/prerequisites.ps1')) {
    Copy-Item -LiteralPath (Join-Path $repoRoot $name) -Destination (Join-Path $payload ([IO.Path]::GetFileName($name)))
}
Copy-Item -LiteralPath (Join-Path $repoRoot 'build/native/validation.json') -Destination $payload
# The uninstaller retains these scripts in compiled Pascal strings, never as installed loose files.
& (Join-Path $repoRoot 'installer/embed-uninstaller.ps1') -Output (Join-Path $work 'uninstall-scripts.iss') -RuntimeScript (Join-Path $repoRoot 'installer/runtime.ps1') -UninstallScript (Join-Path $repoRoot 'installer/uninstall.ps1') -ExeXmlScript (Join-Path $repoRoot 'installer/exe_xml.ps1')
$compiler = & (Join-Path $repoRoot 'installer/bootstrap.ps1')
$output = Join-Path $repoRoot 'build/packages'
$installerBase = "taxi-cam-$version-windows-x64-setup"
$asset = Join-Path $output ($installerBase + '.exe')
if (Test-Path -LiteralPath $asset) { throw 'Installer already exists; retain release assets and use a fresh worktree or application version.' }
$log = Join-Path $work 'compiler.log'
& $compiler "/DPayloadDir=$payload" "/DInternalDir=$work" "/DAppVersion=$version" "/DBuildNumber=$buildNumber" "/DOutputBase=$installerBase" "/O$output" (Join-Path $repoRoot 'installer/taxi-cam.iss') *> $log
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $asset)) { throw "Installer compilation failed. See $log" }
[ordered]@{version=$version;buildNumber=$buildNumber;sourceCommit=$info.sourceCommit;sourceDirty=$info.sourceDirty;installerSha256=(Get-FileHash -LiteralPath $asset).Hash;packageSha256=(Get-FileHash -LiteralPath $zip).Hash;files=$receipt.files;compilerPayload=$payload;compilerInternal=$work} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath ($asset + '.json') -Encoding utf8
Write-Output $asset
