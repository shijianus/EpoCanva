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
    foreach ($id in @('verified-build', 'source-clean', 'corresponding-source', 'real-host', 'installer-lifecycle',
            'english-capabilities', 'language-acceptance', 'stress-reference-model', 'host-soak', 'acceptance-ci-provenance')) {
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
foreach ($name in @('Read-PassingJUnit', 'Assert-Execution', 'Test-HardAcceptanceReport')) {
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
# Parser-only synthetic reports never become release evidence. References are
# replaced here solely to isolate schema/measurement validation from file hashing.
$script:unitReferenceReads = 0
function Read-Reference($Reference) {
    if (-not $Reference) { throw 'Missing unit reference' }
    ++$script:unitReferenceReads
    return [string]$Reference
}
$bindings = @{ commit = ('a' * 40); version = 'unit-only'; source_sha256 = ('b' * 64) }
$entry = [pscustomobject]@{ started_utc='2026-01-01T00:00:00Z'; completed_utc='2026-01-01T00:30:00Z' }
function UnitReport([string]$Id) {
    return [pscustomobject]@{ format=('epocanva-' + $Id + '-v1'); result='passed';
        commit=$bindings.commit; version=$bindings.version; source_sha256=$bindings.source_sha256;
        started_utc=$entry.started_utc; completed_utc=$entry.completed_utc }
}
function RejectReport([string]$Id, $Report, [string]$Message) {
    $rejected=$false
    try { $null=Test-HardAcceptanceReport $Id $Report $entry $bindings } catch { $rejected=$true }
    Check $rejected $Message
}
$stress=UnitReport 'stress-reference-model'
$stress | Add-Member seeds @((1..5) | ForEach-Object {
    [pscustomobject]@{ seed=('0x5eed00000000000' + $_); operations=100000; mismatches=0;
        operation_counts=[pscustomobject]@{ insert=99995; delete=1; 'caret-utf16'=1; 'selection-utf16'=1; undo=1; 'invalid-state'=1 } }
})
$stress | Add-Member total_ops 500000
$stress | Add-Member oracle 'independent-editor-reference-model'
$stress | Add-Member failure_reduction 'replay-and-minimize'
$stress | Add-Member replay_log 'UNIT-REFERENCE'
$stress | Add-Member resource_samples 'UNIT-REFERENCE'
$stress | Add-Member negative_control 'UNIT-REFERENCE'
$null=Test-HardAcceptanceReport 'stress-reference-model' $stress $entry $bindings
Check ($script:unitReferenceReads -eq 3) 'stress parser reads replay, resource and negative-control references'
$stress.seeds[0].operations=99999
RejectReport 'stress-reference-model' $stress 'shortened stress seed rejected'
$stress.seeds[0].operations=100000
$stress.seeds[0].operation_counts.insert=1
RejectReport 'stress-reference-model' $stress 'inconsistent stress operation totals rejected'
$stress.seeds[0].operation_counts.insert=99995
$stress.seeds[0].operation_counts.undo=0
RejectReport 'stress-reference-model' $stress 'stress without undo rejected'
$stress.seeds[0].operation_counts.undo=1
$stress.oracle='composer-self-replay'
RejectReport 'stress-reference-model' $stress 'composer-only replay is not independent editor coverage'
$stress.oracle='independent-editor-reference-model'
$stress.commit='stale-unit-commit'
RejectReport 'stress-reference-model' $stress 'stale hard-acceptance commit rejected'
$soak=UnitReport 'host-soak'
$soak | Add-Member duration_seconds 1800
$soak | Add-Member environment 'authorized-isolated-interactive-windows'
$soak | Add-Member installed $true
$soak | Add-Member activated $true
$soak | Add-Member isolation_authorization 'UNIT-REFERENCE'
$soak | Add-Member performance_approval 'UNIT-REFERENCE'
$soak | Add-Member host_observations 'UNIT-REFERENCE'
$soak | Add-Member samples @((0..30) | ForEach-Object {
    [pscustomobject]@{ utc=([DateTimeOffset]::Parse($entry.started_utc).AddMinutes($_).ToString('o'));
        completed_interactions=($_ + 1); working_set_bytes=1000000; handles=50; cpu_seconds=[double]$_ }
})
$null=Test-HardAcceptanceReport 'host-soak' $soak $entry $bindings
Check $true 'soak parser accepts a complete synthetic thirty-minute sample sequence'
$soak.duration_seconds=1799
RejectReport 'host-soak' $soak 'soak elapsed-time mismatch rejected'
$soak.duration_seconds=[double]::NaN
RejectReport 'host-soak' $soak 'nonfinite soak duration rejected'
$soak.duration_seconds='1800'
RejectReport 'host-soak' $soak 'string soak duration rejected'
$soak.duration_seconds=1800
$soak.activated='true'
RejectReport 'host-soak' $soak 'string activation is not a measured boolean'
$soak.activated=$true
$soak.samples[2].completed_interactions=$soak.samples[1].completed_interactions
RejectReport 'host-soak' $soak 'sleep-only soak without input progress rejected'
$soak.samples[2].completed_interactions=3
$soak.samples[2].cpu_seconds=[double]::NaN
RejectReport 'host-soak' $soak 'nonfinite soak resource measurement rejected'
$soak.samples[2].cpu_seconds=2.0
$soak.activated=$false
RejectReport 'host-soak' $soak 'unactivated host soak rejected'
$ci=UnitReport 'acceptance-ci-provenance'
$ci | Add-Member github_run (Join-Path $fixtureRoot 'UNIT-github-run.json')
$ci | Add-Member github_jobs (Join-Path $fixtureRoot 'UNIT-github-jobs.json')
$ci | Add-Member run_id 1
$ci | Add-Member artifact_manifest 'UNIT-REFERENCE'
$ci | Add-Member runner_authorization 'UNIT-REFERENCE'
$run=[pscustomobject]@{ id=1; head_sha=$bindings.commit; repository=@{full_name='shijianus/EpoCanva'};
    event='workflow_dispatch'; status='completed'; conclusion='success' }
$jobs=[pscustomobject]@{ total_count=6; jobs=@('contract-gate','root-unit-tests','backend','windows-product',
    'windows-integration-acceptance','full-quality-stress-acceptance' | ForEach-Object {
        [pscustomobject]@{ name=$_; head_sha=$bindings.commit; status='completed'; conclusion='success'; runner_name='UNIT-RUNNER' }
    }) }
Write-Text $ci.github_run ($run | ConvertTo-Json -Depth 6)
Write-Text $ci.github_jobs ($jobs | ConvertTo-Json -Depth 6)
$null=Test-HardAcceptanceReport 'acceptance-ci-provenance' $ci $entry $bindings
Check $true 'CI parser accepts complete synthetic exact-commit job snapshots'
$run.head_sha='stale'
Write-Text $ci.github_run ($run | ConvertTo-Json -Depth 6)
RejectReport 'acceptance-ci-provenance' $ci 'historical successful CI commit rejected'
$run.head_sha=$bindings.commit
Write-Text $ci.github_run ($run | ConvertTo-Json -Depth 6)
$jobs.jobs[5].conclusion='skipped'
Write-Text $ci.github_jobs ($jobs | ConvertTo-Json -Depth 6)
RejectReport 'acceptance-ci-provenance' $ci 'skipped full acceptance job rejected'
$jobs.jobs=$jobs.jobs[0..1]
$jobs.total_count=2
Write-Text $ci.github_jobs ($jobs | ConvertTo-Json -Depth 6)
RejectReport 'acceptance-ci-provenance' $ci 'contract-only CI cannot establish product acceptance'
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
