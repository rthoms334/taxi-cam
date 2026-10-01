Set-StrictMode -Version Latest

function Test-TaxiReleaseTagName([string]$Tag) {
    return [bool]($Tag -cmatch '^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-build\.(0|[1-9][0-9]*)$')
}

function Get-TaxiReleaseTagBuildNumber([string]$Tag) {
    if (-not (Test-TaxiReleaseTagName $Tag)) {
        throw 'Release tag must be v<major>.<minor>.<patch>-build.<number>.'
    }
    $null = $Tag -cmatch '^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-build\.(0|[1-9][0-9]*)$'
    return [int]$Matches[4]
}

function Get-TaxiCommitReleaseTags([string]$Repository, [string]$Commit) {
    $tags = @(& git -C $Repository tag --list 'v*-build.*' --points-at $Commit)
    if ($LASTEXITCODE -ne 0) { throw 'Could not list release tags for a commit.' }
    return @($tags | Where-Object { Test-TaxiReleaseTagName $_ })
}

function Get-TaxiLastMainReleaseTag {
    param(
        [Parameter(Mandatory=$true)][string]$Repository,
        [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-fA-F]{7,40}$')][string]$Commit,
        [switch]$ExcludeCommit
    )
    $head = & git -C $Repository rev-parse $Commit
    if ($LASTEXITCODE -ne 0 -or $head -notmatch '^[0-9a-fA-F]{40}$') { throw 'Could not resolve the publish commit.' }
    $start = $head
    if ($ExcludeCommit) {
        & git -C $Repository rev-parse --verify ($head + '^') | Out-Null
        if ($LASTEXITCODE -ne 0) { return $null }
        $start = $head + '^'
    }
    $history = @(& git -C $Repository rev-list --first-parent $start)
    if ($LASTEXITCODE -ne 0) { throw 'Could not walk first-parent history for the last main release tag.' }
    foreach ($candidate in $history) {
        $tags = @(Get-TaxiCommitReleaseTags $Repository $candidate)
        if ($tags.Count -eq 0) { continue }
        return @($tags | Sort-Object { Get-TaxiReleaseTagBuildNumber $_ } -Descending)[0]
    }
    return $null
}

function Get-TaxiNextPublishTag {
    param(
        [Parameter(Mandatory=$true)][string]$Repository,
        [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-fA-F]{7,40}$')][string]$Commit,
        [Parameter(Mandatory=$true)][ValidatePattern('^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$')][string]$Version
    )
    $last = Get-TaxiLastMainReleaseTag -Repository $Repository -Commit $Commit
    if ($last) {
        $tagged = & git -C $Repository rev-parse ($last + '^{commit}')
        $head = & git -C $Repository rev-parse $Commit
        if ($LASTEXITCODE -ne 0 -or $tagged -notmatch '^[0-9a-fA-F]{40}$' -or $head -notmatch '^[0-9a-fA-F]{40}$') {
            throw 'Could not resolve the last main release tag.'
        }
        if ($tagged -eq $head) { return $last }
        # The updater compares major.minor.patch before the build number, and
        # installed binaries carry the workflow run number rather than the tag
        # build number. A release that keeps the previous version could then
        # look older than what users already run, so every publish needs a
        # higher changelog.json version.
        $null = $last -cmatch '^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-build\.'
        $previous = [version]"$($Matches[1]).$($Matches[2]).$($Matches[3])"
        if ([version]$Version -le $previous) {
            throw "Version $Version is not higher than the last release $last. Add a new top entry to changelog.json before publishing."
        }
        $number = (Get-TaxiReleaseTagBuildNumber $last) + 1
    } else {
        $number = 1
    }
    return "v$Version-build.$number"
}
