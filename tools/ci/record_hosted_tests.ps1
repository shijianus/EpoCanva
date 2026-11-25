[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('contract-gate', 'root-unit-tests')][string]$Layer,
    [Parameter(Mandatory)][string]$CTest,
    [Parameter(Mandatory)][string]$BuildDir
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$RepoRoot = $root
. (Join-Path $root 'tools/product/build_identity.ps1')
. (Join-Path $PSScriptRoot 'ci_provenance.ps1')
$sha = & git -C $root rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $sha -cne $env:GITHUB_SHA -or $env:GITHUB_SHA -cnotmatch '\A[0-9a-f]{40}\z' -or
        $env:GITHUB_JOB -cne $Layer -or $env:GITHUB_RUN_ID -cnotmatch '\A[1-9][0-9]*\z' -or
        $env:GITHUB_RUN_ATTEMPT -cnotmatch '\A[1-9][0-9]*\z' -or [string]::IsNullOrWhiteSpace($env:RUNNER_NAME)) {
    throw 'Hosted evidence requires the actual workflow candidate, job and run identity.'
}
$directory = Join-Path $root "build/ci-acceptance-$($env:GITHUB_RUN_ID)-$($env:GITHUB_RUN_ATTEMPT)/$Layer"
$null = New-Item -ItemType Directory -Path $directory
$ci = [IO.Path]::GetFullPath($directory)
$registered = Join-Path $ci 'registered.json'
$junit = Join-Path $ci 'ctest.xml'
$summary = [ordered]@{
    format = 'epocanva-windows-ci-layer-v2'; layer = $Layer; source_commit = $sha
    run_id = $env:GITHUB_RUN_ID; run_attempt = $env:GITHUB_RUN_ATTEMPT; runner_name = $env:RUNNER_NAME
    started_utc = [DateTimeOffset]::UtcNow.ToString('o'); status = 'failed'; exit_code = $null
    workflow_sha256 = Get-ProductIdentityFileSha256 (Join-Path $root '.github/workflows/windows-ci.yml')
    input_lock_sha256 = Get-ProductIdentityFileSha256 (Join-Path $root 'tools/ci/inputs.lock.json')
    ctest_sha256 = Get-ProductIdentityFileSha256 $CTest
    exit_codes = [ordered]@{}; outputs = [ordered]@{}
}
function New-Reference([string]$Path) {
    $full = Assert-ProductPlainPath $Path $root
    return [ordered]@{ path = $full.Substring($root.Length + 1).Replace('\', '/');
        sha256 = Get-ProductIdentityFileSha256 $full; bytes = (Get-Item -LiteralPath $full).Length }
}
function Invoke-Observed([string]$Name, [string]$Executable, [string[]]$Arguments, [string]$Output) {
    $global:LASTEXITCODE = $null
    & $Executable @Arguments *> $Output
    $code = $LASTEXITCODE
    $summary.exit_codes[$Name] = $code
    if ($Name -cne 'registered') {
        "`nEPOCANVA process exit=$code" | Add-Content -LiteralPath $Output -Encoding utf8
        $summary.outputs["log-$Name"] = New-Reference $Output
    }
    if ($code -isnot [int] -or $code -ne 0) { throw "Hosted $Name failed: $code" }
}
try {
    $selection = @('--test-dir', $BuildDir, '-C', 'Debug')
    if ($Layer -ceq 'contract-gate') { $selection += @('-L', '^contract-gate$') }
    Invoke-Observed 'registered' $CTest ($selection + @('--show-only=json-v1')) $registered
    Invoke-Observed 'ctest' 'python.exe' (@('-B', (Join-Path $PSScriptRoot 'preserve_git_environment.py'), '--', $CTest) +
        $selection + @('--no-tests=error', '--output-on-failure', '--output-junit', $junit)) (Join-Path $ci 'ctest.log')
    if (-not (Test-Path -LiteralPath $junit -PathType Leaf)) { throw 'Hosted JUnit was not written at the absolute evidence path.' }
    Invoke-Observed 'verify-junit' 'python.exe' @('-B', (Join-Path $PSScriptRoot 'verify_root_junit.py'),
        '--registered', $registered, '--junit', $junit) (Join-Path $ci 'verify-junit.log')
    $summary.outputs['registered'] = New-Reference $registered
    $summary.outputs['junit'] = New-Reference $junit
    foreach ($name in @('configure.log', 'build.log', 'commands.log', 'metadata.json')) {
        $path = Join-Path $root ("build/vs2022-x64-debug/ci/$name")
        if (Test-Path -LiteralPath $path -PathType Leaf) { $summary.outputs["preflight-$name"] = New-Reference $path }
    }
    $headAfter = & git -C $root rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $headAfter -cne $sha) { throw 'Hosted candidate changed during tests.' }
    $indexFlags = @(& git -C $root ls-files -v)
    if ($LASTEXITCODE -ne 0 -or @($indexFlags | Where-Object { $_ -cmatch '^[a-zS] ' }).Count -ne 0) { throw 'Hosted checkout has hidden index state.' }
    $status = @(& git -c core.fsmonitor=false -C $root status --porcelain=v1 --untracked-files=all)
    if ($LASTEXITCODE -ne 0 -or $status.Count -ne 0) { throw 'Hosted tests changed source files.' }
    $summary.status = 'passed'
    $summary.exit_code = 0
} catch {
    $summary.exit_code = 1
    throw
} finally {
    $summary['completed_utc'] = [DateTimeOffset]::UtcNow.ToString('o')
    Write-ProductJson (Join-Path $ci 'summary.json') $summary
}
