# EpoCanva W2-B-R -- W3 isolation registration template. NOT RUN IN THIS ROUND.
#
# This script writes the minimum registry entries for an in-proc TSF keyboard text service.
# It deliberately does NOT activate the input method and does NOT touch the user's main
# system. It refuses to run without the explicit authorization switch and (for HKLM)
# elevation. Run it only on an authorized, snapshotted isolation machine.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DllPath,
    [ValidateSet('HKLM', 'HKCU')][string]$Hive = 'HKLM',
    [switch]$AuthorizedIsolationEnvironment
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $AuthorizedIsolationEnvironment) {
    throw 'Refusing to write registration: pass -AuthorizedIsolationEnvironment only on the authorized isolation machine.'
}
if (-not (Test-Path -LiteralPath $DllPath -PathType Leaf)) {
    throw "The staged product DLL does not exist: $DllPath"
}
$DllPath = (Resolve-Path -LiteralPath $DllPath).Path
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if ($Hive -eq 'HKLM' -and -not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'HKLM registration requires an elevated shell. Use -Hive HKCU or elevate.'
}

$clsid = '{B440F702-63E4-4D47-BCC4-074899ABA836}'
$category = '{34745C63-B2F0-4784-8B67-5E12C8701A31}'   # GUID_TFCAT_TIP_KEYBOARD

if ($Hive -eq 'HKLM') {
    $base = "Registry::HKEY_LOCAL_MACHINE\Software\Classes\CLSID\$clsid"
} else {
    $base = "Registry::HKEY_CURRENT_USER\Software\Classes\CLSID\$clsid"
}
$inproc = Join-Path $base 'InprocServer32'
$categories = Join-Path $base 'Implemented Categories'
$keyboard_cat = Join-Path $categories $category

New-Item -Path $base -Force | Out-Null
New-Item -Path $inproc -Force | Out-Null
Set-Item -Path $inproc -Value $DllPath
New-ItemProperty -Path $inproc -Name 'ThreadingModel' -Value 'Apartment' -PropertyType String -Force | Out-Null
New-Item -Path $keyboard_cat -Force | Out-Null

Write-Host "Registered $clsid -> $DllPath ($Hive)"
Write-Host "Category: $category"
Write-Host ''
Write-Host 'Next steps (authorized machine only):'
Write-Host '  1. Build/run tip_profile_helper.exe register'
Write-Host '  2. Run tip_profile_helper.exe activate (or add the keyboard in Windows Settings)'
Write-Host '  3. Run the Layer C matrix in docs/windows/isolation-verification-plan.md'
Write-Host '  4. Run unregister_tip.ps1 and tip_profile_helper.exe unregister afterwards'
