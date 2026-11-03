Set-StrictMode -Version Latest

function ConvertTo-TaxiPublishedTagVersion([string]$Tag) {
    if ($Tag -cnotmatch '^v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-build\.(0|[1-9][0-9]*)$') {
        return $null
    }
    [pscustomobject]@{
        Tag = $Tag
        Major = [uint32]$Matches[1]
        Minor = [uint32]$Matches[2]
        Patch = [uint32]$Matches[3]
        Build = [uint32]$Matches[4]
    }
}

function Get-TaxiPublishedMainTip([string]$Repository) {
    foreach ($name in @('refs/remotes/origin/main', 'refs/heads/main')) {
        $sha = [string](& git -C $Repository rev-parse -q --verify $name)
        if ($LASTEXITCODE -eq 0 -and $sha -match '^[0-9a-fA-F]{40}$') { return $sha }
    }
    throw 'Could not resolve origin/main or main for the last published tag.'
}

function Compare-TaxiPublishedTagVersion($Left, $Right) {
    foreach ($field in @('Major', 'Minor', 'Patch', 'Build')) {
        if ($Left.$field -ne $Right.$field) { return ([int64]$Left.$field - [int64]$Right.$field) }
    }
    return 0
}

function Get-TaxiLastPublishedMainTag {
    param(
        [Parameter(Mandatory=$true)][string]$Repository,
        [string]$ExcludeTag
    )
    $tip = Get-TaxiPublishedMainTip $Repository
    $best = $null
    foreach ($tag in @(& git -C $Repository tag --list)) {
        if ([string]::IsNullOrWhiteSpace($tag)) { continue }
        if ($ExcludeTag -and $tag -ceq $ExcludeTag) { continue }
        $parsed = ConvertTo-TaxiPublishedTagVersion $tag
        if (-not $parsed) { continue }
        & git -C $Repository merge-base --is-ancestor $tag $tip
        if ($LASTEXITCODE -eq 1) { continue }
        if ($LASTEXITCODE -ne 0) { throw 'Could not check whether a published tag is on main.' }
        if (-not $best -or (Compare-TaxiPublishedTagVersion $parsed $best) -gt 0) { $best = $parsed }
    }
    if ($best) { return [string]$best.Tag }
    return $null
}

function Test-TaxiReleaseChangeIsNoise([string]$Subject, [string[]]$Paths) {
    if ($Subject -match '^(?i)Merge (remote-tracking branch\b|origin/|branch )') { return $true }
    if ($Subject -match '^(?i)Merge .+ into ') { return $true }
    if ($Subject -match '^(?i)(ci|chore\(ci\))(\([^)]*\))?\s*:') { return $true }
    if (-not $Paths -or @($Paths).Count -eq 0) { return $false }
    foreach ($path in @($Paths)) {
        $normalized = [string]$path
        $normalized = $normalized.Replace('\', '/')
        $ciOnly = $normalized.StartsWith('.github/', [StringComparison]::OrdinalIgnoreCase) -or
            $normalized.StartsWith('ci/', [StringComparison]::OrdinalIgnoreCase) -or
            $normalized.Equals('docs/releases.md', [StringComparison]::OrdinalIgnoreCase)
        if (-not $ciOnly) { return $false }
    }
    return $true
}

function Get-TaxiReleaseChangeSummary([string]$Body, [int]$MaxLength = 240) {
    if ([string]::IsNullOrWhiteSpace($Body)) { return '' }
    $text = [regex]::Replace($Body, '(?s)<!--.*?-->', '')
    foreach ($paragraph in @($text -split '(\r?\n){2,}')) {
        $block = $paragraph.Trim()
        if (-not $block) { continue }
        if ($block -match '^(?i)(validation|test plan|checklist)\s*:') { continue }
        $line = (($block -split '\r?\n') | Select-Object -First 1).Trim()
        if (-not $line) { continue }
        if ($line -match '^(?i)(merge pull request #|signed-off-by:|co-authored-by:)') { continue }
        if ($line.Length -le $MaxLength) { return $line }
        $cut = $line.Substring(0, $MaxLength)
        $space = $cut.LastIndexOf(' ')
        if ($space -ge 80) { $cut = $cut.Substring(0, $space) }
        return ($cut.TrimEnd() + '...')
    }
    return ''
}

function Get-TaxiReleaseChangeHeadline([string]$Subject, [string]$Body) {
    $number = 0
    $title = ''
    if ($Subject -match '^Merge pull request #([1-9][0-9]*) from \S+') {
        $number = [int]$Matches[1]
        $title = Get-TaxiReleaseChangeSummary $Body
        if (-not $title) { $title = "Pull request #$number" }
    } elseif ($Subject -match '^(.+?)\s+\(#([1-9][0-9]*)\)\s*$') {
        $title = $Matches[1].Trim()
        $number = [int]$Matches[2]
    } else {
        $title = $Subject.Trim()
    }
    [pscustomobject]@{ Number = $number; Title = $title }
}

function Get-TaxiCommitPaths([string]$Repository, [string]$Commit) {
    $parents = @((& git -C $Repository rev-list --parents -n 1 $Commit) -split ' ')
    if ($LASTEXITCODE -ne 0) { throw 'Could not read commit parents for release notes.' }
    if ($parents.Count -lt 2) { return @() }
    $paths = @(& git -C $Repository diff --name-only $parents[1] $Commit)
    if ($LASTEXITCODE -ne 0) { throw 'Could not read commit paths for release notes.' }
    return @($paths)
}

function Get-TaxiNormalizedReleaseTitle([string]$Title) {
    return (($Title -replace '\s+', ' ').Trim().ToLowerInvariant())
}

function Get-TaxiGitHubPullRequestInfo {
    param(
        [Parameter(Mandatory=$true)][ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')][string]$Repository,
        [int[]]$Numbers
    )
    $info = @{}
    foreach ($number in @($Numbers | Select-Object -Unique)) {
        if ($number -le 0) { continue }
        $raw = @(& gh api "repos/$Repository/pulls/$number" '--jq', '{number: .number, title: .title, body: .body}')
        if ($LASTEXITCODE -ne 0) { continue }
        $parsed = ($raw -join "`n") | ConvertFrom-Json
        if ($parsed -and $parsed.number) { $info[[int]$parsed.number] = $parsed }
    }
    return $info
}

function Get-TaxiReleaseChangeEntries {
    param(
        [Parameter(Mandatory=$true)][string]$Repository,
        [string]$FromRef,
        [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-fA-F]{7,40}$')][string]$ToCommit,
        [hashtable]$PullRequestInfo
    )
    $range = if ($FromRef) { "$FromRef..$ToCommit" } else { $ToCommit }
    $lines = @(& git -C $Repository log --first-parent --reverse '--format=%H%x09%s' $range)
    if ($LASTEXITCODE -ne 0) { throw 'Could not collect release note commits.' }
    $entries = [System.Collections.Generic.List[object]]::new()
    $seenNumbers = @{}
    $seenTitles = @{}
    foreach ($line in $lines) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        $parts = $line -split "`t", 2
        if ($parts.Count -lt 2) { continue }
        $hash = $parts[0]
        $subject = $parts[1]
        $body = @(& git -C $Repository log -1 '--format=%b' $hash) -join "`n"
        if ($LASTEXITCODE -ne 0) { throw 'Could not read a release note commit body.' }
        $paths = Get-TaxiCommitPaths $Repository $hash
        if (Test-TaxiReleaseChangeIsNoise $subject $paths) { continue }
        $headline = Get-TaxiReleaseChangeHeadline $subject $body
        $number = [int]$headline.Number
        $title = [string]$headline.Title
        $summary = ''
        if ($PullRequestInfo -and $number -gt 0 -and $PullRequestInfo.ContainsKey($number)) {
            $pr = $PullRequestInfo[$number]
            $prTitle = [string]$pr.title
            $prBody = [string]$pr.body
            if (-not [string]::IsNullOrWhiteSpace($prTitle)) { $title = $prTitle }
            if (-not [string]::IsNullOrWhiteSpace($prBody)) { $summary = Get-TaxiReleaseChangeSummary $prBody }
        }
        if (-not $summary) { $summary = Get-TaxiReleaseChangeSummary $body }
        $normalizedSummary = Get-TaxiNormalizedReleaseTitle $summary
        $normalizedTitle = Get-TaxiNormalizedReleaseTitle $title
        if ($normalizedSummary -and $normalizedSummary -ceq $normalizedTitle) { $summary = '' }
        if (-not $title) { continue }
        if ($number -gt 0) {
            if ($seenNumbers.ContainsKey($number)) { continue }
            $seenNumbers[$number] = $true
        }
        $normalized = Get-TaxiNormalizedReleaseTitle $title
        if ($seenTitles.ContainsKey($normalized)) { continue }
        $seenTitles[$normalized] = $true
        $entries.Add([pscustomobject]@{
            Hash = $hash
            Number = $number
            Title = $title
            Summary = $summary
        }) | Out-Null
    }
    return @($entries)
}

function Format-TaxiReleaseChanges {
    param(
        [Parameter(Mandatory=$true)][ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')][string]$Repository,
        [string]$PreviousTag,
        [Parameter(Mandatory=$true)][string]$Commit,
        $Entries
    )
    $since = if ($PreviousTag) { "since ``$PreviousTag``" } else { 'since the start of recorded history' }
    $lines = @(
        '## Changes',
        '',
        "Merged pull requests and first-parent changes $since. Unpublished builds after that last tag on main are included."
    )
    $items = @($Entries)
    if ($items.Count -eq 0) {
        $lines += @('', "No user-facing pull requests or first-parent changes were recorded $since.")
    } else {
        $lines += ''
        foreach ($entry in $items) {
            $title = [System.Net.WebUtility]::HtmlEncode([string]$entry.Title)
            if ($entry.Number -gt 0) {
                $item = "- **$title** ([#$($entry.Number)](https://github.com/$Repository/pull/$($entry.Number)))"
            } else {
                $short = ([string]$entry.Hash).Substring(0, 7)
                $item = "- **$title** ([$short](https://github.com/$Repository/commit/$($entry.Hash)))"
            }
            if (-not [string]::IsNullOrWhiteSpace([string]$entry.Summary)) {
                $summary = [System.Net.WebUtility]::HtmlEncode([string]$entry.Summary)
                $item += " - $summary"
            }
            $lines += $item
        }
    }
    if ($PreviousTag) {
        $lines += @('', "[Full changelog](https://github.com/$Repository/compare/$PreviousTag...$Commit)")
    }
    return $lines
}

function New-TaxiReleaseNotes {
    param(
        [Parameter(Mandatory=$true)][ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')][string]$Repository,
        [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-fA-F]{40}$')][string]$Commit,
        [string]$PreviousTag,
        [Parameter(Mandatory=$true)][string]$BuildRunUrl,
        $Entries
    )
    $notes = @(
        "Windows x64 native package from commit [$($Commit.Substring(0,7))](https://github.com/$Repository/commit/$Commit).",
        '',
        'Download the Windows x64 setup EXE and run it with MSFS and Taxi Cam closed. Existing paths and settings are preserved by default. Clear Keep existing settings only to reset to defaults; uninstall also keeps settings unless removal is explicitly selected. SHA256SUMS.txt covers the installer and optional runtime ZIP.',
        '',
        'Upgrading from 0.8.17 or earlier: download and run setup manually once. Those versions only recognise the old installer filename containing a build number; 0.8.18 and later support the shorter name.',
        '',
        "Licence: GNU GPL version 3 only. [Corresponding source, including build and installation scripts](https://github.com/$Repository/archive/$Commit.zip) for these binaries; [browse this exact revision](https://github.com/$Repository/tree/$Commit). The installer and runtime ZIP include LICENSE.txt and third-party notices.",
        '',
        'Validation: strict native build, software D3D12 (WARP), smoke, IPC, camera lifecycle, graphics-state, installer and package checks. Hardware GPU and live MSFS checks are not performed on the hosted runner.',
        '',
        "[Build and validation logs]($BuildRunUrl)",
        ''
    )
    $notes += Format-TaxiReleaseChanges -Repository $Repository -PreviousTag $PreviousTag -Commit $Commit -Entries $Entries
    return $notes
}
