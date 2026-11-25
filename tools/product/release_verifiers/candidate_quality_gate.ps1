Add-Gate 'candidate-quality' {
    if ($null -eq $evidence -or $null -eq $source -or $null -eq $manifest) {
        throw 'Candidate quality requires current evidence, source and package-manifest identities.'
    }
    $entries = @($evidence.entries | Where-Object { $_.id -ceq 'candidate-quality' })
    if ($entries.Count -ne 1) { throw 'Exactly one current evidence entry is required: candidate-quality' }
    $entry = $entries[0]
    if ($entry.kind -cne 'test-run') { throw 'candidate-quality requires an executed test-run with log and JUnit.' }
    $runnerPath = Read-Reference $entry.runner
    if ($entry.command[0] -ine $runnerPath) { throw 'candidate-quality command does not identify its hashed runner.' }
    $knownRunner = Join-Path $RepoRoot 'tools\product\run_candidate_quality.ps1'
    if ((Get-Item -LiteralPath $knownRunner -Force).FullName -ine $runnerPath) {
        throw 'candidate-quality evidence was not produced by the committed runner.'
    }
    $evaluatorPath = Read-Reference $entry.evaluator
    if ($evaluatorPath -ine (Join-Path $RepoRoot 'tools\backend\evaluate_candidates.py')) {
        throw 'candidate-quality evidence names an unexpected evaluator.'
    }
    $harnessPath = Read-Reference $entry.harness
    Assert-PlainPath $harnessPath $buildBase
    if ($null -eq $receipt) { throw 'Quality requires a verified candidate build receipt.' }
    $qualityRimePath = Read-Reference $entry.rime
    . (Join-Path $RepoRoot 'tools/product/release_verifiers/quality_binary.ps1')
    $qualityBinaries = Assert-ProductQualityBinaryBindings $receipt $BuildDir $harnessPath $qualityRimePath
    $logPath = Read-Reference $entry.log
    $junitPath = Read-Reference $entry.junit
    $reportPath = Read-Reference $entry.report
    if ($reportPath -ine (Join-Path $BuildDir 'candidate-quality\report.json') -or
            $junitPath -ine (Join-Path $BuildDir 'candidate-quality\candidate-quality.junit.xml') -or
            $logPath -ine (Join-Path $BuildDir 'candidate-quality\process.log')) {
        throw 'candidate-quality evidence files are not the producer outputs for this build directory.'
    }
    $qualityPolicyPath = Join-Path $RepoRoot 'data\eval\candidate-v1\quality-policy-v2.json'
    $corpusManifestPath = Join-Path $RepoRoot 'data\eval\candidate-v1\manifest.json'
    $backendManifestPath = Join-Path $RepoRoot 'data\locale\backend-bindings.json'
    $supportPath = Join-Path $RepoRoot 'tools\backend\candidate_quality_policy.py'
    $sourceArchivePath = Join-Path $BuildDir 'corresponding-source.zip'
    Assert-PlainPath $sourceArchivePath $buildBase
    $policyDocument = Read-ProductJson $qualityPolicyPath
    $tierBLockPath = Join-Path (Split-Path -Parent $qualityPolicyPath) ([string]$policyDocument.tier_b_lock)
    Assert-PlainPath $tierBLockPath $RepoRoot
    $tierBLockDocument = Read-ProductJson $tierBLockPath
    $corpusManifestDocument = Read-ProductJson $corpusManifestPath
    # Verify the actual approved corpus bytes; a manifest is not evidence that
    # its listed files still exist, match, or retain the approved provenance.
    foreach ($language in @($policyDocument.languages)) {
        $corpusPath = Join-Path $RepoRoot ('data/eval/candidate-v1/cases/' + $language + '.jsonl')
        Assert-PlainPath $corpusPath $RepoRoot
        if ((Get-Sha256 $corpusPath) -cne $corpusManifestDocument.corpus_sha256.($language + '.jsonl')) {
            throw "Current Tier B corpus bytes differ for $language."
        }
        $approvedRow = Get-CqProperty (Get-CqProperty $tierBLockDocument 'languages') $language
        $caseCount = 0
        foreach ($line in [IO.File]::ReadLines($corpusPath)) {
            $case = ConvertFrom-ProductJson $line
            if ($case.language -cne $language -or $case.source -cne $approvedRow.source -or
                    $case.license -cne $approvedRow.license) { throw "Tier B provenance differs for $language." }
            ++$caseCount
        }
        if ($caseCount -ne $approvedRow.case_count) { throw "Actual Tier B case count differs for $language." }
    }
    $bindings = @{
        version = $version
        commit = $source.commit
        source_sha256 = $source.source_sha256
        package_manifest_sha256 = $manifest.sha256
        package_policy_sha256 = $policyHash
        language_policy_sha256 = $languagePolicyHash
        acceptance_profile = $AcceptanceProfile
        quality_policy_sha256 = (Get-Sha256 $qualityPolicyPath)
        corpus_manifest_sha256 = (Get-Sha256 $corpusManifestPath)
        source_archive_sha256 = (Get-Sha256 $sourceArchivePath)
        evaluator_sha256 = (Get-Sha256 $evaluatorPath)
        evaluator_support_sha256 = (Get-Sha256 $supportPath)
        runner_sha256 = (Get-Sha256 $runnerPath)
        harness_sha256 = $qualityBinaries.harness.sha256
        rime_dll_sha256 = $qualityBinaries.rime.sha256
        backend_manifest_sha256 = (Get-Sha256 $backendManifestPath)
        tier_b_lock_sha256 = (Get-Sha256 $tierBLockPath)
    }
    $reportDocument = Read-ProductJson $reportPath
    $cases = Read-PassingJUnit $junitPath
    $commitTimeText = (& git -C $RepoRoot show -s --format=%cI HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or -not $commitTimeText) { throw 'Cannot determine the candidate commit timestamp.' }
    $commitTime = [DateTimeOffset]::Parse($commitTimeText)
    $null = Test-CandidateQualityEvidence -Report $reportDocument -JUnitCases $cases -Execution $entry `
        -Bindings $bindings -Policy $policyDocument -TierBLock $tierBLockDocument `
        -CorpusManifest $corpusManifestDocument -CommitTimeUtc $commitTime
    'Qualification exited 0, its report passed every recomputed gate, and samples/contracts/determinism bind this candidate.'
}


