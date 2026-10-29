# EpoCanva W3-A -- isolation COM/category removal. NOT RUN IN THIS ROUND.
#
# Removes only the keys owned by this product: the CLSID's InprocServer32, the
# GUID_TFCAT_TIP_KEYBOARD category entry and the EpoCanvaProduct marker. It refuses a CLSID
# that does not carry the product marker and does not point at epocanva_tsf.dll, so another
# input method's registration can never be deleted here. It does not touch language
# profiles (use epocanva_tip_profile_helper.exe unregister) or input method settings.
#
#   powershell -File tools/product/isolation/unregister_tip.ps1 `
#       -Hive HKLM -AuthorizedIsolationEnvironment -EvidencePath <evidence>\removal.json
[CmdletBinding()]
param(
    [ValidateSet('HKLM', 'HKCU')][string]$Hive = 'HKLM',
    [switch]$AuthorizedIsolationEnvironment,
    [string]$EvidencePath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$clsid = '{B440F702-63E4-4D47-BCC4-074899ABA836}'
$profile = '{7D3E2A94-5C61-4F0B-9E3A-1B8D6C4F2A71}'
$category = '{34745C63-B2F0-4784-8B67-5E12C8701A31}'
$markerName = 'EpoCanvaProduct'
$markerValue = 'epocanva_tsf'

if (-not $AuthorizedIsolationEnvironment) {
    throw 'Refusing to remove registration: pass -AuthorizedIsolationEnvironment only on the authorized isolation machine.'
}
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if ($Hive -eq 'HKLM' -and -not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'HKLM removal requires an elevated shell.'
}

if ($Hive -eq 'HKLM') {
    $basePath = "Registry::HKEY_LOCAL_MACHINE\Software\Classes\CLSID\$clsid"
} else {
    $basePath = "Registry::HKEY_CURRENT_USER\Software\Classes\CLSID\$clsid"
}
$inprocPath = Join-Path $basePath 'InprocServer32'
$categoryPath = Join-Path (Join-Path $basePath 'Implemented Categories') $category

$operations = New-Object System.Collections.Generic.List[string]
function Record([string]$operation, [string]$path, [string]$result) {
    $line = '{0} | {1} | {2}' -f $operation, $path, $result
    $operations.Add($line)
    Write-Host $line
}

$existedBefore = Test-Path -LiteralPath $basePath
$defaultRead = $null
$markerRead = $null
$categoryBefore = $false
if ($existedBefore) {
    if (Test-Path -LiteralPath $inprocPath) {
        $defaultRead = (Get-Item -LiteralPath $inprocPath).GetValue('')
    }
    $markerRead = (Get-Item -LiteralPath $basePath).GetValue($markerName)
    $categoryBefore = Test-Path -LiteralPath $categoryPath
}

if (-not $existedBefore) {
    Write-Host "nothing to remove at $basePath"
} else {
    $owned = ($markerRead -eq $markerValue) -or
             ($null -ne $defaultRead -and $defaultRead -like '*epocanva_tsf.dll')
    if (-not $owned) {
        throw "Refusing to remove ${basePath}: it does not carry the EpoCanva marker or DLL path (marker='$markerRead', InprocServer32='$defaultRead')."
    }
    if ($categoryBefore) {
        Remove-Item -LiteralPath $categoryPath -Recurse -Force
        Record 'remove-key' $categoryPath 'ok'
    }
    if ($null -ne $markerRead) {
        Remove-ItemProperty -LiteralPath $basePath -Name $markerName -Force
        Record 'remove-value' ($basePath + '\\' + $markerName) 'ok'
    }
    if (Test-Path -LiteralPath $inprocPath) {
        Remove-Item -LiteralPath $inprocPath -Recurse -Force
        Record 'remove-key' $inprocPath 'ok'
    }
    # The CLSID key itself is removed only when nothing else remains in it.
    $remainingValues = @((Get-Item -LiteralPath $basePath).GetValueNames() |
        Where-Object { $_ -ne '' })
    $remainingKeys = @(Get-ChildItem -LiteralPath $basePath -ErrorAction SilentlyContinue)
    if ($remainingValues.Count -eq 0 -and $remainingKeys.Count -eq 0) {
        Remove-Item -LiteralPath $basePath -Recurse -Force
        Record 'remove-key' $basePath 'ok'
    } else {
        Record 'keep-key' $basePath 'other values or subkeys remain'
    }
}

# Verification: the read state, not the deletion call, is the claim.
$baseAfter = Test-Path -LiteralPath $basePath
$inprocAfter = Test-Path -LiteralPath $inprocPath
$categoryAfter = Test-Path -LiteralPath $categoryPath
$markerAfter = $null
if ($baseAfter) {
    $markerAfter = (Get-Item -LiteralPath $basePath).GetValue($markerName)
}
Write-Host "verify: base_present=$baseAfter inproc_present=$inprocAfter category_present=$categoryAfter marker='$markerAfter'"
if ($inprocAfter -or $categoryAfter -or $markerAfter -eq $markerValue) {
    throw 'verification failed: product registration content still remains'
}
Record 'verify' $basePath 'ok'

$evidence = [ordered]@{
    timestamp = (Get-Date).ToString('o')
    computer = $env:COMPUTERNAME
    user = $env:USERNAME
    hive = $Hive
    clsid = $clsid
    profile_guid = $profile
    category = $category
    existed_before = $existedBefore
    previous_default = $defaultRead
    previous_marker = $markerRead
    previous_category = $categoryBefore
    base_present_after = $baseAfter
    inproc_present_after = $inprocAfter
    category_present_after = $categoryAfter
    operations = @($operations)
}
if ($EvidencePath) {
    $parent = Split-Path -Parent $EvidencePath
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    $evidence | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $EvidencePath -Encoding UTF8
    Write-Host "evidence: $EvidencePath"
}

Write-Host 'Also run: epocanva_tip_profile_helper.exe unregister'
Write-Host 'Then compare capture_state.ps1 before/after reports and confirm the input method list.'
