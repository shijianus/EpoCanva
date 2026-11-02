# EpoCanva W3-A -- one preflight-first formal round.
#
# Order: normal-integrity agent -> preflight-ready -> (user approves one UAC) -> elevated
# registration/activation -> registration-ready -> the SAME agent creates the formal
# Notepad -> test-window-ready/test-start -> agent exits -> cleanup-complete.
# No approval is reused, and a failed/invalid round is not retried.
[CmdletBinding()]
param(
  [switch]$Elevate,
  [switch]$Formal,
  [string]$StageDir = '',
  [string]$RepoRoot = ''
)

$ErrorActionPreference = 'Continue'
$repo = $null
if ($RepoRoot) {
  $repo = (Resolve-Path -LiteralPath $RepoRoot).Path
} else {
  $candidate = (Resolve-Path -LiteralPath $PSScriptRoot).Path
  while ($candidate) {
    if (Test-Path -LiteralPath (Join-Path $candidate 'AGENTS.md')) { $repo = $candidate; break }
    $parent = Split-Path -Parent $candidate
    if (-not $parent -or $parent -eq $candidate) { break }
    $candidate = $parent
  }
}
if (-not $repo) {
  Write-Output 'ABORT: cannot locate the repository root; pass -RepoRoot.'
  exit 5
}
$agent = Join-Path $PSScriptRoot 'c_layer_agent.ps1'
$orchestrator = Join-Path $PSScriptRoot 'orchestrator.ps1'
$listener = Join-Path $PSScriptRoot 'dbwin_listener.ps1'
$restoreLang = Join-Path $PSScriptRoot 'restore_language.ps1'

if (-not $Elevate) {
  Write-Output 'Use -Elevate -StageDir <frozen stage> to run one authorized formal round.'
  Write-Output 'The normal-integrity agent is started first and must finish preflight before UAC.'
  exit 0
}
if (-not $StageDir -or -not (Test-Path -LiteralPath $StageDir -PathType Container)) {
  Write-Output ('ABORT: missing immutable stage directory: ' + $StageDir)
  exit 5
}

$sessionId = [guid]::NewGuid().ToString('N')
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
$startUtc = (Get-Date).ToUniversalTime()
$deadlineUtc = $startUtc.AddMinutes(8)
$startTick = [EpoLayerCTick]::GetTickCount64()
$evidence = Join-Path $repo ('build\w3a-c-local\evidence-' + $sessionId)
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$testRoot = Join-Path $repo 'build\w3a-c-local\test-documents'
New-Item -ItemType Directory -Path $testRoot -Force | Out-Null

$trace = Join-Path $evidence 'trace.txt'
$listenerStop = Join-Path $evidence 'listener-stop.flag'
$stopFlag = Join-Path $evidence 'stop.flag'
$preflightFlag = Join-Path $evidence 'preflight-ready.flag'
$registrationFlag = Join-Path $evidence 'registration-ready.flag'
$testWindowFlag = Join-Path $evidence 'test-window-ready.flag'
$testStartFlag = Join-Path $evidence 'test-start.flag'
$agentDone = Join-Path $evidence 'agent-done.flag'
$cleanupFlag = Join-Path $evidence 'cleanup-complete.flag'
$sessionPath = Join-Path $evidence 'session.json'
$summary = Join-Path $evidence 'elevated-summary.txt'

$session = [ordered]@{
  session_id = $sessionId
  start_utc = $startUtc.ToString('o')
  deadline_utc = $deadlineUtc.ToString('o')
  evidence_dir = $evidence
  test_root = $testRoot
  formal = [bool]$Formal
  stage_dir = $StageDir
  trigger_pid = $PID
  trigger_session = (Get-Process -Id $PID).SessionId
  agent_script = $agent
  orchestrator_script = $orchestrator
}
$session | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $sessionPath -Encoding UTF8
Write-Output ('session_id=' + $sessionId)
Write-Output ('evidence=' + $evidence)
Write-Output ('deadline_utc=' + $deadlineUtc.ToString('o'))

function Write-Flag([string]$path) { Set-Content -LiteralPath $path -Value $sessionId -Encoding ASCII }
function Test-Flag([string]$path) {
  if (-not (Test-Path -LiteralPath $path)) { return $false }
  try { return ((Get-Content -LiteralPath $path -Raw -ErrorAction Stop).Trim() -eq $sessionId) }
  catch { return $false }
}
function Test-ClockJump {
  $wall = ((Get-Date).ToUniversalTime() - $startUtc).TotalSeconds
  $mono = ([EpoLayerCTick]::GetTickCount64() - $startTick) / 1000.0
  return (($wall - $mono) -gt 30.0)
}
function Stop-ProcessIfRunning($proc) {
  if ($null -eq $proc) { return }
  try { if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue } } catch { }
}

foreach ($f in @($preflightFlag, $registrationFlag, $testWindowFlag, $testStartFlag,
                 $agentDone, $cleanupFlag, $listenerStop, $stopFlag, $trace)) {
  Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue
}

$listenerArgs = '-NoProfile -ExecutionPolicy Bypass -File "' + $listener + '" -Path "' + $trace + '" -StopFile "' + $listenerStop + '"'
$listenerProc = Start-Process -FilePath powershell.exe -WindowStyle Hidden -PassThru -ArgumentList $listenerArgs
Start-Sleep -Milliseconds 1200
Write-Output ('listener_pid=' + $listenerProc.Id + ' trace_exists=' + (Test-Path -LiteralPath $trace))

# The normal-integrity agent starts first and performs preflight before any elevation.
$agentArgs = '-NoProfile -ExecutionPolicy Bypass -File "' + $agent + '"' +
             ' -EvidenceDir "' + $evidence + '" -App notepad' +
             ' -SessionId "' + $sessionId + '"' +
             ' -DeadlineUtc "' + $deadlineUtc.ToString('o') + '"' +
             ' -TestRoot "' + $testRoot + '"'
$agentProc = Start-Process -FilePath powershell.exe -WindowStyle Hidden -PassThru -ArgumentList $agentArgs
Write-Output ('agent_pid=' + $agentProc.Id)

$preflightOk = $false
$invalid = $false
for ($i = 0; $i -lt 480; $i++) {
  if (Test-Flag $preflightFlag) { $preflightOk = $true; break }
  if ($agentProc.HasExited) { break }
  if (Test-Path -LiteralPath $stopFlag) { break }
  if ((Get-Date).ToUniversalTime() -ge $deadlineUtc) { $invalid = $true; break }
  if (Test-ClockJump) { $invalid = $true; break }
  Start-Sleep -Milliseconds 250
}
Write-Output ('preflight_ready=' + $preflightOk)
Write-Output ('preflight_deadline_or_clock_invalid=' + $invalid)
if (-not $preflightOk -or $invalid) {
  Set-Content -LiteralPath $stopFlag -Value $sessionId -Encoding ASCII
  Set-Content -LiteralPath $listenerStop -Value 'stop' -Encoding ASCII
  Stop-ProcessIfRunning $agentProc
  Stop-ProcessIfRunning $listenerProc
  Write-Output 'ABORT: preflight did not become valid; no UAC and no registration were requested.'
  if ($invalid) { exit 8 } else { exit 3 }
}

# Only now request one UAC for the elevated registration/activation and cleanup.
$orchArgs = '-NoProfile -ExecutionPolicy Bypass -File "' + $orchestrator + '"' +
            ' -EvidenceDir "' + $evidence + '"' +
            ' -SessionId "' + $sessionId + '"' +
            ' -DeadlineUtc "' + $deadlineUtc.ToString('o') + '"' +
            ' -StageDir "' + $StageDir + '"' +
            ' -RepoRoot "' + $repo + '"'
if (-not $Formal) { $orchArgs += ' -TraceBuild' }
Start-Process -FilePath powershell.exe -Verb RunAs -WindowStyle Hidden -ArgumentList $orchArgs | Out-Null

$registered = $false
for ($i = 0; $i -lt 480; $i++) {
  if (Test-Flag $registrationFlag) { $registered = $true; break }
  if ($agentProc.HasExited) { break }
  if (Test-Path -LiteralPath $stopFlag) { break }
  if ((Get-Date).ToUniversalTime() -ge $deadlineUtc) { $invalid = $true; break }
  if (Test-ClockJump) { $invalid = $true; break }
  Start-Sleep -Milliseconds 250
}
Write-Output ('registration_ready=' + $registered)
if (-not $registered -or $invalid) {
  Set-Content -LiteralPath $stopFlag -Value $sessionId -Encoding ASCII
  Set-Content -LiteralPath $listenerStop -Value 'stop' -Encoding ASCII
  Stop-ProcessIfRunning $agentProc
  Stop-ProcessIfRunning $listenerProc
  Write-Output 'ABORT: registration did not become ready (UAC cancelled/failed or timeout); no retry and no approval reuse.'
  if ($invalid) { exit 8 } else { exit 7 }
}

$agentTimedOut = $false
while (-not $agentProc.HasExited) {
  if (Test-ClockJump) { Set-Content -LiteralPath $stopFlag -Value $sessionId -Encoding ASCII; $invalid = $true; break }
  if ((Get-Date).ToUniversalTime() -ge $deadlineUtc) { Set-Content -LiteralPath $stopFlag -Value $sessionId -Encoding ASCII; $invalid = $true; break }
  if (Test-Path -LiteralPath $stopFlag) { break }
  Start-Sleep -Milliseconds 250
}
if ($invalid) {
  $until = (Get-Date).AddSeconds(20)
  while (-not $agentProc.HasExited -and (Get-Date) -lt $until) { Start-Sleep -Milliseconds 250 }
  Stop-ProcessIfRunning $agentProc
}
if ($agentProc.HasExited) { $agentExit = $agentProc.ExitCode } else { $agentExit = 99; $agentTimedOut = $true }
Write-Output ('agent_exit=' + $agentExit)
Write-Output ('agent_timed_out=' + $agentTimedOut)
Write-Flag $agentDone

$testWindowReady = Test-Flag $testWindowFlag
$testStarted = Test-Flag $testStartFlag
Write-Output ('test_window_ready=' + $testWindowReady)
Write-Output ('test_start=' + $testStarted)
if (-not $testWindowReady -or -not $testStarted) { $invalid = $true }

$cleanupSeen = $false
for ($i = 0; $i -lt 240; $i++) {
  if (Test-Flag $cleanupFlag) { $cleanupSeen = $true; break }
  if (Test-ClockJump) { $invalid = $true; break }
  Start-Sleep -Milliseconds 500
}
Write-Output ('cleanup_seen=' + $cleanupSeen)
if (-not $cleanupSeen) { $invalid = $true }

Set-Content -LiteralPath $listenerStop -Value 'stop' -Encoding ASCII
Start-Sleep -Milliseconds 800
Stop-ProcessIfRunning $listenerProc
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $restoreLang | Out-Null

if (Test-Path -LiteralPath $summary) {
  Write-Output '---SUMMARY-TAIL---'
  Get-Content -LiteralPath $summary -Tail 30 | ForEach-Object { Write-Output $_ }
}
$agentPassed = $false
$agentInvalid = $false
$agentResultPath = Join-Path $evidence 'agent-result.json'
if (Test-Path -LiteralPath $agentResultPath) {
  try {
    $agentResult = Get-Content -LiteralPath $agentResultPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $agentPassed = [bool]$agentResult.passed
    $agentInvalid = [bool]$agentResult.invalid_round
    if ($agentResult.session_id -and $agentResult.session_id -ne $sessionId) { $agentInvalid = $true }
  } catch { Write-Output 'agent_result_parse_error' }
}
Write-Output ('agent_passed=' + $agentPassed)
Write-Output ('agent_invalid_round=' + $agentInvalid)
$overall = ($agentPassed -and $cleanupSeen -and (-not $invalid) -and (-not $agentInvalid) -and $agentExit -eq 0)
Write-Output ('overall=' + $overall)
if ($overall) { exit 0 } elseif ($invalid) { exit 8 } else { exit 3 }
