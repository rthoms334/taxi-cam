[CmdletBinding()]
param([switch]$RunEfsFixture)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Requires build/native/setup-host-test.exe from build.ps1 -Validate. By default this only checks
# prerequisites. -RunEfsFixture still requires an existing current EFS key; it
# never enrolls a certificate, changes machine policy, or touches simulator data.
# Prefer a disposable Windows test account already configured for EFS.
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$testRoot = Join-Path $repo ('build/encryption-tests/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null
$reportPath = Join-Path $testRoot 'result.json'
$report = [ordered]@{
    timestampUtc = [DateTime]::UtcNow.ToString('o')
    powershell = $PSVersionTable.PSVersion.ToString()
    fixtureRoot = $testRoot
    status = 'CHECKING'
    reason = ''
    filesystem = ([IO.DriveInfo]::new([IO.Path]::GetPathRoot($testRoot))).DriveFormat
    efsCertificates = 0
    usableEfsCertificates = 0
    currentEfsKeyAvailable = $false
    copyToDisabledDirectory = $null
    startupUpdate = $null
}
function Write-Result([string]$Status, [string]$Reason) {
    $report.status = $Status
    $report.reason = $Reason
    $report | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding utf8
    Write-Output ($Status + ': ' + $Reason)
    Write-Output ('Receipt: ' + $reportPath)
}
function Get-FixtureFile([string]$Path) {
    $attributes = [IO.File]::GetAttributes($Path)
    return [ordered]@{
        path = $Path
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
        attributes = $attributes.ToString()
        encrypted = ($attributes -band [IO.FileAttributes]::Encrypted) -ne 0
    }
}
function Get-Failure([Management.Automation.ErrorRecord]$Failure) {
    $exception = $Failure.Exception
    while ($exception.InnerException) { $exception = $exception.InnerException }
    return [ordered]@{
        message = $exception.Message
        hresult = ('0x{0:X8}' -f $exception.HResult)
        win32Code = $exception.HResult -band 0xffff
        exceptionType = $exception.GetType().FullName
    }
}
try {
    if ($report.filesystem -ne 'NTFS') {
        Write-Result 'SKIPPED' 'The fixture volume is not NTFS; no EFS experiment was attempted.'
        return
    }
    # Read the standard EKU extension directly; PowerShell's display properties
    # differ between versions. Count only EFS certificates, never export keys.
    $certificates = @(Get-ChildItem Cert:\CurrentUser\My | Where-Object {
        @($_.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.37' } |
            ForEach-Object { $_.EnhancedKeyUsages } | ForEach-Object { $_.Value }) -contains '1.3.6.1.4.1.311.10.3.4'
    })
    $now = Get-Date
    $usable = @($certificates | Where-Object { $_.HasPrivateKey -and $_.NotBefore -le $now -and $_.NotAfter -gt $now })
    $report.efsCertificates = $certificates.Count
    $report.usableEfsCertificates = $usable.Count
    if (-not $usable.Count) {
        Write-Result 'SKIPPED' 'No existing valid EFS certificate with a private key is available. EncryptFile could create personal credentials, so no file was encrypted and the Windows error was not reproduced.'
        return
    }
    # cipher /y is a read-only query. Requiring a matching current certificate
    # avoids treating an unrelated/unused certificate as EFS configuration.
    # https://learn.microsoft.com/windows-server/administration/windows-commands/cipher
    $currentKeyText = (& (Join-Path $env:SystemRoot 'System32/cipher.exe') /y 2>&1 | Out-String) -replace '\s', ''
    $currentKey = @($usable | Where-Object { $currentKeyText -match [Regex]::Escape($_.Thumbprint) })
    $report.currentEfsKeyAvailable = $currentKey.Count -gt 0
    if (-not $report.currentEfsKeyAvailable) {
        Write-Result 'SKIPPED' 'A valid certificate exists, but an existing current EFS key could not be verified. No encryption or key provisioning was attempted.'
        return
    }
    if (-not $RunEfsFixture) {
        Write-Result 'READY' 'Existing EFS credentials are available. Run with -RunEfsFixture to test only disposable repository fixtures.'
        return
    }
    $sourceDirectory = Join-Path $testRoot 'source'
    $disabledDirectory = Join-Path $testRoot 'encryption-disabled'
    New-Item -ItemType Directory -Path $sourceDirectory,$disabledDirectory | Out-Null
    $source = Join-Path $sourceDirectory 'exe.xml'
    $destination = Join-Path $disabledDirectory 'backup-0'
    [IO.File]::WriteAllText($source, '<?xml version="1.0"?><SimBase.Document Type="Launch"><Launch.Addon><Name>Other Addon</Name><Path>C:\Fixture\other.exe</Path></Launch.Addon></SimBase.Document>')
    [IO.File]::Encrypt($source)
    $original = Get-FixtureFile $source
    if (-not $original.encrypted) { throw 'The fixture did not acquire the EFS attribute.' }
    # This changes Desktop.ini in this disposable directory only. Microsoft
    # documents effects on EncryptFile/FileEncryptionStatus; CopyFile behaviour
    # is deliberately measured below, not assumed to fail with error 6000.
    # https://learn.microsoft.com/windows/win32/api/winefs/nf-winefs-encryptiondisable
    if (-not ('TaxiCamEfsFixture.Native' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace TaxiCamEfsFixture {
    public static class Native {
        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool EncryptionDisable(string directory, [MarshalAs(UnmanagedType.Bool)] bool disable);
    }
}
'@
    }
    if (-not [TaxiCamEfsFixture.Native]::EncryptionDisable($disabledDirectory, $true)) {
        throw [ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error())
    }
    $copy = [ordered]@{ source = $original; destination = $destination; succeeded = $false; error = $null; error6000Reproduced = $false }
    try {
        Copy-Item -LiteralPath $source -Destination $destination
        $copy.succeeded = $true
        $copy.destination = Get-FixtureFile $destination
    } catch {
        $copy.error = Get-Failure $_
        $copy.error6000Reproduced = $copy.error.win32Code -eq 6000
    }
    $report.copyToDisabledDirectory = $copy
    if ((Get-FileHash -LiteralPath $source).Hash -ne $original.sha256) { throw 'The copy experiment changed the source fixture.' }
    # Configure startup through the companion's setup commands (test host build).
    . (Join-Path $repo 'tests/support/installer_fixture.ps1')
    $sim = Join-Path $testRoot 'sim'
    New-TaxiFixtureImage (Join-Path $sim 'FlightSimulator2024.exe') $false
    $state = Join-Path $testRoot 'state'
    $env:TAXI_SETUP_TEST_PROCESSES = '-'
    try {
        $process = Start-Process -FilePath (Join-Path $repo 'build/native/setup-host-test.exe') -ArgumentList @('configure','--destination',('"' + (Join-Path $testRoot 'app') + '"'),
            '--state',('"' + $state + '"'),'--simulator',('"' + $sim + '"'),'--startup','automatic','--exe-xml',('"' + $source + '"')) -WindowStyle Hidden -Wait -PassThru
    } finally { Remove-Item Env:\TAXI_SETUP_TEST_PROCESSES -ErrorAction SilentlyContinue }
    if ($process.ExitCode -ne 0) { throw 'The setup command test host failed to configure startup.' }
    $record = Get-Content -Raw -LiteralPath (Join-Path $testRoot 'app/installation.json') | ConvertFrom-Json
    if (-not $record.startupUpdated) { throw "The encrypted startup update fell back: $($record.startupError)" }
    $updated = Get-FixtureFile $source
    $backupFile = Get-FixtureFile $record.exeXmlBackup
    $report.startupUpdate = [ordered]@{ source = $updated; backup = $backupFile; writtenHash = $record.exeXmlInstalledHash }
    if (-not $updated.encrypted -or -not $backupFile.encrypted) { throw 'The startup update did not preserve encryption on both files.' }
    if ($backupFile.sha256 -ne $original.sha256) { throw 'The encrypted sibling backup bytes changed.' }
    if ($updated.sha256 -ne $record.exeXmlInstalledHash) { throw 'The committed startup hash does not match.' }
    [xml]$after = Get-Content -Raw -LiteralPath $source
    if ($after.SelectNodes('//Launch.Addon').Count -ne 2) { throw 'The startup update did not preserve the existing addon.' }
    if ($copy.error6000Reproduced) {
        Write-Result 'REPRODUCED' 'Windows returned actual error 6000 while copying the encrypted fixture; the sibling startup update preserved original backup bytes and encryption.'
    } else {
        Write-Result 'NOT_REPRODUCED' 'The encrypted startup update preserved encryption and backup bytes, but the copy experiment did not reproduce Windows error 6000. Inspect the measured result.'
    }
} catch {
    $report['failure'] = Get-Failure $_
    Write-Result 'FAILED' 'The EFS experiment could not finish. This is not a successful reproduction or validation.'
    throw
}
