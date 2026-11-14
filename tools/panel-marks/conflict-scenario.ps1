# Sets up the issue 200 scenario in the MSFS 2024 Community folder
# (claudedocs/panel-mark-identification.md): one add-on that takes the
# cockpit page (vRAAS's panel-raas) and one that holds its own copy of
# Core/VCockpit.js without listing it in its manifest (YourControls).
# Developer experiments only.
#
#   tools/panel-marks/conflict-scenario.ps1                # stand-ins for both
#   tools/panel-marks/conflict-scenario.ps1 -RaasFrom <folder> -YourControlsFrom <folder>
#   tools/panel-marks/conflict-scenario.ps1 -Status        # what the logs say
#   tools/panel-marks/conflict-scenario.ps1 -Remove
#
# A real add-on proves more than a stand-in: -RaasFrom and -YourControlsFrom
# copy a real package folder (the one holding its manifest.json) in under the
# add-on's name. Without them a stand-in is made from the installed stock
# files: the stock page plus vRAAS's two script lines (its /RAAS/ scripts are
# absent and fail to load), and an unchanged copy of the stock VCockpit.js.
# A stand-in is only as good as its baseline: run the released build on it
# first. If that build still reads the marks, the stand-in does not reproduce
# the conflict and a result on a newer build says nothing.
#
# A folder of either name that this tool did not make (a real installed
# add-on) is used as it is and never changed or removed. Run with MSFS and
# Taxi Cam closed: add-ons mount when the simulator starts.
#
# The Community folder comes from the simulator's UserCfg.opt, and the stock
# files from that same build's install. A PC with both the Store and the Steam
# build needs -Community (and -Content if the install is not found).
param(
    [string]$Content = '',
    [string]$Community = '',
    [string]$RaasFrom = '',
    [string]$YourControlsFrom = '',
    [switch]$Status,
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'
$marker = 'taxi-cam-stand-in.txt'
$pageRelative = 'html_ui/Pages/VCockpit/Core/VCockpit.html'
$scriptRelative = 'html_ui/Pages/VCockpit/Core/VCockpit.js'
$logicRelative = 'html_ui/Pages/VCockpit/Instruments/Shared/Utils/XMLLogic.js'

# Each build's Community folder, from its own UserCfg.opt: Store first.
function Find-Communities {
    foreach ($entry in @(@('Store', (Join-Path $env:LOCALAPPDATA 'Packages\Microsoft.Limitless_8wekyb3d8bbwe\LocalCache\UserCfg.opt')),
                         @('Steam', (Join-Path $env:APPDATA 'Microsoft Flight Simulator 2024\UserCfg.opt')))) {
        if (-not (Test-Path $entry[1])) { continue }
        $line = Select-String -Path $entry[1] -Pattern '^\s*InstalledPackagesPath\s+"(.*)"' | Select-Object -First 1
        if ($line) { [pscustomobject]@{ Build = $entry[0]; Options = $entry[1]; Community = (Join-Path $line.Matches[0].Groups[1].Value 'Community') } }
    }
}
# The folder holding FlightSimulator2024.exe for that build, in its usual
# places. The simulator is closed when this runs, so it cannot be asked.
function Find-Content([string]$build) {
    $candidates = if ($build -eq 'Steam') {
        $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
        @($(if ($steam) { Join-Path ($steam -replace '/', '\') 'steamapps\common\MSFS2024' }),
          'C:\Program Files (x86)\Steam\steamapps\common\MSFS2024')
    } else {
        @('C:\XboxGames\Microsoft Flight Simulator 2024\Content')
    }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path (Join-Path $candidate 'FlightSimulator2024.exe'))) { return $candidate }
    }
    throw "MSFS 2024 ($build) install not found; pass -Content <folder holding FlightSimulator2024.exe>."
}
function Native([string]$relative) { $relative -replace '/', '\' }
function Write-Layout([string]$package) {
    $root = (Resolve-Path $package).Path.TrimEnd('\')
    $files = Get-ChildItem -Recurse -File $root | Where-Object { $_.Name -notin @('layout.json', 'manifest.json', $marker) }
    $layout = @{ content = @($files | ForEach-Object {
        [ordered]@{ path = $_.FullName.Substring($root.Length + 1) -replace '\\', '/'; size = $_.Length; date = $_.LastWriteTimeUtc.ToFileTimeUtc() }
    }) }
    $layout | ConvertTo-Json -Depth 5 | Set-Content -Encoding ascii (Join-Path $root 'layout.json')
}
# What a package folder is: none, ours (made by this tool) or real.
function Kind([string]$package) {
    if (-not (Test-Path $package)) { return 'none' }
    if (Test-Path (Join-Path $package $marker)) { return 'ours' }
    return 'real'
}
function Describe([string]$package) {
    $kind = Kind $package
    if ($kind -eq 'none') { return "$(Split-Path -Leaf $package): not there" }
    $made = if ($kind -eq 'ours') { (Get-Content (Join-Path $package $marker) | Select-Object -First 1) } else { 'installed add-on, not made by this tool' }
    $manifest = Join-Path $package 'manifest.json'
    $text = if (Test-Path $manifest) { Get-Content -Raw $manifest } else { '' }
    $flat = ($text -replace '[\\/]', '').ToLowerInvariant()
    $parts = foreach ($entry in @(@('page', $pageRelative), @('VCockpit.js', $scriptRelative), @('XMLLogic.js', $logicRelative))) {
        $has = Test-Path (Join-Path $package (Native $entry[1]))
        $listed = $flat.Contains(($entry[1] -replace '/', '').ToLowerInvariant())
        "$($entry[0])=$(if (-not $has) { 'no' } elseif ($listed) { 'listed' } else { 'unlisted' })"
    }
    $hint = if ($text -match '"package_order_hint"\s*:\s*"([^"]*)"') { $Matches[1] } else { 'none' }
    return "$(Split-Path -Leaf $package): $made; $($parts -join ' '); order_hint=$hint"
}

$build = ''
if (-not $Community) {
    $found = @(Find-Communities)
    if ($found.Count -eq 0) { throw 'Community folder not found; pass -Community <folder>.' }
    if ($found.Count -gt 1) {
        throw "Both builds are set up here: pass -Community with the one to use. $(($found | ForEach-Object { "$($_.Build): $($_.Community)" }) -join '; ')"
    }
    $Community = $found[0].Community
    $build = $found[0].Build
    "Community folder ($build build, from $($found[0].Options)): $Community"
} else { "Community folder: $Community" }
$raas = Join-Path $Community 'panel-raas'
$controls = Join-Path $Community 'YourControls'

if ($Status) {
    Describe $raas
    Describe $controls
    $data = Join-Path $env:LOCALAPPDATA 'Taxi Cam'
    $launcher = Join-Path $data 'launcher.log'
    if (Test-Path $launcher) {
        ''
        'launcher.log, last Display package line:'
        Select-String -Path $launcher -Pattern 'Display package:' | Select-Object -Last 1 | ForEach-Object { '  ' + $_.Line }
    }
    $bridge = Join-Path $data 'bridge.log'
    if (Test-Path $bridge) {
        # Only the last simulator session: from the last time the bridge started.
        $lines = Get-Content $bridge
        $start = 0
        for ($i = $lines.Count - 1; $i -ge 0; $i--) { if ($lines[$i] -match 'Bridge worker started') { $start = $i; break } }
        $session = $lines[$start..($lines.Count - 1)]
        $pick = { param($pattern, $count) $session | Where-Object { $_ -match $pattern } | Select-Object -Last $count | ForEach-Object { '  ' + ($_ -replace '^.*\| ', '') } }
        ''
        'bridge.log, last simulator session:'
        & $pick 'Display marks: \d+ of \d+ panel textures read' 1
        & $pick 'Display mark scan:' 1
        & $pick 'Display named routing:' 1
        $inactive = @($session | Where-Object { $_ -match "display package is not active" }).Count
        "  status lines saying the package is not active: $inactive"
    }
    return
}

if (Get-Process FlightSimulator2024 -ErrorAction SilentlyContinue) {
    throw 'MSFS is running. Close it first: add-ons mount when the simulator starts, so a change now proves nothing for this session.'
}

# Removes a folder this tool made, its marker last: a delete that stops part
# way leaves a folder still known as ours.
function Remove-Ours([string]$package) {
    Get-ChildItem -Force $package | Where-Object { $_.Name -ne $marker } | Remove-Item -Recurse -Force
    Remove-Item -Recurse -Force $package
}
# Starts a folder this tool makes with its marker, before anything else is
# written: a setup that stops part way is still known as ours.
function New-Ours([string]$package, [string]$what) {
    New-Item -ItemType Directory -Force $package | Out-Null
    Set-Content -Encoding ascii (Join-Path $package $marker) @($what, (Get-Date).ToUniversalTime().ToString('o'))
}

if ($Remove) {
    foreach ($package in @($raas, $controls)) {
        switch (Kind $package) {
            'ours' { Remove-Ours $package; "Removed $package" }
            'real' { "Left alone (not made by this tool): $package" }
            default { "Not there: $package" }
        }
    }
    'Start Taxi Cam once before the next flight so its package goes back to the page.'
    return
}

if (-not $Content) {
    if (-not $build) { throw '-Community was given, so say which install it belongs to: pass -Content <folder holding FlightSimulator2024.exe>.' }
    $Content = Find-Content $build
}
"Simulator install: $Content"
$stockPackage = Join-Path $Content 'Packages\asobo-vcockpits-core'
$stockPage = Join-Path $stockPackage (Native $pageRelative)
$stockScript = Join-Path $stockPackage (Native $scriptRelative)
foreach ($needed in @($stockPage, $stockScript)) { if (-not (Test-Path $needed)) { throw "Stock file not found: $needed" } }
$logicHolders = @(Get-ChildItem -Directory (Split-Path -Parent $stockPackage) | Where-Object { Test-Path (Join-Path $_.FullName (Native $logicRelative)) })
"Stock XMLLogic.js is in: $(if ($logicHolders) { ($logicHolders.Name -join ', ') } else { 'NO stock package (the second script cannot be used)' })"
$stockManifest = Get-Content -Raw (Join-Path $stockPackage 'manifest.json') | ConvertFrom-Json

function Copy-Real([string]$from, [string]$package) {
    if (-not (Test-Path (Join-Path $from 'manifest.json'))) { throw "$from has no manifest.json; pass the add-on's package folder." }
    New-Ours $package "copy of $from"
    Get-ChildItem -Force $from | Copy-Item -Recurse -Force -Destination $package
}
function New-Manifest([string]$package, [string]$title, [hashtable]$extra) {
    $manifest = [ordered]@{
        dependencies = @()
        content_type = 'MISC'
        title = $title
        manufacturer = ''
        creator = 'Taxi Cam stand-in'
        package_version = '0.1.0'
        minimum_game_version = $stockManifest.minimum_game_version
        release_notes = @{ neutral = @{ LastUpdate = ''; OlderHistory = '' } }
        export_type = 'Community'
    }
    foreach ($key in $extra.Keys) { $manifest[$key] = $extra[$key] }
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -Encoding ascii (Join-Path $package 'manifest.json')
}

# panel-raas: takes the page, listed, with the order hint Taxi Cam's package has.
switch (Kind $raas) {
    'real' { "Using the installed add-on as it is: $raas" }
    default {
        if (Test-Path $raas) { Remove-Ours $raas }
        if ($RaasFrom) { Copy-Real $RaasFrom $raas }
        else {
            $bytes = [IO.File]::ReadAllBytes($stockPage)
            $bom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
            $skip = if ($bom) { 3 } else { 0 }
            $text = [Text.Encoding]::UTF8.GetString($bytes, $skip, $bytes.Length - $skip)
            $pattern = '(?m)^([ \t]*)<script type="text/javascript" src="VCockpit\.js"></script>[ \t]*(\r?\n)'
            $found = [regex]::Matches($text, $pattern)
            if ($found.Count -ne 1) { throw "Stock VCockpit.html has $($found.Count) VCockpit.js script lines (expected 1)." }
            $m = $found[0]
            $indent = $m.Groups[1].Value
            $ending = $m.Groups[2].Value
            $added = $indent + '<script type="text/javascript" src="/RAAS/signalr-3.1.31.js"></script>' + $ending +
                     $indent + '<script defer type="text/javascript" src="/RAAS/RaasMain.js"></script>' + $ending
            $page = $text.Insert($m.Index + $m.Length, $added)
            New-Ours $raas 'stand-in made from the stock page'
            New-Item -ItemType Directory -Force (Split-Path -Parent (Join-Path $raas (Native $pageRelative))) | Out-Null
            [IO.File]::WriteAllText((Join-Path $raas (Native $pageRelative)), $page, (New-Object Text.UTF8Encoding($bom)))
            New-Manifest $raas 'panel-raas (Taxi Cam stand-in)' @{
                package_order_hint = 'PANEL_PATCH'
                globally_overriden_base_sim_files = @('\html_ui\pages\VCockpit\Core\VCockpit.html')
            }
            Write-Layout $raas
        }
        "Wrote $raas"
    }
}
# YourControls: its own copy of VCockpit.js, not listed, no order hint.
switch (Kind $controls) {
    'real' { "Using the installed add-on as it is: $controls" }
    default {
        if (Test-Path $controls) { Remove-Ours $controls }
        if ($YourControlsFrom) { Copy-Real $YourControlsFrom $controls }
        else {
            New-Ours $controls 'stand-in made from the stock VCockpit.js'
            $target = Join-Path $controls (Native $scriptRelative)
            New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
            Copy-Item $stockScript $target
            New-Manifest $controls 'YourControls (Taxi Cam stand-in)' @{}
            Write-Layout $controls
        }
        "Wrote $controls"
    }
}
''
Describe $raas
Describe $controls
''
'Next: start MSFS with the build under test, load a flight, wait for the cockpit, then run this again with -Status.'
