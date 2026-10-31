param(
  [Parameter(Mandatory = $true)][string]$EvidenceDir,
  [Parameter(Mandatory = $true)][string]$SessionId,
  [Parameter(Mandatory = $true)][string]$DeadlineUtc,
  [Parameter(Mandatory = $true)][string]$StageDir,
  [string]$RepoRoot = '',
  [switch]$TraceBuild
)

$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Path $EvidenceDir -Force | Out-Null
$summaryPath = Join-Path $EvidenceDir 'elevated-summary.txt'
Set-Content -Path $summaryPath -Value ('EpoCanva local C-layer run session=' + $SessionId) -Encoding UTF8
function Log([string]$m) {
  $line = '{0} {1}' -f (Get-Date).ToString('s'), $m
  Add-Content -Path $summaryPath -Value $line -Encoding UTF8
  Write-Output $line
}
$deadline = [datetime]::Parse($DeadlineUtc).ToUniversalTime()
if (-not $StageDir -or -not (Test-Path -LiteralPath $StageDir -PathType Container)) {
  Log ('FAIL: missing immutable stage directory: ' + $StageDir)
  exit 5
}
if ((Get-Date).ToUniversalTime() -ge $deadline) {
  Log 'INVALID: the session deadline passed before registration; no registration and no input.'
  exit 8
}
Log ('session_id=' + $SessionId)
Log ('deadline_utc=' + $deadline.ToString('o'))
Log ('stage_dir=' + $StageDir)
$preflightFlag = Join-Path $EvidenceDir 'preflight-ready.flag'
$preflightText = ''
try { $preflightText = (Get-Content -LiteralPath $preflightFlag -Raw -ErrorAction Stop).Trim() } catch { }
if ($preflightText -ne $SessionId) {
  Log 'FAIL: preflight-ready flag is missing or belongs to another session; refusing to register'
  exit 5
}
Log 'preflight_ready=True'

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class EpoFgRelay {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool altTab);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("kernel32.dll")] public static extern uint GetTickCount();
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct LASTINPUTINFO { public uint cbSize; public uint dwTime; }
  [DllImport("user32.dll")] public static extern bool GetLastInputInfo(ref LASTINPUTINFO plii);
}
'@

function Get-IdleMs {
  $lii = New-Object EpoFgRelay+LASTINPUTINFO
  $lii.cbSize = [Runtime.InteropServices.Marshal]::SizeOf([type][EpoFgRelay+LASTINPUTINFO])
  if (-not [EpoFgRelay]::GetLastInputInfo([ref]$lii)) { return 0 }
  return [int]([EpoFgRelay]::GetTickCount() - $lii.dwTime)
}

function Set-TestWindowForeground([IntPtr]$hwnd) {
  [void][EpoFgRelay]::ShowWindow($hwnd, 9)
  [EpoFgRelay]::SwitchToThisWindow($hwnd, $true)
  Start-Sleep -Milliseconds 300
  $fg = [EpoFgRelay]::GetForegroundWindow()
  if ($fg -ne $hwnd) {
    [uint32]$fgPid = 0
    $fgThread = [EpoFgRelay]::GetWindowThreadProcessId($fg, [ref]$fgPid)
    [uint32]$targetPid = 0
    $targetThread = [EpoFgRelay]::GetWindowThreadProcessId($hwnd, [ref]$targetPid)
    $curThread = [EpoFgRelay]::GetCurrentThreadId()
    [void][EpoFgRelay]::AttachThreadInput($curThread, $fgThread, $true)
    if ($targetThread -ne 0) { [void][EpoFgRelay]::AttachThreadInput($curThread, $targetThread, $true) }
    [void][EpoFgRelay]::SetForegroundWindow($hwnd)
    [void][EpoFgRelay]::BringWindowToTop($hwnd)
    [void][EpoFgRelay]::AttachThreadInput($curThread, $fgThread, $false)
    if ($targetThread -ne 0) { [void][EpoFgRelay]::AttachThreadInput($curThread, $targetThread, $false) }
    Start-Sleep -Milliseconds 300
    $fg = [EpoFgRelay]::GetForegroundWindow()
  }
  return $fg
}

$repo = if ($RepoRoot) { (Resolve-Path -LiteralPath $RepoRoot).Path } else { (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }
$dll = Join-Path $StageDir 'epocanva_tsf.dll'
$stageExe = Join-Path $StageDir 'EpoCanva.exe'
$helper = Join-Path $repo 'build\w3a-isolation-tools\isolation-tools\Release\epocanva_tip_profile_helper.exe'
$register = Join-Path $repo 'tools\product\isolation\register_tip.ps1'
$unregister = Join-Path $repo 'tools\product\isolation\unregister_tip.ps1'
$clsid = '{B440F702-63E4-4D47-BCC4-074899ABA836}'
$category = '{34745C63-B2F0-4784-8B67-5E12C8701A31}'
$uiElementCategory = '{49D2F9CF-1F5E-11D7-A6D3-00065B84435C}'

# A stage is a complete product layout, never a bare DLL copy. Verify every load-bearing
# file and run the package's own --diagnose before touching registration.
$required = @(
  'EpoCanva.exe',
  'epocanva_tsf.dll',
  'rime.dll',
  'package.manifest',
  'data\default.yaml',
  'data\epocanva_english.schema.yaml',
  'data\lexicon\epocanva_en_alpha.tsv'
)
foreach ($relative in $required) {
  $path = Join-Path $StageDir $relative
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    Log ('FAIL: incomplete stage, missing ' + $relative)
    exit 5
  }
}
$diagnosePath = Join-Path $EvidenceDir 'diagnose.txt'
& $stageExe --diagnose $diagnosePath | Out-Null
$diagnoseExit = $LASTEXITCODE
Log ('diagnose_exit=' + $diagnoseExit)
if (-not (Test-Path -LiteralPath $diagnosePath)) {
  Log 'FAIL: --diagnose produced no report'
  exit 5
}
$diagnoseText = Get-Content -LiteralPath $diagnosePath -Raw -Encoding UTF8
foreach ($field in @('package=ready', 'manifest=ok', 'manifest_failures=0',
                     'manifest_files=21')) {
  if ($diagnoseText -notmatch [regex]::Escape($field)) {
    Log ('FAIL: diagnose is missing ' + $field)
    exit 5
  }
}
$commitLine = ($diagnoseText -split "`n" | Where-Object { $_ -match '^commit=' } | Select-Object -First 1)
Log ('diagnose_source_identity=' + $commitLine)
if (-not (Test-Path -LiteralPath $dll)) {
  Log ('FAIL: missing stage DLL ' + $dll)
  exit 5
}
if (-not (Test-Path -LiteralPath $helper)) {
  Log ('FAIL: missing helper ' + $helper)
  exit 5
}

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Log ('is_admin=' + $isAdmin)
if (-not $isAdmin) { Log 'FAIL: this script must run elevated'; exit 5 }
$hash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash
Log ('dll_path=' + $dll)
Log ('dll_sha256=' + $hash)
Log ('trace_build=' + [bool]$TraceBuild)
$diagnoseVersion = ($diagnoseText -split "`n" | Where-Object { $_ -match '^version=' } | Select-Object -First 1)
Log ('diagnose_version=' + $diagnoseVersion)
$os = Get-CimInstance Win32_OperatingSystem
Log ('os=' + $os.Caption + ' ' + $os.Version + ' ' + $os.BuildNumber)
Log ('session=' + (Get-Process -Id $PID).SessionId)

$hklmCom = "Registry::HKEY_LOCAL_MACHINE\Software\Classes\CLSID\$clsid"
$hkcuCom = "Registry::HKEY_CURRENT_USER\Software\Classes\CLSID\$clsid"
$hklmTip = "Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\CTF\TIP\$clsid"
$hkcuTip = "Registry::HKEY_CURRENT_USER\SOFTWARE\Microsoft\CTF\TIP\$clsid"
$hklmInproc = Join-Path $hklmCom 'InprocServer32'
$hklmCategories = Join-Path $hklmCom 'Implemented Categories'
$hklmCategory = Join-Path $hklmCategories $category
$hklmUiElementCategory = Join-Path $hklmCategories $uiElementCategory
$hkcuCategories = Join-Path $hkcuCom 'Implemented Categories'
$hkcuCategory = Join-Path $hkcuCategories $category
$hkcuUiElementCategory = Join-Path $hkcuCategories $uiElementCategory

Log ('pre_hklm_com_base=' + (Test-Path -LiteralPath $hklmCom))
Log ('pre_hklm_inproc=' + (Test-Path -LiteralPath $hklmInproc))
Log ('pre_hklm_category=' + (Test-Path -LiteralPath $hklmCategory))
Log ('pre_hklm_ui_element_category=' + (Test-Path -LiteralPath $hklmUiElementCategory))
Log ('pre_hkcu_com=' + (Test-Path -LiteralPath $hkcuCom))
Log ('pre_hklm_tip=' + (Test-Path -LiteralPath $hklmTip))
Log ('pre_hkcu_tip=' + (Test-Path -LiteralPath $hkcuTip))
if ((Test-Path -LiteralPath $hklmInproc) -or (Test-Path -LiteralPath $hklmCategory) -or
    (Test-Path -LiteralPath $hklmUiElementCategory) -or (Test-Path -LiteralPath $hklmTip)) {
  Log 'FAIL: product registration exists before this run; refusing to overwrite'
  exit 5
}
if ((Test-Path -LiteralPath $hkcuCom) -or (Test-Path -LiteralPath $hkcuTip)) {
  Log 'FAIL: per-user product keys exist; clean them first'
  exit 5
}
if (Test-Path -LiteralPath $hklmCom) {
  $subs = @(Get-ChildItem -LiteralPath $hklmCom -ErrorAction SilentlyContinue)
  $vals = @((Get-Item -LiteralPath $hklmCom).GetValueNames() | Where-Object { $_ -ne '' })
  if ($subs.Count -eq 0 -and $vals.Count -eq 0) {
    Remove-Item -LiteralPath $hklmCom -Recurse -Force -ErrorAction SilentlyContinue
    Log 'preclean_empty_clsid_base=removed'
  } else {
    Remove-Item -LiteralPath (Join-Path $hklmCom 'Implemented Categories') -Recurse -Force -ErrorAction SilentlyContinue
    Log 'preclean_implemented_categories=removed'
  }
}
if (Test-Path -LiteralPath $hklmCom) {
  $subs = @(Get-ChildItem -LiteralPath $hklmCom -ErrorAction SilentlyContinue)
  $vals = @((Get-Item -LiteralPath $hklmCom).GetValueNames() | Where-Object { $_ -ne '' })
  if ($subs.Count -eq 0 -and $vals.Count -eq 0) { Remove-Item -LiteralPath $hklmCom -Force -ErrorAction SilentlyContinue }
  Log ('pre_hklm_com_base_after_clean=' + (Test-Path -LiteralPath $hklmCom))
}

function Get-HelperStatus {
  $old = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  $out = & $helper status 2>&1
  $ErrorActionPreference = $old
  return @($out)
}
function Wait-StatusPattern([string]$pattern, [int]$seconds) {
  $deadline = (Get-Date).AddSeconds($seconds)
  $last = @()
  while ((Get-Date) -lt $deadline) {
    $last = Get-HelperStatus
    if (($last -join "`n") -match $pattern) { return $last }
    Start-Sleep -Milliseconds 400
  }
  return $last
}

$preStatus = Get-HelperStatus
$preStatus | Out-File -FilePath (Join-Path $EvidenceDir 'status-pre.txt') -Encoding UTF8
$preStatus | ForEach-Object { Log ('pre_status: ' + $_) }

$agentExit = 99
$cleanupVerified = $false
try {
  $regOut = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $register -DllPath $dll -Hive HKLM -AuthorizedIsolationEnvironment -EvidencePath (Join-Path $EvidenceDir 'registration.json') 2>&1
  $registrationExit = $LASTEXITCODE
  $regOut | Out-File -FilePath (Join-Path $EvidenceDir 'register-output.txt') -Encoding UTF8
  $regOut | ForEach-Object { Log ('register: ' + $_) }
  Log ('registration_exit=' + $registrationExit)
  if ($registrationExit -ne 0) { throw 'COM/category registration failed' }

  $helperReg = & $helper register 2>&1
  $helperRegisterExit = $LASTEXITCODE
  $helperReg | Out-File -FilePath (Join-Path $EvidenceDir 'helper-register.txt') -Encoding UTF8
  $helperReg | ForEach-Object { Log ('helper_register: ' + $_) }
  Log ('helper_register_exit=' + $helperRegisterExit)
  $regText = ($helperReg -join "`n")
  if ($regText -notmatch 'Register CLSID\s+0x00000000' -or $regText -notmatch 'AddLanguageProfile en-US\s+0x00000000') {
    throw 'TSF Register/AddLanguageProfile did not return S_OK'
  }

  $afterReg = Wait-StatusPattern 'profile_present\s+yes' 15
  $afterReg | Out-File -FilePath (Join-Path $EvidenceDir 'status-after-register.txt') -Encoding UTF8
  $afterReg | ForEach-Object { Log ('after_register: ' + $_) }
  if (($afterReg -join "`n") -notmatch 'profile_present\s+yes') { throw 'profile did not become visible within 15s' }

  $activateOut = & $helper activate 2>&1
  $activateOut | Out-File -FilePath (Join-Path $EvidenceDir 'helper-activate.txt') -Encoding UTF8
  $activateOut | ForEach-Object { Log ('activate: ' + $_) }
  $activateText = ($activateOut -join "`n")
  $activateHrOk = $activateText -match 'ActivateProfile\s+0x00000000'
  Log ('activate_hresult_ok=' + $activateHrOk)
  $afterActivate = Get-HelperStatus
  $afterActivate | Out-File -FilePath (Join-Path $EvidenceDir 'status-after-activate.txt') -Encoding UTF8
  $afterActivate | ForEach-Object { Log ('after_activate: ' + $_) }
  $afterActivateText = ($afterActivate -join "`n")
  Log ('activate_active_keyboard_reported=' + ($afterActivateText -match 'profile_active_keyboard\s+yes'))
  if (-not $activateHrOk -or $afterActivateText -notmatch 'profile_present\s+yes' -or $afterActivateText -notmatch 'profile_enabled\s+yes') {
    throw 'profile activation did not leave an enabled en-US profile'
  }

  # Leftover Notepad processes are reported, never killed: they may be the user's own
  # documents. The agent creates and closes its own unique window.
  $leftovers = @(Get-Process Notepad -ErrorAction SilentlyContinue)
  Log ('notepad_processes_present=' + $leftovers.Count)
  Start-Sleep -Milliseconds 700

  # The test agent runs outside this elevated process (normal integrity), started by the
  # non-elevated trigger. This elevated process only registers/activates and later cleans up.
  Set-Content -Path (Join-Path $EvidenceDir 'registration-ready.flag') -Value $SessionId -Encoding ASCII
  # The elevated process does not touch the foreground or send input: elevation is only for
  # COM/profile registration and removal. The normal-integrity agent owns the test window.
  Log 'foreground_relay=disabled (normal-integrity agent sets its own window)'
  while ((Get-Date).ToUniversalTime() -lt $deadline) {
    if (Test-Path -LiteralPath (Join-Path $EvidenceDir 'agent-done.flag')) {
      $doneText = ''
      try { $doneText = (Get-Content -LiteralPath (Join-Path $EvidenceDir 'agent-done.flag') -Raw -ErrorAction Stop).Trim() } catch { }
      if ($doneText -ne $SessionId) {
        Log 'INVALID: agent-done flag belongs to another session'
      }
      break
    }
    if (Test-Path -LiteralPath (Join-Path $EvidenceDir 'stop.flag')) {
      Log 'stop flag seen before agent completion; ending wait'
      break
    }
    Start-Sleep -Milliseconds 500
  }
  Log ('agent_done_seen=' + (Test-Path -LiteralPath (Join-Path $EvidenceDir 'agent-done.flag')))
  $windowText = ''
  $startText = ''
  try { $windowText = (Get-Content -LiteralPath (Join-Path $EvidenceDir 'test-window-ready.flag') -Raw -ErrorAction Stop).Trim() } catch { }
  try { $startText = (Get-Content -LiteralPath (Join-Path $EvidenceDir 'test-start.flag') -Raw -ErrorAction Stop).Trim() } catch { }
  Log ('test_window_ready=' + ($windowText -eq $SessionId))
  Log ('test_start=' + ($startText -eq $SessionId))
  if ((Get-Date).ToUniversalTime() -ge $deadline) {
    Log 'INVALID: orchestrator deadline reached while waiting for the agent'
    Set-Content -Path (Join-Path $EvidenceDir 'stop.flag') -Value $SessionId -Encoding ASCII
  }
} catch {
  Log ('run_error=' + $_.Exception.Message)
} finally {
  $ErrorActionPreference = 'Continue'
  $deactivateOut = & $helper deactivate 2>&1
  $deactivateOut | Out-File -FilePath (Join-Path $EvidenceDir 'helper-deactivate.txt') -Encoding UTF8
  $deactivateOut | ForEach-Object { Log ('deactivate: ' + $_) }
  $unregHelperOut = & $helper unregister 2>&1
  $unregHelperOut | Out-File -FilePath (Join-Path $EvidenceDir 'helper-unregister.txt') -Encoding UTF8
  $unregHelperOut | ForEach-Object { Log ('helper_unregister: ' + $_) }
  $unregOut = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $unregister -Hive HKLM -AuthorizedIsolationEnvironment -EvidencePath (Join-Path $EvidenceDir 'removal.json') 2>&1
  $unregOut | Out-File -FilePath (Join-Path $EvidenceDir 'unregister-output.txt') -Encoding UTF8
  $unregOut | ForEach-Object { Log ('unregister: ' + $_) }

  if (Test-Path -LiteralPath $hklmCategories) {
    $implSubs = @(Get-ChildItem -LiteralPath $hklmCategories -ErrorAction SilentlyContinue)
    if ($implSubs.Count -eq 0) { Remove-Item -LiteralPath $hklmCategories -Force -ErrorAction SilentlyContinue }
  }
  if (Test-Path -LiteralPath $hklmCom) {
    $subs = @(Get-ChildItem -LiteralPath $hklmCom -ErrorAction SilentlyContinue)
    $vals = @((Get-Item -LiteralPath $hklmCom).GetValueNames() | Where-Object { $_ -ne '' })
    if ($subs.Count -eq 0 -and $vals.Count -eq 0) { Remove-Item -LiteralPath $hklmCom -Force -ErrorAction SilentlyContinue }
  }
  if (Test-Path -LiteralPath $hklmTip) { Remove-Item -LiteralPath $hklmTip -Recurse -Force -ErrorAction SilentlyContinue }
  if (Test-Path -LiteralPath $hkcuTip) { Remove-Item -LiteralPath $hkcuTip -Recurse -Force -ErrorAction SilentlyContinue }

  $postStatus = Wait-StatusPattern 'profile_present\s+no' 8
  $postStatus | Out-File -FilePath (Join-Path $EvidenceDir 'status-post.txt') -Encoding UTF8
  $postStatus | ForEach-Object { Log ('post_status: ' + $_) }
  $postText = ($postStatus -join "`n")
  $postHklmCom = Test-Path -LiteralPath $hklmCom
  $postHkcuCom = Test-Path -LiteralPath $hkcuCom
  $postHklmTip = Test-Path -LiteralPath $hklmTip
  $postHkcuTip = Test-Path -LiteralPath $hkcuTip
  $postHklmCategory = Test-Path -LiteralPath $hklmCategory
  $postHklmUiElementCategory = Test-Path -LiteralPath $hklmUiElementCategory
  $postHkcuCategory = Test-Path -LiteralPath $hkcuCategory
  $postHkcuUiElementCategory = Test-Path -LiteralPath $hkcuUiElementCategory
  Log ('post_hklm_com=' + $postHklmCom)
  Log ('post_hkcu_com=' + $postHkcuCom)
  Log ('post_hklm_tip=' + $postHklmTip)
  Log ('post_hkcu_tip=' + $postHkcuTip)
  Log ('post_hklm_category=' + $postHklmCategory)
  Log ('post_hklm_ui_element_category=' + $postHklmUiElementCategory)
  Log ('post_hkcu_category=' + $postHkcuCategory)
  Log ('post_hkcu_ui_element_category=' + $postHkcuUiElementCategory)
  $profileGone = ($postText -match 'profile_present\s+no') -and ($postText -match 'profile_active_keyboard\s+no')
  $cleanupVerified = (-not $postHklmCom) -and (-not $postHkcuCom) -and
                     (-not $postHklmTip) -and (-not $postHkcuTip) -and
                     (-not $postHklmCategory) -and (-not $postHklmUiElementCategory) -and
                     (-not $postHkcuCategory) -and (-not $postHkcuUiElementCategory) -and
                     $profileGone
  Log ('cleanup_verified=' + $cleanupVerified)
  Set-Content -Path (Join-Path $EvidenceDir 'cleanup-complete.flag') -Value $SessionId -Encoding ASCII
  Set-Content -Path (Join-Path $EvidenceDir 'cleanup-done.flag') -Value $SessionId -Encoding ASCII
}

$agentPassed = $false
$agentInvalid = $false
$agentResultPath = Join-Path $EvidenceDir 'agent-result.json'
if (Test-Path -LiteralPath $agentResultPath) {
  try {
    $agentResult = Get-Content -LiteralPath $agentResultPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $agentPassed = [bool]$agentResult.passed
    $agentInvalid = [bool]$agentResult.invalid_round
    if ($agentResult.session_id -and $agentResult.session_id -ne $SessionId) {
      Log 'INVALID: agent result belongs to another session'
      $agentInvalid = $true
    }
  } catch { Log ('agent_result_parse_error=' + $_.Exception.Message) }
}
Log ('agent_passed=' + $agentPassed)
Log ('agent_invalid_round=' + $agentInvalid)
$overall = ($agentPassed -and $cleanupVerified -and (-not $agentInvalid))
Log ('overall=' + $overall)
if ($overall) { exit 0 } elseif ($agentInvalid) { exit 8 } elseif (-not $cleanupVerified) { exit 6 } else { exit 3 }
