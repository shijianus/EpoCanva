[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('backend', 'windows-product', 'windows-integration-acceptance', 'full-quality-stress-acceptance')]
    [string]$Layer
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$RepoRoot = $root
. (Join-Path $root 'tools/product/build_identity.ps1')
. (Join-Path $PSScriptRoot 'ci_provenance.ps1')
if ($env:GITHUB_EVENT_NAME -cne 'workflow_dispatch' -or $env:GITHUB_REF_PROTECTED -cne 'true' -or
        $env:GITHUB_REPOSITORY -cne 'shijianus/EpoCanva' -or $env:GITHUB_REF_TYPE -cne 'branch' -or
        $env:GITHUB_SHA -cnotmatch '^[0-9a-f]{40}$' -or $env:GITHUB_RUN_ID -cnotmatch '^[1-9][0-9]*$' -or
        $env:GITHUB_RUN_ATTEMPT -cnotmatch '^[1-9][0-9]*$' -or $env:RUNNER_OS -cne 'Windows' -or
        $env:RUNNER_ARCH -cne 'X64' -or [string]::IsNullOrWhiteSpace($env:EPOCANVA_CI_RUNNER_NAME) -or
        $env:RUNNER_NAME -cne $env:EPOCANVA_CI_RUNNER_NAME) {
    throw 'Requires an authorized, protected Windows x64 acceptance dispatch.'
}

$run = Join-Path $root "build/ci-acceptance-$($env:GITHUB_RUN_ID)-$($env:GITHUB_RUN_ATTEMPT)"
$directory = Join-Path $run $Layer
$null = New-Item -ItemType Directory -Path $directory
$windowsProduct = Join-Path $root 'build/windows-product'
$inputLock = Join-Path $PSScriptRoot 'inputs.lock.json'
$powershell = Join-Path $env:SystemRoot 'System32/WindowsPowerShell/v1.0/powershell.exe'
$started = [DateTimeOffset]::UtcNow.ToString('o')
$summary = [ordered]@{
    format = 'epocanva-windows-ci-layer-v2'
    source_commit = $env:GITHUB_SHA
    run_id = $env:GITHUB_RUN_ID
    run_attempt = $env:GITHUB_RUN_ATTEMPT
    runner_name = $env:RUNNER_NAME
    layer = $Layer
    started_utc = $started
    status = 'failed'
    exit_code = $null
    outputs = [ordered]@{}
    exit_codes = [ordered]@{}
}

function Get-Digest([string]$Path) {
    return Get-ProductIdentityFileSha256 $Path
}

function New-Reference([string]$Path) {
    $full = Assert-ProductPlainPath $Path $root
    return [ordered]@{ path = $full.Substring($root.Length + 1).Replace('\', '/');
        sha256 = Get-Digest $full; bytes = (Get-Item -LiteralPath $full).Length }
}

function Read-Reference($Reference) {
    Assert-ProductObjectFields $Reference @('path', 'sha256', 'bytes')
    Assert-ProductRelativePath $Reference.path
    $path = Assert-ProductPlainPath (Join-Path $root $Reference.path) $root
    if (-not (Test-ProductJsonInteger $Reference.bytes) -or $Reference.bytes -le 0 -or
            -not (Test-ProductSha256 $Reference.sha256) -or
            (Get-Item -LiteralPath $path).Length -ne $Reference.bytes -or (Get-Digest $path) -cne $Reference.sha256) {
        throw 'CI evidence reference is missing or changed.'
    }
    return $path
}

function Invoke-Step([string]$Name, [string]$Executable, [string[]]$Arguments, [int[]]$Allowed = @(0)) {
    $global:LASTEXITCODE = $null
    & $Executable @Arguments *> (Join-Path $directory "$Name.log")
    $code = $LASTEXITCODE
    $summary.exit_codes[$Name] = $code
    # Retain even an empty successful stdout as a nonempty process record.
    if ($Name -cne 'registered') {
        "`nEPOCANVA process exit=$code" | Add-Content -LiteralPath (Join-Path $directory "$Name.log") -Encoding utf8
        $summary.outputs["log-$Name"] = New-Reference (Join-Path $directory "$Name.log")
    }
    if ($code -isnot [int] -or $code -notin $Allowed) {
        throw "$Layer step '$Name' failed. Exit code = $code."
    }
}

function Invoke-Script([string]$Name, [string]$RelativePath, [string[]]$Arguments = @(), [int[]]$Allowed = @(0)) {
    $prefix = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $root $RelativePath))
    Invoke-Step $Name 'python.exe' (@('-B', (Join-Path $PSScriptRoot 'preserve_git_environment.py'), '--', $powershell) + $prefix + $Arguments) $Allowed
}

function Assert-Checkout {
    $head = & git -C $root rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $head -cne $env:GITHUB_SHA) { throw 'Candidate checkout differs from the dispatch SHA.' }
    $indexFlags = @(& git -C $root ls-files -v)
    if ($LASTEXITCODE -ne 0 -or @($indexFlags | Where-Object { $_ -cmatch '^[a-zS] ' }).Count -ne 0) { throw 'Candidate checkout has hidden index state.' }
    $status = @(& git -c core.fsmonitor=false -C $root status --porcelain=v1 --untracked-files=all)
    if ($LASTEXITCODE -ne 0 -or $status.Count -ne 0) { throw 'The acceptance checkout is not clean.' }
}

function Test-Inventory([string]$Registered, [string]$JUnit) {
    Invoke-Step 'verify-junit' 'python.exe' @('-B', (Join-Path $PSScriptRoot 'verify_root_junit.py'), '--registered', $Registered, '--junit', $JUnit)
    $summary.outputs['registered'] = New-Reference $Registered
    $summary.outputs['junit'] = New-Reference $JUnit
}

function Assert-Predecessor([string]$Previous) {
    $path = Join-Path $run "$Previous/summary.json"
    $result = Read-ProductJson $path
    Assert-CiLayerSummary $result $Previous $env:GITHUB_SHA $env:GITHUB_RUN_ID $env:GITHUB_RUN_ATTEMPT $env:RUNNER_NAME
    $summary.outputs["predecessor-$Previous"] = New-Reference $path
    return $result
}

function Assert-PackageGates([string[]]$Required) {
    Invoke-Script 'release-check' 'tools/product/check_release.ps1' @('-AcceptanceProfile', 'internal-unsigned') @(0, 1)
    $path = Join-Path $windowsProduct 'release-readiness.json'
    $result = Read-ProductJson $path
    if ($result.source.commit -cne $env:GITHUB_SHA -or -not (Test-ProductJsonInteger $result.exit_code) -or
            $result.exit_code -ne $summary.exit_codes['release-check']) {
        throw 'Release checker report and observed invocation disagree.'
    }
    foreach ($id in $Required) {
        $checks = @($result.checks | Where-Object { $_.id -ceq $id })
        if ($checks.Count -ne 1 -or $checks[0].status -cne 'passed') { throw "Required gate '$id' failed." }
    }
    # Later layers rewrite the live readiness report. Bind this invocation's
    # immutable snapshot so predecessor revalidation does not read new evidence.
    $snapshot = Join-Path $directory 'release-readiness.json'
    Copy-Item -LiteralPath $path -Destination $snapshot
    $summary.outputs['release_check'] = New-Reference $snapshot
}

function Get-ProductArtifacts {
    $manifest = @(Get-ChildItem -LiteralPath (Join-Path $windowsProduct 'stage') -Filter package.manifest -Recurse -File)
    if ($manifest.Count -ne 1) { throw 'A sole frozen package manifest is required.' }
    $bootstrapper = @(Get-ChildItem -LiteralPath (Join-Path $windowsProduct 'installer') -Filter '*-x64-setup.exe' -File)
    if ($bootstrapper.Count -ne 1) { throw 'A sole frozen EXE bootstrapper is required.' }
    $paths = [ordered]@{
        package = $manifest[0].FullName
        receipt = Join-Path $windowsProduct 'product-build-receipt.json'
        source_manifest = Join-Path $windowsProduct 'source-manifest.json'
        source_zip = Join-Path $windowsProduct 'corresponding-source.zip'
        installer_manifest = Join-Path $windowsProduct 'installer/installer-manifest.json'
        installer_exe = $bootstrapper[0].FullName
        product_exe = Join-Path $manifest[0].DirectoryName 'EpoCanva.exe'
        tsf_dll = Join-Path $manifest[0].DirectoryName 'epocanva_tsf.dll'
        rime_dll = Join-Path $manifest[0].DirectoryName 'rime.dll'
    }
    $hashes = [ordered]@{}
    foreach ($item in $paths.GetEnumerator()) {
        $hashes[$item.Key] = Get-Digest $item.Value
        $summary.outputs["product-$($item.Key)"] = New-Reference $item.Value
    }
    return $hashes
}

try {
    Assert-Checkout
    Set-Location -LiteralPath $root
    $summary['workflow_sha256'] = Get-Digest (Join-Path $root '.github/workflows/windows-ci.yml')
    $summary['input_lock_sha256'] = Get-Digest $inputLock
    if ($env:GITHUB_JOB -cne $Layer) { throw 'CI layer differs from the actual workflow job.' }
    $cache = (Resolve-Path -LiteralPath $env:EPOCANVA_CI_INPUT_CACHE).Path
    $workspace = $root.TrimEnd('\') + '\'
    if ($cache -ieq $root -or $cache.StartsWith($workspace, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The authorized input cache must be outside the workflow checkout.'
    }
    Invoke-Script 'verify-immutable-cache' 'tools/ci/verify_inputs.ps1' @('-RepoRoot', $cache, '-Lock', $inputLock)
    Invoke-Script 'verify-workspace-inputs' 'tools/ci/verify_inputs.ps1' @('-RepoRoot', $root, '-Lock', $inputLock)
    Invoke-Script 'verify-engine-provenance-cache' 'tools/ci/verify_engine_provenance.ps1' @('-RepoRoot', $root, '-InputRoot', $cache)
    Invoke-Script 'verify-engine-provenance-workspace' 'tools/ci/verify_engine_provenance.ps1' @('-RepoRoot', $root, '-InputRoot', $root)

    if ($Layer -ceq 'backend') {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        $installs = @(& $vswhere -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
        if ($LASTEXITCODE -ne 0) { throw 'VS discovery failed.' }
        $selected = @($installs | Where-Object {
            $candidate = Join-Path $_ 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
            (Test-Path -LiteralPath $candidate -PathType Leaf) -and (Get-Digest $candidate) -ceq $env:EPOCANVA_CI_CMAKE_SHA256
        })
        if ($selected.Count -ne 1) { throw 'Exactly one reviewed VS CMake installation is required.' }
        $cmake = Join-Path $selected[0] 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
        $summary['cmake_sha256'] = Get-Digest $cmake
        $ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
        $summary['ctest_sha256'] = Get-Digest $ctest
        if ($summary.ctest_sha256 -cne $env:EPOCANVA_CI_CTEST_SHA256) { throw 'CTest differs from the reviewed runner toolchain.' }
        $build = Join-Path $directory 'build'
        Invoke-Step 'configure' $cmake @('-S', (Join-Path $root 'tools/backend'), '-B', $build, '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DCMAKE_GENERATOR_INSTANCE=$($selected[0])", '-DEPOCANVA_BACKEND_WITH_FRENCH_DICTIONARY=ON')
        Invoke-Step 'build' $cmake @('--build', $build, '--config', 'Release', '--parallel', '8')
        $junit = Join-Path $directory 'ctest.xml'
        Invoke-Step 'registered' $ctest @('--test-dir', $build, '-C', 'Release', '--show-only=json-v1')
        Invoke-Step 'ctest' 'python.exe' @('-B', (Join-Path $PSScriptRoot 'preserve_git_environment.py'), '--',
            $ctest, '--test-dir', $build, '-C', 'Release', '--no-tests=error', '--output-on-failure', '--output-junit', $junit)
        Test-Inventory (Join-Path $directory 'registered.log') $junit
    } elseif ($Layer -ceq 'windows-product') {
        $null = Assert-Predecessor 'backend'
        if (Test-Path -LiteralPath $windowsProduct) { throw 'The product build path must be unused for this run.' }
        Invoke-Script 'build-product' 'tools/product/build_product.ps1'
        $receipt = Read-ProductJson (Join-Path $windowsProduct 'product-build-receipt.json')
        if ($receipt.status -cne 'verified') { throw 'Product build receipt is not verified.' }
        Test-Inventory (Join-Path $receipt.evidence 'ctest-list.json') (Join-Path $receipt.evidence 'ctest.xml')
        Invoke-Script 'stage-product' 'tools/product/stage_product.ps1' @('-StrictPaths')
        Invoke-Script 'stage-source' 'tools/product/stage_source.ps1'
        Invoke-Script 'build-helper' 'tools/installer/build_helper.ps1'
        Invoke-Script 'build-msi' 'tools/installer/build_msi.ps1'
        Invoke-Script 'build-bootstrapper' 'tools/installer/build_bootstrapper.ps1'
        # Diagnosis creates the candidate-bound evidence document consumed by
        # installer recording. Run it first so a fresh checkout needs no stale file.
        Invoke-Script 'diagnose-package' 'tools/product/record_package_diagnose.ps1'
        Invoke-Script 'record-installer' 'tools/installer/record_installer.ps1'
        $installerJUnit = Join-Path $directory 'installer-contract.junit.xml'
        Invoke-Step 'installer-contract' 'python.exe' @('-B', (Join-Path $PSScriptRoot 'preserve_git_environment.py'), '--',
            'python.exe', '-B', (Join-Path $root 'tests/installer/test_static_contract.py'), '--build-dir', $windowsProduct,
            '--junit', $installerJUnit, '-v')
        $null = Read-ProductPassingJUnit $installerJUnit
        $summary.outputs['installer_junit'] = New-Reference $installerJUnit
        Invoke-Script 'english' 'tools/product/run_english_acceptance.ps1'
        Invoke-Script 'privacy' 'tools/product/run_offline_privacy.ps1'
        Assert-PackageGates (Get-CiRequiredGates $Layer)
        $summary['product'] = Get-ProductArtifacts
    } else {
        $product = Assert-Predecessor 'windows-product'
        foreach ($artifact in (Get-ProductArtifacts).GetEnumerator()) {
            if ($product.product.($artifact.Key) -cne $artifact.Value) { throw 'The product artifact changed after the product layer.' }
        }
        if ($Layer -ceq 'full-quality-stress-acceptance') {
            $null = Assert-Predecessor 'windows-integration-acceptance'
            Invoke-Script 'reference-stress' 'tools/product/run_stress_reference_model.ps1'
        }
        $plan = (Resolve-Path -LiteralPath $env:EPOCANVA_CI_PLAN_PATH).Path
        if ($plan.StartsWith($workspace, [StringComparison]::OrdinalIgnoreCase) -or
                $env:EPOCANVA_CI_PLAN_SHA256 -cnotmatch '^[0-9a-f]{64}$' -or
                (Get-Digest $plan) -cne $env:EPOCANVA_CI_PLAN_SHA256) {
            throw 'The approved independent acceptance plan is missing or changed.'
        }
        $summary['plan_sha256'] = $env:EPOCANVA_CI_PLAN_SHA256
        Invoke-Step 'authorized-plan' 'python.exe' @('-B', (Join-Path $PSScriptRoot 'preserve_git_environment.py'), '--',
            $powershell, '-NoProfile', '-File', $plan, '-Layer', $Layer,
            '-RepoRoot', $root, '-BuildDir', $windowsProduct, '-OutputDir', $directory, '-ExpectedCommit', $env:GITHUB_SHA)
        Test-Inventory (Join-Path $directory 'registered.json') (Join-Path $directory 'ctest.xml')
        if ($Layer -ceq 'windows-integration-acceptance') {
            Assert-PackageGates (Get-CiRequiredGates $Layer)
        } else {
            Assert-PackageGates (Get-CiRequiredGates $Layer)
        }
        foreach ($artifact in (Get-ProductArtifacts).GetEnumerator()) {
            if ($product.product.($artifact.Key) -cne $artifact.Value) { throw 'The product artifact changed during acceptance.' }
        }
        $summary['product'] = $product.product
    }
    Invoke-Script 'verify-workspace-inputs-after' 'tools/ci/verify_inputs.ps1' @('-RepoRoot', $root, '-Lock', $inputLock)
    Invoke-Script 'verify-immutable-cache-after' 'tools/ci/verify_inputs.ps1' @('-RepoRoot', $cache, '-Lock', $inputLock)
    Assert-Checkout
    $summary.status = 'passed'
    $summary.exit_code = 0
    $summary['completed_utc'] = [DateTimeOffset]::UtcNow.ToString('o')
    $check = ConvertFrom-ProductJson ($summary | ConvertTo-Json -Depth 12)
    Assert-CiLayerSummary $check $Layer $env:GITHUB_SHA $env:GITHUB_RUN_ID $env:GITHUB_RUN_ATTEMPT $env:RUNNER_NAME
} catch {
    $summary.status = 'failed'
    $summary.exit_code = 1
    throw
} finally {
    $summary['completed_utc'] = [DateTimeOffset]::UtcNow.ToString('o')
    Write-ProductJson (Join-Path $directory 'summary.json') $summary
}
