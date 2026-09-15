[CmdletBinding()]
param(
    [ValidateSet('Discover','Install','Rollback','CheckClosed','Uninstall')][string]$Mode,
    [Parameter(Mandatory=$true)][string]$Destination,
    [string]$SimulatorDirectory, [string]$ExeXml, [string]$PayloadDirectory,
    [Parameter(Mandatory=$true)][string]$StateDirectory,
    [ValidateRange(0,2147483647)][int]$UpdateFromPid = 0
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Setup may inherit PSModulePath from PowerShell 7; load the Windows PowerShell
# utility module by its own absolute path for Get-FileHash and JSON operations.
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -ErrorAction Stop
New-Item -ItemType Directory -Force -Path $StateDirectory | Out-Null
$statePath = Join-Path $StateDirectory 'transaction.json'
function Restore-Transaction {
    if (-not (Test-Path -LiteralPath $statePath)) { return }
    try {
    $state = Get-Content -Raw -LiteralPath $statePath | ConvertFrom-Json
    $conflicts = @()
    foreach ($entry in $state.files) {
        if (-not $entry.owned) { continue }
        $currentHash = if (Test-Path -LiteralPath $entry.path -PathType Leaf) { (Get-FileHash -LiteralPath $entry.path).Hash } else { '' }
        if ($currentHash -ne $entry.installedHash) { $conflicts += $entry.path; continue }
        if ($entry.existed) { Copy-Item -LiteralPath $entry.backup -Destination $entry.path -Force }
        elseif (Test-Path -LiteralPath $entry.path -PathType Leaf) { Remove-Item -LiteralPath $entry.path }
    }
    if ($state.createdLegacyBackup -and (Test-Path -LiteralPath $state.createdLegacyBackup)) {
        if ((Get-FileHash -LiteralPath $state.createdLegacyBackup).Hash -eq $state.createdLegacyHash) { Remove-Item -LiteralPath $state.createdLegacyBackup }
        else { $conflicts += $state.createdLegacyBackup }
    }
    if ($conflicts.Count) {
        throw ('Rollback preserved files changed by another writer: ' + ($conflicts -join ', '))
    }
    Remove-Item -LiteralPath $statePath
    } catch {
        $rollbackError = $_.Exception.Message
        $recovery = Join-Path ([IO.Path]::GetTempPath()) ('taxi-cam-recovery-' + [Guid]::NewGuid().ToString('N'))
        Copy-Item -LiteralPath $StateDirectory -Destination $recovery -Recurse
        $recoveryStatePath = Join-Path $recovery 'transaction.json'
        $recoveryState = Get-Content -Raw -LiteralPath $recoveryStatePath | ConvertFrom-Json
        foreach ($entry in $recoveryState.files) { $entry.backup = Join-Path $recovery ([IO.Path]::GetFileName($entry.backup)) }
        $recoveryState | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $recoveryStatePath -Encoding utf8
        throw ($rollbackError + '. Recovery snapshot: ' + $recovery)
    }
}
try {
    if ($Mode -eq 'Discover') {
        $recordPath = Join-Path $Destination 'installation.json'
        if (Test-Path -LiteralPath $recordPath) {
            $record = Get-Content -Raw -LiteralPath $recordPath | ConvertFrom-Json
            $SimulatorDirectory = Split-Path -Parent $record.simulator
            $ExeXml = $record.exeXml
        } else {
            $simChoices = @('C:\XboxGames\Microsoft Flight Simulator 2024\Content', 'C:\Program Files (x86)\Steam\steamapps\common\Limitless')
            $found = @($simChoices | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'FlightSimulator2024.exe') })
            if ($found.Count -eq 1) { $SimulatorDirectory = $found[0] }
            $xmlChoices = @((Join-Path $env:LOCALAPPDATA 'Packages/Microsoft.Limitless_8wekyb3d8bbwe/LocalCache/exe.xml'), (Join-Path $env:APPDATA 'Microsoft Flight Simulator 2024/exe.xml'))
            $foundXml = @($xmlChoices | Where-Object { Test-Path -LiteralPath $_ })
            if ($foundXml.Count -eq 1) { $ExeXml = $foundXml[0] }
        }
        @('[Paths]', "Simulator=$SimulatorDirectory", "ExeXml=$ExeXml") | Set-Content -LiteralPath (Join-Path $StateDirectory 'choices.ini') -Encoding Unicode
        exit 0
    }
    if ($Mode -eq 'Rollback') { Restore-Transaction; exit 0 }
    if ($UpdateFromPid) {
        $updater = Get-Process -Id $UpdateFromPid -ErrorAction SilentlyContinue
        if ($updater) {
            if ($updater.Path -notin @((Join-Path $Destination 'taxi-cam.exe'),(Join-Path $Destination '380-taxi-cam.exe'))) { throw 'UPDATEFROMPID does not identify the installed companion.' }
            if (-not $updater.WaitForExit(30000)) { throw 'The companion is still exiting. Close it and retry Setup.' }
        }
    }
    if (Get-Process -Name FlightSimulator2024 -ErrorAction SilentlyContinue) { throw 'Close MSFS 2024 before installing or uninstalling Taxi Cam.' }
    foreach ($process in @(Get-Process -Name taxi-cam,380-taxi-cam -ErrorAction SilentlyContinue)) {
        if ($process.Path -in @((Join-Path $Destination 'taxi-cam.exe'),(Join-Path $Destination '380-taxi-cam.exe'))) { throw 'Exit the taxi camera app from its tray menu, then retry.' }
    }
    if ($Mode -eq 'CheckClosed') { exit 0 }
    if ($Mode -eq 'Uninstall') {
        & (Join-Path $PSScriptRoot 'uninstall.ps1') -Installation $Destination
        exit 0
    }
    if (Test-Path -LiteralPath $statePath) { throw 'A previous installation transaction has not finished.' }
    if (-not (Test-Path -LiteralPath (Join-Path $SimulatorDirectory 'FlightSimulator2024.exe') -PathType Leaf)) { throw 'Select the MSFS 2024 Content directory containing FlightSimulator2024.exe.' }
    if ([IO.Path]::GetFileName($ExeXml) -ine 'exe.xml') { throw 'Select the simulator launch configuration named exe.xml.' }
    $destFull = [IO.Path]::GetFullPath($Destination).TrimEnd('\')
    $payloadFull = (Resolve-Path -LiteralPath $PayloadDirectory).Path
    $targets = @((Join-Path $destFull 'installation.json'), (Join-Path $destFull '380-taxi-cam.exe'), $ExeXml)
    # Setup can pass an 8.3 temporary path. Directory enumeration expands it,
    # so slicing absolute paths by the original prefix length corrupts targets.
    foreach ($name in @('taxi-cam.exe','taxi-camera-bridge.dll','taxi-camera-mounts.cfg','LICENSE.txt','THIRD_PARTY_NOTICES.txt')) {
        $targets += Join-Path $destFull $name
    }
    $legacy = Join-Path $SimulatorDirectory 'taxi-camera-native.addon64'
    $legacyBackups = @(Get-ChildItem -LiteralPath $SimulatorDirectory -Filter 'taxi-camera-native.addon64.disabled-native-*' | ForEach-Object FullName)
    $targets += $legacy
    $snapshot = @(); $index = 0
    foreach ($target in ($targets | Select-Object -Unique)) {
        $backup = Join-Path $StateDirectory ("backup-$index"); $index++
        $existed = Test-Path -LiteralPath $target -PathType Leaf
        if ($existed) { Copy-Item -LiteralPath $target -Destination $backup }
        $snapshot += [ordered]@{path=$target; backup=$backup; existed=$existed;owned=$false;installedHash=''}
    }
    $state = [ordered]@{files=$snapshot;createdLegacyBackup='';createdLegacyHash=''}
    $state | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $statePath -Encoding utf8
    $nativeSucceeded = $false
    try {
        & (Join-Path $payloadFull 'install.ps1') -SimulatorDirectory $SimulatorDirectory -ExeXml $ExeXml -Destination $destFull -PayloadDirectory $payloadFull -NoShortcut
        $nativeSucceeded = $true
        $installedRecord = Get-Content -Raw -LiteralPath (Join-Path $destFull 'installation.json') | ConvertFrom-Json
        if ($installedRecord.legacyBackup -and $installedRecord.legacyBackup -notin $legacyBackups) {
            $state.createdLegacyBackup = $installedRecord.legacyBackup
            $state.createdLegacyHash = (Get-FileHash -LiteralPath $installedRecord.legacyBackup).Hash
        }
        foreach ($entry in $snapshot) {
            $entry.installedHash = if (Test-Path -LiteralPath $entry.path -PathType Leaf) { (Get-FileHash -LiteralPath $entry.path).Hash } else { '' }
            $priorHash = if ($entry.existed) { (Get-FileHash -LiteralPath $entry.backup).Hash } else { '' }
            $entry.owned = $entry.installedHash -ne $priorHash
        }
        $state | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $statePath -Encoding utf8
    } catch {
        # install.ps1 owns its own failure rollback. Never undo its concurrent XML edit guard.
        if ($nativeSucceeded) { Restore-Transaction } else { Remove-Item -LiteralPath $statePath }
        throw
    }
} catch {
    [IO.File]::WriteAllText((Join-Path $StateDirectory 'error.txt'), $_.Exception.Message, [Text.UTF8Encoding]::new($false))
    exit 1
}
