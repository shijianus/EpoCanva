# Shared, read-only Corresponding Source inventory. The producer and checker must
# use this function, not maintain separate dependency/file enumerators.
Set-StrictMode -Version Latest

function Assert-ProductSourceRelativePath {
    param([Parameter(Mandatory)][string]$Path)
    if ([IO.Path]::IsPathRooted($Path) -or $Path -match '[\x00-\x1f\x7f<>:"\\|?*]' -or
            $Path.StartsWith('/') -or $Path.EndsWith('/')) {
        throw "Invalid source-relative path: $Path"
    }
    foreach ($part in ($Path -split '/')) {
        if (-not $part -or $part -in @('.', '..', '.git') -or $part -match '[. ]$' -or
                $part -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') {
            throw "Invalid source path component: $Path"
        }
    }
}

function Assert-ProductSourcePlainPath {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$RepoRoot,
        [Collections.Generic.HashSet[string]]$VerifiedDirectories = $null
    )
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\', '/')
    $root = [IO.Path]::GetFullPath($RepoRoot).TrimEnd('\', '/')
    if ($full -ine $root -and -not $full.StartsWith($root + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) { throw 'Source path is outside the repository.' }
    # A caller may cache directories only for one source-validation pass. Leaves
    # are always checked afresh; each later phase must use a fresh directory cache.
    $current = $full
    $checkedDirectories = [Collections.Generic.List[string]]::new()
    while ($current) {
        if ($current -ine $full -and $null -ne $VerifiedDirectories -and $VerifiedDirectories.Contains($current)) { break }
        $attributes = $null
        try { $attributes = [IO.File]::GetAttributes($current) }
        catch [IO.FileNotFoundException] { }
        catch [IO.DirectoryNotFoundException] { }
        if ($null -ne $attributes) {
            # .NET attribute reads avoid the PowerShell provider for ordinary
            # files. OneDrive reparse placeholders need the slower LinkType test:
            # they must not be confused with name-surrogate symlinks/junctions.
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
                $link = $item.PSObject.Properties['LinkType']
                $target = $item.PSObject.Properties['Target']
                if (($null -ne $link -and $link.Value) -or
                        ($null -ne $target -and @($target.Value | Where-Object { $_ }).Count -ne 0)) {
                    throw "Source/output path redirects through a link: $full"
                }
            }
            if (($attributes -band [IO.FileAttributes]::Directory) -ne 0) { $checkedDirectories.Add($current) }
        }
        $current = [IO.Path]::GetDirectoryName($current)
    }
    if ($null -ne $VerifiedDirectories) {
        foreach ($directory in $checkedDirectories) { $null = $VerifiedDirectories.Add($directory) }
    }
}

function Get-ProductSourceFileRecord {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string]$RelativePath,
        [Collections.Generic.HashSet[string]]$VerifiedDirectories = $null
    )
    # Optional caller-owned directory cache is valid for this validation phase
    # only. File attributes, link metadata, byte count and hash are never cached.
    Assert-ProductSourceRelativePath $RelativePath
    $path = Join-Path $RepoRoot $RelativePath
    Assert-ProductSourcePlainPath $path $RepoRoot $VerifiedDirectories
    $item = Get-Item -LiteralPath $path -Force -ErrorAction Stop
    if ($item.PSIsContainer) { throw "Required source is a directory: $RelativePath" }
    $stream = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $byteCount = $stream.Length
        $hash = ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
        if ($stream.Length -ne $byteCount) { throw "Source byte count changed: $RelativePath" }
        return [pscustomobject]@{ path = $RelativePath; sha256 = $hash; bytes = [long]$byteCount }
    } finally { $algorithm.Dispose(); $stream.Dispose() }
}

function Invoke-ProductSourceGit {
    param([Parameter(Mandatory)][string]$Directory, [Parameter(Mandatory)][string[]]$Arguments)
    $result = @(& git -c core.quotepath=false -C $Directory @Arguments)
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect source checkout ($($Arguments[0]))." }
    return $result
}

function Assert-ProductSourceCheckout {
    param([string]$RepoRoot, [string]$RelativePath, [string]$Commit)
    Assert-ProductSourceRelativePath $RelativePath
    $path = Join-Path $RepoRoot $RelativePath
    Assert-ProductSourcePlainPath $path $RepoRoot
    if ($Commit -cnotmatch '^[0-9a-f]{40}$') { throw "Invalid pinned source revision: $RelativePath" }
    $top = @(Invoke-ProductSourceGit $path @('rev-parse', '--show-toplevel'))
    if ($top.Count -ne 1 -or [IO.Path]::GetFullPath($top[0]).TrimEnd('\', '/') -ine
            [IO.Path]::GetFullPath($path).TrimEnd('\', '/')) { throw "Not the required source checkout: $RelativePath" }
    $head = @(Invoke-ProductSourceGit $path @('rev-parse', 'HEAD'))
    $status = @(Invoke-ProductSourceGit $path @('status', '--porcelain', '--untracked-files=all'))
    if ($head.Count -ne 1 -or $head[0] -cne $Commit -or $status.Count -ne 0) {
        throw "Pinned dependency is missing, modified or at another revision: $RelativePath"
    }
}

function Get-ProductCorrespondingSourceFiles {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$RepoRoot)
    $root = (Get-Item -LiteralPath $RepoRoot -Force -ErrorAction Stop).FullName.TrimEnd('\', '/')
    Assert-ProductSourcePlainPath $root $root
    $top = @(Invoke-ProductSourceGit $root @('rev-parse', '--show-toplevel'))
    if ($top.Count -ne 1 -or [IO.Path]::GetFullPath($top[0]).TrimEnd('\', '/') -ine $root) {
        throw 'RepoRoot must be the project worktree root.'
    }
    $files = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
    $verifiedDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    function Add-SourcePath([string]$RelativePath) {
        Assert-ProductSourceRelativePath $RelativePath
        if ($files.ContainsKey($RelativePath)) {
            if ($files[$RelativePath] -cne $RelativePath) { throw "Case-colliding source paths: $RelativePath" }
            return
        }
        $full = Join-Path $root $RelativePath
        Assert-ProductSourcePlainPath $full $root $verifiedDirectories
        if (-not [IO.File]::Exists($full)) { throw "Required source is missing: $RelativePath" }
        $files.Add($RelativePath, $RelativePath)
    }
    # Git supplies tracked and current non-ignored project files; tracked deletions
    # are errors. No recursive copy of build/, user data, .git or local caches.
    foreach ($path in @(Invoke-ProductSourceGit $root @('ls-files', '--cached', '--others', '--exclude-standard'))) {
        if ($path -cmatch '^(engine|platforms|tests|tools|data|cmake|third_party)/' -or
                $path -cmatch '^docs/build/') { Add-SourcePath $path }
    }
    foreach ($path in @('CMakeLists.txt', 'CMakePresets.json', '.gitattributes', '.gitignore',
            'LICENSE', 'AGENTS.md', 'README.md', 'docs/build/bootstrap.md',
            'docs/build/librime-windows-baseline.md', 'docs/product/license-decision.md',
            'data/manifests/license-decision.json', 'third_party/sources.lock.json',
            'third_party/README.md', 'data/manifests/english-pilot.sources.lock.json',
            'tools/data/build_english_pilot.py', 'tools/data/build_product_lexicon.py',
            'docs/compliance/english-pilot-data.md', 'docs/compliance/dependency-audit.md',
            'tools/product/source_contract.ps1', 'tools/product/stage_source.ps1')) { Add-SourcePath $path }
    foreach ($directory in @('engine', 'platforms', 'tests', 'tools', 'data')) {
        if (@($files.Keys | Where-Object { $_.StartsWith($directory + '/', [StringComparison]::Ordinal) }).Count -eq 0) {
            throw "Required project source tree is empty: $directory"
        }
    }
    $lock = Get-Content -LiteralPath (Join-Path $root 'third_party/sources.lock.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $rime = @($lock.packages | Where-Object { $_.name -ceq 'librime' })
    if ($rime.Count -ne 1 -or $rime[0].local_path -cne 'third_party/librime') { throw 'Unsupported librime source lock.' }
    $rime = $rime[0]
    $checkouts = @([pscustomobject]@{ path = 'third_party/librime'; commit = $rime.pinned_commit; kind = 'librime' })
    foreach ($name in @('yaml-cpp', 'leveldb', 'marisa-trie', 'opencc')) {
        $sub = @($rime.declared_submodules | Where-Object { $_.path -ceq "deps/$name" })
        if ($sub.Count -ne 1) { throw "Required dependency is not uniquely pinned: $name" }
        $checkouts += [pscustomobject]@{ path = "third_party/librime/deps/$name"; commit = $sub[0].recorded_commit; kind = $name }
    }
    foreach ($checkout in $checkouts) {
        Assert-ProductSourceCheckout $root $checkout.path $checkout.commit
        # ls-files --stage distinguishes regular files from gitlinks/symlinks.
        foreach ($row in @(Invoke-ProductSourceGit (Join-Path $root $checkout.path) @('ls-files', '--stage'))) {
            if ($row -cnotmatch '^(\d{6}) [0-9a-f]{40} 0\t(.+)$') { throw 'Invalid dependency index row.' }
            $mode, $path = $Matches[1], $Matches[2]
            if ($mode -ceq '160000') { continue } # separately pinned dependencies above
            if ($mode -notin @('100644', '100755')) { throw "Non-regular dependency source: $path" }
            if ($checkout.kind -ceq 'librime' -and $path -cmatch '^(data/minimal|sample)/') { continue }
            # These disabled, unrelated OpenCC features are not product inputs.
            if ($checkout.kind -ceq 'opencc' -and
                    $path -cmatch '^deps/(googletest-1\.15\.0|google-benchmark|pybind11-2\.13\.1|marisa-0\.2\.6)/') { continue }
            Add-SourcePath ($checkout.path + '/' + $path)
        }
    }
    # Boost is header-only for this pinned Windows recipe (librime CMakeLists.txt
    # and tools/build_librime_baseline.ps1). Include its complete header closure,
    # not its documentation, node_modules, binaries or 1.5 GB extraction tree.
    $boost = @($rime.other_required_inputs | Where-Object { $_.name -ceq 'boost' })
    if ($boost.Count -ne 1) { throw 'Boost source is not uniquely pinned.' }
    $boostRoot = 'build/librime-baseline/dependencies/boost/boost_1_89_0'
    if ($boost[0].acquisition.extracted_to -notlike "$boostRoot*") { throw 'Unexpected Boost source location.' }
    foreach ($path in @('LICENSE_1_0.txt', 'boost/version.hpp', 'boost/config.hpp', 'boost/regex.hpp',
            'boost/interprocess/file_mapping.hpp', 'boost/signals2/signal.hpp')) { Add-SourcePath "$boostRoot/$path" }
    $version = Get-Content -LiteralPath (Join-Path $root "$boostRoot/boost/version.hpp") -Raw -Encoding UTF8
    if ($version -notmatch '#define\s+BOOST_VERSION\s+108900\b') { throw 'Boost headers are not version 1.89.0.' }
    $boostLicense = Get-ProductSourceFileRecord $root "$boostRoot/LICENSE_1_0.txt"
    if ($boostLicense.sha256 -cne $boost[0].license_evidence.sha256) { throw 'Boost licence differs from the source lock.' }
    $queue = [Collections.Generic.Queue[string]]::new()
    $queue.Enqueue((Join-Path $root "$boostRoot/boost"))
    while ($queue.Count -gt 0) {
        $directory = $queue.Dequeue()
        foreach ($full in [IO.Directory]::EnumerateFileSystemEntries($directory)) {
            $attributes = [IO.File]::GetAttributes($full)
            if (($attributes -band [IO.FileAttributes]::Directory) -ne 0) {
                Assert-ProductSourcePlainPath $full $root $verifiedDirectories
                $queue.Enqueue($full)
            } else { Add-SourcePath $full.Substring($root.Length + 1).Replace('\', '/') }
        }
    }
    $dataLock = Get-Content -LiteralPath (Join-Path $root 'data/manifests/english-pilot.sources.lock.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $dataRoot = 'build/third-party-fetch/esdb'
    if ($dataLock.checkout_verification.default_cache_dir -cne $dataRoot) { throw 'Unexpected word-list source location.' }
    Assert-ProductSourceCheckout $root $dataRoot $dataLock.pinned_commit
    $dataInputs = @($dataLock.inputs) + @($dataLock.licence_evidence_files | Where-Object {
        $_.repo_path -cin @('alt12dicts/README', 'alt12dicts/README-orig')
    })
    if ($dataInputs.Count -ne 3 -or @($dataLock.inputs).Count -ne 1 -or
            $dataLock.inputs[0].repo_path -cne 'alt12dicts/2of12full.txt') { throw 'Unsupported word-list input scope.' }
    foreach ($inputFile in $dataInputs) {
        $relative = "$dataRoot/$($inputFile.repo_path)"
        Add-SourcePath $relative
        $actual = Get-ProductSourceFileRecord $root $relative
        if ($actual.sha256 -cne $inputFile.sha256 -or $actual.bytes -ne $inputFile.size_bytes) {
            throw "Pinned word-list source differs: $relative"
        }
    }
    # The current package policy also pins generated product data/provenance and
    # notice bytes. Read those explicit sources only, never enumerate build/.
    $policy = Join-Path $root 'data/manifests/product-package-files.inc'
    foreach ($line in Get-Content -LiteralPath $policy -Encoding UTF8) {
        if (-not $line.Trim() -or $line.TrimStart().StartsWith('//')) { continue }
        if ($line -cnotmatch '^EPOCANVA_PACKAGE_FILE\("([^\"]+)",\s*"([^\"]*)",\s*"([^\"]*)",\s*(\d+)\)$') {
            throw 'Invalid product payload policy.'
        }
        $origin, $hash, $byteCount = $Matches[2], $Matches[3], [long]$Matches[4]
        if (-not $origin) { continue }
        if ($origin.StartsWith('build/', [StringComparison]::Ordinal) -and
                $origin -cnotin @('build/product-data/epocanva_en_alpha.tsv',
                    'build/product-data/epocanva_en_alpha.provenance.txt', "$boostRoot/LICENSE_1_0.txt",
                    "$dataRoot/alt12dicts/README", "$dataRoot/alt12dicts/README-orig")) {
            throw "Unreviewed generated source location: $origin"
        }
        Add-SourcePath $origin
        $actual = Get-ProductSourceFileRecord $root $origin
        if ($hash -cnotmatch '^[0-9a-f]{64}$' -or $byteCount -le 0 -or
                $actual.sha256 -cne $hash -or $actual.bytes -ne $byteCount) {
            throw "Required package source differs from current policy: $origin"
        }
    }
    foreach ($path in @('build/product-data/epocanva_en_alpha.tsv', 'build/product-data/epocanva_en_alpha.provenance.txt')) {
        if (-not $files.ContainsKey($path)) { throw "Required product data source is not pinned: $path" }
    }
    [string[]]$result = @($files.Values)
    [Array]::Sort($result, [StringComparer]::Ordinal)
    return $result
}
