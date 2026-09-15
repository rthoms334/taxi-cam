[CmdletBinding()]
param(
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]{0,79}$')][string]$BuildLabel,
    [ValidatePattern('^[0-9a-fA-F]{40}$')][string]$SourceCommit
)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-StrictMode -Version Latest
. (Join-Path $repoRoot 'installer/validation_receipt.ps1')
$payload = Join-Path $repoRoot 'build/native'
$receipt = Assert-TaxiNativeReceipt $payload
$license = Join-Path $repoRoot 'LICENSE'
if (-not (Test-Path -LiteralPath $license -PathType Leaf)) { throw 'The root LICENSE is required for runtime packages.' }
if ($BuildLabel -match '^build\.([0-9]+)$' -and [int]$Matches[1] -ne $receipt.buildNumber) {
    throw 'Release build label must match the build number compiled into the validated application.'
}
$label = if ($BuildLabel) { $BuildLabel } else { [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff') }
$package = Join-Path $repoRoot ("build/packages/taxi-cam-$($receipt.version)-$label-windows-x64")
$zip = $package + '.zip'
if ((Test-Path -LiteralPath $package) -or (Test-Path -LiteralPath $zip)) { throw 'Package already exists; use a new build label.' }
New-Item -ItemType Directory -Path $package -Force | Out-Null
foreach ($file in @('taxi-cam.exe','taxi-camera-bridge.dll')) {
    Copy-Item -LiteralPath (Join-Path $payload $file) -Destination (Join-Path $package $file)
}
Copy-Item -LiteralPath (Join-Path $repoRoot 'taxi-camera-mounts.cfg') -Destination $package
Copy-Item -LiteralPath $license -Destination (Join-Path $package 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $repoRoot 'licenses/native-runtime-notices.txt') -Destination (Join-Path $package 'THIRD_PARTY_NOTICES.txt')
$sourceStatus = @(& git -C $repoRoot status --porcelain)
if ($LASTEXITCODE -ne 0) { throw 'Could not record source working-tree status.' }
[ordered]@{
    version=$receipt.version; build=$label; buildNumber=$receipt.buildNumber; sourceCommit=$SourceCommit; sourceDirty=($sourceStatus.Count -gt 0);
    createdUtc=[DateTime]::UtcNow.ToString('o'); simulatorVerified=$receipt.simulatorVerified
} | ConvertTo-Json | Set-Content -LiteralPath ($package + '.build-info.json') -Encoding utf8
Get-ChildItem -LiteralPath $package -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{file=$_.FullName.Substring($package.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
} | ConvertTo-Json | Set-Content -LiteralPath ($package + '.manifest.json') -Encoding utf8
Compress-Archive -LiteralPath $package -DestinationPath $zip
Write-Output $zip
