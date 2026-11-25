# Offline consistency verification. Hash-bound JSON is not proof of who issued
# an authorization or retrieved an API snapshot; review that trust separately.
Set-StrictMode -Version Latest

function Assert-CiPositiveInteger($Value, [string]$Field) {
    if (-not (Test-ProductJsonInteger $Value) -or $Value -le 0) { throw "CI $Field must be a positive JSON integer." }
}

function Assert-CiText($Value, [string]$Field) {
    if ($Value -isnot [string] -or [string]::IsNullOrWhiteSpace($Value)) { throw "CI $Field must be a nonempty string." }
}

function Assert-CiTextMembers($Value, [string[]]$Fields) {
    if ($Value -isnot [pscustomobject]) { throw 'CI requires a JSON object.' }
    foreach ($field in $Fields) { Assert-CiText $Value.$field $field }
}

function Assert-CiLabels($Labels, [string[]]$Required) {
    if ($Labels -isnot [array] -or @($Labels | Where-Object { $_ -isnot [string] }).Count -ne 0 -or
            @($Labels | Select-Object -Unique).Count -ne $Labels.Count) { throw 'CI runner labels are invalid.' }
    foreach ($label in $Required) {
        if ($label -cnotin $Labels) { throw "CI runner is missing required label: $label" }
    }
}

function Assert-CiArchiveSummary([string]$Archive, [string]$EntryPath, [string]$Summary) {
    Assert-ProductRelativePath $EntryPath
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $found = $false
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName.TrimEnd('/')
            Assert-ProductRelativePath $name
            if (-not $seen.Add($name)) { throw 'CI artifact has duplicate archive paths.' }
            # Unix symlink payloads must not be treated as ordinary evidence.
            if ((($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) { throw 'CI artifact contains a symlink.' }
            if ($entry.FullName -ceq $EntryPath) {
                if ($entry.Length -le 0 -or $entry.Length -gt 4MB) { throw 'CI summary archive member has an invalid byte count.' }
                $stream = $entry.Open()
                $sha = [Security.Cryptography.SHA256]::Create()
                try { $digest = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
                finally { $stream.Dispose(); $sha.Dispose() }
                if ($digest -cne (Get-ProductIdentityFileSha256 $Summary)) { throw 'CI summary differs from the uploaded artifact.' }
                $found = $true
            }
        }
        if (-not $found) { throw 'CI artifact does not contain the referenced layer summary.' }
    } finally { $zip.Dispose() }
}

function Get-CiRequiredGates([string]$Layer) {
    switch ($Layer) {
        'windows-product' { @('source-clean', 'verified-build', 'engine-identity', 'build-tests', 'package-integrity',
            'package-identity', 'package-path-hygiene', 'corresponding-source', 'installer-artifact',
            'installer-toolchain', 'package-diagnose', 'inputs-stable') }
        'windows-integration-acceptance' { @('installer-lifecycle', 'real-host', 'english-capabilities',
            'offline-privacy', 'language-acceptance', 'host-soak', 'inputs-stable') }
        'full-quality-stress-acceptance' { @('candidate-quality', 'stress-reference-model', 'inputs-stable') }
        default { @() }
    }
}

function Assert-CiProvenance($Report, $Bindings) {
    Assert-CiPositiveInteger $Report.run_id 'report run_id'
    $run = Read-ProductJson (Read-Reference $Report.github_run)
    $jobs = Read-ProductJson (Read-Reference $Report.github_jobs)
    $manifest = Read-ProductJson (Read-Reference $Report.artifact_manifest)
    $authorization = Read-ProductJson (Read-Reference $Report.runner_authorization)
    Assert-CiTextMembers $run @('head_sha', 'path', 'status', 'conclusion', 'event')
    Assert-CiTextMembers $run.repository @('full_name')
    Assert-CiPositiveInteger $run.id 'run.id'
    Assert-CiPositiveInteger $run.run_attempt 'run.run_attempt'
    Assert-CiPositiveInteger $jobs.total_count 'jobs.total_count'
    if ($Bindings.commit -cnotmatch '\A[0-9a-f]{40}\z' -or $run.head_sha -cne $Bindings.commit -or
            $run.repository.full_name -cne 'shijianus/EpoCanva' -or
            $run.path -cne '.github/workflows/windows-ci.yml' -or
            $run.status -cne 'completed' -or $run.conclusion -cne 'success' -or
            $run.event -cne 'workflow_dispatch' -or $Report.run_id -ne $run.id -or
            $jobs.jobs -isnot [array] -or $jobs.total_count -ne $jobs.jobs.Count) {
        throw 'CI requires complete successful dispatch snapshots for the exact candidate and attempt.'
    }
    foreach ($document in @($manifest, $authorization)) {
        Assert-CiTextMembers $document @('format', 'repository', 'source_commit')
        Assert-CiPositiveInteger $document.run_id 'bound run_id'
        Assert-CiPositiveInteger $document.run_attempt 'bound run_attempt'
        if ($document.repository -cne $run.repository.full_name -or $document.run_id -ne $run.id -or
                $document.run_attempt -ne $run.run_attempt -or $document.source_commit -cne $Bindings.commit) {
            throw 'CI artifact/authorization record belongs to another candidate or run attempt.'
        }
    }
    if ($manifest.format -cne 'epocanva-ci-artifacts-v1' -or $manifest.entries -isnot [array] -or
            $manifest.entries.Count -ne 6 -or $authorization.format -cne 'epocanva-ci-runner-authorization-v1' -or
            $authorization.decision -cne 'approved-isolated-acceptance') { throw 'Unsupported CI artifact or runner record schema.' }
    foreach ($name in @('owner', 'record_id', 'runner_name', 'environment_id', 'snapshot_restore_policy', 'runner_label')) {
        Assert-CiText $authorization.$name "authorization.$name"
    }
    Assert-CiText $authorization.decision 'authorization.decision'
    Assert-CiPositiveInteger $authorization.runner_id 'authorized runner_id'
    Assert-CiPositiveInteger $authorization.runner_group_id 'authorized runner_group_id'
    $null = Read-Reference $authorization.record
    $apiArtifacts = Read-ProductJson (Read-Reference $manifest.github_artifacts)
    Assert-CiPositiveInteger $apiArtifacts.total_count 'artifacts.total_count'
    if ($apiArtifacts.artifacts -isnot [array] -or $apiArtifacts.total_count -ne $apiArtifacts.artifacts.Count) {
        throw 'CI artifact API snapshot is incomplete.'
    }
    $artifactIds = [Collections.Generic.HashSet[long]]::new()
    foreach ($artifact in $apiArtifacts.artifacts) {
        Assert-CiPositiveInteger $artifact.id 'artifact.id'
        if (-not $artifactIds.Add($artifact.id)) { throw 'CI artifact IDs are duplicated.' }
    }
    $jobIds = [Collections.Generic.HashSet[long]]::new()
    foreach ($job in $jobs.jobs) {
        Assert-CiTextMembers $job @('name', 'head_sha', 'status', 'conclusion', 'runner_name')
        Assert-CiPositiveInteger $job.id 'job.id'
        Assert-CiPositiveInteger $job.run_id 'job.run_id'
        Assert-CiPositiveInteger $job.run_attempt 'job.run_attempt'
        if (-not $jobIds.Add($job.id) -or $job.run_id -ne $run.id -or $job.run_attempt -ne $run.run_attempt -or
                $job.head_sha -cne $Bindings.commit) { throw 'CI job IDs, run attempt or candidate are inconsistent.' }
    }
    $layers = @('contract-gate', 'root-unit-tests', 'backend', 'windows-product',
        'windows-integration-acceptance', 'full-quality-stress-acceptance')
    foreach ($name in $layers) {
        $rows = @($jobs.jobs | Where-Object { $_.name -ceq $name })
        if ($rows.Count -ne 1 -or $rows[0].status -cne 'completed' -or $rows[0].conclusion -cne 'success') {
            throw "Missing, skipped, cancelled or nonpassing CI job: $name"
        }
        $job = $rows[0]
        Assert-CiPositiveInteger $job.runner_id 'job.runner_id'
        Assert-CiText $job.runner_name 'job.runner_name'
        if ($job.steps -isnot [array] -or $job.steps.Count -eq 0) { throw 'CI job steps are absent.' }
        foreach ($step in $job.steps) {
            Assert-CiTextMembers $step @('name', 'status', 'conclusion')
            if ($step.status -cne 'completed' -or $step.conclusion -cne 'success') { throw 'CI job contains an unexecuted or failed step.' }
        }
        if ($name -cin @('contract-gate', 'root-unit-tests')) {
            Assert-CiLabels $job.labels @('windows-2022')
            if ('self-hosted' -cin $job.labels) { throw 'Hosted CI layer used a self-hosted runner.' }
        } else {
            Assert-CiPositiveInteger $job.runner_group_id 'job.runner_group_id'
            Assert-CiLabels $job.labels @('self-hosted', 'Windows', 'X64', 'epocanva-acceptance-x64', $authorization.runner_label)
            if ($job.runner_id -ne $authorization.runner_id -or $job.runner_name -cne $authorization.runner_name -or
                    $job.runner_group_id -ne $authorization.runner_group_id) { throw 'CI job ran outside the designated isolated runner.' }
        }
        $entries = @($manifest.entries | Where-Object { $_.job -ceq $name })
        if ($entries.Count -ne 1) { throw "CI artifact manifest is missing or duplicates layer: $name" }
        $entry = $entries[0]
        Assert-CiTextMembers $entry @('job', 'summary_archive_path')
        Assert-CiPositiveInteger $entry.artifact_id 'entry.artifact_id'
        Assert-CiPositiveInteger $entry.job_id 'entry.job_id'
        if ($entry.job_id -ne $job.id) { throw 'CI artifact manifest points to another job.' }
        $artifacts = @($apiArtifacts.artifacts | Where-Object { $_.id -eq $entry.artifact_id })
        if ($artifacts.Count -ne 1) { throw 'CI artifact is absent from the API snapshot.' }
        $artifact = $artifacts[0]
        Assert-CiTextMembers $artifact @('name', 'digest')
        Assert-CiTextMembers $artifact.workflow_run @('head_sha')
        Assert-CiPositiveInteger $artifact.workflow_run.id 'artifact workflow_run.id'
        Assert-CiPositiveInteger $artifact.size_in_bytes 'artifact.size_in_bytes'
        if ($artifact.expired -isnot [bool] -or $artifact.expired -or $artifact.workflow_run.id -ne $run.id -or
                $artifact.workflow_run.head_sha -cne $Bindings.commit -or
                $artifact.name -cne "$name-$($Bindings.commit)-$($run.id)-$($run.run_attempt)" -or
                $artifact.digest -isnot [string] -or $artifact.digest -cnotmatch '\Asha256:[0-9a-f]{64}\z') {
            throw 'CI artifact identity, expiry or GitHub digest is invalid.'
        }
        $archive = Read-Reference $entry.archive
        if ($artifact.digest -cne ('sha256:' + (Get-ProductIdentityFileSha256 $archive)) -or
                (Get-Item -LiteralPath $archive).Length -ne $artifact.size_in_bytes) { throw 'CI artifact archive differs from GitHub digest or byte count.' }
        $summaryPath = Read-Reference $entry.summary
        Assert-CiArchiveSummary $archive $entry.summary_archive_path $summaryPath
        $summary = Read-ProductJson $summaryPath
        Assert-CiLayerSummary $summary $name $Bindings.commit ([string]$run.id) ([string]$run.run_attempt) $job.runner_name
        if ($name -cin @('windows-product', 'windows-integration-acceptance', 'full-quality-stress-acceptance')) {
            if ($summary.product.package -cne $Bindings.package_manifest_sha256 -or
                    $summary.product.installer_exe -cne $Bindings.installer_sha256) {
                throw 'CI product artifacts differ from the candidate under acceptance.'
            }
            if ($name -ceq 'windows-product') { $productIdentity = $summary.product }
            foreach ($property in $productIdentity.PSObject.Properties) {
                if ($summary.product.($property.Name) -cne $property.Value) { throw 'CI layers tested different product bytes.' }
            }
        }
    }
}

function Assert-CiLayerSummary($Summary, [string]$Layer, [string]$Commit, [string]$RunId, [string]$Attempt, [string]$Runner) {
    Assert-CiTextMembers $Summary @('format', 'status', 'layer', 'source_commit', 'run_id', 'run_attempt',
        'runner_name', 'workflow_sha256', 'input_lock_sha256')
    if ($Summary.format -cne 'epocanva-windows-ci-layer-v2' -or $Summary.status -cne 'passed' -or
            $Summary.layer -cne $Layer -or $Summary.source_commit -cne $Commit -or
            $Summary.run_id -isnot [string] -or $Summary.run_id -cne $RunId -or
            $Summary.run_attempt -isnot [string] -or $Summary.run_attempt -cne $Attempt -or
            $Summary.runner_name -cne $Runner -or -not (Test-ProductJsonInteger $Summary.exit_code) -or $Summary.exit_code -ne 0) {
        throw 'CI layer summary identity, state or observed exit code is invalid.'
    }
    $start = ConvertTo-ProductUtc $Summary.started_utc
    $end = ConvertTo-ProductUtc $Summary.completed_utc
    if ($end -lt $start) { throw 'CI layer timestamps are reversed.' }
    if ($Summary.workflow_sha256 -cne (Get-ProductIdentityFileSha256 (Join-Path $RepoRoot '.github/workflows/windows-ci.yml')) -or
            $Summary.input_lock_sha256 -cne (Get-ProductIdentityFileSha256 (Join-Path $RepoRoot 'tools/ci/inputs.lock.json'))) {
        throw 'CI layer used another workflow or input lock.'
    }
    $codes = $Summary.exit_codes
    if ($codes -isnot [pscustomobject] -or @($codes.PSObject.Properties).Count -eq 0) { throw 'CI layer has no observed process exits.' }
    foreach ($field in $codes.PSObject.Properties) {
        if (-not (Test-ProductJsonInteger $field.Value) -or
                ($field.Value -ne 0 -and -not ($field.Name -ceq 'release-check' -and $field.Value -eq 1))) {
            throw 'CI layer contains a failed or noninteger process exit.'
        }
    }
    $required = switch ($Layer) {
        'contract-gate' { @('registered', 'ctest', 'verify-junit') }
        'root-unit-tests' { @('registered', 'ctest', 'verify-junit') }
        'backend' { @('verify-immutable-cache', 'verify-workspace-inputs', 'configure', 'build', 'registered', 'ctest', 'verify-junit', 'verify-workspace-inputs-after') }
        'windows-product' { @('build-product', 'stage-product', 'stage-source', 'build-helper', 'build-msi', 'build-bootstrapper', 'record-installer', 'installer-contract', 'diagnose-package', 'release-check', 'verify-junit', 'verify-workspace-inputs-after') }
        default { @('authorized-plan', 'verify-junit', 'release-check', 'verify-workspace-inputs-after') }
    }
    foreach ($name in $required) { if ($name -cnotin @($codes.PSObject.Properties.Name)) { throw "CI completion lacks an observed exit: $name" } }
    if ($Layer -ceq 'full-quality-stress-acceptance' -and 'reference-stress' -cnotin @($codes.PSObject.Properties.Name)) { throw 'CI stress command was not executed.' }
    # Reopen both files and compare names, never trust a summary count alone.
    $inventory = Read-ProductJson (Read-Reference $Summary.outputs.registered)
    if ($inventory.kind -cne 'ctestInfo' -or -not (Test-ProductJsonInteger $inventory.version.major) -or
            $inventory.version.major -ne 1 -or -not (Test-ProductJsonInteger $inventory.version.minor) -or
            $inventory.version.minor -lt 0 -or $inventory.tests -isnot [array] -or $inventory.tests.Count -eq 0) {
        throw 'CI registration is not a CTest v1 inventory.'
    }
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($test in $inventory.tests) {
        if ($test.name -isnot [string] -or [string]::IsNullOrWhiteSpace($test.name) -or -not $names.Add($test.name)) {
            throw 'CI registration contains invalid or duplicate names.'
        }
    }
    $cases = Read-ProductPassingJUnit (Read-Reference $Summary.outputs.junit)
    Assert-ProductTestInventory $inventory $cases
    foreach ($field in $Summary.outputs.PSObject.Properties) { $null = Read-Reference $field.Value }
    if ($Layer -ceq 'windows-product') { $null = Read-ProductPassingJUnit (Read-Reference $Summary.outputs.installer_junit) }
    $gates = @(Get-CiRequiredGates $Layer)
    if ($gates.Count -gt 0) {
        $release = Read-ProductJson (Read-Reference $Summary.outputs.release_check)
        Assert-CiTextMembers $release @('status', 'acceptance_profile')
        if (-not (Test-ProductJsonInteger $release.format) -or $release.format -ne 2 -or $release.status -cne 'completed' -or
                $release.acceptance_profile -cne 'internal-unsigned' -or $release.source.commit -cne $Commit -or
                -not (Test-ProductJsonInteger $release.exit_code) -or $release.exit_code -ne $codes.'release-check' -or
                $release.checks -isnot [array]) { throw 'CI release check snapshot is stale or incomplete.' }
        foreach ($id in $gates) {
            $checks = @($release.checks | Where-Object { $_.id -is [string] -and $_.id -ceq $id })
            if ($checks.Count -ne 1 -or $checks[0].status -isnot [string] -or $checks[0].status -cne 'passed') {
                throw "CI layer did not pass mandatory gate: $id"
            }
        }
        foreach ($key in @('package', 'receipt', 'source_manifest', 'source_zip', 'installer_manifest', 'installer_exe',
                'product_exe', 'tsf_dll', 'rime_dll')) {
            if (-not (Test-ProductSha256 $Summary.product.$key) -or
                    $Summary.product.$key -cne $Summary.outputs.("product-$key").sha256) { throw 'CI product hash is not bound to its actual file.' }
        }
    }
}
