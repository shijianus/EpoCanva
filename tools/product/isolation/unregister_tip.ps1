# EpoCanva W2-B-R -- W3 isolation rollback template. NOT RUN IN THIS ROUND.
[CmdletBinding()]
param(
    [ValidateSet('HKLM', 'HKCU')][string]$Hive = 'HKLM',
    [switch]$AuthorizedIsolationEnvironment
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $AuthorizedIsolationEnvironment) {
    throw 'Refusing to remove registration: pass -AuthorizedIsolationEnvironment only on the authorized isolation machine.'
}
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if ($Hive -eq 'HKLM' -and -not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'HKLM removal requires an elevated shell.'
}

$clsid = '{B440F702-63E4-4D47-BCC4-074899ABA836}'
if ($Hive -eq 'HKLM') {
    $base = "Registry::HKEY_LOCAL_MACHINE\Software\Classes\CLSID\$clsid"
} else {
    $base = "Registry::HKEY_CURRENT_USER\Software\Classes\CLSID\$clsid"
}
if (Test-Path -LiteralPath $base) {
    Remove-Item -LiteralPath $base -Recurse -Force
    Write-Host "Removed $base"
} else {
    Write-Host "Nothing to remove at $base"
}
Write-Host 'Also run: tip_profile_helper.exe unregister'
Write-Host 'Then verify the input-method list and compare against the pre-test registry snapshot.'
