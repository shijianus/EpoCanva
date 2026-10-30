# EpoCanva W2-B-R -- read-only isolation state capture. Safe to run anywhere; writes only
# the requested report file. It never changes registration, input methods or services.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ReportPath,
    [string]$DllPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("captured: " + (Get-Date).ToString('s'))
$lines.Add("computer: " + $env:COMPUTERNAME)
$lines.Add("user: " + $env:USERNAME)
$os = Get-CimInstance Win32_OperatingSystem
$lines.Add("os: " + $os.Caption + " " + $os.Version + " build " + $os.BuildNumber)
$lines.Add("")
$lines.Add("modules:")
foreach ($name in @('textinputframework.dll', 'msctf.dll', 'rime.dll')) {
    $path = Join-Path $env:WINDIR ("System32\" + $name)
    if (Test-Path -LiteralPath $path) {
        $item = Get-Item -LiteralPath $path
        $lines.Add("  $name " + $item.VersionInfo.FileVersion + " " + $item.Length + " bytes")
    }
}
if ($DllPath -and (Test-Path -LiteralPath $DllPath -PathType Leaf)) {
    $hash = Get-FileHash -LiteralPath $DllPath -Algorithm SHA256
    $lines.Add("")
    $lines.Add("product_dll: " + (Resolve-Path -LiteralPath $DllPath).Path)
    $lines.Add("product_sha256: " + $hash.Hash)
    $version = (Get-Item -LiteralPath $DllPath).VersionInfo.FileVersion
    $lines.Add("product_version: " + $version)
}
$clsid = '{B440F702-63E4-4D47-BCC4-074899ABA836}'
$lines.Add("")
$lines.Add("registration snapshot (read-only):")
foreach ($hive in @('HKEY_LOCAL_MACHINE', 'HKEY_CURRENT_USER')) {
    $path = "Registry::$hive\Software\Classes\CLSID\$clsid"
    if (Test-Path -LiteralPath $path) {
        $lines.Add("  present: $path")
        $inprocKey = Get-Item -LiteralPath (Join-Path $path 'InprocServer32') -ErrorAction SilentlyContinue
        if ($null -ne $inprocKey) {
            $lines.Add("    InprocServer32=" + $inprocKey.GetValue('') + " ThreadingModel=" + $inprocKey.GetValue('ThreadingModel'))
        }
        foreach ($category in @(
            '{34745C63-B2F0-4784-8B67-5E12C8701A31}',
            '{49D2F9CF-1F5E-11D7-A6D3-00065B84435C}')) {
            $categoryPath = Join-Path (Join-Path $path 'Implemented Categories') $category
            $lines.Add("    category $category present=" + (Test-Path -LiteralPath $categoryPath))
        }
    } else {
        $lines.Add("  absent: $path")
    }
}
$lines | Set-Content -LiteralPath $ReportPath -Encoding UTF8
Write-Host "State captured to $ReportPath"
