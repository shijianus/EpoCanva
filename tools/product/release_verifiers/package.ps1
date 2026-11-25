Add-Gate 'package-policy' {
    $parsed = @{}
    foreach ($line in (Get-Content -LiteralPath $policyPath -Encoding UTF8)) {
        if (-not $line.Trim() -or $line.TrimStart().StartsWith('//')) { continue }
        if ($line -cnotmatch '^EPOCANVA_PACKAGE_FILE\("([^"]+)",\s*"([^"]*)",\s*"([^"]*)",\s*(\d+)\)$') {
            throw 'Unrecognised shared payload policy row.'
        }
        $relative, $origin, $sha, $bytes = $Matches[1], $Matches[2], $Matches[3], $Matches[4]
        Assert-RelativePath $relative
        if ($relative -eq 'package.manifest' -or $parsed.ContainsKey($relative)) { throw 'Duplicate or self-referential payload policy row.' }
        if ($origin) {
            Assert-RelativePath $origin
            if ($sha -cnotmatch '^[0-9a-f]{64}$' -or [uint64]$bytes -eq 0) { throw 'Fixed payload policy lacks a hash/byte count.' }
        } elseif ($relative -notin @('EpoCanva.exe', 'epocanva_tsf.dll', 'rime.dll', 'VERSION.txt') -or $sha -or [uint64]$bytes -ne 0) {
            throw 'Only the three receipt-bound binaries and generated VERSION.txt may have an empty source.'
        }
        $parsed[$relative] = [pscustomobject]@{ path = $relative; source = $origin; sha256 = $sha; bytes = [uint64]$bytes }
    }
    if ($parsed.Count -eq 0) { throw 'Shared payload policy is empty.' }
    $script:policy = $parsed
    $script:policyHash = Get-Sha256 $policyPath
    "$($policy.Count) payload files required by the shared policy; no hardcoded file count."
}
Add-Gate 'package-integrity' {
    if ($policy.Count -eq 0) { throw 'Shared payload policy is unavailable.' }
    Assert-PlainPath $StageRoot $StageRoot
    $manifestPath = Join-Path $StageRoot 'package.manifest'
    Assert-PlainPath $manifestPath $StageRoot
    $lines = @(Get-Content -LiteralPath $manifestPath -Encoding UTF8 | Where-Object { $_ -and -not $_.StartsWith('#') })
    if ($lines.Count -lt 5 -or $lines[0] -cnotmatch '^epocanva-package-manifest [12]$') {
        throw 'Missing supported package manifest.'
    }
    $format = [int]$lines[0].Substring($lines[0].Length - 1)
    $metadata = @{}
    $files = @{}
    foreach ($line in ($lines | Select-Object -Skip 1)) {
        if ($line -cmatch '^(version|commit|internal-only|acceptance-profile|distribution-policy|path-exception)=(.+)$') {
            if ($metadata.ContainsKey($Matches[1])) { throw 'Duplicate manifest metadata.' }
            $metadata[$Matches[1]] = $Matches[2]
        } elseif ($line -cmatch '^file\t([^\t]+)\t([0-9a-f]{64})\t(\d+)$') {
            $relative, $hash, $bytes = $Matches[1], $Matches[2], [uint64]$Matches[3]
            Assert-RelativePath $relative
            if ($files.ContainsKey($relative) -or -not $policy.ContainsKey($relative) -or $relative -cne $policy[$relative].path) {
                throw 'Duplicate or non-whitelisted manifest payload.'
            }
            $filePath = Join-Path $StageRoot $relative
            Assert-PlainPath $filePath $StageRoot
            $file = Get-Item -LiteralPath $filePath -Force
            if ($file.PSIsContainer -or $file.Length -ne $bytes -or (Get-Sha256 $filePath) -cne $hash) {
                throw "Staged hash/byte count mismatch: $relative"
            }
            $locked = $policy[$relative]
            if ($locked.source -and ($hash -cne $locked.sha256 -or $bytes -ne $locked.bytes)) {
                throw "Staged file differs from the fixed source policy: $relative"
            }
            $files[$relative] = [pscustomobject]@{ sha256 = $hash; bytes = $bytes }
        } else { throw 'Unrecognised package manifest row.' }
    }
    if (-not $metadata['version'] -or -not $metadata['commit'] -or $files.Count -ne $policy.Count) {
        throw 'Manifest requires unique version/commit and the complete shared payload set.'
    }
    if ($format -eq 1) {
        if ($metadata.Count -ne 3 -or $metadata['internal-only'] -cnotmatch '^[01]$') {
            throw 'Legacy manifest requires only version/commit/internal-only metadata.'
        }
    } elseif ($metadata.Count -ne 5 -or
            $metadata['acceptance-profile'] -cnotin @('internal-unsigned', 'public-signed') -or
            $metadata['distribution-policy'] -cnotin @('internal-only', 'public-candidate') -or
            $metadata['path-exception'] -cnotin @('none', 'embedded-build-paths')) {
        throw 'Format 2 requires separate acceptance-profile/distribution-policy/path-exception values.'
    }
    $directories = [System.Collections.Generic.Queue[string]]::new()
    $directories.Enqueue($StageRoot)
    $actualCount = 0
    while ($directories.Count -gt 0) {
        foreach ($item in (Get-ChildItem -LiteralPath $directories.Dequeue() -Force)) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -and
                    ($item.LinkType -or @($item.Target).Count -gt 0)) { throw 'Package contains a symbolic link/junction.' }
            if ($item.PSIsContainer) { $directories.Enqueue($item.FullName); continue }
            $relative = $item.FullName.Substring($StageRoot.TrimEnd('\', '/').Length + 1).Replace('\', '/')
            if ($relative -ceq 'package.manifest') { continue }
            if (-not $files.ContainsKey($relative) -or $relative -cne $policy[$relative].path) { throw "Extra staged file: $relative" }
            ++$actualCount
        }
    }
    if ($actualCount -ne $files.Count) { throw 'Actual payload set differs from the manifest.' }
    $script:manifest = [pscustomobject]@{ format = $format; version = $metadata['version']; commit = $metadata['commit'];
        acceptance_profile = $metadata['acceptance-profile']; distribution_policy = $metadata['distribution-policy'];
        path_exception = $metadata['path-exception']; sha256 = (Get-Sha256 $manifestPath); files = $files }
    "$actualCount payload files verified; no extras."
}
Add-Gate 'package-identity' {
    if ($null -eq $manifest -or $null -eq $source -or $null -eq $receipt) { throw 'Verified manifest/source/build identity is unavailable.' }
    if ($manifest.version -cne $version -or $manifest.commit -cne $source.commit -or $manifest.commit -cne $receipt.source.commit) {
        throw 'Staged version/commit does not match current source and the tested build.'
    }
    $versionLines = @(Get-Content -LiteralPath (Join-Path $StageRoot 'VERSION.txt') -Encoding UTF8)
    foreach ($pair in @(@('Version', $version), @('Commit', $source.commit),
            @('Acceptance-profile', $manifest.acceptance_profile), @('Distribution-policy', $manifest.distribution_policy),
            @('Path-exception', $manifest.path_exception))) {
        $rows = @($versionLines | Where-Object { $_.StartsWith($pair[0] + ':') })
        if ($rows.Count -ne 1 -or $rows[0] -cne ($pair[0] + ': ' + $pair[1])) { throw 'VERSION.txt identity mismatch or duplicate field.' }
    }
    $names = @{ exe = 'EpoCanva.exe'; tsf = 'epocanva_tsf.dll'; rime = 'rime.dll' }
    foreach ($name in $names.Keys) {
        $binary = @($receipt.binaries | Where-Object { $_.name -eq $name })
        $staged = $manifest.files[$names[$name]]
        if ($binary.Count -ne 1 -or $staged.sha256 -cne $binary[0].sha256 -or $staged.bytes -ne $binary[0].bytes) {
            throw "Staged binary differs from tested bytes: $name"
        }
    }
    'VERSION.txt, manifest, current source and three tested binaries agree.'
}
Add-Gate 'acceptance-profile' {
    if ($null -eq $manifest -or $manifest.format -ne 2) { throw 'Legacy format 1 is diagnostic-only; restage with explicit format 2 policies.' }
    if ($manifest.acceptance_profile -cne $AcceptanceProfile) { throw 'Requested acceptance profile differs from the staged artifact.' }
    if ($manifest.path_exception -cne 'none') { throw 'Embedded build-path exception prevents technical acceptance for either profile.' }
    if ($AcceptanceProfile -ceq 'public-signed' -and $manifest.distribution_policy -cne 'public-candidate') {
        throw 'Public acceptance requires an explicit public-candidate package.'
    }
    "Format 2 acceptance profile: $AcceptanceProfile; path-exception=none. Distribution authorization is separate."
}
Add-Gate 'public-package' {
    if ($null -eq $manifest -or $manifest.format -ne 2 -or $manifest.distribution_policy -cne 'public-candidate') {
        throw 'No format-2 public-distribution candidate; internal-only scope does not authorize public release.'
    }
    'Explicit public candidate metadata verified; this is not owner/legal approval.'
} 'distribution'
Add-Gate 'project-license-selection' {
    $path = Join-Path $RepoRoot 'data\manifests\license-decision.json'
    $script:licenseDecisionHash = Get-Sha256 $path
    $decision = Read-ProductJson $path
    if ($decision.format -isnot [int] -or $decision.format -ne 1 -or
            $decision.record_kind -cne 'user-instruction-scope' -or $decision.project_license -cne 'AGPL-3.0-only' -or
            $decision.acceptance_profile -cne 'internal-unsigned' -or $decision.distribution_policy -cne 'internal-only' -or
            $decision.public_distribution -cne 'not-authorized-by-this-record' -or
            $decision.lexicon_jurisdictions -cne 'not-decided-by-this-record') {
        throw 'License decision must record only the actual AGPL-only/internal unsigned instruction, not invented public or data approvals.'
    }
    $rootLicense = Join-Path $RepoRoot 'LICENSE'
    if ((Get-Item -LiteralPath $rootLicense).Length -ne 34523 -or
            (Get-Sha256 $rootLicense) -cne '0d96a4ff68ad6d4b6f1f30f713b18d5184912ba8dd389f86aa7710db079abcb0' -or
            $decision.license_text.path -cne 'LICENSE' -or $decision.license_text.bytes -isnot [int] -or
            $decision.license_text.bytes -ne 34523 -or $decision.license_text.sha256 -cne (Get-Sha256 $rootLicense)) {
        throw 'Root LICENSE or the recorded actual GNU license-text identity differs.'
    }
    if ($null -eq $manifest -or -not $manifest.files.ContainsKey('LICENSE') -or
            -not $manifest.files.ContainsKey('license-decision.json') -or
            $manifest.files['LICENSE'].sha256 -cne (Get-Sha256 $rootLicense) -or
            $manifest.files['license-decision.json'].sha256 -cne $licenseDecisionHash) { throw 'Package does not carry the exact project license and decision scope.' }
    'AGPL-3.0-only and internal unsigned scope recorded; no public/data approval inferred.'
}
Add-Gate 'package-path-hygiene' {
    if ($null -eq $manifest) { throw 'No verified payload to inspect.' }
    foreach ($relative in $manifest.files.Keys) {
        $bytes = [IO.File]::ReadAllBytes((Join-Path $StageRoot $relative))
        foreach ($encoding in @([Text.Encoding]::UTF8, [Text.Encoding]::Unicode)) {
            if ($encoding.GetString($bytes) -match '(?i)[A-Z]:[\\/](?:Users|Documents and Settings)[\\/]|OneDrive[\\/]') {
                throw "Embedded personal/build path in payload: $relative (matched text is not logged)."
            }
        }
    }
    'No recognised personal build-path pattern in payload bytes; this is not a general privacy proof.'
}

Add-Gate 'language-readiness' {
    $script:languagePolicyHash = Get-Sha256 $languagePolicyPath
    $languages = Read-ProductJson $languagePolicyPath
    $required = @('en', 'zh', 'fr', 'ja', 'ko')
    if ($languages.format -isnot [int] -or $languages.format -ne 1 -or
            $languages.required_languages -isnot [array] -or $languages.languages -isnot [array] -or
            ($languages.required_languages -join ',') -cne ($required -join ',') -or
            @($languages.languages).Count -ne 5) { throw 'Language policy must require exactly en,zh,fr,ja,ko in priority order.' }
    foreach ($id in $required) {
        $rows = @($languages.languages | Where-Object { $_.id -ceq $id })
        if ($rows.Count -ne 1) { throw "Missing or duplicate language policy: $id" }
        $row = $rows[0]
        if ($row.status -cne 'implemented' -or $row.data_status -cne 'packaged') {
            throw "Language $id is not implemented with actual packaged data; core-only or absent data is blocked."
        }
        if ($null -eq $manifest -or $row.payload_paths -isnot [array] -or @($row.payload_paths).Count -eq 0 -or
                $row.source_paths -isnot [array] -or @($row.source_paths).Count -eq 0) {
            throw "Language $id lacks verified payload paths and source references."
        }
        foreach ($relative in $row.payload_paths) {
            Assert-RelativePath $relative
            if (-not $manifest.files.ContainsKey($relative) -or -not $policy.ContainsKey($relative) -or
                    -not $policy[$relative].source) { throw "Language $id data is not in the fixed product payload policy." }
        }
        foreach ($reference in $row.source_paths) { $null = Read-Reference $reference -CorrespondingSource }
    }
    'Five language implementations, exact source references and fixed data payloads present; executed acceptance is checked separately.'
}
