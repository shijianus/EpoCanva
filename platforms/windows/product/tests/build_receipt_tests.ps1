[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$RunRoot,
    [switch]$ForceFailure
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
. (Join-Path $repo 'tools\product\build_identity.ps1')
$RunRoot = [System.IO.Path]::GetFullPath($RunRoot)
$buildBase = (Join-Path $repo 'build') + [System.IO.Path]::DirectorySeparatorChar
if (-not $RunRoot.StartsWith($buildBase, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'RunRoot must be inside build/.'
}
New-Item -ItemType Directory -Force -Path (Join-Path $RunRoot 'product\bin') | Out-Null
$paths = @{
    exe = (Join-Path $RunRoot 'product\bin\EpoCanva.exe')
    tsf = (Join-Path $RunRoot 'product\bin\epocanva_tsf.dll')
    rime = (Join-Path $RunRoot 'rime.dll')
}
foreach ($key in $paths.Keys) { [System.IO.File]::WriteAllText($paths[$key], "fixture-$key") }
$identity = Get-ProductSourceIdentity -RepoRoot $repo
$receiptPath = Join-Path $RunRoot 'product-build-receipt.json'
$cachePath = Join-Path $RunRoot 'CMakeCache.txt'
[System.IO.File]::WriteAllText($cachePath, "EPOCANVA_PRODUCT_TRACE:BOOL=OFF`n")
function New-Receipt {
    return [pscustomobject]@{
        format = 1; status = 'verified'; configuration = 'Release'; trace = $false
        source = ($identity | ConvertTo-Json | ConvertFrom-Json)
        build_exit = 0; ctest_exit = 0
        cache_sha256 = (Get-FileHash -LiteralPath $cachePath -Algorithm SHA256).Hash.ToLowerInvariant()
        tests = @{ total = 2; passed = 1; skipped = 1; failed = 0 }
        binaries = @($paths.Keys | ForEach-Object {
            [pscustomobject]@{ name = $_; path = $paths[$_]; bytes = (Get-Item -LiteralPath $paths[$_]).Length
                sha256 = (Get-FileHash -LiteralPath $paths[$_] -Algorithm SHA256).Hash.ToLowerInvariant() }
        })
    }
}
function Save-Receipt($Receipt) {
    [System.IO.File]::WriteAllText($receiptPath, ($Receipt | ConvertTo-Json -Depth 10))
}
function Assert-Rejected([string]$Name) {
    $rejected = $false
    try { $null = Assert-ProductBuildReceipt -RepoRoot $repo -BuildDir $RunRoot -RimeDll $paths.rime }
    catch { $rejected = $true }
    if (-not $rejected) { throw "$Name was accepted" }
    Write-Host "PASS reject $Name"
}
try {
    Save-Receipt (New-Receipt)
    $null = Assert-ProductBuildReceipt -RepoRoot $repo -BuildDir $RunRoot -RimeDll $paths.rime
    Write-Host 'PASS current source and binary receipt'
    $bad = New-Receipt; $bad.status = 'in_progress'; Save-Receipt $bad; Assert-Rejected 'unfinished build'
    $bad = New-Receipt; $bad.build_exit = 1; Save-Receipt $bad; Assert-Rejected 'failed build'
    $bad = New-Receipt; $bad.ctest_exit = 8; Save-Receipt $bad; Assert-Rejected 'failed tests'
    $bad = New-Receipt; $bad.trace = $true; Save-Receipt $bad; Assert-Rejected 'trace binary'
    $bad = New-Receipt; $bad.source.source_sha256 = ('0' * 64); Save-Receipt $bad; Assert-Rejected 'stale source'
    $bad = New-Receipt; $bad.source.commit = 'incorrect'; Save-Receipt $bad; Assert-Rejected 'wrong commit'
    $bad = New-Receipt; $bad.tests.total = 0; Save-Receipt $bad; Assert-Rejected 'empty test suite'
    Save-Receipt (New-Receipt)
    [System.IO.File]::WriteAllText($cachePath, "EPOCANVA_PRODUCT_TRACE:BOOL=ON`n")
    Assert-Rejected 'changed build configuration'
    [System.IO.File]::WriteAllText($cachePath, "EPOCANVA_PRODUCT_TRACE:BOOL=OFF`n")
    $bad = New-Receipt; $bad.binaries += $bad.binaries[0]; Save-Receipt $bad; Assert-Rejected 'duplicate binary'
    Save-Receipt (New-Receipt)
    [System.IO.File]::WriteAllText($paths.tsf, 'altered fixture')
    Assert-Rejected 'changed product DLL'
    if ($ForceFailure) { throw 'injected receipt test failure' }
    Write-Host 'RESULT PASS build receipt tampering tests'
    exit 0
} catch {
    Write-Host "RESULT FAIL $($_.Exception.Message)"
    exit 14
}
