function Get-CqProperty($Object, [string]$Name) {
    if ($Object -isnot [pscustomobject]) { throw "candidate-quality input is missing object field $Name." }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $property.Name -cne $Name) { throw "candidate-quality input lacks $Name." }
    return ,$property.Value
}
function Get-CqInteger($Object, [string]$Name) {
    $value = Get-CqProperty $Object $Name
    if (-not (Test-ProductJsonInteger $value) -or $value -lt 1) {
        throw "candidate-quality field $Name is not a positive integer."
    }
    return $value
}
function Get-CqNumber($Object, [string]$Name) {
    $value = Get-CqProperty $Object $Name
    if ($value -is [bool] -or
            ($value -isnot [int] -and $value -isnot [long] -and $value -isnot [double] -and $value -isnot [decimal])) {
        throw "candidate-quality field $Name is not numeric."
    }
    $number = [double]$value
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) { throw "candidate-quality field $Name is not finite." }
    return $number
}
function Get-CqRate($Object, [string]$Name, [double]$Minimum, [double]$Maximum) {
    $value = Get-CqNumber $Object $Name
    if ($value -lt $Minimum -or $value -gt $Maximum) { throw "candidate-quality field $Name is outside its policy range." }
    return $value
}
function Test-CandidateQualityPolicy($Policy) {
    # This is a shape check of the one machine policy. It never supplies a
    # second threshold table: every comparison below reads these values.
    if (-not [object]::Equals((Get-CqProperty $Policy 'format'), 'epocanva-candidate-quality-policy-v3')) {
        throw 'candidate-quality policy schema is unsupported.'
    }
    if ((Get-CqProperty $Policy 'version') -isnot [int] -or (Get-CqProperty $Policy 'version') -ne 3) {
        throw 'candidate-quality policy version is unsupported.'
    }
    $languages = @('en', 'zh', 'fr', 'ja', 'ko')
    $declared = Get-CqProperty $Policy 'languages'
    if ($declared -isnot [array] -or $declared.Count -ne $languages.Count -or
            ($declared -join ',') -cne ($languages -join ',')) {
        throw 'candidate-quality policy must name exactly en,zh,fr,ja,ko in order.'
    }
    $minimums = Get-CqProperty $Policy 'minimum_cases'
    foreach ($language in $languages) {
        $null = Get-CqInteger $minimums $language
    }
    $qualification = Get-CqProperty $Policy 'qualification'
    foreach ($field in @('runs', 'repeat', 'warmup_queries', 'seed', 'bootstrap_replicates')) {
        $null = Get-CqInteger $qualification $field
    }
    $thresholds = Get-CqProperty $Policy 'thresholds'
    foreach ($field in @('deterministic_case_rate_min', 'raw_present_rate_min', 'raw_first_rate_min')) {
        $null = Get-CqRate $thresholds $field 0 1
    }
    foreach ($field in @('wrong_language_rate_max', 'empty_menu_rate_max')) {
        $null = Get-CqRate $thresholds $field 0 1
    }
    if (-not [object]::Equals((Get-CqProperty $thresholds 'not_observable_is_failure'), $true)) {
        throw 'candidate-quality policy must fail closed when a metric is not observable.'
    }
    $absolute = Get-CqProperty $Policy 'absolute_quality'
    if (-not [object]::Equals((Get-CqProperty $absolute 'decision'), 'approved')) {
        throw 'candidate-quality absolute quality thresholds are not approved by the product owner.'
    }
    foreach ($field in @('top1_min', 'top3_min', 'mrr_min')) {
        $null = Get-CqRate $absolute $field 0 1
    }
    return $languages
}
function Test-CandidateTierBApproval($Lock, $Policy, $Manifest, [string]$ManifestSha256) {
    # A report's claim and matching lock hash cannot approve an unapproved lock.
    # Recompute approval from the CURRENT lock and CURRENT corpus manifest.
    if (-not [object]::Equals((Get-CqProperty $Lock 'format'), 'epocanva-candidate-tier-b-lock-v1') -or
            (Get-CqProperty $Lock 'version') -isnot [int] -or
            (Get-CqProperty $Lock 'version') -ne 1 -or
            -not [object]::Equals((Get-CqProperty $Lock 'status'), 'approved') -or
            -not [object]::Equals((Get-CqProperty $Manifest 'tier'), 'B')) {
        throw 'candidate-quality requires a currently approved Tier B lock and explicit Tier B manifest.'
    }
    Assert-ProductObjectFields $Lock @('format', 'version', 'status', 'languages')
    $languages = Get-CqProperty $Policy 'languages'
    $entries = Get-CqProperty $Lock 'languages'
    Assert-ProductObjectFields $entries $languages
    foreach ($language in $languages) {
        $row = Get-CqProperty $entries $language
        Assert-ProductObjectFields $row @('path', 'sha256', 'manifest_sha256', 'case_count', 'source', 'license',
            'license_conclusion', 'approval_reference', 'no_user_content', 'reproduction')
        foreach ($field in @('source', 'license', 'approval_reference')) {
            $value = Get-CqProperty $row $field
            if ($value -isnot [string] -or [string]::IsNullOrWhiteSpace($value)) {
                throw "Tier B $language lacks an attributable $field."
            }
        }
        $path = Get-CqProperty $row 'path'
        if ($path -isnot [string] -or $path -cne ('data/eval/candidate-v1/cases/' + $language + '.jsonl') -or
                -not [object]::Equals((Get-CqProperty $row 'license_conclusion'), 'approved_for_evaluation') -or
                -not [object]::Equals((Get-CqProperty $row 'no_user_content'), $true)) {
            throw "Tier B $language path, licence or privacy approval is invalid."
        }
        $count = Get-CqInteger $row 'case_count'
        if ($count -ne (Get-CqInteger (Get-CqProperty $Manifest 'case_count') $language)) {
            throw "Tier B $language case count differs from the current manifest."
        }
        $digest = Get-CqProperty $row 'sha256'
        if ($digest -isnot [string] -or $digest -cnotmatch '^[0-9a-f]{64}$' -or
                -not [object]::Equals($digest, (Get-CqProperty (Get-CqProperty $Manifest 'corpus_sha256') ($language + '.jsonl'))) -or
                -not [object]::Equals((Get-CqProperty $row 'manifest_sha256'), $ManifestSha256)) {
            throw "Tier B $language corpus/manifest binding differs from the current inputs."
        }
        $reproduction = Get-CqProperty $row 'reproduction'
        Assert-ProductObjectFields $reproduction @('kind', 'reference')
        $reference = Get-CqProperty $reproduction 'reference'
        if ((Get-CqProperty $reproduction 'kind') -cnotin @('approved_fixed_input', 'reproducible_generation') -or
                $reference -isnot [string] -or [string]::IsNullOrWhiteSpace($reference)) {
            throw "Tier B $language reproduction record is absent."
        }
    }
}
function Assert-CqRegression($Regression, $Policy, [string]$Language, $Current) {
    if (-not [object]::Equals((Get-CqProperty $Regression 'status'), 'passed')) {
        throw 'candidate-quality requires a comparable passing performance baseline.'
    }
    $measurements = Get-CqProperty (Get-CqProperty $Regression 'per_language') $Language
    $bounds = Get-CqProperty $Policy 'regression'
    foreach ($metric in $Current.Keys) {
        $row = Get-CqProperty $measurements $metric
        $baseline = Get-CqNumber $row 'baseline'
        $measured = Get-CqNumber $row 'current'
        $limit = Get-CqProperty $bounds $metric
        $relative = Get-CqNumber $limit 'relative_increment_max'
        $absolute = Get-CqNumber $limit 'absolute_increment_max'
        $increment = [Math]::Max($baseline * $relative, $absolute)
        if ($baseline -lt 0 -or $relative -lt 0 -or $absolute -lt 0 -or [double]::IsInfinity($increment) -or
                $measured -ne $Current[$metric] -or $measured -gt $baseline + $increment -or
                (Get-CqNumber $row 'allowed_increment') -ne $increment -or
                (Get-CqNumber $row 'delta') -ne ($measured - $baseline) -or
                -not [object]::Equals((Get-CqProperty $row 'passed'), $true)) {
            throw "candidate-quality $Language $metric regression disagrees with policy or measurement."
        }
    }
}
function Test-CandidateQualityEvidence {
    [CmdletBinding()]
    param(
        $Report,
        $JUnitCases,
        $Execution,
        [hashtable]$Bindings,
        $Policy,
        $TierBLock,
        $CorpusManifest,
        [datetimeoffset]$CommitTimeUtc
    )
    # Fail-closed validation of the qualification run. Every mandatory
    # predicate is recomputed from the report and the current input hashes;
    # quality_gate.status alone is never trusted.
    $languages = Test-CandidateQualityPolicy $Policy
    Test-CandidateTierBApproval $TierBLock $Policy $CorpusManifest $Bindings['corpus_manifest_sha256']
    Assert-Execution $Execution
    $started = [DateTimeOffset]::Parse((Get-CqProperty $Execution 'started_utc'))
    if ($started -lt $CommitTimeUtc) {
        throw 'candidate-quality evidence was produced before the candidate commit existed.'
    }
    if (-not [object]::Equals((Get-CqProperty $Report 'format'), 'epocanva-candidate-eval-v3')) {
        throw 'candidate-quality report schema is unsupported.'
    }
    $identity = Get-CqProperty $Report 'identity'
    if (-not [object]::Equals((Get-CqProperty $identity 'mode'), 'qualification')) {
        throw 'candidate-quality report was not produced in qualification mode.'
    }
    if (-not [object]::Equals((Get-CqProperty $identity 'repo_commit'), $Bindings['commit'])) {
        throw 'candidate-quality report commit does not match the candidate.'
    }
    if (-not [object]::Equals((Get-CqProperty $identity 'source_tree_clean'), $true)) {
        throw 'candidate-quality report was produced from a dirty source tree.'
    }
    $identityBindings = @{
        quality_policy_sha256 = $Bindings['quality_policy_sha256']
        manifest_sha256 = $Bindings['corpus_manifest_sha256']
        backend_manifest_sha256 = $Bindings['backend_manifest_sha256']
        harness_sha256 = $Bindings['harness_sha256']
        rime_dll_sha256 = $Bindings['rime_dll_sha256']
        package_manifest_sha256 = $Bindings['package_manifest_sha256']
        package_policy_sha256 = $Bindings['package_policy_sha256']
        language_policy_sha256 = $Bindings['language_policy_sha256']
        source_archive_sha256 = $Bindings['source_archive_sha256']
        evaluator_sha256 = $Bindings['evaluator_sha256']
        evaluator_support_sha256 = $Bindings['evaluator_support_sha256']
        acceptance_profile = $Bindings['acceptance_profile']
    }
    foreach ($field in $identityBindings.Keys) {
        if (-not [object]::Equals((Get-CqProperty $identity $field), $identityBindings[$field])) {
            throw "candidate-quality report input binding mismatch: $field"
        }
    }
    $corpus = Get-CqProperty $Report 'corpus'
    $corpusEvidence = Get-CqProperty $corpus 'evidence'
    if ((Get-CqProperty $corpusEvidence 'status') -cne 'passed' -or
            (Get-CqProperty $corpusEvidence 'tier') -cne 'B') {
        throw 'candidate-quality corpus lacks an approved Tier B provenance lock.'
    }
    if ((Get-CqProperty $corpusEvidence 'lock_sha256') -cne $Bindings['tier_b_lock_sha256']) {
        throw 'candidate-quality Tier B lock changed after the run.'
    }
    $gate = Get-CqProperty $Report 'quality_gate'
    if (-not [object]::Equals((Get-CqProperty $gate 'status'), 'passed') -or
            -not [object]::Equals((Get-CqProperty $gate 'met'), $true)) {
        throw 'candidate-quality evaluator did not report a passing quality gate.'
    }
    if ((Get-CqProperty $gate 'policy_version') -cne (Get-CqProperty $Policy 'format')) {
        throw 'candidate-quality report policy version mismatch.'
    }
    foreach ($field in @('unmet_reasons', 'not_evaluated_predicates')) {
        $rows = Get-CqProperty $gate $field
        if ($rows -isnot [array] -or $rows.Count -ne 0) {
            throw 'candidate-quality report still lists unmet or unevaluated predicates.'
        }
    }
    $minimums = Get-CqProperty $gate 'minimum_cases'
    $policyMinimums = Get-CqProperty $Policy 'minimum_cases'
    foreach ($language in $languages) {
        if ((Get-CqInteger $minimums $language) -ne (Get-CqInteger $policyMinimums $language)) {
            throw "candidate-quality report minimum sample count disagrees for $language."
        }
    }
    $expectedPredicates = @('minimum_samples', 'exact_process_and_repeat_protocol',
        'all_expected_cases_executed', 'no_extra_cases', 'validation_failures_empty',
        'cross_process_determinism', 'in_process_repeat_determinism', 'empty_menu_rate_zero',
        'raw_contract', 'candidate_quality_metrics_recorded', 'latency_memory_recorded',
        'privacy_and_abi_status', 'wrong_language_observable', 'wrong_language_rate',
        'tier_b_corpus', 'performance_regression', 'absolute_quality_thresholds',
        'measured_scheme_admission')
    $predicates = Get-CqProperty $gate 'predicates'
    $observedNames = @($predicates.PSObject.Properties | ForEach-Object { $_.Name })
    if ($observedNames.Count -ne $expectedPredicates.Count -or
            @(Compare-Object ($expectedPredicates | Sort-Object) ($observedNames | Sort-Object)).Count -ne 0) {
        throw 'candidate-quality report predicate set is incomplete or contains unknown entries.'
    }
    foreach ($name in $expectedPredicates) {
        if ((Get-CqProperty (Get-CqProperty $predicates $name) 'status') -cne 'passed') {
            throw "candidate-quality predicate did not pass: $name"
        }
    }
    $failures = Get-CqProperty $Report 'failures'
    if ($failures -isnot [array] -or $failures.Count -ne 0) {
        throw 'candidate-quality report records case-level failures.'
    }
    $unverified = Get-CqProperty $Report 'unverified'
    if ($null -ne $unverified -and @($unverified.PSObject.Properties).Count -ne 0) {
        throw 'candidate-quality report still carries an unverified/partial-pass section.'
    }
    $determinism = Get-CqProperty $Report 'determinism'
    $qualification = Get-CqProperty $Policy 'qualification'
    if ((Get-CqInteger $determinism 'repeats_per_case') -ne (Get-CqInteger $qualification 'repeat') -or
            (Get-CqInteger $determinism 'fresh_runs') -ne (Get-CqInteger $qualification 'runs')) {
        throw 'candidate-quality determinism protocol differs from the frozen policy.'
    }
    $thresholds = Get-CqProperty $Policy 'thresholds'
    $perLanguage = Get-CqProperty $Report 'per_language'
    $observedLanguages = @($perLanguage.PSObject.Properties | ForEach-Object { $_.Name })
    if ($observedLanguages.Count -ne 5 -or
            @(Compare-Object ($languages | Sort-Object) ($observedLanguages | Sort-Object)).Count -ne 0) {
        throw 'candidate-quality report does not cover exactly the five required languages.'
    }
    foreach ($language in $languages) {
        $values = Get-CqProperty $perLanguage $language
        $minimum = Get-CqInteger $policyMinimums $language
        $caseCount = Get-CqInteger $values 'cases'
        if ($caseCount -lt $minimum -or $caseCount -ne [math]::Floor($caseCount) -or
                $caseCount -ne (Get-CqInteger (Get-CqProperty $CorpusManifest 'case_count') $language) -or
                (Get-CqInteger $values 'measured_cases') -ne $caseCount -or
                -not [object]::Equals((Get-CqProperty $values 'minimum_cases_met'), $true)) {
            throw "candidate-quality sample shortfall for $language."
        }
        $absolute = Get-CqProperty $Policy 'absolute_quality'
        foreach ($field in @('top1', 'top3', 'mrr', 'suggestion_top1', 'suggestion_top3', 'suggestion_mrr')) {
            $rate = Get-CqRate $values $field 0 1
            if ($field -in @('top1', 'top3', 'mrr') -and
                    $rate -lt (Get-CqRate $absolute ($field + '_min') 0 1)) {
                throw "candidate-quality measured $field is below the approved threshold for $language."
            }
        }
        foreach ($field in @('deterministic_case_rate', 'in_process_repeat_deterministic_rate',
                'cross_process_deterministic_rate')) {
            if ((Get-CqRate $values $field 0 1) -lt (Get-CqRate $thresholds 'deterministic_case_rate_min' 0 1)) {
                throw "candidate-quality determinism failed for $language."
            }
        }
        foreach ($field in @('raw_present_rate', 'raw_first_rate')) {
            $rate = Get-CqRate $values $field 0 1
            if ($rate -lt (Get-CqRate $thresholds ($field + '_min') 0 1)) {
                throw "candidate-quality raw contract failed for $language."
            }
        }
        $empty = Get-CqRate $values 'empty_menu_rate' 0 1
        if ($empty -gt (Get-CqRate $thresholds 'empty_menu_rate_max' 0 1)) {
            throw "candidate-quality empty-menu contract failed for $language."
        }
        $wrong = Get-CqRate $values 'wrong_language_rate' 0 1
        if ($wrong -gt (Get-CqRate $thresholds 'wrong_language_rate_max' 0 1)) {
            throw "candidate-quality wrong-language rate failed for $language."
        }
        $observability = Get-CqProperty $values 'wrong_language_observability'
        if ((Get-CqProperty $observability 'status') -cne 'observable') {
            throw "candidate-quality language attribution is not observable for $language."
        }
        $latency = Get-CqProperty $values 'latency_ms'
        if ((Get-CqNumber $latency 'cold') -lt 0) { throw "candidate-quality cold latency is invalid for $language." }
        $warm = Get-CqProperty $latency 'warm'
        foreach ($field in @('p50', 'p95', 'p99')) {
            if ((Get-CqNumber $warm $field) -lt 0) { throw "candidate-quality warm $field is invalid for $language." }
        }
        $p95 = Get-CqNumber $warm 'p95'
        $peak = Get-CqInteger $values 'peak_working_set_bytes'
        if ((Get-CqNumber $warm 'p50') -gt $p95 -or $p95 -gt (Get-CqNumber $warm 'p99') -or
                (Get-CqInteger $latency 'warmup_queries') -ne (Get-CqInteger $qualification 'warmup_queries') -or
                (Get-CqNumber $values 'resource_load_ms') -lt 0) {
            throw "candidate-quality memory/load record is missing for $language."
        }
        Assert-CqRegression (Get-CqProperty $Report 'regression') $Policy $language @{
            warm_p95_ms = $p95; peak_working_set_bytes = $peak
        }
    }
    $requiredCases = @('qualification', 'coverage', 'determinism', 'contracts', 'identity-binding')
    $cases = @($JUnitCases)
    if ($cases.Count -ne $requiredCases.Count) {
        throw 'candidate-quality JUnit must contain exactly the five mandatory cases.'
    }
    foreach ($name in $requiredCases) {
        $qualified = 'candidate-quality.' + $name
        if (@($cases | Where-Object { $_.GetAttribute('name') -ceq $qualified }).Count -ne 1) {
            throw "candidate-quality JUnit is missing or duplicates $qualified."
        }
    }
    foreach ($case in $cases) {
        if (@($case.SelectNodes('.//failure | .//error | .//skipped')).Count -ne 0) {
            throw 'candidate-quality JUnit contains a failed, errored or skipped case.'
        }
    }
    $document = $cases[0].OwnerDocument
    foreach ($key in $Bindings.Keys) {
        $properties = @($document.SelectNodes("//properties/property[@name='epocanva.$key']"))
        if ($properties.Count -ne 1 -or $properties[0].GetAttribute('value') -cne [string]$Bindings[$key]) {
            throw "candidate-quality JUnit lacks the current binding: epocanva.$key"
        }
    }
    return $true
}
