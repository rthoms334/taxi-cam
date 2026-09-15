param([string]$OutputDirectory, [string]$CurrentVersion, [uint32]$CurrentBuild = 0)

# Embedded in the companion; never executes release-provided code or command text.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:Repository = 'rthoms334/taxi-cam'
$script:DownloadLimit = 256MB

function ConvertTo-UpdateVersion([string]$Tag) {
    if ($Tag -cnotmatch '^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-build\.(0|[1-9][0-9]*)$') {
        throw 'Invalid release version.'
    }
    $numbers = @($Matches[1], $Matches[2], $Matches[3], $Matches[4])
    return ,([uint32[]]($numbers | ForEach-Object { [uint32]::Parse($_) }))
}

function Test-NewerUpdate([uint32[]]$Candidate, [uint32[]]$Current) {
    for ($i = 0; $i -lt 4; $i++) {
        if ($Candidate[$i] -ne $Current[$i]) { return $Candidate[$i] -gt $Current[$i] }
    }
    return $false
}

function Assert-DownloadUri([string]$Url) {
    $uri = [uri]$Url
    if (-not $uri.IsAbsoluteUri -or $uri.Scheme -cne 'https' -or $uri.Port -ne 443 -or
        $uri.UserInfo -or $uri.Fragment -or
        $uri.DnsSafeHost -cnotin @('api.github.com', 'github.com', 'objects.githubusercontent.com', 'release-assets.githubusercontent.com')) {
        throw 'Untrusted download URL.'
    }
    return $uri
}

function Select-UpdateAsset($Release, [uint32[]]$Current) {
    if ($Release.draft -isnot [bool] -or $Release.prerelease -isnot [bool] -or $Release.draft -or $Release.prerelease) {
        throw 'Release is not a stable published release.'
    }
    $tag = [string]$Release.tag_name
    $version = ConvertTo-UpdateVersion $tag
    if (-not (Test-NewerUpdate $version $Current)) { return $null }
    $name = 'taxi-cam-' + ($version[0..2] -join '.') + '-windows-x64-setup.exe'
    $legacyName = 'taxi-cam-' + $tag.Substring(1) + '-windows-x64-setup.exe'
    $assets = @($Release.assets | Where-Object { $_.name -ceq $name })
    $legacyAssets = @($Release.assets | Where-Object { $_.name -ceq $legacyName })
    if ($assets.Count -gt 1 -or $legacyAssets.Count -gt 1) { throw 'Duplicate Windows installer assets.' }
    # An advertised clean name is authoritative, even when its metadata is bad.
    # Only releases without it can use the exact older build-number filename.
    if ($assets.Count -eq 0) { $name = $legacyName; $assets = $legacyAssets }
    if ($assets.Count -ne 1) { throw 'Expected one exact Windows installer asset.' }
    $asset = $assets[0]
    $expected = "https://github.com/$script:Repository/releases/download/$tag/$name"
    if ($asset.browser_download_url -cne $expected -or $asset.state -cne 'uploaded' -or
        $asset.size -isnot [ValueType] -or [double]$asset.size -ne [math]::Truncate([double]$asset.size) -or
        $asset.size -le 0 -or $asset.size -gt $script:DownloadLimit) {
        throw 'Invalid installer asset metadata.'
    }
    $null = Assert-DownloadUri $asset.browser_download_url
    $digest = $null
    if ($asset.PSObject.Properties['digest'] -and $null -ne $asset.digest -and [string]$asset.digest -ne '') {
        if ([string]$asset.digest -cnotmatch '^sha256:([0-9a-fA-F]{64})$') { throw 'Invalid asset digest.' }
        $digest = $Matches[1].ToLowerInvariant()
    }
    $sums = $null
    if (-not $digest) {
        $sumsAssets = @($Release.assets | Where-Object { $_.name -ceq 'SHA256SUMS.txt' })
        if ($sumsAssets.Count -ne 1) { throw 'No unambiguous SHA256 checksum asset.' }
        $sums = $sumsAssets[0]
        if ($sums.browser_download_url -cne "https://github.com/$script:Repository/releases/download/$tag/SHA256SUMS.txt" -or
            $sums.state -cne 'uploaded' -or $sums.size -le 0 -or $sums.size -gt 1MB) {
            throw 'Invalid checksum asset.'
        }
    }
    return [pscustomobject]@{ Tag = $tag; Name = $name; Url = $expected; Size = [long]$asset.size; Digest = $digest; Sums = $sums }
}

function Read-InstallerChecksum([string]$Text, [string]$Name) {
    $found = @()
    foreach ($line in ($Text -split '\r?\n')) {
        if ($line -cmatch '^([0-9a-fA-F]{64}) [ *](.+)$' -and $Matches[2] -ceq $Name) {
            $found += $Matches[1].ToLowerInvariant()
        }
    }
    if ($found.Count -ne 1) { throw 'Missing or ambiguous installer checksum.' }
    return $found[0]
}

function Receive-BoundedDownload([string]$Url, [long]$Limit, [string]$Path = '') {
    $uri = Assert-DownloadUri $Url
    $handler = [System.Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect = $false
    $client = [System.Net.Http.HttpClient]::new($handler)
    $client.Timeout = [TimeSpan]::FromSeconds(30)
    $client.DefaultRequestHeaders.UserAgent.ParseAdd('Taxi-Cam-Updater/1.0')
    $client.DefaultRequestHeaders.Accept.ParseAdd('application/vnd.github+json')
    $cancel = [Threading.CancellationTokenSource]::new([TimeSpan]::FromSeconds(120))
    $response = $null
    $stream = $null
    $output = $null
    try {
        for ($redirect = 0; $redirect -le 5; $redirect++) {
            $response = $client.GetAsync($uri, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead, $cancel.Token).GetAwaiter().GetResult()
            $status = [int]$response.StatusCode
            if ($status -in @(301, 302, 303, 307, 308)) {
                if ($redirect -eq 5 -or -not $response.Headers.Location) { throw 'Too many or invalid redirects.' }
                $uri = Assert-DownloadUri ([uri]::new($uri, $response.Headers.Location).AbsoluteUri)
                $response.Dispose()
                $response = $null
                continue
            }
            if ($status -ne 200) { throw "Release download returned HTTP $status." }
            break
        }
        if ($null -ne $response.Content.Headers.ContentLength -and $response.Content.Headers.ContentLength -gt $Limit) {
            throw 'Download exceeds limit.'
        }
        $stream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        if ($Path) { $output = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None) }
        else { $output = [IO.MemoryStream]::new() }
        $buffer = [byte[]]::new(65536)
        [long]$total = 0
        while (($read = $stream.ReadAsync($buffer, 0, $buffer.Length, $cancel.Token).GetAwaiter().GetResult()) -gt 0) {
            $total += $read
            if ($total -gt $Limit) { throw 'Download exceeds limit.' }
            $output.Write($buffer, 0, $read)
        }
        if ($total -eq 0) { throw 'Empty download.' }
        if ($Path) { return $total }
        return [Text.Encoding]::UTF8.GetString($output.ToArray())
    } finally {
        if ($output) { $output.Dispose() }
        if ($stream) { $stream.Dispose() }
        if ($response) { $response.Dispose() }
        $cancel.Dispose()
        $client.Dispose()
        $handler.Dispose()
    }
}

function Invoke-UpdateCheck {
    $current = ConvertTo-UpdateVersion "v$CurrentVersion-build.$CurrentBuild"
    if (-not [IO.Directory]::Exists($OutputDirectory)) { throw 'Missing update cache.' }
    Add-Type -AssemblyName System.Net.Http
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $release = (Receive-BoundedDownload "https://api.github.com/repos/$script:Repository/releases/latest" 1MB) | ConvertFrom-Json
    $asset = Select-UpdateAsset $release $current
    $resultPath = Join-Path $OutputDirectory 'result.txt'
    if (-not $asset) {
        [IO.File]::WriteAllText($resultPath, "current`n", [Text.Encoding]::ASCII)
        return
    }
    $digest = $asset.Digest
    if (-not $digest) {
        $sums = Receive-BoundedDownload $asset.Sums.browser_download_url 1MB
        $digest = Read-InstallerChecksum $sums $asset.Name
    }
    $partial = Join-Path $OutputDirectory 'setup.exe.partial'
    $total = Receive-BoundedDownload $asset.Url $script:DownloadLimit $partial
    if ($total -ne $asset.Size -or (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ine $digest) {
        throw 'Installer integrity check failed.'
    }
    [IO.File]::Move($partial, (Join-Path $OutputDirectory 'setup.exe'))
    [IO.File]::WriteAllText($resultPath, "ready`n$($asset.Tag)`n$digest`n", [Text.Encoding]::ASCII)
}

if ($MyInvocation.InvocationName -ne '.') {
    try { Invoke-UpdateCheck; exit 0 } catch { exit 1 }
}
