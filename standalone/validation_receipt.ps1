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

function Get-TaxiPhysicalFilePath([string]$Path) {
    if (-not ('TaxiCam.InstallationPaths' -as [type])) {
        Add-Type -TypeDefinition '
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
namespace TaxiCam {
    public static class InstallationPaths {
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        private static extern uint GetFinalPathNameByHandleW(SafeFileHandle file, StringBuilder path, uint size, uint flags);
        public static string Resolve(SafeFileHandle file) {
            var path = new StringBuilder(32768);
            uint length = GetFinalPathNameByHandleW(file, path, (uint)path.Capacity, 0);
            if (length == 0 || length >= path.Capacity) throw new Win32Exception(Marshal.GetLastWin32Error());
            return path.ToString();
        }
    }
}'
    }
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    try { return [TaxiCam.InstallationPaths]::Resolve($stream.SafeFileHandle) }
    finally { $stream.Dispose() }
}
function Test-TaxiRedirectedInstallPath([string]$Path, [string]$PhysicalPath) {
    $cache = '\\AppData\\Local\\Packages\\[^\\]+\\LocalCache\\(?:Local|Roaming)\\'
    return ($PhysicalPath -match $cache -and [IO.Path]::GetFullPath($Path) -notmatch $cache)
}
function Assert-TaxiVisibleInstallPath([string]$Path) {
    $physical = Get-TaxiPhysicalFilePath $Path
    # Packaged desktop hosts can redirect AppData writes into their own cache.
    # Hash verification alone succeeds there, but Explorer and MSFS cannot see
    # the requested path. Reject before changing simulator startup.
    if (Test-TaxiRedirectedInstallPath $Path $physical) {
        throw "Windows redirected the installation into a packaged app cache: $physical. Choose a folder outside AppData, such as $env:USERPROFILE\Apps\Taxi Cam, or run the installer from a normal Windows terminal."
    }
}
