# EpoCanva W3-A -- normal-integrity preflight primitives.
#
# This file is dot-sourced by the preflight runner and, later, by the formal agent. It
# performs no registration and no input. It launches only a unique empty Notepad, verifies
# that the window/process is new (never a reused user instance), closes only that owned
# window/process, and returns a structured record.
#
# All P/Invoke declarations below are Unicode and SetLastError=true. Raw BOOL returns and
# the immediately-read Win32 error are preserved in the result; no exception is swallowed.
$ErrorActionPreference = 'Continue'

if (-not ('LayerCPreflightNative' -as [type])) {
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class LayerCPreflightNative {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct STARTUPINFO {
    public int cb;
    public string lpReserved;
    public string lpDesktop;
    public string lpTitle;
    public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    public short wShowWindow, cbReserved2;
    public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
  }
  [StructLayout(LayoutKind.Sequential)]
  public struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
  public delegate bool EnumProc(IntPtr h, IntPtr l);

  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true, ExactSpelling = true)]
  public static extern bool CreateProcessW(string app, string cmd, IntPtr pa, IntPtr ta,
      bool inherit, uint flags, IntPtr env, string dir, ref STARTUPINFO si,
      out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll", SetLastError = true)]
  public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32.dll", SetLastError = true)]
  public static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll", SetLastError = true)]
  public static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);
  [DllImport("kernel32.dll")]
  public static extern IntPtr GetCurrentProcess();
  [DllImport("kernel32.dll")]
  public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll", SetLastError = true)]
  public static extern IntPtr GetProcessWindowStation();
  [DllImport("user32.dll", SetLastError = true)]
  public static extern IntPtr GetThreadDesktop(uint threadId);
  [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  public static extern bool GetUserObjectInformationW(IntPtr obj, int index, StringBuilder info,
      int length, out int needed);
  [DllImport("user32.dll", SetLastError = true)]
  public static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
  [DllImport("user32.dll", SetLastError = true)]
  public static extern bool CloseDesktop(IntPtr desktop);
  [DllImport("user32.dll")]
  public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")]
  public static extern IntPtr GetShellWindow();
  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")]
  public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")]
  public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")]
  public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)]
  public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("advapi32.dll", SetLastError = true)]
  public static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
  [DllImport("advapi32.dll", SetLastError = true)]
  public static extern bool GetTokenInformation(IntPtr token, int infoClass, IntPtr info,
      int length, out int needed);
}
'@
}

function Get-LayerCWin32Message([int]$code) {
  if ($code -eq 0) { return '' }
  try { return ([ComponentModel.Win32Exception]$code).Message } catch { return '' }
}

function Get-LayerCIntegrity([IntPtr]$process) {
  $token = [IntPtr]::Zero
  if (-not [LayerCPreflightNative]::OpenProcessToken($process, 0x0008, [ref]$token)) {
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    return [ordered]@{ ok = $false; error = $err; message = (Get-LayerCWin32Message $err) }
  }
  try {
    $len = 0
    [void][LayerCPreflightNative]::GetTokenInformation($token, 25, [IntPtr]::Zero, 0, [ref]$len)
    if ($len -le 0) {
      $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
      return [ordered]@{ ok = $false; error = $err; message = (Get-LayerCWin32Message $err) }
    }
    $buffer = [Runtime.InteropServices.Marshal]::AllocHGlobal($len)
    try {
      $needed = 0
      if (-not [LayerCPreflightNative]::GetTokenInformation($token, 25, $buffer, $len, [ref]$needed)) {
        $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        return [ordered]@{ ok = $false; error = $err; message = (Get-LayerCWin32Message $err) }
      }
      $sid = [Runtime.InteropServices.Marshal]::ReadIntPtr($buffer, 0)
      $count = [int][Runtime.InteropServices.Marshal]::ReadByte($sid, 1)
      if ($count -le 0) { return [ordered]@{ ok = $false; error = 0; message = 'empty SID' } }
      $rid = [Runtime.InteropServices.Marshal]::ReadInt32($sid, 8 + 4 * ($count - 1))
      $levels = @{
        0x0000 = 'Untrusted'; 0x1000 = 'Low'; 0x2000 = 'Medium'; 0x2100 = 'MediumPlus';
        0x3000 = 'High'; 0x4000 = 'System'; 0x5000 = 'Protected'
      }
      $name = if ($levels.ContainsKey($rid)) { $levels[$rid] } else { ('RID-0x{0:X4}' -f $rid) }
      return [ordered]@{ ok = $true; rid = ('0x{0:X4}' -f $rid); name = $name }
    } finally { [Runtime.InteropServices.Marshal]::FreeHGlobal($buffer) }
  } finally { [void][LayerCPreflightNative]::CloseHandle($token) }
}

function Get-LayerCProcessIntegrity([int]$processId) {
  $handle = [LayerCPreflightNative]::OpenProcess(0x1000, $false, $processId)
  if ($handle -eq [IntPtr]::Zero) {
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    return [ordered]@{ ok = $false; error = $err; message = (Get-LayerCWin32Message $err) }
  }
  try { return Get-LayerCIntegrity $handle }
  finally { [void][LayerCPreflightNative]::CloseHandle($handle) }
}

function Get-LayerCObjectName([IntPtr]$object) {
  if ($object -eq [IntPtr]::Zero) {
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    return [ordered]@{ ok = $false; error = $err; message = (Get-LayerCWin32Message $err) }
  }
  $buffer = New-Object System.Text.StringBuilder 512
  $needed = 0
  if (-not [LayerCPreflightNative]::GetUserObjectInformationW($object, 2, $buffer, 512, [ref]$needed)) {
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    return [ordered]@{ ok = $false; error = $err; message = (Get-LayerCWin32Message $err) }
  }
  return [ordered]@{ ok = $true; name = $buffer.ToString() }
}

function Get-LayerCContext {
  $context = [ordered]@{}
  $context.pid = $PID
  $context.is_64bit_process = [Environment]::Is64BitProcess
  $context.powershell_version = $PSVersionTable.PSVersion.ToString()
  try {
    $me = Get-CimInstance Win32_Process -Filter ("ProcessId=" + $PID)
    $context.parent_pid = [int]$me.ParentProcessId
    $context.process_name = $me.Name
    $parent = Get-CimInstance Win32_Process -Filter ("ProcessId=" + [int]$me.ParentProcessId)
    $context.parent_name = $parent.Name
    $context.parent_created = $parent.CreationDate.ToString('o')
  } catch { $context.process_query_error = $_.Exception.Message }
  try { $context.user = [Security.Principal.WindowsIdentity]::GetCurrent().Name } catch { $context.user_error = $_.Exception.Message }
  try { $context.session = [int](Get-Process -Id $PID).SessionId } catch { $context.session_error = $_.Exception.Message }
  $integrity = Get-LayerCIntegrity ([LayerCPreflightNative]::GetCurrentProcess())
  $context.integrity = $integrity
  $winsta = Get-LayerCObjectName ([LayerCPreflightNative]::GetProcessWindowStation())
  $context.window_station = $winsta
  $desktop = Get-LayerCObjectName ([LayerCPreflightNative]::GetThreadDesktop([LayerCPreflightNative]::GetCurrentThreadId()))
  $context.thread_desktop = $desktop
  $input = [LayerCPreflightNative]::OpenInputDesktop(0, $false, 0x0001)
  $inputName = Get-LayerCObjectName $input
  if ($input -ne [IntPtr]::Zero) { [void][LayerCPreflightNative]::CloseDesktop($input) }
  $context.input_desktop = $inputName
  $context.input_desktop_matches_thread = (([bool]$inputName['ok']) -and ([bool]$desktop['ok']) -and ($inputName['name'] -eq $desktop['name']))
  $context.foreground_hwnd = [int64][LayerCPreflightNative]::GetForegroundWindow()
  $context.shell_hwnd = [int64][LayerCPreflightNative]::GetShellWindow()
  return $context
}

function Get-LayerCNotepadSnapshot {
  $items = @(Get-Process Notepad -ErrorAction SilentlyContinue)
  $snapshot = @()
  foreach ($item in $items) {
    $entry = [ordered]@{ id = [int]$item.Id; start_time = ''; title = '' }
    try { $entry.start_time = $item.StartTime.ToString('o') } catch { }
    try { $entry.title = $item.MainWindowTitle } catch { }
    $snapshot += [pscustomobject]$entry
  }
  return $snapshot
}

function Find-LayerCWindowByTitle([string]$needle) {
  $script:layerCFound = [IntPtr]::Zero
  $cb = [LayerCPreflightNative+EnumProc]{
    param($h, $l)
    try {
      if ([LayerCPreflightNative]::IsWindowVisible($h)) {
        $tb = New-Object System.Text.StringBuilder 1024
        [void][LayerCPreflightNative]::GetWindowTextW($h, $tb, 1024)
        if ($tb.ToString().Contains($needle)) { $script:layerCFound = $h; return $false }
      }
    } catch { }
    return $true
  }
  [void][LayerCPreflightNative]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:layerCFound
}

function Get-LayerCWindowInfo([IntPtr]$hwnd) {
  $tb = New-Object System.Text.StringBuilder 1024
  $cb = New-Object System.Text.StringBuilder 256
  [void][LayerCPreflightNative]::GetWindowTextW($hwnd, $tb, 1024)
  [void][LayerCPreflightNative]::GetClassNameW($hwnd, $cb, 256)
  [uint32]$windowPid = 0
  [void][LayerCPreflightNative]::GetWindowThreadProcessId($hwnd, [ref]$windowPid)
  return [ordered]@{
    hwnd = [int64]$hwnd; pid = [int]$windowPid; class = $cb.ToString();
    title = $tb.ToString(); visible = [bool][LayerCPreflightNative]::IsWindowVisible($hwnd)
  }
}

function Get-LayerCWindowProcess([int]$windowPid) {
  try {
    $process = Get-CimInstance Win32_Process -Filter ("ProcessId=" + $windowPid)
    return [ordered]@{
      name = $process.Name; parent_pid = [int]$process.ParentProcessId;
      created = $process.CreationDate.ToString('o')
    }
  } catch { return [ordered]@{ error = $_.Exception.Message } }
}

function Get-LayerCDocumentControl([IntPtr]$hwnd) {
  try {
    Add-Type -AssemblyName UIAutomationClient
    Add-Type -AssemblyName UIAutomationTypes
    $top = [System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
    if ($null -eq $top) { return [ordered]@{ ok = $false; reason = 'AutomationElement.FromHandle returned null' } }
    $docCond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Document)
    $doc = $top.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $docCond)
    if ($null -eq $doc) {
      $classCond = New-Object System.Windows.Automation.PropertyCondition(
          [System.Windows.Automation.AutomationElement]::ClassNameProperty, 'RichEditD2DPT')
      $doc = $top.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $classCond)
    }
    if ($null -eq $doc) {
      $editCond = New-Object System.Windows.Automation.PropertyCondition(
          [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
          [System.Windows.Automation.ControlType]::Edit)
      $doc = $top.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $editCond)
    }
    if ($null -eq $doc) { return [ordered]@{ ok = $false; reason = 'no Document/RichEditD2DPT/Edit control' } }
    return [ordered]@{
      ok = $true; name = $top.Current.Name; class = $top.Current.ClassName;
      document_name = $doc.Current.Name; document_class = $doc.Current.ClassName;
      document_control_type = $doc.Current.ControlType.ProgrammaticName;
      document_hwnd = [int64]$doc.Current.NativeWindowHandle
    }
  } catch {
    return [ordered]@{ ok = $false; reason = $_.Exception.ToString() }
  }
}

function Stop-LayerCOwnedProcess([int]$processId, [int[]]$preIds, [string]$reason) {
  if ($processId -le 0 -or ($preIds -contains $processId)) { return $false }
  try {
    Stop-Process -Id $processId -Force -ErrorAction Stop
    return $true
  } catch { return $false }
}

function Invoke-LayerCPreflight {
  param(
    [Parameter(Mandatory = $true)][string]$SessionId,
    [Parameter(Mandatory = $true)][int]$RunIndex,
    [Parameter(Mandatory = $true)][string]$EvidenceDir,
    [Parameter(Mandatory = $true)][string]$TestRoot,
    [string]$RepoRoot = ''
  )
  if (-not [System.IO.Path]::IsPathRooted($EvidenceDir)) {
    $EvidenceDir = [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $EvidenceDir))
  }
  if (-not [System.IO.Path]::IsPathRooted($TestRoot)) {
    $TestRoot = [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $TestRoot))
  }
  $runDir = Join-Path $EvidenceDir ('preflight-run-' + $RunIndex)
  New-Item -ItemType Directory -Path $runDir -Force | Out-Null
  $logPath = Join-Path $runDir 'preflight.log'
  $resultPath = Join-Path $runDir 'preflight.json'
  $result = [ordered]@{ session_id = $SessionId; run_index = $RunIndex; timestamp = (Get-Date).ToString('o') }
  function Log([string]$m) {
    $line = '{0} {1}' -f (Get-Date).ToString('s'), $m
    Add-Content -LiteralPath $logPath -Value $line -Encoding UTF8
    Write-Output $line
  }
  function SetR([string]$k, $v) { $result[$k] = $v; Log ("$k=$v") }

  $result['context'] = Get-LayerCContext
  SetR evidence_dir $EvidenceDir
  SetR test_root $TestRoot
  $pre = @(Get-LayerCNotepadSnapshot)
  $preIds = @($pre | ForEach-Object { [int]$_.id })
  SetR preexisting_notepad_pids ($preIds -join ',')
  SetR preexisting_notepad_count $pre.Count
  if ($pre.Count -gt 0) {
    SetR ok $false
    SetR blocked 'pre-existing Notepad process(es); refusing to launch or close a possibly reused instance'
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding UTF8
    return [pscustomobject]$result
  }
  if (-not (Test-Path -LiteralPath $TestRoot -PathType Container)) {
    New-Item -ItemType Directory -Path $TestRoot -Force | Out-Null
  }
  $leaf = 'w3a-preflight-' + [guid]::NewGuid().ToString('N') + '.txt'
  $testFile = Join-Path $TestRoot $leaf
  [System.IO.File]::WriteAllText($testFile, '', (New-Object System.Text.UTF8Encoding($false)))
  SetR test_file $testFile
  SetR test_file_exists (Test-Path -LiteralPath $testFile -PathType Leaf)
  $app = 'C:\Windows\System32\notepad.exe'
  SetR notepad_path $app
  SetR notepad_path_exists (Test-Path -LiteralPath $app -PathType Leaf)
  $cmd = '"' + $app + '" "' + $testFile + '"'
  SetR command_line $cmd

  $si = New-Object LayerCPreflightNative+STARTUPINFO
  $si.cb = [Runtime.InteropServices.Marshal]::SizeOf([type][LayerCPreflightNative+STARTUPINFO])
  SetR startupinfo_cb $si.cb
  SetR startupinfo_size ([Runtime.InteropServices.Marshal]::SizeOf([type][LayerCPreflightNative+STARTUPINFO]))
  $pi = New-Object LayerCPreflightNative+PROCESS_INFORMATION
  $raw = $false
  $win32 = 0
  $exception = ''
  SetR launch_entered $true
  SetR createprocess_called $true
  SetR createprocess_api 'CreateProcessW (ExactSpelling, CharSet=Unicode, SetLastError=true)'
  try {
    $raw = [LayerCPreflightNative]::CreateProcessW($app, $cmd, [IntPtr]::Zero, [IntPtr]::Zero,
        $false, 0, [IntPtr]::Zero, 'C:\Windows\System32', [ref]$si, [ref]$pi)
    $win32 = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
  } catch {
    $exception = $_.Exception.ToString()
    $win32 = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
  }
  SetR raw_return ([bool]$raw)
  SetR win32_error ([int]$win32)
  SetR win32_message (Get-LayerCWin32Message ([int]$win32))
  SetR exception $exception
  SetR process_information ([ordered]@{
    hProcess = [int64]$pi.hProcess; hThread = [int64]$pi.hThread;
    process_id = [int]$pi.dwProcessId; thread_id = [int]$pi.dwThreadId
  })
  $launcherPid = [int]$pi.dwProcessId
  SetR launcher_pid $launcherPid
  if ($raw) {
    [void][LayerCPreflightNative]::CloseHandle($pi.hThread)
    [void][LayerCPreflightNative]::CloseHandle($pi.hProcess)
  }
  if (-not $raw) {
    if (Test-Path -LiteralPath $testFile) { Remove-Item -LiteralPath $testFile -Force -ErrorAction SilentlyContinue }
    SetR test_file_removed (-not (Test-Path -LiteralPath $testFile))
    SetR ok $false
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding UTF8
    return [pscustomobject]$result
  }

  $launchTime = Get-Date
  $hwnd = [IntPtr]::Zero
  for ($i = 0; $i -lt 80; $i++) {
    $hwnd = Find-LayerCWindowByTitle $leaf
    if ($hwnd -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 250
  }
  SetR window_found ($hwnd -ne [IntPtr]::Zero)
  if ($hwnd -eq [IntPtr]::Zero) {
    # The launcher is ours (new PID) and has no window; stop only that new PID.
    $launcherStopped = Stop-LayerCOwnedProcess $launcherPid $preIds 'launcher without window'
    SetR launcher_stopped $launcherStopped
    if (Test-Path -LiteralPath $testFile) { Remove-Item -LiteralPath $testFile -Force -ErrorAction SilentlyContinue }
    SetR test_file_removed (-not (Test-Path -LiteralPath $testFile))
    SetR ok $false
    SetR blocked 'unique test window did not appear'
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding UTF8
    return [pscustomobject]$result
  }

  $window = Get-LayerCWindowInfo $hwnd
  $result['window'] = $window
  $windowPid = [int]$window.pid
  $windowProcess = Get-LayerCWindowProcess $windowPid
  $result['window_process'] = $windowProcess
  $result['window_integrity'] = Get-LayerCProcessIntegrity $windowPid
  $result['window_is_preexisting'] = ($preIds -contains $windowPid)
  $result['window_pid_matches_launcher'] = ($windowPid -eq $launcherPid)
  $result['window_title_has_unique_leaf'] = $window.title.Contains($leaf)
  $windowCreated = $null
  if ($windowProcess -and $windowProcess['created']) {
    try { $windowCreated = [datetime]::Parse($windowProcess['created']).ToLocalTime() } catch { }
  }
  $result['window_created_after_launch'] = ($null -ne $windowCreated -and $windowCreated -ge $launchTime.AddSeconds(-2))
  $result['uia'] = Get-LayerCDocumentControl $hwnd
  $owned = (-not $result['window_is_preexisting']) -and $result['window_title_has_unique_leaf'] -and
           ($result['window_created_after_launch']) -and ($windowPid -gt 0)
  SetR window_owned_by_this_launch $owned

  if (-not $owned) {
    SetR close_verified $false
    SetR close_skipped 'window identity is not proven new; refusing to touch it'
    SetR test_file_left_for_operator $testFile
    SetR ok $false
    SetR blocked 'window/process identity is uncertain or reuses a pre-existing instance'
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding UTF8
    return [pscustomobject]$result
  }

  # Re-read title/PID immediately before closing the owned window.
  $again = Get-LayerCWindowInfo $hwnd
  if (-not $again.title.Contains($leaf) -or [int]$again.pid -ne $windowPid) {
    SetR close_verified $false
    SetR close_skipped 'window identity changed immediately before close'
    SetR test_file_left_for_operator $testFile
    SetR ok $false
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding UTF8
    return [pscustomobject]$result
  }
  SetR close_verified $true
  [void][LayerCPreflightNative]::PostMessageW($hwnd, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 1200
  $windowAlive = $null -ne (Get-Process -Id $windowPid -ErrorAction SilentlyContinue)
  SetR window_alive_after_wm_close $windowAlive
  if ($windowAlive) {
    $stopped = Stop-LayerCOwnedProcess $windowPid $preIds 'owned window process after WM_CLOSE'
    SetR window_stopped_after_verify $stopped
    Start-Sleep -Milliseconds 500
  }
  $launcherAlive = $null -ne (Get-Process -Id $launcherPid -ErrorAction SilentlyContinue)
  SetR launcher_alive_after_close $launcherAlive
  if ($launcherAlive -and $launcherPid -ne $windowPid) {
    $stopped = Stop-LayerCOwnedProcess $launcherPid $preIds 'owned launcher after window close'
    SetR launcher_stopped_after_verify $stopped
  }
  if (Test-Path -LiteralPath $testFile) { Remove-Item -LiteralPath $testFile -Force -ErrorAction SilentlyContinue }
  SetR test_file_removed (-not (Test-Path -LiteralPath $testFile))
  # A terminated Notepad can remain visible in the process table for a short grace period.
  # Wait only for that; any process still present after the grace is recorded, never killed
  # unless it was already proven owned above.
  $remaining = 0
  for ($i = 0; $i -lt 20; $i++) {
    $remaining = (Get-Process Notepad -ErrorAction SilentlyContinue | Measure-Object).Count
    if ($remaining -eq 0) { break }
    Start-Sleep -Milliseconds 250
  }
  SetR remaining_notepad_count $remaining
  if ($remaining -ne 0) {
    $remainingInfo = @(Get-Process Notepad -ErrorAction SilentlyContinue | ForEach-Object {
      [ordered]@{ id = $_.Id; start_time = $(try { $_.StartTime.ToString('o') } catch { '' });
                  title = $(try { $_.MainWindowTitle } catch { '' }) }
    })
    SetR remaining_notepad $remainingInfo
    SetR blocked 'a Notepad process remained after the bounded exit wait'
  }
  $ok = $result['window_title_has_unique_leaf'] -and $result['window_created_after_launch'] -and
        $result['uia']['ok'] -and $result['test_file_removed'] -and
        ($result['remaining_notepad_count'] -eq 0)
  SetR ok ([bool]$ok)
  $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultPath -Encoding UTF8
  return [pscustomobject]$result
}
