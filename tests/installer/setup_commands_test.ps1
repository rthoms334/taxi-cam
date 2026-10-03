[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$SetupHost)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Drives the companion's installer commands through a test host whose process
# list comes from TAXI_SETUP_TEST_PROCESSES. Every file lives in a fresh build/
# fixture; LOCALAPPDATA and APPDATA point into it and are restored afterwards.
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repoRoot 'tests/support/installer_fixture.ps1')
$hostPath = (Resolve-Path -LiteralPath $SetupHost).Path
$fixture = Join-Path $repoRoot ('build/setup-command-tests/' + [Guid]::NewGuid().ToString('N'))
$script:checks = 0
function Assert-That([bool]$Condition, [string]$Message) {
    $script:checks++
    if (-not $Condition) { throw $Message }
}
function Invoke-Setup([string]$Command, [string]$Destination, [string[]]$Arguments = @(), [string]$Processes = '-') {
    $state = Join-Path $fixture ('state-' + [Guid]::NewGuid().ToString('N'))
    $env:TAXI_SETUP_TEST_PROCESSES = $Processes
    $quoted = @('--destination', $Destination, '--state', $state) + $Arguments | ForEach-Object { '"' + $_ + '"' }
    $process = Start-Process -FilePath $hostPath -ArgumentList (@($Command) + $quoted) -WindowStyle Hidden -PassThru -Wait
    $errorPath = Join-Path $state 'error.txt'
    $warning = Join-Path $state 'warning.txt'
    return [pscustomobject]@{
        Code = $process.ExitCode; State = $state
        Error = $(if (Test-Path -LiteralPath $errorPath) { [IO.File]::ReadAllText($errorPath) } else { '' })
        Warning = $(if (Test-Path -LiteralPath $warning) { [IO.File]::ReadAllText($warning) } else { '' })
    }
}
function Get-Record([string]$App) { Get-Content -Raw -LiteralPath (Join-Path $App 'installation.json') | ConvertFrom-Json }
function Get-Hash([string]$Path) { (Get-FileHash -LiteralPath $Path).Hash }
function Get-TaxiEntries([string]$Path) {
    [xml]$document = Get-Content -Raw -LiteralPath $Path
    return ,@($document.SelectNodes('/SimBase.Document/Launch.Addon[Name="Taxi Cam"]'))
}
$savedLocal = $env:LOCALAPPDATA
$savedRoaming = $env:APPDATA
try {
    $env:LOCALAPPDATA = Join-Path $fixture 'local'
    $env:APPDATA = Join-Path $fixture 'roaming'
    $sim = Join-Path $fixture 'sim'
    $app = Join-Path $fixture 'app'
    New-Item -ItemType Directory -Force -Path $env:LOCALAPPDATA,$env:APPDATA,$sim,$app | Out-Null
    New-TaxiFixtureImage (Join-Path $sim 'FlightSimulator2024.exe') $false
    New-TaxiFixtureImage (Join-Path $sim 'SimConnect_internal.dll')
    [IO.File]::WriteAllText((Join-Path $sim 'taxi-camera-native.addon64'), 'legacy fixture')
    [IO.File]::WriteAllText((Join-Path $sim 'dxgi.dll'), 'unrelated graphics fixture')
    $xml = Join-Path $fixture 'config/exe.xml'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $xml) | Out-Null
    [IO.File]::WriteAllText($xml, '<SimBase.Document Type="Launch"><Launch.Addon><Name>Keep Me</Name><Path>C:\Other.exe</Path></Launch.Addon></SimBase.Document>')
    $simExe = Join-Path $sim 'FlightSimulator2024.exe'
    $appExe = Join-Path $app 'taxi-cam.exe'

    # Pre-installation checks never depend on, or touch, real running programs.
    $result = Invoke-Setup check $app @('--simulator', $sim) "FlightSimulator2024|$simExe"
    Assert-That ($result.Code -ne 0 -and $result.Error -like 'Close MSFS 2024*') 'Running simulator was not refused.'
    $result = Invoke-Setup check $app @('--simulator', $sim) ('380-taxi-cam|' + (Join-Path $app '380-taxi-cam.exe'))
    Assert-That ($result.Code -ne 0 -and $result.Error -like 'Exit the taxi camera app*') 'Running former-name companion was not refused.'
    $result = Invoke-Setup check $app @('--simulator', $sim) 'taxi-cam|C:\Other Installation\taxi-cam.exe'
    Assert-That ($result.Code -eq 0) 'Another installation''s companion blocked a keep-settings install.'
    $result = Invoke-Setup check $app @('--simulator', $sim, '--settings-closed') 'taxi-cam|C:\Other Installation\taxi-cam.exe'
    Assert-That ($result.Code -ne 0 -and $result.Error -like 'Exit every Taxi Cam*') 'Settings reset accepted another running companion.'
    $result = Invoke-Setup check $app @('--simulator', (Join-Path $fixture 'missing-sim'))
    Assert-That ($result.Code -ne 0 -and $result.Error -like 'Select the MSFS 2024 Content directory*') 'Missing simulator accepted.'
    $result = Invoke-Setup check $sim @('--simulator', $sim)
    Assert-That ($result.Code -ne 0 -and $result.Error -like 'Use a dedicated companion installation directory*') 'Simulator folder accepted as installation.'
    $result = Invoke-Setup check $app @('--simulator', $sim, '--exe-xml', (Join-Path $fixture 'config/other.xml'))
    Assert-That ($result.Code -ne 0 -and $result.Error -like '*named exe.xml*') 'Wrong startup file name accepted.'
    $result = Invoke-Setup check $app @('--simulator', $sim, '--update-from-pid', [string]$PID)
    Assert-That ($result.Code -ne 0 -and $result.Error -like 'UPDATEFROMPID does not identify*') 'Unrelated update process accepted.'
    Remove-Item -LiteralPath (Join-Path $sim 'SimConnect_internal.dll')
    $result = Invoke-Setup check $app @('--simulator', $sim)
    Assert-That ($result.Code -ne 0 -and $result.Error -like '*SimConnect_internal.dll*') 'Missing simulator component was not diagnosed.'
    New-TaxiFixtureImage (Join-Path $sim 'SimConnect_internal.dll')
    $result = Invoke-Setup check $app @('--simulator', $sim)
    Assert-That ($result.Code -eq 0 -and -not @(Get-ChildItem -LiteralPath $app -Force).Count) "Valid check failed or left files: $($result.Error)"

    # Configure after Setup copied the files: startup entry, record and legacy add-on.
    $result = Invoke-Setup configure $app @('--simulator', $sim, '--startup', 'automatic', '--exe-xml', $xml)
    Assert-That ($result.Code -eq 0 -and -not $result.Warning) "Configure failed: $($result.Error)"
    $record = Get-Record $app
    Assert-That ($record.startupStatus -eq 'configured' -and $record.startupUpdated -and $record.exeXml -eq $xml -and $record.simulator -eq $simExe) 'Installation record is incorrect.'
    Assert-That ($record.exeXmlInstalledHash -eq (Get-Hash $xml)) 'Record does not own the committed startup bytes.'
    Assert-That (@($record.startupPaths) -contains $xml) 'Startup path not recorded.'
    $entries = Get-TaxiEntries $xml
    Assert-That ($entries.Count -eq 1 -and $entries[0].Path -eq $appExe -and $entries[0].CommandLine -eq ('--background --simulator "' + $simExe + '"')) 'Startup entry is incorrect.'
    [xml]$document = Get-Content -Raw -LiteralPath $xml
    Assert-That ($document.SelectNodes('//Launch.Addon[Name="Keep Me"]').Count -eq 1) 'Unrelated startup entry changed.'
    Assert-That ((Test-Path -LiteralPath $record.legacyBackup) -and -not (Test-Path -LiteralPath (Join-Path $sim 'taxi-camera-native.addon64'))) 'Legacy add-on not disabled and retained.'
    Assert-That ([IO.File]::ReadAllText((Join-Path $sim 'dxgi.dll')) -eq 'unrelated graphics fixture') 'Unrelated simulator file changed.'
    Assert-That (Test-Path -LiteralPath (Join-Path $app 'setup-diagnostics.log')) 'Setup diagnostics missing.'
    $firstLegacy = $record.legacyBackup
    $configuredHash = Get-Hash $xml
    $result = Invoke-Setup configure $app @('--simulator', $sim, '--startup', 'automatic')
    $record = Get-Record $app
    Assert-That ($result.Code -eq 0 -and $record.exeXml -eq $xml -and (Get-TaxiEntries $xml).Count -eq 1) 'Same-simulator update did not reuse its startup file.'
    Assert-That ($record.legacyBackup -eq $firstLegacy) 'Update lost the original legacy backup.'
    Assert-That ((Get-Hash $xml) -eq $configuredHash) 'Repeated configure changed an already configured startup file.'

    # A locked startup file falls back to manual startup and reports it.
    $lock = [IO.File]::Open($xml, 'Open', 'Read', 'Read')
    try { $result = Invoke-Setup configure $app @('--simulator', $sim, '--startup', 'automatic', '--exe-xml', $xml) } finally { $lock.Dispose() }
    $record = Get-Record $app
    Assert-That ($result.Code -eq 0 -and $result.Warning -like 'Taxi Cam was installed, but automatic startup could not be configured*') 'Locked startup file did not warn.'
    Assert-That ($record.startupStatus -eq 'unchanged' -and -not $record.startupUpdated -and $record.startupError) 'Locked startup fallback record is incorrect.'
    Assert-That ((Get-Hash $xml) -eq $configuredHash -and -not @(Get-ChildItem -LiteralPath (Split-Path -Parent $xml) -Filter 'exe.xml.taxi-*.tmp').Count) 'Locked startup fallback changed files.'
    $result = Invoke-Setup configure $app @('--simulator', $sim, '--startup', 'manual')
    Assert-That ($result.Code -eq 0 -and $result.Warning -like 'Automatic startup was not changed*' -and (Get-Hash $xml) -eq $configuredHash) 'Manual startup changed exe.xml.'

    # Steam launch files with a copied SimConnect header are repaired, keeping the original.
    $steamApp = Join-Path $fixture 'steam-app'
    $steamXml = Join-Path $fixture 'steam/exe.xml'
    New-Item -ItemType Directory -Force -Path $steamApp,(Split-Path -Parent $steamXml) | Out-Null
    $mislabelled = @'
<?xml version="1.0" encoding="utf-8"?>
<!-- Steam launch configuration: keep this comment -->
<SimBase.Document Type="SimConnect" version="1,0">
  <Descr>SimConnect</Descr>
  <Filename>SimConnect.xml</Filename>
  <Disabled>False</Disabled>
  <Launch.Addon><Name>Steam Utility A</Name><Disabled>False</Disabled><Path>C:\Other Apps\utility-a.exe</Path><CommandLine>--keep &amp; preserve</CommandLine></Launch.Addon>
</SimBase.Document>
'@
    [IO.File]::WriteAllText($steamXml, $mislabelled, [Text.UTF8Encoding]::new($true))
    $steamOriginal = Get-Hash $steamXml
    $result = Invoke-Setup configure $steamApp @('--simulator', $sim, '--startup', 'automatic', '--exe-xml', $steamXml)
    $record = Get-Record $steamApp
    Assert-That ($result.Code -eq 0 -and $record.startupStatus -eq 'configured' -and (Get-Hash $record.exeXmlBackup) -eq $steamOriginal) 'Steam header repair failed or lost the original.'
    [xml]$document = Get-Content -Raw -LiteralPath $steamXml
    Assert-That ($document.DocumentElement.Type -eq 'Launch' -and $document.SelectNodes('/SimBase.Document/Launch.Addon').Count -eq 2) 'Steam header not normalized.'
    Assert-That ([IO.File]::ReadAllText($steamXml).Contains('<!-- Steam launch configuration: keep this comment -->')) 'Steam repair dropped a document comment.'

    # Store-to-Steam: a remembered startup file follows only its own simulator.
    $steamSim = Join-Path $fixture 'steam-sim'
    New-TaxiFixtureImage (Join-Path $steamSim 'FlightSimulator2024.exe') $false
    New-TaxiFixtureImage (Join-Path $steamSim 'SimConnect_internal.dll')
    $roamingXml = Join-Path $env:APPDATA 'Microsoft Flight Simulator 2024/exe.xml'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $roamingXml) | Out-Null
    [IO.File]::WriteAllText($roamingXml, '<SimBase.Document Type="Launch"><Launch.Addon><Name>Other</Name><Path>C:\o.exe</Path></Launch.Addon></SimBase.Document>')
    $storeHash = Get-Hash $xml
    $result = Invoke-Setup configure $app @('--simulator', $steamSim, '--startup', 'automatic')
    $record = Get-Record $app
    Assert-That ($result.Code -eq 0 -and $record.exeXml -eq $roamingXml -and $record.startupStatus -eq 'configured' -and (Get-Hash $xml) -eq $storeHash) 'Simulator change reused the previous startup file or skipped discovery.'
    Assert-That ((Get-TaxiEntries $roamingXml)[0].CommandLine -eq ('--background --simulator "' + (Join-Path $steamSim 'FlightSimulator2024.exe') + '"')) 'Discovered startup kept the previous simulator.'
    Assert-That (@($record.startupPaths).Count -eq 2) 'Both owned startup files are not recorded.'
    $storeXml = Join-Path $env:LOCALAPPDATA 'Packages/Microsoft.Limitless_8wekyb3d8bbwe/LocalCache/exe.xml'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $storeXml) | Out-Null
    [IO.File]::WriteAllText($storeXml, '<SimBase.Document Type="Launch"/>')
    $ambiguousApp = Join-Path $fixture 'ambiguous-app'
    $result = Invoke-Setup configure $ambiguousApp @('--simulator', $sim, '--startup', 'automatic')
    $record = Get-Record $ambiguousApp
    Assert-That ($result.Code -eq 0 -and -not $record.startupUpdated -and $record.startupError -like '*Select the simulator exe.xml*') 'Ambiguous discovery guessed a startup file.'

    # Discovery for the wizard.
    $result = Invoke-Setup discover $app
    $choices = [IO.File]::ReadAllText((Join-Path $result.State 'choices.ini'), [Text.Encoding]::Unicode)
    Assert-That ($result.Code -eq 0 -and $choices.Contains("Simulator=$steamSim") -and $choices.Contains("ExeXml=$roamingXml") -and $choices.Contains('Startup=automatic')) 'Discovery did not return the recorded choices.'
    $result = Invoke-Setup discover (Join-Path $fixture 'new-app')
    $choices = [IO.File]::ReadAllText((Join-Path $result.State 'choices.ini'), [Text.Encoding]::Unicode)
    Assert-That ($result.Code -eq 0 -and $choices.Contains('ExeXml=' + [Environment]::NewLine)) 'Ambiguous standard startup files were guessed.'

    # Settings reset removes only known files and refuses while any companion runs.
    $known = @('Taxi Cam/settings.ini','Taxi Cam/hotkeys.ini','Taxi Cam/startup-state','Taxi Cam/profiles/fbw-a380x.ini','380 Taxi Cam/profiles/pmdg-777.ini')
    $unrelated = @('Taxi Cam/logs/keep.log','Taxi Cam/profiles/custom-aircraft.ini','380 Taxi Cam/profiles/unknown.ini')
    function Seed-Settings {
        foreach ($relative in @($known + $unrelated)) {
            $path = Join-Path $env:LOCALAPPDATA $relative
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
            [IO.File]::WriteAllText($path, "fixture $relative")
        }
        [IO.File]::WriteAllText((Join-Path $app 'taxi-camera-mounts.cfg'), 'installed calibration fixture')
    }
    function Assert-Known([bool]$Present, [string]$Message) {
        foreach ($relative in $known) { Assert-That ((Test-Path -LiteralPath (Join-Path $env:LOCALAPPDATA $relative)) -eq $Present) "$Message $relative" }
        foreach ($relative in $unrelated) { Assert-That (Test-Path -LiteralPath (Join-Path $env:LOCALAPPDATA $relative)) "Unrelated file removed: $relative" }
    }
    Seed-Settings
    $result = Invoke-Setup configure $app @('--simulator', $sim, '--startup', 'manual', '--reset-settings') 'taxi-cam|C:\Other\taxi-cam.exe'
    Assert-That ($result.Code -ne 0 -and $result.Error -like 'Exit every Taxi Cam*') 'Reset accepted a running companion.'
    Assert-Known $true 'Refused reset removed'
    $result = Invoke-Setup configure $app @('--simulator', $sim, '--startup', 'manual', '--reset-settings')
    Assert-That ($result.Code -eq 0) "Reset failed: $($result.Error)"
    Assert-Known $false 'Reset kept'
    Assert-That (Test-Path -LiteralPath (Join-Path $app 'taxi-camera-mounts.cfg')) 'Reset removed calibration that Setup replaces itself.'

    # Uninstall removes only this installation's entries from every owned file.
    Seed-Settings
    $other = Join-Path $fixture 'other-app'
    $otherEntry = '<Launch.Addon><Name>Taxi Cam</Name><Path>' + (Join-Path $other 'taxi-cam.exe') + '</Path></Launch.Addon>'
    $result = Invoke-Setup configure $app @('--simulator', $sim, '--startup', 'automatic', '--exe-xml', $xml)
    $result = Invoke-Setup configure $app @('--simulator', $steamSim, '--startup', 'automatic', '--exe-xml', $roamingXml)
    $record = Get-Record $app
    Assert-That (@($record.startupPaths).Count -eq 2 -and (Get-TaxiEntries $xml).Count -eq 1 -and (Get-TaxiEntries $roamingXml).Count -eq 1) 'Two owned startup files expected.'
    $xmlBefore = Get-Hash $xml
    $roamingBefore = Get-Hash $roamingXml
    $lock = [IO.File]::Open($roamingXml, 'Open', 'Read', 'Read')
    try { $result = Invoke-Setup uninstall $app @('--remove-settings') } finally { $lock.Dispose() }
    Assert-That ($result.Code -ne 0) 'Uninstall ignored a locked owned startup file.'
    Assert-That ((Get-Hash $xml) -eq $xmlBefore -and (Get-Hash $roamingXml) -eq $roamingBefore) 'Failed uninstall did not restore the first startup file.'
    Assert-Known $true 'Failed uninstall removed'
    $settingsLock = [IO.File]::Open((Join-Path $env:LOCALAPPDATA 'Taxi Cam/hotkeys.ini'), 'Open', 'Read', 'Read')
    try { $result = Invoke-Setup uninstall $app @('--remove-settings') } finally { $settingsLock.Dispose() }
    Assert-That ($result.Code -ne 0 -and (Get-Hash $xml) -eq $xmlBefore -and (Get-Hash $roamingXml) -eq $roamingBefore) 'Failed settings removal did not restore startup files.'
    Assert-Known $true 'Failed settings removal removed'
    $result = Invoke-Setup uninstall $app @('--remove-settings') 'taxi-cam|C:\Other\taxi-cam.exe'
    Assert-That ($result.Code -ne 0 -and $result.Error -like 'Exit every Taxi Cam*') 'Uninstall removal accepted a running companion.'
    $result = Invoke-Setup uninstall $app
    Assert-That ($result.Code -eq 0 -and (Get-TaxiEntries $xml).Count -eq 0 -and (Get-TaxiEntries $roamingXml).Count -eq 0) "Uninstall kept an owned entry: $($result.Error)"
    [xml]$document = Get-Content -Raw -LiteralPath $xml
    Assert-That ($document.SelectNodes('//Launch.Addon[Name="Keep Me"]').Count -eq 1) 'Uninstall changed an unrelated entry.'
    Assert-Known $true 'Keep-settings uninstall removed'
    $result = Invoke-Setup uninstall $app @('--remove-settings')
    Assert-That ($result.Code -eq 0 -and -not (Test-Path -LiteralPath (Join-Path $app 'taxi-camera-mounts.cfg'))) "Settings removal failed: $($result.Error)"
    Assert-Known $false 'Settings removal kept'

    # Entries of another installation are never removed.
    [IO.File]::WriteAllText($xml, '<SimBase.Document Type="Launch">' + $otherEntry + '</SimBase.Document>')
    $otherHash = Get-Hash $xml
    $result = Invoke-Setup uninstall $app
    Assert-That ($result.Code -eq 0 -and (Get-Hash $xml) -eq $otherHash) 'Uninstall removed another installation''s entry.'

    # A failure to write the installation record undoes every configure change,
    # so the uninstaller never meets a startup entry that no record describes.
    $failApp = Join-Path $fixture 'record-failure-app'
    New-Item -ItemType Directory -Force -Path (Join-Path $failApp 'installation.json') | Out-Null
    [IO.File]::WriteAllText($xml, '<SimBase.Document Type="Launch"><Launch.Addon><Name>Keep Me</Name><Path>C:\Other.exe</Path></Launch.Addon></SimBase.Document>')
    $failXmlHash = Get-Hash $xml
    [IO.File]::WriteAllText((Join-Path $sim 'taxi-camera-native.addon64'), 'legacy fixture')
    $disabledBefore = @(Get-ChildItem -LiteralPath $sim -Filter 'taxi-camera-native.addon64.disabled-native-*').Count
    Seed-Settings
    $result = Invoke-Setup configure $failApp @('--simulator', $sim, '--startup', 'automatic', '--exe-xml', $xml, '--reset-settings')
    Assert-That ($result.Code -ne 0) 'A failed installation record write reported success.'
    Assert-That ((Get-Hash $xml) -eq $failXmlHash) 'Failed configure did not restore exe.xml.'
    Assert-That ([IO.File]::ReadAllText((Join-Path $sim 'taxi-camera-native.addon64')) -eq 'legacy fixture' -and
        @(Get-ChildItem -LiteralPath $sim -Filter 'taxi-camera-native.addon64.disabled-native-*').Count -eq $disabledBefore) 'Failed configure did not restore the legacy add-on.'
    Assert-Known $true 'Failed configure did not restore reset settings:'
    Remove-Item -LiteralPath (Join-Path $sim 'taxi-camera-native.addon64')

    # Settings paths never follow junctions.
    $unsafe = Join-Path $fixture 'unsafe-local'
    $target = Join-Path $fixture 'junction-target'
    New-Item -ItemType Directory -Force -Path (Join-Path $unsafe 'Taxi Cam'),$target | Out-Null
    [IO.File]::WriteAllText((Join-Path $target 'ini-a380.ini'), 'outside junction target')
    New-Item -ItemType Junction -Path (Join-Path $unsafe 'Taxi Cam/profiles') -Target $target | Out-Null
    $env:LOCALAPPDATA = $unsafe
    $result = Invoke-Setup uninstall $app @('--remove-settings')
    Assert-That ($result.Code -ne 0 -and $result.Error -like '*reparse point*') 'A junction in the settings path was followed.'
    Assert-That ([IO.File]::ReadAllText((Join-Path $target 'ini-a380.ini')) -eq 'outside junction target') 'Reparse refusal changed its target.'
    Write-Output "PASS setup commands: $script:checks checks; process guards, prerequisites, startup configure/repair/discovery/fallback, record, legacy add-on, settings reset/removal, uninstall rollback and path guards. Fixture: $fixture"
} finally {
    $env:LOCALAPPDATA = $savedLocal
    $env:APPDATA = $savedRoaming
    Remove-Item Env:\TAXI_SETUP_TEST_PROCESSES -ErrorAction SilentlyContinue
}
