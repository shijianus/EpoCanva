# EpoCanva W3-A -- isolation COM/category registration. NOT RUN IN THIS ROUND.
#
# Writes only the minimum COM/TSF category keys for the formal product CLSID:
#   HKLM|HKCU\Software\Classes\CLSID\{B440F702-...}\InprocServer32
#   HKLM|HKCU\Software\Classes\CLSID\{B440F702-...}\Implemented Categories\{34745C63-...}  (keyboard TIP)
#   HKLM|HKCU\Software\Classes\CLSID\{B440F702-...}\Implemented Categories\{49D2F9CF-...}  (UIElement-enabled TIP)
# It does NOT register or activate the language profile (use epocanva_tip_profile_helper),
# does NOT change the user's input method and refuses a CLSID owned by another product.
#
# Every write is read back and verified. On any failure the changes made by this run are
# rolled back. A pre-state/evidence JSON is written so removal can be checked against it.
#
# Requirements: an authorized, snapshotted isolation machine; elevation for HKLM.
#   powershell -File tools/product/isolation/register_tip.ps1 `
#       -DllPath <stage>\epocanva_tsf.dll -AuthorizedIsolationEnvironment `
#       -EvidencePath <evidence>\registration-before.json
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$DllPath,
    [ValidateSet('HKLM', 'HKCU')][string]$Hive = 'HKLM',
    [switch]$AuthorizedIsolationEnvironment,
    [string]$EvidencePath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$clsid = '{B440F702-63E4-4D47-BCC4-074899ABA836}'
$profile = '{7D3E2A94-5C61-4F0B-9E3A-1B8D6C4F2A71}'
$category = '{34745C63-B2F0-4784-8B67-5E12C8701A31}'   # GUID_TFCAT_TIP_KEYBOARD
# GUID_TFCAT_TIPCAP_UIELEMENTENABLED: the TIP uses ITfUIElementMgr and must be categorized
# so the host's UILess/visibility control applies (TSF UILess mode requirements).
$uiElementCategory = '{49D2F9CF-1F5E-11D7-A6D3-00065B84435C}'
$markerName = 'EpoCanvaProduct'
$markerValue = 'epocanva_tsf'

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

# The DLL must be the x64 product image. Reading the PE header is not a system change.
$bytes = [System.IO.File]::ReadAllBytes($DllPath)
if ($bytes.Length -lt 0x40) {
    throw 'The product DLL is too small to be a PE image.'
}
$peOffset = [System.BitConverter]::ToInt32($bytes, 0x3C)
if ($peOffset + 6 -gt $bytes.Length -or
    [System.BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) {
    throw 'The product DLL has no PE signature.'
}
$machine = [System.BitConverter]::ToUInt16($bytes, $peOffset + 4)
if ($machine -ne 0x8664) {
    throw ('The product DLL is not x64 (PE machine 0x{0:X4}).' -f $machine)
}
$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if ($null -ne $dumpbin) {
    $exports = & $dumpbin.Source /nologo /exports $DllPath 2>&1 | Out-String
    foreach ($required in @('DllGetClassObject', 'DllCanUnloadNow')) {
        if ($exports -notmatch [regex]::Escape($required)) {
            throw "The product DLL does not export $required."
        }
    }
    if ($exports -match 'DllRegisterServer') {
        Write-Host 'note: DllRegisterServer exists; this tool still performs the registration explicitly.'
    }
} else {
    Write-Warning 'dumpbin.exe is not on PATH; the export check was skipped. The profile helper records its own results.'
}

if ($Hive -eq 'HKLM') {
    $basePath = "Registry::HKEY_LOCAL_MACHINE\Software\Classes\CLSID\$clsid"
} else {
    $basePath = "Registry::HKEY_CURRENT_USER\Software\Classes\CLSID\$clsid"
}
$inprocPath = Join-Path $basePath 'InprocServer32'
$categoryRoot = Join-Path $basePath 'Implemented Categories'
$categoryPath = Join-Path $categoryRoot $category
$uiElementCategoryPath = Join-Path $categoryRoot $uiElementCategory

$existedBefore = Test-Path -LiteralPath $basePath
$previousDefault = $null
$previousThreading = $null
$previousMarker = $null
$previousCategory = $false
$previousUiElementCategory = $false
if ($existedBefore) {
    if (Test-Path -LiteralPath $inprocPath) {
        $inproc = Get-Item -LiteralPath $inprocPath
        $previousDefault = $inproc.GetValue('')
        $previousThreading = $inproc.GetValue('ThreadingModel')
    }
    $base = Get-Item -LiteralPath $basePath
    $previousMarker = $base.GetValue($markerName)
    $previousCategory = Test-Path -LiteralPath $categoryPath
    $previousUiElementCategory = Test-Path -LiteralPath $uiElementCategoryPath
    $owned = ($previousMarker -eq $markerValue) -or
             ($null -ne $previousDefault -and $previousDefault -like '*epocanva_tsf.dll')
    if (-not $owned) {
        throw "Refusing to modify ${basePath}: it belongs to another product (marker='$previousMarker', InprocServer32='$previousDefault')."
    }
    if ($null -ne $previousDefault -and $previousDefault -ne $DllPath) {
        Write-Host "note: replacing the previous product DLL path '$previousDefault' with '$DllPath'."
    }
    if ($null -ne $previousThreading -and $previousThreading -ne 'Apartment') {
        throw "Refusing to change ThreadingModel='$previousThreading' on an existing key."
    }
}

$createdBase = -not $existedBefore
$createdInproc = -not (Test-Path -LiteralPath $inprocPath)
$createdThreading = $false
$createdMarker = ($null -eq $previousMarker)
$createdCategory = -not $previousCategory
$createdUiElementCategory = -not $previousUiElementCategory
$operations = New-Object System.Collections.Generic.List[string]

function Record([string]$operation, [string]$path, [string]$value, [string]$result) {
    $line = '{0} | {1} | {2} | {3}' -f $operation, $path, $value, $result
    $operations.Add($line)
    Write-Host $line
}

function Rollback {
    Write-Host 'rolling back changes made by this run...' -ForegroundColor Yellow
    try {
        if ($createdCategory -and (Test-Path -LiteralPath $categoryPath)) {
            Remove-Item -LiteralPath $categoryPath -Recurse -Force
            Record 'rollback' $categoryPath '' 'removed'
        }
        if ($createdUiElementCategory -and (Test-Path -LiteralPath $uiElementCategoryPath)) {
            Remove-Item -LiteralPath $uiElementCategoryPath -Recurse -Force
            Record 'rollback' $uiElementCategoryPath '' 'removed'
        }
        if ($createdThreading -and (Test-Path -LiteralPath $inprocPath)) {
            Remove-ItemProperty -LiteralPath $inprocPath -Name 'ThreadingModel' -ErrorAction SilentlyContinue
            Record 'rollback' $inprocPath 'ThreadingModel' 'removed'
        }
        if ($null -eq $previousDefault -and (Test-Path -LiteralPath $inprocPath)) {
            Remove-ItemProperty -LiteralPath $inprocPath -Name '(default)' -ErrorAction SilentlyContinue
            Record 'rollback' $inprocPath '(default)' 'removed'
        } elseif ($null -ne $previousDefault -and (Test-Path -LiteralPath $inprocPath)) {
            Set-Item -LiteralPath $inprocPath -Value $previousDefault
            Record 'rollback' $inprocPath '(default)' 'restored'
        }
        if ($createdMarker -and (Test-Path -LiteralPath $basePath)) {
            Remove-ItemProperty -LiteralPath $basePath -Name $markerName -ErrorAction SilentlyContinue
            Record 'rollback' $basePath $markerName 'removed'
        }
        if ($createdInproc -and (Test-Path -LiteralPath $inprocPath)) {
            Remove-Item -LiteralPath $inprocPath -Recurse -Force
            Record 'rollback' $inprocPath '' 'removed'
        }
        if ($createdBase -and (Test-Path -LiteralPath $basePath)) {
            Remove-Item -LiteralPath $basePath -Recurse -Force
            Record 'rollback' $basePath '' 'removed'
        }
    } catch {
        Write-Host ('rollback incomplete: ' + $_.Exception.Message) -ForegroundColor Red
    }
}

try {
    if ($createdBase) {
        New-Item -Path $basePath -Force | Out-Null
        Record 'create-key' $basePath '' 'ok'
    }
    New-ItemProperty -Path $basePath -Name $markerName -Value $markerValue `
        -PropertyType String -Force | Out-Null
    Record 'set-value' $basePath $markerName 'ok'
    if ($createdInproc) {
        New-Item -Path $inprocPath -Force | Out-Null
        Record 'create-key' $inprocPath '' 'ok'
    }
    Set-Item -LiteralPath $inprocPath -Value $DllPath
    Record 'set-default' $inprocPath $DllPath 'ok'
    if ($null -eq $previousThreading) {
        New-ItemProperty -Path $inprocPath -Name 'ThreadingModel' -Value 'Apartment' `
            -PropertyType String -Force | Out-Null
        $createdThreading = $true
        Record 'set-value' $inprocPath 'ThreadingModel' 'ok'
    }
    if ($createdCategory) {
        New-Item -Path $categoryPath -Force | Out-Null
        Record 'create-key' $categoryPath '' 'ok'
    }
    if ($createdUiElementCategory) {
        New-Item -Path $uiElementCategoryPath -Force | Out-Null
        Record 'create-key' $uiElementCategoryPath '' 'ok'
    }

    # Read-back verification: the script's claim is the read value, not the write call.
    $inprocRead = Get-Item -LiteralPath $inprocPath
    $defaultRead = $inprocRead.GetValue('')
    $threadingRead = $inprocRead.GetValue('ThreadingModel')
    $markerRead = (Get-Item -LiteralPath $basePath).GetValue($markerName)
    $categoryRead = Test-Path -LiteralPath $categoryPath
    $uiElementCategoryRead = Test-Path -LiteralPath $uiElementCategoryPath
    if ($defaultRead -ne $DllPath) { throw "verification failed: InprocServer32='$defaultRead'" }
    if ($threadingRead -ne 'Apartment') { throw "verification failed: ThreadingModel='$threadingRead'" }
    if ($markerRead -ne $markerValue) { throw "verification failed: marker='$markerRead'" }
    if (-not $categoryRead) { throw 'verification failed: the keyboard category key is absent' }
    if (-not $uiElementCategoryRead) { throw 'verification failed: the UIElement category key is absent' }
    Record 'verify' $basePath $DllPath 'ok'
} catch {
    Rollback
    throw
}

$evidence = [ordered]@{
    timestamp = (Get-Date).ToString('o')
    computer = $env:COMPUTERNAME
    user = $env:USERNAME
    hive = $Hive
    product_dll = $DllPath
    product_sha256 = (Get-FileHash -LiteralPath $DllPath -Algorithm SHA256).Hash
    clsid = $clsid
    profile_guid = $profile
    category = $category
    ui_element_category = $uiElementCategory
    existed_before = $existedBefore
    previous_default = $previousDefault
    previous_threading = $previousThreading
    previous_marker = $previousMarker
    previous_category = $previousCategory
    previous_ui_element_category = $previousUiElementCategory
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

Write-Host ''
Write-Host 'Next steps (authorized machine only):'
Write-Host '  1. epocanva_tip_profile_helper.exe register'
Write-Host '  2. epocanva_tip_profile_helper.exe status   (profile_present=yes)'
Write-Host '  3. epocanva_tip_profile_helper.exe activate (profile_active_keyboard=yes)'
Write-Host '  4. Run the Layer C matrix in docs/windows/isolation-verification-plan.md'
Write-Host '  5. epocanva_tip_profile_helper.exe deactivate + unregister'
Write-Host '  6. unregister_tip.ps1 -AuthorizedIsolationEnvironment; compare capture_state.ps1 reports'
