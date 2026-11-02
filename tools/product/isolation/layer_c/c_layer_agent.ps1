param(
  [Parameter(Mandatory = $true)][string]$EvidenceDir,
  [Parameter(Mandatory = $true)][string]$SessionId,
  [Parameter(Mandatory = $true)][string]$DeadlineUtc,
  [string]$App = 'notepad',
  [string]$TestRoot = '',
  [switch]$PreflightOnly,
  [int]$PreflightRunIndex = 1,
  [switch]$DryRun
)

$ErrorActionPreference = 'Continue'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
# [Environment]::TickCount64 does not exist on Windows PowerShell 5.1 (.NET Framework)
# and silently evaluates to $null there, which turned the sleep/wake detector into a
# guaranteed false positive after 30 wall seconds. Call kernel32 directly instead.
if (-not ('EpoLayerCTick' -as [type])) {
  Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class EpoLayerCTick {
  [DllImport("kernel32.dll")] public static extern ulong GetTickCount64();
}
'@
}
$agentStartUtc = (Get-Date).ToUniversalTime()
$agentStartTick = [EpoLayerCTick]::GetTickCount64()
if (-not $TestRoot) { $TestRoot = Join-Path $repoRoot 'build\w3a-c-local\test-documents' }
if (-not [System.IO.Path]::IsPathRooted($TestRoot)) { $TestRoot = [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $TestRoot)) }
New-Item -ItemType Directory -Path $EvidenceDir -Force | Out-Null
$logPath = Join-Path $EvidenceDir 'agent.log'
Set-Content -Path $logPath -Value ('EpoCanva C-layer agent session=' + $SessionId) -Encoding UTF8
$result = [ordered]@{ session_id = $SessionId }
$agentDeadline = [datetime]::Parse($DeadlineUtc).ToUniversalTime()
$readyFlag = Join-Path $EvidenceDir 'registration-ready.flag'
$preflightReadyFlag = Join-Path $EvidenceDir 'preflight-ready.flag'
$agentReadyFlag = Join-Path $EvidenceDir 'agent-ready.flag'
$testWindowReadyFlag = Join-Path $EvidenceDir 'test-window-ready.flag'
$testStartFlag = Join-Path $EvidenceDir 'test-start.flag'
$cleanupFlag = Join-Path $EvidenceDir 'cleanup-complete.flag'
$cleanupDoneLegacyFlag = Join-Path $EvidenceDir 'cleanup-done.flag'
$stopFlag = Join-Path $EvidenceDir 'stop.flag'
. (Join-Path $PSScriptRoot 'layer_c_preflight_lib.ps1')
function Log([string]$m) {
  $line = '{0} {1}' -f (Get-Date).ToString('s'), $m
  Add-Content -Path $logPath -Value $line -Encoding UTF8
  Write-Output $line
}
function SetR([string]$k, $v) { $result[$k] = $v; Log ("$k=$v") }
function Read-Flag([string]$path) {
  try { return (Get-Content -LiteralPath $path -Raw -ErrorAction Stop).Trim() }
  catch { return '' }
}
function Invoke-InitialAbort([string]$reason) {
  SetR aborted $reason
  SetR invalid_round $true
  $result['passed'] = $false
  $result | ConvertTo-Json -Depth 6 | Set-Content -Path (Join-Path $EvidenceDir 'agent-result.json') -Encoding UTF8
  Log ('ABORT: ' + $reason)
  exit 8
}
function Test-AgentSession([bool]$RequireRegistration = $true) {
  if ((Get-Date).ToUniversalTime() -ge $agentDeadline) {
    Invoke-InitialAbort 'deadline passed before input'
  }
  $wall = ((Get-Date).ToUniversalTime() - $agentStartUtc).TotalSeconds
  $mono = ([EpoLayerCTick]::GetTickCount64() - $agentStartTick) / 1000.0
  if (($wall - $mono) -gt 30.0) {
    Invoke-InitialAbort 'wall clock jumped (sleep/wake); no input'
  }
  if ((Test-Path -LiteralPath $cleanupFlag) -or (Test-Path -LiteralPath $cleanupDoneLegacyFlag)) {
    Invoke-InitialAbort 'cleanup already completed before this agent started; no input'
  }
  if (Test-Path -LiteralPath $stopFlag) {
    Invoke-InitialAbort 'stop flag present; no input'
  }
  if ($RequireRegistration) {
    $ready = Read-Flag $readyFlag
    if ($ready -ne $SessionId) {
      Invoke-InitialAbort 'registration-ready flag is missing or belongs to another session'
    }
  }
}

Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class W3ANative {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO {
    public int cbSize; public int flags; public IntPtr hwndActive; public IntPtr hwndFocus;
    public IntPtr hwndCapture; public IntPtr hwndMenuOwner; public IntPtr hwndMoveSize;
    public IntPtr hwndCaret; public RECT rcCaret;
  }
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint tid, ref GUITHREADINFO info);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h, uint flags);
  [DllImport("user32.dll")] public static extern bool IsChild(IntPtr parent, IntPtr child);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern ushort GetKeyState(int vk);
  [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint mapType);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr LoadKeyboardLayout(string id, uint flags);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetKeyboardLayout(uint tid);
  [DllImport("user32.dll")] public static extern IntPtr ActivateKeyboardLayout(IntPtr hkl, uint flags);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern void SwitchToThisWindow(IntPtr h, bool altTab);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
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
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  public static extern bool CreateProcess(string app, string cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags,
                                          IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
  [DllImport("kernel32.dll", SetLastError = true)] public static extern bool CloseHandle(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct COORD { public short X, Y; }
  [StructLayout(LayoutKind.Sequential)] public struct SMALL_RECT { public short Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct CONSOLE_SCREEN_BUFFER_INFO {
    public COORD dwSize, dwCursorPosition;
    public short wAttributes;
    public SMALL_RECT srWindow;
    public COORD dwMaximumWindowSize;
  }
  [DllImport("kernel32.dll")] public static extern bool FreeConsole();
  [DllImport("kernel32.dll")] public static extern bool AttachConsole(uint pid);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr sa, uint create, uint flags, IntPtr template);
  [DllImport("kernel32.dll")] public static extern bool GetConsoleScreenBufferInfo(IntPtr h, out CONSOLE_SCREEN_BUFFER_INFO info);
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] public static extern bool ReadConsoleOutputCharacterW(IntPtr h, System.Text.StringBuilder buf, uint len, COORD coord, out uint read);
  [StructLayout(LayoutKind.Sequential)] public struct TOKEN_ELEVATION { public int TokenIsElevated; }
  [DllImport("kernel32.dll", SetLastError = true)] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("advapi32.dll", SetLastError = true)] public static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
  [DllImport("advapi32.dll", SetLastError = true)] public static extern bool GetTokenInformation(IntPtr token, int infoClass, IntPtr info, int length, out int returned);
}
'@

function Test-ProcessElevated([int]$processId) {
  $process = [W3ANative]::OpenProcess(0x1000, $false, $processId)  # QUERY_LIMITED_INFORMATION
  if ($process -eq [IntPtr]::Zero) { return $null }
  $token = [IntPtr]::Zero
  if (-not [W3ANative]::OpenProcessToken($process, 0x0008, [ref]$token)) {  # TOKEN_QUERY
    [void][W3ANative]::CloseHandle($process)
    return $null
  }
  $size = [Runtime.InteropServices.Marshal]::SizeOf([type][W3ANative+TOKEN_ELEVATION])
  $buffer = [Runtime.InteropServices.Marshal]::AllocHGlobal($size)
  $returned = 0
  $ok = [W3ANative]::GetTokenInformation($token, 20, $buffer, $size, [ref]$returned)  # TokenElevation
  $elevated = $null
  if ($ok) {
    $value = [Runtime.InteropServices.Marshal]::PtrToStructure($buffer, [type][W3ANative+TOKEN_ELEVATION])
    $elevated = ($value.TokenIsElevated -ne 0)
  }
  [Runtime.InteropServices.Marshal]::FreeHGlobal($buffer)
  [void][W3ANative]::CloseHandle($token)
  [void][W3ANative]::CloseHandle($process)
  return $elevated
}

function SendVk([int]$vk, [bool]$extended = $false) {
  $scan = [W3ANative]::MapVirtualKey([uint32]$vk, 0)
  $down = if ($extended) { [uint32]1 } else { [uint32]0 }
  $up = if ($extended) { [uint32]3 } else { [uint32]2 }
  [W3ANative]::keybd_event([byte]$vk, [byte]$scan, $down, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 25
  [W3ANative]::keybd_event([byte]$vk, [byte]$scan, $up, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 60
}

function SendCtrlKey([int]$vk) {
  $scanCtrl = [W3ANative]::MapVirtualKey(0x11, 0)
  [W3ANative]::keybd_event(0x11, [byte]$scanCtrl, 0, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 30
  SendVk $vk
  [W3ANative]::keybd_event(0x11, [byte]$scanCtrl, 2, [UIntPtr]::Zero)
  Start-Sleep -Milliseconds 100
}

function Find-CandidateWindow([int]$procId) {
  $found = [IntPtr]::Zero
  $cb = [W3ANative+EnumProc]{
    param($h, $l)
    try {
      if ([W3ANative]::IsWindowVisible($h)) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][W3ANative]::GetClassName($h, $sb, 256)
        if ($sb.ToString() -eq 'EpoCanva.CandidateWindow.W2B') {
          [uint32]$p = 0
          [void][W3ANative]::GetWindowThreadProcessId($h, [ref]$p)
          if ($procId -le 0 -or [int]$p -eq $procId) { $script:candFound = $h }
        }
      }
    } catch { }
    return $true
  }
  $script:candFound = [IntPtr]::Zero
  [void][W3ANative]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:candFound
}

function Find-NotepadWindow([int]$procId) {
  $script:npFound = [IntPtr]::Zero
  $cb = [W3ANative+EnumProc]{
    param($h, $l)
    try {
      if ([W3ANative]::IsWindowVisible($h)) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][W3ANative]::GetClassName($h, $sb, 256)
        if ($sb.ToString() -eq 'Notepad') {
          [uint32]$p = 0
          [void][W3ANative]::GetWindowThreadProcessId($h, [ref]$p)
          if ([int]$p -eq $procId) { $script:npFound = $h }
        }
      }
    } catch { }
    return $true
  }
  [void][W3ANative]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:npFound
}

function Find-NotepadWindowByTitle([string]$leaf) {
  $script:npTitleFound = [IntPtr]::Zero
  $cb = [W3ANative+EnumProc]{
    param($h, $l)
    try {
      if ([W3ANative]::IsWindowVisible($h)) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][W3ANative]::GetClassName($h, $sb, 256)
        if ($sb.ToString() -eq 'Notepad') {
          $tb = New-Object System.Text.StringBuilder 512
          [void][W3ANative]::GetWindowText($h, $tb, 512)
          if ($tb.ToString().Contains($leaf)) { $script:npTitleFound = $h }
        }
      }
    } catch { }
    return $true
  }
  [void][W3ANative]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:npTitleFound
}

function Read-ConsoleText([int]$procId) {
  [void][W3ANative]::FreeConsole()
  if (-not [W3ANative]::AttachConsole([uint32]$procId)) { return '' }
  $h = [W3ANative]::CreateFileW('CONOUT$', 0xC0000000, 3, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
  if ($h -eq [IntPtr]::Zero -or $h -eq [IntPtr](-1)) { [void][W3ANative]::FreeConsole(); return '' }
  $info = New-Object W3ANative+CONSOLE_SCREEN_BUFFER_INFO
  $text = ''
  if ([W3ANative]::GetConsoleScreenBufferInfo($h, [ref]$info)) {
    $width = [int]$info.dwSize.X
    $height = [int]$info.dwSize.Y
    if ($width -gt 0 -and $height -gt 0) {
      $lines = New-Object System.Collections.Generic.List[string]
      $first = [int]$info.srWindow.Top
      $last = [int]$info.srWindow.Bottom
      if ($first -lt 0) { $first = 0 }
      if ($last -ge $height) { $last = $height - 1 }
      for ($row = $first; $row -le $last; $row++) {
        $sb = New-Object System.Text.StringBuilder ($width + 1)
        $coord = New-Object W3ANative+COORD
        $coord.X = 0; $coord.Y = [short]$row
        [uint32]$read = 0
        if ([W3ANative]::ReadConsoleOutputCharacterW($h, $sb, [uint32]$width, $coord, [ref]$read)) {
          $lines.Add($sb.ToString().TrimEnd())
        }
      }
      $text = ($lines -join "`n").TrimEnd()
    }
  }
  [void][W3ANative]::CloseHandle($h)
  [void][W3ANative]::FreeConsole()
  return $text
}

function Find-ProcessWindowAny([int]$procId) {
  $script:anyFound = [IntPtr]::Zero
  $cb = [W3ANative+EnumProc]{
    param($h, $l)
    try {
      if ([W3ANative]::IsWindowVisible($h)) {
        [uint32]$p = 0
        [void][W3ANative]::GetWindowThreadProcessId($h, [ref]$p)
        if ([int]$p -eq $procId) { $script:anyFound = $h }
      }
    } catch { }
    return $true
  }
  [void][W3ANative]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:anyFound
}

function Find-ProcessWindow([int]$procId, [string]$className) {
  $script:pwFound = [IntPtr]::Zero
  $cb = [W3ANative+EnumProc]{
    param($h, $l)
    try {
      if ([W3ANative]::IsWindowVisible($h)) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][W3ANative]::GetClassName($h, $sb, 256)
        if ($sb.ToString() -eq $className) {
          [uint32]$p = 0
          [void][W3ANative]::GetWindowThreadProcessId($h, [ref]$p)
          if ([int]$p -eq $procId) { $script:pwFound = $h }
        }
      }
    } catch { }
    return $true
  }
  [void][W3ANative]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:pwFound
}

function Find-ConsoleWindow {
  $script:conFound = [IntPtr]::Zero
  $cb = [W3ANative+EnumProc]{
    param($h, $l)
    try {
      if ([W3ANative]::IsWindowVisible($h)) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][W3ANative]::GetClassName($h, $sb, 256)
        if ($sb.ToString() -eq 'ConsoleWindowClass') { $script:conFound = $h }
      }
    } catch { }
    return $true
  }
  [void][W3ANative]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:conFound
}

function Find-EdgeWindow {
  $script:edgeFound = [IntPtr]::Zero
  $cb = [W3ANative+EnumProc]{
    param($h, $l)
    try {
      if ([W3ANative]::IsWindowVisible($h)) {
        $sb = New-Object System.Text.StringBuilder 256
        [void][W3ANative]::GetClassName($h, $sb, 256)
        if ($sb.ToString() -eq 'Chrome_WidgetWin_1') {
          $tb = New-Object System.Text.StringBuilder 512
          [void][W3ANative]::GetWindowText($h, $tb, 512)
          if ($tb.ToString() -match 'EpoCanva W3A Test') { $script:edgeFound = $h }
        }
      }
    } catch { }
    return $true
  }
  [void][W3ANative]::EnumWindows($cb, [IntPtr]::Zero)
  return $script:edgeFound
}

function Has-Module([int]$procId, [string]$name) {
  try {
    $m = Get-Process -Id $procId -Module -ErrorAction SilentlyContinue | Where-Object { $_.ModuleName -eq $name }
    return [bool]$m
  } catch { return $false }
}

function Get-DocText($doc) {
  if ($App -eq 'cmd' -and $notepadPid -ne 0) { return (Read-ConsoleText $notepadPid) }
  try {
    $tp = $doc.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern)
    return $tp.DocumentRange.GetText(-1)
  } catch { }
  try {
    $vp = $doc.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
    return $vp.Current.Value
  } catch { }
  try {
    $hwnd = [IntPtr]$doc.Current.NativeWindowHandle
    if ($hwnd -ne [IntPtr]::Zero) {
      $sb = New-Object System.Text.StringBuilder 4096
      [void][W3ANative]::GetWindowText($hwnd, $sb, 4096)
      return $sb.ToString()
    }
  } catch { }
  return $null
}

function Normalize([string]$s) {
  if ($null -eq $s) { return $null }
  return $s.TrimEnd([char]13, [char]10)
}

function Get-WindowTitle([IntPtr]$h) {
  try {
    $sb = New-Object System.Text.StringBuilder 512
    [void][W3ANative]::GetWindowText($h, $sb, 512)
    return $sb.ToString()
  } catch { return '' }
}

function Get-TitleText([string]$title) {
  if ($null -eq $title) { return '' }
  $t = $title.Trim()
  if ($t.StartsWith('*')) { $t = $t.Substring(1) }
  $idx = $t.LastIndexOf(' - ')
  if ($idx -gt 0) { $t = $t.Substring(0, $idx) }
  return $t.Trim()
}

function Ensure-DocFocus([bool]$Click) {
  $fgNow = [W3ANative]::GetForegroundWindow()
  $fgRootNow = [W3ANative]::GetAncestor($fgNow, 2)
  if ($fgRootNow -ne $topRoot) {
    [uint32]$fgPidNow = 0
    $fgThreadNow = [W3ANative]::GetWindowThreadProcessId($fgNow, [ref]$fgPidNow)
    $curThreadNow = [W3ANative]::GetCurrentThreadId()
    [void][W3ANative]::AttachThreadInput($curThreadNow, $fgThreadNow, $true)
    [void][W3ANative]::SetForegroundWindow($topHwnd)
    [void][W3ANative]::AttachThreadInput($curThreadNow, $fgThreadNow, $false)
    Start-Sleep -Milliseconds 400
    $fgNow = [W3ANative]::GetForegroundWindow()
    $fgRootNow = [W3ANative]::GetAncestor($fgNow, 2)
  }
  if ($fgRootNow -ne $topRoot) {
    # Windows restricts SetForegroundWindow for a background process. SwitchToThisWindow is
    # the shell path used by task switching; it is tried first, without any click.
    [void][W3ANative]::ShowWindow($topHwnd, 9)
    [W3ANative]::SwitchToThisWindow($topHwnd, $true)
    Start-Sleep -Milliseconds 500
    $fgNow = [W3ANative]::GetForegroundWindow()
    $fgRootNow = [W3ANative]::GetAncestor($fgNow, 2)
  }
  if ($fgRootNow -ne $topRoot) {
    # Fallback: raise our own test window (only if the raise succeeds) and click its title
    # bar. The click never runs when the window could not be raised, so it cannot land on
    # the user's foreground application.
    $wr = New-Object W3ANative+RECT
    if ([W3ANative]::GetWindowRect($topHwnd, [ref]$wr)) {
      $raised = [W3ANative]::SetWindowPos($topHwnd, [IntPtr](-1), 0, 0, 0, 0, 0x0001 -bor 0x0002 -bor 0x0040 -bor 0x0010)
      if ($raised) {
        $oldTitleCursor = New-Object W3ANative+POINT
        [void][W3ANative]::GetCursorPos([ref]$oldTitleCursor)
        [void][W3ANative]::SetCursorPos([int](($wr.Left + $wr.Right) / 2), [int]($wr.Top + 12))
        Start-Sleep -Milliseconds 120
        [W3ANative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
        [W3ANative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
        Start-Sleep -Milliseconds 500
        [void][W3ANative]::SetCursorPos($oldTitleCursor.X, $oldTitleCursor.Y)
        [void][W3ANative]::SetWindowPos($topHwnd, [IntPtr](-2), 0, 0, 0, 0, 0x0001 -bor 0x0002 -bor 0x0010)
      }
    }
    $fgNow = [W3ANative]::GetForegroundWindow()
    $fgRootNow = [W3ANative]::GetAncestor($fgNow, 2)
  }
  if ($fgRootNow -ne $topRoot) {
    SetR focus_state 'notepad_not_foreground'
    return $false
  }
  if ($Click) {
    $rect = $null
    try { $rect = $doc.Current.BoundingRectangle } catch { }
    if ($null -ne $rect -and $rect.Width -gt 20 -and $rect.Height -gt 20) {
      $oldCursor = New-Object W3ANative+POINT
      [void][W3ANative]::GetCursorPos([ref]$oldCursor)
      [void][W3ANative]::SetCursorPos([int]($rect.X + 30), [int]($rect.Y + 30))
      Start-Sleep -Milliseconds 120
      [W3ANative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
      [W3ANative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
      Start-Sleep -Milliseconds 300
      [void][W3ANative]::SetCursorPos($oldCursor.X, $oldCursor.Y)
    }
    try { $doc.SetFocus() } catch { }
    [void][W3ANative]::SetForegroundWindow($topHwnd)
    Start-Sleep -Milliseconds 500
  }
  $focused = [System.Windows.Automation.AutomationElement]::FocusedElement
  $fh = [IntPtr]::Zero
  $fp = 0
  if ($null -ne $focused) {
    $fh = [IntPtr]$focused.Current.NativeWindowHandle
    [uint32]$fpu = 0
    if ($fh -ne [IntPtr]::Zero) {
      [void][W3ANative]::GetWindowThreadProcessId($fh, [ref]$fpu)
      $fp = [int]$fpu
    }
    SetR focused_control_type $focused.Current.ControlType.ProgrammaticName
    SetR focused_class $focused.Current.ClassName
    SetR focused_hwnd ([int64]$fh)
    SetR focused_pid $fp
  }
  $dh = [IntPtr]$doc.Current.NativeWindowHandle
  $ok = ($fh -eq $dh) -or ([W3ANative]::IsChild($dh, $fh)) -or (($fp -eq [int]$topPid) -and ($fh -ne [IntPtr]::Zero))
  SetR document_focus_ok $ok
  SetR focus_state ($(if ($ok) { 'ok' } else { 'not_document' }))
  return $ok
}

function Abort-Agent([string]$reason) {
  SetR aborted $reason
  $result['passed'] = $false
  $result | ConvertTo-Json -Depth 6 | Set-Content -Path (Join-Path $EvidenceDir 'agent-result.json') -Encoding UTF8
  if ($null -ne $origHkl -and $origHkl -ne [IntPtr]::Zero -and $prevFg -ne [IntPtr]::Zero) {
    $prevRootAbort = [W3ANative]::GetAncestor($prevFg, 2)
    if ($prevRootAbort -ne [IntPtr]::Zero) { [void][W3ANative]::PostMessage($prevRootAbort, 0x50, [IntPtr]::Zero, $origHkl) }
    [void][W3ANative]::ActivateKeyboardLayout($origHkl, 0)
  }
  if ($null -ne (Get-Variable -Name notepadPid -ErrorAction SilentlyContinue) -and $notepadPid -ne 0) {
    Stop-Process -Id ([int]$notepadPid) -Force -ErrorAction SilentlyContinue
  }
  if ($topPid -ne 0) { Stop-Process -Id ([int]$topPid) -Force -ErrorAction SilentlyContinue }
  if ($null -ne (Get-Variable -Name launcherPid -ErrorAction SilentlyContinue) -and $launcherPid -ne 0 -and $launcherPid -ne $notepadPid -and $launcherPid -ne $topPid) {
    Stop-Process -Id ([int]$launcherPid) -Force -ErrorAction SilentlyContinue
  }
  if ($testFile) { Remove-Item -LiteralPath $testFile -Force -ErrorAction SilentlyContinue }
  if ($null -ne (Get-Variable -Name profile -ErrorAction SilentlyContinue) -and $profile) {
    Start-Sleep -Milliseconds 500
    Remove-Item -LiteralPath $profile -Recurse -Force -ErrorAction SilentlyContinue
  }
  if ($prevFg -ne [IntPtr]::Zero) { [void][W3ANative]::SetForegroundWindow($prevFg) }
  exit 3
}

function Test-RuntimeGate {
  if ((Get-Date).ToUniversalTime() -ge $agentDeadline) {
    $result['invalid_round'] = $true
    Abort-Agent 'deadline reached during the agent round; no further input'
  }
  $wall = ((Get-Date).ToUniversalTime() - $agentStartUtc).TotalSeconds
  $mono = ([EpoLayerCTick]::GetTickCount64() - $agentStartTick) / 1000.0
  if (($wall - $mono) -gt 30.0) {
    $result['invalid_round'] = $true
    Abort-Agent 'wall clock jumped during the agent round (sleep/wake); no further input'
  }
  if (Test-Path -LiteralPath $cleanupFlag) {
    $result['invalid_round'] = $true
    Abort-Agent 'cleanup completed while the agent was still running; no further input'
  }
  if (Test-Path -LiteralPath $stopFlag) {
    $result['invalid_round'] = $true
    Abort-Agent 'stop flag set during the agent round; no further input'
  }
}

function Save-WindowPng([IntPtr]$hwnd, [string]$path) {
  $rect = New-Object W3ANative+RECT
  if (-not [W3ANative]::GetWindowRect($hwnd, [ref]$rect)) { return $false }
  $w = $rect.Right - $rect.Left; $h = $rect.Bottom - $rect.Top
  if ($w -le 0 -or $h -le 0) { return $false }
  $bmp = New-Object System.Drawing.Bitmap $w, $h
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  $ok = [W3ANative]::PrintWindow($hwnd, $hdc, 2)
  $g.ReleaseHdc($hdc)
  $g.Dispose()
  if (-not $ok) {
    $g2 = [System.Drawing.Graphics]::FromImage($bmp)
    $g2.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size($w, $h)))
    $g2.Dispose()
  }
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
  return $true
}

function Save-ScreenPng([string]$path) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
  $g.Dispose()
  $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
}

$prevFg = [W3ANative]::GetForegroundWindow()
SetR prev_foreground ([int64]$prevFg)
SetR session ((Get-Process -Id $PID).SessionId)
SetR is_elevated (([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
$os = Get-CimInstance Win32_OperatingSystem
SetR os ($os.Caption + ' ' + $os.Version + ' ' + $os.BuildNumber)
SetR screens (([System.Windows.Forms.Screen]::AllScreens | ForEach-Object { $_.DeviceName + ':' + $_.Bounds.Width + 'x' + $_.Bounds.Height }) -join ';')
SetR deadline_utc ($agentDeadline.ToString('o'))
Test-AgentSession -RequireRegistration:$false

$caps = [W3ANative]::GetKeyState(0x14)
SetR caps_lock_on (($caps -band 1) -ne 0)
SetR shift_down (([W3ANative]::GetKeyState(0x10) -band 0x8000) -ne 0)
SetR ctrl_down (([W3ANative]::GetKeyState(0x11) -band 0x8000) -ne 0)
if (($caps -band 1) -ne 0 -or (([W3ANative]::GetKeyState(0x10) -band 0x8000) -ne 0) -or (([W3ANative]::GetKeyState(0x11) -band 0x8000) -ne 0)) {
  SetR aborted 'keyboard modifier state is not neutral'
  $result['passed'] = $false
  $result | ConvertTo-Json -Depth 5 | Set-Content -Path (Join-Path $EvidenceDir 'agent-result.json') -Encoding UTF8
  exit 3
}

# Preflight-first handshake stage. No registration and no input have happened yet. The
# preflight launches and closes only its own unique empty Notepad, verifies ownership and
# UIA, and records the raw CreateProcess result and runtime context.
$preflight = Invoke-LayerCPreflight -SessionId $SessionId -RunIndex $PreflightRunIndex `
    -EvidenceDir $EvidenceDir -TestRoot $TestRoot
$result['preflight'] = $preflight
SetR preflight_ok ([bool]$preflight.ok)
if (-not $preflight.ok) {
  $result['passed'] = $false
  $result['invalid_round'] = $false
  $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $EvidenceDir 'agent-result.json') -Encoding UTF8
  exit 3
}
Set-Content -LiteralPath $preflightReadyFlag -Value $SessionId -Encoding ASCII
if ($PreflightOnly) {
  SetR preflight_only $true
  $result['passed'] = $true
  $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $EvidenceDir 'agent-result.json') -Encoding UTF8
  exit 0
}

# The same normal-integrity agent now waits for the elevated helper to register/activate.
# It proceeds only for this session and only before the shared deadline.
$registered = $false
while ((Get-Date).ToUniversalTime() -lt $agentDeadline) {
  if ((Test-Path -LiteralPath $stopFlag) -or (Test-Path -LiteralPath $cleanupFlag) -or
      (Test-Path -LiteralPath $cleanupDoneLegacyFlag)) {
    Invoke-InitialAbort 'stop/cleanup appeared while waiting for registration'
  }
  $ready = Read-Flag $readyFlag
  if ($ready -eq $SessionId) { $registered = $true; break }
  Start-Sleep -Milliseconds 250
}
if (-not $registered) {
  Invoke-InitialAbort 'registration-ready flag did not arrive before the deadline'
}
Test-AgentSession -RequireRegistration:$true
SetR registration_ready $true

$notepadPid = 0
$launcherPid = 0
$topPid = 0
$profile = $null
$preLaunchedPid = 0
$preFormalNotepad = @(Get-LayerCNotepadSnapshot)
$preFormalIds = @($preFormalNotepad | ForEach-Object { [int]$_.id })
SetR preexisting_notepad_pids ($preFormalIds -join ',')
SetR preexisting_notepad_count $preFormalNotepad.Count
if ($preFormalNotepad.Count -gt 0) {
  Abort-Agent 'pre-existing Notepad process(es); refusing to reuse or close a user instance'
}
$appPidFlag = Join-Path $EvidenceDir 'app-pid.flag'
if (Test-Path -LiteralPath $appPidFlag) {
  try { $preLaunchedPid = [int]((Get-Content -LiteralPath $appPidFlag -Raw).Trim()) } catch { $preLaunchedPid = 0 }
}
if ($preLaunchedPid -ne 0) {
  $notepadPid = $preLaunchedPid
} elseif ($App -eq 'edge') {
  $htmlFile = 'D:\EpoCanvaIsolation\edge-test.html'
  $html = '<!doctype html><html><head><title>EpoCanva W3A Test</title></head><body><textarea id="t" style="width:900px;height:400px;font-size:24px"></textarea><script>document.getElementById("t").focus();</script></body></html>'
  Set-Content -Path $htmlFile -Value $html -Encoding ASCII
  $testFile = $htmlFile
  $profile = 'D:\EpoCanvaIsolation\edge-profile-' + (Get-Date -Format 'HHmmssfff')
  $app = 'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
  $cmd = '"' + $app + '" --user-data-dir="' + $profile + '" --no-first-run --no-default-browser-check --disable-sync --disable-extensions --disable-gpu --app="file:///D:/EpoCanvaIsolation/edge-test.html"'
} elseif ($App -eq 'cmd') {
  $testFile = $null
  $app = 'C:\Windows\System32\cmd.exe'
  $cmd = '"' + $app + '"'
} elseif ($App -eq 'regedit') {
  $testFile = $null
  $app = 'C:\Windows\System32\regedit.exe'
  $cmd = '"' + $app + '"'
} elseif ($App -eq 'charmap') {
  $testFile = $null
  $app = 'C:\Windows\System32\charmap.exe'
  $cmd = '"' + $app + '"'
} elseif ($App -eq '7zfm') {
  $testFile = $null
  $app = 'C:\Program Files\7-Zip\7zFM.exe'
  $cmd = '"' + $app + '"'
} else {
  New-Item -ItemType Directory -Path $TestRoot -Force | Out-Null
  $testFile = Join-Path $TestRoot ('w3a-test-' + (Get-Date -Format 'HHmmssfff') + '-' + [guid]::NewGuid().ToString('N') + '.txt')
  Set-Content -Path $testFile -Value '' -Encoding ASCII -NoNewline
  $app = 'C:\Windows\System32\notepad.exe'
  $cmd = '"' + $app + '" "' + $testFile + '"'
  $leaf = Split-Path -Leaf $testFile
}
$si = New-Object W3ANative+STARTUPINFO
$si.cb = [Runtime.InteropServices.Marshal]::SizeOf([type][W3ANative+STARTUPINFO])
$si.lpDesktop = $null
$pi = New-Object W3ANative+PROCESS_INFORMATION
$launchStart = Get-Date
SetR formal_launch_entered $true
SetR formal_createprocess_called $true
SetR formal_createprocess_api 'W3ANative.CreateProcess (CharSet=Unicode, SetLastError=true)'
SetR formal_startupinfo_cb $si.cb
$created = $false
$launchWin32 = 0
$launchException = ''
try {
  $created = [W3ANative]::CreateProcess($app, $cmd, [IntPtr]::Zero, [IntPtr]::Zero, $false, 0, [IntPtr]::Zero, 'C:\Windows\System32', [ref]$si, [ref]$pi)
  $launchWin32 = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
} catch {
  $launchException = $_.Exception.ToString()
  $launchWin32 = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
}
SetR formal_raw_return ([bool]$created)
SetR formal_win32_error ([int]$launchWin32)
SetR formal_win32_message (Get-LayerCWin32Message ([int]$launchWin32))
SetR formal_exception $launchException
SetR formal_process_information ([ordered]@{
  hProcess = [int64]$pi.hProcess; hThread = [int64]$pi.hThread;
  process_id = [int]$pi.dwProcessId; thread_id = [int]$pi.dwThreadId
})
if ($created) {
  $launcherPid = $pi.dwProcessId
  $notepadPid = $launcherPid
  [void][W3ANative]::CloseHandle($pi.hProcess)
  [void][W3ANative]::CloseHandle($pi.hThread)
}
SetR notepad_launcher_pid $launcherPid
$topHwnd = [IntPtr]::Zero
if ($notepadPid -ne 0) {
  if ($App -eq 'regedit') {
    $mainHwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 120; $i++) {
      $mainHwnd = Find-ProcessWindow ([int]$notepadPid) 'RegEdit_RegEdit'
      if ($mainHwnd -ne [IntPtr]::Zero) { break }
      Start-Sleep -Milliseconds 250
    }
    if ($mainHwnd -ne [IntPtr]::Zero) {
      [void][W3ANative]::ShowWindow($mainHwnd, 9)
      [void][W3ANative]::SetForegroundWindow($mainHwnd)
      Start-Sleep -Milliseconds 400
      SendCtrlKey 0x46
      Start-Sleep -Milliseconds 600
      for ($i = 0; $i -lt 80; $i++) {
        $topHwnd = Find-ProcessWindow ([int]$notepadPid) '#32770'
        if ($topHwnd -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 250
      }
    }
  } else {
    $leaf = ''
    if ($testFile) { $leaf = Split-Path -Leaf $testFile }
    for ($i = 0; $i -lt 120; $i++) {
      if ($App -eq 'edge') { $topHwnd = Find-EdgeWindow } elseif ($App -eq 'cmd') { $topHwnd = Find-ConsoleWindow } elseif ($App -eq 'charmap' -or $App -eq '7zfm') { $topHwnd = Find-ProcessWindowAny ([int]$notepadPid) } else {
        $topHwnd = Find-NotepadWindow ([int]$notepadPid)
        if ($topHwnd -eq [IntPtr]::Zero -and $leaf -ne '') { $topHwnd = Find-NotepadWindowByTitle $leaf }
      }
      if ($topHwnd -ne [IntPtr]::Zero) { break }
      Start-Sleep -Milliseconds 250
    }
    # Store-packaged Notepad starts a launcher process; the document window belongs to a
    # different Notepad process. Match the unique test file name, then track the real PID.
    if ($topHwnd -ne [IntPtr]::Zero -and $App -ne 'edge' -and $App -ne 'cmd' -and $App -ne 'charmap' -and $App -ne '7zfm') {
      [uint32]$winPid = 0
      [void][W3ANative]::GetWindowThreadProcessId($topHwnd, [ref]$winPid)
      if ($winPid -ne 0 -and [int]$winPid -ne [int]$notepadPid) {
        SetR notepad_window_pid ([int]$winPid)
        $notepadPid = [int]$winPid
      }
    }
  }
}
SetR notepad_top_hwnd ([int64]$topHwnd)
if ($topHwnd -eq [IntPtr]::Zero) {
  Abort-Agent 'notepad window not found'
}
$winInfo = Get-LayerCWindowInfo $topHwnd
SetR formal_window ([ordered]@{ hwnd = [int64]$winInfo.hwnd; pid = [int]$winInfo.pid; class = $winInfo.class; title = $winInfo.title })
$winPid = [int]$winInfo.pid
$winProcess = Get-LayerCWindowProcess $winPid
SetR formal_window_process $winProcess
$winNew = -not ($preFormalIds -contains $winPid)
$winTitleLeaf = ($leaf -ne '' -and $winInfo.title.Contains($leaf))
$winCreatedAfter = $false
if ($winProcess -and $winProcess['created']) {
  try {
    $winCreated = [datetime]::Parse($winProcess['created']).ToLocalTime()
    $winCreatedAfter = ($winCreated -ge $launchStart.AddSeconds(-2))
  } catch { }
}
SetR formal_window_is_preexisting (-not $winNew)
SetR formal_window_title_has_unique_leaf $winTitleLeaf
SetR formal_window_created_after_launch $winCreatedAfter
if (-not $winNew -or -not $winTitleLeaf -or -not $winCreatedAfter) {
  # Never let Abort-Agent stop a window that was not proven new.
  $notepadPid = 0
  $topPid = 0
  Abort-Agent 'formal window/process identity is uncertain or reuses a pre-existing instance'
}
# The elevated process never touches the foreground. This normal-integrity agent publishes
# the window handle, briefly waits for the operator to click it if Windows refuses the
# agent's own SetForegroundWindow, and then verifies the foreground itself.
Set-Content -Path (Join-Path $EvidenceDir 'test-window-hwnd.flag') -Value ([int64]$topHwnd) -Encoding ASCII
$fgReadyFlag = Join-Path $EvidenceDir 'foreground-ready.flag'
for ($i = 0; $i -lt 20; $i++) {
  if (Test-Path -LiteralPath $fgReadyFlag) { break }
  Start-Sleep -Milliseconds 250
}
SetR foreground_relay_seen (Test-Path -LiteralPath $fgReadyFlag)
[void][W3ANative]::ShowWindow($topHwnd, 9)
Start-Sleep -Milliseconds 500
[void][W3ANative]::SetForegroundWindow($topHwnd)

$top = [System.Windows.Automation.AutomationElement]::FromHandle($topHwnd)
SetR notepad_class $top.Current.ClassName
SetR notepad_name $top.Current.Name

$doc = $null
if ($App -eq 'cmd') {
  $doc = $top
} elseif ($App -eq 'charmap' -or $App -eq '7zfm') {
  $editCond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Edit)
  $doc = $top.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $editCond)
} elseif ($App -eq 'edge') {
  $editCond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Edit)
  $edits = $top.FindAll([System.Windows.Automation.TreeScope]::Descendants, $editCond)
  $bestArea = 0
  foreach ($e in $edits) {
    try {
      $r = $e.Current.BoundingRectangle
      $area = [double]$r.Width * [double]$r.Height
      if ($area -gt $bestArea) { $bestArea = $area; $doc = $e }
    } catch { }
  }
} else {
  $docCond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Document)
  $doc = $top.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $docCond)
  if ($null -eq $doc) {
    $classCond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ClassNameProperty, 'RichEditD2DPT')
    $doc = $top.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $classCond)
  }
  if ($null -eq $doc) {
    $editCond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Edit)
    $doc = $top.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $editCond)
  }
}
if ($null -eq $doc) {
  Abort-Agent 'document control not found'
}
SetR document_class $doc.Current.ClassName
SetR document_hwnd ([int64]$doc.Current.NativeWindowHandle)
Set-Content -LiteralPath $testWindowReadyFlag -Value $SessionId -Encoding ASCII
SetR test_window_ready $true
try { $doc.SetFocus() } catch { SetR document_set_focus_error $_.Exception.Message }
[void][W3ANative]::SetForegroundWindow($topHwnd)
Start-Sleep -Milliseconds 800

$fg = [W3ANative]::GetForegroundWindow()
$fgRoot = [W3ANative]::GetAncestor($fg, 2)
$topRoot = [W3ANative]::GetAncestor($topHwnd, 2)
# After a UAC secure-desktop switch, Windows frequently denies the first foreground
# request of a background process. Retry inside a bounded budget with the relay sequence
# the orchestrator used in the W3-A trace rounds. The verdict is unchanged: unless Notepad
# is genuinely the foreground window, the agent refuses to type.
$fgAttempt = 0
for ($fgAttempt = 0; $fgAttempt -lt 16 -and $fgRoot -ne $topRoot; $fgAttempt++) {
  [void][W3ANative]::ShowWindow($topHwnd, 9)
  [W3ANative]::SwitchToThisWindow($topHwnd, $true)
  Start-Sleep -Milliseconds 400
  $fg = [W3ANative]::GetForegroundWindow()
  $fgRoot = [W3ANative]::GetAncestor($fg, 2)
  if ($fgRoot -eq $topRoot) { break }
  [uint32]$fgPid = 0
  $fgThread = [W3ANative]::GetWindowThreadProcessId($fg, [ref]$fgPid)
  $curThread = [W3ANative]::GetCurrentThreadId()
  [void][W3ANative]::AttachThreadInput($curThread, $fgThread, $true)
  [void][W3ANative]::SetForegroundWindow($topHwnd)
  [void][W3ANative]::AttachThreadInput($curThread, $fgThread, $false)
  Start-Sleep -Milliseconds 400
  $fg = [W3ANative]::GetForegroundWindow()
  $fgRoot = [W3ANative]::GetAncestor($fg, 2)
}
SetR foreground_attempts $fgAttempt
SetR foreground_before_typing_hwnd ([int64]$fg)
SetR foreground_is_notepad ($fgRoot -eq $topRoot)
if ($fgRoot -ne $topRoot) {
  # The tool's design lets the operator assist: if Windows keeps denying the automated
  # foreground handoff (the operator's last input usually belongs to their terminal or
  # the UAC secure desktop), wait for the operator to click the Notepad window itself.
  # The verdict is unchanged: typing starts only when Notepad is genuinely foreground.
  SetR foreground_operator_wait ((Get-Date).ToUniversalTime().ToString('o'))
  for ($opWait = 0; $opWait -lt 720; $opWait++) {
    Start-Sleep -Milliseconds 250
    $fg = [W3ANative]::GetForegroundWindow()
    $fgRoot = [W3ANative]::GetAncestor($fg, 2)
    if ($fgRoot -eq $topRoot) { break }
    if ((Get-Date).ToUniversalTime() -ge $agentDeadline) { break }
  }
  SetR foreground_operator_wait_attempts $opWait
  SetR foreground_before_typing_hwnd ([int64]$fg)
  SetR foreground_is_notepad ($fgRoot -eq $topRoot)
  if ($fgRoot -ne $topRoot) {
    Abort-Agent 'notepad is not foreground; refusing to type'
  }
}

[uint32]$topPid = 0
$notepadThread = [W3ANative]::GetWindowThreadProcessId($topRoot, [ref]$topPid)
SetR notepad_pid $topPid
SetR notepad_thread $notepadThread
$origHkl = [W3ANative]::GetKeyboardLayout($notepadThread)
SetR notepad_hkl_before ('0x{0:X8}' -f $origHkl.ToInt64())
$enHkl = [IntPtr]::Zero
if (-not $DryRun) {
  $enHkl = [W3ANative]::LoadKeyboardLayout('00000409', 1)
  SetR en_us_hkl ('0x{0:X8}' -f $enHkl.ToInt64())
  if ($enHkl -ne [IntPtr]::Zero) {
    [void][W3ANative]::PostMessage($topHwnd, 0x50, [IntPtr]::Zero, $enHkl)
    $docH = [IntPtr]$doc.Current.NativeWindowHandle
    if ($docH -ne [IntPtr]::Zero) { [void][W3ANative]::PostMessage($docH, 0x50, [IntPtr]::Zero, $enHkl) }
    Start-Sleep -Milliseconds 1500
    $hklAfter = [W3ANative]::GetKeyboardLayout($notepadThread)
    SetR notepad_hkl_after ('0x{0:X8}' -f $hklAfter.ToInt64())
    SetR notepad_hkl_switched ($hklAfter.ToInt64() -eq $enHkl.ToInt64())
  }
}
$moduleLoaded = $false
for ($i = 0; $i -lt 40; $i++) {
  if ((Has-Module ([int]$topPid) 'epocanva_tsf.dll') -or ($App -eq 'cmd' -and (Has-Module ([int]$notepadPid) 'epocanva_tsf.dll'))) { $moduleLoaded = $true; break }
  Start-Sleep -Milliseconds 250
}
SetR tsf_dll_loaded_in_notepad $moduleLoaded

if (-not (Ensure-DocFocus $true)) {
  Abort-Agent 'document does not have keyboard focus before warm-up'
}

$rimeLoaded = $false
if (-not $DryRun) {
  # The test opens a unique empty file; refuse to type if the active document is not empty.
  if (-not (Ensure-DocFocus $true)) { Abort-Agent 'test document does not have keyboard focus' }
  $freshText = Normalize (Get-DocText $doc)
  SetR text_after_new_document $freshText
  if ($App -ne 'cmd' -and $App -ne '7zfm' -and $freshText -ne '') { Abort-Agent 'test document is not empty; refusing to type' }
  for ($i = 0; $i -lt 80; $i++) {
    if ((Has-Module ([int]$topPid) 'rime.dll') -or ($App -eq 'cmd' -and (Has-Module ([int]$notepadPid) 'rime.dll'))) { $rimeLoaded = $true; break }
    Start-Sleep -Milliseconds 250
  }
  SetR rime_dll_loaded_before_typing $rimeLoaded
  Start-Sleep -Milliseconds 1200
  if (-not (Ensure-DocFocus $true)) { Abort-Agent 'test document focus lost before typing' }
}

Start-Sleep -Milliseconds 600
$pre = Get-DocText $doc
SetR text_before_typing (Normalize $pre)
$titlePre = Get-WindowTitle $topHwnd
SetR title_before_typing $titlePre
if ($App -eq 'notepad') {
  if ((Normalize $pre) -ne '') { Abort-Agent 'test document is not empty before typing' }
  if ($titlePre.StartsWith('*')) { Abort-Agent 'test document was modified before typing' }
}

$gtiPre = New-Object W3ANative+GUITHREADINFO
$gtiPre.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf([type][W3ANative+GUITHREADINFO])
[uint32]$gtiPid = 0
$gtiThread = [W3ANative]::GetWindowThreadProcessId($topRoot, [ref]$gtiPid)
[void][W3ANative]::GetGUIThreadInfo($gtiThread, [ref]$gtiPre)
SetR focus_before_typing_hwnd ([int64]$gtiPre.hwndFocus)
SetR focus_before_typing_is_child ([W3ANative]::IsChild($topRoot, $gtiPre.hwndFocus))
SetR dry_run ([bool]$DryRun)
if ($DryRun) {
  $dryPng = Join-Path $EvidenceDir 'dry-run.png'
  Save-ScreenPng $dryPng
  SetR dry_run_screen_png $dryPng
  SetR dry_run_screen_saved (Test-Path $dryPng)
  if ($notepadPid -ne 0) { Stop-Process -Id ([int]$notepadPid) -Force -ErrorAction SilentlyContinue }
  if ($topPid -ne 0) { Stop-Process -Id ([int]$topPid) -Force -ErrorAction SilentlyContinue }
  if ($launcherPid -ne 0 -and $launcherPid -ne $notepadPid -and $launcherPid -ne $topPid) {
    Stop-Process -Id ([int]$launcherPid) -Force -ErrorAction SilentlyContinue
  }
  if ($testFile) { Remove-Item -LiteralPath $testFile -Force -ErrorAction SilentlyContinue }
  $result['passed'] = $false
  $result | ConvertTo-Json -Depth 6 | Set-Content -Path (Join-Path $EvidenceDir 'dry-run-result.json') -Encoding UTF8
  Log 'DRY RUN complete'
  exit 0
}

# Final pre-input gate: session/deadline still valid, the exact document child still holds
# focus, the en-US layout is active, and the Notepad PID is this round's process. Only then
# write the two handshake flags the trigger requires before it considers a test started.
Test-RuntimeGate
if (-not $gtiPre.hwndFocus -or -not [W3ANative]::IsChild($topRoot, $gtiPre.hwndFocus)) {
  Abort-Agent 'document child does not hold focus before typing'
}
$notepadElevated = Test-ProcessElevated ([int]$topPid)
SetR notepad_elevated $notepadElevated
if ($null -eq $notepadElevated) {
  Abort-Agent 'cannot verify the Notepad token integrity level'
}
if ($notepadElevated) {
  Abort-Agent 'Notepad is elevated; refusing normal-integrity input'
}
if (-not $DryRun) {
  $hklGate = [W3ANative]::GetKeyboardLayout($notepadThread)
  if ($enHkl -eq [IntPtr]::Zero -or $hklGate.ToInt64() -ne $enHkl.ToInt64()) {
    Abort-Agent 'en-US input layout is not active before typing'
  }
}
Set-Content -LiteralPath $agentReadyFlag -Value $SessionId -Encoding ASCII
Test-RuntimeGate
Set-Content -LiteralPath $testStartFlag -Value $SessionId -Encoding ASCII
SetR test_started_utc ((Get-Date).ToUniversalTime().ToString('o'))

$typedChars = 0
foreach ($ch in 'alpha hel'.ToCharArray()) {
  Test-RuntimeGate
  $fgKey = [W3ANative]::GetForegroundWindow()
  $fgKeyRoot = [W3ANative]::GetAncestor($fgKey, 2)
  if ($fgKeyRoot -ne $topRoot) {
    SetR keys_sent_before_abort $typedChars
    Abort-Agent 'foreground changed during typing; stopped before the next key'
  }
  $gtiKey = New-Object W3ANative+GUITHREADINFO
  $gtiKey.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf([type][W3ANative+GUITHREADINFO])
  [void][W3ANative]::GetGUIThreadInfo($notepadThread, [ref]$gtiKey)
  if (($gtiKey.hwndFocus -eq [IntPtr]::Zero) -or (-not [W3ANative]::IsChild($topRoot, $gtiKey.hwndFocus))) {
    SetR keys_sent_before_abort $typedChars
    Abort-Agent 'document focus changed during typing; stopped before the next key'
  }
  $hklKey = [W3ANative]::GetKeyboardLayout($notepadThread)
  if ($enHkl -eq [IntPtr]::Zero -or $hklKey.ToInt64() -ne $enHkl.ToInt64()) {
    SetR keys_sent_before_abort $typedChars
    Abort-Agent 'input layout changed during typing; stopped before the next key'
  }
  $shiftNow = ([W3ANative]::GetAsyncKeyState(0x10) -band 0x8000) -ne 0
  $ctrlNow = ([W3ANative]::GetAsyncKeyState(0x11) -band 0x8000) -ne 0
  $altNow = ([W3ANative]::GetAsyncKeyState(0x12) -band 0x8000) -ne 0
  $capsNow = ([W3ANative]::GetKeyState(0x14) -band 1) -ne 0
  if ($shiftNow -or $ctrlNow -or $altNow -or $capsNow) {
    SetR keys_sent_before_abort $typedChars
    SetR modifier_shift $shiftNow
    SetR modifier_ctrl $ctrlNow
    SetR modifier_alt $altNow
    SetR modifier_caps $capsNow
    Abort-Agent 'keyboard modifier changed during typing; stopped before the next key'
  }
  if ($ch -eq ' ') { SendVk 0x20 } else { SendVk ([int][char]([string]$ch).ToUpperInvariant()) }
  $typedChars++
  # Paced typing: the product starts its engine asynchronously on first activation and
  # passes keys through literally until the session is ready. Round 6 (2026-11-02) proved
  # the cold start finishes about one second after the first key at full speed, so all
  # keys landed literal and no composition ever existed. A pause between keys gives the
  # cold start time to finish while keys are still arriving; the per-key gates above are
  # unchanged and the document is still asserted to equal 'alpha hel' afterwards.
  Start-Sleep -Milliseconds 400
}
SetR typed_char_count $typedChars
Start-Sleep -Milliseconds 1000
$earlyPng = Join-Path $EvidenceDir 'after-typing-1s.png'
Save-ScreenPng $earlyPng
SetR after_typing_1s_png $earlyPng
SetR after_typing_1s_png_saved (Test-Path $earlyPng)
Start-Sleep -Milliseconds 400
$afterType = Get-DocText $doc
SetR text_after_typing (Normalize $afterType)
SetR title_after_typing (Get-WindowTitle $topHwnd)
if ((Normalize $afterType) -cne 'alpha hel') {
  Abort-Agent 'host text does not match the injected keys; refusing to select'
}

$cand = [IntPtr]::Zero
for ($i = 0; $i -lt 250; $i++) {
  if ($App -eq 'cmd') { $h = Find-CandidateWindow 0 } else { $h = Find-CandidateWindow ([int]$topPid) }
  if ($h -ne [IntPtr]::Zero) { $cand = $h; break }
  Start-Sleep -Milliseconds 100
}
SetR candidate_hwnd ([int64]$cand)
SetR candidate_visible (($cand -ne [IntPtr]::Zero) -and [W3ANative]::IsWindowVisible($cand))
if ($cand -ne [IntPtr]::Zero) {
  $crect = New-Object W3ANative+RECT
  [void][W3ANative]::GetWindowRect($cand, [ref]$crect)
  SetR candidate_rect (('{0},{1},{2},{3}' -f $crect.Left, $crect.Top, $crect.Right, $crect.Bottom))
  $candPng = Join-Path $EvidenceDir 'candidate-window.png'
  $candOk = Save-WindowPng $cand $candPng
  SetR candidate_window_png $candPng
  SetR candidate_window_png_saved $candOk
}
$beforePng = Join-Path $EvidenceDir 'before-selection.png'
Save-ScreenPng $beforePng
SetR screen_before_selection_png $beforePng
SetR screen_before_selection_saved (Test-Path $beforePng)

if ($cand -eq [IntPtr]::Zero) {
  Abort-Agent 'candidate window did not appear'
}

if (-not (Ensure-DocFocus $false)) {
  Abort-Agent 'document focus lost before selection; no input sent'
}
$fgDown = [W3ANative]::GetForegroundWindow()
if ([W3ANative]::GetAncestor($fgDown, 2) -ne $topRoot) {
  Abort-Agent 'foreground left the test window before selection; no input sent'
}

# Select row 1 by its real window rectangle. Row 0 is the literal row by the product's
# candidate snapshot contract; row 1 is a completion or spelling row. This is not a blind
# Down count: no arrow or Enter key is sent, and a click that does not change the document
# aborts instead of retrying.
Test-RuntimeGate
if (-not (Ensure-DocFocus $true)) { Abort-Agent 'document focus lost before selection; no input sent' }
$crect = New-Object W3ANative+RECT
if (-not [W3ANative]::GetWindowRect($cand, [ref]$crect)) {
  Abort-Agent 'candidate window rectangle unavailable before selection'
}
$candDpi = [W3ANative]::GetDpiForWindow($cand)
if ($candDpi -eq 0) { $candDpi = 96 }
$candPad = [int][Math]::Round((6.0 * $candDpi) / 96.0)
$candRowHeight = [int][Math]::Round((22.0 * $candDpi) / 96.0)
$candHeight = $crect.Bottom - $crect.Top
if ($candHeight -lt (2 * $candRowHeight + 2 * $candPad)) {
  Abort-Agent 'candidate window has no visible second row; refusing to select'
}
$row1X = $crect.Left + [int](($crect.Right - $crect.Left) / 2)
$row1Y = $crect.Top + $candPad + $candRowHeight + [int]($candRowHeight / 2)
SetR candidate_row1_point ($row1X.ToString() + ',' + $row1Y.ToString())
SetR selection_row 1
SetR selection_row_kind 'non_raw_by_product_contract'
SetR selection_method 'mouse_row_1_click'
$old = New-Object W3ANative+POINT
[void][W3ANative]::GetCursorPos([ref]$old)
[void][W3ANative]::SetCursorPos($row1X, $row1Y)
Start-Sleep -Milliseconds 150
[W3ANative]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)
[W3ANative]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)
Start-Sleep -Milliseconds 800
[void][W3ANative]::SetCursorPos($old.X, $old.Y)
$final = Normalize (Get-DocText $doc)
for ($i = 0; $i -lt 30; $i++) {
  if ($final -ne (Normalize $afterType)) { break }
  Start-Sleep -Milliseconds 100
  $final = Normalize (Get-DocText $doc)
}
SetR text_after_selection $final
if ($final -eq (Normalize $afterType)) {
  Abort-Agent 'row 1 click did not change the document; no blind Down/Enter retry'
}

Test-RuntimeGate
$fgAfter = [W3ANative]::GetForegroundWindow()
$fgAfterRoot = [W3ANative]::GetAncestor($fgAfter, 2)
$gti = New-Object W3ANative+GUITHREADINFO
$gti.cbSize = [System.Runtime.InteropServices.Marshal]::SizeOf([type][W3ANative+GUITHREADINFO])
[void][W3ANative]::GetGUIThreadInfo($notepadThread, [ref]$gti)
SetR focus_after_selection_hwnd ([int64]$gti.hwndFocus)
SetR focus_after_selection_is_child ([W3ANative]::IsChild($topRoot, $gti.hwndFocus))
SetR foreground_after_selection_hwnd ([int64]$fgAfter)
SetR foreground_after_is_notepad ($fgAfterRoot -eq $topRoot)
SetR candidate_visible_after (($cand -ne [IntPtr]::Zero) -and [W3ANative]::IsWindowVisible($cand))
$afterPng = Join-Path $EvidenceDir 'after-selection.png'
Save-ScreenPng $afterPng
SetR screen_after_selection_png $afterPng
SetR screen_after_selection_saved (Test-Path $afterPng)

SetR title_after_selection (Get-WindowTitle $topHwnd)
$preN = Normalize $afterType
if (-not $preN) { $preN = Get-TitleText (Get-WindowTitle $topHwnd) }
$finalN = Normalize $final
if (-not $finalN) { $finalN = Get-TitleText (Get-WindowTitle $topHwnd) }
$prefixKept = ($null -ne $finalN) -and ($finalN.Length -gt 6) -and ($finalN.Substring(0, 6) -ceq 'alpha ')
$focusKept = ($fgAfterRoot -eq $topRoot) -and ($gti.hwndFocus -ne [IntPtr]::Zero) -and [W3ANative]::IsChild($topRoot, $gti.hwndFocus)
SetR prefix_alpha_kept $prefixKept
SetR focus_kept_after_selection $focusKept
$passed = ($preN -ceq 'alpha hel') -and $prefixKept -and ($finalN -cne $preN) -and $focusKept
SetR passed $passed

if ($origHkl -ne [IntPtr]::Zero -and $prevFg -ne [IntPtr]::Zero) {
  $prevRoot = [W3ANative]::GetAncestor($prevFg, 2)
  if ($prevRoot -ne [IntPtr]::Zero) { [void][W3ANative]::PostMessage($prevRoot, 0x50, [IntPtr]::Zero, $origHkl) }
}
[void][W3ANative]::ActivateKeyboardLayout($origHkl, 0)
if ($notepadPid -ne 0) { Stop-Process -Id ([int]$notepadPid) -Force -ErrorAction SilentlyContinue }
if ($topPid -ne 0) { Stop-Process -Id ([int]$topPid) -Force -ErrorAction SilentlyContinue }
if ($launcherPid -ne 0 -and $launcherPid -ne $notepadPid -and $launcherPid -ne $topPid) {
  Stop-Process -Id ([int]$launcherPid) -Force -ErrorAction SilentlyContinue
}
if ($testFile) { Remove-Item -LiteralPath $testFile -Force -ErrorAction SilentlyContinue }
if ($null -ne (Get-Variable -Name profile -ErrorAction SilentlyContinue) -and $profile) {
  Start-Sleep -Milliseconds 500
  Remove-Item -LiteralPath $profile -Recurse -Force -ErrorAction SilentlyContinue
}
if ($prevFg -ne [IntPtr]::Zero) { [void][W3ANative]::SetForegroundWindow($prevFg) }

$result | ConvertTo-Json -Depth 6 | Set-Content -Path (Join-Path $EvidenceDir 'agent-result.json') -Encoding UTF8
Log ('RESULT passed=' + $passed)
if ($passed) { exit 0 } else { exit 3 }
