Add-Gate 'release-evidence-binding' {
    Assert-PlainPath $EvidencePath $buildBase
    if (-not (Test-Path -LiteralPath $EvidencePath -PathType Leaf)) { throw 'release-evidence.json is missing; approvals and test runs are not inferred.' }
    if ($null -eq $source -or $null -eq $manifest -or $null -eq $receipt) { throw 'Evidence cannot be accepted without verified source/package/build identities.' }
    $documentHash = Get-Sha256 $EvidencePath
    $document = Read-ProductJson $EvidencePath
    if ($document.format -isnot [int] -or $document.format -ne 2 -or $document.entries -isnot [array]) {
        throw 'Unsupported release evidence format or entries type.'
    }
    $bindings = @{ version = $version; commit = $source.commit; source_sha256 = $source.source_sha256;
        package_manifest_sha256 = $manifest.sha256; build_receipt_sha256 = $receiptHash; package_policy_sha256 = $policyHash;
        acceptance_profile = $AcceptanceProfile; distribution_policy = $manifest.distribution_policy;
        license_decision_sha256 = $licenseDecisionHash; language_policy_sha256 = $languagePolicyHash }
    if (-not $licenseDecisionHash -or -not $languagePolicyHash) { throw 'License/language policy identity unavailable.' }
    foreach ($key in $bindings.Keys) {
        if ($document.$key -isnot [string] -or $document.$key -cne $bindings[$key]) { throw "Release evidence identity mismatch: $key" }
    }
    $ids = @{}
    foreach ($entry in @($document.entries)) {
        if ($entry.id -isnot [string] -or -not $entry.id -or $ids.ContainsKey($entry.id) -or
                $entry.kind -isnot [string]) { throw 'Missing/duplicate evidence id or invalid evidence kind.' }
        $ids[$entry.id] = $true
    }
    if ((Get-Sha256 $EvidencePath) -cne $documentHash) { throw 'Release evidence changed while being read.' }
    $script:evidence = $document
    $script:evidenceHash = $documentHash
    'Evidence is hash/version-bound to this source, tested build, policy and exact package manifest.'
}
