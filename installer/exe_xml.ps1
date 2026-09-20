Set-StrictMode -Version Latest
function Read-TaxiLaunchXml([string]$Path, [switch]$RepairLaunchHeader) {
    $document = [Xml.XmlDocument]::new()
    $document.PreserveWhitespace = $true
    $document.XmlResolver = $null
    if (Test-Path -LiteralPath $Path) {
        $settings = [Xml.XmlReaderSettings]::new()
        $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
        $settings.XmlResolver = $null
        $reader = [Xml.XmlReader]::Create($Path, $settings)
        try { $document.Load($reader) } finally { $reader.Dispose() }
        $root = $document.DocumentElement
        if ($RepairLaunchHeader -and $root.Name -ceq 'SimBase.Document' -and $root.GetAttribute('Type') -ceq 'SimConnect' -and
            -not $root.NamespaceURI) {
            # Some add-ons write launch entries under a copied SimConnect header.
            # Repair only that recognizable launch-only shape, in memory. A real
            # or mixed SimConnect configuration must never become a launch file.
            $allowed = @('Descr','Filename','Disabled','Launch.ManualLoad','Launch.Addon')
            $elements = @($root.ChildNodes | Where-Object { $_.NodeType -eq [Xml.XmlNodeType]::Element })
            # LocalName avoids PowerShell's XML adapter substituting an add-on's
            # child <Name> for the element's CLR Name property.
            $unknown = @($elements | Where-Object { $allowed -cnotcontains $_.LocalName -or $_.NamespaceURI })
            $ambiguous = @($allowed | Where-Object { $_ -ne 'Launch.Addon' -and $root.SelectNodes($_).Count -gt 1 })
            $complexHeaders = @($elements | Where-Object { $_.LocalName -in @('Descr','Filename') } | ForEach-Object {
                $_.ChildNodes | Where-Object { $_.NodeType -notin @([Xml.XmlNodeType]::Text, [Xml.XmlNodeType]::CDATA,
                    [Xml.XmlNodeType]::Whitespace, [Xml.XmlNodeType]::SignificantWhitespace) }
            })
            $filename = $root.SelectSingleNode('Filename')
            # Either a copied SimConnect.xml header or an exe.xml that only its
            # Type attribute misnames (some add-on managers write that shape).
            if ($unknown.Count -eq 0 -and $ambiguous.Count -eq 0 -and $complexHeaders.Count -eq 0 -and $root.SelectNodes('Launch.Addon').Count -gt 0 -and
                $filename -and $filename.InnerText -cin @('SimConnect.xml', 'exe.xml')) {
                $root.SetAttribute('Type','Launch')
                if ($filename.InnerText -ceq 'SimConnect.xml') {
                    $filename.InnerText = 'exe.xml'
                    $description = $root.SelectSingleNode('Descr')
                    if (-not $description) { $description = $document.CreateElement('Descr'); [void]$root.PrependChild($description) }
                    $description.InnerText = 'Launch'
                }
                # An exe.xml keeps its own description. Disabled/ManualLoad flags,
                # add-on entries and comments stay as supplied. Save-TaxiLaunchXml
                # backs up original bytes atomically.
            }
        }
        if ($document.DocumentElement.Name -ne 'SimBase.Document' -or $document.DocumentElement.GetAttribute('Type') -ne 'Launch') { throw 'Unrecognized exe.xml launch document.' }
    } else {
        $document.LoadXml('<?xml version="1.0" encoding="utf-8"?><SimBase.Document Type="Launch" version="1,0"><Descr>Launch</Descr><Filename>exe.xml</Filename><Disabled>False</Disabled><Launch.ManualLoad>False</Launch.ManualLoad></SimBase.Document>')
    }
    return ,$document
}
function Set-TaxiStartupEntry([Xml.XmlDocument]$Document, [string]$Executable, [string]$Simulator, [switch]$Remove) {
    if (-not $Remove -and (-not [IO.Path]::IsPathRooted($Executable) -or -not [IO.Path]::IsPathRooted($Simulator))) { throw 'Startup executable paths must be absolute.' }
    $root = $Document.DocumentElement
    # Recognize the former name so an upgrade replaces its entry in place.
    $matches = @($root.SelectNodes('Launch.Addon') | Where-Object { $node=$_.SelectSingleNode('Name'); $null -ne $node -and $node.InnerText -in @('Taxi Cam','380 Taxi Cam') })
    if ($matches.Count -gt 1) { throw 'Multiple Taxi Cam startup entries found; refusing an ambiguous update.' }
    if ($Remove) { foreach ($entry in $matches) { [void]$root.RemoveChild($entry) }; return }
    $entry = if ($matches.Count) { $matches[0] } else { $Document.CreateElement('Launch.Addon') }
    foreach ($pair in @(
        @('Name', 'Taxi Cam'), @('Disabled', 'False'), @('ManualLoad', 'False'),
        @('Path', $Executable), @('CommandLine', ('--background --simulator "' + $Simulator + '"')), @('NewConsole', 'False')
    )) {
        $node = $entry.SelectSingleNode($pair[0])
        if (-not $node) { $node = $Document.CreateElement($pair[0]); [void]$entry.AppendChild($node) }
        $node.InnerText = $pair[1]
    }
    if (-not $entry.ParentNode) { [void]$root.AppendChild($entry) }
}
function New-TaxiStartupConflict([string]$Message) {
    $exception = [InvalidOperationException]::new($Message)
    $exception.Data['TaxiStartupConflict'] = $true
    return $exception
}
function Save-TaxiLaunchXml([Xml.XmlDocument]$Document,[string]$Path,[string]$ExpectedHash,[ref]$WrittenHash,[switch]$RequireVisiblePath) {
    if ($null -ne $WrittenHash) { $WrittenHash.Value = '' }
    $absolute = [IO.Path]::GetFullPath($Path)
    if ([IO.Path]::GetFileName($absolute) -ine 'exe.xml') { throw 'Startup target must be named exe.xml.' }
    $parent = Split-Path -Parent $absolute
    $temporary = $null
    $backup = $null
    $backupVerified = $false
    $encrypted = $null
    $committed = $false
    $operation = 'Prepare startup directory'
    $operationSource = $absolute
    $operationDestination = $parent
    try {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
        $operation = 'Verify original startup file'
        $operationDestination = $absolute
        if (Test-Path -LiteralPath $absolute) {
            if (-not $ExpectedHash -or (Get-FileHash -LiteralPath $absolute).Hash -ne $ExpectedHash) {
                throw (New-TaxiStartupConflict 'exe.xml changed during installation; no startup entry was written.')
            }
            $encrypted = ([IO.File]::GetAttributes($absolute) -band [IO.FileAttributes]::Encrypted) -ne 0
        } elseif ($ExpectedHash) { throw (New-TaxiStartupConflict 'exe.xml disappeared during installation.') }
        $temporary = Join-Path $parent ('exe.xml.taxi-' + [Guid]::NewGuid().ToString('N') + '.tmp')
        # Reserve a private sibling and apply EFS before writing XML content. Never
        # copy encrypted user configuration into an unencrypted temporary folder.
        $operation = 'Create startup replacement'
        $operationDestination = $temporary
        $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
        $stream.Dispose()
        if ($RequireVisiblePath) {
            # A packaged host can redirect Roaming AppData independently of the
            # binary directory. Verify the actual destination before any XML is
            # written/backed up, so a private copy cannot report auto-start success.
            $operation = 'Verify startup path visibility'
            Assert-TaxiVisibleInstallPath $temporary
            if (Test-Path -LiteralPath $absolute) { Assert-TaxiVisibleInstallPath $absolute }
        }
        if ($encrypted) {
            $operation = 'Encrypt startup replacement'
            [IO.File]::Encrypt($temporary)
            if (([IO.File]::GetAttributes($temporary) -band [IO.FileAttributes]::Encrypted) -eq 0) {
                throw 'Could not preserve exe.xml encryption on the replacement file.'
            }
        }
        $settings = [Xml.XmlWriterSettings]::new()
        $settings.Encoding = [Text.UTF8Encoding]::new($false)
        $settings.Indent = $true
        $settings.NewLineHandling = [Xml.NewLineHandling]::None
        $operation = 'Write startup replacement'
        $writer = [Xml.XmlWriter]::Create($temporary, $settings)
        try { $Document.Save($writer) } finally { $writer.Dispose() }
        $operation = 'Verify startup replacement'
        [void](Read-TaxiLaunchXml $temporary)
        $preparedHash = (Get-FileHash -LiteralPath $temporary).Hash
        if ($ExpectedHash) {
            $operation = 'Verify original startup file before backup'
            $operationDestination = $absolute
            if (-not (Test-Path -LiteralPath $absolute) -or (Get-FileHash -LiteralPath $absolute).Hash -ne $ExpectedHash) {
                throw (New-TaxiStartupConflict 'exe.xml changed before backup.')
            }
            $backup = $absolute + '.taxi-backup-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N')
            $operation = 'Back up startup file'
            $operationDestination = $backup
            Copy-Item -LiteralPath $absolute -Destination $backup
            $operation = 'Verify startup backup'
            if ((Get-FileHash -LiteralPath $backup).Hash -ne $ExpectedHash) { throw (New-TaxiStartupConflict 'Startup backup verification failed.') }
            if ($encrypted -and ([IO.File]::GetAttributes($backup) -band [IO.FileAttributes]::Encrypted) -eq 0) {
                throw 'Could not preserve exe.xml encryption on the backup file.'
            }
            $backupVerified = $true
            $operation = 'Verify original startup file before replacement'
            $operationDestination = $absolute
            if ((Get-FileHash -LiteralPath $absolute).Hash -ne $ExpectedHash) { throw (New-TaxiStartupConflict 'exe.xml changed before replacement.') }
            if ((([IO.File]::GetAttributes($absolute) -band [IO.FileAttributes]::Encrypted) -ne 0) -ne $encrypted) {
                throw (New-TaxiStartupConflict 'exe.xml encryption changed before replacement.')
            }
            $operation = 'Replace startup file'
            $operationSource = $temporary
            [IO.File]::Replace($temporary, $absolute, [NullString]::Value)
        } else {
            $operation = 'Create startup file'
            $operationSource = $temporary
            $operationDestination = $absolute
            if (Test-Path -LiteralPath $absolute) { throw (New-TaxiStartupConflict 'exe.xml appeared during installation; no startup entry was written.') }
            [IO.File]::Move($temporary, $absolute)
        }
        $committed = $true
        # Nothing that accesses the filesystem may fail after the atomic commit.
        # Callers use this prepared digest to distinguish our write from later edits.
        if ($null -ne $WrittenHash) { $WrittenHash.Value = $preparedHash }
        return $backup
    } catch {
        $failure = $_
        $failure.Exception.Data['TaxiStartupOperation'] = $operation
        $failure.Exception.Data['TaxiStartupSource'] = $operationSource
        $failure.Exception.Data['TaxiStartupDestination'] = $operationDestination
        if ($backup -and -not $backupVerified) {
            try {
                # Copy can leave partial or plaintext data even when it fails.
                # Delete only this invocation's unverified backup before fallback.
                # File.Delete also handles short paths and a missing file safely.
                [IO.File]::Delete($backup)
            } catch {
                $cleanupFailure = [InvalidOperationException]::new(
                    "Could not remove unverified startup backup '$backup': $($_.Exception.Message)", $failure.Exception)
                $cleanupFailure.Data['TaxiStartupUnsafe'] = $true
                $cleanupFailure.Data['TaxiStartupOperation'] = 'Remove unverified startup backup'
                $cleanupFailure.Data['TaxiStartupSource'] = $backup
                $cleanupFailure.Data['TaxiStartupDestination'] = $backup
                throw $cleanupFailure
            }
        }
        if (-not $failure.Exception.Data['TaxiStartupConflict']) {
            $unchanged = $false
            if (-not $committed) {
                try {
                    if ($ExpectedHash) {
                        $unchanged = (Test-Path -LiteralPath $absolute -PathType Leaf) -and (Get-FileHash -LiteralPath $absolute).Hash -eq $ExpectedHash
                        if ($unchanged -and $null -ne $encrypted) {
                            $unchanged = (([IO.File]::GetAttributes($absolute) -band [IO.FileAttributes]::Encrypted) -ne 0) -eq $encrypted
                        }
                    } else { $unchanged = -not (Test-Path -LiteralPath $absolute) }
                } catch { $unchanged = $false }
            }
            if (-not $unchanged) { $failure.Exception.Data['TaxiStartupUnsafe'] = $true }
        }
        throw $failure
    } finally {
        # Cleanup is best effort and cannot turn an already committed write into
        # a recoverable startup error. Only verified sibling backups are retained;
        # failed backup cleanup above must prevent the manual-startup fallback.
        if ($temporary -and [IO.File]::Exists($temporary)) {
            try { Remove-Item -LiteralPath $temporary -ErrorAction Stop } catch { }
        }
    }
}
