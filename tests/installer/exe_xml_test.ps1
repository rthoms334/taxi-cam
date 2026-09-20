$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
. (Join-Path $repoRoot 'installer/exe_xml.ps1')
$testRoot = Join-Path $repoRoot 'build/tests/installer/xml-validation'
New-Item -ItemType Directory -Force -Path $testRoot | Out-Null
$path = Join-Path $testRoot 'exe.xml'
@'
<?xml version="1.0" encoding="utf-8"?>
<SimBase.Document Type="Launch" version="1,0">
<!-- An unrelated add-on owns this comment -->
<Disabled>False</Disabled><Launch.ManualLoad>False</Launch.ManualLoad>
<Launch.Addon><Name>Existing &amp; Addon</Name><Path>C:\Other App\other.exe</Path><CommandLine>--keep="exact"</CommandLine><Disabled>True</Disabled><Custom>preserved</Custom></Launch.Addon>
</SimBase.Document>
'@ | Set-Content -LiteralPath $path -Encoding utf8
$document = Read-TaxiLaunchXml $path
$untouched = $document.SelectSingleNode('//Launch.Addon').OuterXml
$original = (Get-FileHash -LiteralPath $path).Hash
Set-TaxiStartupEntry $document 'C:\Native Camera & Tools\taxi-cam.exe' 'C:\MSFS\FlightSimulator2024.exe'
$writtenHash = ''
$backup = Save-TaxiLaunchXml $document $path $original ([ref]$writtenHash)
if (-not $backup -or (Get-FileHash -LiteralPath $backup).Hash -ne $original) { throw 'Original startup file was not preserved.' }
if ($writtenHash -ne (Get-FileHash -LiteralPath $path).Hash) { throw 'Written hash does not describe the committed XML bytes.' }
$read = Read-TaxiLaunchXml $path
if ($read.SelectSingleNode('//Launch.Addon[Name="Existing & Addon"]').OuterXml -ne $untouched) { throw 'Unrelated entry changed.' }
if ($read.SelectSingleNode('//Launch.Addon[Name="Taxi Cam"]/CommandLine').InnerText -ne '--background --simulator "C:\MSFS\FlightSimulator2024.exe"') { throw 'Argument quoting changed.' }
Set-TaxiStartupEntry $read 'C:\Native Camera & Tools\taxi-cam.exe' 'C:\MSFS\FlightSimulator2024.exe'
if ($read.SelectNodes('//Launch.Addon[Name="Taxi Cam"]').Count -ne 1) { throw 'Duplicate startup entry.' }
$read.SelectSingleNode('//Launch.Addon[Name="Taxi Cam"]/Name').InnerText = '380 Taxi Cam'
Set-TaxiStartupEntry $read 'C:\Native Camera & Tools\taxi-cam.exe' 'C:\MSFS\FlightSimulator2024.exe'
if ($read.SelectNodes('//Launch.Addon').Count -ne 2 -or $read.SelectNodes('//Launch.Addon[Name="Taxi Cam"]').Count -ne 1) { throw 'Rename did not migrate the existing entry in place.' }
$duplicate = $read.SelectSingleNode('//Launch.Addon[Name="Taxi Cam"]').CloneNode($true)
$duplicate.SelectSingleNode('Name').InnerText = '380 Taxi Cam'
[void]$read.DocumentElement.AppendChild($duplicate)
$refused = $false
try { Set-TaxiStartupEntry $read 'C:\Native Camera & Tools\taxi-cam.exe' 'C:\MSFS\FlightSimulator2024.exe' } catch { $refused = $true }
if (-not $refused) { throw 'Mixed old/new startup entries must refuse an ambiguous upgrade.' }
[void]$read.DocumentElement.RemoveChild($duplicate)
$refused = $false
try { Save-TaxiLaunchXml $read $path ('0' * 64) } catch { $refused = $_.Exception -is [InvalidOperationException] -and $_.Exception.Data['TaxiStartupConflict'] }
if (-not $refused) { throw 'Concurrent edit guard did not refuse.' }
Set-TaxiStartupEntry $read '' '' -Remove
if ($read.SelectNodes('//Launch.Addon').Count -ne 1 -or $read.SelectSingleNode('//Launch.Addon').OuterXml -ne $untouched) { throw 'Uninstall changed unrelated startup.' }
$invalid = Join-Path $testRoot 'dtd.xml'
'<!DOCTYPE SimBase.Document [<!ENTITY external SYSTEM "file:///C:/Windows/win.ini">]><SimBase.Document Type="Launch">&external;</SimBase.Document>' | Set-Content -LiteralPath $invalid
$refused = $false
try { Read-TaxiLaunchXml $invalid } catch { $refused = $true }
if (-not $refused) { throw 'External XML entity was not refused.' }
Write-Output 'PASS exe.xml: preserve unrelated entries/comments, escaped paths, exact arguments, backup, idempotence, concurrent-edit refusal, targeted removal, DTD refusal.'

# Some third-party startup files have a SimConnect header but only launch entries.
# Repair is explicitly requested and remains an in-memory edit until the existing
# verified-backup/atomic-replacement transaction commits the complete document.
$repairRoot = Join-Path $testRoot ('header-repair-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $repairRoot | Out-Null
$repairPath = Join-Path $repairRoot 'exe.xml'
$repairContents = @'
<?xml version="1.0" encoding="utf-8"?>
<SimBase.Document Type="SimConnect" version="1,0">
  <Descr>SimConnect</Descr><Filename>SimConnect.xml</Filename>
  <Disabled>False</Disabled><Launch.ManualLoad>False</Launch.ManualLoad>
  <!-- Preserve the first add-on's startup details -->
  <Launch.Addon><Name>Synaptic A220</Name><Path>C:\Other Aircraft\Synaptic.exe</Path><CommandLine>--keep="exact &amp; intact"</CommandLine><Disabled>False</Disabled><Custom>preserved</Custom></Launch.Addon>
  <!-- Preserve the second add-on's startup details -->
  <Launch.Addon><!-- Keep the add-on's own comment --><Name>FSRealistic</Name><Path>C:\Other App\FSRealistic.exe</Path><Disabled>True</Disabled><ManualLoad>True</ManualLoad></Launch.Addon>
</SimBase.Document>
'@
[IO.File]::WriteAllText($repairPath, $repairContents, [Text.UTF8Encoding]::new($true))
$repairHash = (Get-FileHash -LiteralPath $repairPath).Hash
$refused = $false
try { [void](Read-TaxiLaunchXml $repairPath) } catch { $refused = $true }
if (-not $refused) { throw 'Default startup reading unexpectedly repaired a SimConnect header.' }
$repairDocument = Read-TaxiLaunchXml $repairPath -RepairLaunchHeader
if ($repairDocument.DocumentElement.GetAttribute('Type') -cne 'Launch' -or
    $repairDocument.DocumentElement.SelectSingleNode('Descr').InnerText -cne 'Launch' -or
    $repairDocument.DocumentElement.SelectSingleNode('Filename').InnerText -cne 'exe.xml' -or
    $repairDocument.DocumentElement.SelectSingleNode('Disabled').InnerText -cne 'False' -or
    $repairDocument.DocumentElement.SelectSingleNode('Launch.ManualLoad').InnerText -cne 'False' -or
    $repairDocument.DocumentElement.GetAttribute('version') -cne '1,0') { throw 'Opt-in startup repair did not normalize only the launch header.' }
if ((Get-FileHash -LiteralPath $repairPath).Hash -ne $repairHash -or
    @(Get-ChildItem -LiteralPath $repairRoot -File).Count -ne 1) { throw 'Reading a repairable header changed disk files before commit.' }
$originalRepairDocument = [Xml.XmlDocument]::new()
$originalRepairDocument.PreserveWhitespace = $true
$originalRepairDocument.LoadXml($repairContents)
$originalAddons = @($originalRepairDocument.DocumentElement.SelectNodes('Launch.Addon') | ForEach-Object { $_.OuterXml })
$originalComments = @($originalRepairDocument.DocumentElement.SelectNodes('comment()') | ForEach-Object { $_.OuterXml })
for ($index = 0; $index -lt $originalAddons.Count; ++$index) {
    if ($repairDocument.DocumentElement.SelectNodes('Launch.Addon')[$index].OuterXml -cne $originalAddons[$index]) { throw 'Header repair changed an existing add-on before commit.' }
}
Set-TaxiStartupEntry $repairDocument 'C:\Native Camera & Tools\taxi-cam.exe' 'C:\MSFS\FlightSimulator2024.exe'
$writtenHash = ''
$repairBackup = Save-TaxiLaunchXml $repairDocument $repairPath $repairHash ([ref]$writtenHash)
if (-not $repairBackup -or (Get-FileHash -LiteralPath $repairBackup).Hash -ne $repairHash) { throw 'Header repair did not preserve the exact original startup bytes in its backup.' }
if ($writtenHash -ne (Get-FileHash -LiteralPath $repairPath).Hash) { throw 'Header repair reported an incorrect committed file hash.' }
$repaired = Read-TaxiLaunchXml $repairPath
if ($repaired.DocumentElement.SelectNodes('Launch.Addon').Count -ne 3 -or
    $repaired.DocumentElement.SelectNodes('Launch.Addon[Name="Taxi Cam"]').Count -ne 1) { throw 'Header repair lost an existing add-on or duplicated Taxi Cam.' }
for ($index = 0; $index -lt $originalAddons.Count; ++$index) {
    if ($repaired.DocumentElement.SelectNodes('Launch.Addon')[$index].OuterXml -cne $originalAddons[$index]) { throw 'Header repair round trip changed an unrelated add-on.' }
}
$repairedComments = @($repaired.DocumentElement.SelectNodes('comment()') | ForEach-Object { $_.OuterXml })
if ($repairedComments.Count -ne $originalComments.Count -or
    ($repairedComments -join "`n") -cne ($originalComments -join "`n")) { throw 'Header repair changed unrelated startup comments.' }
$repairedAgain = Read-TaxiLaunchXml $repairPath -RepairLaunchHeader
if ($repairedAgain.OuterXml -cne $repaired.OuterXml) { throw 'Opt-in reading changed an already valid launch document.' }
Set-TaxiStartupEntry $repairedAgain 'C:\Native Camera & Tools\taxi-cam.exe' 'C:\MSFS\FlightSimulator2024.exe'
if ($repairedAgain.DocumentElement.SelectNodes('Launch.Addon').Count -ne 3) { throw 'A repaired startup file is not idempotent.' }

# An exe.xml whose only defect is a SimConnect Type attribute (written by some
# add-on managers) is repaired in place: its own description and filename stay.
$misnamedRoot = Join-Path $repairRoot 'misnamed'
New-Item -ItemType Directory -Path $misnamedRoot | Out-Null
$misnamedPath = Join-Path $misnamedRoot 'exe.xml'
$misnamedContents = @'
<?xml version="1.0" encoding="utf-8"?>
<SimBase.Document Type="SimConnect" version="1,0">
  <Descr>Auto launch external applications on MSFS start</Descr>
  <Filename>exe.xml</Filename>
  <Disabled>False</Disabled>
  <Launch.Addon>
    <Disabled>False</Disabled>
    <ManualLoad>False</ManualLoad>
    <Name>FSUIPC7</Name>
    <Path>C:\FSUIPC7\FSUIPC7.exe</Path>
    <CommandLine>-auto</CommandLine>
    <NewConsole>False</NewConsole>
  </Launch.Addon>
  <Launch.Addon>
    <Name>IVAO Pilot Client</Name>
    <Disabled>False</Disabled>
    <Path>C:\IVAO\pilot_core_msfs.exe</Path>
    <Commandline />
  </Launch.Addon>
</SimBase.Document>
'@
[IO.File]::WriteAllText($misnamedPath, $misnamedContents, [Text.UTF8Encoding]::new($true))
$misnamedHash = (Get-FileHash -LiteralPath $misnamedPath).Hash
$refused = $false
try { [void](Read-TaxiLaunchXml $misnamedPath) } catch { $refused = $true }
if (-not $refused) { throw 'Default startup reading unexpectedly accepted a misnamed exe.xml header.' }
$misnamedDocument = Read-TaxiLaunchXml $misnamedPath -RepairLaunchHeader
if ($misnamedDocument.DocumentElement.GetAttribute('Type') -cne 'Launch' -or
    $misnamedDocument.DocumentElement.SelectSingleNode('Descr').InnerText -cne 'Auto launch external applications on MSFS start' -or
    $misnamedDocument.DocumentElement.SelectSingleNode('Filename').InnerText -cne 'exe.xml' -or
    $misnamedDocument.DocumentElement.SelectSingleNode('Disabled').InnerText -cne 'False') { throw 'Misnamed exe.xml repair changed more than the Type attribute.' }
if ((Get-FileHash -LiteralPath $misnamedPath).Hash -ne $misnamedHash) { throw 'Reading a misnamed exe.xml header changed the file before commit.' }
$originalMisnamed = [Xml.XmlDocument]::new()
$originalMisnamed.PreserveWhitespace = $true
$originalMisnamed.LoadXml($misnamedContents)
$originalMisnamedAddons = @($originalMisnamed.DocumentElement.SelectNodes('Launch.Addon') | ForEach-Object { $_.OuterXml })
Set-TaxiStartupEntry $misnamedDocument 'C:\Native Camera & Tools\taxi-cam.exe' 'C:\MSFS\FlightSimulator2024.exe'
$writtenHash = ''
$misnamedBackup = Save-TaxiLaunchXml $misnamedDocument $misnamedPath $misnamedHash ([ref]$writtenHash)
if (-not $misnamedBackup -or (Get-FileHash -LiteralPath $misnamedBackup).Hash -ne $misnamedHash) { throw 'Misnamed exe.xml repair did not back up the exact original bytes.' }
$misnamedRepaired = Read-TaxiLaunchXml $misnamedPath
if ($misnamedRepaired.DocumentElement.SelectNodes('Launch.Addon').Count -ne 3 -or
    $misnamedRepaired.DocumentElement.SelectNodes('Launch.Addon[Name="Taxi Cam"]').Count -ne 1) { throw 'Misnamed exe.xml repair lost an add-on or duplicated Taxi Cam.' }
for ($index = 0; $index -lt $originalMisnamedAddons.Count; ++$index) {
    if ($misnamedRepaired.DocumentElement.SelectNodes('Launch.Addon')[$index].OuterXml -cne $originalMisnamedAddons[$index]) { throw 'Misnamed exe.xml repair changed an unrelated add-on.' }
}
Write-Output 'PASS exe.xml misnamed SimConnect header: repaired to Launch in place, description and filename kept, add-ons preserved.'

$launchChild = '<Launch.Addon><Name>Keep Me</Name><Path>C:\Other.exe</Path></Launch.Addon>'
$launchHeaders = '<Descr>SimConnect</Descr><Filename>SimConnect.xml</Filename><Disabled>False</Disabled><Launch.ManualLoad>False</Launch.ManualLoad>'
$refusalCases = [ordered]@{
    'real-simconnect' = '<SimBase.Document Type="SimConnect">' + $launchHeaders + '<SimConnect.Comm><Protocol>IPv4</Protocol></SimConnect.Comm></SimBase.Document>'
    'mixed-comm-and-launch' = '<SimBase.Document Type="SimConnect">' + $launchHeaders + $launchChild + '<SimConnect.Comm><Protocol>IPv4</Protocol></SimConnect.Comm></SimBase.Document>'
    'unknown-child' = '<SimBase.Document Type="SimConnect">' + $launchHeaders + $launchChild + '<Unexpected>Keep</Unexpected></SimBase.Document>'
    'unknown-root' = '<Unexpected Type="SimConnect">' + $launchHeaders + $launchChild + '</Unexpected>'
    'unknown-type' = '<SimBase.Document Type="Other">' + $launchHeaders + $launchChild + '</SimBase.Document>'
    'namespaced-root' = '<SimBase.Document xmlns="urn:other-startup" Type="SimConnect">' + $launchHeaders + $launchChild + '</SimBase.Document>'
    'namespaced-child' = '<SimBase.Document Type="SimConnect">' + $launchHeaders + $launchChild + '<Disabled xmlns="urn:other-startup">False</Disabled></SimBase.Document>'
    'missing-addon' = '<SimBase.Document Type="SimConnect">' + $launchHeaders + '</SimBase.Document>'
    'missing-filename' = '<SimBase.Document Type="SimConnect"><Descr>SimConnect</Descr>' + $launchChild + '</SimBase.Document>'
    'different-filename' = '<SimBase.Document Type="SimConnect"><Filename>Other.xml</Filename>' + $launchChild + '</SimBase.Document>'
    'nonexact-filename' = '<SimBase.Document Type="SimConnect"><Filename>SimConnect.xml </Filename>' + $launchChild + '</SimBase.Document>'
    'nonexact-exe-filename' = '<SimBase.Document Type="SimConnect"><Filename>Exe.xml</Filename>' + $launchChild + '</SimBase.Document>'
    'nested-filename-content' = '<SimBase.Document Type="SimConnect"><Filename><Value>SimConnect.xml</Value></Filename>' + $launchChild + '</SimBase.Document>'
    'filename-comment' = '<SimBase.Document Type="SimConnect"><Filename><!-- Preserve me -->SimConnect.xml</Filename>' + $launchChild + '</SimBase.Document>'
    'nested-description-content' = '<SimBase.Document Type="SimConnect"><Descr><Value>SimConnect</Value></Descr><Filename>SimConnect.xml</Filename>' + $launchChild + '</SimBase.Document>'
    'description-comment' = '<SimBase.Document Type="SimConnect"><Descr><!-- Preserve me -->SimConnect</Descr><Filename>SimConnect.xml</Filename>' + $launchChild + '</SimBase.Document>'
}
foreach ($header in @('Descr', 'Filename', 'Disabled', 'Launch.ManualLoad')) {
    $duplicateValue = switch ($header) { 'Descr' { 'SimConnect' }; 'Filename' { 'SimConnect.xml' }; default { 'False' } }
    $refusalCases['duplicate-' + $header] = '<SimBase.Document Type="SimConnect">' + $launchHeaders +
        '<' + $header + '>' + $duplicateValue + '</' + $header + '>' + $launchChild + '</SimBase.Document>'
}
foreach ($case in $refusalCases.GetEnumerator()) {
    $casePath = Join-Path $repairRoot ($case.Key + '.xml')
    [IO.File]::WriteAllText($casePath, $case.Value)
    $caseHash = (Get-FileHash -LiteralPath $casePath).Hash
    $refused = $false
    try { [void](Read-TaxiLaunchXml $casePath -RepairLaunchHeader) } catch { $refused = $true }
    if (-not $refused) { throw "Unsafe or ambiguous header repair was accepted: $($case.Key)" }
    if ((Get-FileHash -LiteralPath $casePath).Hash -ne $caseHash) { throw "Refused header repair changed its source: $($case.Key)" }
}
foreach ($flags in @(@('True', 'False'), @('False', 'True'), @('True', 'True'))) {
    $flagPath = Join-Path $repairRoot ('flags-' + ($flags -join '-') + '.xml')
    $flagContents = '<SimBase.Document Type="SimConnect"><Descr>SimConnect</Descr><Filename>SimConnect.xml</Filename><Disabled>' +
        $flags[0] + '</Disabled><Launch.ManualLoad>' + $flags[1] + '</Launch.ManualLoad>' + $launchChild + '</SimBase.Document>'
    [IO.File]::WriteAllText($flagPath, $flagContents)
    $flagDocument = Read-TaxiLaunchXml $flagPath -RepairLaunchHeader
    Set-TaxiStartupEntry $flagDocument 'C:\Native Camera\taxi-cam.exe' 'C:\MSFS\FlightSimulator2024.exe'
    if ($flagDocument.DocumentElement.SelectSingleNode('Disabled').InnerText -cne $flags[0] -or
        $flagDocument.DocumentElement.SelectSingleNode('Launch.ManualLoad').InnerText -cne $flags[1]) {
        throw 'Header repair overrode global disabled/manual startup flags needed by the installer refusal policy.'
    }
}
Write-Output ('PASS exe.xml launch-header repair: explicit opt-in, memory-only preparation, exact original backup, preserved add-ons/comments/global flags, strict round trip, idempotence and ' + $refusalCases.Count + ' unsafe/ambiguous fixture refusals.')

# Failures before replacement must leave user XML intact and identify whether
# callers may safely continue with manual startup. All fixtures remain in build/.
$failureRoot = Join-Path $testRoot ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $failureRoot | Out-Null
$failurePath = Join-Path $failureRoot 'exe.xml'
$failureContents = '<SimBase.Document Type="Launch"><Launch.Addon><Name>Keep Me</Name><Path>C:\Other.exe</Path></Launch.Addon></SimBase.Document>'
[IO.File]::WriteAllText($failurePath, $failureContents)
$failureHash = (Get-FileHash -LiteralPath $failurePath).Hash
$failureDocument = Read-TaxiLaunchXml $failurePath
Set-TaxiStartupEntry $failureDocument 'C:\Native Camera\taxi-cam.exe' 'C:\MSFS\FlightSimulator2024.exe'
$script:copyFailure = ''
function Copy-Item {
    param([string]$LiteralPath,[string]$Destination)
    if ($Destination -like '*.taxi-backup-*') {
        switch ($script:copyFailure) {
            'encryption' { throw [ComponentModel.Win32Exception]::new(6000) }
            'permission' { throw [UnauthorizedAccessException]::new('Fixture denies the backup operation.') }
            'partial-backup' {
                [IO.File]::WriteAllText($Destination, '<SimBase.Document')
                throw [ComponentModel.Win32Exception]::new(6000)
            }
            'cleanup-failure' {
                [IO.File]::WriteAllText($Destination, '<SimBase.Document')
                [IO.File]::SetAttributes($Destination, [IO.FileAttributes]::ReadOnly)
                throw [ComponentModel.Win32Exception]::new(6000)
            }
            'changed-source' {
                [IO.File]::AppendAllText($LiteralPath, '<!-- concurrent edit during failed backup -->')
                throw [ComponentModel.Win32Exception]::new(6000)
            }
            'corrupt-backup' { [IO.File]::WriteAllText($Destination, 'not the original XML'); return }
        }
    }
    Microsoft.PowerShell.Management\Copy-Item -LiteralPath $LiteralPath -Destination $Destination
}
try {
    foreach ($kind in @('encryption','permission','partial-backup')) {
        $script:copyFailure = $kind
        $failure = $null; $writtenHash = 'not committed'
        try { Save-TaxiLaunchXml $failureDocument $failurePath $failureHash ([ref]$writtenHash) } catch { $failure = $_ }
        if (-not $failure -or $failure.Exception.Data['TaxiStartupUnsafe'] -or $failure.Exception.Data['TaxiStartupConflict']) { throw "Safe $kind failure was not classified as recoverable." }
        if ($kind -eq 'encryption' -and $failure.Exception.NativeErrorCode -ne 6000) { throw 'Synthetic encryption error lost its Windows error code.' }
        if ($failure.Exception.Data['TaxiStartupOperation'] -ne 'Back up startup file' -or
            $failure.Exception.Data['TaxiStartupSource'] -ne $failurePath -or
            $failure.Exception.Data['TaxiStartupDestination'] -notlike ($failurePath + '.taxi-backup-*')) {
            throw "Failed $kind backup omitted its exact operation, source or destination."
        }
        if ($writtenHash -or (Get-FileHash -LiteralPath $failurePath).Hash -ne $failureHash) { throw "Failed $kind backup changed the original XML or reported a commit." }
        if (@(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-*.tmp').Count) { throw "Failed $kind backup left temporary XML." }
        if (@(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-backup-*').Count) { throw "Failed $kind backup left an unverified copy." }
    }
    $script:copyFailure = 'corrupt-backup'; $failure = $null
    try { Save-TaxiLaunchXml $failureDocument $failurePath $failureHash } catch { $failure = $_ }
    if (-not $failure -or -not $failure.Exception.Data['TaxiStartupConflict']) { throw 'Backup verification failure did not retain the conflict guard.' }
    if ((Get-FileHash -LiteralPath $failurePath).Hash -ne $failureHash) { throw 'Bad backup changed the original XML.' }
    if (@(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-backup-*').Count) { throw 'Hash-mismatched backup was retained.' }

    $script:copyFailure = 'cleanup-failure'; $failure = $null
    try {
        try { Save-TaxiLaunchXml $failureDocument $failurePath $failureHash } catch { $failure = $_ }
        if (-not $failure -or -not $failure.Exception.Data['TaxiStartupUnsafe']) { throw 'Unverified backup cleanup failure was allowed to fall back to manual startup.' }
        $remaining = @(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-backup-*')
        if ($remaining.Count -ne 1 -or -not $failure.Exception.Message.Contains($remaining[0].FullName)) { throw 'Cleanup failure did not identify the retained unverified backup.' }
        if ((Get-FileHash -LiteralPath $failurePath).Hash -ne $failureHash) { throw 'Cleanup failure changed the original XML.' }
    } finally {
        foreach ($item in @(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-backup-*')) {
            [IO.File]::SetAttributes($item.FullName, [IO.FileAttributes]::Normal)
            [IO.File]::Delete($item.FullName)
        }
    }

    $script:copyFailure = 'changed-source'; $failure = $null
    try { Save-TaxiLaunchXml $failureDocument $failurePath $failureHash } catch { $failure = $_ }
    if (-not $failure -or -not $failure.Exception.Data['TaxiStartupUnsafe']) { throw 'Failed backup with uncertain original state was allowed to continue.' }
    if (-not [IO.File]::ReadAllText($failurePath).Contains('<!-- concurrent edit during failed backup -->')) { throw 'Failure handler erased another writer''s XML edit.' }
    if (@(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-*.tmp').Count) { throw 'Failure paths left temporary XML.' }
} finally { Remove-Item Function:\Copy-Item }

# Simulate the EFS attribute/encryption calls in a private copy of the helper.
# The backup copy succeeds with matching bytes but without its expected EFS flag.
# No personal EFS key or encrypted fixture is created by this default test.
[IO.File]::WriteAllText($failurePath, $failureContents)
& {
    $helper = Get-Content -Raw -LiteralPath (Join-Path $repoRoot 'installer/exe_xml.ps1')
    foreach ($argument in @('$absolute','$temporary','$backup')) {
        $call = '[IO.File]::GetAttributes(' + $argument + ')'
        if (-not $helper.Contains($call)) { throw 'The simulated encryption attribute hook no longer matches.' }
        $helper = $helper.Replace($call, ('(Get-FixtureAttributes ' + $argument + ')'))
    }
    $encryptCall = '[IO.File]::Encrypt($temporary)'
    if (-not $helper.Contains($encryptCall)) { throw 'The simulated encryption operation hook no longer matches.' }
    $helper = $helper.Replace($encryptCall, 'Set-FixtureEncryption $temporary')
    . ([scriptblock]::Create($helper))
    $encryptedPaths = @{$failurePath = $true}
    function Get-FixtureAttributes([string]$Path) {
        $attributes = [IO.File]::GetAttributes($Path)
        if ($encryptedPaths.ContainsKey($Path)) { return $attributes -bor [IO.FileAttributes]::Encrypted }
        return $attributes
    }
    function Set-FixtureEncryption([string]$Path) { $encryptedPaths[$Path] = $true }
    $failure = $null; $writtenHash = 'not committed'
    try { Save-TaxiLaunchXml $failureDocument $failurePath $failureHash ([ref]$writtenHash) } catch { $failure = $_ }
    if (-not $failure -or $failure.Exception.Message -ne 'Could not preserve exe.xml encryption on the backup file.') { throw 'The encryption-mismatched backup fixture did not reach verification.' }
    if ($failure.Exception.Data['TaxiStartupUnsafe'] -or $failure.Exception.Data['TaxiStartupConflict']) { throw 'Successful unverified backup cleanup blocked safe manual startup.' }
    if ($writtenHash -or (Get-FileHash -LiteralPath $failurePath).Hash -ne $failureHash) { throw 'Encryption-mismatched backup changed the original XML or reported a commit.' }
    if (@(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-backup-*').Count) { throw 'Encryption-mismatched backup left an unencrypted copy.' }
    if (@(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-*.tmp').Count) { throw 'Encryption-mismatched backup left temporary XML.' }
}

# A real access-denied replacement, after a verified backup exists, is still safe
# to downgrade only when the original bytes and encryption remain unchanged.
[IO.File]::WriteAllText($failurePath, $failureContents)
[IO.File]::SetAttributes($failurePath, [IO.FileAttributes]::ReadOnly)
try {
    $failure = $null; $writtenHash = ''
    try { Save-TaxiLaunchXml $failureDocument $failurePath $failureHash ([ref]$writtenHash) } catch { $failure = $_ }
    if (-not $failure -or $failure.Exception.Data['TaxiStartupUnsafe'] -or $writtenHash) { throw 'Read-only original did not produce a safe uncommitted failure.' }
    if ((Get-FileHash -LiteralPath $failurePath).Hash -ne $failureHash) { throw 'Read-only original changed.' }
    if (@(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-*.tmp').Count) { throw 'Failed replacement left temporary XML.' }
    $validBackups = @(Get-ChildItem -LiteralPath $failureRoot -Filter 'exe.xml.taxi-backup-*' | Where-Object { (Get-FileHash -LiteralPath $_.FullName).Hash -eq $failureHash })
    if (-not $validBackups.Count) { throw 'Failed replacement discarded its verified recovery backup.' }
} finally { [IO.File]::SetAttributes($failurePath, [IO.FileAttributes]::Normal) }

# New XML has no backup, but still reports the bytes owned by the transaction.
$newRoot = Join-Path $failureRoot 'new'
$newPath = Join-Path $newRoot 'exe.xml'
$newDocument = Read-TaxiLaunchXml $newPath
Set-TaxiStartupEntry $newDocument 'C:\Native Camera\taxi-cam.exe' 'C:\MSFS\FlightSimulator2024.exe'
$writtenHash = ''
$newBackup = Save-TaxiLaunchXml $newDocument $newPath '' ([ref]$writtenHash)
if ($newBackup -or $writtenHash -ne (Get-FileHash -LiteralPath $newPath).Hash) { throw 'New-file transaction ownership is incorrect.' }
Write-Output 'PASS exe.xml failure resilience: encryption error 6000 and access denial preserve original, partial/hash/encryption-mismatched backups removed, cleanup failure blocks fallback, typed conflicts, exact committed hashes and retained verified backups.'

# Simulate packaged-host filesystem redirection without writing AppData or
# changing a real launch file. Visibility must be proved before XML is written
# to the reserved sibling; failure must remain an uncommitted transaction.
& {
    $visibilityRoot = Join-Path $testRoot ('visibility-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $visibilityRoot | Out-Null
    $visibilityPath = Join-Path $visibilityRoot 'exe.xml'
    [IO.File]::WriteAllText($visibilityPath, $failureContents)
    $visibilityHash = (Get-FileHash -LiteralPath $visibilityPath).Hash
    $visibilityDocument = Read-TaxiLaunchXml $visibilityPath
    Set-TaxiStartupEntry $visibilityDocument 'C:\Native Camera\taxi-cam.exe' 'C:\MSFS\FlightSimulator2024.exe'
    $visibilityCalls = [Collections.Generic.List[string]]::new()
    $visibilityMode = 'refuse-temporary'
    function Assert-TaxiVisibleInstallPath([string]$Path) {
        [void]$visibilityCalls.Add($Path)
        if ($Path -like ($visibilityPath + '.taxi-*.tmp')) {
            if (-not [IO.File]::Exists($Path) -or ([IO.FileInfo]::new($Path)).Length -ne 0) {
                throw 'Fixture visibility check did not precede writing XML to an existing empty sibling.'
            }
            if ($visibilityMode -eq 'refuse-temporary') { throw 'Fixture detects a redirected startup replacement.' }
        } elseif ($Path -eq $visibilityPath) {
            if ($visibilityMode -eq 'refuse-original') { throw 'Fixture detects a redirected existing startup file.' }
        } else { throw "Unexpected path passed to startup visibility proof: $Path" }
        if ($visibilityMode -eq 'refuse-all') { throw 'Default XML saving unexpectedly requested visibility proof.' }
    }
    foreach ($mode in @('refuse-temporary', 'refuse-original')) {
        $visibilityMode = $mode
        $visibilityCalls.Clear()
        $failure = $null; $writtenHash = 'not committed'
        try { Save-TaxiLaunchXml $visibilityDocument $visibilityPath $visibilityHash ([ref]$writtenHash) -RequireVisiblePath } catch { $failure = $_ }
        $expectedMessage = if ($mode -eq 'refuse-temporary') { 'Fixture detects a redirected startup replacement.' } else { 'Fixture detects a redirected existing startup file.' }
        if (-not $failure -or $failure.Exception.Message -ne $expectedMessage) { throw "The $mode fixture did not reach its requested visibility guard." }
        if ($writtenHash -or (Get-FileHash -LiteralPath $visibilityPath).Hash -ne $visibilityHash) { throw 'Refused visibility proof changed the startup file or reported a commit.' }
        if (@(Get-ChildItem -LiteralPath $visibilityRoot -Filter 'exe.xml.taxi-*').Count) { throw 'Refused visibility proof left temporary XML or a backup.' }
        if ($mode -eq 'refuse-original' -and -not $visibilityCalls.Contains($visibilityPath)) { throw 'Existing startup visibility was not checked.' }
        if ($mode -eq 'refuse-temporary' -and @($visibilityCalls | Where-Object { $_ -like ($visibilityPath + '.taxi-*.tmp') }).Count -ne 1) {
            throw 'The created startup sibling was not checked exactly once before writing content.'
        }
    }
    $visibilityMode = 'allow'
    $visibilityCalls.Clear()
    $writtenHash = ''
    $visibilityBackup = Save-TaxiLaunchXml $visibilityDocument $visibilityPath $visibilityHash ([ref]$writtenHash) -RequireVisiblePath
    if (-not $visibilityBackup -or (Get-FileHash -LiteralPath $visibilityBackup).Hash -ne $visibilityHash -or
        $writtenHash -ne (Get-FileHash -LiteralPath $visibilityPath).Hash) { throw 'Successful visibility proof lost exact backup or committed hash semantics.' }
    if (-not $visibilityCalls.Contains($visibilityPath) -or
        @($visibilityCalls | Where-Object { $_ -like ($visibilityPath + '.taxi-*.tmp') }).Count -ne 1) { throw 'Successful startup saving omitted an original or empty-sibling visibility check.' }

    # The common helper remains usable without requiring installer-specific
    # visibility machinery unless the installer explicitly requests that check.
    $visibilityMode = 'refuse-all'
    $visibilityCalls.Clear()
    $currentHash = $writtenHash
    $writtenHash = ''
    [void](Save-TaxiLaunchXml $visibilityDocument $visibilityPath $currentHash ([ref]$writtenHash))
    if ($visibilityCalls.Count -ne 0 -or $writtenHash -ne (Get-FileHash -LiteralPath $visibilityPath).Hash) { throw 'Default direct XML saving changed its optional visibility contract.' }

    $visibilityPath = Join-Path $visibilityRoot 'new/exe.xml'
    $visibilityDocument = Read-TaxiLaunchXml $visibilityPath
    Set-TaxiStartupEntry $visibilityDocument 'C:\Native Camera\taxi-cam.exe' 'C:\MSFS\FlightSimulator2024.exe'
    $visibilityMode = 'refuse-temporary'
    $visibilityCalls.Clear()
    $failure = $null; $writtenHash = 'not committed'
    try { Save-TaxiLaunchXml $visibilityDocument $visibilityPath '' ([ref]$writtenHash) -RequireVisiblePath } catch { $failure = $_ }
    if (-not $failure -or $failure.Exception.Message -ne 'Fixture detects a redirected startup replacement.' -or
        $writtenHash -or [IO.File]::Exists($visibilityPath)) { throw 'A refused new-file visibility proof created or reported startup XML.' }
    if (@(Get-ChildItem -LiteralPath (Split-Path -Parent $visibilityPath) -File).Count) { throw 'A refused new-file visibility proof left a sibling or backup.' }
    if ($visibilityCalls.Count -ne 1 -or $visibilityCalls[0] -notlike ($visibilityPath + '.taxi-*.tmp')) { throw 'A new startup transaction did not check only its created empty sibling.' }
}
Write-Output 'PASS exe.xml path visibility: original and empty sibling checked before content, redirected-path refusals leave original/new targets unchanged with no commit/backup/temp, successful opt-in and unchanged default saving.'

# Real EFS coverage is opt-in through tests/installer/test-encryption.ps1 -RunEfsFixture.
# That fixture requires an existing EFS key; this default test must not cause
# Windows to create a personal encryption key on supported developer or CI hosts.
