[CmdletBinding()]
param([string]$Root = '', [switch]$List, [switch]$LowerBaseline)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $Root) { $Root = Join-Path $PSScriptRoot '../..' }
$repoRoot = [IO.Path]::GetFullPath($Root)
$baselinePath = Join-Path $PSScriptRoot 'comment_baseline.json'
$script:checks = 0
function Assert-That([bool]$Condition, [string]$Message) {
    $script:checks++
    if (-not $Condition) { throw $Message }
}

$excludedPrefixes = @('.github/', 'build/', 'licenses/')
$excludedFiles = @('LICENSE', 'THIRD_PARTY_NOTICES.md', 'REVIEW.md')
$thirdPartyFiles = @()
$noCommentExtensions = @('.json', '.md', '.ico', '.png', '.jpg', '.txt')
$cFamilyExtensions = @('.cpp', '.hpp', '.h', '.c', '.rc', '.S', '.hlsl', '.js')
$powerShellExtensions = @('.ps1', '.psm1', '.psd1')
$hashExtensions = @('.cfg', '.clang-format', '.gitignore', '.gitattributes', '.yml', '.yaml', '.py', '.toml', '.ini')
$markupExtensions = @('.manifest', '.xml')
$toolDirectives = @('^//\s*clang-format (off|on)$', '^//\s*NOLINT(NEXTLINE)?(\([^)]*\))?$', '^#requires\s')

$singleline = [Text.RegularExpressions.RegexOptions]::Singleline
$cTokens = (
    '(?<raw>(?:u8|[uUL])?R"(?<delimiter>[^()\\\s"]{0,16})\((?<body>.*?)\)\k<delimiter>")' +
    '|(?<string>(?:u8|[uUL])?"(?:\\.|[^"\\\r\n])*")' +
    '|(?<template>`(?:\\.|[^`\\])*`)' +
    '|(?<char>(?<![\w.])(?:u8|[uUL])?''(?:\\.|[^''\\\r\n])*'')' +
    '|(?<comment>//[^\r\n]*|/\*.*?\*/)')
$assemblyHashComment = '|(?<comment>(?m:^|(?<=[ \t]))#(?![ \t]*(?:include|define|undef|if|ifdef|ifndef|elif|else|endif|pragma|error|warning|line)\b)[^\r\n]*)'
$cToken = New-Object Text.RegularExpressions.Regex ($cTokens), $singleline
$assemblyToken = New-Object Text.RegularExpressions.Regex ($cTokens + $assemblyHashComment), $singleline
$assemblyRawStringOpener = New-Object Text.RegularExpressions.Regex '(?:\basm|\b__asm__)\s*(?:volatile\s*)?\(\s*$'
$pascalToken = New-Object Text.RegularExpressions.Regex ('(?<string>''(?:[^''\r\n]|'''')*'')|(?<comment>//[^\r\n]*|\{.*?\}|\(\*.*?\*\))'), $singleline
$markupComment = New-Object Text.RegularExpressions.Regex ('<!--.*?-->'), $singleline
$newline = New-Object Text.RegularExpressions.Regex "`n"

function New-LineIndex([string]$Text) {
    $breaks = $newline.Matches($Text)
    $starts = New-Object 'int[]' ($breaks.Count + 1)
    $starts[0] = -1
    for ($i = 0; $i -lt $breaks.Count; $i++) { $starts[$i + 1] = $breaks[$i].Index }
    return ,$starts
}
function Get-LineNumber([int[]]$LineIndex, [int]$Offset) {
    $found = [Array]::BinarySearch($LineIndex, $Offset)
    if ($found -lt 0) { $found = -$found - 1 }
    return $found
}
function New-Comment([int]$Line, [string]$Text) {
    return [pscustomobject]@{ Line = $Line; Text = $Text.Trim() }
}
function Get-CComments([string]$Text, [int[]]$LineIndex = (New-LineIndex $Text), [int]$Offset = 0, [switch]$Assembly) {
    $tokens = if ($Assembly) { $assemblyToken } else { $cToken }
    foreach ($match in $tokens.Matches($Text)) {
        if ($match.Groups['comment'].Success) {
            New-Comment (Get-LineNumber $LineIndex ($Offset + $match.Index)) $match.Value
        } elseif ($match.Groups['raw'].Success) {
            $body = $match.Groups['body']
            $before = $Text.Substring([Math]::Max(0, $match.Index - 32), [Math]::Min(32, $match.Index))
            Get-CComments $body.Value $LineIndex ($Offset + $body.Index) -Assembly:($assemblyRawStringOpener.IsMatch($before))
        }
    }
}
function Get-PowerShellComments([string]$Text) {
    $errors = $null
    $previous = $null
    foreach ($token in [Management.Automation.PSParser]::Tokenize($Text, [ref]$errors)) {
        if ($token.Type -eq 'Comment') { New-Comment $token.StartLine $token.Content }
        if ($token.Type -eq 'String' -and $previous -and $previous.Type -eq 'CommandParameter' -and $previous.Content -eq '-TypeDefinition') {
            $firstSourceLine = if ($Text[$token.Start] -eq '@') { $token.StartLine + 1 } else { $token.StartLine }
            foreach ($comment in @(Get-CComments $token.Content)) { New-Comment ($firstSourceLine + $comment.Line - 1) $comment.Text }
        }
        if ($token.Type -ne 'NewLine' -and $token.Type -ne 'LineContinuation') { $previous = $token }
    }
}
function Get-LinePrefixComments([string]$Text, [string]$Prefix) {
    $lines = $Text -split "`r?`n"
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i].TrimStart().StartsWith($Prefix)) { New-Comment ($i + 1) $lines[$i] }
    }
}
function Get-InnoComments([string]$Text) {
    $lines = $Text -split "`r?`n"
    $code = $false
    $codeStart = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $trimmed = $lines[$i].Trim()
        if ($trimmed -match '^\[(\w+)\]$') {
            if ($code) { break }
            $code = $Matches[1] -eq 'Code'
            if ($code) { $codeStart = $i + 1 }
            continue
        }
        if (-not $code -and ($trimmed.StartsWith(';') -or $trimmed.StartsWith('//'))) { New-Comment ($i + 1) $lines[$i] }
    }
    if ($codeStart -ge 0) {
        $codeText = ($lines[$codeStart..($lines.Count - 1)] -join "`n")
        $lineIndex = New-LineIndex $codeText
        foreach ($match in $pascalToken.Matches($codeText)) {
            if ($match.Groups['comment'].Success) { New-Comment ($codeStart + (Get-LineNumber $lineIndex $match.Index)) $match.Value }
        }
    }
}
function Get-MarkupComments([string]$Text) {
    $lineIndex = New-LineIndex $Text
    foreach ($match in $markupComment.Matches($Text)) { New-Comment (Get-LineNumber $lineIndex $match.Index) $match.Value }
}
function Test-CommentRule([string]$Path) {
    $extension = [IO.Path]::GetExtension($Path)
    return ($cFamilyExtensions + $powerShellExtensions + $hashExtensions + $markupExtensions + $noCommentExtensions + @('.iss', '.isl')) -ccontains $extension
}
function Get-FileComments([string]$Path, [string]$Text) {
    $extension = [IO.Path]::GetExtension($Path)
    if ($extension -ceq '.S') { return @(Get-CComments $Text -Assembly) }
    if ($cFamilyExtensions -ccontains $extension) { return @(Get-CComments $Text) }
    if ($powerShellExtensions -contains $extension) { return @(Get-PowerShellComments $Text) }
    if ($extension -eq '.iss') { return @(Get-InnoComments $Text) }
    if ($extension -eq '.isl') { return @(Get-LinePrefixComments $Text ';') }
    if ($hashExtensions -contains $extension) { return @(Get-LinePrefixComments $Text '#') }
    if ($markupExtensions -contains $extension) { return @(Get-MarkupComments $Text) }
    if ($noCommentExtensions -contains $extension) { return @() }
    throw "No comment rule for $Path; add its file type to tests/repository/comments_test.ps1."
}
function Test-ToolDirective([string]$Comment) {
    foreach ($pattern in $toolDirectives) { if ($Comment -match $pattern) { return $true } }
    return $false
}
function Get-ProjectComments([string]$File, [string]$Text) {
    foreach ($comment in @(Get-FileComments $File $Text)) {
        if ($comment.Line -eq 1 -and $comment.Text.StartsWith('#!')) { continue }
        if (Test-ToolDirective $comment.Text) { continue }
        $comment
    }
}

function Get-Texts([object[]]$Comments) { return @($Comments | ForEach-Object { $_.Text }) }
$cSample = @'
auto url = "https://example.test/a//b"; auto quote = "\"// not a comment"; auto slash = '/';
int big = 1'000'000; auto wide = L'x'; // first
auto raw = R"x(text // shader
/* block */ ")x"; /* second
spans */ int after = 0;
'@
$found = @(Get-CComments $cSample)
Assert-That ((Get-Texts $found) -join '|' -ceq "// first|// shader|/* block */|/* second`nspans */") "C-family comments were [$((Get-Texts $found) -join '|')]."
Assert-That ($found[0].Line -eq 2 -and $found[1].Line -eq 3 -and $found[2].Line -eq 4 -and $found[3].Line -eq 4) 'C-family comment lines were wrong.'
Assert-That ((Get-Texts (Get-CComments 'const a = `x // not`, b = /\d+/.exec(s); // real')) -join '|' -ceq '// real') 'JavaScript template or regular-expression text was taken for a comment.'
Assert-That ((Get-Texts (Get-CComments "a = ''; done(); // after empty`nb = '' + ' // not';")) -join '|' -ceq '// after empty') 'An empty single-quoted string desynchronised the scanner.'
$asmSample = "#include `"x.h`"`n#define A 1`n  movq %rcx, %rdi # copy`n# whole line`n  .ascii `"# not`"`n"
Assert-That ((Get-Texts (Get-FileComments 'a.S' $asmSample)) -join '|' -ceq '# copy|# whole line') 'GAS # comments in .S files were missed.'
$asmRaw = "asm(R`"(`n  pushq %rbp # saved`n)`");`nconst char* shader = R`"(`n#define B 1`n)`";"
$asmRawFound = @(Get-CComments $asmRaw)
Assert-That ((Get-Texts $asmRawFound) -join '|' -ceq '# saved' -and $asmRawFound[0].Line -eq 2) 'GAS # comments in asm raw strings were missed, or shader directives were counted.'
$psSample = "#requires -Version 5.1`n`$a = '# not'; `$b = `"# not`" # trailing`n<# block #>`n@'`n# here-string`n'@"
Assert-That ((Get-Texts (Get-PowerShellComments $psSample)) -join '|' -ceq '#requires -Version 5.1|# trailing|<# block #>') 'PowerShell comments were wrong.'
$innoSample = "[Setup]`n; setup note`nAppName=x ; kept`n[Code]`nconst A = '{ not }'; // code note`n{ brace note }`n(* star note *)"
$innoFound = @(Get-InnoComments $innoSample)
Assert-That ((Get-Texts $innoFound) -join '|' -ceq '; setup note|// code note|{ brace note }|(* star note *)') 'Inno Setup comments were wrong.'
Assert-That ($innoFound[0].Line -eq 2 -and $innoFound[1].Line -eq 5 -and $innoFound[3].Line -eq 7) 'Inno Setup comment lines were wrong.'
Assert-That ((Get-Texts (Get-ProjectComments 'a.ps1' "#!/usr/bin/env pwsh`n#requires -Version 5.1`n# note`n#! later")) -join '|' -ceq '# note|#! later') 'Only a first-line shebang and #requires may be allowed.'
$typeSample = "`$a = 1`nAdd-Type -TypeDefinition @'`npublic static class A {`n    // embedded`n}`n'@`nAdd-Type -TypeDefinition 'class B { /* inline */ }'"
$typeFound = @(Get-PowerShellComments $typeSample)
Assert-That ((Get-Texts $typeFound) -join '|' -ceq '// embedded|/* inline */' -and $typeFound[0].Line -eq 4 -and $typeFound[1].Line -eq 7) "Add-Type source comments were [$((Get-Texts $typeFound) -join '|')] on lines [$(@($typeFound | ForEach-Object { $_.Line }) -join ',')]."
Assert-That ((Get-Texts (Get-ProjectComments 'a.cpp' "// clang-format off`n// NOLINT(bugprone-x)`n// clang-format off: why`n// NOLINT because")) -join '|' -ceq '// clang-format off: why|// NOLINT because') 'Tool directives with explanations were allowed.'
Assert-That ((Get-Texts (Get-ProjectComments 'a.manifest' "<a><!-- note --></a>")) -join '|' -ceq '<!-- note -->') 'Markup comments were missed.'
Assert-That ((Get-Texts (Get-ProjectComments 'a.cfg' "# note`nkey = 1 # kept")) -join '|' -ceq '# note') 'Configuration comments were missed.'
$thrown = $false
try { [void](Get-FileComments 'tools/script.lua' '') } catch { $thrown = $true }
Assert-That ($thrown -and -not (Test-CommentRule 'tools/script.lua') -and (Test-CommentRule 'src/a.cpp')) 'An unknown file type was not refused.'

$tracked = @(& git -C $repoRoot ls-files --cached)
if ($LASTEXITCODE -ne 0) { throw 'git ls-files failed.' }
$untracked = @(& git -C $repoRoot ls-files --others --exclude-standard)
if ($LASTEXITCODE -ne 0) { throw 'git ls-files failed.' }
$files = @($tracked) + @($untracked | Where-Object { Test-CommentRule $_ })
$utf8 = New-Object Text.UTF8Encoding($false)
$counts = @{}
$found = New-Object Collections.Generic.List[string]
$scanned = 0
foreach ($file in $files) {
    if ($excludedFiles -ccontains $file -or $thirdPartyFiles -ccontains $file) { continue }
    if (@($excludedPrefixes | Where-Object { $file.StartsWith($_) }).Count) { continue }
    $path = Join-Path $repoRoot $file
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
    $scanned++
    $comments = @(Get-ProjectComments $file ([IO.File]::ReadAllText($path, $utf8)))
    if (-not $comments.Count) { continue }
    $counts[$file] = $comments.Count
    foreach ($comment in $comments) { $found.Add("${file}:$($comment.Line): $(($comment.Text -split "`r?`n")[0])") }
}
if ($List) { $found; return }
Assert-That ($scanned -gt 0) 'No project files were scanned.'

$baseline = @{}
if (Test-Path -LiteralPath $baselinePath) {
    foreach ($entry in (Get-Content -Raw -LiteralPath $baselinePath | ConvertFrom-Json).PSObject.Properties) { $baseline[$entry.Name] = [int]$entry.Value }
}
if ($LowerBaseline) {
    $lowered = [ordered]@{}
    foreach ($file in @($baseline.Keys | Sort-Object)) {
        $count = [Math]::Min($baseline[$file], $(if ($counts.ContainsKey($file)) { $counts[$file] } else { 0 }))
        if ($count -gt 0) { $lowered[$file] = $count }
    }
    $entries = @($lowered.Keys | ForEach-Object { '    "' + $_ + '": ' + $lowered[$_] })
    [IO.File]::WriteAllText($baselinePath, "{`n" + ($entries -join ",`n") + "`n}`n", $utf8)
    Write-Output "Comment baseline lowered: $($lowered.Count) files keep $(@($lowered.Values | Measure-Object -Sum).Sum) existing comments."
    return
}
$failures = New-Object Collections.Generic.List[string]
foreach ($file in @($counts.Keys | Sort-Object)) {
    $allowed = if ($baseline.ContainsKey($file)) { $baseline[$file] } else { 0 }
    if ($counts[$file] -gt $allowed) {
        $failures.Add("${file}: $($counts[$file]) comments, $allowed allowed by the baseline. Remove the new comments:")
        foreach ($line in @($found | Where-Object { $_.StartsWith("${file}:") })) { $failures.Add("  $line") }
    }
}
foreach ($file in @($baseline.Keys | Sort-Object)) {
    $count = if ($counts.ContainsKey($file)) { $counts[$file] } else { 0 }
    if ($count -lt $baseline[$file]) {
        $failures.Add("${file}: the baseline allows $($baseline[$file]) comments but $count remain. Run tests/repository/comments_test.ps1 -LowerBaseline.")
    }
}
if ($failures.Count) { throw "Comments are not allowed in project files (AGENTS.md):`n$(@($failures | Select-Object -First 80) -join "`n")" }
$remaining = @($baseline.Values | Measure-Object -Sum).Sum
Write-Output "PASS comments: $scanned project files, no new comments; $($baseline.Count) files still hold $remaining existing comments in the baseline ($script:checks checks)."
