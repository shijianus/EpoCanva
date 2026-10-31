# EpoCanva W3-A -- stage an immutable run copy of the normal-privilege test tools.
# The tools directory is the maintained source; build/ holds only this run copy and evidence.
[CmdletBinding()]
param(
  [string]$RepoRoot = '',
  [string]$RunRoot = ''
)
$ErrorActionPreference = 'Stop'
if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path }
$RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName
if (-not $RunRoot) { $RunRoot = Join-Path $RepoRoot ('build\w3a-c-local\run-' + (Get-Date -Format 'yyyyMMdd-HHmmssfff')) }
$RunRoot = [System.IO.Path]::GetFullPath($RunRoot)
$buildRoot = [System.IO.Path]::GetFullPath((Join-Path $RepoRoot 'build\w3a-c-local'))
if (-not $RunRoot.StartsWith($buildRoot + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
  throw "run copy must be below $buildRoot"
}
New-Item -ItemType Directory -Path $RunRoot -Force | Out-Null
$files = @(
  'c_layer_agent.ps1', 'layer_c_preflight_lib.ps1', 'preflight.ps1',
  'run_preflight_3.ps1', 'trigger.ps1', 'orchestrator.ps1',
  'dbwin_listener.ps1', 'restore_language.ps1'
)
$identity = [ordered]@{
  generated_utc = (Get-Date).ToUniversalTime().ToString('o')
  source_git_head = ''
  source_tools_dir = $PSScriptRoot
  run_root = $RunRoot
  files = @()
}
try { $identity.source_git_head = (& git -C $RepoRoot rev-parse HEAD 2>$null) } catch { }
foreach ($name in $files) {
  $source = Join-Path $PSScriptRoot $name
  if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "missing tool: $source" }
  $destination = Join-Path $RunRoot $name
  Copy-Item -LiteralPath $source -Destination $destination -Force
  $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
  $identity.files += [ordered]@{ name = $name; sha256 = $hash; source = $source; destination = $destination }
}
$identity | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $RunRoot 'run-identity.json') -Encoding UTF8
Write-Output ('run_root=' + $RunRoot)
Write-Output ('run_identity=' + (Join-Path $RunRoot 'run-identity.json'))
