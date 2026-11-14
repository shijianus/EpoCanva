# Behavioral unit fixtures, not installed-host privacy evidence.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$RunRoot, [switch]$ForceFailure)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$RunRoot=[IO.Path]::GetFullPath($RunRoot).TrimEnd('\','/')
if ((Split-Path -Parent $RunRoot) -ne (Join-Path $repo 'build')) { throw 'RunRoot must be directly below build/.' }
$root=Join-Path $RunRoot ('privacy-unit-'+[guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path $root
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $repo 'tools\product\run_offline_privacy.ps1'),[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw 'Privacy producer has parse errors.' }
foreach ($name in @('Get-PrivacyScanFiles','Find-CanaryInTree','Find-LearningStoreFile','Write-PrivacyProcessLog','Invoke-ObservedPrivacyRun')) {
    $matches=@($ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name},$true))
    if ($matches.Count -ne 1) { throw "Missing production function $name" }
    . ([scriptblock]::Create($matches[0].Extent.Text))
}
$script:checks=0
function Check([bool]$Value,[string]$Message) {
    ++$script:checks
    if (-not $Value) { throw "FAIL: $Message" }
}
$needle='UNITCanary0123456789'
$file=Join-Path $root 'large.bin'
$stream=[IO.File]::Open($file,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write)
try { $stream.SetLength(17MB); $stream.Position=16MB-5; $b=[Text.Encoding]::UTF8.GetBytes($needle); $stream.Write($b,0,$b.Length) } finally { $stream.Dispose() }
$scanned=0
Check ((Find-CanaryInTree $root $needle ([ref]$scanned)) -eq $file) 'large-file cross-block UTF8 canary detected'
$stream=[IO.File]::Open($file,[IO.FileMode]::Open,[IO.FileAccess]::Write)
try { $stream.SetLength(0); $stream.SetLength(17MB); $stream.Position=16MB-7; $b=[Text.Encoding]::Unicode.GetBytes($needle); $stream.Write($b,0,$b.Length) } finally { $stream.Dispose() }
$scanned=0
Check ((Find-CanaryInTree $root $needle ([ref]$scanned)) -eq $file) 'large-file cross-block UTF16LE canary detected'
$rejected=$false
try { $null=Find-CanaryInTree (Join-Path $root 'missing') $needle ([ref]$scanned) } catch { $rejected=$true }
Check $rejected 'absent scan root cannot pass'
$locked=[IO.File]::Open($file,[IO.FileMode]::Open,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
try {
    $rejected=$false
    try { $null=Find-CanaryInTree $root $needle ([ref]$scanned) } catch { $rejected=$true }
    Check $rejected 'unreadable file cannot be ignored'
} finally { $locked.Dispose() }
$log=Join-Path $RunRoot ('privacy-log-'+[guid]::NewGuid().ToString('N')+'.log')
Write-PrivacyProcessLog $log 'UNIT' ('prefix '+$needle) ($needle.ToUpperInvariant()+' suffix') $needle
$text=[IO.File]::ReadAllText($log)
Check ($text -notmatch [regex]::Escape($needle) -and $text.Contains('[REDACTED]')) 'canary redacted before persistence'
$store=Join-Path $root 'learning.corrupt'
[IO.File]::WriteAllBytes($store,[byte[]]@(1))
Check ((Find-LearningStoreFile $root) -eq $store) 'quarantined learning file is observed'
# Controlled observation failures: no registry/network modification. The child
# only emits an authored canary; fake table queries deliberately fail.
function Get-CimInstance { throw 'UNIT observation unavailable' }
function Get-NetTCPConnection { throw 'UNIT observation unavailable' }
function Get-NetUDPEndpoint { throw 'UNIT observation unavailable' }
$child=Join-Path $RunRoot ('privacy-child-'+[guid]::NewGuid().ToString('N')+'.ps1')
[IO.File]::WriteAllText($child,'[Console]::WriteLine($env:EPOCANVA_PRIVACY_CANARY); Start-Sleep -Milliseconds 600; [Console]::WriteLine("RESULT PASS"); exit 0')
$ps=(Get-Command powershell.exe -CommandType Application).Source
$observed=Invoke-ObservedPrivacyRun -Executable $ps -Arguments @('-NoProfile','-NonInteractive','-File',$child) -Environment @{EPOCANVA_PRIVACY_CANARY=$needle} -LogPath $log -TimeoutSeconds 10
Check ($observed.passed -and -not $observed.monitoring_available -and $observed.network_seen) 'observation failure is not a no-network pass'
Check ($observed.stdout.Contains($needle)) 'raw memory output retained for canary assertion'
Check ([IO.File]::ReadAllText($log) -notmatch [regex]::Escape($needle)) 'observed output is redacted on disk'
if ($ForceFailure) { Write-Output 'RESULT FAIL intentional negative control'; exit 14 }
Write-Output "RESULT PASS assertions=$script:checks; synthetic privacy producer regression only"
exit 0
