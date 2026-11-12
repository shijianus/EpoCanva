# Unit regression of the real gate. Fixture payloads are never build/owner/host evidence.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$RunRoot, [switch]$ForceFailure)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$RunRoot = [IO.Path]::GetFullPath($RunRoot).TrimEnd('\', '/')
if ((Split-Path -Parent $RunRoot) -ne (Join-Path $repo 'build') -or
        (Split-Path -Leaf $RunRoot) -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$') { throw 'RunRoot must be <repo>/build/<preset>.' }
$fixtureRoot = Join-Path $RunRoot 'gate-unit-fixtures'
$stage = Join-Path $fixtureRoot 'EpoCanva-unit-only'
$null = New-Item -ItemType Directory -Force -Path $stage
$encoding = New-Object Text.UTF8Encoding($false)
$script:failures = 0
$script:assertions = 0
$records = [Collections.Generic.List[object]]::new()
function Check([bool]$Condition, [string]$Message) {
    ++$script:assertions
    if (-not $Condition) { ++$script:failures; Write-Host "FAIL: $Message" }
}
function Write-Text([string]$Path, [string]$Text) {
    $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path)
    [IO.File]::WriteAllText($Path, $Text, $encoding)
}
Write-Text (Join-Path $fixtureRoot 'UNIT-FIXTURES.txt') 'UNIT TESTS ONLY. Placeholder binaries. No build receipt, owner approval or host acceptance is supplied.'
$policy = @()
foreach ($line in Get-Content -LiteralPath (Join-Path $repo 'data\manifests\product-package-files.inc')) {
    if ($line -cmatch '^EPOCANVA_PACKAGE_FILE\("([^"]+)", "([^"]*)", "([^"]*)", (\d+)\)$') {
        $policy += [pscustomobject]@{ path=$Matches[1]; source=$Matches[2] }
    }
}
Check ($policy.Count -gt 0) 'shared policy has entries'
foreach ($entry in $policy) {
    $path = Join-Path $stage $entry.path
    $null = New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path)
    if ($entry.source) { Copy-Item -LiteralPath (Join-Path $repo $entry.source) -Destination $path -Force }
    else { Write-Text $path ('UNIT TEST PLACEHOLDER ' + $entry.path + "`n") }
}
$version = (Get-Content -LiteralPath (Join-Path $repo 'platforms\windows\product\VERSION') -Raw).Trim()
function Manifest([int]$Format = 2, [string]$Exception = 'none', [string]$Omit = '', [string]$ExtraMetadata = '') {
    $rows = @("epocanva-package-manifest $Format", "version=$version", 'commit=unit-test-not-a-product-build')
    if ($Format -eq 1) { $rows += 'internal-only=0' }
    else { $rows += @('acceptance-profile=internal-unsigned', 'distribution-policy=internal-only', "path-exception=$Exception") }
    if ($ExtraMetadata) { $rows += $ExtraMetadata }
    foreach ($entry in $policy) {
        if ($entry.path -ceq $Omit) { continue }
        $file = Get-Item -LiteralPath (Join-Path $stage $entry.path)
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $rows += "file`t$($entry.path)`t$hash`t$($file.Length)"
    }
    Write-Text (Join-Path $stage 'package.manifest') (($rows -join "`n") + "`n")
}
$checker = Join-Path $repo 'tools\product\check_release.ps1'
$powershell = (Get-Command powershell.exe -CommandType Application).Source
function Gate([string]$Case, [string]$Profile = 'internal-unsigned', [string]$Package = $stage) {
    $arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $checker,
        '-RepoRoot', $repo, '-BuildDir', $RunRoot, '-StageRoot', $Package, '-AcceptanceProfile', $Profile,
        '-EvidencePath', (Join-Path $fixtureRoot 'absent-release-evidence.json'))
    $log = Join-Path $fixtureRoot "$Case.log"
    $ErrorActionPreference = 'Continue'
    & $powershell @arguments 2>&1 | ForEach-Object { $_.ToString() } | Out-File -LiteralPath $log -Encoding UTF8
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $records.Add([pscustomobject]@{ case=$Case; command=(@($powershell)+$arguments); exit_code=$code })
    Check ($code -eq 1) "$Case cannot pass aggregate acceptance without real evidence"
    $report = Get-Content -LiteralPath (Join-Path $RunRoot 'release-readiness.json') -Raw | ConvertFrom-Json
    Copy-Item -LiteralPath (Join-Path $RunRoot 'release-readiness.json') -Destination (Join-Path $fixtureRoot "$Case.json") -Force
    Check (-not $report.technical_ready -and -not $report.acceptance_ready -and -not $report.distribution_authorized -and
           -not $report.signature_verified -and -not $report.release_ready) "$Case makes no readiness/authorization/signature claim"
    foreach ($id in @('verified-build', 'source-clean', 'corresponding-source', 'real-host', 'installer-lifecycle', 'english-capabilities', 'language-acceptance')) {
        $gate = @($report.checks | Where-Object { $_.id -ceq $id })
        Check ($gate.Count -eq 1 -and $gate[0].status -ceq 'blocked') "$Case retains mandatory blocker $id"
    }
    return $report
}
function Status($Report, [string]$Id) { return @($Report.checks | Where-Object { $_.id -ceq $Id })[0].status }
$report = Gate 'missing-package' 'internal-unsigned' (Join-Path $fixtureRoot 'absent-package')
Check ((Status $report 'package-integrity') -ceq 'blocked') 'missing package blocks integrity'
Manifest
$report = Gate 'strict-internal'
foreach ($id in @('package-policy', 'package-integrity', 'acceptance-profile', 'project-license-selection', 'package-path-hygiene')) {
    Check ((Status $report $id) -ceq 'passed') "strict internal positive unit payload passes $id"
}
Check ((Status $report 'language-readiness') -ceq 'passed') 'complete five-language fixture payload satisfies the readiness policy'
Check (-not $report.acceptance_ready) 'readiness policy alone never authorizes acceptance without executed evidence'
Check ((Status $report 'signing') -ceq 'not_required' -and $report.signing_status -ceq 'not_required') 'unsigned acceptance omits signing without claiming verification'
$report = Gate 'public-mismatch' 'public-signed'
Check ((Status $report 'acceptance-profile') -ceq 'blocked' -and (Status $report 'signing') -ceq 'blocked') 'public profile cannot reuse unsigned metadata or omit signatures'
Manifest 1
$report = Gate 'legacy-diagnostic-only'
Check ((Status $report 'package-integrity') -ceq 'passed' -and (Status $report 'acceptance-profile') -ceq 'blocked') 'legacy manifest retains integrity diagnosis only'
Manifest 2 'embedded-build-paths'
$report = Gate 'path-exception'
Check ((Status $report 'package-integrity') -ceq 'passed' -and (Status $report 'acceptance-profile') -ceq 'blocked') 'path exception is not unsigned acceptance'
Manifest 2 'none' '' 'acceptance-profile=internal-unsigned'
$report = Gate 'duplicate-policy'
Check ((Status $report 'package-integrity') -ceq 'blocked') 'duplicate profile rejected'
Remove-Item -LiteralPath (Join-Path $stage 'LICENSE')
Manifest 2 'none' 'LICENSE'
$report = Gate 'license-file-and-row-omitted'
Check ((Status $report 'package-integrity') -ceq 'blocked') 'omitting license file and row cannot hide omission'
Write-Text (Join-Path $stage 'LICENSE') 'UNIT TAMPER: intentionally not the GNU license'
Manifest
$report = Gate 'license-rehashed-tamper'
Check ((Status $report 'package-integrity') -ceq 'blocked') 'rehashing changed license does not bless fixed-content tamper'
Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination (Join-Path $stage 'LICENSE') -Force
Write-Text (Join-Path $stage 'unlisted.txt') 'UNIT EXTRA'
Manifest
$report = Gate 'extra-payload'
Check ((Status $report 'package-integrity') -ceq 'blocked') 'extra payload rejected'
Remove-Item -LiteralPath (Join-Path $stage 'unlisted.txt')
Manifest
# Test actual evidence parser functions without creating owner/host records or a receipt.
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($checker,[ref]$tokens,[ref]$errors)
Check ($errors.Count -eq 0) 'gate parses under the installed PowerShell'
foreach ($name in @('Read-PassingJUnit', 'Assert-Execution')) {
    $functionAst=$ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name },$true)
    if ($functionAst.Count -ne 1) { throw "Missing production parser function $name" }
    . ([scriptblock]::Create($functionAst[0].Extent.Text))
}
$valid=[pscustomobject]@{ exit_code=0; started_utc='2026-01-01T00:00:00Z'; completed_utc='2026-01-01T00:00:01Z'; command=@('unit-parser-only') }
Assert-Execution $valid
Check $true 'actual parser accepts typed integer exit code and ordered timestamps'
foreach ($value in @($true, '0', 7)) {
    $valid.exit_code=$value
    $rejected=$false
    try { Assert-Execution $valid } catch { $rejected=$true }
    Check $rejected 'boolean/string/nonzero exit code rejected'
}
$xml=Join-Path $fixtureRoot 'unit-parser-only.xml'
foreach ($text in @('<testsuite tests="1"><testcase name="unit.parser"><skipped/></testcase></testsuite>',
        '<testsuite tests="2"><testcase name="unit.parser"/></testsuite>',
        '<!DOCTYPE testsuite [<!ENTITY bad SYSTEM "file:///invalid">]><testsuite tests="1"><testcase name="unit.parser">&bad;</testcase></testsuite>')) {
    Write-Text $xml $text
    $rejected=$false
    try { $null=Read-PassingJUnit $xml } catch { $rejected=$true }
    Check $rejected 'skipped/count-mismatch/DTD JUnit rejected'
}
$badOutput=Join-Path $RunRoot 'invalid-nested-build'
$arguments=@('-NoProfile','-NonInteractive','-File',$checker,'-RepoRoot',$repo,'-BuildDir',$badOutput)
$ErrorActionPreference='Continue'
& $powershell @arguments 2>&1 | ForEach-Object { $_.ToString() } | Out-File -LiteralPath (Join-Path $fixtureRoot 'invalid-boundary.log') -Encoding UTF8
$code=$LASTEXITCODE
$ErrorActionPreference='Stop'
$records.Add([pscustomobject]@{case='invalid-boundary';command=(@($powershell)+$arguments);exit_code=$code})
Check ($code -eq 2 -and -not (Test-Path -LiteralPath (Join-Path $badOutput 'release-readiness.json'))) 'invalid output boundary exits 2 without report'
$records.ToArray() | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $fixtureRoot 'commands.json') -Encoding UTF8
if ($ForceFailure) { Check $false 'intentional can-fail control' }
if ($script:failures) { Write-Output "RESULT FAIL assertions=$script:assertions failures=$script:failures"; exit 14 }
Write-Output "RESULT PASS assertions=$script:assertions; fixture cases never authorize product acceptance"
exit 0
