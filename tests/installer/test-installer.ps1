[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Installer)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Set-StrictMode -Version Latest
$repo = $repoRoot
. (Join-Path $repoRoot 'tests/support/installer_fixture.ps1')
$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$receipt = Get-Content -Raw -LiteralPath ($installerPath + '.json') | ConvertFrom-Json
if ((Get-FileHash -LiteralPath $installerPath).Hash -ne $receipt.installerSha256) { throw 'Installer hash does not match its build receipt.' }
$testRoot = Join-Path $repo ('build/installer-tests/' + [Guid]::NewGuid().ToString('N'))
$sim = Join-Path $testRoot 'sim'; $xmlPath = Join-Path $testRoot 'config/exe.xml'
New-Item -ItemType Directory -Force -Path $sim,(Split-Path -Parent $xmlPath) | Out-Null
New-TaxiFixtureImage (Join-Path $sim 'FlightSimulator2024.exe') $false
New-TaxiFixtureImage (Join-Path $sim 'SimConnect_internal.dll')
$xml = '<?xml version="1.0"?><SimBase.Document Type="Launch"><Disabled>False</Disabled><Launch.Addon><Name>Other Addon</Name><Path>C:\Other\other.exe</Path></Launch.Addon></SimBase.Document>'
function Invoke-Setup([string]$Executable,[string]$App,[string]$Log,[switch]$Paths) {
    $arguments = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/DIR="' + $App + '"'),('/LOG="' + $Log + '"'))
    if ($Paths) { $arguments += @('/SIMULATORDIR="' + $sim + '"','/EXEXML="' + $xmlPath + '"') }
    $process = Start-Process -FilePath $Executable -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(60000)) { throw "Installer test timed out; process $($process.Id), log $Log" }
    $process.Refresh()
    return $process.ExitCode
}
function Assert-That([bool]$Condition,[string]$Message) { if (-not $Condition) { throw $Message } }
# Exercise the exact production executable through a guaranteed pre-write failure.
# Even if MSFS is running, Setup must return failure and leave all fixture bytes unchanged.
$xml.Replace('<Disabled>False</Disabled>','<Disabled>True</Disabled>') | Set-Content -LiteralPath $xmlPath
$before = (Get-FileHash -LiteralPath $xmlPath).Hash
$negativeApp = Join-Path $testRoot 'production-rejected'
$exitCode = Invoke-Setup $installerPath $negativeApp (Join-Path $testRoot 'production-rejection.log') -Paths
Assert-That ($exitCode -ne 0) 'The production installer ignored a disabled launch document.'
Assert-That ((Get-FileHash -LiteralPath $xmlPath).Hash -eq $before) 'The rejected production install modified exe.xml.'
Assert-That (-not (Test-Path -LiteralPath (Join-Path $negativeApp 'taxi-cam.exe'))) 'The rejected production install wrote a live executable.'

# A separately compiled fixture suppresses registration and shortcuts. Only its private copies
# of process discovery are mocked, allowing local simulator sessions to remain untouched.
$payload = Join-Path $testRoot 'payload'
Copy-Item -LiteralPath $receipt.compilerPayload -Destination $payload -Recurse
$runtime = Join-Path $testRoot 'runtime.ps1'
Copy-Item -LiteralPath (Join-Path $repoRoot 'installer/runtime.ps1') -Destination $runtime
foreach ($path in @($runtime,(Join-Path $payload 'install.ps1'),(Join-Path $payload 'uninstall.ps1'))) {
    $source = (Get-Content -Raw -LiteralPath $path).Replace('Get-Process','Get-FixtureProcess')
    $source = $source.Replace('Set-StrictMode -Version Latest', "Set-StrictMode -Version Latest`r`nfunction Get-FixtureProcess { return @() }")
    Set-Content -LiteralPath $path -Value $source -Encoding utf8
}
& (Join-Path $repoRoot 'installer/embed-uninstaller.ps1') -Output (Join-Path $testRoot 'uninstall-scripts.iss') -RuntimeScript $runtime -UninstallScript (Join-Path $payload 'uninstall.ps1') -ExeXmlScript (Join-Path $payload 'exe_xml.ps1')
$compiler = & (Join-Path $repo 'installer/bootstrap.ps1')
& $compiler "/DPayloadDir=$payload" "/DInternalDir=$testRoot" "/DRuntimeScript=$runtime" "/DAppVersion=$($receipt.version)" "/DBuildNumber=$($receipt.buildNumber)" '/DInstallerTest=1' '/DOutputBase=isolated-setup' "/O$testRoot" (Join-Path $repoRoot 'installer/taxi-cam.iss') *> (Join-Path $testRoot 'compiler.log')
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
$exitCode = Invoke-Setup $fixture $app (Join-Path $testRoot 'install.log') -Paths
Assert-That ($exitCode -eq 0) "Isolated first install failed ($exitCode): $testRoot"
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
[xml]$launch = Get-Content -Raw -LiteralPath $xmlPath
Assert-That ($launch.SelectNodes('//Launch.Addon').Count -eq 2) 'Upgrade duplicated the startup entry.'
Assert-That ($launch.SelectNodes('//Launch.Addon[Name="Taxi Cam"]').Count -eq 1) 'Upgrade did not rename startup registration.'
Assert-That (-not (Test-Path -LiteralPath $oldExe)) 'Upgrade retained the obsolete companion executable.'
# Fail after the native transaction to verify Setup cancellation/error rollback.
$rollbackScript = Join-Path $testRoot 'rollback.iss'
$iss = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'installer/taxi-cam.iss')
$iss = $iss.Replace('if CurStep = ssDone then Completed := True;', "if CurStep = ssInstall then Abort;`r`n  if CurStep = ssDone then Completed := True;")
Set-Content -LiteralPath $rollbackScript -Value $iss -Encoding utf8
& $compiler "/DPayloadDir=$payload" "/DInternalDir=$testRoot" "/DRuntimeScript=$runtime" "/DAppVersion=$($receipt.version)" "/DBuildNumber=$($receipt.buildNumber)" '/DInstallerTest=1' '/DOutputBase=rollback-setup' "/O$testRoot" $rollbackScript *> (Join-Path $testRoot 'rollback-compiler.log')
if ($LASTEXITCODE -ne 0) { throw "Rollback fixture compilation failed: $testRoot" }
Move-Item -LiteralPath (Join-Path $app 'taxi-cam.exe') -Destination $oldExe
$oldExeHash = (Get-FileHash -LiteralPath $oldExe).Hash
$launch.SelectSingleNode('//Launch.Addon[Name="Taxi Cam"]/Name').InnerText = '380 Taxi Cam'
$launch.SelectSingleNode('//Launch.Addon[Name="380 Taxi Cam"]/Path').InnerText = $oldExe
$launch.Save($xmlPath)
$xmlHash = (Get-FileHash -LiteralPath $xmlPath).Hash
$recordHash = (Get-FileHash -LiteralPath (Join-Path $app 'installation.json')).Hash
foreach ($name in @('LICENSE.txt','THIRD_PARTY_NOTICES.txt')) { [IO.File]::WriteAllText((Join-Path $app $name), "prior $name fixture") }
$exitCode = Invoke-Setup (Join-Path $testRoot 'rollback-setup.exe') $app (Join-Path $testRoot 'rollback.log')
Assert-That ($exitCode -ne 0) 'Injected Setup abort did not fail.'
Assert-That ((Get-FileHash -LiteralPath $xmlPath).Hash -eq $xmlHash) 'Setup abort did not restore exe.xml.'
Assert-That ((Get-FileHash -LiteralPath (Join-Path $app 'installation.json')).Hash -eq $recordHash) 'Setup abort did not restore installation.json.'
Assert-That ((Get-FileHash -LiteralPath $oldExe).Hash -eq $oldExeHash) 'Setup abort did not restore the previous executable name and bytes.'
Assert-That (-not (Test-Path -LiteralPath (Join-Path $app 'taxi-cam.exe'))) 'Setup abort retained the renamed executable.'
foreach ($name in @('LICENSE.txt','THIRD_PARTY_NOTICES.txt')) {
    Assert-That ([IO.File]::ReadAllText((Join-Path $app $name)) -eq "prior $name fixture") "Setup abort did not restore $name."
}
$exitCode = Invoke-Setup $fixture $app (Join-Path $testRoot 'retry-upgrade.log')
Assert-That ($exitCode -eq 0 -and -not (Test-Path -LiteralPath $oldExe)) 'Retry after rename rollback did not complete the upgrade.'
foreach ($name in @('LICENSE.txt','THIRD_PARTY_NOTICES.txt')) {
    Assert-That ((Get-FileHash -LiteralPath (Join-Path $app $name)).Hash -eq (Get-FileHash -LiteralPath (Join-Path $payload $name)).Hash) "Retry did not restore the bundled $name."
}
$uninstaller = Join-Path $app 'unins000.exe'
$process = Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/LOG="' + (Join-Path $testRoot 'uninstall.log') + '"')) -WindowStyle Hidden -Wait -PassThru
Assert-That ($process.ExitCode -eq 0) 'Isolated uninstall failed.'
Assert-That ((Get-FileHash -LiteralPath $mount).Hash -eq $mountHash) 'Uninstall removed calibration.'
[xml]$launch = Get-Content -Raw -LiteralPath $xmlPath
Assert-That ($launch.SelectNodes('//Launch.Addon').Count -eq 1) 'Uninstall did not preserve exactly the unrelated startup entry.'
Assert-That (-not (Test-Path -LiteralPath (Join-Path $app 'taxi-cam.exe'))) 'Uninstall retained the installed executable.'
foreach ($name in @('LICENSE.txt','THIRD_PARTY_NOTICES.txt')) {
    Assert-That (-not (Test-Path -LiteralPath (Join-Path $app $name))) "Uninstall retained $name."
}

function Invoke-FixtureRuntime([string]$Mode,[string]$App,[string]$State) {
    $arguments = @('-NoLogo','-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',('"' + $runtime + '"'),'-Mode',$Mode,
        '-Destination',('"' + $App + '"'),'-StateDirectory',('"' + $State + '"'),
        '-SimulatorDirectory',('"' + $sim + '"'),'-ExeXml',('"' + $xmlPath + '"'),'-PayloadDirectory',('"' + $payload + '"'))
    $process = Start-Process -FilePath (Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe') -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    return $process.ExitCode
}
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
$laterApp = Join-Path $testRoot 'concurrent-later-app'; $laterState = Join-Path $testRoot 'concurrent-later-state'
$exitCode = Invoke-FixtureRuntime 'Install' $laterApp $laterState
Assert-That ($exitCode -eq 0) 'Concurrency fixture could not complete its initial transaction.'
Add-Content -LiteralPath $xmlPath -Value '<!-- concurrent later edit -->'
$laterHash = (Get-FileHash -LiteralPath $xmlPath).Hash
$exitCode = Invoke-FixtureRuntime 'Rollback' $laterApp $laterState
Assert-That ($exitCode -ne 0) 'Rollback did not report the concurrent edit conflict.'
Assert-That ((Get-FileHash -LiteralPath $xmlPath).Hash -eq $laterHash) 'Rollback erased an XML edit made after its transaction.'
$conflict = Get-Content -Raw -LiteralPath (Join-Path $laterState 'error.txt')
Assert-That ($conflict -match 'Recovery snapshot: (.+)$') 'Rollback did not retain a recovery snapshot.'
Assert-That (Test-Path -LiteralPath (Join-Path $Matches[1] 'transaction.json')) 'The reported rollback snapshot does not exist.'

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

[ordered]@{passed=$true;installerSha256=$receipt.installerSha256;shortPathValidation=$shortPathResult;tests=@('exact production rejection','missing SimConnect refused before application and startup writes','isolated first install','remembered upgrade paths and former name migration','calibration preservation','post-transaction Setup rollback restores former executable and startup','retry after rename rollback','isolated uninstall','inner concurrent XML edit preserved','post-transaction concurrent XML edit preserved with recovery snapshot');simulatorVerified=$false} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $testRoot 'result.json') -Encoding utf8
Write-Output "Installer checks passed: $testRoot"
