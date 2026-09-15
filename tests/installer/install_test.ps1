$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$root = $repoRoot
. (Join-Path $repoRoot 'tests/support/installer_fixture.ps1')
$fixture = Join-Path $root ('build/native/install-validation-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
$sim = Join-Path $fixture 'sim'
$app = Join-Path $fixture 'app'
New-Item -ItemType Directory -Path $sim -Force | Out-Null
New-TaxiFixtureImage (Join-Path $sim 'FlightSimulator2024.exe') $false
New-TaxiFixtureImage (Join-Path $sim 'SimConnect_internal.dll')
[IO.File]::WriteAllText((Join-Path $sim 'taxi-camera-native.addon64'),'legacy fixture')
[IO.File]::WriteAllText((Join-Path $sim 'dxgi.dll'),'unrelated graphics fixture')
Copy-Item -LiteralPath (Join-Path $root 'taxi-camera-mounts.cfg') -Destination (Join-Path $sim 'taxi-camera-mounts.cfg')
$mountHash = (Get-FileHash -LiteralPath (Join-Path $sim 'taxi-camera-mounts.cfg')).Hash
$xml = Join-Path $fixture 'exe.xml'
[IO.File]::WriteAllText($xml,'<SimBase.Document Type="Launch"><Launch.Addon><Name>Keep Me</Name><Path>C:\Other.exe</Path></Launch.Addon></SimBase.Document>')
# Override process enumeration only inside this test script. All writes target the
# fresh fixture above; the user's simulator and companion remain untouched.
$fixtureSimulatorRunning = $true
$fixtureOldCompanionRunning = $false
function Get-Process {
    param([string[]]$Name, $ErrorAction)
    if ($fixtureSimulatorRunning -and 'FlightSimulator2024' -in $Name) {
        [pscustomobject]@{ProcessName='FlightSimulator2024'; Path=(Join-Path $sim 'FlightSimulator2024.exe')}
    }
    if ($fixtureOldCompanionRunning -and '380-taxi-cam' -in $Name) {
        [pscustomobject]@{ProcessName='380-taxi-cam'; Path=(Join-Path $app '380-taxi-cam.exe')}
    }
}
$refused = $false
try {
    & (Join-Path $root 'installer/install.ps1') -SimulatorDirectory $sim -ExeXml $xml -Destination $app -NoShortcut
} catch { $refused = $_.Exception.Message -like 'Close MSFS*' }
if (-not $refused -or (Test-Path -LiteralPath (Join-Path $app 'taxi-cam.exe'))) { throw 'Running simulator installation guard failed.' }
$fixtureSimulatorRunning = $false
$fixtureOldCompanionRunning = $true
$refused = $false
try {
    & (Join-Path $root 'installer/install.ps1') -SimulatorDirectory $sim -ExeXml $xml -Destination $app -NoShortcut
} catch { $refused = $_.Exception.Message -like 'Exit the taxi camera app*' }
if (-not $refused) { throw 'Running former-name companion installation guard failed.' }
$fixtureOldCompanionRunning = $false
& (Join-Path $root 'installer/install.ps1') -SimulatorDirectory $sim -ExeXml $xml -Destination $app -NoShortcut
$record = Get-Content -Raw -LiteralPath (Join-Path $app 'installation.json') | ConvertFrom-Json
[xml]$doc = Get-Content -Raw -LiteralPath $xml
if ($doc.SelectNodes('//Launch.Addon').Count -ne 2 -or -not $doc.SelectSingleNode('//Launch.Addon[Name="Keep Me"]')) { throw 'Installer changed unrelated startup.' }
if ((Get-FileHash -LiteralPath (Join-Path $app 'taxi-camera-mounts.cfg')).Hash -ne $mountHash) { throw 'Calibration import changed.' }
if (-not (Test-Path -LiteralPath $record.legacyBackup) -or (Test-Path -LiteralPath (Join-Path $sim 'taxi-camera-native.addon64'))) { throw 'Legacy add-on not retained and disabled.' }
if ([IO.File]::ReadAllText((Join-Path $sim 'dxgi.dll')) -ne 'unrelated graphics fixture') { throw 'Unrelated graphics files changed.' }
$firstLegacyBackup = $record.legacyBackup
& (Join-Path $root 'installer/install.ps1') -SimulatorDirectory $sim -ExeXml $xml -Destination $app -NoShortcut
$updated = Get-Content -Raw -LiteralPath (Join-Path $app 'installation.json') | ConvertFrom-Json
if ($updated.legacyBackup -ne $firstLegacyBackup) { throw 'Update lost the original legacy rollback file.' }
[xml]$doc = Get-Content -Raw -LiteralPath $xml
if ($doc.SelectNodes('//Launch.Addon[Name="Taxi Cam"]').Count -ne 1) { throw 'Update duplicated startup.' }
& (Join-Path $root 'installer/uninstall.ps1') -Installation $app -RestoreLegacy
[xml]$doc = Get-Content -Raw -LiteralPath $xml
if ($doc.SelectNodes('//Launch.Addon').Count -ne 1 -or $doc.SelectSingleNode('//Launch.Addon/Name').InnerText -ne 'Keep Me') { throw 'Uninstall removed unrelated startup.' }
if ([IO.File]::ReadAllText((Join-Path $sim 'taxi-camera-native.addon64')) -ne 'legacy fixture') { throw 'Rollback did not restore the exact old file.' }
if (-not (Test-Path -LiteralPath (Join-Path $app 'taxi-camera-mounts.cfg'))) { throw 'Uninstall removed user calibration.' }
Write-Output 'PASS native install/rollback: isolated process fixtures and paths, running-simulator refusal, exact binary receipts, startup preservation, calibration import, legacy retention and unrelated graphics files preserved.'

. (Join-Path $repoRoot 'installer/validation_receipt.ps1')
$requested = 'C:\Users\Pilot\AppData\Local\Taxi Cam\app\taxi-cam.exe'
$redirected = '\\?\C:\Users\Pilot\AppData\Local\Packages\Example.Desktop_123\LocalCache\Local\Taxi Cam\app\taxi-cam.exe'
if (-not (Test-TaxiRedirectedInstallPath $requested $redirected)) { throw 'Packaged LocalAppData redirection was accepted.' }
if (-not (Test-TaxiRedirectedInstallPath $requested ($redirected -replace 'LocalCache\\Local','LocalCache\Roaming'))) { throw 'Packaged roaming redirection was accepted.' }
if (Test-TaxiRedirectedInstallPath $requested ('\\?\' + $requested)) { throw 'Ordinary per-user installation was rejected.' }
if (Test-TaxiRedirectedInstallPath 'C:\Users\Pilot\Apps\Taxi Cam\taxi-cam.exe' '\\?\D:\Apps\Taxi Cam\taxi-cam.exe') { throw 'A normal filesystem alias was rejected.' }
if (Test-TaxiRedirectedInstallPath $redirected $redirected) { throw 'An explicitly selected physical path was rejected.' }
$fixturePhysical = Get-TaxiPhysicalFilePath (Join-Path $sim 'dxgi.dll')
if (-not $fixturePhysical.EndsWith('\sim\dxgi.dll', [StringComparison]::OrdinalIgnoreCase)) { throw 'Physical file path lookup failed.' }
Write-Output 'PASS installation visibility: ordinary paths, physical aliases, explicit cache paths, Local/Roaming redirection and real handle resolution.'
