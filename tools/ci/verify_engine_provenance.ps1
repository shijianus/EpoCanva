[CmdletBinding()]
param([string]$RepoRoot = '', [string]$InputRoot = '', [string]$Record = '')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
try {
    if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path }
    if (-not $InputRoot) { $InputRoot = $RepoRoot }
    if (-not $Record) { $Record = Join-Path $PSScriptRoot 'provenance/english-engine-20261125.json' }
    . (Join-Path $PSScriptRoot '../product/build_identity.ps1')
    $recordData = Read-ProductJson $Record
    if ($recordData.format -cne 'epocanva-engine-input-provenance-v1' -or
            $recordData.scope -cne 'reviewed-local-input-recovery' -or
            $recordData.configuration -cne 'Release' -or $recordData.architecture -cne 'x64' -or
            $recordData.references -isnot [array] -or $recordData.references.Count -lt 5) {
        throw 'Unsupported engine provenance record.'
    }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($reference in $recordData.references) {
        Assert-ProductObjectFields $reference @('path', 'sha256', 'bytes')
        Assert-ProductRelativePath $reference.path
        if (-not $seen.Add($reference.path) -or -not (Test-ProductSha256 $reference.sha256) -or
                -not (Test-ProductJsonInteger $reference.bytes) -or $reference.bytes -le 0) { throw 'Invalid engine evidence reference.' }
        $path = Assert-ProductPlainPath (Join-Path $InputRoot $reference.path) $InputRoot
        if ((Get-Item -LiteralPath $path).Length -ne $reference.bytes -or
                (Get-ProductIdentityFileSha256 $path) -cne $reference.sha256) { throw "Changed engine evidence: $($reference.path)" }
    }
    foreach ($required in @('build/librime-english/rime/install/lib/rime.dll', 'build/librime-english/engine-build-receipt.json',
            'build/ci-input-provenance/english-engine-20261125/engine-identity.json',
            'build/ci-input-provenance/english-engine-20261125/build.command.json',
            'build/ci-input-provenance/english-engine-20261125/build.log')) {
        if (-not $seen.Contains($required)) { throw "Missing engine provenance input: $required" }
    }
    $receipt = Read-ProductJson (Join-Path $InputRoot 'build/librime-english/engine-build-receipt.json')
    $identity = Get-ProductEngineIdentity -RepoRoot $RepoRoot
    if (-not (Test-ProductJsonInteger $receipt.format) -or $receipt.format -ne 1 -or
            -not [object]::Equals($receipt.status, 'built') -or
            $receipt.source_sha256 -cne $identity.source_sha256 -or $receipt.source_sha256 -cne $recordData.engine_source_sha256 -or
            $receipt.rime_sha256 -cne $recordData.rime_sha256 -or
            $receipt.rime_sha256 -cne (Get-ProductIdentityFileSha256 (Join-Path $InputRoot 'build/librime-english/rime/install/lib/rime.dll'))) {
        throw 'The engine, current source and provenance receipt do not match.'
    }
    foreach ($flag in @('logging', 'external_plugins', 'tests')) {
        if ($receipt.flags.$flag -isnot [bool] -or $receipt.flags.$flag) { throw 'Engine flags do not match offline Release configuration.' }
    }
    $build = Read-ProductJson (Join-Path $InputRoot 'build/ci-input-provenance/english-engine-20261125/build.command.json')
    if (-not (Test-ProductJsonInteger $build.exit_code) -or $build.exit_code -ne 0 -or
            (ConvertTo-ProductUtc $build.completed_utc) -lt (ConvertTo-ProductUtc $build.started_utc)) { throw 'Engine build did not complete successfully.' }
    $snapshot = Read-ProductJson (Join-Path $InputRoot 'build/ci-input-provenance/english-engine-20261125/engine-identity.json')
    if ($snapshot.source_sha256 -cne $identity.source_sha256 -or $snapshot.inputs -isnot [array] -or
            @($snapshot.inputs).Count -ne @($identity.inputs).Count -or
            (Compare-Object -ReferenceObject $identity.inputs -DifferenceObject $snapshot.inputs -CaseSensitive)) {
        throw 'Current engine inputs differ from the reviewed source inventory.'
    }
    Write-Output 'ENGINE PROVENANCE VERIFIED: current source, receipt, build record and all referenced bytes match.'
    exit 0
} catch {
    Write-Output ('ENGINE PROVENANCE BLOCKED: ' + $_.Exception.Message)
    exit 1
}
