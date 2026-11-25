Add-Gate 'source-identity' {
    $script:source = Get-ProductSourceIdentity -RepoRoot $RepoRoot
    $script:version = (Get-Content -LiteralPath (Join-Path $RepoRoot 'platforms\windows\product\VERSION') -Raw -Encoding UTF8).Trim()
    if (-not $version) { throw 'Product VERSION is empty.' }
    'Current source fingerprint and full commit identity recorded.'
}
if (-not $StageRoot) { $StageRoot = Join-Path $BuildDir "stage\EpoCanva-$version" }
$StageRoot = [IO.Path]::GetFullPath($StageRoot)
Add-Gate 'verified-build' {
    $before = Get-Sha256 $receiptPath
    $verified = Assert-ProductBuildReceipt -RepoRoot $RepoRoot -BuildDir $BuildDir -RimeDll $RimeDll
    if ((Get-Sha256 $receiptPath) -cne $before) { throw 'Build receipt changed during verification.' }
    $script:receiptHash = $before
    $script:receipt = $verified
    'Format 2 Release receipt matches current source, CTest records and tested binaries.'
}
Add-Gate 'engine-identity' {
    if ($null -eq $receipt) { throw 'No verified product build receipt is available for engine binding.' }
    $null = Assert-ProductBuildEngineBinding $RepoRoot $RimeDll $receipt
    'English source, pinned dependency checkouts, disabled logging/plugins and runtime bytes agree.'
}
Add-Gate 'build-tests' {
    if ($null -eq $receipt) { throw 'No valid build receipt; build/test evidence is unavailable.' }
    "$($receipt.tests.passed) passing CTest cases, matched to the original inventory by the receipt verifier."
}
Add-Gate 'source-clean' {
    if ($null -eq $receipt) { throw 'No verified build receipt exists; a fixture package cannot establish the clean source revision.' }
    if ($null -eq $source -or $source.dirty) { throw 'Aggregate acceptance requires a clean source revision; dirty identity is preserved, never stripped.' }
    'Source worktree is clean.'
}
