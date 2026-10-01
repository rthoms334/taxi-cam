Set-StrictMode -Version Latest

function Get-TaxiReleaseBuildNumber(
    [string]$GitHubActions = $env:GITHUB_ACTIONS,
    [string]$EventName = $env:GITHUB_EVENT_NAME,
    [string]$Ref = $env:GITHUB_REF,
    [string]$RunNumber = $env:GITHUB_RUN_NUMBER
) {
    # Only main-branch workflow runs that can produce a publishable candidate
    # receive a release build number. Pull requests, other refs and local
    # builds stay at zero so they cannot look newer than a published tag.
    if ($GitHubActions -cne 'true') { return 0 }
    if ($EventName -like 'pull_request*') { return 0 }
    if ($Ref -cne 'refs/heads/main') { return 0 }
    if ([string]::IsNullOrWhiteSpace($RunNumber)) { return 0 }
    $buildNumber = 0
    if ($RunNumber -notmatch '^[1-9][0-9]{0,9}$' -or
        -not [int]::TryParse($RunNumber, [ref]$buildNumber)) { throw 'Invalid GitHub build number.' }
    return $buildNumber
}

function ConvertTo-TaxiReleaseVersion([string]$Text) {
    if ($Text -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
        throw "changelog.json version '$Text' is not major.minor.patch."
    }
    $parts = @($Matches[1], $Matches[2], $Matches[3]) | ForEach-Object {
        $number = 0
        if (-not [int]::TryParse($_, [ref]$number) -or $number -gt 65535) { throw 'Windows version components must fit in 16 bits.' }
        $number
    }
    return ,[int[]]$parts
}

function Get-TaxiVersion([string]$Repository, [int]$BuildNumber = 0) {
    if ($BuildNumber -lt 0) { throw 'Build number must not be negative.' }
    # The release version is the newest entry in changelog.json, so every build
    # carries exactly the version its What's new notes describe. Add a new top
    # entry to release a new version.
    $changelog = Get-Content -Raw -LiteralPath (Join-Path $Repository 'changelog.json') | ConvertFrom-Json
    if (-not ($changelog.PSObject.Properties.Name -contains 'releases')) { throw 'changelog.json must contain a releases list.' }
    $releases = @($changelog.releases)
    if ($releases.Count -eq 0) { throw 'changelog.json must list at least one release.' }
    $previous = $null
    foreach ($release in $releases) {
        if (-not ($release.PSObject.Properties.Name -contains 'version')) { throw 'Every changelog.json release needs a version.' }
        $current = ConvertTo-TaxiReleaseVersion ([string]$release.version)
        if ($previous) {
            $order = 0
            for ($i = 0; $i -lt 3 -and $order -eq 0; $i++) { $order = $previous[$i].CompareTo($current[$i]) }
            if ($order -le 0) { throw "changelog.json versions must be strictly descending; $($release.version) is out of order." }
        }
        $previous = $current
    }
    $parts = ConvertTo-TaxiReleaseVersion ([string]$releases[0].version)
    [pscustomobject]@{
        Version = "$($parts[0]).$($parts[1]).$($parts[2])"
        Major = $parts[0]; Minor = $parts[1]; Patch = $parts[2]
        BuildNumber = $BuildNumber; Source = 'changelog.json'
    }
}
