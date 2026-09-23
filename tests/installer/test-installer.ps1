[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Installer)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Set-StrictMode -Version Latest
$repo = $repoRoot
. (Join-Path $repoRoot 'tests/support/installer_fixture.ps1')
. (Join-Path $repoRoot 'installer/settings.ps1')
$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$receipt = Get-Content -Raw -LiteralPath ($installerPath + '.json') | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $installerPath).Hash -ne $receipt.installerSha256) { throw 'Installer hash does not match its build receipt.' }
if ([IO.Path]::GetFileName($installerPath) -cne "taxi-cam-$($receipt.version)-windows-x64-setup.exe") { throw 'Installer filename must contain the application version without a build-number suffix.' }
$testRoot = Join-Path $repo ('build/installer-tests/' + [Guid]::NewGuid().ToString('N'))
$sim = Join-Path $testRoot 'sim'; $xmlPath = Join-Path $testRoot 'config/exe.xml'
New-Item -ItemType Directory -Force -Path $sim,(Split-Path -Parent $xmlPath) | Out-Null
$savedLocalAppData = $env:LOCALAPPDATA
$fixtureLocalAppData = Join-Path $testRoot 'localappdata'
New-Item -ItemType Directory -Path $fixtureLocalAppData | Out-Null
$env:LOCALAPPDATA = $fixtureLocalAppData
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
function Assert-That([bool]$Condition,[string]$Message) { if (-not $Condition) { throw $Message } }
$knownSettings = @('Taxi Cam/settings.ini','Taxi Cam/hotkeys.ini','Taxi Cam/startup-state')
foreach ($key in @('fbw-a380x','ini-a350-900','ini-a350-1000','ini-a380','pmdg-777','pmdg-777-300er','pmdg-777f','aerosoft-a346','ini-a340-300')) {
    foreach ($folder in @('Taxi Cam','380 Taxi Cam')) { $knownSettings += "$folder/profiles/$key.ini" }
}
$rateSettings = @('Taxi Cam/settings.ini')
foreach ($key in @('fbw-a380x','ini-a350-900','ini-a350-1000','ini-a380','pmdg-777','pmdg-777-300er','pmdg-777f','aerosoft-a346','ini-a340-300')) {
    foreach ($folder in @('Taxi Cam','380 Taxi Cam')) { $rateSettings += "$folder/profiles/$key.ini" }
}
$unrelatedSettings = @('Taxi Cam/logs/history.log','Taxi Cam/profiles/custom-aircraft.ini','Taxi Cam/readme.txt','380 Taxi Cam/profiles/custom-aircraft.ini')
function Seed-Settings([string]$Tag) {
    $hashes = @{}
    foreach ($relative in @($knownSettings + $unrelatedSettings)) {
        $path = Join-Path $fixtureLocalAppData $relative
        New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
        [IO.File]::WriteAllText($path, "$Tag settings fixture: $relative")
        $hashes[$relative] = (Get-FileHash -LiteralPath $path).Hash
    }
    return $hashes
}
function Assert-Settings($Hashes,[switch]$Removed,[switch]$RateForced) {
    foreach ($relative in @($knownSettings + $unrelatedSettings)) {
        $path = Join-Path $fixtureLocalAppData $relative
        if ($Removed -and $relative -in $knownSettings) {
            if ($relative -eq 'Taxi Cam/settings.ini' -and (Test-Path -LiteralPath $path -PathType Leaf) -and
                (Get-TaxiIniKey $path 'display' 'camera_rate_revision') -eq '2' -and
                $null -eq (Get-TaxiIniKey $path 'display' 'camera_rate')) {
                continue
            }
            Assert-That (-not (Test-Path -LiteralPath $path)) "Opt-in settings removal retained $relative."
        } elseif ($RateForced -and $relative -in $rateSettings) {
            Assert-That ((Test-Path -LiteralPath $path -PathType Leaf) -and (Get-TaxiIniKey $path 'display' 'camera_rate') -eq '10') "Keep-install did not force camera_rate=10: $relative."
            Assert-That ((Get-TaxiIniKey $path 'display' 'exposure') -eq '-8') "Keep-install did not force exposure=-8: $relative."
            if ($relative -eq 'Taxi Cam/settings.ini') {
                Assert-That ((Get-TaxiIniKey $path 'display' 'camera_rate_revision') -eq '2') "Keep-install omitted camera_rate_revision on settings.ini."
                Assert-That ((Get-TaxiIniKey $path 'display' 'exposure_revision') -eq '1') "Keep-install omitted exposure_revision on settings.ini."
            }
            Assert-That ([IO.File]::ReadAllText($path).Contains("settings fixture: $relative")) "Keep-install rate force dropped prior settings text: $relative."
        } else {
            Assert-That ((Test-Path -LiteralPath $path -PathType Leaf) -and (Get-FileHash -LiteralPath $path).Hash -eq $Hashes[$relative]) "Saved or unrelated settings changed: $relative."
        }
    }
}
function Invoke-Uninstall([string]$App,[string]$Log,[switch]$RemoveSettings) {
    $arguments = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/LOG="' + $Log + '"'))
    if ($RemoveSettings) { $arguments += '/REMOVESETTINGS=1' }
    $process = Start-Process -FilePath (Join-Path $App 'unins000.exe') -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(60000)) { throw "Uninstaller test timed out; process $($process.Id), log $Log" }
    $process.Refresh()
    return $process.ExitCode
}
# Exercise the exact production executable through a guaranteed pre-write failure.
# Valid selections reach the native guard against installing into the simulator itself.
# Even if MSFS is running, Setup must return failure and leave all fixture bytes unchanged.
$xml | Set-Content -LiteralPath $xmlPath
$before = (Get-FileHash -LiteralPath $xmlPath).Hash
$negativeApp = $sim
$exitCode = Invoke-Setup $installerPath $negativeApp (Join-Path $testRoot 'production-rejection.log') -Paths
Assert-That ($exitCode -ne 0) 'The production installer accepted the simulator directory as its application directory.'
Assert-That ((Get-FileHash -LiteralPath $xmlPath).Hash -eq $before) 'The rejected production install modified exe.xml.'
Assert-That (-not (Test-Path -LiteralPath (Join-Path $negativeApp 'taxi-cam.exe'))) 'The rejected production install wrote a live executable.'

# A separately compiled fixture suppresses registration and shortcuts. Only its private copies
# of process discovery are mocked, allowing local simulator sessions to remain untouched.
$payload = Join-Path $testRoot 'payload'
Copy-Item -LiteralPath $receipt.compilerPayload -Destination $payload -Recurse
$runtime = Join-Path $testRoot 'runtime.ps1'
Copy-Item -LiteralPath (Join-Path $repoRoot 'installer/runtime.ps1') -Destination $runtime
$settingsHelper = Join-Path $testRoot 'settings.ps1'
Copy-Item -LiteralPath (Join-Path $repoRoot 'installer/settings.ps1') -Destination $settingsHelper
Copy-Item -LiteralPath $settingsHelper -Destination (Join-Path $payload 'settings.ps1') -Force
foreach ($path in @($runtime,$settingsHelper,(Join-Path $payload 'settings.ps1'),(Join-Path $payload 'install.ps1'),(Join-Path $payload 'uninstall.ps1'))) {
    $source = (Get-Content -Raw -LiteralPath $path).Replace('Get-Process','Get-FixtureProcess')
    $isolation = "`r`nfunction Get-FixtureProcess { return @() }`r`n`$env:LOCALAPPDATA = '" + $fixtureLocalAppData.Replace("'", "''") + "'"
    $source = $source.Replace('Set-StrictMode -Version Latest', ('Set-StrictMode -Version Latest' + $isolation))
    Set-Content -LiteralPath $path -Value $source -Encoding utf8
}
& (Join-Path $repoRoot 'installer/embed-uninstaller.ps1') -Output (Join-Path $testRoot 'uninstall-scripts.iss') -RuntimeScript $runtime -UninstallScript (Join-Path $payload 'uninstall.ps1') -ExeXmlScript (Join-Path $payload 'exe_xml.ps1') -SettingsScript $settingsHelper
$compiler = & (Join-Path $repo 'installer/bootstrap.ps1')
& $compiler "/DPayloadDir=$payload" "/DInternalDir=$testRoot" "/DAppIcon=$(Join-Path $repoRoot 'src/app/taxi-cam.ico')" "/DRuntimeScript=$runtime" "/DSettingsScript=$settingsHelper" "/DAppVersion=$($receipt.version)" "/DBuildNumber=$($receipt.buildNumber)" '/DInstallerTest=1' '/DOutputBase=isolated-setup' "/O$testRoot" (Join-Path $repoRoot 'installer/taxi-cam.iss') *> (Join-Path $testRoot 'compiler.log')
if ($LASTEXITCODE -ne 0) { throw "Fixture compilation failed: $testRoot" }
$fixture = Join-Path $testRoot 'isolated-setup.exe'; $app = Join-Path $testRoot 'app'
$xml | Set-Content -LiteralPath $xmlPath
# Keep dependency checking active in the compiled fixture: only process discovery is mocked.
$client = Join-Path $sim 'SimConnect_internal.dll'
Remove-Item -LiteralPath $client
$missingApp = Join-Path $testRoot 'missing-dependency'
$before = (Get-FileHash -LiteralPath $xmlPath).Hash
$dependencyLog = Join-Path $testRoot 'missing-dependency.log'
$exitCode = Invoke-Setup $fixture $missingApp $dependencyLog -Paths
Assert-That ($exitCode -ne 0) 'Installer accepted a missing SimConnect client.'
Assert-That ((Get-Content -Raw -LiteralPath $dependencyLog).Contains('SimConnect_internal.dll')) 'Installer did not explain the missing SimConnect dependency.'
Assert-That ((Get-FileHash -LiteralPath $xmlPath).Hash -eq $before) 'Dependency refusal changed startup configuration.'
Assert-That (-not (Test-Path -LiteralPath (Join-Path $missingApp 'taxi-cam.exe'))) 'Dependency refusal wrote application files.'
New-TaxiFixtureImage $client
$savedSettings = Seed-Settings 'initial'
New-Item -ItemType Directory -Path $app -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $app 'taxi-camera-mounts.cfg'), '# existing calibrated camera mounts')
$existingMountHash = (Get-FileHash -LiteralPath (Join-Path $app 'taxi-camera-mounts.cfg')).Hash
$exitCode = Invoke-Setup $fixture $app (Join-Path $testRoot 'install.log') -Paths
Assert-That ($exitCode -eq 0) "Isolated first install failed ($exitCode): $testRoot"
Assert-Settings $savedSettings -RateForced
Assert-That ((Get-FileHash -LiteralPath (Join-Path $app 'taxi-camera-mounts.cfg')).Hash -eq $existingMountHash) 'Default install replaced existing calibration.'
foreach ($name in @('taxi-cam.exe','taxi-camera-bridge.dll')) { Assert-That ((Get-FileHash -LiteralPath (Join-Path $app $name)).Hash -eq $receipt.files.PSObject.Properties[$name].Value) "Installed hash mismatch: $name" }
foreach ($name in @('LICENSE.txt','THIRD_PARTY_NOTICES.txt')) {
    Assert-That ((Get-FileHash -LiteralPath (Join-Path $app $name)).Hash -eq (Get-FileHash -LiteralPath (Join-Path $payload $name)).Hash) "Installer omitted or changed $name."
}
Assert-That (@(Get-ChildItem -LiteralPath $app -Filter '*.ps1' -Recurse).Count -eq 0) 'Installer left loose PowerShell files in the application.'
Assert-That (@(Get-ChildItem -LiteralPath $app -Directory).Count -eq 0) 'Installer left support or staging folders in the application.'
[xml]$launch = Get-Content -Raw -LiteralPath $xmlPath
Assert-That ($launch.SelectNodes('//Launch.Addon').Count -eq 2) 'Existing startup entry was not preserved.'
$mount = Join-Path $app 'taxi-camera-mounts.cfg'
Add-Content -LiteralPath $mount -Value '# user calibration fixture'
$mountHash = (Get-FileHash -LiteralPath $mount).Hash
# Model an existing installation under the former product name.
$oldExe = Join-Path $app '380-taxi-cam.exe'
Move-Item -LiteralPath (Join-Path $app 'taxi-cam.exe') -Destination $oldExe
$launch.SelectSingleNode('//Launch.Addon[Name="Taxi Cam"]/Name').InnerText = '380 Taxi Cam'
$launch.SelectSingleNode('//Launch.Addon[Name="380 Taxi Cam"]/Path').InnerText = $oldExe
$launch.Save($xmlPath)
$exitCode = Invoke-Setup $fixture $app (Join-Path $testRoot 'upgrade.log')
Assert-That ($exitCode -eq 0) "Isolated upgrade with remembered paths failed ($exitCode): $testRoot"
Assert-That ((Get-FileHash -LiteralPath $mount).Hash -eq $mountHash) 'Upgrade overwrote user calibration.'
Assert-Settings $savedSettings -RateForced
[xml]$launch = Get-Content -Raw -LiteralPath $xmlPath
Assert-That ($launch.SelectNodes('//Launch.Addon').Count -eq 2) 'Upgrade duplicated the startup entry.'
Assert-That ($launch.SelectNodes('//Launch.Addon[Name="Taxi Cam"]').Count -eq 1) 'Upgrade did not rename startup registration.'
Assert-That (-not (Test-Path -LiteralPath $oldExe)) 'Upgrade retained the obsolete companion executable.'
# Fail after the native transaction to verify Setup cancellation/error rollback.
$rollbackScript = Join-Path $testRoot 'rollback.iss'
$iss = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'installer/taxi-cam.iss')
$iss = $iss.Replace('if CurStep = ssDone then Completed := True;', "if CurStep = ssInstall then Abort;`r`n  if CurStep = ssDone then Completed := True;")
Set-Content -LiteralPath $rollbackScript -Value $iss -Encoding utf8
& $compiler "/DPayloadDir=$payload" "/DInternalDir=$testRoot" "/DAppIcon=$(Join-Path $repoRoot 'src/app/taxi-cam.ico')" "/DRuntimeScript=$runtime" "/DSettingsScript=$settingsHelper" "/DAppVersion=$($receipt.version)" "/DBuildNumber=$($receipt.buildNumber)" '/DInstallerTest=1' '/DOutputBase=rollback-setup' "/O$testRoot" $rollbackScript *> (Join-Path $testRoot 'rollback-compiler.log')
if ($LASTEXITCODE -ne 0) { throw "Rollback fixture compilation failed: $testRoot" }
Move-Item -LiteralPath (Join-Path $app 'taxi-cam.exe') -Destination $oldExe
$oldExeHash = (Get-FileHash -LiteralPath $oldExe).Hash
$launch.SelectSingleNode('//Launch.Addon[Name="Taxi Cam"]/Name').InnerText = '380 Taxi Cam'
$launch.SelectSingleNode('//Launch.Addon[Name="380 Taxi Cam"]/Path').InnerText = $oldExe
$launch.Save($xmlPath)
$xmlHash = (Get-FileHash -LiteralPath $xmlPath).Hash
$recordHash = (Get-FileHash -LiteralPath (Join-Path $app 'installation.json')).Hash
foreach ($name in @('LICENSE.txt','THIRD_PARTY_NOTICES.txt')) { [IO.File]::WriteAllText((Join-Path $app $name), "prior $name fixture") }
$exitCode = Invoke-Setup (Join-Path $testRoot 'rollback-setup.exe') $app (Join-Path $testRoot 'rollback.log') -ResetSettings
Assert-That ($exitCode -ne 0) 'Injected Setup abort did not fail.'
Assert-That ((Get-FileHash -LiteralPath $xmlPath).Hash -eq $xmlHash) 'Setup abort did not restore exe.xml.'
Assert-That ((Get-FileHash -LiteralPath (Join-Path $app 'installation.json')).Hash -eq $recordHash) 'Setup abort did not restore installation.json.'
Assert-That ((Get-FileHash -LiteralPath $oldExe).Hash -eq $oldExeHash) 'Setup abort did not restore the previous executable name and bytes.'
Assert-That (-not (Test-Path -LiteralPath (Join-Path $app 'taxi-cam.exe'))) 'Setup abort retained the renamed executable.'
Assert-Settings $savedSettings -RateForced
Assert-That ((Get-FileHash -LiteralPath $mount).Hash -eq $mountHash) 'Setup abort after reset did not restore calibrated mounts.'
foreach ($name in @('LICENSE.txt','THIRD_PARTY_NOTICES.txt')) {
    Assert-That ([IO.File]::ReadAllText((Join-Path $app $name)) -eq "prior $name fixture") "Setup abort did not restore $name."
}
$exitCode = Invoke-Setup $fixture $app (Join-Path $testRoot 'retry-upgrade.log')
Assert-That ($exitCode -eq 0 -and -not (Test-Path -LiteralPath $oldExe)) 'Retry after rename rollback did not complete the upgrade.'
foreach ($name in @('LICENSE.txt','THIRD_PARTY_NOTICES.txt')) {
    Assert-That ((Get-FileHash -LiteralPath (Join-Path $app $name)).Hash -eq (Get-FileHash -LiteralPath (Join-Path $payload $name)).Hash) "Retry did not restore the bundled $name."
}
$exitCode = Invoke-Uninstall $app (Join-Path $testRoot 'uninstall.log')
Assert-That ($exitCode -eq 0) 'Isolated uninstall failed.'
Assert-That ((Get-FileHash -LiteralPath $mount).Hash -eq $mountHash) 'Uninstall removed calibration.'
Assert-Settings $savedSettings -RateForced
[xml]$launch = Get-Content -Raw -LiteralPath $xmlPath
Assert-That ($launch.SelectNodes('//Launch.Addon').Count -eq 1) 'Uninstall did not preserve exactly the unrelated startup entry.'
Assert-That (-not (Test-Path -LiteralPath (Join-Path $app 'taxi-cam.exe'))) 'Uninstall retained the installed executable.'
foreach ($name in @('LICENSE.txt','THIRD_PARTY_NOTICES.txt')) {
    Assert-That (-not (Test-Path -LiteralPath (Join-Path $app $name))) "Uninstall retained $name."
}

# Explicit reset uses the package defaults and removes only known current/legacy settings.
$exitCode = Invoke-Setup $fixture $app (Join-Path $testRoot 'reset-install.log') -Paths -ResetSettings
Assert-That ($exitCode -eq 0) 'Opt-in reset installation failed.'
Assert-Settings $savedSettings -Removed
Assert-That ((Get-FileHash -LiteralPath $mount).Hash -eq (Get-FileHash -LiteralPath (Join-Path $payload 'taxi-camera-mounts.cfg')).Hash) 'Reset did not install the packaged default mounts.'
# A pilot can save new settings after reset; opt-in uninstall removes these too.
$savedSettings = Seed-Settings 'after reset'
[IO.File]::WriteAllText($mount, '# newly calibrated camera mounts')
$retainedRecordHash = (Get-FileHash -LiteralPath (Join-Path $app 'installation.json')).Hash
$exitCode = Invoke-Uninstall $app (Join-Path $testRoot 'remove-settings-uninstall.log') -RemoveSettings
Assert-That ($exitCode -eq 0) 'Opt-in settings removal uninstall failed.'
Assert-Settings $savedSettings -Removed
Assert-That (-not (Test-Path -LiteralPath $mount)) 'Opt-in uninstall retained the local mount calibration.'
Assert-That ((Get-FileHash -LiteralPath (Join-Path $app 'installation.json')).Hash -eq $retainedRecordHash) 'Opt-in uninstall removed or changed the installation receipt.'
[xml]$launch = Get-Content -Raw -LiteralPath $xmlPath
Assert-That ($launch.SelectNodes('//Launch.Addon').Count -eq 1) 'Opt-in uninstall changed unrelated startup entries.'
# A later default-keep reinstall must not resurrect legacy simulator calibration
# after the user explicitly removed the installation's saved settings.
[IO.File]::WriteAllText((Join-Path $sim 'taxi-camera-mounts.cfg'), '# stale simulator calibration')
$exitCode = Invoke-Setup $fixture $app (Join-Path $testRoot 'reinstall-after-remove.log')
Assert-That ($exitCode -eq 0) 'Default-keep reinstall after settings removal failed.'
Assert-Settings $savedSettings -Removed
Assert-That ((Get-FileHash -LiteralPath $mount).Hash -eq (Get-FileHash -LiteralPath (Join-Path $payload 'taxi-camera-mounts.cfg')).Hash) 'Reinstall resurrected old simulator calibration after explicit settings removal.'
$exitCode = Invoke-Uninstall $app (Join-Path $testRoot 'reinstall-after-remove-uninstall.log')
Assert-That ($exitCode -eq 0) 'Cleanup uninstall after default-keep reinstall failed.'
Remove-Item -LiteralPath (Join-Path $sim 'taxi-camera-mounts.cfg')

# Inject Windows error 6000 into only the fixture's sibling XML backup. The real
# filesystem encryption policy and any simulator configuration remain untouched.
$xmlHelper = Join-Path $payload 'exe_xml.ps1'
$originalHelper = Get-Content -Raw -LiteralPath $xmlHelper
$copyXmlBackup = 'Copy-Item -LiteralPath $absolute -Destination $backup'
Assert-That ($originalHelper.Contains($copyXmlBackup)) 'The encrypted-backup fault injection hook no longer matches the XML helper.'
$faultMarker = Join-Path (Split-Path -Parent $xmlPath) 'inject-encryption-failure'
$injectedCopy = @'
if (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $absolute) 'inject-encryption-failure')) {
            [IO.File]::WriteAllText($backup, '<SimBase.Document')
            if ([IO.File]::ReadAllText((Join-Path (Split-Path -Parent $absolute) 'inject-encryption-failure')).Contains('deny-cleanup')) {
                [IO.File]::SetAttributes($backup, [IO.FileAttributes]::ReadOnly)
            }
            throw [ComponentModel.Win32Exception]::new(6000, 'Injected encrypted XML backup failure (Windows error 6000).')
        }
        Copy-Item -LiteralPath $absolute -Destination $backup
'@
try {
    Set-Content -LiteralPath $xmlHelper -Value ($originalHelper.Replace($copyXmlBackup, $injectedCopy)) -Encoding utf8
    & (Join-Path $repoRoot 'installer/embed-uninstaller.ps1') -Output (Join-Path $testRoot 'uninstall-scripts.iss') -RuntimeScript $runtime -UninstallScript (Join-Path $payload 'uninstall.ps1') -ExeXmlScript $xmlHelper -SettingsScript $settingsHelper
    & $compiler "/DPayloadDir=$payload" "/DInternalDir=$testRoot" "/DAppIcon=$(Join-Path $repoRoot 'src/app/taxi-cam.ico')" "/DRuntimeScript=$runtime" "/DSettingsScript=$settingsHelper" "/DAppVersion=$($receipt.version)" "/DBuildNumber=$($receipt.buildNumber)" '/DInstallerTest=1' '/DOutputBase=encryption-setup' "/O$testRoot" (Join-Path $repoRoot 'installer/taxi-cam.iss') *> (Join-Path $testRoot 'encryption-compiler.log')
    if ($LASTEXITCODE -ne 0) { throw "Encryption fixture compilation failed: $testRoot" }
    & $compiler "/DPayloadDir=$payload" "/DInternalDir=$testRoot" "/DAppIcon=$(Join-Path $repoRoot 'src/app/taxi-cam.ico')" "/DRuntimeScript=$runtime" "/DSettingsScript=$settingsHelper" "/DAppVersion=$($receipt.version)" "/DBuildNumber=$($receipt.buildNumber)" '/DInstallerTest=1' '/DOutputBase=encryption-rollback-setup' "/O$testRoot" $rollbackScript *> (Join-Path $testRoot 'encryption-rollback-compiler.log')
    if ($LASTEXITCODE -ne 0) { throw "Encryption rollback fixture compilation failed: $testRoot" }
} finally { Set-Content -LiteralPath $xmlHelper -Value $originalHelper -Encoding utf8 }
$encryptionFixture = Join-Path $testRoot 'encryption-setup.exe'
$encryptionApp = Join-Path $testRoot 'encryption-app'
$xml | Set-Content -LiteralPath $xmlPath
$xmlHash = (Get-FileHash -LiteralPath $xmlPath).Hash
$backupsBeforeFailure = @(Get-ChildItem -LiteralPath (Split-Path -Parent $xmlPath) -Filter 'exe.xml.taxi-backup-*' | ForEach-Object FullName)
'Enable private fixture fault' | Set-Content -LiteralPath $faultMarker
try {
    $encryptionLog = Join-Path $testRoot 'encryption-install.log'
    $exitCode = Invoke-Setup $encryptionFixture $encryptionApp $encryptionLog -Paths
    Assert-That ($exitCode -eq 0) "An optional encrypted XML backup failure prevented installation ($exitCode): $testRoot"
    foreach ($name in @('taxi-cam.exe','taxi-camera-bridge.dll')) {
        Assert-That ((Get-FileHash -LiteralPath (Join-Path $encryptionApp $name)).Hash -eq $receipt.files.PSObject.Properties[$name].Value) "Encrypted XML fallback did not install the verified $name."
    }
    Assert-That ((Get-FileHash -LiteralPath $xmlPath).Hash -eq $xmlHash) 'Encrypted XML fallback changed the existing startup configuration.'
    $backupsAfterFailure = @(Get-ChildItem -LiteralPath (Split-Path -Parent $xmlPath) -Filter 'exe.xml.taxi-backup-*' | ForEach-Object FullName)
    Assert-That ($backupsAfterFailure.Count -eq $backupsBeforeFailure.Count -and @($backupsAfterFailure | Where-Object { $_ -notin $backupsBeforeFailure }).Count -eq 0) 'Manual-startup fallback retained a partial backup or removed a previous verified backup.'
    $encryptionRecord = Get-Content -Raw -LiteralPath (Join-Path $encryptionApp 'installation.json') | ConvertFrom-Json
    Assert-That ($encryptionRecord.startupRequested -eq 'automatic' -and $encryptionRecord.startupStatus -eq 'manual' -and -not $encryptionRecord.startupUpdated) 'Encrypted XML fallback did not record automatic intent and manual-launch status.'
    Assert-That (@($encryptionRecord.startupPaths).Count -eq 0) 'Encrypted XML fallback claimed ownership of unchanged startup configuration.'
    Assert-That (-not [string]::IsNullOrWhiteSpace($encryptionRecord.startupWarning)) 'Encrypted XML fallback omitted its persisted startup warning.'
    $setupLog = Get-Content -Raw -LiteralPath $encryptionLog
    Assert-That ($setupLog.Contains('Taxi Cam startup notice:')) 'Silent setup did not log its startup warning.'
    $diagnostics = Get-Content -Raw -LiteralPath (Join-Path $encryptionApp 'setup-diagnostics.log')
    Assert-That ($diagnostics.Contains($xmlPath) -and $diagnostics.Contains('6000')) 'Encrypted XML diagnostics omitted the failing path or Windows error.'

    # Failure to remove an unverified copy must stop installation, even when the
    # original XML is intact. Keep this lock simulation inside the fixture only.
    'deny-cleanup' | Set-Content -LiteralPath $faultMarker
    try {
        $cleanupApp = Join-Path $testRoot 'backup-cleanup-failure-app'
        $cleanupLog = Join-Path $testRoot 'backup-cleanup-failure.log'
        $exitCode = Invoke-Setup $encryptionFixture $cleanupApp $cleanupLog -Paths
        Assert-That ($exitCode -ne 0) 'Unverified backup cleanup failure incorrectly completed installation.'
        Assert-That (-not (Test-Path -LiteralPath (Join-Path $cleanupApp 'taxi-cam.exe'))) 'Unverified backup cleanup failure did not roll back the application.'
        Assert-That ((Get-FileHash -LiteralPath $xmlPath).Hash -eq $xmlHash) 'Unverified backup cleanup failure changed the original XML.'
        $unverified = @(Get-ChildItem -LiteralPath (Split-Path -Parent $xmlPath) -Filter 'exe.xml.taxi-backup-*' | Where-Object { $_.FullName -notin $backupsBeforeFailure })
        Assert-That ($unverified.Count -eq 1 -and (Get-Content -Raw -LiteralPath $cleanupLog).Contains($unverified[0].FullName)) 'Cleanup failure omitted the unverified backup path from the setup log.'
    } finally {
        foreach ($item in @(Get-ChildItem -LiteralPath (Split-Path -Parent $xmlPath) -Filter 'exe.xml.taxi-backup-*' | Where-Object { $_.FullName -notin $backupsBeforeFailure })) {
            [IO.File]::SetAttributes($item.FullName, [IO.FileAttributes]::Normal)
            [IO.File]::Delete($item.FullName)
        }
        'Enable private fixture fault' | Set-Content -LiteralPath $faultMarker
    }

    $cancelledApp = Join-Path $testRoot 'encryption-cancelled-app'
    $exitCode = Invoke-Setup (Join-Path $testRoot 'encryption-rollback-setup.exe') $cancelledApp (Join-Path $testRoot 'encryption-cancelled.log') -Paths
    Assert-That ($exitCode -ne 0) 'The injected outer abort after optional startup failure did not fail.'
    Assert-That ((Get-FileHash -LiteralPath $xmlPath).Hash -eq $xmlHash) 'Outer rollback erased the unchanged XML after optional startup failure.'
    Assert-That (-not (Test-Path -LiteralPath (Join-Path $cancelledApp 'taxi-cam.exe'))) 'Outer rollback retained the executable after optional startup failure.'
    Assert-That (-not (Test-Path -LiteralPath (Join-Path $cancelledApp 'installation.json'))) 'Outer rollback retained the installation record after optional startup failure.'

    # A requested settings reset is independent of optional startup failure.
    # Cancelling the outer setup must restore both the settings and calibration.
    $fallbackSettings = Seed-Settings 'before startup fallback reset'
    $fallbackMount = Join-Path $encryptionApp 'taxi-camera-mounts.cfg'
    [IO.File]::WriteAllText($fallbackMount, '# calibrated before startup fallback reset')
    $fallbackMountHash = (Get-FileHash -LiteralPath $fallbackMount).Hash
    $fallbackRecordHash = (Get-FileHash -LiteralPath (Join-Path $encryptionApp 'installation.json')).Hash
    $exitCode = Invoke-Setup (Join-Path $testRoot 'encryption-rollback-setup.exe') $encryptionApp (Join-Path $testRoot 'encryption-reset-cancelled.log') -ResetSettings
    Assert-That ($exitCode -ne 0) 'Injected cancellation after startup fallback and settings reset did not fail.'
    Assert-Settings $fallbackSettings
    Assert-That ((Get-FileHash -LiteralPath $fallbackMount).Hash -eq $fallbackMountHash) 'Rollback after startup fallback and reset lost the original calibration.'
    Assert-That ((Get-FileHash -LiteralPath (Join-Path $encryptionApp 'installation.json')).Hash -eq $fallbackRecordHash) 'Rollback after startup fallback and reset changed the previous receipt.'
    Assert-That ((Get-FileHash -LiteralPath $xmlPath).Hash -eq $xmlHash) 'Rollback after startup fallback and reset changed startup XML.'

    $exitCode = Invoke-Setup $encryptionFixture $encryptionApp (Join-Path $testRoot 'encryption-reset.log') -ResetSettings
    Assert-That ($exitCode -eq 0) 'Optional startup failure prevented the requested settings reset.'
    Assert-Settings $fallbackSettings -Removed
    Assert-That ((Get-FileHash -LiteralPath $fallbackMount).Hash -eq (Get-FileHash -LiteralPath (Join-Path $payload 'taxi-camera-mounts.cfg')).Hash) 'Startup fallback did not retain the requested default calibration reset.'
    $resetFallbackRecord = Get-Content -Raw -LiteralPath (Join-Path $encryptionApp 'installation.json') | ConvertFrom-Json
    Assert-That ($resetFallbackRecord.startupStatus -eq 'manual' -and -not $resetFallbackRecord.startupUpdated) 'Reset with startup failure did not retain the manual fallback status.'
    Assert-That ((Get-FileHash -LiteralPath $xmlPath).Hash -eq $xmlHash) 'Reset with startup fallback changed the existing XML.'
} finally { Remove-Item -LiteralPath $faultMarker -ErrorAction SilentlyContinue }

# Automatic intent remains selected after fallback so the next normal upgrade
# retries setup without requiring the user to rediscover their original paths.
$exitCode = Invoke-Setup $fixture $encryptionApp (Join-Path $testRoot 'encryption-retry.log')
Assert-That ($exitCode -eq 0) 'The normal upgrade could not retry automatic startup after encrypted XML fallback.'
$encryptionRecord = Get-Content -Raw -LiteralPath (Join-Path $encryptionApp 'installation.json') | ConvertFrom-Json
Assert-That ($encryptionRecord.startupStatus -eq 'configured' -and $encryptionRecord.startupUpdated) 'Retry did not configure automatic startup.'
[xml]$launch = Get-Content -Raw -LiteralPath $xmlPath
Assert-That ($launch.SelectNodes('//Launch.Addon').Count -eq 2 -and $launch.SelectNodes('//Launch.Addon[Name="Taxi Cam"]').Count -eq 1) 'Automatic retry duplicated or omitted a startup entry.'
$xmlHash = (Get-FileHash -LiteralPath $xmlPath).Hash
'Enable private fixture fault' | Set-Content -LiteralPath $faultMarker
try {
    $exitCode = Invoke-Setup $encryptionFixture $encryptionApp (Join-Path $testRoot 'encryption-upgrade.log')
    Assert-That ($exitCode -eq 0) 'An optional encrypted XML backup failure blocked an existing automatic-startup installation.'
    $encryptionRecord = Get-Content -Raw -LiteralPath (Join-Path $encryptionApp 'installation.json') | ConvertFrom-Json
    Assert-That ($encryptionRecord.startupStatus -eq 'unchanged' -and -not $encryptionRecord.startupUpdated -and @($encryptionRecord.startupPaths).Count -eq 1) 'Upgrade fallback lost ownership of the previous startup entry.'
    Assert-That ((Get-FileHash -LiteralPath $xmlPath).Hash -eq $xmlHash) 'Upgrade fallback changed the previous startup entry.'
} finally { Remove-Item -LiteralPath $faultMarker -ErrorAction SilentlyContinue }
$exitCode = Invoke-Setup $fixture $encryptionApp (Join-Path $testRoot 'manual-existing-startup.log') -Startup manual
Assert-That ($exitCode -eq 0) 'Manual launch preference could not preserve an existing startup installation.'
$encryptionRecord = Get-Content -Raw -LiteralPath (Join-Path $encryptionApp 'installation.json') | ConvertFrom-Json
Assert-That ($encryptionRecord.startupRequested -eq 'manual' -and $encryptionRecord.startupStatus -eq 'unchanged' -and @($encryptionRecord.startupPaths).Count -eq 1) 'Manual preference lost ownership of a preserved startup entry.'
Assert-That ((Get-FileHash -LiteralPath $xmlPath).Hash -eq $xmlHash) 'Manual launch preference changed existing startup entries.'
$process = Start-Process -FilePath (Join-Path $encryptionApp 'unins000.exe') -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART') -WindowStyle Hidden -Wait -PassThru
Assert-That ($process.ExitCode -eq 0) 'Uninstall after startup fallback and manual preference failed.'
[xml]$launch = Get-Content -Raw -LiteralPath $xmlPath
Assert-That ($launch.SelectNodes('//Launch.Addon').Count -eq 1 -and $launch.SelectNodes('//Launch.Addon[Name="Other Addon"]').Count -eq 1) 'Uninstall did not remove the preserved owned startup entry while retaining the unrelated entry.'

$manualApp = Join-Path $testRoot 'manual-app'
$xmlHash = (Get-FileHash -LiteralPath $xmlPath).Hash
$exitCode = Invoke-Setup $fixture $manualApp (Join-Path $testRoot 'manual-install.log') -Paths -WithoutXml -Startup manual
Assert-That ($exitCode -eq 0) 'Manual installation without an XML selection failed.'
$manualRecord = Get-Content -Raw -LiteralPath (Join-Path $manualApp 'installation.json') | ConvertFrom-Json
Assert-That ($manualRecord.startupRequested -eq 'manual' -and $manualRecord.startupStatus -eq 'manual' -and @($manualRecord.startupPaths).Count -eq 0) 'Manual installation claimed a startup registration.'
$exitCode = Invoke-Setup $fixture $manualApp (Join-Path $testRoot 'manual-upgrade.log')
Assert-That ($exitCode -eq 0) 'Upgrade did not remember manual launch without an XML selection.'
$manualRecord = Get-Content -Raw -LiteralPath (Join-Path $manualApp 'installation.json') | ConvertFrom-Json
Assert-That ($manualRecord.startupRequested -eq 'manual' -and -not $manualRecord.startupUpdated) 'Upgrade changed the remembered manual launch preference.'
$process = Start-Process -FilePath (Join-Path $manualApp 'unins000.exe') -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART') -WindowStyle Hidden -Wait -PassThru
Assert-That ($process.ExitCode -eq 0) 'Manual installation could not be uninstalled without XML ownership.'
Assert-That (-not (Test-Path -LiteralPath (Join-Path $manualApp 'taxi-cam.exe'))) 'Manual uninstall retained the executable.'
Assert-That ((Get-FileHash -LiteralPath $xmlPath).Hash -eq $xmlHash) 'Manual install, upgrade or uninstall changed unrelated startup XML.'

# A simulator-wide choice to disable launch entries must survive installation.
# It prevents automatic startup, but does not prevent installing the companion.
$disabledApp = Join-Path $testRoot 'disabled-startup-app'
$xml.Replace('<Disabled>False</Disabled>','<Disabled>True</Disabled>') | Set-Content -LiteralPath $xmlPath
$disabledXmlHash = (Get-FileHash -LiteralPath $xmlPath).Hash
$exitCode = Invoke-Setup $fixture $disabledApp (Join-Path $testRoot 'disabled-startup-install.log') -Paths
Assert-That ($exitCode -eq 0) 'A globally disabled launch document prevented installing the companion.'
Assert-That ((Get-FileHash -LiteralPath (Join-Path $disabledApp 'taxi-cam.exe')).Hash -eq $receipt.files.PSObject.Properties['taxi-cam.exe'].Value) 'Disabled startup fallback did not install the verified executable.'
Assert-That ((Get-FileHash -LiteralPath $xmlPath).Hash -eq $disabledXmlHash) 'Installation changed the globally disabled launch document.'
$disabledRecord = Get-Content -Raw -LiteralPath (Join-Path $disabledApp 'installation.json') | ConvertFrom-Json
Assert-That ($disabledRecord.startupRequested -eq 'automatic' -and $disabledRecord.startupStatus -eq 'manual' -and -not $disabledRecord.startupUpdated) 'Globally disabled startup did not record automatic intent and manual fallback.'
Assert-That (-not [string]::IsNullOrWhiteSpace($disabledRecord.startupWarning)) 'Globally disabled startup omitted the manual launch notice.'
$process = Start-Process -FilePath (Join-Path $disabledApp 'unins000.exe') -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART') -WindowStyle Hidden -Wait -PassThru
Assert-That ($process.ExitCode -eq 0) 'Uninstall after globally disabled startup fallback failed.'
Assert-That ((Get-FileHash -LiteralPath $xmlPath).Hash -eq $disabledXmlHash) 'Uninstall changed the globally disabled launch document.'

function Invoke-FixtureRuntime([string]$Mode,[string]$App,[string]$State) {
    $arguments = @('-NoLogo','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',('"' + $runtime + '"'),'-Mode',$Mode,
        '-Destination',('"' + $App + '"'),'-StateDirectory',('"' + $State + '"'),
        '-SimulatorDirectory',('"' + $sim + '"'),'-ExeXml',('"' + $xmlPath + '"'),'-PayloadDirectory',('"' + $payload + '"'))
    $process = Start-Process -FilePath (Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe') -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    return $process.ExitCode
}
# Losing the first journal write after native installation must still roll back
# the exact writes reported by native, using the transaction held in memory.
$originalRuntime = Get-Content -Raw -LiteralPath $runtime
$persistJournal = '$state | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $statePath -Encoding utf8'
Assert-That ($originalRuntime.Contains($persistJournal)) 'The post-native journal fault injection hook no longer matches the runtime.'
$injectedPersist = @'
$completedNative = Get-Variable -Name nativeSucceeded -ErrorAction SilentlyContinue
        if ($null -ne $completedNative -and $completedNative.Value) { throw 'Injected first post-native journal persistence failure.' }
        $state | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $statePath -Encoding utf8
'@
try {
    Set-Content -LiteralPath $runtime -Value ($originalRuntime.Replace($persistJournal, $injectedPersist)) -Encoding utf8
    $xml | Set-Content -LiteralPath $xmlPath
    $journalXmlHash = (Get-FileHash -LiteralPath $xmlPath).Hash
    $journalApp = Join-Path $testRoot 'journal-failure-app'
    $journalState = Join-Path $testRoot 'journal-failure-state'
    $exitCode = Invoke-FixtureRuntime 'Install' $journalApp $journalState
    Assert-That ($exitCode -ne 0) 'The injected post-native journal persistence failure did not fail installation.'
    $journalError = Get-Content -Raw -LiteralPath (Join-Path $journalState 'error.txt')
    Assert-That ($journalError.Contains('Injected first post-native journal persistence failure.')) 'The journal failure fixture failed before reaching the injected persistence error.'
    foreach ($name in @('taxi-cam.exe','taxi-camera-bridge.dll','installation.json','taxi-camera-mounts.cfg')) {
        Assert-That (-not (Test-Path -LiteralPath (Join-Path $journalApp $name))) "In-memory rollback after journal failure retained $name."
    }
    Assert-That ((Get-FileHash -LiteralPath $xmlPath).Hash -eq $journalXmlHash) 'In-memory rollback after journal failure did not restore the original XML.'
    Assert-That (-not (Test-Path -LiteralPath (Join-Path $journalState 'transaction.json'))) 'Successful in-memory rollback retained its stale transaction journal.'
} finally { Set-Content -LiteralPath $runtime -Value $originalRuntime -Encoding utf8 }

# Inject an unrelated XML edit after the inner installer has recorded the input hash.
# Its optimistic concurrency check must fail without either rollback layer erasing that edit.
$xmlHelper = Join-Path $payload 'exe_xml.ps1'
$originalHelper = Get-Content -Raw -LiteralPath $xmlHelper
try {
    $injectedHelper = $originalHelper.Replace('$absolute = [IO.Path]::GetFullPath($Path)', ('$absolute = [IO.Path]::GetFullPath($Path)' + "`r`n    Add-Content -LiteralPath `$Path -Value '<!-- concurrent inner edit -->'"))
    Set-Content -LiteralPath $xmlHelper -Value $injectedHelper -Encoding utf8
    $xml | Set-Content -LiteralPath $xmlPath
    $innerApp = Join-Path $testRoot 'concurrent-inner-app'
    $innerState = Join-Path $testRoot 'concurrent-inner-state'
    $exitCode = Invoke-FixtureRuntime 'Install' $innerApp $innerState
    Assert-That ($exitCode -ne 0) 'The inner XML concurrency guard did not fail.'
    Assert-That ((Get-Content -Raw -LiteralPath $xmlPath).Contains('<!-- concurrent inner edit -->')) 'Outer rollback erased an XML edit rejected by the inner guard.'
    Assert-That (-not (Test-Path -LiteralPath (Join-Path $innerApp 'taxi-cam.exe'))) 'Inner failure did not roll back the executable.'
    foreach ($name in @('LICENSE.txt','THIRD_PARTY_NOTICES.txt')) {
        Assert-That (-not (Test-Path -LiteralPath (Join-Path $innerApp $name))) "Inner failure retained $name."
    }
} finally { Set-Content -LiteralPath $xmlHelper -Value $originalHelper -Encoding utf8 }

$xml | Set-Content -LiteralPath $xmlPath
$laterOriginalHash = (Get-FileHash -LiteralPath $xmlPath).Hash
$laterApp = Join-Path $testRoot 'concurrent-later-app'; $laterState = Join-Path $testRoot 'concurrent-later-state'
$exitCode = Invoke-FixtureRuntime 'Install' $laterApp $laterState
Assert-That ($exitCode -eq 0) 'Concurrency fixture could not complete its initial transaction.'
Add-Content -LiteralPath $xmlPath -Value '<!-- concurrent later edit -->'
$laterHash = (Get-FileHash -LiteralPath $xmlPath).Hash
$exitCode = Invoke-FixtureRuntime 'Rollback' $laterApp $laterState
Assert-That ($exitCode -ne 0) 'Rollback did not report the concurrent edit conflict.'
Assert-That ((Get-FileHash -LiteralPath $xmlPath).Hash -eq $laterHash) 'Rollback erased an XML edit made after its transaction.'
$conflict = Get-Content -Raw -LiteralPath (Join-Path $laterState 'error.txt')
Assert-That ($conflict -match '(?m)Recovery snapshot: ([^\r\n]+)') 'Rollback did not retain a recovery snapshot.'
$recoveryJournal = Join-Path $Matches[1] 'transaction.json'
Assert-That (Test-Path -LiteralPath $recoveryJournal) 'The reported rollback snapshot does not exist.'
$recoveryState = Get-Content -Raw -LiteralPath $recoveryJournal | ConvertFrom-Json
$recoveryXml = @($recoveryState.files | Where-Object { $_.path -eq $xmlPath })
Assert-That ($recoveryXml.Count -eq 1) 'The recovery snapshot lost the XML rollback entry.'
Assert-That ((Split-Path -Parent $recoveryXml[0].backup) -eq (Split-Path -Parent $xmlPath)) 'The recovery snapshot relocated the XML sibling backup into temporary storage.'
Assert-That (Test-Path -LiteralPath $recoveryXml[0].backup -PathType Leaf) 'The recovery snapshot points to a missing XML sibling backup.'
Assert-That ((Get-FileHash -LiteralPath $recoveryXml[0].backup).Hash -eq $laterOriginalHash) 'The retained XML sibling backup no longer contains the original configuration.'

# Inno uses short temporary paths on hosted Windows. Exercise that path form
# explicitly where the filesystem supports 8.3 names, without changing its policy.
if (-not ('TaxiInstallerShortPath' -as [type])) {
    Add-Type -TypeDefinition 'using System; using System.Text; using System.Runtime.InteropServices; public class TaxiInstallerShortPath { [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] public static extern uint GetShortPathName(string path, StringBuilder output, int capacity); }'
}
$shortBuffer = New-Object Text.StringBuilder 32768
$shortLength = [TaxiInstallerShortPath]::GetShortPathName($payload, $shortBuffer, $shortBuffer.Capacity)
$shortPath = $shortBuffer.ToString()
$shortPathResult = 'not-run: 8.3 alias unavailable'
if ($shortLength -gt 0 -and $shortLength -lt $shortBuffer.Capacity -and $shortPath -ine $payload) {
    $longPayload = $payload
    try {
        $payload = $shortPath
        $xml | Set-Content -LiteralPath $xmlPath
        $shortApp = Join-Path $testRoot 'short-path-app'; $shortState = Join-Path $testRoot 'short-path-state'
        $exitCode = Invoke-FixtureRuntime 'Install' $shortApp $shortState
        Assert-That ($exitCode -eq 0) 'Short-path payload installation failed.'
        foreach ($name in @('taxi-cam.exe','taxi-camera-bridge.dll','taxi-camera-mounts.cfg','LICENSE.txt','THIRD_PARTY_NOTICES.txt')) {
            Assert-That ((Get-FileHash -LiteralPath (Join-Path $shortApp $name)).Hash -eq (Get-FileHash -LiteralPath (Join-Path $payload $name)).Hash) "Short-path install omitted or changed $name."
        }
        $exitCode = Invoke-FixtureRuntime 'Rollback' $shortApp $shortState
        Assert-That ($exitCode -eq 0) 'Short-path rollback failed.'
        Assert-That (@(Get-ChildItem -LiteralPath $shortApp -File -Recurse).Count -eq 0) 'Short-path rollback retained installed files.'
        $shortPathResult = 'passed'
    } finally { $payload = $longPayload }
}
Write-Output "Short-path install and rollback: $shortPathResult"

[ordered]@{passed=$true;installerSha256=$receipt.installerSha256;shortPathValidation=$shortPathResult;tests=@('exact production rejection','missing SimConnect refused before application and startup writes','isolated first install preserves known current and legacy settings','remembered upgrade paths and former name migration','calibration preservation','post-transaction Setup reset rollback restores settings, mount, former executable and startup','retry after rename rollback','default uninstall keeps settings','opt-in reset installs packaged defaults and preserves unknown files/logs','opt-in uninstall removes known settings and mount but preserves unknown files/logs','retained installation receipt prevents legacy calibration resurrection on default-keep reinstall','partial encrypted XML backup is removed before manual fallback with diagnostics','unverified backup cleanup failure blocks fallback and rolls back application','outer rollback after encrypted XML fallback preserves startup','startup fallback permits requested settings reset and cancellation restores settings and calibration','automatic retry after encrypted XML fallback','upgrade fallback preserves existing startup ownership','manual preference preserves prior automatic startup','manual install upgrade and uninstall without XML selection','globally disabled startup falls back without XML changes','post-native journal failure rolls back from in-memory transaction','inner concurrent XML edit preserved','post-transaction concurrent XML edit preserved with recovery snapshot and sibling XML backup');simulatorVerified=$false} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $testRoot 'result.json') -Encoding utf8
Write-Output "Installer checks passed: $testRoot"
} finally {
    $env:LOCALAPPDATA = $savedLocalAppData
}
