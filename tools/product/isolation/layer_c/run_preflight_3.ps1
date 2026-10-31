# EpoCanva W3-A -- three consecutive normal-integrity preflights.
# No registration, no activation, no input, no UAC. Each run uses a fresh unique empty
# Notepad; the runner refuses to continue if any Notepad process exists before a run or
# remains after it.
[CmdletBinding()]
param(
  [string]$RepoRoot = '',
  [string]$TestRoot = '',
  [string]$EvidenceRoot = '',
  [int]$Runs = 3
)

$ErrorActionPreference = 'Continue'
if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }
$RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
if (-not $TestRoot) { $TestRoot = Join-Path $RepoRoot 'build\w3a-c-local\test-documents' }
if (-not $EvidenceRoot) { $EvidenceRoot = Join-Path $RepoRoot 'build\w3a-c-local\preflight' }
$sessionId = [guid]::NewGuid().ToString('N')
$startUtc = (Get-Date).ToUniversalTime()
$deadlineUtc = $startUtc.AddMinutes(10)
$evidence = Join-Path $EvidenceRoot ('session-' + $sessionId)
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
New-Item -ItemType Directory -Path $TestRoot -Force | Out-Null
$summaryPath = Join-Path $evidence 'preflight-summary.json'
$summary = [ordered]@{
  session_id = $sessionId
  start_utc = $startUtc.ToString('o')
  deadline_utc = $deadlineUtc.ToString('o')
  repo_root = $RepoRoot
  test_root = $TestRoot
  evidence_dir = $evidence
  runs_requested = $Runs
  runs = @()
}
function Save-Summary {
  $summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
}
$agent = Join-Path $PSScriptRoot 'c_layer_agent.ps1'
for ($index = 1; $index -le $Runs; $index++) {
  $pre = @(Get-Process Notepad -ErrorAction SilentlyContinue)
  if ($pre.Count -gt 0) {
    $summary.runs += [ordered]@{
      run = $index; ok = $false; blocked = 'pre-existing Notepad before run';
      pids = ($pre | ForEach-Object { $_.Id }) -join ','
    }
    Save-Summary
    Write-Output ('preflight_abort=pre_existing_notepad_before_run_' + $index)
    exit 3
  }
  $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $agent + '"'),
            '-SessionId', ('"' + $sessionId + '"'),
            '-DeadlineUtc', ('"' + $deadlineUtc.ToString('o') + '"'),
            '-EvidenceDir', ('"' + $evidence + '"'),
            '-TestRoot', ('"' + $TestRoot + '"'),
            '-App', 'notepad',
            '-PreflightOnly',
            '-PreflightRunIndex', $index.ToString())
  $process = Start-Process -FilePath powershell.exe -WindowStyle Hidden -PassThru -ArgumentList $args
  $process.WaitForExit(180000) | Out-Null
  $exit = $null
  if ($process.HasExited) { $exit = $process.ExitCode }
  $runJson = Join-Path $evidence ('preflight-run-' + $index + '\preflight.json')
  $run = $null
  if (Test-Path -LiteralPath $runJson) {
    try { $run = Get-Content -LiteralPath $runJson -Raw -Encoding UTF8 | ConvertFrom-Json } catch { }
  }
  $post = @(Get-Process Notepad -ErrorAction SilentlyContinue)
  $summary.runs += [ordered]@{
    run = $index; exit = $exit; ok = [bool]($run -and $run.ok);
    json = $runJson; remaining_notepad = $post.Count;
    remaining_pids = ($post | ForEach-Object { $_.Id }) -join ','
  }
  Save-Summary
  if ($null -eq $exit -or $exit -ne 0 -or $null -eq $run -or -not $run.ok -or $post.Count -ne 0) {
    Write-Output ('preflight_abort=run_' + $index + '_failed exit=' + $exit + ' remaining_notepad=' + $post.Count)
    exit 3
  }
  Write-Output ('preflight_run=' + $index + ' ok=True')
}
$summary.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
$summary.ok = $true
Save-Summary
Write-Output ('preflight_session_ok=' + $sessionId)
Write-Output ('summary=' + $summaryPath)
exit 0
