[CmdletBinding()]
param(
    [string]$IdentityRepoRoot = '',
    [switch]$PrintCommit
)

Set-StrictMode -Version Latest

function Get-ProductSourceIdentity {
    param([Parameter(Mandatory)][string]$RepoRoot)
    $root = (Get-Item -LiteralPath $RepoRoot).FullName
    $head = (& git -C $root rev-parse HEAD)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot determine source HEAD.' }
    $status = @(& git -C $root status --porcelain --untracked-files=all)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot determine source worktree state.' }
    $paths = @(& git -c core.quotepath=false -C $root ls-files --cached --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate source inputs.' }
    $inputs = @($paths | Where-Object {
        ($_ -match '^(engine|platforms|tests|tools|data|cmake|third_party)/' -or
         $_ -match '^(CMakeLists\.txt|CMakePresets\.json|\.gitattributes|LICENSE|AGENTS\.md)$' -or
         $_ -match '^docs/product/license-decision\.md$') -and
        $_ -notmatch '^tools/product/isolation/'
    } | Sort-Object -Unique)
    $rows = [System.Collections.Generic.List[string]]::new()
    foreach ($relative in $inputs) {
        $path = Join-Path $root $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $rows.Add("$relative`tmissing")
            continue
        }
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        $rows.Add("$relative`t$hash")
    }
    if ($rows.Count -eq 0) { throw 'Source input list is empty.' }
    $algorithm = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes(($rows -join "`n") + "`n")
        $digest = ([BitConverter]::ToString($algorithm.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally { $algorithm.Dispose() }
    $dirty = $status.Count -ne 0
    $commit = $head.Trim()
    if ($dirty) { $commit += '+dirty.' + $digest.Substring(0, 12) }
    return [pscustomobject]@{
        head = $head.Trim()
        commit = $commit
        source_sha256 = $digest
        dirty = $dirty
        source_files = $rows.Count
    }
}

function Get-ProductEngineIdentity {
    param([Parameter(Mandatory)][string]$RepoRoot)
    $root = (Get-Item -LiteralPath $RepoRoot).FullName
    $paths = @(& git -c core.quotepath=false -C $root ls-files --cached --others --exclude-standard -- engine tools/cmake tools/build_librime_baseline.ps1 tools/product/build_identity.ps1 third_party/sources.lock.json)
    if ($LASTEXITCODE -ne 0 -or $paths.Count -eq 0) { throw 'Cannot enumerate engine source inputs.' }
    $rows = [System.Collections.Generic.List[string]]::new()
    foreach ($relative in ($paths | Sort-Object -Unique)) {
        $file = Join-Path $root $relative
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing engine source: $relative" }
        $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
        $rows.Add("$relative`t$hash")
    }
    $upstream = Join-Path $root 'third_party\librime'
    foreach ($relative in @('', 'deps/yaml-cpp', 'deps/leveldb', 'deps/marisa-trie', 'deps/opencc')) {
        $checkout = $upstream
        if ($relative) { $checkout = Join-Path $upstream $relative }
        $head = & git -C $checkout rev-parse HEAD
        if ($LASTEXITCODE -ne 0) { throw "Cannot determine pinned dependency: $relative" }
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
    $path = Join-Path $RepoRoot 'build\librime-english\engine-build-receipt.json'
    $receipt = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
    $identity = Get-ProductEngineIdentity -RepoRoot $RepoRoot
    $hash = (Get-FileHash -LiteralPath $RimeDll -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($receipt.format -ne 1 -or $receipt.status -ne 'built' -or
        $receipt.source_sha256 -ne $identity.source_sha256 -or $receipt.rime_sha256 -ne $hash -or
        $receipt.flags.logging -ne $false -or $receipt.flags.external_plugins -ne $false -or
        $receipt.flags.tests -ne $false) {
        throw 'English engine source/configuration does not match its build receipt. Rebuild english-completion.'
    }
    return $receipt
}

function Assert-ProductBuildReceipt {
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string]$BuildDir,
        [Parameter(Mandatory)][string]$RimeDll
    )
    $path = Join-Path $BuildDir 'product-build-receipt.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw 'No successful product build receipt. Run tools/product/build_product.ps1 with tests.'
    }
    $receipt = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($receipt.format -ne 1 -or $receipt.status -ne 'verified' -or
        $receipt.configuration -ne 'Release' -or $receipt.trace -ne $false -or
        $receipt.build_exit -ne 0 -or $receipt.ctest_exit -ne 0) {
        throw 'Product build receipt is not a verified non-trace Release build.'
    }
    $cachePath = Join-Path $BuildDir 'CMakeCache.txt'
    $cacheHash = (Get-FileHash -LiteralPath $cachePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $cache = @(Get-Content -LiteralPath $cachePath -Encoding UTF8)
    if ($receipt.cache_sha256 -ne $cacheHash -or
        @($cache | Where-Object { $_ -eq 'EPOCANVA_PRODUCT_TRACE:BOOL=OFF' }).Count -ne 1) {
        throw 'Product configuration changed after verification, or trace is enabled.'
    }
    $current = Get-ProductSourceIdentity -RepoRoot $RepoRoot
    if ($receipt.source.head -ne $current.head -or
        $receipt.source.source_sha256 -ne $current.source_sha256 -or
        $receipt.source.commit -ne $current.commit) {
        throw 'Source changed after product verification. Rebuild and retest before staging.'
    }
    if ($receipt.tests.total -le 0 -or $receipt.tests.passed -le 0 -or
        $receipt.tests.failed -ne 0 -or
        $receipt.tests.total -ne ($receipt.tests.passed + $receipt.tests.skipped)) {
        throw 'Product test receipt has missing or failed tests.'
    }
    $expected = @{
        exe = (Join-Path $BuildDir 'product\bin\EpoCanva.exe')
        tsf = (Join-Path $BuildDir 'product\bin\epocanva_tsf.dll')
        rime = $RimeDll
    }
    foreach ($key in $expected.Keys) {
        $entries = @($receipt.binaries | Where-Object { $_.name -eq $key })
        if ($entries.Count -ne 1) { throw "Build receipt lacks a unique $key binary." }
        $binary = $entries[0]
        $file = Get-Item -LiteralPath $expected[$key]
        $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -ne $binary.sha256 -or $file.Length -ne $binary.bytes) {
            throw "Binary changed after product tests: $key"
        }
    }
    return $receipt
}

if ($PrintCommit) {
    if (-not $IdentityRepoRoot) { $IdentityRepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
    (Get-ProductSourceIdentity -RepoRoot $IdentityRepoRoot).commit
}
