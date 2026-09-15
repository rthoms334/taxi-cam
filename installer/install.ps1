[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$SimulatorDirectory,
    [string]$ExeXml,
    [string]$Destination = (Join-Path $env:LOCALAPPDATA 'Taxi Cam\app'),
    [string]$PayloadDirectory,
    [switch]$NoShortcut
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'exe_xml.ps1')
. (Join-Path $PSScriptRoot 'validation_receipt.ps1')
. (Join-Path $PSScriptRoot 'prerequisites.ps1')
if (-not $PayloadDirectory) {
    $PayloadDirectory = if (Test-Path -LiteralPath (Join-Path $PSScriptRoot '../build/native/validation.json')) { Join-Path $PSScriptRoot '../build/native' } else { $PSScriptRoot }
}
$payload = (Resolve-Path -LiteralPath $PayloadDirectory).Path
$receipt = Assert-TaxiNativeReceipt $payload
$sim = (Resolve-Path -LiteralPath $SimulatorDirectory).Path
$simExe = Join-Path $sim 'FlightSimulator2024.exe'
if (-not (Test-Path -LiteralPath $simExe -PathType Leaf)) { throw 'Select the MSFS 2024 Content directory containing FlightSimulator2024.exe.' }
if (-not $ExeXml) {
    $choices = @(
        (Join-Path $env:LOCALAPPDATA 'Packages/Microsoft.Limitless_8wekyb3d8bbwe/LocalCache/exe.xml'),
        (Join-Path $env:APPDATA 'Microsoft Flight Simulator 2024/exe.xml')
    )
    $existing = @($choices | Where-Object { Test-Path -LiteralPath $_ })
    if ($existing.Count -ne 1) { throw 'Provide -ExeXml with the simulator launch configuration path.' }
    $ExeXml = $existing[0]
}
$ExeXml = [IO.Path]::GetFullPath($ExeXml)
$dest = [IO.Path]::GetFullPath($Destination)
if ($dest -eq [IO.Path]::GetPathRoot($dest) -or $dest -eq $sim) { throw 'Use a dedicated companion installation directory.' }
$exe = Join-Path $dest 'taxi-cam.exe'
$oldExe = Join-Path $dest '380-taxi-cam.exe'
function Assert-Closed {
    if (Get-Process -Name FlightSimulator2024 -ErrorAction SilentlyContinue) { throw 'Close MSFS before replacing its native camera bridge.' }
    foreach ($p in @(Get-Process -Name taxi-cam,380-taxi-cam -ErrorAction SilentlyContinue)) {
        if ($p.Path -and $p.Path -in @($exe,$oldExe)) { throw 'Exit the taxi camera app from its tray menu before updating it.' }
    }
}
Assert-Closed
Assert-TaxiPrerequisites -SimulatorDirectory $sim
$hash = if (Test-Path -LiteralPath $ExeXml) { (Get-FileHash -LiteralPath $ExeXml).Hash } else { '' }
$document = Read-TaxiLaunchXml $ExeXml
$globalDisabled = $document.DocumentElement.SelectSingleNode('Disabled')
$globalManual = $document.DocumentElement.SelectSingleNode('Launch.ManualLoad')
if (($globalDisabled -and $globalDisabled.InnerText -ieq 'True') -or
    ($globalManual -and $globalManual.InnerText -ieq 'True')) {
    throw 'The simulator launch document disables automatic startup globally; retain its settings and resolve that explicitly.'
}
Set-TaxiStartupEntry $document $exe $simExe
$tag = [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$staging = Join-Path ([IO.Path]::GetTempPath()) ('taxi-cam-install-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $staging | Out-Null
foreach ($name in @('taxi-cam.exe','taxi-camera-bridge.dll')) {
    Copy-Item -LiteralPath (Join-Path $payload $name) -Destination (Join-Path $staging $name)
    if ((Get-FileHash -LiteralPath (Join-Path $staging $name)).Hash -ne $receipt.files.PSObject.Properties[$name].Value) { throw "Staging verification failed: $name" }
}
$prior = @{}
$installed = @()
$legacy = Join-Path $sim 'taxi-camera-native.addon64'
$disabled = $null
$previousLegacy = $null
$previousRecordPath = Join-Path $dest 'installation.json'
if (Test-Path -LiteralPath $previousRecordPath) {
    $previousRecord = Get-Content -Raw -LiteralPath $previousRecordPath | ConvertFrom-Json
    if ($previousRecord.legacyBackup -and (Test-Path -LiteralPath $previousRecord.legacyBackup -PathType Leaf) -and
        (Split-Path -Parent $previousRecord.legacyBackup) -eq $sim -and
        (Split-Path -Leaf $previousRecord.legacyBackup) -like 'taxi-camera-native.addon64.disabled-native-*') {
        $previousLegacy = $previousRecord.legacyBackup
    }
}
$xmlBackup = $null
$xmlWritten = $false
$xmlWrittenHash = ''
$rollbackComplete = $true
$mount = Join-Path $dest 'taxi-camera-mounts.cfg'
$hadMount = Test-Path -LiteralPath $mount
$recordBackup = Join-Path $staging 'installation.json'
if (Test-Path -LiteralPath $previousRecordPath) { Copy-Item -LiteralPath $previousRecordPath -Destination $recordBackup }
$startMenu = if ($NoShortcut) { $null } else { Join-Path $env:APPDATA 'Microsoft/Windows/Start Menu/Programs/Taxi Cam.lnk' }
try {
    Assert-Closed
    if (Test-Path -LiteralPath $oldExe -PathType Leaf) {
        $oldBackup = Join-Path $staging '380-taxi-cam.exe.backup'
        Copy-Item -LiteralPath $oldExe -Destination $oldBackup
        $prior['380-taxi-cam.exe'] = $oldBackup
        $installed += '380-taxi-cam.exe'
        Remove-Item -LiteralPath $oldExe
    }
    foreach ($name in @('taxi-cam.exe','taxi-camera-bridge.dll')) {
        $target = Join-Path $dest $name
        if (Test-Path -LiteralPath $target) {
            $backup = Join-Path $staging ($name + '.backup')
            Copy-Item -LiteralPath $target -Destination $backup
            $prior[$name] = $backup
        }
        $installed += $name
        Copy-Item -LiteralPath (Join-Path $staging $name) -Destination $target -Force
        if ((Get-FileHash -LiteralPath $target).Hash -ne $receipt.files.PSObject.Properties[$name].Value) { throw "Installed binary verification failed: $name" }
        Assert-TaxiVisibleInstallPath $target
    }
    $mount = Join-Path $dest 'taxi-camera-mounts.cfg'
    if (-not (Test-Path -LiteralPath $mount)) {
        $sourceMount = if (Test-Path -LiteralPath (Join-Path $sim 'taxi-camera-mounts.cfg')) { Join-Path $sim 'taxi-camera-mounts.cfg' }
            elseif (Test-Path -LiteralPath (Join-Path $payload 'taxi-camera-mounts.cfg')) { Join-Path $payload 'taxi-camera-mounts.cfg' }
            else { Join-Path $PSScriptRoot '../taxi-camera-mounts.cfg' }
        Copy-Item -LiteralPath $sourceMount -Destination $mount
    }
    Assert-Closed
    if (Test-Path -LiteralPath $legacy) {
        $disabled = $legacy + '.disabled-native-' + $tag
        Move-Item -LiteralPath $legacy -Destination $disabled
    }
    $xmlBackup = Save-TaxiLaunchXml $document $ExeXml $hash
    $xmlWritten = $true
    $xmlWrittenHash = (Get-FileHash -LiteralPath $ExeXml).Hash
    [ordered]@{
        version=$receipt.version; buildNumber=$receipt.buildNumber; installedUtc=[DateTime]::UtcNow.ToString('o'); destination=$dest;
        simulator=$simExe; exeXml=$ExeXml; exeXmlBackup=$xmlBackup; legacyBackup=$(if ($disabled) { $disabled } else { $previousLegacy });
        files=$receipt.files; shortcut=$startMenu; simulatorVerified=$false
    } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $previousRecordPath -Encoding utf8
} catch {
    $installFailure = $_
    $rollbackComplete = $false
    try {
    foreach ($name in $installed) {
        $target = Join-Path $dest $name
        if ($prior.ContainsKey($name)) { Copy-Item -LiteralPath $prior[$name] -Destination $target -Force }
        elseif (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target }
    }
    if ($disabled -and (Test-Path -LiteralPath $disabled) -and -not (Test-Path -LiteralPath $legacy)) {
        Move-Item -LiteralPath $disabled -Destination $legacy
    }
    if ($xmlWritten) {
        if (-not (Test-Path -LiteralPath $ExeXml) -or (Get-FileHash -LiteralPath $ExeXml).Hash -ne $xmlWrittenHash) { throw 'exe.xml changed after installation; the newer contents were preserved.' }
        if ($xmlBackup) { Copy-Item -LiteralPath $xmlBackup -Destination $ExeXml -Force }
        elseif (Test-Path -LiteralPath $ExeXml) { Remove-Item -LiteralPath $ExeXml }
    }
    if (-not $hadMount -and (Test-Path -LiteralPath $mount)) { Remove-Item -LiteralPath $mount }
    if (Test-Path -LiteralPath $recordBackup) { Copy-Item -LiteralPath $recordBackup -Destination $previousRecordPath -Force }
    elseif (Test-Path -LiteralPath $previousRecordPath) { Remove-Item -LiteralPath $previousRecordPath }
    $rollbackComplete = $true
    } catch { throw "Installation failed and rollback needs attention. Recovery files: $staging. $($_.Exception.Message)" }
    throw $installFailure
} finally {
    # The GUID staging directory was created by this invocation; no installed file is removed here.
    if ($rollbackComplete -and [IO.Path]::GetFullPath($staging).StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $staging -Recurse -Force
    }
}
try {
    if ($NoShortcut) { $startMenu = $null } else {
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($startMenu); $shortcut.TargetPath=$exe
    $shortcut.WorkingDirectory=$dest; $shortcut.Description='Taxi Cam settings'; $shortcut.Save()
    $oldShortcut = Join-Path $env:APPDATA 'Microsoft/Windows/Start Menu/Programs/380 Taxi Cam.lnk'
    if ((Test-Path -LiteralPath $oldShortcut) -and $shell.CreateShortcut($oldShortcut).TargetPath -eq $oldExe) {
        Remove-Item -LiteralPath $oldShortcut
    }
    }
} catch { Write-Warning 'Installed successfully, but the Start menu shortcut could not be created.' }
Write-Output "Installed Taxi Cam: $exe"
Write-Output "Automatic tray startup: $ExeXml"
Write-Output "Legacy taxi add-on retained: $disabled"
Write-Output 'Unrelated simulator files and startup entries were preserved.'
