$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$script:checks = 0
function Assert-That([bool]$Condition, [string]$Message) {
    $script:checks++
    if (-not $Condition) { throw $Message }
}
$utf8 = New-Object Text.UTF8Encoding($false)
$script = [IO.File]::ReadAllText((Join-Path $repoRoot 'installer/taxi-cam.iss'), $utf8)

$releaseAppIds = @([regex]::Matches($script, '(?m)^AppId=(.+)$') | ForEach-Object { $_.Groups[1].Value } | Where-Object { $_ -notmatch 'InstallerTest' })
Assert-That ($releaseAppIds.Count -eq 1 -and $releaseAppIds[0] -ceq '{{C8581992-5605-46CA-AF6F-60E44477C023}') "The release AppId changed: [$($releaseAppIds -join ', ')]. Existing installations upgrade in place only through this AppId."
Assert-That ($script -cmatch '(?m)^SetupMutex=380TaxiCamInstaller$') 'The setup mutex changed; an older and a newer Setup would then run at the same time.'
Assert-That ($script -cmatch '(?m)^ShowLanguageDialog=no$') 'Setup must follow the Windows display language without a language page.'

$languages = @([regex]::Matches($script, '(?m)^Name: "(\w+)"; MessagesFile: "([^"]+)"$'))
Assert-That ($languages.Count -ge 2) 'The [Languages] section has no language entries.'
Assert-That ($languages[0].Groups[1].Value -ceq 'english') 'English must be the first language: Inno falls back to the first entry, as the companion falls back to English.'
function Read-Messages([string]$Path) {
    $messages = @{}
    foreach ($line in [IO.File]::ReadAllLines($Path, $utf8)) {
        if ($line -match '^([A-Za-z]\w*)=(.*)$') {
            $messages[$Matches[1]] = (@([regex]::Matches($Matches[2], '%\d') | ForEach-Object { $_.Value } | Sort-Object) -join ',')
        }
    }
    return $messages
}
$english = Read-Messages (Join-Path $repoRoot 'installer/messages/en.isl')
Assert-That ($english.Count -gt 0) 'installer/messages/en.isl has no messages.'
$listed = @()
foreach ($language in $languages) {
    $file = @($language.Groups[2].Value -split ',' | Where-Object { $_ -like 'messages\*.isl' })
    Assert-That ($file.Count -eq 1) "Language $($language.Groups[1].Value) does not load one Taxi Cam message file."
    $listed += $file[0].Substring('messages\'.Length)
    $translated = Read-Messages (Join-Path $repoRoot ('installer/' + $file[0].Replace('\', '/')))
    $missing = @($english.Keys | Where-Object { -not $translated.ContainsKey($_) })
    $extra = @($translated.Keys | Where-Object { -not $english.ContainsKey($_) })
    Assert-That (-not $missing.Count -and -not $extra.Count) "$($file[0]) keys differ from en.isl: missing [$($missing -join ', ')], extra [$($extra -join ', ')]."
    foreach ($key in $english.Keys) {
        Assert-That ($translated[$key] -ceq $english[$key]) "$($file[0]) $key has placeholders [$($translated[$key])], en.isl has [$($english[$key])]."
    }
}
$present = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot 'installer/messages') -Filter '*.isl' | ForEach-Object { $_.Name })
$unlisted = @($present | Where-Object { $listed -notcontains $_ })
Assert-That (-not $unlisted.Count) "Message files not listed in [Languages]: $($unlisted -join ', ')."
Write-Output "PASS installer script: stable AppId and setup mutex, English fallback first and $($languages.Count) message files with matching keys and placeholders ($script:checks checks)."
