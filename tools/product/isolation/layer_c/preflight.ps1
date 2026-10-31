# EpoCanva W3-A -- one normal-integrity preflight run.
# No registration, no activation, no input. Launches only a unique empty Notepad, verifies
# ownership, and closes only that new window/process.
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)][string]$SessionId,
  [Parameter(Mandatory = $true)][string]$DeadlineUtc,
  [Parameter(Mandatory = $true)][string]$EvidenceDir,
  [Parameter(Mandatory = $true)][string]$TestRoot,
  [int]$RunIndex = 1
)

$ErrorActionPreference = 'Continue'
if (-not [System.IO.Path]::IsPathRooted($EvidenceDir)) {
  $EvidenceDir = [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $EvidenceDir))
}
if (-not [System.IO.Path]::IsPathRooted($TestRoot)) {
  $TestRoot = [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $TestRoot))
}
. (Join-Path $PSScriptRoot 'layer_c_preflight_lib.ps1')
$deadline = [datetime]::Parse($DeadlineUtc).ToUniversalTime()
if ((Get-Date).ToUniversalTime() -ge $deadline) {
  Write-Output 'preflight_invalid=deadline_passed'
  exit 8
}
$cleanupFlag = Join-Path $EvidenceDir 'cleanup-complete.flag'
$stopFlag = Join-Path $EvidenceDir 'stop.flag'
if (Test-Path -LiteralPath $cleanupFlag) {
  Write-Output 'preflight_invalid=cleanup_already_complete'
  exit 8
}
if (Test-Path -LiteralPath $stopFlag) {
  Write-Output 'preflight_invalid=stop_flag_present'
  exit 8
}
$result = Invoke-LayerCPreflight -SessionId $SessionId -RunIndex $RunIndex `
    -EvidenceDir $EvidenceDir -TestRoot $TestRoot
if ($result.ok) {
  Write-Output ('preflight_run=' + $RunIndex + ' ok=True')
  exit 0
}
Write-Output ('preflight_run=' + $RunIndex + ' ok=False')
exit 3
