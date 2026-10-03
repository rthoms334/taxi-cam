$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repoRoot 'tests/support/installer_fixture.ps1')
. (Join-Path $repoRoot 'installer/settings.ps1')
$fixture = Join-Path $repoRoot ('build/installer-settings-tests/' + [Guid]::NewGuid().ToString('N'))
$local = Join-Path $fixture 'Local'
$sim = Join-Path $fixture 'sim'
$app = Join-Path $fixture 'app'
$payload = Join-Path $fixture 'payload'
$xml = Join-Path $fixture 'exe.xml'
$checks = 0
function Assert-SettingsTest([bool]$Condition, [string]$Message) {
    $script:checks++
    if (-not $Condition) { throw $Message }
}
$otherCompanion = $false
function Get-Process {
    param([string[]]$Name, $ErrorAction)
    if ($otherCompanion -and 'taxi-cam' -in $Name) { [pscustomobject]@{Path='C:\Other Installation\taxi-cam.exe'} }
}
function Seed-Settings {
    $map = @{}
    foreach ($target in @(Get-TaxiSettingsTargets -Installation $app -IncludeMount)) {
        Assert-SettingsTest ($target.path.StartsWith($fixture + '\', [StringComparison]::OrdinalIgnoreCase)) 'Test attempted to leave its fixture.'
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target.path) | Out-Null
        [IO.File]::WriteAllText($target.path, ('saved fixture: ' + $target.path))
        $map[$target.path] = (Get-FileHash -LiteralPath $target.path).Hash
    }
    return $map
}
function Assert-Saved($Expected) {
    foreach ($path in $Expected.Keys) {
        Assert-SettingsTest ((Test-Path -LiteralPath $path -PathType Leaf) -and (Get-FileHash -LiteralPath $path).Hash -eq $Expected[$path]) "Saved file changed: $path"
    }
}
function Get-PreservedSettingsMap {
    $map = @{}
    foreach ($target in @(Get-TaxiSettingsTargets -Installation $app -IncludeMount)) {
        $leaf = [IO.Path]::GetFileName($target.path)
        if ($leaf -eq 'settings.ini' -or $target.path -match '[\\/]profiles[\\/]') { continue }
        if (Test-Path -LiteralPath $target.path -PathType Leaf) { $map[$target.path] = (Get-FileHash -LiteralPath $target.path).Hash }
    }
    return $map
}
function Get-ExistingRateMap {
    $map = @{}
    foreach ($target in @(Get-TaxiCameraRateTargets)) {
        if (Test-Path -LiteralPath $target.path -PathType Leaf) { $map[$target.path] = (Get-FileHash -LiteralPath $target.path).Hash }
    }
    return $map
}
function Assert-ForcedCameraRate([string]$ExpectedRate = '10') {
    $found = 0
    $settingsPath = Get-TaxiCameraRateSettingsPath
    foreach ($target in @(Get-TaxiCameraRateTargets)) {
        if (-not (Test-Path -LiteralPath $target.path -PathType Leaf)) { continue }
        $rate = Get-TaxiIniKey $target.path 'display' 'camera_rate'
        if ($null -eq $rate -and $target.path -eq $settingsPath -and (Get-TaxiIniKey $target.path 'display' 'camera_rate_revision') -eq '2') { continue }
        $found++
        Assert-SettingsTest ($rate -eq $ExpectedRate) "Install did not write camera_rate=${ExpectedRate}: $($target.path)"
    }
    Assert-SettingsTest ($found -gt 0) 'Camera-rate force found no existing settings files to check.'
}
function Assert-CameraRateRevision([string]$Path, $Expected) {
    $actual = Get-TaxiIniKey $Path 'display' 'camera_rate_revision'
    if ($null -eq $Expected) {
        Assert-SettingsTest ($null -eq $actual) "Unexpected camera_rate_revision '$actual' on $Path"
    } else {
        Assert-SettingsTest ($actual -eq $Expected) "camera_rate_revision is '$actual', expected '$Expected': $Path"
    }
}
# The exposure migrate is retired: installs never write exposure_revision.
function Assert-NoExposureRevision([string]$Path) {
    $actual = Get-TaxiIniKey $Path 'display' 'exposure_revision'
    Assert-SettingsTest ($null -eq $actual) "Unexpected exposure_revision '$actual' on $Path"
}
function Assert-MountCalibrationUnchanged([string]$Path, [string]$Forward = '27.25', [string]$Budget = '4096') {
    Assert-SettingsTest ((Get-TaxiIniKey $Path 'nose' 'forward') -eq $Forward) "Keep-install changed nose forward on $Path"
    Assert-SettingsTest ((Get-TaxiIniKey $Path 'display' 'calibration_budget') -eq $Budget) "Keep-install changed calibration_budget on $Path"
}
function Assert-StampOnlySettings {
    Assert-SettingsTest (Test-Path -LiteralPath $settingsIni -PathType Leaf) 'Keep-install did not stamp a missing settings.ini.'
    Assert-CameraRateRevision $settingsIni '2'
    Assert-NoExposureRevision $settingsIni
    Assert-SettingsTest ($null -eq (Get-TaxiIniKey $settingsIni 'display' 'camera_rate')) 'Stamp-only settings.ini included camera_rate.'
}
function Write-RateFixture([string]$SettingsRate = '', [string]$ProfileRate = '', [string]$ProfileExposure = '-7.5') {
    $settingsBody = "[aircraft]`r`nprofile=4`r`nautomatic=1`r`n[display]`r`n"
    if ($SettingsRate.Length) { $settingsBody += "camera_rate=$SettingsRate`r`n" }
    [IO.File]::WriteAllText($settingsIni, $settingsBody)
    $profileBody = "[display]`r`n"
    if ($ProfileRate.Length) { $profileBody += "camera_rate=$ProfileRate`r`n" }
    $profileBody += "exposure=$ProfileExposure`r`ncalibration_budget=4096`r`n[nose]`r`nforward=27.25`r`n"
    [IO.File]::WriteAllBytes($profileIni, ([Text.Encoding]::Unicode.GetPreamble() + [Text.Encoding]::Unicode.GetBytes($profileBody)))
}
function Invoke-Install([switch]$Reset) {
    & (Join-Path $repoRoot 'installer/install.ps1') -SimulatorDirectory $sim -ExeXml $xml -Destination $app -PayloadDirectory $payload -NoShortcut -ResetSettings:$Reset
}
function Invoke-Runtime([string]$Mode, [string]$State, [switch]$Reset) {
    $arguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$hostScript,'-Runtime',(Join-Path $repoRoot 'installer/runtime.ps1'),'-Mode',$Mode,'-Destination',$app,'-SimulatorDirectory',$sim,'-ExeXml',$xml,'-PayloadDirectory',$payload,'-StateDirectory',$State)
    if ($Reset) { $arguments += '-ResetSettings' }
    & (Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe') @arguments
    return $LASTEXITCODE
}
$priorLocal = $env:LOCALAPPDATA
$priorTemp = $env:TEMP
$priorTmp = $env:TMP
try {
    foreach ($directory in @($local,$sim,$app,$payload,(Join-Path $fixture 'temp'))) { New-Item -ItemType Directory -Force -Path $directory | Out-Null }
    $env:LOCALAPPDATA = $local
    $env:TEMP = $env:TMP = Join-Path $fixture 'temp'
    New-TaxiFixtureImage (Join-Path $sim 'FlightSimulator2024.exe') $false
    New-TaxiFixtureImage (Join-Path $sim 'SimConnect_internal.dll')
    [IO.File]::WriteAllText((Join-Path $sim 'taxi-camera-mounts.cfg'),'stale simulator calibration must not be imported on reset')
    [IO.File]::WriteAllText($xml,'<SimBase.Document Type="Launch"><Launch.Addon><Name>Keep Me</Name><Path>C:\Other.exe</Path></Launch.Addon></SimBase.Document>')
    foreach ($name in @('taxi-cam.exe','taxi-camera-bridge.dll','validation.json')) {
        Copy-Item -LiteralPath (Join-Path $repoRoot "build/native/$name") -Destination $payload
    }
    foreach ($name in @('install.ps1','exe_xml.ps1','validation_receipt.ps1','prerequisites.ps1','settings.ps1')) {
        Copy-Item -LiteralPath (Join-Path $repoRoot "installer/$name") -Destination $payload
    }
    Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination (Join-Path $payload 'LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $repoRoot 'licenses/native-runtime-notices.txt') -Destination (Join-Path $payload 'THIRD_PARTY_NOTICES.txt')
    Copy-Item -LiteralPath (Join-Path $repoRoot 'taxi-camera-mounts.cfg') -Destination $payload
    $defaultHash = (Get-FileHash -LiteralPath (Join-Path $payload 'taxi-camera-mounts.cfg')).Hash

    $iniUnit = Join-Path $fixture 'ini-unit'
    New-Item -ItemType Directory -Force -Path $iniUnit | Out-Null
    $ansiIni = Join-Path $iniUnit 'settings.ini'
    [IO.File]::WriteAllText($ansiIni, "[aircraft]`r`nprofile=4`r`nautomatic=1`r`n[display]`r`ncamera_rate=15`r`nsingle_camera=0`r`n")
    Set-TaxiIniKey $ansiIni 'display' 'camera_rate' '5'
    Assert-SettingsTest ((Get-TaxiIniKey $ansiIni 'display' 'camera_rate') -eq '5') 'ANSI INI writer did not replace camera_rate.'
    Assert-SettingsTest ((Get-TaxiIniKey $ansiIni 'aircraft' 'profile') -eq '4' -and (Get-TaxiIniKey $ansiIni 'display' 'single_camera') -eq '0') 'ANSI INI writer changed unrelated keys.'
    $utf16Ini = Join-Path $iniUnit 'fbw-a380x.ini'
    $utf16 = "[service]`r`nenabled=1`r`n[display]`r`ncamera_rate=30`r`nexposure=-7.5`r`n[nose]`r`nforward=27.25`r`n"
    [IO.File]::WriteAllBytes($utf16Ini, ([Text.Encoding]::Unicode.GetPreamble() + [Text.Encoding]::Unicode.GetBytes($utf16)))
    Set-TaxiIniKey $utf16Ini 'display' 'camera_rate' '5'
    Assert-SettingsTest ((Get-TaxiIniKey $utf16Ini 'display' 'camera_rate') -eq '5') 'UTF-16 INI writer did not replace camera_rate.'
    Assert-SettingsTest ((Get-TaxiIniKey $utf16Ini 'display' 'exposure') -eq '-7.5' -and (Get-TaxiIniKey $utf16Ini 'nose' 'forward') -eq '27.25') 'UTF-16 INI writer changed calibration.'
    Set-TaxiIniKey $utf16Ini 'display' 'exposure' '-8'
    Assert-SettingsTest ((Get-TaxiIniKey $utf16Ini 'display' 'exposure') -eq '-8') 'UTF-16 INI writer did not replace exposure.'
    Assert-SettingsTest ((Get-TaxiIniKey $utf16Ini 'display' 'camera_rate') -eq '5' -and (Get-TaxiIniKey $utf16Ini 'nose' 'forward') -eq '27.25') 'UTF-16 exposure write changed rate or mounts.'
    Set-TaxiIniKey $utf16Ini 'display' 'calibration_budget' '4096'
    Set-TaxiIniKey $utf16Ini 'display' 'exposure' '-8'
    Assert-SettingsTest ((Get-TaxiIniKey $utf16Ini 'display' 'calibration_budget') -eq '4096' -and (Get-TaxiIniKey $utf16Ini 'nose' 'forward') -eq '27.25') 'UTF-16 exposure rewrite changed calibration_budget or mounts.'
    $bytes = [IO.File]::ReadAllBytes($utf16Ini)
    Assert-SettingsTest ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) 'UTF-16 INI writer dropped the BOM.'
    $plainIni = Join-Path $iniUnit 'plain.ini'
    [IO.File]::WriteAllText($plainIni, "saved fixture without a display section`r`n")
    Set-TaxiIniKey $plainIni 'display' 'camera_rate' '5'
    Assert-SettingsTest ((Get-TaxiIniKey $plainIni 'display' 'camera_rate') -eq '5' -and [IO.File]::ReadAllText($plainIni).Contains('saved fixture without a display section')) 'INI writer did not append camera_rate while keeping prior text.'
    $missingIni = Join-Path $iniUnit 'missing.ini'
    $refused = $false
    try { Set-TaxiIniKey $missingIni 'display' 'camera_rate' '5' } catch { $refused = $_.Exception.Message -like 'Settings file is missing*' }
    Assert-SettingsTest ($refused -and -not (Test-Path -LiteralPath $missingIni)) 'INI writer created a missing settings file.'

    $saved = Seed-Settings
    $settingsIni = Join-Path $local 'Taxi Cam/settings.ini'
    $profileIni = Join-Path $local 'Taxi Cam/profiles/fbw-a380x.ini'
    Write-RateFixture '15' '15'
    $preserved = Get-PreservedSettingsMap
    $unrelated = @{}
    foreach ($path in @((Join-Path $local 'Taxi Cam/logs/keep.log'),(Join-Path $local 'Taxi Cam/history/old.ini'),(Join-Path $local 'Taxi Cam/profiles/custom-aircraft.ini'),(Join-Path $local '380 Taxi Cam/profiles/unknown.ini'))) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
        [IO.File]::WriteAllText($path,'unrelated fixture')
        $unrelated[$path] = (Get-FileHash -LiteralPath $path).Hash
    }
    Invoke-Install
    Assert-ForcedCameraRate
    Assert-CameraRateRevision $settingsIni '2'
    Assert-NoExposureRevision $settingsIni
    Assert-SettingsTest ((Get-TaxiIniKey $settingsIni 'aircraft' 'profile') -eq '4' -and (Get-TaxiIniKey $settingsIni 'aircraft' 'automatic') -eq '1') 'Keep-install rate force changed aircraft selection.'
    Assert-MountCalibrationUnchanged $profileIni
    Assert-SettingsTest ((Get-TaxiIniKey $profileIni 'display' 'exposure') -eq '-7.5') 'Keep-install changed the retired profile exposure key.'
    Assert-Saved $preserved
    Assert-Saved $unrelated

    Set-TaxiIniKey $settingsIni 'display' 'camera_rate' '15'
    Set-TaxiIniKey $profileIni 'display' 'camera_rate' '15'
    Assert-CameraRateRevision $settingsIni '2'
    Assert-NoExposureRevision $settingsIni
    Invoke-Install
    Assert-SettingsTest ((Get-TaxiIniKey $settingsIni 'display' 'camera_rate') -eq '15') 'Second keep-install rewrote a user camera_rate after the migration stamp.'
    Assert-SettingsTest ((Get-TaxiIniKey $profileIni 'display' 'camera_rate') -eq '15') 'Second keep-install rewrote a profile camera_rate after the migration stamp.'
    Assert-SettingsTest ((Get-TaxiIniKey $profileIni 'display' 'exposure') -eq '-7.5') 'Second keep-install changed the retired profile exposure key.'
    Assert-CameraRateRevision $settingsIni '2'
    Assert-NoExposureRevision $settingsIni
    Assert-MountCalibrationUnchanged $profileIni
    Assert-Saved $preserved
    Assert-Saved $unrelated

    Write-RateFixture '5' '5'
    Set-TaxiIniKey $settingsIni 'display' 'camera_rate_revision' '1'
    Assert-CameraRateRevision $settingsIni '1'
    Assert-NoExposureRevision $settingsIni
    Invoke-Install
    Assert-ForcedCameraRate
    Assert-CameraRateRevision $settingsIni '2'
    Assert-NoExposureRevision $settingsIni
    Assert-MountCalibrationUnchanged $profileIni
    Assert-SettingsTest ((Get-TaxiIniKey $profileIni 'display' 'exposure') -eq '-7.5') 'Revision-1 migrate changed the retired profile exposure key.'
    Set-TaxiIniKey $settingsIni 'display' 'camera_rate' '5'
    Set-TaxiIniKey $profileIni 'display' 'camera_rate' '5'
    Set-TaxiIniKey $profileIni 'display' 'exposure' '-6'
    Invoke-Install
    Assert-SettingsTest ((Get-TaxiIniKey $settingsIni 'display' 'camera_rate') -eq '5') 'Install after revision 2 rewrote a user camera_rate of 5.'
    Assert-SettingsTest ((Get-TaxiIniKey $profileIni 'display' 'camera_rate') -eq '5') 'Install after revision 2 rewrote a user profile camera_rate of 5.'
    Assert-SettingsTest ((Get-TaxiIniKey $profileIni 'display' 'exposure') -eq '-6') 'Install changed the retired profile exposure key.'
    Assert-CameraRateRevision $settingsIni '2'
    Assert-NoExposureRevision $settingsIni
    Assert-MountCalibrationUnchanged $profileIni
    $rateAfterInstall = Get-ExistingRateMap

    $otherCompanion = $true
    $refused = $false
    try { Invoke-Install -Reset } catch { $refused = $_.Exception.Message -like 'Exit every Taxi Cam*' }
    Assert-SettingsTest $refused 'Reset failed to refuse a companion from another installation.'
    Assert-Saved $rateAfterInstall
    Assert-Saved $preserved
    $otherCompanion = $false

    $bundledMount = Join-Path $payload 'taxi-camera-mounts.cfg'
    $heldMount = $bundledMount + '.held'
    Move-Item -LiteralPath $bundledMount -Destination $heldMount
    $refused = $false
    try { Invoke-Install -Reset } catch { $refused = $_.Exception.Message -like 'Bundled default camera mounts*' }
    finally { Move-Item -LiteralPath $heldMount -Destination $bundledMount }
    Assert-SettingsTest $refused 'Reset silently imported simulator calibration when bundled defaults were missing.'
    Assert-Saved $rateAfterInstall
    Assert-Saved $preserved

    Write-RateFixture '15' '15'
    Assert-CameraRateRevision $settingsIni $null
    $exeLock = [IO.File]::Open((Join-Path $app 'taxi-cam.exe'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::None)
    $refused = $false
    try { Invoke-Install } catch { $refused = $true } finally { $exeLock.Dispose() }
    Assert-SettingsTest $refused 'Locked destination executable did not fail keep-install.'
    Assert-SettingsTest ((Get-TaxiIniKey $settingsIni 'display' 'camera_rate') -eq '15') 'Failed keep-install left a forced camera_rate after rollback.'
    Assert-CameraRateRevision $settingsIni $null
    Assert-NoExposureRevision $settingsIni
    Assert-SettingsTest ((Get-TaxiIniKey $profileIni 'display' 'camera_rate') -eq '15') 'Failed keep-install left a forced profile camera_rate after rollback.'
    Assert-SettingsTest ((Get-TaxiIniKey $profileIni 'display' 'exposure') -eq '-7.5') 'Failed keep-install changed the retired profile exposure key.'
    Assert-SettingsTest ((Get-TaxiIniKey $profileIni 'nose' 'forward') -eq '27.25' -and (Get-TaxiIniKey $profileIni 'display' 'calibration_budget') -eq '4096') 'Failed keep-install rate rollback dropped calibration.'
    Assert-Saved $preserved
    Assert-Saved $unrelated
    Invoke-Install
    Assert-ForcedCameraRate
    Assert-CameraRateRevision $settingsIni '2'
    Assert-NoExposureRevision $settingsIni
    Assert-SettingsTest ((Get-TaxiIniKey $settingsIni 'aircraft' 'profile') -eq '4') 'Retry keep-install after rate rollback changed aircraft selection.'
    Assert-MountCalibrationUnchanged $profileIni
    Assert-SettingsTest ((Get-TaxiIniKey $profileIni 'display' 'exposure') -eq '-7.5') 'Retry keep-install after rollback changed the retired profile exposure key.'

    Write-RateFixture
    Assert-SettingsTest ($null -eq (Get-TaxiIniKey $settingsIni 'display' 'camera_rate')) 'Missing-key fixture still had camera_rate.'
    Assert-CameraRateRevision $settingsIni $null
    Assert-NoExposureRevision $settingsIni
    Invoke-Install
    Assert-ForcedCameraRate
    Assert-CameraRateRevision $settingsIni '2'
    Assert-NoExposureRevision $settingsIni
    Assert-SettingsTest ((Get-TaxiIniKey $settingsIni 'aircraft' 'profile') -eq '4') 'Missing-key migrate changed aircraft selection.'
    Assert-MountCalibrationUnchanged $profileIni
    Assert-SettingsTest ((Get-TaxiIniKey $profileIni 'display' 'exposure') -eq '-7.5') 'Missing-key migrate changed the retired profile exposure key.'

    Remove-Item -LiteralPath $settingsIni
    Invoke-Install
    Assert-StampOnlySettings
    Assert-SettingsTest ((Get-TaxiIniKey $profileIni 'display' 'camera_rate') -eq '10') 'Missing settings.ini migrate left a profile rate other than 10.'
    Assert-SettingsTest ((Get-TaxiIniKey $profileIni 'display' 'exposure') -eq '-7.5') 'Missing settings.ini migrate changed the retired profile exposure key.'
    Assert-MountCalibrationUnchanged $profileIni
    Set-TaxiIniKey $profileIni 'display' 'camera_rate' '15'
    Invoke-Install
    Assert-StampOnlySettings
    Assert-SettingsTest ((Get-TaxiIniKey $profileIni 'display' 'camera_rate') -eq '15') 'Install after a stamp-only settings.ini rewrote a user profile rate.'
    Assert-SettingsTest ((Get-TaxiIniKey $profileIni 'display' 'exposure') -eq '-7.5') 'Install after a stamp-only settings.ini changed the retired profile exposure key.'
    Assert-MountCalibrationUnchanged $profileIni
    $rateAfterInstall = Get-ExistingRateMap

    $lockedXmlHash = (Get-FileHash -LiteralPath $xml).Hash
    $xmlLock = [IO.File]::Open($xml, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try { Invoke-Install -Reset } finally { $xmlLock.Dispose() }
    $lockedRecord = Get-Content -Raw -LiteralPath (Join-Path $app 'installation.json') | ConvertFrom-Json
    Assert-SettingsTest ($lockedRecord.startupStatus -eq 'unchanged' -and -not $lockedRecord.startupUpdated) 'Locked XML fallback did not preserve existing startup ownership.'
    Assert-SettingsTest (-not [string]::IsNullOrWhiteSpace($lockedRecord.startupWarning)) 'Locked XML fallback omitted its startup warning.'
    Assert-SettingsTest ((Get-FileHash -LiteralPath $xml).Hash -eq $lockedXmlHash) 'Locked XML fallback changed simulator startup.'
    foreach ($target in @(Get-TaxiSettingsTargets -Installation $app)) {
        Assert-SettingsTest (-not (Test-Path -LiteralPath $target.path)) 'Locked XML fallback prevented the requested settings reset.'
    }
    Assert-SettingsTest ((Get-FileHash -LiteralPath (Join-Path $app 'taxi-camera-mounts.cfg')).Hash -eq $defaultHash) 'Locked XML fallback did not retain the requested default calibration reset.'
    Assert-Saved $unrelated

    $saved = Seed-Settings
    Invoke-Install -Reset
    foreach ($target in @(Get-TaxiSettingsTargets -Installation $app)) {
        Assert-SettingsTest (-not (Test-Path -LiteralPath $target.path)) 'Explicit reset retained a known current or legacy setting.'
    }
    Assert-SettingsTest ((Get-FileHash -LiteralPath (Join-Path $app 'taxi-camera-mounts.cfg')).Hash -eq $defaultHash) 'Reset did not install bundled camera defaults.'
    Invoke-Install
    Assert-SettingsTest ((Get-FileHash -LiteralPath (Join-Path $app 'taxi-camera-mounts.cfg')).Hash -eq $defaultHash) 'A subsequent keep install reimported stale simulator calibration.'
    Assert-Saved $unrelated

    $saved = Seed-Settings
    $otherCompanion = $true
    $refused = $false
    try { & (Join-Path $repoRoot 'installer/uninstall.ps1') -Installation $app -RemoveSettings }
    catch { $refused = $_.Exception.Message -like 'Exit every Taxi Cam*' }
    Assert-SettingsTest $refused 'Uninstall removal accepted another running installation.'
    $otherCompanion = $false

    # More than one startup file can remain owned after a user changes their
    # simulator selection. A later failure must undo every earlier XML write.
    $secondXml = Join-Path $fixture 'second-startup/exe.xml'
    New-Item -ItemType Directory -Path (Split-Path -Parent $secondXml) | Out-Null
    Copy-Item -LiteralPath $xml -Destination $secondXml
    $recordPath = Join-Path $app 'installation.json'
    $record = Get-Content -Raw -LiteralPath $recordPath | ConvertFrom-Json
    $record.startupPaths = @($xml,$secondXml)
    $record | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $recordPath -Encoding utf8
    $xmlBefore = (Get-FileHash -LiteralPath $xml).Hash
    $secondXmlBefore = (Get-FileHash -LiteralPath $secondXml).Hash
    $xmlLock = [IO.File]::Open($secondXml, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $refused = $false
    try { & (Join-Path $repoRoot 'installer/uninstall.ps1') -Installation $app -RemoveSettings }
    catch { $refused = $true } finally { $xmlLock.Dispose() }
    Assert-SettingsTest $refused 'Uninstall did not fail when its second owned startup file was locked.'
    Assert-Saved $saved
    Assert-SettingsTest ((Get-FileHash -LiteralPath $xml).Hash -eq $xmlBefore) 'Failure on the second startup file did not restore the first owned startup file.'
    Assert-SettingsTest ((Get-FileHash -LiteralPath $secondXml).Hash -eq $secondXmlBefore) 'Failed uninstall changed the locked second startup file.'

    $settingsLock = [IO.File]::Open((Join-Path $local 'Taxi Cam/hotkeys.ini'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $refused = $false
    try { & (Join-Path $repoRoot 'installer/uninstall.ps1') -Installation $app -RemoveSettings }
    catch { $refused = $true } finally { $settingsLock.Dispose() }
    Assert-SettingsTest $refused 'Locked settings did not fail uninstall removal.'
    Assert-Saved $saved
    Assert-SettingsTest ((Get-FileHash -LiteralPath $xml).Hash -eq $xmlBefore) 'Failed settings removal did not restore simulator startup.'
    Assert-SettingsTest ((Get-FileHash -LiteralPath $secondXml).Hash -eq $secondXmlBefore) 'Failed settings removal did not restore the second owned startup file.'
    & (Join-Path $repoRoot 'installer/uninstall.ps1') -Installation $app
    Assert-Saved $saved
    foreach ($startup in @($xml,$secondXml)) {
        [xml]$remaining = Get-Content -Raw -LiteralPath $startup
        Assert-SettingsTest ($remaining.SelectNodes('//Launch.Addon[Name="Taxi Cam"]').Count -eq 0) 'Successful uninstall retained an owned startup entry.'
        Assert-SettingsTest ($remaining.SelectNodes('//Launch.Addon[Name="Keep Me"]').Count -eq 1) 'Successful uninstall changed an unrelated startup entry.'
    }
    & (Join-Path $repoRoot 'installer/uninstall.ps1') -Installation $app -RemoveSettings
    foreach ($target in @(Get-TaxiSettingsTargets -Installation $app -IncludeMount)) {
        Assert-SettingsTest (-not (Test-Path -LiteralPath $target.path)) 'Explicit uninstall removal retained a known saved setting.'
    }
    Invoke-Install
    Assert-SettingsTest ((Get-FileHash -LiteralPath (Join-Path $app 'taxi-camera-mounts.cfg')).Hash -eq $defaultHash) 'Default reinstall into a recorded installation resurrected old simulator calibration.'
    foreach ($target in @(Get-TaxiSettingsTargets -Installation $app)) {
        if ($target.path -eq $settingsIni) {
            Assert-StampOnlySettings
            continue
        }
        Assert-SettingsTest (-not (Test-Path -LiteralPath $target.path)) 'Default reinstall resurrected known legacy profile settings.'
    }
    Assert-Saved $unrelated

    # Run the real outer transaction in a child Windows PowerShell process.
    # Only the fixture host masks process enumeration; LOCALAPPDATA is inherited
    # from this private directory and no executable is launched or stopped.
    $hostScript = Join-Path $fixture 'runtime-host.ps1'
    @'
param([string]$Runtime,[string]$Mode,[string]$Destination,[string]$SimulatorDirectory,[string]$ExeXml,[string]$PayloadDirectory,[string]$StateDirectory,[switch]$ResetSettings)
function Get-Process { param($Name,$Id,$ErrorAction) }
& $Runtime -Mode $Mode -Destination $Destination -SimulatorDirectory $SimulatorDirectory -ExeXml $ExeXml -PayloadDirectory $PayloadDirectory -StateDirectory $StateDirectory -ResetSettings:$ResetSettings
exit $LASTEXITCODE
'@ | Set-Content -LiteralPath $hostScript -Encoding utf8
    $saved = Seed-Settings
    $state = Join-Path $fixture 'outer-reset'
    $result = @(Invoke-Runtime Install $state -Reset)
    Assert-SettingsTest ($result[-1] -eq 0) 'Outer reset installation failed.'
    $result = @(Invoke-Runtime Rollback $state)
    Assert-SettingsTest ($result[-1] -eq 0) 'Outer Setup rollback failed.'
    Assert-Saved $saved
    Assert-Saved $unrelated

    $state = Join-Path $fixture 'outer-conflict'
    $result = @(Invoke-Runtime Install $state -Reset)
    Assert-SettingsTest ($result[-1] -eq 0) 'Outer conflict fixture install failed.'
    $concurrent = Join-Path $local 'Taxi Cam/profiles/ini-a380.ini'
    [IO.File]::WriteAllText($concurrent,'newer concurrent settings')
    $result = @(Invoke-Runtime Rollback $state)
    Assert-SettingsTest ($result[-1] -ne 0 -and [IO.File]::ReadAllText($concurrent) -eq 'newer concurrent settings') 'Outer rollback overwrote a newer settings writer.'
    Assert-Saved $unrelated

    $refused = $false
    try { Assert-TaxiSettingsPath (Join-Path $fixture 'outside.ini') (Join-Path $local 'Taxi Cam') } catch { $refused = $true }
    Assert-SettingsTest $refused 'Settings path bounds accepted an outside file.'
    $unsafeLocal = Join-Path $fixture 'unsafe-local'
    $unknownTarget = Join-Path $fixture 'junction-target'
    New-Item -ItemType Directory -Force -Path (Join-Path $unsafeLocal 'Taxi Cam'),$unknownTarget | Out-Null
    [IO.File]::WriteAllText((Join-Path $unknownTarget 'ini-a380.ini'),'outside junction target')
    New-Item -ItemType Junction -Path (Join-Path $unsafeLocal 'Taxi Cam/profiles') -Target $unknownTarget | Out-Null
    $env:LOCALAPPDATA = $unsafeLocal
    $refused = $false
    try { [void]@(Get-TaxiSettingsTargets -Installation $app) } catch { $refused = $_.Exception.Message -like '*reparse point*' }
    Assert-SettingsTest $refused 'A junction in the settings path was followed.'
    Assert-SettingsTest ([IO.File]::ReadAllText((Join-Path $unknownTarget 'ini-a380.ini')) -eq 'outside junction target') 'Reparse refusal changed its target.'
    $env:LOCALAPPDATA = Join-Path $fixture 'directory-local'
    New-Item -ItemType Directory -Force -Path (Join-Path $env:LOCALAPPDATA 'Taxi Cam/settings.ini') | Out-Null
    $refused = $false
    try { [void]@(Get-TaxiSettingsTargets -Installation $app) } catch { $refused = $_.Exception.Message -like '*file is a directory*' }
    Assert-SettingsTest $refused 'A directory was accepted as a known settings file.'
    Write-Output "PASS settings lifecycle: $checks checks; keep defaults, one-shot camera_rate=10 migrate, retired exposure keys left alone, revision-1 force-5 restamp, user override kept, missing-key migrate, rollback of rate and stamp, reset defaults with locked startup fallback, multiple startup file rollback, inner/outer rollback, concurrent writer, legacy imports, explicit uninstall removal, unrelated files and path guards. Fixture: $fixture"
} finally {
    $env:LOCALAPPDATA = $priorLocal
    $env:TEMP = $priorTemp
    $env:TMP = $priorTmp
}
