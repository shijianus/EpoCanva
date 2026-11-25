Add-Gate 'inputs-stable' {
    if ($null -eq $source) { throw 'Initial source snapshot is unavailable.' }
    $current = Get-ProductSourceIdentity -RepoRoot $RepoRoot
    if ($current.commit -cne $source.commit -or $current.source_sha256 -cne $source.source_sha256 -or
            $current.repository_sha256 -cne $source.repository_sha256 -or
            ($receiptHash -and (Get-Sha256 $receiptPath) -cne $receiptHash) -or
            ($policyHash -and (Get-Sha256 $policyPath) -cne $policyHash) -or
            ($evidenceHash -and (Get-Sha256 $EvidencePath) -cne $evidenceHash) -or
            ($licenseDecisionHash -and (Get-Sha256 (Join-Path $RepoRoot 'data\manifests\license-decision.json')) -cne $licenseDecisionHash) -or
            ($languagePolicyHash -and (Get-Sha256 $languagePolicyPath) -cne $languagePolicyHash) -or
            ($sourceManifestHash -and (Get-Sha256 (Join-Path $BuildDir 'source-manifest.json')) -cne $sourceManifestHash) -or
            ($null -ne $manifest -and (Get-Sha256 (Join-Path $StageRoot 'package.manifest')) -cne $manifest.sha256)) {
        throw 'Source or release inputs changed during the check; rerun on an immutable candidate.'
    }
    if ($null -ne $receipt) {
        $null = Assert-ProductBuildReceipt -RepoRoot $RepoRoot -BuildDir $BuildDir -RimeDll $RimeDll
        $null = Assert-ProductBuildEngineBinding $RepoRoot $RimeDll $receipt
    }
    if ($null -ne $manifest) {
        foreach ($relative in $manifest.files.Keys) {
            $path = Join-Path $StageRoot $relative
            $file = Get-Item -LiteralPath $path -Force
            if ($file.Length -ne $manifest.files[$relative].bytes -or
                    (Get-Sha256 $path) -cne $manifest.files[$relative].sha256) { throw 'Payload bytes changed during the check.' }
        }
    }
    foreach ($reference in $references) {
        $file = Get-Item -LiteralPath $reference.path -Force
        if ($file.Length -ne $reference.bytes -or (Get-Sha256 $reference.path) -cne $reference.sha256) {
            throw 'An evidence or installer file changed during the check.'
        }
    }
    'Source, tested binaries, receipt, policy, evidence and package identities remained stable during this check.'
}

