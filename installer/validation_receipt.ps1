Set-StrictMode -Version Latest
function Assert-TaxiNativeReceipt([string]$Directory) {
    $receiptPath = Join-Path $Directory 'validation.json'
    $receipt = Get-Content -Raw -LiteralPath $receiptPath | ConvertFrom-Json
    if (-not $receipt.passed -or $receipt.version -notmatch '^\d+\.\d+\.\d+$' -or
        $null -eq $receipt.PSObject.Properties['buildNumber'] -or
        ($receipt.buildNumber -isnot [int] -and $receipt.buildNumber -isnot [long]) -or
        $receipt.buildNumber -lt 0 -or $receipt.buildNumber -gt [int]::MaxValue) {
        throw 'A successful native validation receipt with a build number is required.'
    }
    foreach ($name in @('taxi-cam.exe','taxi-camera-bridge.dll')) {
        $file = Join-Path $Directory $name
        $expected = $receipt.files.PSObject.Properties[$name].Value
        if (-not $expected -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $expected) { throw "Native binary differs from the validated build: $name" }
    }
    return $receipt
}
