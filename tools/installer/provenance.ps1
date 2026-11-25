# Shared byte contracts; producers capture actual inputs and consumers recompute
# them independently. A local receipt is evidence, never an authorization/signature.
function Get-InstallerTextHash([string]$Text) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function Get-InstallerSourcePaths([string]$Kind) {
    $common = @('tools/installer/installer_contract.ps1', 'tools/installer/provenance.ps1',
        'tools/product/evidence_contract.ps1')
    switch -Exact ($Kind) {
        'helper' { return @('tools/product/isolation/tip_profile_helper.cc', 'tools/product/isolation/tip_profile_policy.h', 'tools/product/isolation/tip_profile_policy_tests.cc', 'tests/helper/CMakeLists.txt', 'platforms/windows/tsf/identifiers.cc',
            'platforms/windows/tsf/identifiers.h', 'tools/installer/helper/CMakeLists.txt', 'tools/installer/build_helper.ps1') + $common }
        'bootstrapper' { return @('tools/installer/bootstrapper.cc', 'tools/installer/build_bootstrapper.ps1',
            'platforms/windows/product/VERSION') + $common }
        'msi' { return @('tools/installer/build_msi.ps1', 'tools/installer/wix_tool.ps1', 'tools/installer/static_msi.ps1',
            'platforms/windows/product/VERSION', 'platforms/windows/product/EpoCanva.ico') + $common }
        default { throw 'Unknown installer source scope.' }
    }
}

function Get-InstallerSources([string]$RepoRoot, [string]$Kind) {
    $rows = @(foreach ($relative in Get-InstallerSourcePaths $Kind) {
        $path = Assert-InstallerBuildPath (Join-Path $RepoRoot $relative) $RepoRoot
        $identity = Get-InstallerFileIdentity $path
        [pscustomobject]@{ path = $relative; sha256 = $identity.sha256; bytes = $identity.bytes }
    })
    $body = ($rows | ForEach-Object { "$($_.path)`t$($_.sha256)`t$($_.bytes)" }) -join "`n"
    return [pscustomobject]@{ kind = $Kind; sha256 = Get-InstallerTextHash ($body + "`n"); files = $rows }
}

function Assert-InstallerSources($Recorded, [string]$RepoRoot, [string]$Kind) {
    $actual = Get-InstallerSources $RepoRoot $Kind
    if ($Recorded.kind -cne $Kind -or $Recorded.sha256 -cne $actual.sha256 -or
        @($Recorded.files).Count -ne $actual.files.Count) { throw 'Installer source provenance differs from current source.' }
    for ($index = 0; $index -lt $actual.files.Count; $index++) {
        $expected = $actual.files[$index]
        $row = $Recorded.files[$index]
        if ($row.path -cne $expected.path -or $row.sha256 -cne $expected.sha256 -or
            -not (Test-InstallerInteger $row.bytes) -or $row.bytes -ne $expected.bytes) {
            throw 'Installer source file provenance differs from current source.'
        }
    }
}

function Assert-InstallerFileReference($Reference, [string]$Boundary = '') {
    if ($Reference.path -isnot [string] -or $Reference.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        -not (Test-InstallerInteger $Reference.bytes) -or $Reference.bytes -le 0) { throw 'Invalid installer file reference.' }
    $path = if ($Boundary) { Assert-InstallerBuildPath $Reference.path $Boundary } else { $Reference.path }
    $actual = Get-InstallerFileIdentity $path
    if ($actual.sha256 -cne $Reference.sha256 -or $actual.bytes -ne $Reference.bytes) {
        throw 'Installer referenced file identity differs.'
    }
    return $actual
}

function Assert-InstallerHelperReceipt([string]$RepoRoot, [string]$ReceiptPath, [string]$HelperPath) {
    $receiptPathSafe = Assert-InstallerBuildPath $ReceiptPath (Join-Path $RepoRoot 'build')
    $reference = Get-InstallerFileIdentity $receiptPathSafe
    $receipt = Read-ProductJson $receiptPathSafe
    if (-not (Test-InstallerInteger $receipt.format) -or $receipt.format -ne 4 -or $receipt.status -cne 'verified' -or
        $receipt.configuration -cne 'Release;x64;CXX17;extensions=OFF;permissive=OFF;logging=OFF;runtime=MT' -or
        $receipt.configuration_sha256 -cne (Get-InstallerTextHash $receipt.configuration)) {
        throw 'Helper requires a current format-4 source/build/test receipt.'
    }
    foreach ($code in @($receipt.configure_exit, $receipt.build_exit, $receipt.test_exit)) {
        if (-not (Test-InstallerInteger $code) -or $code -ne 0) { throw 'Helper has a failed or missing process exit.' }
    }
    Assert-InstallerSources $receipt.source $RepoRoot 'helper'
    if ($receipt.source_sha256 -cne $receipt.source.sha256) { throw 'Helper source aggregate mismatch.' }
    $null = Assert-InstallerFileReference $receipt.toolchain.cmake_file
    $compiler = Assert-InstallerFileReference $receipt.toolchain.compiler_file
    $null = Assert-InstallerFileReference $receipt.toolchain.linker_file
    if ([Diagnostics.FileVersionInfo]::GetVersionInfo($compiler.path).FileVersion -cne $receipt.toolchain.compiler_version) {
        throw 'Helper compiler version differs.'
    }
    $toolchainBody = $receipt.toolchain | ConvertTo-Json -Depth 8 -Compress
    if ($receipt.toolchain_sha256 -cne (Get-InstallerTextHash $toolchainBody)) { throw 'Helper toolchain aggregate mismatch.' }
    $boundary = [IO.Path]::GetDirectoryName($receiptPathSafe)
    $null = Assert-InstallerFileReference $receipt.configure_log $boundary
    $null = Assert-InstallerFileReference $receipt.build_log $boundary
    $null = Assert-InstallerFileReference $receipt.test_log $boundary
    $testJUnit = Assert-InstallerFileReference $receipt.test_junit $boundary
    $testCases = Read-ProductPassingJUnit $testJUnit.path
    if ($testCases.Count -ne 1 -or $testCases[0].GetAttribute('name') -cne 'installer.helper.ownership_policy') {
        throw 'Helper policy execution is incomplete.'
    }
    $binary = Assert-InstallerFileReference $receipt.binary $boundary
    if ($binary.path -ine [IO.Path]::GetFullPath($HelperPath)) { throw 'Helper receipt names a different binary path.' }
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($binary.path))
    try { $null = Get-InstallerPeLayout $reader } finally { $reader.Dispose() }
    if ((Get-InstallerFileIdentity $receiptPathSafe).sha256 -cne $reference.sha256) { throw 'Helper receipt changed during validation.' }
    return [pscustomobject]@{ receipt = $receipt; reference = $reference; binary = $binary }
}
