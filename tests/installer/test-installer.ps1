[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Installer)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Set-StrictMode -Version Latest
. (Join-Path $repoRoot 'tests/support/installer_fixture.ps1')
$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$receipt = Get-Content -Raw -LiteralPath ($installerPath + '.json') | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $installerPath).Hash -ne $receipt.installerSha256) { throw 'Installer hash does not match its build receipt.' }
if ([IO.Path]::GetFileName($installerPath) -cne "taxi-cam-$($receipt.version)-windows-x64-setup.exe") { throw 'Installer filename must contain the application version without a build-number suffix.' }
$testRoot = Join-Path $repoRoot ('build/installer-tests/' + [Guid]::NewGuid().ToString('N'))
$sim = Join-Path $testRoot 'sim'; $xmlPath = Join-Path $testRoot 'config/exe.xml'
New-Item -ItemType Directory -Force -Path $sim,(Split-Path -Parent $xmlPath) | Out-Null
$savedLocalAppData = $env:LOCALAPPDATA
$fixtureLocalAppData = Join-Path $testRoot 'localappdata'
New-Item -ItemType Directory -Path $fixtureLocalAppData | Out-Null
$env:LOCALAPPDATA = $fixtureLocalAppData
# Startup discovery reads APPDATA too; never let a fixture reach the real MSFS exe.xml.
$savedAppData = $env:APPDATA
$env:APPDATA = Join-Path $testRoot 'appdata'
New-Item -ItemType Directory -Path $env:APPDATA | Out-Null
try {
& (Join-Path $repoRoot 'build/native/application-icon-test.exe') $installerPath (Join-Path $testRoot 'setup-icon')
if ($LASTEXITCODE -ne 0) { throw 'Setup shell icon validation failed.' }
New-TaxiFixtureImage (Join-Path $sim 'FlightSimulator2024.exe') $false
New-TaxiFixtureImage (Join-Path $sim 'SimConnect_internal.dll')
$xml = '<?xml version="1.0"?><SimBase.Document Type="Launch"><Disabled>False</Disabled><Launch.Addon><Name>Other Addon</Name><Path>C:\Other\other.exe</Path></Launch.Addon></SimBase.Document>'
function Invoke-Setup([string]$Executable,[string]$App,[string]$Log,[switch]$Paths,[switch]$ResetSettings,[string]$Startup = '',[switch]$WithoutXml) {
    $arguments = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/DIR="' + $App + '"'),('/LOG="' + $Log + '"'))
    if ($Paths) {
        $arguments += '/SIMULATORDIR="' + $sim + '"'
        if (-not $WithoutXml) { $arguments += '/EXEXML="' + $xmlPath + '"' }
    }
    if ($ResetSettings) { $arguments += '/RESETSETTINGS=1' }
    if ($Startup) { $arguments += '/STARTUP=' + $Startup }
    $process = Start-Process -FilePath $Executable -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(60000)) {
        # Inno can own a temporary setup child; stop only this invocation's tree.
        if (-not $process.HasExited) {
            & (Join-Path $env:WINDIR 'System32/taskkill.exe') /PID $process.Id /T /F *> $null
            [void]$process.WaitForExit(5000)
        }
        throw "Installer test timed out; attempted to stop its process tree $($process.Id), log $Log"
    }
    $process.Refresh()
    return $process.ExitCode
}
function Invoke-Uninstall([string]$App,[string]$Log,[switch]$RemoveSettings) {
    $arguments = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/LOG="' + $Log + '"'))
    if ($RemoveSettings) { $arguments += '/REMOVESETTINGS=1' }
    $process = Start-Process -FilePath (Join-Path $App 'unins000.exe') -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(60000)) { throw "Uninstaller test timed out; process $($process.Id), log $Log" }
    $process.Refresh()
    # The uninstaller removes its own files from a second process after this one
    # exits. Wait for that, so the next Setup reuses unins000 instead of unins001.
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while (@(Get-ChildItem -LiteralPath $App -Filter 'unins000.*' -ErrorAction SilentlyContinue).Count -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 200
    }
    return $process.ExitCode
}
function Assert-That([bool]$Condition,[string]$Message) { if (-not $Condition) { throw $Message } }
function Get-Record([string]$App) { Get-Content -Raw -LiteralPath (Join-Path $App 'installation.json') | ConvertFrom-Json }
function Get-Hash([string]$Path) { (Get-FileHash -LiteralPath $Path).Hash }
function Get-Entries([string]$Name = '') {
    [xml]$launch = Get-Content -Raw -LiteralPath $xmlPath
    $query = if ($Name) { "//Launch.Addon[Name=""$Name""]" } else { '//Launch.Addon' }
    return ,@($launch.SelectNodes($query))
}
$knownSettings = @('Taxi Cam/settings.ini','Taxi Cam/hotkeys.ini','Taxi Cam/startup-state')
foreach ($key in @('fbw-a380x','ini-a350-900','ini-a350-1000','ini-a380','pmdg-777','pmdg-777-300er','pmdg-777f','aerosoft-a346','ini-a340-300')) {
    foreach ($folder in @('Taxi Cam','380 Taxi Cam')) { $knownSettings += "$folder/profiles/$key.ini" }
}
$unrelatedSettings = @('Taxi Cam/logs/history.log','Taxi Cam/profiles/custom-aircraft.ini','Taxi Cam/readme.txt','380 Taxi Cam/profiles/custom-aircraft.ini')
function Seed-Settings([string]$Tag) {
    $hashes = @{}
    foreach ($relative in @($knownSettings + $unrelatedSettings)) {
        $path = Join-Path $fixtureLocalAppData $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        [IO.File]::WriteAllText($path, "$Tag settings fixture: $relative")
        $hashes[$relative] = Get-Hash $path
    }
    return $hashes
}
# Setup leaves saved settings alone unless a reset or removal is requested; the
# one-shot camera-rate and exposure defaults now belong to the companion.
function Assert-Settings($Hashes,[switch]$Removed) {
    foreach ($relative in @($knownSettings + $unrelatedSettings)) {
        $path = Join-Path $fixtureLocalAppData $relative
        if ($Removed -and $relative -in $knownSettings) {
            Assert-That (-not (Test-Path -LiteralPath $path)) "Opt-in settings removal retained $relative."
        } else {
            Assert-That ((Test-Path -LiteralPath $path -PathType Leaf) -and (Get-Hash $path) -eq $Hashes[$relative]) "Saved or unrelated settings changed: $relative."
        }
    }
}
# Exercise the exact production executable through a guaranteed pre-write failure.
# Installing into the simulator itself is refused (as is a running simulator);
# Setup must return failure and leave all fixture bytes unchanged.
$xml | Set-Content -LiteralPath $xmlPath
$before = Get-Hash $xmlPath
$exitCode = Invoke-Setup $installerPath $sim (Join-Path $testRoot 'production-rejection.log') -Paths
Assert-That ($exitCode -ne 0) 'The production installer accepted the simulator directory as its application directory.'
Assert-That ((Get-Hash $xmlPath) -eq $before) 'The rejected production install modified exe.xml.'
Assert-That (-not (Test-Path -LiteralPath (Join-Path $sim 'taxi-cam.exe'))) 'The rejected production install wrote a live executable.'

# A separately compiled fixture suppresses registration and shortcuts. Its
# taxi-cam.exe is the setup command test host, whose process list comes from
# TAXI_SETUP_TEST_PROCESSES, so local simulator sessions remain untouched.
$payload = Join-Path $testRoot 'payload'
Copy-Item -LiteralPath $receipt.compilerPayload -Destination $payload -Recurse
Copy-Item -LiteralPath (Join-Path $repoRoot 'build/native/setup-host-test.exe') -Destination (Join-Path $payload 'taxi-cam.exe') -Force
$compiler = & (Join-Path $repoRoot 'installer/bootstrap.ps1')
& $compiler "/DPayloadDir=$payload" "/DAppIcon=$(Join-Path $repoRoot 'src/app/taxi-cam.ico')" "/DAppVersion=$($receipt.version)" "/DBuildNumber=$($receipt.buildNumber)" '/DInstallerTest=1' '/DOutputBase=isolated-setup' "/O$testRoot" (Join-Path $repoRoot 'installer/taxi-cam.iss') *> (Join-Path $testRoot 'compiler.log')
if ($LASTEXITCODE -ne 0) { throw "Fixture compilation failed: $testRoot" }
$fixture = Join-Path $testRoot 'isolated-setup.exe'; $app = Join-Path $testRoot 'app'
$env:TAXI_SETUP_TEST_PROCESSES = '-'

# A running simulator stops Setup before any application or startup write.
$env:TAXI_SETUP_TEST_PROCESSES = 'FlightSimulator2024|' + (Join-Path $sim 'FlightSimulator2024.exe')
$runningApp = Join-Path $testRoot 'running-simulator'
$runningLog = Join-Path $testRoot 'running-simulator.log'
$exitCode = Invoke-Setup $fixture $runningApp $runningLog -Paths
Assert-That ($exitCode -ne 0 -and (Get-Content -Raw -LiteralPath $runningLog).Contains('Close MSFS 2024')) 'A running simulator did not stop Setup.'
Assert-That ((Get-Hash $xmlPath) -eq $before -and -not (Test-Path -LiteralPath (Join-Path $runningApp 'taxi-cam.exe'))) 'Running-simulator refusal wrote files.'
$env:TAXI_SETUP_TEST_PROCESSES = '-'

# Dependency checking stays active: a missing SimConnect client is refused.
$client = Join-Path $sim 'SimConnect_internal.dll'
Remove-Item -LiteralPath $client
$missingApp = Join-Path $testRoot 'missing-dependency'
$dependencyLog = Join-Path $testRoot 'missing-dependency.log'
$exitCode = Invoke-Setup $fixture $missingApp $dependencyLog -Paths
Assert-That ($exitCode -ne 0) 'Installer accepted a missing SimConnect client.'
Assert-That ((Get-Content -Raw -LiteralPath $dependencyLog).Contains('SimConnect_internal.dll')) 'Installer did not explain the missing SimConnect dependency.'
Assert-That ((Get-Hash $xmlPath) -eq $before) 'Dependency refusal changed startup configuration.'
Assert-That (-not (Test-Path -LiteralPath (Join-Path $missingApp 'taxi-cam.exe'))) 'Dependency refusal wrote application files.'
New-TaxiFixtureImage $client

# First install keeps existing calibration and every saved setting.
$savedSettings = Seed-Settings 'initial'
New-Item -ItemType Directory -Path $app -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $app 'taxi-camera-mounts.cfg'), '# existing calibrated camera mounts')
$mount = Join-Path $app 'taxi-camera-mounts.cfg'
$existingMountHash = Get-Hash $mount
$exitCode = Invoke-Setup $fixture $app (Join-Path $testRoot 'install.log') -Paths
Assert-That ($exitCode -eq 0) "Isolated first install failed ($exitCode): $testRoot"
Assert-Settings $savedSettings
Assert-That ((Get-Hash $mount) -eq $existingMountHash) 'Default install replaced existing calibration.'
foreach ($name in @('taxi-cam.exe','taxi-camera-bridge.dll','LICENSE.txt','THIRD_PARTY_NOTICES.txt')) {
    Assert-That ((Get-Hash (Join-Path $app $name)) -eq (Get-Hash (Join-Path $payload $name))) "Installer omitted or changed $name."
}
Assert-That ((Get-Hash (Join-Path $app 'taxi-camera-bridge.dll')) -eq $receipt.files.PSObject.Properties['taxi-camera-bridge.dll'].Value) 'Installed bridge differs from the validated build.'
Assert-That (@(Get-ChildItem -LiteralPath $app -Filter '*.ps1' -Recurse).Count -eq 0) 'Installer left PowerShell files in the application.'
Assert-That (@(Get-ChildItem -LiteralPath $app -Directory).Count -eq 0) 'Installer left support or staging folders in the application.'
Assert-That ((Get-Entries).Count -eq 2 -and (Get-Entries 'Taxi Cam').Count -eq 1) 'Startup entry not added beside the existing entry.'
$record = Get-Record $app
Assert-That ($record.startupStatus -eq 'configured' -and $record.startupUpdated -and $record.exeXml -eq $xmlPath) 'Installation record does not describe the configured startup.'
Add-Content -LiteralPath $mount -Value '# user calibration fixture'
$mountHash = Get-Hash $mount

# Upgrade an installation that still uses the former product name.
$oldExe = Join-Path $app '380-taxi-cam.exe'
Move-Item -LiteralPath (Join-Path $app 'taxi-cam.exe') -Destination $oldExe
[xml]$launch = Get-Content -Raw -LiteralPath $xmlPath
$launch.SelectSingleNode('//Launch.Addon[Name="Taxi Cam"]/Name').InnerText = '380 Taxi Cam'
$launch.SelectSingleNode('//Launch.Addon[Name="380 Taxi Cam"]/Path').InnerText = $oldExe
$launch.Save($xmlPath)
$exitCode = Invoke-Setup $fixture $app (Join-Path $testRoot 'upgrade.log')
Assert-That ($exitCode -eq 0) "Isolated upgrade with remembered paths failed ($exitCode): $testRoot"
Assert-That ((Get-Hash $mount) -eq $mountHash) 'Upgrade overwrote user calibration.'
Assert-Settings $savedSettings
Assert-That ((Get-Entries).Count -eq 2 -and (Get-Entries 'Taxi Cam').Count -eq 1) 'Upgrade duplicated or did not rename the startup entry.'
Assert-That ((Get-Entries 'Taxi Cam')[0].Path -eq (Join-Path $app 'taxi-cam.exe')) 'Upgrade did not point startup at the renamed companion.'
Assert-That (-not (Test-Path -LiteralPath $oldExe)) 'Upgrade retained the obsolete companion executable.'

# Default uninstall keeps settings, calibration and the record.
$recordHash = Get-Hash (Join-Path $app 'installation.json')
$exitCode = Invoke-Uninstall $app (Join-Path $testRoot 'uninstall.log')
Assert-That ($exitCode -eq 0) 'Isolated uninstall failed.'
Assert-That ((Get-Hash $mount) -eq $mountHash) 'Uninstall removed calibration.'
Assert-Settings $savedSettings
Assert-That ((Get-Entries).Count -eq 1 -and (Get-Entries 'Other Addon').Count -eq 1) 'Uninstall did not preserve exactly the unrelated startup entry.'
foreach ($name in @('taxi-cam.exe','taxi-camera-bridge.dll','LICENSE.txt','THIRD_PARTY_NOTICES.txt')) {
    Assert-That (-not (Test-Path -LiteralPath (Join-Path $app $name))) "Uninstall retained $name."
}
Assert-That ((Get-Hash (Join-Path $app 'installation.json')) -eq $recordHash) 'Uninstall removed or changed the installation record.'

# Explicit reset uses the package defaults and removes only known current/legacy settings.
$exitCode = Invoke-Setup $fixture $app (Join-Path $testRoot 'reset-install.log') -Paths -ResetSettings
Assert-That ($exitCode -eq 0) 'Opt-in reset installation failed.'
Assert-Settings $savedSettings -Removed
Assert-That ((Get-Hash $mount) -eq (Get-Hash (Join-Path $payload 'taxi-camera-mounts.cfg'))) 'Reset did not install the packaged default mounts.'
$savedSettings = Seed-Settings 'after reset'
[IO.File]::WriteAllText($mount, '# newly calibrated camera mounts')
$exitCode = Invoke-Uninstall $app (Join-Path $testRoot 'remove-settings-uninstall.log') -RemoveSettings
Assert-That ($exitCode -eq 0) 'Opt-in settings removal uninstall failed.'
Assert-Settings $savedSettings -Removed
Assert-That (-not (Test-Path -LiteralPath $mount)) 'Opt-in uninstall retained the local mount calibration.'
Assert-That ((Get-Entries).Count -eq 1) 'Opt-in uninstall changed unrelated startup entries.'
# A later default-keep reinstall must not resurrect simulator calibration after
# the user explicitly removed the installation's saved settings.
[IO.File]::WriteAllText((Join-Path $sim 'taxi-camera-mounts.cfg'), '# stale simulator calibration')
$exitCode = Invoke-Setup $fixture $app (Join-Path $testRoot 'reinstall-after-remove.log')
Assert-That ($exitCode -eq 0) 'Default-keep reinstall after settings removal failed.'
Assert-That ((Get-Hash $mount) -eq (Get-Hash (Join-Path $payload 'taxi-camera-mounts.cfg'))) 'Reinstall resurrected old simulator calibration after explicit settings removal.'
$exitCode = Invoke-Uninstall $app (Join-Path $testRoot 'reinstall-after-remove-uninstall.log')
Assert-That ($exitCode -eq 0) 'Cleanup uninstall after default-keep reinstall failed.'
# A first installation imports the simulator folder's calibration.
$importApp = Join-Path $testRoot 'import-app'
$exitCode = Invoke-Setup $fixture $importApp (Join-Path $testRoot 'import-install.log') -Paths
Assert-That ($exitCode -eq 0 -and (Get-Hash (Join-Path $importApp 'taxi-camera-mounts.cfg')) -eq (Get-Hash (Join-Path $sim 'taxi-camera-mounts.cfg'))) 'First installation did not import simulator calibration.'
$exitCode = Invoke-Uninstall $importApp (Join-Path $testRoot 'import-uninstall.log')
Assert-That ($exitCode -eq 0) 'Import cleanup uninstall failed.'
Remove-Item -LiteralPath (Join-Path $sim 'taxi-camera-mounts.cfg')

# A locked exe.xml falls back to manual launch; the companion is still installed.
$xml | Set-Content -LiteralPath $xmlPath
$xmlHash = Get-Hash $xmlPath
$lockedApp = Join-Path $testRoot 'locked-startup-app'
$lockedLog = Join-Path $testRoot 'locked-startup.log'
$lock = [IO.File]::Open($xmlPath, 'Open', 'Read', 'Read')
try { $exitCode = Invoke-Setup $fixture $lockedApp $lockedLog -Paths } finally { $lock.Dispose() }
Assert-That ($exitCode -eq 0 -and (Test-Path -LiteralPath (Join-Path $lockedApp 'taxi-cam.exe'))) 'A locked exe.xml prevented installing the companion.'
$lockedRecord = Get-Record $lockedApp
Assert-That ($lockedRecord.startupRequested -eq 'automatic' -and $lockedRecord.startupStatus -eq 'manual' -and -not $lockedRecord.startupUpdated -and @($lockedRecord.startupPaths).Count -eq 0) 'Locked startup fallback record is incorrect.'
Assert-That (-not [string]::IsNullOrWhiteSpace($lockedRecord.startupWarning) -and (Get-Content -Raw -LiteralPath $lockedLog).Contains('Taxi Cam startup notice:')) 'Locked startup fallback did not warn.'
Assert-That ((Get-Hash $xmlPath) -eq $xmlHash -and -not @(Get-ChildItem -LiteralPath (Split-Path -Parent $xmlPath) -Filter 'exe.xml.taxi-*.tmp').Count) 'Locked startup fallback changed exe.xml or left temporary files.'
# Automatic intent remains selected, so the next normal upgrade retries.
$exitCode = Invoke-Setup $fixture $lockedApp (Join-Path $testRoot 'locked-retry.log')
$lockedRecord = Get-Record $lockedApp
Assert-That ($exitCode -eq 0 -and $lockedRecord.startupStatus -eq 'configured' -and (Get-Entries 'Taxi Cam').Count -eq 1) 'Retry did not configure automatic startup.'
$exitCode = Invoke-Setup $fixture $lockedApp (Join-Path $testRoot 'manual-existing-startup.log') -Startup manual
$lockedRecord = Get-Record $lockedApp
Assert-That ($exitCode -eq 0 -and $lockedRecord.startupRequested -eq 'manual' -and $lockedRecord.startupStatus -eq 'unchanged' -and @($lockedRecord.startupPaths).Count -eq 1) 'Manual preference lost ownership of a preserved startup entry.'
$exitCode = Invoke-Uninstall $lockedApp (Join-Path $testRoot 'locked-uninstall.log')
Assert-That ($exitCode -eq 0 -and (Get-Entries).Count -eq 1 -and (Get-Entries 'Other Addon').Count -eq 1) 'Uninstall did not remove the preserved owned startup entry.'

# Manual installation without an exe.xml selection.
$manualApp = Join-Path $testRoot 'manual-app'
$xmlHash = Get-Hash $xmlPath
$exitCode = Invoke-Setup $fixture $manualApp (Join-Path $testRoot 'manual-install.log') -Paths -WithoutXml -Startup manual
$manualRecord = Get-Record $manualApp
Assert-That ($exitCode -eq 0 -and $manualRecord.startupRequested -eq 'manual' -and $manualRecord.startupStatus -eq 'manual' -and @($manualRecord.startupPaths).Count -eq 0) 'Manual installation claimed a startup registration.'
$exitCode = Invoke-Setup $fixture $manualApp (Join-Path $testRoot 'manual-upgrade.log')
$manualRecord = Get-Record $manualApp
Assert-That ($exitCode -eq 0 -and $manualRecord.startupRequested -eq 'manual' -and -not $manualRecord.startupUpdated) 'Upgrade changed the remembered manual launch preference.'
$exitCode = Invoke-Uninstall $manualApp (Join-Path $testRoot 'manual-uninstall.log')
Assert-That ($exitCode -eq 0 -and -not (Test-Path -LiteralPath (Join-Path $manualApp 'taxi-cam.exe'))) 'Manual installation could not be uninstalled.'
Assert-That ((Get-Hash $xmlPath) -eq $xmlHash) 'Manual install, upgrade or uninstall changed unrelated startup XML.'

# A simulator-wide choice to disable launch entries survives installation.
$disabledApp = Join-Path $testRoot 'disabled-startup-app'
$xml.Replace('<Disabled>False</Disabled>','<Disabled>True</Disabled>') | Set-Content -LiteralPath $xmlPath
$disabledXmlHash = Get-Hash $xmlPath
$exitCode = Invoke-Setup $fixture $disabledApp (Join-Path $testRoot 'disabled-startup-install.log') -Paths
$disabledRecord = Get-Record $disabledApp
Assert-That ($exitCode -eq 0 -and (Get-Hash $xmlPath) -eq $disabledXmlHash) 'Globally disabled startup blocked installation or changed exe.xml.'
Assert-That ($disabledRecord.startupRequested -eq 'automatic' -and $disabledRecord.startupStatus -eq 'manual' -and -not $disabledRecord.startupUpdated -and $disabledRecord.startupWarning) 'Globally disabled startup did not record its manual fallback.'
$exitCode = Invoke-Uninstall $disabledApp (Join-Path $testRoot 'disabled-uninstall.log')
Assert-That ($exitCode -eq 0 -and (Get-Hash $xmlPath) -eq $disabledXmlHash) 'Uninstall changed the globally disabled launch document.'

[ordered]@{passed=$true;installerSha256=$receipt.installerSha256;tests=@('exact production rejection','running simulator refused before writes','missing SimConnect refused before application and startup writes','first install preserves calibration and settings','former name upgrade','default uninstall keeps settings and record','opt-in reset installs packaged defaults','opt-in uninstall removes known settings and mount','no calibration resurrection after removal','first install imports simulator calibration','locked startup falls back with notice and retries','manual preference keeps startup ownership','manual install without XML','globally disabled startup preserved');simulatorVerified=$false} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $testRoot 'result.json') -Encoding utf8
Write-Output "Installer checks passed: $testRoot"
} finally {
    $env:LOCALAPPDATA = $savedLocalAppData
    $env:APPDATA = $savedAppData
    Remove-Item Env:\TAXI_SETUP_TEST_PROCESSES -ErrorAction SilentlyContinue
}
