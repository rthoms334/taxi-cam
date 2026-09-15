$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Set-StrictMode -Version Latest
$root = $repoRoot
$label = 'package-test-' + [Guid]::NewGuid().ToString('N')
$commit = 'a' * 40
$zip = & (Join-Path $root 'installer/package.ps1') -BuildLabel $label -SourceCommit $commit
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $entries = @{}
    foreach ($entry in $archive.Entries) {
        if (-not $entry.Name) { continue }
        $relative = ($entry.FullName.Replace('\','/') -split '/',2)[1]
        if ($entries.ContainsKey($relative)) { throw 'Duplicate ZIP entry.' }
        $entries[$relative] = $entry
    }
    $requiredFiles = @('taxi-cam.exe','taxi-camera-bridge.dll','taxi-camera-mounts.cfg','LICENSE.txt','THIRD_PARTY_NOTICES.txt')
    foreach ($required in $requiredFiles) {
        if (-not $entries.ContainsKey($required)) { throw "Package entry missing: $required" }
    }
    if ($entries.Count -ne $requiredFiles.Count) { throw 'Runtime package contains unnecessary files.' }
    $base = Join-Path (Split-Path -Parent $zip) ([IO.Path]::GetFileNameWithoutExtension($zip))
    $manifest = @(Get-Content -Raw -LiteralPath ($base + '.manifest.json') | ConvertFrom-Json)
    if ($manifest.Count -ne $entries.Count) { throw 'Manifest does not cover the complete package.' }
    foreach ($file in $manifest) {
        $stream = $entries[$file.file].Open()
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $hash = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','') }
        finally { $sha.Dispose(); $stream.Dispose() }
        if ($hash -ne $file.sha256) { throw "Package manifest hash mismatch: $($file.file)" }
        $legalSource = switch ($file.file) {
            'LICENSE.txt' { Join-Path $repoRoot 'LICENSE' }
            'THIRD_PARTY_NOTICES.txt' { Join-Path $repoRoot 'licenses/native-runtime-notices.txt' }
        }
        if ($legalSource -and $hash -ne (Get-FileHash -LiteralPath $legalSource -Algorithm SHA256).Hash) {
            throw "Package changed the legal text: $($file.file)"
        }
    }
    $info = Get-Content -Raw -LiteralPath ($base + '.build-info.json') | ConvertFrom-Json
    if ($info.sourceCommit -ne $commit -or $info.build -ne $label -or $info.simulatorVerified) { throw 'Incorrect package provenance.' }
} finally { $archive.Dispose() }
$refused = $false
try { & (Join-Path $root 'installer/package.ps1') -BuildLabel $label -SourceCommit $commit | Out-Null } catch { $refused = $true }
if (-not $refused) { throw 'Existing package was overwritten.' }
$refused = $false
try { & (Join-Path $root 'installer/package.ps1') -BuildLabel '../escape' | Out-Null } catch { $refused = $true }
if (-not $refused) { throw 'Invalid package label accepted.' }
Write-Output "PASS package: exactly $($entries.Count) runtime files, verbatim GPL license and third-party notices, complete manifest hashes, provenance and overwrite/path refusal."
