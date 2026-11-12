# Negative and positive unit tests for the candidate-quality gate.
#
# These exercise the production validation function extracted from
# tools/product/check_release.ps1. Fixtures are synthetic objects under
# build/; they never authorize a release and never claim an evaluator ran.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$RunRoot, [switch]$ForceFailure)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$RunRoot = [IO.Path]::GetFullPath($RunRoot).TrimEnd('\', '/')
if ((Split-Path -Parent $RunRoot) -ne (Join-Path $repo 'build') -or
        (Split-Path -Leaf $RunRoot) -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$') { throw 'RunRoot must be <repo>/build/<preset>.' }
$fixtureRoot = Join-Path $RunRoot 'candidate-quality-fixtures'
$null = New-Item -ItemType Directory -Force -Path $fixtureRoot
$encoding = New-Object Text.UTF8Encoding($false)
$script:failures = 0
$script:assertions = 0
function Check([bool]$Condition, [string]$Message) {
    ++$script:assertions
    if (-not $Condition) { ++$script:failures; Write-Host "FAIL: $Message" }
}
function Expect-Reject([scriptblock]$Action, [string]$Message) {
    $rejected = $false
    try { $null = & $Action } catch { $rejected = $true }
    Check $rejected $Message
}
function Expect-Accept([scriptblock]$Action, [string]$Message) {
    $accepted = $false
    try { $null = & $Action; $accepted = $true } catch { Write-Host "unexpected: $($_.Exception.Message)" }
    Check $accepted $Message
}

$checker = Join-Path $repo 'tools\product\check_release.ps1'
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($checker, [ref]$tokens, [ref]$errors)
Check ($errors.Count -eq 0) 'production gate parses under the installed PowerShell'
foreach ($name in @('Get-CqProperty', 'Get-CqNumber', 'Get-CqRate', 'Test-CandidateQualityPolicy',
        'Test-CandidateQualityEvidence', 'Read-PassingJUnit', 'Assert-Execution')) {
    $functionAst = $ast.FindAll({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }, $true)
    if ($functionAst.Count -ne 1) { throw "Missing production validator $name" }
    . ([scriptblock]::Create($functionAst[0].Extent.Text))
}

$languages = @('en', 'zh', 'fr', 'ja', 'ko')
function New-Policy([string]$AbsoluteDecision = 'approved') {
    $minimums = [ordered]@{ en = 2; zh = 2; fr = 2; ja = 2; ko = 2 }
    $qualification = [ordered]@{ runs = 2; repeat = 2; warmup_queries = 20; seed = 20261107; bootstrap_replicates = 10000 }
    $absolute = [ordered]@{ decision = $AbsoluteDecision; top1_min = 0.5; top3_min = 0.7; mrr_min = 0.6 }
    if ($AbsoluteDecision -eq 'pending_product_owner') {
        $absolute['top1_min'] = $null; $absolute['top3_min'] = $null; $absolute['mrr_min'] = $null
    }
    return [pscustomobject]@{
        format = 'epocanva-candidate-quality-policy-v2'
        version = 2
        languages = $languages
        qualification = [pscustomobject]$qualification
        minimum_cases = [pscustomobject]$minimums
        thresholds = [pscustomobject]@{
            deterministic_case_rate_min = 1.0; wrong_language_rate_max = 0.0
            empty_menu_rate_max = 0.0; raw_present_rate_min = 1.0; raw_first_rate_min = 1.0
            not_observable_is_failure = $true
        }
        absolute_quality = [pscustomobject]$absolute
        tier_b_lock = 'tier-b.lock.json'
    }
}
function New-Bindings {
    $values = [ordered]@{}
    foreach ($name in @('version', 'commit', 'source_sha256', 'package_manifest_sha256',
            'package_policy_sha256', 'language_policy_sha256', 'acceptance_profile',
            'quality_policy_sha256', 'corpus_manifest_sha256', 'source_archive_sha256',
            'evaluator_sha256', 'evaluator_support_sha256', 'runner_sha256',
            'harness_sha256', 'backend_manifest_sha256', 'tier_b_lock_sha256')) {
        $values[$name] = if ($name -eq 'version') { '0.0.0-unit' }
            elseif ($name -eq 'acceptance_profile') { 'internal-unsigned' }
            else { ('a' * 64) }
    }
    $values['commit'] = ('b' * 40)
    return $values
}
function New-Report($Bindings, $Policy) {
    $predicates = [ordered]@{}
    foreach ($name in @('minimum_samples', 'exact_process_and_repeat_protocol',
            'all_expected_cases_executed', 'no_extra_cases', 'validation_failures_empty',
            'cross_process_determinism', 'in_process_repeat_determinism', 'empty_menu_rate_zero',
            'raw_contract', 'candidate_quality_metrics_recorded', 'latency_memory_recorded',
            'privacy_and_abi_status', 'wrong_language_observable', 'wrong_language_rate',
            'tier_b_corpus', 'performance_regression', 'absolute_quality_thresholds')) {
        $predicates[$name] = [pscustomobject]@{ status = 'passed' }
    }
    $perLanguage = [ordered]@{}
    foreach ($language in $languages) {
        $perLanguage[$language] = [pscustomobject]@{
            cases = 2; measured_cases = 2; minimum_cases_met = $true
            deterministic_case_rate = 1.0
            in_process_repeat_deterministic_rate = 1.0
            cross_process_deterministic_rate = 1.0
            raw_present_rate = 1.0; raw_first_rate = 1.0
            empty_menu_rate = 0.0; wrong_language_rate = 0.0
            wrong_language_observability = [pscustomobject]@{ status = 'observable' }
            latency_ms = [pscustomobject]@{ cold = 1.0; warm = [pscustomobject]@{ p50 = 1.0; p95 = 2.0; p99 = 3.0 } }
            peak_working_set_bytes = 1000; resource_load_ms = 1.0
        }
    }
    return [pscustomobject]@{
        format = 'epocanva-candidate-eval-v3'
        identity = [pscustomobject]@{
            mode = 'qualification'; repo_commit = $Bindings['commit']; source_tree_clean = $true
            quality_policy_sha256 = $Bindings['quality_policy_sha256']
            manifest_sha256 = $Bindings['corpus_manifest_sha256']
            backend_manifest_sha256 = $Bindings['backend_manifest_sha256']
            harness_sha256 = $Bindings['harness_sha256']
            package_manifest_sha256 = $Bindings['package_manifest_sha256']
            package_policy_sha256 = $Bindings['package_policy_sha256']
            language_policy_sha256 = $Bindings['language_policy_sha256']
            source_archive_sha256 = $Bindings['source_archive_sha256']
            evaluator_sha256 = $Bindings['evaluator_sha256']
            evaluator_support_sha256 = $Bindings['evaluator_support_sha256']
            acceptance_profile = $Bindings['acceptance_profile']
        }
        corpus = [pscustomobject]@{
            id = 'unit'; version = 1; license = 'AGPL-3.0-only'
            evidence = [pscustomobject]@{ status = 'passed'; tier = 'B'; lock_sha256 = $Bindings['tier_b_lock_sha256'] }
        }
        quality_gate = [pscustomobject]@{
            status = 'passed'; met = $true; policy_version = $Policy.format
            minimum_cases = $Policy.minimum_cases
            unmet_reasons = @(); not_evaluated_predicates = @()
            predicates = [pscustomobject]$predicates
        }
        per_language = [pscustomobject]$perLanguage
        failures = @()
        unverified = [pscustomobject]@{}
        determinism = [pscustomobject]@{ repeats_per_case = 2; fresh_runs = 2 }
    }
}
function New-Execution([datetimeoffset]$Started, [int]$ExitCode = 0) {
    return [pscustomobject]@{
        exit_code = $ExitCode
        started_utc = $Started.ToString('o')
        completed_utc = $Started.AddMinutes(1).ToString('o')
        command = @('unit-runner')
    }
}
function New-JUnit($Bindings, [string[]]$CaseNames) {
    $rows = @('<testsuite name="candidate-quality" tests="' + $CaseNames.Count + '" failures="0" errors="0" skipped="0" disabled="0">')
    $rows += '<properties>'
    foreach ($key in $Bindings.Keys) {
        $rows += ('<property name="epocanva.' + $key + '" value="' + [Security.SecurityElement]::Escape([string]$Bindings[$key]) + '"/>')
    }
    $rows += '</properties>'
    foreach ($name in $CaseNames) {
        $rows += ('<testcase name="' + $name + '"/>')
    }
    $rows += '</testsuite>'
    $path = Join-Path $fixtureRoot 'candidate-quality.junit.xml'
    [IO.File]::WriteAllText($path, ('<?xml version="1.0" encoding="utf-8"?>' + "`n" + ($rows -join "`n") + "`n"), $encoding)
    return (Read-PassingJUnit $path)
}

$now = [DateTimeOffset]::UtcNow
$commitTime = $now.AddMinutes(-10)
$started = $now.AddMinutes(-5)
$policy = New-Policy
$bindings = New-Bindings
$report = New-Report $bindings $policy
$requiredCases = @('candidate-quality.qualification', 'candidate-quality.coverage',
    'candidate-quality.determinism', 'candidate-quality.contracts',
    'candidate-quality.identity-binding')
$cases = New-JUnit $bindings $requiredCases
$execution = New-Execution $started

Expect-Accept { Test-CandidateQualityEvidence -Report $report -JUnitCases $cases -Execution $execution -Bindings $bindings -Policy $policy -CommitTimeUtc $commitTime } 'valid evidence passes'

Expect-Reject { Test-CandidateQualityEvidence -Report $null -JUnitCases $cases -Execution $execution -Bindings $bindings -Policy $policy -CommitTimeUtc $commitTime } 'missing report rejected'
$badExit = New-Execution $started 1
Expect-Reject { Test-CandidateQualityEvidence -Report $report -JUnitCases $cases -Execution $badExit -Bindings $bindings -Policy $policy -CommitTimeUtc $commitTime } 'nonzero evaluator exit rejected even with a passing report'
$badCommit = New-Report $bindings $policy
$badCommit.identity.repo_commit = ('c' * 40)
Expect-Reject { Test-CandidateQualityEvidence -Report $badCommit -JUnitCases $cases -Execution $execution -Bindings $bindings -Policy $policy -CommitTimeUtc $commitTime } 'commit mismatch rejected'
$badManifestReport = New-Report $bindings $policy
$badManifestReport.identity.manifest_sha256 = ('d' * 64)
Expect-Reject { Test-CandidateQualityEvidence -Report $badManifestReport -JUnitCases $cases -Execution $execution -Bindings $bindings -Policy $policy -CommitTimeUtc $commitTime } 'corpus manifest hash mismatch rejected'
$badPolicyReport = New-Report $bindings $policy
$badPolicyReport.identity.quality_policy_sha256 = ('e' * 64)
Expect-Reject { Test-CandidateQualityEvidence -Report $badPolicyReport -JUnitCases $cases -Execution $execution -Bindings $bindings -Policy $policy -CommitTimeUtc $commitTime } 'quality policy hash mismatch rejected'
$short = New-Report $bindings $policy
$short.per_language.en.cases = 1
$short.per_language.en.minimum_cases_met = $false
Expect-Reject { Test-CandidateQualityEvidence -Report $short -JUnitCases $cases -Execution $execution -Bindings $bindings -Policy $policy -CommitTimeUtc $commitTime } 'sample shortfall rejected'
$nondet = New-Report $bindings $policy
$nondet.per_language.fr.deterministic_case_rate = 0.999
Expect-Reject { Test-CandidateQualityEvidence -Report $nondet -JUnitCases $cases -Execution $execution -Bindings $bindings -Policy $policy -CommitTimeUtc $commitTime } 'sub-unit determinism accepted (must reject)'
$missingCase = New-JUnit $bindings @('candidate-quality.qualification', 'candidate-quality.coverage',
    'candidate-quality.determinism', 'candidate-quality.contracts')
Expect-Reject { Test-CandidateQualityEvidence -Report $report -JUnitCases $missingCase -Execution $execution -Bindings $bindings -Policy $policy -CommitTimeUtc $commitTime } 'missing mandatory JUnit case rejected'
$duplicateCase = New-JUnit $bindings @('candidate-quality.qualification', 'candidate-quality.qualification',
    'candidate-quality.coverage', 'candidate-quality.determinism', 'candidate-quality.contracts', 'candidate-quality.identity-binding')
Expect-Reject { Test-CandidateQualityEvidence -Report $report -JUnitCases $duplicateCase -Execution $execution -Bindings $bindings -Policy $policy -CommitTimeUtc $commitTime } 'duplicate JUnit case rejected'
$changedRunner = New-Bindings
$changedRunner['runner_sha256'] = ('f' * 64)
Expect-Reject { Test-CandidateQualityEvidence -Report $report -JUnitCases $cases -Execution $execution -Bindings $changedRunner -Policy $policy -CommitTimeUtc $commitTime } 'runner hash change rejected'
$early = New-Execution ($commitTime.AddHours(-1))
Expect-Reject { Test-CandidateQualityEvidence -Report $report -JUnitCases $cases -Execution $early -Bindings $bindings -Policy $policy -CommitTimeUtc $commitTime } 'report generated before the candidate commit rejected'
$changedInput = New-Bindings
$changedInput['quality_policy_sha256'] = ('1' * 64)
Expect-Reject { Test-CandidateQualityEvidence -Report $report -JUnitCases $cases -Execution $execution -Bindings $changedInput -Policy $policy -CommitTimeUtc $commitTime } 'input changed after the run rejected'
# Every release artifact the report claims to bind must match the current
# bytes. Mutating the report identity is the "artifact changed after the run"
# case; mutating the bindings is the "JUnit envelope points elsewhere" case.
foreach ($name in @('package_manifest_sha256', 'package_policy_sha256', 'language_policy_sha256',
        'source_archive_sha256', 'evaluator_sha256', 'evaluator_support_sha256')) {
    $tamperedReport = New-Report $bindings $policy
    $tamperedReport.identity.PSObject.Properties[$name].Value = ('9' * 64)
    Expect-Reject { Test-CandidateQualityEvidence -Report $tamperedReport -JUnitCases $cases -Execution $execution -Bindings $bindings -Policy $policy -CommitTimeUtc $commitTime } "report artifact binding tamper rejected: $name"
    $tamperedBindings = New-Bindings
    $tamperedBindings[$name] = ('9' * 64)
    Expect-Reject { Test-CandidateQualityEvidence -Report $report -JUnitCases $cases -Execution $execution -Bindings $tamperedBindings -Policy $policy -CommitTimeUtc $commitTime } "JUnit artifact binding mismatch rejected: $name"
}
$tamperedProfile = New-Report $bindings $policy
$tamperedProfile.identity.acceptance_profile = 'public-signed'
Expect-Reject { Test-CandidateQualityEvidence -Report $tamperedProfile -JUnitCases $cases -Execution $execution -Bindings $bindings -Policy $policy -CommitTimeUtc $commitTime } 'report acceptance-profile tamper rejected'
$notObservable = New-Report $bindings $policy
$notObservable.per_language.ko.wrong_language_observability.status = 'not_observable'
Expect-Reject { Test-CandidateQualityEvidence -Report $notObservable -JUnitCases $cases -Execution $execution -Bindings $bindings -Policy $policy -CommitTimeUtc $commitTime } 'unobservable language attribution rejected'
$partial = New-Report $bindings $policy
$partial.unverified = [pscustomobject]@{ zhuyin = [pscustomobject]@{ cases = 1 } }
Expect-Reject { Test-CandidateQualityEvidence -Report $partial -JUnitCases $cases -Execution $execution -Bindings $bindings -Policy $policy -CommitTimeUtc $commitTime } 'partial-pass unverified section rejected'
$failedPredicate = New-Report $bindings $policy
$failedPredicate.quality_gate.predicates.raw_contract.status = 'failed'
Expect-Reject { Test-CandidateQualityEvidence -Report $failedPredicate -JUnitCases $cases -Execution $execution -Bindings $bindings -Policy $policy -CommitTimeUtc $commitTime } 'failed predicate rejected'
$tierA = New-Report $bindings $policy
$tierA.corpus.evidence.status = 'not_evaluable'
Expect-Reject { Test-CandidateQualityEvidence -Report $tierA -JUnitCases $cases -Execution $execution -Bindings $bindings -Policy $policy -CommitTimeUtc $commitTime } 'unapproved corpus rejected'
$pending = New-Policy 'pending_product_owner'
Expect-Reject { Test-CandidateQualityPolicy $pending } 'pending absolute-quality decision rejected'
$missingSeed = New-Policy
$missingSeed.qualification.PSObject.Properties.Remove('seed')
Expect-Reject { Test-CandidateQualityPolicy $missingSeed } 'unfrozen seed policy rejected'
$skipped = Join-Path $fixtureRoot 'skipped.xml'
[IO.File]::WriteAllText($skipped, '<testsuite tests="1" skipped="1"><testcase name="candidate-quality.qualification"><skipped/></testcase></testsuite>', $encoding)
Expect-Reject { Read-PassingJUnit $skipped } 'skipped JUnit rejected'

if ($ForceFailure) { Check $false 'intentional can-fail control' }
if ($script:failures) { Write-Output "RESULT FAIL assertions=$script:assertions failures=$script:failures"; exit 14 }
Write-Output "RESULT PASS assertions=$script:assertions; synthetic fixtures never substitute for a real evaluator run"
exit 0
