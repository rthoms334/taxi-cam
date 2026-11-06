param(
    [string]$Content = '',
    [string]$Community = '',
    [int]$MaxShowMs = 0,
    [switch]$Check,
    [switch]$Remove
)
$ErrorActionPreference = 'Stop'
$packageName = 'taxi-cam-panel-marks'
$relative = 'html_ui/Pages/VCockpit/Core/VCockpit.html'
$helperName = 'taxi-cam-marks.js'
$marker = 'taxi-cam-marks.stock.txt'

function Find-Content {
    $running = Get-Process FlightSimulator2024 -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($running -and $running.Path) { return Split-Path -Parent $running.Path }
    foreach ($candidate in @('C:\XboxGames\Microsoft Flight Simulator 2024\Content')) {
        if (Test-Path (Join-Path $candidate 'FlightSimulator2024.exe')) { return $candidate }
    }
    throw 'MSFS 2024 content folder not found; pass -Content <folder holding FlightSimulator2024.exe>.'
}
function Find-Community {
    foreach ($opt in @((Join-Path $env:LOCALAPPDATA 'Packages\Microsoft.Limitless_8wekyb3d8bbwe\LocalCache\UserCfg.opt'),
                       (Join-Path $env:APPDATA 'Microsoft Flight Simulator 2024\UserCfg.opt'))) {
        if (-not (Test-Path $opt)) { continue }
        $line = Select-String -Path $opt -Pattern '^\s*InstalledPackagesPath\s+"(.*)"' | Select-Object -First 1
        if ($line) { return Join-Path $line.Matches[0].Groups[1].Value 'Community' }
    }
    throw 'Community folder not found; pass -Community <folder>.'
}
function FileTime([string]$path) { (Get-Item $path).LastWriteTimeUtc.ToFileTimeUtc() }
function Sha256([string]$path) { (Get-FileHash -Algorithm SHA256 $path).Hash }

if (-not $Content) { $Content = Find-Content }
if (-not $Community) { $Community = Find-Community }
$package = Join-Path $Community $packageName
$stockPackage = Join-Path $Content 'Packages\asobo-vcockpits-core'
$stock = Join-Path $stockPackage ($relative -replace '/', '\')

if ($Remove) {
    if (Test-Path (Join-Path $package $marker)) { Remove-Item -Recurse -Force $package; "Removed $package" }
    elseif (Test-Path $package) { throw "$package has no $marker; not removing a folder this tool did not make." }
    else { 'Nothing to remove.' }
    return
}
if (-not (Test-Path $stock)) { throw "Stock VCockpit.html not found at $stock." }
if ($Check) {
    $recorded = if (Test-Path (Join-Path $package $marker)) { (Get-Content (Join-Path $package $marker) | Select-Object -First 1) } else { '' }
    $now = Sha256 $stock
    if (-not $recorded) { 'No package.' } elseif ($recorded -eq $now) { "Current: stock $now" } else { "STALE: package made from $recorded, stock is now $now. Run again." }
    return
}

$bytes = [IO.File]::ReadAllBytes($stock)
$bom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
$text = [Text.Encoding]::UTF8.GetString($bytes, $(if ($bom) { 3 } else { 0 }), $bytes.Length - $(if ($bom) { 3 } else { 0 }))
$vcockpitScriptLine = '(?m)^([ \t]*)<script type="text/javascript" src="VCockpit\.js"></script>[ \t]*(\r?\n)'
$vcockpitScriptLines = [regex]::Matches($text, $vcockpitScriptLine)
if ($vcockpitScriptLines.Count -ne 1) { throw "Stock VCockpit.html has $($vcockpitScriptLines.Count) VCockpit.js script lines (expected 1); not overriding it." }
if ($text -match [regex]::Escape($helperName)) { throw 'Stock VCockpit.html already names the helper.' }
$m = $vcockpitScriptLines[0]
$line = $m.Groups[1].Value + '<script type="text/javascript" src="/Pages/VCockpit/Core/' + $helperName + '"></script>' + $m.Groups[2].Value
$page = $text.Insert($m.Index + $m.Length, $line)

$helper = Get-Content -Raw (Join-Path $PSScriptRoot "..\..\src\panel_marks\$helperName")
if ($MaxShowMs -gt 0) {
    $before = $helper
    $helper = $helper -replace 'MAX_SHOW_MS = \d+', "MAX_SHOW_MS = $MaxShowMs"
    if ($helper -eq $before) { throw 'MAX_SHOW_MS not found in the helper.' }
}

$core = Join-Path $package 'html_ui\Pages\VCockpit\Core'
New-Item -ItemType Directory -Force $core | Out-Null
$utf8 = New-Object Text.UTF8Encoding($bom)
[IO.File]::WriteAllText((Join-Path $core 'VCockpit.html'), $page, $utf8)
[IO.File]::WriteAllText((Join-Path $core $helperName), $helper, (New-Object Text.UTF8Encoding($false)))
Set-Content -Encoding ascii (Join-Path $package $marker) @((Sha256 $stock), $stock, "max_show_ms=$MaxShowMs")

$stockManifest = Get-Content -Raw (Join-Path $stockPackage 'manifest.json') | ConvertFrom-Json
$manifest = [ordered]@{
    dependencies = @()
    content_type = 'MISC'
    title = 'Taxi Cam panel marks (experiment)'
    manufacturer = ''
    creator = 'Taxi Cam'
    package_version = '0.1.0'
    minimum_game_version = $stockManifest.minimum_game_version
    release_notes = @{ neutral = @{ LastUpdate = ''; OlderHistory = '' } }
    export_type = 'Community'
    package_order_hint = 'PANEL_PATCH'
    globally_overriden_base_sim_files = @('\html_ui\pages\VCockpit\Core\VCockpit.html')
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -Encoding ascii (Join-Path $package 'manifest.json')
$files = @("html_ui/Pages/VCockpit/Core/VCockpit.html", "html_ui/Pages/VCockpit/Core/$helperName")
$layout = @{ content = @($files | ForEach-Object {
    $full = Join-Path $package ($_ -replace '/', '\')
    [ordered]@{ path = $_; size = (Get-Item $full).Length; date = (FileTime $full) }
}) }
$layout | ConvertTo-Json -Depth 5 | Set-Content -Encoding ascii (Join-Path $package 'layout.json')
"Wrote $package from $stock (max_show_ms=$MaxShowMs)."
if (Get-Process FlightSimulator2024 -ErrorAction SilentlyContinue) { 'MSFS is running: the package takes effect at its next start.' }
