[CmdletBinding()]
param(
    [string]$IdentityRepoRoot = '',
    [switch]$PrintCommit
)

Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'evidence_contract.ps1')
. (Join-Path $PSScriptRoot 'test_inventory_contract.ps1')

function Get-ProductIdentityFileSha256 {
    param([Parameter(Mandatory)][string]$Path)
    $ErrorActionPreference = 'Stop'
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = $null
    try {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        if ($null -ne $algorithm) { $algorithm.Dispose() }
        $stream.Dispose()
    }
}

function Get-ProductOrdinalPaths([string[]]$Paths) {
    $unique = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $windows = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($path in $Paths) {
        if ($unique.Add($path) -and -not $windows.Add($path)) { throw 'Git paths collide on Windows.' }
    }
    [string[]]$sorted = @($unique)
    [Array]::Sort($sorted, [StringComparer]::Ordinal)
    return $sorted
}

function Get-ProductSourceIdentity {
    param([Parameter(Mandatory)][string]$RepoRoot)
    # Never inherit a caller's Continue/Ignore policy for identity failures.
    $ErrorActionPreference = 'Stop'
    $root = (Get-Item -LiteralPath $RepoRoot).FullName
    $head = (& git -C $root rev-parse HEAD)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot determine source HEAD.' }
    if ($head -isnot [string] -or $head -cnotmatch '\A[0-9a-f]{40}\z') { throw 'Invalid source HEAD.' }
    $status = @(& git -C $root status --porcelain --untracked-files=all)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot determine source worktree state.' }
    $paths = @(& git -c core.quotepath=false -C $root ls-files --cached --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate source inputs.' }
    $paths = @(Get-ProductOrdinalPaths $paths)
    $inputs = @($paths | Where-Object {
        ($_ -match '^(engine|platforms|tests|tools|data|cmake|third_party)/' -or
         $_ -match '^(CMakeLists\.txt|CMakePresets\.json|\.gitattributes|LICENSE|AGENTS\.md)$' -or
         $_ -match '^docs/product/license-decision\.md$') -and
        $_ -notmatch '^tools/product/isolation/'
    })
    $rows = [System.Collections.Generic.List[string]]::new()
    $repositoryRows = [System.Collections.Generic.List[string]]::new()
    # Product content and complete repository content have different scopes.
    # Workflows/docs are intentionally outside the runtime fingerprint, but
    # must still invalidate build/acceptance evidence when their bytes change.
    $fileHashes = @{}
    $hashedDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($relative in $paths) {
        Assert-ProductRelativePath $relative
        $path = Join-Path $root $relative
        $directory = [IO.Path]::GetDirectoryName($path)
        if ($hashedDirectories.Add($directory)) { $null = Assert-ProductPlainPath $directory $root }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing repository input: $relative" }
        if (([IO.File]::GetAttributes($path) -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            $null = Assert-ProductPlainPath $path $root
        }
        $hash = Get-ProductIdentityFileSha256 -Path $path
        if ($hash -isnot [string] -or $hash -cnotmatch '^[0-9a-f]{64}$') { throw 'Repository file digest is absent or invalid.' }
        $fileHashes[$relative] = $hash
        $repositoryRows.Add("$relative`t$hash")
    }
    foreach ($relative in $inputs) {
        $hash = $fileHashes[$relative]
        if ($hash -isnot [string] -or $hash -cnotmatch '^[0-9a-f]{64}$') { throw 'Source file digest is absent or invalid.' }
        $rows.Add("$relative`t$hash")
    }
    if ($rows.Count -eq 0) { throw 'Source input list is empty.' }
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes(($rows -join "`n") + "`n")
        $digest = ([BitConverter]::ToString($algorithm.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
        $repositoryBytes = [Text.Encoding]::UTF8.GetBytes(($repositoryRows -join "`n") + "`n")
        $repositoryDigest = ([BitConverter]::ToString($algorithm.ComputeHash($repositoryBytes))).Replace('-', '').ToLowerInvariant()
    } finally { $algorithm.Dispose() }
    $dirty = $status.Count -ne 0
    $commit = $head.Trim()
    if ($dirty) { $commit += '+dirty.' + $digest.Substring(0, 12) }
    return [pscustomobject]@{
        head = $head.Trim()
        commit = $commit
        source_sha256 = $digest
        repository_sha256 = $repositoryDigest
        dirty = $dirty
        source_files = $rows.Count
    }
}

function Get-ProductEngineIdentity {
    param([Parameter(Mandatory)][string]$RepoRoot)
    $ErrorActionPreference = 'Stop'
    $root = (Get-Item -LiteralPath $RepoRoot).FullName
    $paths = @(& git -c core.quotepath=false -C $root ls-files --cached --others --exclude-standard -- engine tools/cmake tools/build_librime_baseline.ps1 tools/product/build_identity.ps1 tools/product/evidence_contract.ps1 tools/product/test_inventory_contract.ps1 third_party/sources.lock.json)
    if ($LASTEXITCODE -ne 0 -or $paths.Count -eq 0) { throw 'Cannot enumerate engine source inputs.' }
    $rows = [System.Collections.Generic.List[string]]::new()
    foreach ($relative in (Get-ProductOrdinalPaths $paths)) {
        Assert-ProductRelativePath $relative
        $file = Assert-ProductPlainPath (Join-Path $root $relative) $root
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing engine source: $relative" }
        $hash = Get-ProductIdentityFileSha256 -Path $file
        if ($hash -isnot [string] -or $hash -cnotmatch '^[0-9a-f]{64}$') { throw 'Engine file digest is absent or invalid.' }
        $rows.Add("$relative`t$hash")
    }
    $upstream = Join-Path $root 'third_party\librime'
    $lockPath = Assert-ProductPlainPath (Join-Path $root 'third_party/sources.lock.json') $root
    $lock = Read-ProductJson $lockPath
    $librime = @($lock.packages | Where-Object { [object]::Equals($_.name, 'librime') })
    if ($librime.Count -ne 1) { throw 'Source lock lacks a unique librime pin.' }
    $pins = [ordered]@{ '' = $librime[0].pinned_commit }
    foreach ($dependency in @('deps/yaml-cpp', 'deps/leveldb', 'deps/marisa-trie', 'deps/opencc')) {
        $entries = @($librime[0].declared_submodules | Where-Object { [object]::Equals($_.path, $dependency) })
        if ($entries.Count -ne 1) { throw "Source lock lacks a unique dependency pin: $dependency" }
        $pins[$dependency] = $entries[0].recorded_commit
    }
    foreach ($relative in $pins.Keys) {
        $checkout = $upstream
        if ($relative) { $checkout = Join-Path $upstream $relative }
        $checkout = Assert-ProductPlainPath $checkout $root
        $top = & git -C $checkout rev-parse --show-toplevel
        if ($LASTEXITCODE -ne 0 -or $top -isnot [string] -or [IO.Path]::GetFullPath($top) -ine $checkout) {
            throw "Missing independent pinned dependency checkout: $relative"
        }
        $head = & git -C $checkout rev-parse HEAD
        if ($LASTEXITCODE -ne 0 -or $head -isnot [string] -or $head -cnotmatch '\A[0-9a-f]{40}\z') {
            throw "Cannot determine pinned dependency: $relative"
        }
        if (-not [object]::Equals($pins[$relative], $head)) { throw "Dependency HEAD differs from its source lock: $relative" }
        $dirty = @(& git -C $checkout status --porcelain --untracked-files=all)
        if ($LASTEXITCODE -ne 0 -or $dirty.Count -ne 0) { throw "Dependency checkout is not clean: $relative" }
        $rows.Add("librime/$relative`t$($head.Trim())")
    }
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes(($rows -join "`n") + "`n")
        $digest = ([BitConverter]::ToString($algorithm.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally { $algorithm.Dispose() }
    return [pscustomobject]@{ source_sha256 = $digest; inputs = $rows.ToArray() }
}

function Assert-ProductEngineReceipt {
    param([Parameter(Mandatory)][string]$RepoRoot, [Parameter(Mandatory)][string]$RimeDll)
    $ErrorActionPreference = 'Stop'
    $path = Assert-ProductPlainPath (Join-Path $RepoRoot 'build\librime-english\engine-build-receipt.json') $RepoRoot
    $receipt = Read-ProductJson $path
    $identity = Get-ProductEngineIdentity -RepoRoot $RepoRoot
    $hash = Get-ProductIdentityFileSha256 -Path $RimeDll
    if (-not (Test-ProductJsonInteger $receipt.format) -or $receipt.format -ne 1 -or
        -not [object]::Equals($receipt.status, 'built') -or
        -not (Test-ProductSha256 $receipt.source_sha256) -or $receipt.source_sha256 -cne $identity.source_sha256 -or
        -not (Test-ProductSha256 $receipt.rime_sha256) -or $receipt.rime_sha256 -cne $hash -or
        $receipt.flags.logging -isnot [bool] -or $receipt.flags.external_plugins -isnot [bool] -or
        $receipt.flags.tests -isnot [bool] -or
        $receipt.flags.logging -ne $false -or $receipt.flags.external_plugins -ne $false -or
        $receipt.flags.tests -ne $false) {
        throw 'English engine source/configuration does not match its build receipt. Rebuild english-completion.'
    }
    return $receipt
}

function New-ProductBinaryRecord([string]$Name, [string]$Path) {
    $file = Get-Item -LiteralPath $Path -Force -ErrorAction Stop
    if ($file.PSIsContainer -or $file.Length -le 0) { throw "Missing or empty tested binary: $Name" }
    return [pscustomobject]@{ name = $Name; path = $file.FullName; bytes = $file.Length
        sha256 = Get-ProductIdentityFileSha256 -Path $file.FullName }
}

function Assert-ProductBinaryRecord($Binding, [string]$Name, [string]$Path) {
    Assert-ProductObjectFields $Binding @('name', 'path', 'bytes', 'sha256')
    $actual = New-ProductBinaryRecord $Name $Path
    if (-not [object]::Equals($Binding.name, $Name) -or $Binding.path -isnot [string] -or
            $Binding.path -ine $actual.path -or -not (Test-ProductJsonInteger $Binding.bytes) -or
            $Binding.bytes -ne $actual.bytes -or -not (Test-ProductSha256 $Binding.sha256) -or
            $Binding.sha256 -cne $actual.sha256) {
        throw "Binary changed after product tests: $Name"
    }
}

function Assert-ProductBuildReceipt {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string]$BuildDir,
        [Parameter(Mandatory)][string]$RimeDll
    )
    $ErrorActionPreference = 'Stop'
    $BuildDir = Assert-ProductPlainPath $BuildDir (Join-Path $RepoRoot 'build')
    $path = Assert-ProductPlainPath (Join-Path $BuildDir 'product-build-receipt.json') $BuildDir
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw 'No successful product build receipt. Run tools/product/build_product.ps1 with tests.'
    }
    $receipt = Read-ProductJson $path
    if (-not (Test-ProductJsonInteger $receipt.format) -or $receipt.format -ne 2 -or
        -not [object]::Equals($receipt.status, 'verified') -or
        -not [object]::Equals($receipt.configuration, 'Release') -or $receipt.trace -isnot [bool] -or $receipt.trace -ne $false -or
        -not (Test-ProductJsonInteger $receipt.build_exit) -or -not (Test-ProductJsonInteger $receipt.ctest_exit) -or
        $receipt.build_exit -ne 0 -or $receipt.ctest_exit -ne 0) {
        throw 'Product build receipt is not a verified non-trace Release build.'
    }
    Assert-ProductObjectFields $receipt @('format', 'status', 'configuration', 'trace', 'source', 'build_exit',
        'ctest_exit', 'evidence', 'engine_source_sha256', 'engine_receipt_sha256', 'cache_sha256', 'ctest_list_sha256',
        'ctest_xml_sha256', 'commands_sha256', 'tests', 'binaries', 'test_binaries')
    if (-not (Test-ProductSha256 $receipt.engine_source_sha256) -or -not (Test-ProductSha256 $receipt.engine_receipt_sha256)) {
        throw 'Build lacks the English engine source and receipt digests.'
    }
    $cachePath = Assert-ProductPlainPath (Join-Path $BuildDir 'CMakeCache.txt') $BuildDir
    $cacheHash = Get-ProductIdentityFileSha256 -Path $cachePath
    $cache = @(Get-Content -LiteralPath $cachePath -Encoding UTF8)
    $trace = @($cache | Where-Object { $_ -clike 'EPOCANVA_PRODUCT_TRACE:*' })
    if (-not (Test-ProductSha256 $receipt.cache_sha256) -or $receipt.cache_sha256 -cne $cacheHash -or
        $trace.Count -ne 1 -or $trace[0] -cne 'EPOCANVA_PRODUCT_TRACE:BOOL=OFF') {
        throw 'Product configuration changed after verification, or trace is enabled.'
    }
    $current = Get-ProductSourceIdentity -RepoRoot $RepoRoot
    Assert-ProductObjectFields $receipt.source @('head', 'commit', 'source_sha256', 'repository_sha256', 'dirty', 'source_files')
    foreach ($field in @('head', 'commit', 'source_sha256', 'repository_sha256', 'dirty')) {
        if (-not [object]::Equals($receipt.source.$field, $current.$field)) {
            throw 'Source changed after product verification. Rebuild and retest before staging.'
        }
    }
    if (-not (Test-ProductJsonInteger $receipt.source.source_files) -or $receipt.source.source_files -ne $current.source_files) {
        throw 'Source changed after product verification. Rebuild and retest before staging.'
    }
    $buildId = @($cache | Where-Object { $_ -clike 'EPOCANVA_PRODUCT_BUILD_ID:*' })
    if ($buildId.Count -ne 1 -or $buildId[0] -cne ('EPOCANVA_PRODUCT_BUILD_ID:STRING=' + $current.commit)) {
        throw 'Build configuration does not bind the current complete source identity.'
    }
    Assert-ProductObjectFields $receipt.tests @('total', 'passed', 'skipped', 'failed', 'skipped_names')
    foreach ($field in @('total', 'passed', 'skipped', 'failed')) {
        if (-not (Test-ProductJsonInteger $receipt.tests.$field)) { throw 'Product test counts require typed integers.' }
    }
    if ($receipt.tests.total -le 0 -or $receipt.tests.passed -ne $receipt.tests.total -or
        $receipt.tests.failed -ne 0 -or $receipt.tests.skipped -ne 0 -or
        $receipt.tests.skipped_names -isnot [array] -or $receipt.tests.skipped_names.Count -ne 0) {
        throw 'Product test receipt has missing, failed or skipped tests.'
    }
    # A summary in mutable JSON is not proof of execution. Reopen each original
    # file, verify its digest and then independently parse its actual content.
    if ($receipt.evidence -isnot [string]) { throw 'Product receipt lacks its original evidence directory.' }
    $evidenceRoot = Assert-ProductPlainPath $receipt.evidence $BuildDir
    if ($evidenceRoot -ieq [IO.Path]::GetFullPath($BuildDir)) { throw 'Build evidence requires a unique run directory.' }
    foreach ($binding in @(
            @('ctest.xml', 'ctest_xml_sha256'), @('ctest-list.json', 'ctest_list_sha256'),
            @('commands.json', 'commands_sha256'))) {
        $filePath = Assert-ProductPlainPath (Join-Path $evidenceRoot $binding[0]) $BuildDir
        $digest = $receipt.($binding[1])
        if (-not (Test-ProductSha256 $digest) -or
                (Get-ProductIdentityFileSha256 -Path $filePath) -cne $digest) {
            throw 'Original build/test evidence changed after verification.'
        }
    }
    $cases = Read-ProductPassingJUnit (Join-Path $evidenceRoot 'ctest.xml')
    $inventory = Read-ProductTestInventory (Join-Path $evidenceRoot 'ctest-list.json')
    Assert-ProductRegisteredTestContract -RepoRoot $RepoRoot -BuildDir $BuildDir -Inventory $inventory
    Assert-ProductTestInventory $inventory $cases
    if ($cases.Count -ne $receipt.tests.total -or $cases.Count -ne $receipt.tests.passed) {
        throw 'Build receipt summary differs from independently parsed JUnit.'
    }
    $commands = Read-ProductJson (Join-Path $evidenceRoot 'commands.json')
    Assert-ProductBuildCommands $commands $BuildDir $evidenceRoot 'Release'
    $testPaths = @(Get-ProductTestExecutablePaths $inventory $BuildDir)
    if ($receipt.test_binaries -isnot [array] -or $receipt.test_binaries.Count -ne $testPaths.Count) {
        throw 'Test executable evidence differs from the complete CTest inventory.'
    }
    foreach ($testPath in $testPaths) {
        $matches = @($receipt.test_binaries | Where-Object { $_.path -is [string] -and $_.path -ieq $testPath })
        if ($matches.Count -ne 1) { throw 'Test executable is absent or duplicated in the receipt.' }
        $plain = Assert-ProductPlainPath $testPath $BuildDir
        Assert-ProductBinaryRecord $matches[0] ([IO.Path]::GetFileName($plain)) $plain
    }
    $expected = @{
        exe = (Join-Path $BuildDir 'product\bin\EpoCanva.exe')
        tsf = (Join-Path $BuildDir 'product\bin\epocanva_tsf.dll')
        rime = $RimeDll
    }
    if ($receipt.binaries -isnot [array] -or $receipt.binaries.Count -ne $expected.Count) {
        throw 'Product receipt must bind exactly the three product binaries.'
    }
    foreach ($key in $expected.Keys) {
        $entries = @($receipt.binaries | Where-Object { [object]::Equals($_.name, $key) })
        if ($entries.Count -ne 1) { throw "Build receipt lacks a unique $key binary." }
        $plain = if ($key -eq 'rime') { $expected[$key] } else { Assert-ProductPlainPath $expected[$key] $BuildDir }
        Assert-ProductBinaryRecord $entries[0] $key $plain
    }
    return $receipt
}

function Assert-ProductBuildEngineBinding([string]$RepoRoot, [string]$RimeDll, $Receipt) {
    $path = Assert-ProductPlainPath (Join-Path $RepoRoot 'build/librime-english/engine-build-receipt.json') $RepoRoot
    $engine = Assert-ProductEngineReceipt -RepoRoot $RepoRoot -RimeDll $RimeDll
    if ($Receipt.engine_source_sha256 -cne $engine.source_sha256 -or
            $Receipt.engine_receipt_sha256 -cne (Get-ProductIdentityFileSha256 $path)) {
        throw 'Product and English engine receipts describe different build inputs.'
    }
    return $engine
}

if ($PrintCommit) {
    if (-not $IdentityRepoRoot) { $IdentityRepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
    (Get-ProductSourceIdentity -RepoRoot $IdentityRepoRoot).commit
}
