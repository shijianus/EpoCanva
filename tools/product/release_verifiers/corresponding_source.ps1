Add-Gate 'corresponding-source' {
    if (-not $sourceContractLoaded -or -not (Get-Command Get-ProductCorrespondingSourceFiles -ErrorAction SilentlyContinue)) {
        throw 'tools/product/source_contract.ps1 is missing; Corresponding Source cannot be accepted.'
    }
    if ($null -eq $evidence -or $null -eq $source -or $null -eq $manifest -or $null -eq $receipt) {
        throw 'Corresponding Source requires current evidence/source/package/build identities.'
    }
    $sourceManifestPath = Read-Reference $evidence.source_manifest
    if ($sourceManifestPath -cne (Join-Path $BuildDir 'source-manifest.json')) {
        throw 'Source manifest must be the producer output at BuildDir/source-manifest.json.'
    }
    $script:sourceManifestHash = Get-Sha256 $sourceManifestPath
    $document = Read-ProductJson $sourceManifestPath
    if ($document.format -isnot [int] -or $document.format -ne 1 -or $document.files -isnot [array]) {
        throw 'Unsupported corresponding-source manifest.'
    }
    $bindings = @{ version = $version; commit = $source.commit; source_sha256 = $source.source_sha256;
        package_manifest_sha256 = $manifest.sha256; build_receipt_sha256 = $receiptHash; package_policy_sha256 = $policyHash }
    foreach ($key in $bindings.Keys) {
        if ($document.$key -isnot [string] -or $document.$key -cne $bindings[$key]) { throw "Corresponding-source identity mismatch: $key" }
    }
    $archivePath = Read-Reference $document.archive -CorrespondingSource
    if ($archivePath -cne (Join-Path $BuildDir 'corresponding-source.zip')) { throw 'Archive must be the producer output at BuildDir/corresponding-source.zip.' }
    $required = @(Get-ProductCorrespondingSourceFiles -RepoRoot $RepoRoot)
    $expected = @{}
    foreach ($relative in $required) {
        Assert-ProductSourceRelativePath $relative
        if ($expected.ContainsKey($relative)) { throw 'Source contract contains a duplicate path.' }
        $expected[$relative] = $relative
    }
    if ($expected.Count -eq 0 -or $document.files.Count -ne $expected.Count) { throw 'Corresponding-source file set is incomplete.' }
    $listed = @{}
    $sourceDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $document.files) {
        Assert-ProductSourceRelativePath $entry.path
        if ($listed.ContainsKey($entry.path) -or -not $expected.ContainsKey($entry.path) -or
                $entry.path -cne $expected[$entry.path] -or $entry.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
                [string]$entry.bytes -notmatch '^\d+$') { throw 'Invalid, unrequired or duplicate corresponding-source entry.' }
        $path = Join-Path $RepoRoot $entry.path
        $actual = Get-ProductSourceFileRecord -RepoRoot $RepoRoot -RelativePath $entry.path -VerifiedDirectories $sourceDirectories
        if ($actual.bytes -ne [uint64]$entry.bytes -or $actual.sha256 -cne $entry.sha256) {
            throw 'Corresponding-source manifest differs from current source bytes.'
        }
        $listed[$entry.path] = $entry
        $script:references.Add([pscustomobject]@{ path = $path; sha256 = $entry.sha256; bytes = $entry.bytes })
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        if ($zip.Entries.Count -ne $listed.Count) { throw 'Source archive entry count differs from source manifest.' }
        $seen = @{}
        foreach ($entry in $zip.Entries) {
            $relative = $entry.FullName
            Assert-ProductSourceRelativePath $relative
            if ($seen.ContainsKey($relative) -or -not $listed.ContainsKey($relative) -or
                    $relative -cne $listed[$relative].path -or $entry.Length -ne [uint64]$listed[$relative].bytes -or
                    (($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000 -or
                    ($entry.ExternalAttributes -band 0x410) -ne 0) { throw 'Invalid, linked, extra or duplicate source archive entry.' }
            $stream = $entry.Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $hash = ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
            finally { $stream.Dispose(); $sha.Dispose() }
            if ($hash -cne $listed[$relative].sha256) { throw 'Source archive entry digest mismatch.' }
            $seen[$relative] = $true
        }
    } finally { $zip.Dispose() }
    "$($listed.Count) actual source archive entries match the complete contract, current source and exact tested package."
}
