$requiredTests = [ordered]@{
    'installer-lifecycle' = @('install', 'repeat-install', 'upgrade', 'repair', 'uninstall', 'rollback', 'cleanup',
        'multi-user', 'registration', 'user-data-policy')
    'real-host' = @('registration', 'test-start', 'notepad', 'chromium', 'word', 'sensitive-fields',
        'english-cross-app-consistency', 'cleanup')
    'english-capabilities' = @('frequency', 'context-learning', 'morphology', 'spelling', 'segmentation', 'abbreviation', 'cross-app-consistency')
    'offline-privacy' = @('runtime-no-network', 'dependency-review', 'no-content-logging', 'sensitive-no-persistence')
    'language-acceptance' = @('en', 'zh', 'fr', 'ja', 'ko' | ForEach-Object {
        $language = $_
        'core', 'engine', 'tsf', 'installed-host' | ForEach-Object { "$language.$_" }
    })
    'stress-reference-model' = @('seed-1', 'seed-2', 'seed-3', 'seed-4', 'seed-5', 'negative-control',
        'replay-minimization', 'resource-growth')
    'host-soak' = @('duration', 'notepad', 'chromium', 'word', 'language-switch', 'focus-switch',
        'candidate-interaction', 'long-input', 'resource-growth', 'cleanup')
    'acceptance-ci-provenance' = @('commit', 'runner', 'build-configuration', 'toolchain', 'junit',
        'exit-codes', 'artifact-hashes', 'source-archive')
    'signing' = @('EpoCanva.exe', 'epocanva_tsf.dll', 'rime.dll', 'installer', 'publisher', 'timestamp', 'trust')
}
$requiredIds = @('project-license', 'lexicon-redistribution', 'package-diagnose') + @($requiredTests.Keys)
foreach ($id in $requiredIds) {
    $category = if ($id -in @('project-license', 'lexicon-redistribution')) { 'distribution' }
        elseif ($id -eq 'signing') { 'signature' } else { 'technical' }
    if ($id -eq 'signing' -and $AcceptanceProfile -ceq 'internal-unsigned' -and
            ($null -eq $evidence -or @($evidence.entries | Where-Object { $_.id -ceq 'signing' }).Count -eq 0)) {
        $checks.Add([pscustomobject]@{ id = 'signing'; category = 'signature'; status = 'not_required';
            detail = 'Internal unsigned acceptance omits signing only; no signature has been verified.' })
        continue
    }
    Add-Gate $id {
        if ($null -eq $evidence) { throw "Missing current, hash-bound evidence: $id. Not executed/approved is blocked, never green." }
        $entries = @($evidence.entries | Where-Object { $_.id -ceq $id })
        if ($entries.Count -ne 1) { throw "Exactly one current evidence entry is required: $id" }
        $entry = $entries[0]
        if ($id -in @('project-license', 'lexicon-redistribution')) {
            if ($entry.kind -cne 'owner-record' -or $entry.decision -cne 'approved-public-distribution' -or
                    [string]::IsNullOrWhiteSpace($entry.owner) -or [string]::IsNullOrWhiteSpace($entry.record_id) -or
                    $entry.jurisdictions -isnot [array] -or @($entry.jurisdictions).Count -eq 0 -or
                    @($entry.jurisdictions | Where-Object { $_ -isnot [string] -or [string]::IsNullOrWhiteSpace($_) }).Count -ne 0) {
                throw 'An attributable owner/legal record with decision, scope and jurisdictions is required; a manual bool is not evidence.'
            }
            $record = Read-Reference $entry.record
            "Owner-supplied record hash verified: $id. Authenticity/authority remains the release owner's responsibility."
        } else {
            Assert-Execution $entry
            if ($id -eq 'package-diagnose') {
                if ($entry.kind -cne 'diagnose') { throw 'Native diagnosis evidence is required.' }
                $reportPath = Read-Reference $entry.report
                $userRoot = Join-Path $BuildDir 'diagnose-user'
                Assert-PlainPath $reportPath $BuildDir
                Assert-PlainPath $userRoot $BuildDir
                $expected = @((Join-Path $StageRoot 'EpoCanva.exe'), '--diagnose', $reportPath, '--user-data-root', $userRoot)
                if (@($entry.command).Count -ne $expected.Count) { throw 'Diagnosis must use the isolated build/<preset>/diagnose-user root.' }
                for ($argumentIndex = 0; $argumentIndex -lt $expected.Count; ++$argumentIndex) {
                    if ($entry.command[$argumentIndex] -cne $expected[$argumentIndex]) { throw 'Diagnosis command differs from the required isolated invocation.' }
                }
                $values = @{}
                foreach ($line in (Get-Content -LiteralPath $reportPath -Encoding UTF8)) {
                    if ($line -notmatch '^([^=]+)=(.*)$' -or $values.ContainsKey($Matches[1])) { throw 'Malformed or duplicate diagnosis field.' }
                    $values[$Matches[1]] = $Matches[2]
                }
                $expectedValues = @{ product = 'EpoCanva'; package = 'ready'; manifest = 'ok'; manifest_failures = '0';
                    manifest_files = [string]$policy.Count; version = $version; commit = $source.commit; user_data = 'ok'; user_root = $userRoot;
                    manifest_format = '2'; acceptance_profile = $AcceptanceProfile;
                    distribution_policy = $manifest.distribution_policy; path_exception = 'none';
                    readiness_scope = 'local-package-integrity-only' }
                foreach ($key in $expectedValues.Keys) {
                    if ($values[$key] -cne $expectedValues[$key]) { throw "Diagnosis disagrees with current package/isolation: $key" }
                }
                'Recorded headless diagnosis succeeded with an explicit isolated user-data root; registration status is not release evidence.'
            } else {
                if ($entry.kind -cne 'test-run') { throw 'An executed test-run with JUnit and a process log is required.' }
                if ($id -in @('installer-lifecycle', 'signing', 'real-host', 'language-acceptance', 'host-soak', 'acceptance-ci-provenance') -and $null -eq $installer) {
                    throw 'Installer/signing/installed-host tests require the exact installer artifact.'
                }
                if ($id -eq 'language-acceptance' -and -not (Test-GatePassed 'language-readiness')) {
                    throw 'All five implemented languages and actual packaged data are required before aggregate language acceptance.'
                }
                $runnerPath = Read-Reference $entry.runner
                if ($entry.command[0] -cne $runnerPath) { throw 'Executed command does not identify the hashed test runner.' }
                $logPath = Read-Reference $entry.log
                $junitPath = Read-Reference $entry.junit
                $cases = Read-PassingJUnit $junitPath
                # Bind the runner's own report, not only an envelope that could be
                # accidentally repointed at an unrelated historical passing JUnit.
                $reportBindings = @{ version = $version; commit = $source.commit; source_sha256 = $source.source_sha256;
                    package_manifest_sha256 = $manifest.sha256; language_policy_sha256 = $languagePolicyHash;
                    acceptance_profile = $AcceptanceProfile }
                if ($id -in @('installer-lifecycle', 'signing', 'real-host', 'language-acceptance', 'host-soak', 'acceptance-ci-provenance')) {
                    $reportBindings['installer_sha256'] = $evidence.installer.sha256
                }
                foreach ($key in $reportBindings.Keys) {
                    $properties = @($cases[0].OwnerDocument.SelectNodes("//properties/property[@name='epocanva.$key']"))
                    if ($properties.Count -ne 1 -or $properties[0].GetAttribute('value') -cne $reportBindings[$key]) {
                        throw "Test runner report lacks the current artifact binding: epocanva.$key"
                    }
                }
                foreach ($name in $requiredTests[$id]) {
                    $qualifiedName = $id + '.' + $name
                    if (@($cases | Where-Object { $_.GetAttribute('name') -ceq $qualifiedName }).Count -ne 1) {
                        throw "Missing/duplicate executed acceptance case: $qualifiedName"
                    }
                }
                if ($id -in @('stress-reference-model', 'host-soak', 'acceptance-ci-provenance')) {
                    $reportPath = Read-Reference $entry.report
                    $measured = Read-ProductJson $reportPath
                    $null = Test-HardAcceptanceReport $id $measured $entry $reportBindings
                    if ($id -ceq 'stress-reference-model') {
                        $validator = Join-Path $RepoRoot 'tools/product/verify_stress_reference_model.py'
                        Assert-PlainPath $validator $RepoRoot
                        if (-not (Test-Path -LiteralPath $validator -PathType Leaf)) { throw 'Stress verifier is missing.' }
                        $python = Get-Command python -CommandType Application -ErrorAction Stop | Select-Object -First 1
                        $PSNativeCommandUseErrorActionPreference = $false
                        $global:LASTEXITCODE = $null
                        $validation = @(& $python.Source -B $validator --repo $RepoRoot --build-dir $BuildDir `
                            --stage-root $StageRoot --report $reportPath --runner $runnerPath --junit $junitPath --log $logPath)
                        $validatorExit = $LASTEXITCODE
                        if ($validatorExit -isnot [int] -or $validatorExit -ne 0) {
                            throw "Stress raw evidence verifier blocked (exit $validatorExit): $($validation -join ' ')"
                        }
                    }
                }
                if ($id -ceq 'english-capabilities' -and -not (Test-GatePassed 'real-host')) {
                    throw 'English cross-app consistency requires installed Notepad/Chromium/Word evidence; controlled sessions alone are insufficient.'
                }
                if ($id -ceq 'offline-privacy' -and -not (Test-GatePassed 'real-host')) {
                    throw 'Formal privacy acceptance requires the authorized installed-host sensitive-field run; import scans and controlled endpoint polling alone are insufficient.'
                }
                "$($cases.Count) executed passing cases with hashed runner, JUnit and process log: $id."
            }
        }
    } $category
}
