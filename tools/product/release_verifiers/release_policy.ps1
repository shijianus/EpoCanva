$blockers = @($checks | Where-Object { $_.status -eq 'blocked' })
$technicalReady = @($checks | Where-Object { $_.category -eq 'technical' -and $_.status -ne 'passed' }).Count -eq 0
$distributionAuthorized = @($checks | Where-Object { $_.category -eq 'distribution' -and $_.status -ne 'passed' }).Count -eq 0
$signatureVerified = Test-GatePassed 'signing'
$releaseReady = $technicalReady -and $distributionAuthorized -and $signatureVerified -and $AcceptanceProfile -ceq 'public-signed'
$acceptanceReady = if ($AcceptanceProfile -ceq 'internal-unsigned') { $technicalReady } else { $releaseReady }
$acceptanceBlockers = @($blockers | Where-Object { $AcceptanceProfile -ceq 'public-signed' -or $_.category -ceq 'technical' })
$exitCode = if ($acceptanceReady) { 0 } else { 1 }
$report = [ordered]@{
    format = 2; checked_utc = [DateTimeOffset]::UtcNow.ToString('o'); scope = 'windows-aggregate-acceptance'
    mode = 'read-only-files'; status = 'completed'; acceptance_profile = $AcceptanceProfile
    distribution_policy = $(if ($null -ne $manifest) { $manifest.distribution_policy } else { '' })
    acceptance_ready = $acceptanceReady; technical_ready = $technicalReady
    distribution_authorized = $distributionAuthorized; signature_verified = $signatureVerified
    signing_status = $(if ($signatureVerified) { 'verified' } elseif (@($checks | Where-Object { $_.id -ceq 'signing' -and $_.status -ceq 'not_required' }).Count -eq 1) { 'not_required' } else { 'blocked' })
    release_ready = $releaseReady; exit_code = $exitCode
    version = $version; source = $source; package_policy_sha256 = $policyHash
    package_manifest_sha256 = $(if ($null -ne $manifest) { $manifest.sha256 } else { '' })
    build_receipt_sha256 = $receiptHash; license_decision_sha256 = $licenseDecisionHash
    language_policy_sha256 = $languagePolicyHash; source_manifest_sha256 = $sourceManifestHash
    expected_payload_files = $policy.Count
    tools_on_path = $tools; checks = $checks.ToArray(); blockers = $blockers; acceptance_blockers = $acceptanceBlockers
}
