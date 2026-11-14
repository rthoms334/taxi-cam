[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Package,
    [Parameter(Mandatory=$true)][string]$Installer,
    [Parameter(Mandatory=$true)][ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')][string]$Repository,
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-fA-F]{40}$')][string]$Commit,
    [Parameter(Mandatory=$true)][ValidateRange(1,2147483647)][int]$BuildNumber,
    [Parameter(Mandatory=$true)][ValidatePattern('^[1-9][0-9]*$')][string]$BuildRunId
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:GITHUB_REF -ne 'refs/heads/main') { throw 'Release publication runs only in the main-branch workflow.' }
if ($BuildRunId -ne $env:GITHUB_RUN_ID) { throw 'Release publication must use artifacts from this workflow run.' }
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'release-notes.ps1')
. (Join-Path $PSScriptRoot 'publish-tag.ps1')
. (Join-Path $root 'installer/validation_receipt.ps1')
$receiptPath = Join-Path $root 'build/native/validation.json'
$receipt = Assert-TaxiNativeReceipt (Split-Path -Parent $receiptPath)
$packagePath = (Resolve-Path -LiteralPath $Package).Path
$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$expectedBase = "taxi-cam-$($receipt.version)-build.$BuildNumber-windows-x64"
$expectedInstaller = "taxi-cam-$($receipt.version)-windows-x64-setup.exe"
if ([IO.Path]::GetFileName($installerPath) -cne $expectedInstaller -or
    [IO.Path]::GetFileName($packagePath) -cne "$expectedBase.zip" -or $receipt.buildNumber -ne $BuildNumber) {
    throw 'Release asset names and validated application must match the release build.'
}
function Invoke-OriginFetchWithGhCredentialForThisCommandOnly([string[]]$Arguments, [string]$Failure) {
    & git -C $root -c 'credential.helper=!gh auth git-credential' fetch origin @Arguments
    if ($LASTEXITCODE -ne 0) { throw $Failure }
}
Invoke-OriginFetchWithGhCredentialForThisCommandOnly @('--tags', '--force') 'Could not fetch release tags from origin.'
Invoke-OriginFetchWithGhCredentialForThisCommandOnly @('main:refs/remotes/origin/main', '--force') 'Could not fetch origin/main.'
$tag = Get-TaxiNextPublishTag -Repository $root -Commit $Commit -Version $receipt.version
$title = "Taxi Cam $($receipt.version)"
function Invoke-Gh([string[]]$Arguments) {
    $result = @(& gh @Arguments)
    if ($LASTEXITCODE -ne 0) { throw "GitHub operation failed: $($Arguments[0])" }
    return ($result -join "`n")
}
$head = & git rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $head -ne $Commit) { throw 'Release target must be the exact checked-out commit.' }
$info = Get-Content -Raw -LiteralPath ([IO.Path]::ChangeExtension($packagePath, '.build-info.json')) | ConvertFrom-Json
if ($info.sourceCommit -ne $Commit -or $info.build -ne "build.$BuildNumber" -or $info.buildNumber -ne $BuildNumber -or $info.sourceDirty) { throw 'Package provenance does not match this workflow.' }
$setupReceipt = Get-Content -Raw -LiteralPath ($installerPath + '.json') | ConvertFrom-Json
if ($setupReceipt.sourceCommit -ne $Commit -or $setupReceipt.version -ne $receipt.version -or
    $setupReceipt.buildNumber -ne $BuildNumber -or
    $setupReceipt.installerSha256 -ne (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash -or
    $setupReceipt.packageSha256 -ne (Get-FileHash -LiteralPath $packagePath -Algorithm SHA256).Hash) {
    throw 'Installer provenance does not match the validated release package.'
}
foreach ($name in @('taxi-cam.exe','taxi-camera-bridge.dll')) {
    if ($setupReceipt.files.PSObject.Properties[$name].Value -ne $receipt.files.PSObject.Properties[$name].Value) {
        throw "Installer contains a different validated binary: $name"
    }
}

$pages = Invoke-Gh -Arguments @('api',"repos/$Repository/releases?per_page=100",'--paginate','--slurp') | ConvertFrom-Json -NoEnumerate
$releases = @(foreach ($page in $pages) { foreach ($release in $page) { $release } })
$existing = $releases | Where-Object { $_.tag_name -eq $tag } | Select-Object -First 1
if ($existing) {
    if ($existing.target_commitish -ne $Commit) { throw 'Existing release targets a different commit.' }
    if (-not $existing.draft) {
        Write-Output "Release already published; preserving its assets: $($existing.html_url)"
        "Release: $($existing.html_url)" >> $env:GITHUB_STEP_SUMMARY
        return
    }
}
$previous = Get-TaxiLastMainReleaseTag -Repository $root -Commit $Commit -ExcludeCommit
$entries = @(Get-TaxiReleaseChangeEntries -Repository $root -FromRef $previous -ToCommit $Commit)
$numbers = @($entries | Where-Object { $_.Number -gt 0 } | ForEach-Object { [int]$_.Number })
try {
    $pullRequests = Get-TaxiGitHubPullRequestInfo -Repository $Repository -Numbers $numbers
    if ($pullRequests.Count -gt 0) {
        $entries = @(Get-TaxiReleaseChangeEntries -Repository $root -FromRef $previous -ToCommit $Commit -PullRequestInfo $pullRequests)
    }
} catch {
    Write-Output 'GitHub pull request details are unavailable; release notes use the merge commit text.'
}
$runUrl = "$env:GITHUB_SERVER_URL/$Repository/actions/runs/$BuildRunId"
$notes = New-TaxiReleaseNotes -Repository $Repository -Commit $Commit -PreviousTag $previous -BuildRunUrl $runUrl -Entries $entries
$notesPath = Join-Path $root 'build/release-notes.md'
$notes | Set-Content -LiteralPath $notesPath -Encoding utf8
$checksum = Join-Path $root 'build/SHA256SUMS.txt'
@($packagePath, $installerPath) | ForEach-Object {
    "$((Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($_))"
} | Set-Content -LiteralPath $checksum -Encoding ascii
if (-not $existing) {
    $arguments = @('release','create',$tag,'--repo',$Repository,'--target',$Commit,'--title',$title,'--draft',
        '--notes-file',$notesPath)
    [void](Invoke-Gh -Arguments $arguments)
}
[void](Invoke-Gh -Arguments @('release','upload',$tag,$packagePath,$installerPath,$checksum,$receiptPath,'--repo',$Repository,'--clobber'))
$recorded = & git rev-parse -q --verify "$tag^{commit}"
if ($LASTEXITCODE -eq 0) {
    if ($recorded -ne $Commit) { throw 'Existing publish tag targets a different commit.' }
} else {
    & git tag $tag $Commit
    if ($LASTEXITCODE -ne 0) { throw 'Could not record the publish tag locally.' }
}
$latest = if ((Get-TaxiLastPublishedMainTag -Repository $root) -ceq $tag) { '--latest=true' } else { '--latest=false' }
[void](Invoke-Gh -Arguments @('release','edit',$tag,'--repo',$Repository,'--draft=false',$latest))
$url = Invoke-Gh -Arguments @('release','view',$tag,'--repo',$Repository,'--json','url','--jq','.url')
Write-Output "Published $url"
"Release: $url" >> $env:GITHUB_STEP_SUMMARY
