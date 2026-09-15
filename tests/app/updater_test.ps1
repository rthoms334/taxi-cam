$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
Set-StrictMode -Version Latest
. (Join-Path $repoRoot 'src/app/update-check.ps1')
$script:Count = 0
function Assert([bool]$Condition, [string]$Name) {
    if (-not $Condition) { throw "FAIL: $Name" }
    $script:Count++
}
function Reject([scriptblock]$Action, [string]$Name) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Assert $rejected $Name
}
function Release {
    param([switch]$Legacy)
    $name = if ($Legacy) { 'taxi-cam-0.8.0-build.12-windows-x64-setup.exe' } else { 'taxi-cam-0.8.0-windows-x64-setup.exe' }
    return [pscustomobject]@{
        draft=$false; prerelease=$false; tag_name='v0.8.0-build.12'; assets=@([pscustomobject]@{
            name=$name; state='uploaded'; size=1234;
            browser_download_url="https://github.com/rthoms334/taxi-cam/releases/download/v0.8.0-build.12/$name";
            digest=('sha256:' + ('a' * 64))
        })
    }
}
$current = ConvertTo-UpdateVersion 'v0.8.0-build.9'
Assert (Test-NewerUpdate (ConvertTo-UpdateVersion 'v0.8.0-build.10') $current) 'Numeric build ordering'
Assert (Test-NewerUpdate (ConvertTo-UpdateVersion 'v0.8.1-build.1') $current) 'Patch version before build'
Assert (Test-NewerUpdate (ConvertTo-UpdateVersion 'v0.9.0-build.1') $current) 'Version before build'
Assert (-not (Test-NewerUpdate $current $current)) 'No reinstall'
foreach ($tag in @('v0.8.0-build.01','v0.8.0-build.-1','v0.8.0-build.4294967296','v0.8.0-build.10;calc','v0.8.0-build.10-preview')) {
    Reject { ConvertTo-UpdateVersion $tag } 'Malformed tag'
}
$release = Release
$selected = Select-UpdateAsset $release $current
Assert ($selected.Digest -ceq ('a' * 64)) 'GitHub asset digest selected'
Assert ($selected.Name -ceq 'taxi-cam-0.8.0-windows-x64-setup.exe' -and $selected.Url -ceq $release.assets[0].browser_download_url) 'Clean-only installer selected'
Assert ($null -eq (Select-UpdateAsset $release (ConvertTo-UpdateVersion 'v0.9.0-build.1'))) 'Older release ignored'
$legacy = Release -Legacy
$selected = Select-UpdateAsset $legacy $current
Assert ($selected.Name -ceq 'taxi-cam-0.8.0-build.12-windows-x64-setup.exe' -and $selected.Url -ceq $legacy.assets[0].browser_download_url) 'Exact legacy-only installer supported'
foreach ($cleanFirst in @($true, $false)) {
    $release = Release
    $legacy = Release -Legacy
    $legacy.assets[0].digest = 'sha256:' + ('c' * 64)
    $release.assets = if ($cleanFirst) { @($release.assets[0], $legacy.assets[0]) } else { @($legacy.assets[0], $release.assets[0]) }
    $selected = Select-UpdateAsset $release $current
    Assert ($selected.Name -ceq 'taxi-cam-0.8.0-windows-x64-setup.exe' -and $selected.Digest -ceq ('a' * 64)) 'Clean installer preferred regardless of asset order'
}
foreach ($field in @('draft', 'prerelease')) {
    $release = Release
    $release.$field = $true
    Reject { Select-UpdateAsset $release $current } 'Draft/prerelease rejected'
}
$release = Release
$release.assets += $release.assets[0]
Reject { Select-UpdateAsset $release $current } 'Duplicate installers rejected'
foreach ($legacyDuplicate in @($true, $false)) {
    $release = Release
    $legacy = Release -Legacy
    $duplicate = if ($legacyDuplicate) { $legacy.assets[0] } else { $release.assets[0] }
    $release.assets = @($release.assets[0], $legacy.assets[0], $duplicate)
    Reject { Select-UpdateAsset $release $current } 'Duplicate exact installer names rejected when both styles present'
}
$release = Release -Legacy
$release.assets += $release.assets[0]
Reject { Select-UpdateAsset $release $current } 'Duplicate legacy-only installers rejected'
foreach ($legacyStyle in @($true, $false)) {
    $release = Release -Legacy:$legacyStyle
    $release.assets[0].name = $release.assets[0].name.Replace('0.8.0', '0.8.1')
    $release.assets[0].browser_download_url = $release.assets[0].browser_download_url.Replace('0.8.0', '0.8.1')
    Reject { Select-UpdateAsset $release $current } 'Wrong semantic-version filename rejected'
    foreach ($urlChange in @('wrong-build', 'wrong-version', 'wrong-repository', 'query')) {
        $release = Release -Legacy:$legacyStyle
        $release.assets[0].browser_download_url = switch ($urlChange) {
            'wrong-build' { $release.assets[0].browser_download_url.Replace('/v0.8.0-build.12/', '/v0.8.0-build.13/') }
            'wrong-version' { $release.assets[0].browser_download_url.Replace('/v0.8.0-build.12/', '/v0.8.1-build.12/') }
            'wrong-repository' { $release.assets[0].browser_download_url.Replace('/rthoms334/taxi-cam/', '/other/taxi-cam/') }
            'query' { $release.assets[0].browser_download_url + '?other=true' }
        }
        Reject { Select-UpdateAsset $release $current } 'Both filename styles require exact version, build and repository URL'
    }
}
$release = Release -Legacy
$release.assets[0].name = $release.assets[0].name.Replace('build.12', 'build.13')
$release.assets[0].browser_download_url = $release.assets[0].browser_download_url.Replace('build.12-windows', 'build.13-windows')
Reject { Select-UpdateAsset $release $current } 'Wrong legacy build filename rejected'
foreach ($badClean in @('url', 'state', 'size', 'digest', 'missing-checksum')) {
    $release = Release
    switch ($badClean) {
        'url' { $release.assets[0].browser_download_url += '?other=true' }
        'state' { $release.assets[0].state = 'new' }
        'size' { $release.assets[0].size = 257MB }
        'digest' { $release.assets[0].digest = 'md5:abcd' }
        'missing-checksum' { $release.assets[0].digest = $null }
    }
    $release.assets += (Release -Legacy).assets[0]
    Reject { Select-UpdateAsset $release $current } 'Invalid clean asset never falls back to valid legacy asset'
}
foreach ($url in @('http://github.com/a','https://github.com.evil.example/a','https://github.com@evil.example/a','https://github.com:444/a','file:///c:/a','https://github.com/a#b')) {
    Reject { Assert-DownloadUri $url } 'Unsafe transport rejected'
}
$release = Release
$release.assets[0].browser_download_url += '?other=true'
Reject { Select-UpdateAsset $release $current } 'Non-exact installer URL rejected'
$release = Release
$release.assets[0].size = 257MB
Reject { Select-UpdateAsset $release $current } 'Oversized installer rejected'
$release = Release
$release.assets[0].digest = 'md5:abcd'
Reject { Select-UpdateAsset $release $current } 'Malformed digest cannot fallback'
$release = Release
$release.assets[0].digest = $null
Reject { Select-UpdateAsset $release $current } 'No digest or checksum rejected'
$release.assets += [pscustomobject]@{ name='SHA256SUMS.txt'; state='uploaded'; size=100; browser_download_url='https://github.com/rthoms334/taxi-cam/releases/download/v0.8.0-build.12/SHA256SUMS.txt' }
$release.assets += (Release -Legacy).assets[0]
$selected = Select-UpdateAsset $release $current
Assert ($selected.Sums.name -ceq 'SHA256SUMS.txt' -and -not $selected.Digest) 'Clean checksum fallback preferred over legacy asset digest'
Assert ($selected.Name -ceq 'taxi-cam-0.8.0-windows-x64-setup.exe') 'Checksum fallback retains clean filename'
$line = ('b' * 64) + '  ' + $selected.Name
Assert ((Read-InstallerChecksum $line $selected.Name) -ceq ('b' * 64)) 'Exact checksum filename'
Assert ((Read-InstallerChecksum ((('c' * 64) + '  taxi-cam-0.8.0-build.12-windows-x64-setup.exe') + "`n$line") $selected.Name) -ceq ('b' * 64)) 'Clean checksum selected independently of legacy checksum'
Reject { Read-InstallerChecksum (('c' * 64) + '  taxi-cam-0.8.0-build.12-windows-x64-setup.exe') $selected.Name } 'Legacy checksum cannot validate clean installer'
Reject { Read-InstallerChecksum "$line`n$line" $selected.Name } 'Duplicate checksum rejected'
Reject { Read-InstallerChecksum ($line + '.other') $selected.Name } 'Similar filename rejected'
Write-Output "Updater release selection checks passed: $script:Count"
