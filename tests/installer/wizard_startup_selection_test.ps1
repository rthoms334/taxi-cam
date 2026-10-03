[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$testRoot = Join-Path $repoRoot ('build/tests/installer/wizard-startup-' + [Guid]::NewGuid().ToString('N'))
$payload = Join-Path $testRoot 'payload'
$store = Join-Path $testRoot 'Store/Content'
$steam = Join-Path $testRoot 'Steam/MSFS2024'
$oldXml = Join-Path $testRoot 'Store-config/exe.xml'
$newXml = Join-Path $testRoot 'Steam-config/exe.xml'
New-Item -ItemType Directory -Force -Path $payload,$store,$steam,(Split-Path -Parent $oldXml),(Split-Path -Parent $newXml) | Out-Null
# The payload's taxi-cam.exe is an argument recorder: it answers discovery
# from the fixture record and always refuses the pre-installation check, so no
# native installer, registration, shortcut, settings or simulator change occurs.
foreach ($name in @('taxi-camera-bridge.dll','LICENSE.txt','THIRD_PARTY_NOTICES.txt','taxi-camera-mounts.cfg')) {
    [IO.File]::WriteAllText((Join-Path $payload $name), 'This fixture never installs a native runtime.')
}
foreach ($sim in @($store,$steam)) { [IO.File]::WriteAllText((Join-Path $sim 'FlightSimulator2024.exe'), 'File-existence fixture only.') }
foreach ($xml in @($oldXml,$newXml)) { [IO.File]::WriteAllText($xml, '<SimBase.Document Type="Launch"/>') }
$xmlHashes = @{}
foreach ($xml in @($oldXml,$newXml)) { $xmlHashes[$xml] = (Get-FileHash -LiteralPath $xml).Hash }
$deps = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'dependencies.json') | ConvertFrom-Json
$cxx = Join-Path $repoRoot ('build/deps/' + $deps.'llvm-mingw'.directory + '/bin/clang++.exe')
& $cxx -std=c++20 -O2 -static -municode -DNOMINMAX -DWIN32_LEAN_AND_MEAN (Join-Path $PSScriptRoot 'wizard_capture.cpp') -o (Join-Path $payload 'taxi-cam.exe')
if ($LASTEXITCODE -ne 0) { throw 'Wizard argument recorder compilation failed.' }
$compiler = & (Join-Path $repoRoot 'installer/bootstrap.ps1')
$script:checks = 0
function Assert-Wizard([bool]$Condition,[string]$Message) {
    $script:checks++
    if (-not $Condition) { throw $Message }
}
function Build-WizardFixture([string]$Source,[string]$Name) {
    $log = Join-Path $testRoot ($Name + '-compiler.log')
    & $compiler "/DPayloadDir=$payload" "/DAppIcon=$(Join-Path $repoRoot 'src/app/taxi-cam.ico')" `
        '/DAppVersion=0.0.0' '/DBuildNumber=0' '/DInstallerTest=1' `
        "/DOutputBase=$Name" "/O$testRoot" $Source *> $log
    if ($LASTEXITCODE -ne 0) { throw "Wizard fixture compilation failed. See $log" }
    return Join-Path $testRoot ($Name + '.exe')
}
function Invoke-WizardCase([string]$Fixture,[string]$Name,[string[]]$Options,[string]$ExpectedSimulator,
                           [bool]$ExpectXml,[string]$ExpectedXml = '',[string]$ExpectedStartup = 'automatic') {
    $app = Join-Path $testRoot $Name
    New-Item -ItemType Directory -Path $app | Out-Null
    $record = Join-Path $app 'installation.json'
    @{simulator=(Join-Path $store 'FlightSimulator2024.exe'); exeXml=$oldXml} |
        ConvertTo-Json | Set-Content -LiteralPath $record -Encoding utf8
    $recordHash = (Get-FileHash -LiteralPath $record).Hash
    $log = Join-Path $testRoot ($Name + '.log')
    $arguments = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/DIR="' + $app + '"'),('/LOG="' + $log + '"')) + $Options
    $process = Start-Process -FilePath $Fixture -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(30000)) {
        # Only stop the process tree launched for this fixture; never discover or
        # stop any simulator, companion or unrelated setup process.
        if (-not $process.HasExited) { & (Join-Path $env:WINDIR 'System32/taskkill.exe') /PID $process.Id /T /F *> $null }
        throw "Wizard fixture timed out. See $log"
    }
    $process.Refresh()
    Assert-Wizard ($process.ExitCode -ne 0) 'Argument fixture unexpectedly completed installation.'
    $capturePath = Join-Path $app 'captured-arguments.json'
    Assert-Wizard (Test-Path -LiteralPath $capturePath -PathType Leaf) "Wizard did not reach the argument recorder: $log"
    $capture = Get-Content -LiteralPath $capturePath -Raw | ConvertFrom-Json
    Assert-Wizard ($capture.simulator -eq $ExpectedSimulator) "Wrong simulator passed for $Name."
    Assert-Wizard ($capture.exeXmlWasPassed -eq $ExpectXml) "Wrong inherited/explicit XML argument decision for $Name."
    Assert-Wizard ($capture.exeXml -eq $ExpectedXml) "Wrong XML path passed for $Name."
    Assert-Wizard ($capture.startup -eq $ExpectedStartup) "Startup mode changed for $Name."
    Assert-Wizard ((Get-FileHash -LiteralPath $record).Hash -eq $recordHash) 'Fixture changed the installation record.'
    Assert-Wizard (-not (Test-Path -LiteralPath (Join-Path $app 'taxi-cam.exe'))) 'Fixture wrote a native application.'
}

$productionSource = Join-Path $repoRoot 'installer/taxi-cam.iss'
$fixture = Build-WizardFixture $productionSource 'wizard-startup-capture'
Invoke-WizardCase $fixture 'unchanged-simulator' @() $store $true $oldXml
Invoke-WizardCase $fixture 'silent-store-to-steam' @('/SIMULATORDIR="' + $steam + '"') $steam $false
Invoke-WizardCase $fixture 'explicit-new-xml' @('/SIMULATORDIR="' + $steam + '"','/EXEXML="' + $newXml + '"') $steam $true $newXml
Invoke-WizardCase $fixture 'explicit-old-xml' @('/SIMULATORDIR="' + $steam + '"','/EXEXML="' + $oldXml + '"') $steam $true $oldXml
Invoke-WizardCase $fixture 'manual-simulator-change' @('/SIMULATORDIR="' + $steam + '"','/STARTUP=manual') $steam $false '' 'manual'

# A private source copy assigns control values immediately before the final
# production guard. It exercises that second guard independently of page Next,
# and triggers the real edit event instead of reimplementing the selection rule.
$source = Get-Content -LiteralPath $productionSource -Raw
$marker = "  ClearStaleInheritedXml;`n  KnownInstallation"
$normalized = $source.Replace("`r`n", "`n")
Assert-Wizard ($normalized.Split([string[]]@($marker),[StringSplitOptions]::None).Length -eq 2) 'Final selection-guard test marker is ambiguous.'
$driver = @'
  if ExpandConstant('{param:TESTEDITXML|}') <> '' then
    XmlPage.Values[0] := ExpandConstant('{param:TESTEDITXML|}');
  if ExpandConstant('{param:TESTRESELECTINHERITED|0}') = '1' then begin
    XmlPage.Values[0] := InheritedXml + '.selection-in-progress';
    XmlPage.Values[0] := InheritedXml;
  end;
  if ExpandConstant('{param:TESTLATESIM|}') <> '' then
    SimulatorPage.Values[0] := ExpandConstant('{param:TESTLATESIM|}');
  ClearStaleInheritedXml;
  KnownInstallation
'@
$driverSource = Join-Path $testRoot 'wizard-control-driver.iss'
$normalized.Replace($marker,$driver.Replace("`r`n","`n")) | Set-Content -LiteralPath $driverSource -Encoding utf8
$driverFixture = Build-WizardFixture $driverSource 'wizard-control-driver'
Invoke-WizardCase $driverFixture 'prepare-guard-simulator-change' @('/TESTLATESIM="' + $steam + '"') $steam $false
Invoke-WizardCase $driverFixture 'preserve-user-edit' @('/TESTLATESIM="' + $steam + '"','/TESTEDITXML="' + $newXml + '"') $steam $true $newXml
Invoke-WizardCase $driverFixture 'preserve-user-reselection' @('/TESTLATESIM="' + $steam + '"','/TESTRESELECTINHERITED=1') $steam $true $oldXml
foreach ($xml in @($oldXml,$newXml)) {
    Assert-Wizard ((Get-FileHash -LiteralPath $xml).Hash -eq $xmlHashes[$xml]) 'Wizard fixture changed a startup XML file.'
}
[ordered]@{
    utc=[DateTime]::UtcNow.ToString('o'); checks=$checks; cases=8; nativeInstallationPerformed=$false;
    method='Compiled wizard argument capture; helper always refuses before installation. Private control driver exercises edit events and final guard.';
    sourceSha256=(Get-FileHash -LiteralPath $productionSource).Hash;
    compilerSha256=(Get-FileHash -LiteralPath $compiler).Hash;
    fixtureSha256=(Get-FileHash -LiteralPath $fixture).Hash;
    driverFixtureSha256=(Get-FileHash -LiteralPath $driverFixture).Hash
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $testRoot 'result.json') -Encoding utf8
Write-Output "PASS wizard startup selection: $checks checks across 8 compiled wizard cases; no installation performed. Evidence: $testRoot"
