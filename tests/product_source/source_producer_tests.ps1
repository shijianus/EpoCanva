# Isolated producer tests. All receipts and collaborators below are explicitly
# SYNTHETIC TEST FIXTURES, not product build/test/owner approval evidence.
[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$RunRoot = '',
    [switch]$ForceFailure
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
if (-not $RepoRoot) { $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path }
$RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName.TrimEnd('\', '/')
. (Join-Path $RepoRoot 'tools/product/source_contract.ps1')
$buildBase = Join-Path $RepoRoot 'build'
if (-not $RunRoot) { $RunRoot = Join-Path $buildBase 'multi-source-check' }
$RunRoot = [IO.Path]::GetFullPath($RunRoot).TrimEnd('\', '/')
if ((Split-Path -Parent $RunRoot) -ine $buildBase -or
        (Split-Path -Leaf $RunRoot) -cnotmatch '^[A-Za-z0-9][A-Za-z0-9_.-]*$' -or
        $RunRoot.EndsWith('.')) { throw 'Producer test RunRoot must be exactly <repo>/build/<preset>.' }
Assert-ProductSourcePlainPath $RunRoot $RepoRoot
$run = Join-Path $RunRoot ('synthetic-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $run -Force
$hostExe = (Get-Process -Id $PID).Path
$results = [Collections.Generic.List[object]]::new()
$utf8 = [Text.UTF8Encoding]::new($false)
function Write-Fixture([string]$Path, [string]$Text) {
    $null = New-Item -ItemType Directory -Path (Split-Path -Parent $Path) -Force
    [IO.File]::WriteAllText($Path, $Text, $utf8)
}
function Get-TestHash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Assert-Test([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function New-Fixture([string]$Name, [string]$Mode = 'normal') {
    $root = Join-Path $run $Name
    $bin = Join-Path $root 'tools/product'
    $null = New-Item -ItemType Directory -Path $bin -Force
    Copy-Item -LiteralPath (Join-Path $RepoRoot 'tools/product/stage_source.ps1') -Destination (Join-Path $bin 'stage_source.ps1')
    Copy-Item -LiteralPath (Join-Path $RepoRoot 'tools/product/source_contract.ps1') -Destination (Join-Path $bin 'source_contract.ps1')
    $identity = @'
# SYNTHETIC TEST COLLABORATORS; never real product evidence.
$script:fixtureMode = '__MODE__'
$script:identityCalls = 0
function Get-ProductSourceIdentity {
    param([string]$RepoRoot)
    ++$script:identityCalls
    $digest = 'a' * 64
    if ($script:fixtureMode -eq 'identity-change' -and $script:identityCalls -gt 1) { $digest = 'b' * 64 }
    [pscustomobject]@{ commit = ('1' * 40); source_sha256 = $digest; head = ('1' * 40); dirty = $false }
}
function Assert-ProductBuildReceipt {
    param([string]$RepoRoot, [string]$BuildDir, [string]$RimeDll)
    if ($script:fixtureMode -eq 'receipt-failure') { throw 'Synthetic invalid build receipt.' }
    [pscustomobject]@{ source = [pscustomobject]@{ commit = ('1' * 40); source_sha256 = ('a' * 64) }; engine_source_sha256 = ('c' * 64) }
}
function Assert-ProductEngineReceipt {
    param([string]$RepoRoot, [string]$RimeDll)
    if ($script:fixtureMode -eq 'engine-failure') { throw 'Synthetic invalid engine receipt.' }
    [pscustomobject]@{ source_sha256 = ('c' * 64) }
}
'@
    Write-Fixture (Join-Path $bin 'build_identity.ps1') $identity.Replace('__MODE__', $Mode)
    $enumerator = @'

# Override only in this private synthetic fixture, not in the production helper.
$script:listCalls = 0
function Get-ProductCorrespondingSourceFiles {
    param([string]$RepoRoot)
    ++$script:listCalls
    if ($script:fixtureMode -eq 'source-change' -and $script:listCalls -gt 1) {
        [IO.File]::WriteAllText((Join-Path $RepoRoot 'engine/main.cc'), 'changed synthetic source')
    }
    if ($script:fixtureMode -eq 'binding-change' -and $script:listCalls -gt 1) {
        [IO.File]::WriteAllText((Join-Path $RepoRoot 'build/candidate/product-build-receipt.json'), 'changed synthetic receipt')
    }
    if ($script:fixtureMode -eq 'set-change' -and $script:listCalls -gt 1) { return @('engine/main.cc') }
    switch ($script:fixtureMode) {
        'missing-source' { return @('engine/absent.cc', 'engine/main.cc') }
        'duplicate-source' { return @('engine/main.cc', 'engine/main.cc') }
        'case-collision' { return @('engine/Main.cc', 'engine/main.cc') }
        'directory-source' { return @('engine') }
        'empty-list' { return @() }
        'self-output' { return @('build/candidate/corresponding-source.zip') }
        default { return @('engine/empty.h', 'engine/main.cc') }
    }
}
'@
    [IO.File]::AppendAllText((Join-Path $bin 'source_contract.ps1'), $enumerator, $utf8)
    Write-Fixture (Join-Path $root 'engine/main.cc') "// SYNTHETIC TEST SOURCE`nint main() { return 0; }`n"
    Write-Fixture (Join-Path $root 'engine/empty.h') ''
    Write-Fixture (Join-Path $root 'platforms/windows/product/VERSION') "1.2.3-test`n"
    Write-Fixture (Join-Path $root 'data/manifests/product-package-files.inc') "// SYNTHETIC policy binding fixture`n"
    Write-Fixture (Join-Path $root 'build/candidate/product-build-receipt.json') '{"fixture":"synthetic-not-real-build-evidence"}'
    Write-Fixture (Join-Path $root 'build/candidate/foreign-sentinel.txt') 'do not alter this unrelated file'
    Write-Fixture (Join-Path $root 'build/private/not-source.txt') 'must not be archived'
    Write-Fixture (Join-Path $root 'build/stage/payload.txt') 'synthetic package payload'
    Write-Fixture (Join-Path $root 'build/rime/rime.dll') 'synthetic engine bytes'
    $payload = Join-Path $root 'build/stage/payload.txt'
    $manifest = @('epocanva-package-manifest 2', 'version=1.2.3-test', ('commit=' + ('1' * 40)),
        'acceptance-profile=internal-unsigned', 'distribution-policy=internal-only', 'path-exception=none',
        ("file`tpayload.txt`t" + (Get-TestHash $payload) + "`t" + (Get-Item -LiteralPath $payload).Length)) -join "`n"
    Write-Fixture (Join-Path $root 'build/stage/package.manifest') ($manifest + "`n")
    [pscustomobject]@{ root = $root; script = (Join-Path $bin 'stage_source.ps1'); build = (Join-Path $root 'build/candidate');
        stage = (Join-Path $root 'build/stage'); rime = (Join-Path $root 'build/rime/rime.dll'); name = $Name }
}
function Invoke-Producer($Fixture, [string[]]$Extra = @(), [string]$Expected = '') {
    $arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $Fixture.script,
        '-RepoRoot', $Fixture.root, '-BuildDir', $Fixture.build, '-StageRoot', $Fixture.stage, '-RimeDll', $Fixture.rime) + $Extra
    $output = @(& $hostExe @arguments 2>&1 | ForEach-Object { $_.ToString() })
    $code = $LASTEXITCODE
    $log = Join-Path $run ($Fixture.name + '-' + $results.Count + '.log')
    [IO.File]::WriteAllText($log, ($output -join "`n") + "`n", $utf8)
    $results.Add([pscustomobject]@{ case = $Fixture.name; command = @($hostExe) + $arguments; exit_code = $code; log = $log })
    if ($Expected) {
        Assert-Test ($code -ne 0) "Expected failure was accepted: $($Fixture.name)"
        Assert-Test (($output -join "`n") -match [regex]::Escape($Expected)) "Failure did not identify the expected cause ($Expected): $($output -join ' ')"
    } else { Assert-Test ($code -eq 0) "Producer failed: $($output -join ' ')" }
    Write-Host "PASS $($Fixture.name) (producer exit $code)"
}
function Assert-NoOutputs($Fixture) {
    Assert-Test (-not (Test-Path -LiteralPath (Join-Path $Fixture.build 'source-manifest.json'))) 'Failed producer published a source manifest.'
    Assert-Test (-not (Test-Path -LiteralPath (Join-Path $Fixture.build 'corresponding-source.zip'))) 'Failed producer published an archive.'
    Assert-Test (@(Get-ChildItem -LiteralPath $Fixture.build -Filter '.source-*' -Force).Count -eq 0) 'Failed producer leaked temporary files.'
}
function Assert-IndependentArchive($Fixture) {
    $zipPath = Join-Path $Fixture.build 'corresponding-source.zip'
    $document = Get-Content -LiteralPath (Join-Path $Fixture.build 'source-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-Test ($document.format -eq 1 -and @($document.PSObject.Properties).Count -eq 9) 'Wrong source manifest fields/format.'
    Assert-Test ($document.version -ceq '1.2.3-test' -and $document.commit -ceq ('1' * 40) -and $document.source_sha256 -ceq ('a' * 64)) 'Wrong source identity binding.'
    Assert-Test ($document.package_manifest_sha256 -ceq (Get-TestHash (Join-Path $Fixture.stage 'package.manifest'))) 'Wrong package hash binding.'
    Assert-Test ($document.package_policy_sha256 -ceq (Get-TestHash (Join-Path $Fixture.root 'data/manifests/product-package-files.inc'))) 'Wrong policy hash binding.'
    Assert-Test ($document.build_receipt_sha256 -ceq (Get-TestHash (Join-Path $Fixture.build 'product-build-receipt.json'))) 'Wrong receipt hash binding.'
    Assert-Test ($document.archive.path -ceq 'build/candidate/corresponding-source.zip' -and
        $document.archive.sha256 -ceq (Get-TestHash $zipPath) -and $document.archive.bytes -eq (Get-Item -LiteralPath $zipPath).Length) 'Wrong archive reference.'
    Assert-Test ($document.files.Count -eq 2 -and $document.files[0].path -ceq 'engine/empty.h' -and $document.files[1].path -ceq 'engine/main.cc') 'Wrong exact source file set.'
    $file = [IO.File]::OpenRead($zipPath)
    $archive = [IO.Compression.ZipArchive]::new($file, [IO.Compression.ZipArchiveMode]::Read)
    try {
        Assert-Test ($archive.Entries.Count -eq 2) 'ZIP has extra/missing entries.'
        foreach ($entry in $archive.Entries) {
            Assert-Test (-not $entry.FullName.EndsWith('/') -and $entry.ExternalAttributes -eq 0) 'ZIP contains a non-regular entry.'
            $rows = @($document.files | Where-Object { $_.path -ceq $entry.FullName })
            Assert-Test ($rows.Count -eq 1) 'ZIP has duplicate/unlisted entries.'
            $inputStream = $entry.Open(); $bytes = [IO.MemoryStream]::new()
            try { $inputStream.CopyTo($bytes); $actualBytes = $bytes.ToArray() }
            finally { $inputStream.Dispose(); $bytes.Dispose() }
            $originalBytes = [IO.File]::ReadAllBytes((Join-Path $Fixture.root $entry.FullName))
            Assert-Test ([Convert]::ToBase64String($actualBytes) -ceq [Convert]::ToBase64String($originalBytes)) 'ZIP bytes differ from independent source bytes.'
            Assert-Test ($rows[0].bytes -eq $originalBytes.Length -and $rows[0].sha256 -ceq (Get-TestHash (Join-Path $Fixture.root $entry.FullName))) 'Wrong file digest/byte count.'
        }
    } finally { $archive.Dispose(); $file.Dispose() }
    Assert-Test ((Get-Content -LiteralPath (Join-Path $Fixture.build 'foreign-sentinel.txt') -Raw) -ceq 'do not alter this unrelated file') 'Producer altered an unrelated file.'
}
try {
    $ok = New-Fixture 'success'
    Invoke-Producer $ok
    Assert-IndependentArchive $ok
    $firstZip = Get-TestHash (Join-Path $ok.build 'corresponding-source.zip')
    $firstManifest = Get-TestHash (Join-Path $ok.build 'source-manifest.json')
    Invoke-Producer $ok @() 'already exist'
    Invoke-Producer $ok @('-Replace')
    Assert-IndependentArchive $ok
    Assert-Test ((Get-TestHash (Join-Path $ok.build 'corresponding-source.zip')) -ceq $firstZip -and
        (Get-TestHash (Join-Path $ok.build 'source-manifest.json')) -ceq $firstManifest) 'Stable source did not produce identical outputs.'
    foreach ($case in @(
        @('missing-source', 'missing-source', 'Cannot find path'),
        @('duplicate-source', 'duplicate-source', 'sorted and unique'),
        @('case-collision', 'case-collision', 'sorted and unique'),
        @('directory-source', 'directory-source', 'is a directory'),
        @('empty-list', 'empty-list', 'inventory is empty'),
        @('self-output', 'self-output', 'includes its own output'),
        @('source-change', 'source-change', 'bytes changed'),
        @('set-change', 'set-change', 'file set changed'),
        @('identity-change', 'identity-change', 'identity changed'),
        @('binding-change', 'binding-change', 'bytes changed'),
        @('receipt-failure', 'receipt-failure', 'invalid build receipt'),
        @('engine-failure', 'engine-failure', 'invalid engine receipt'))) {
        $fixture = New-Fixture $case[0] $case[1]
        Invoke-Producer $fixture @() $case[2]
        Assert-NoOutputs $fixture
    }
    $legacy = New-Fixture 'legacy-package'
    $manifest = Join-Path $legacy.stage 'package.manifest'
    Write-Fixture $manifest ((Get-Content -LiteralPath $manifest -Raw).Replace('epocanva-package-manifest 2', 'epocanva-package-manifest 1'))
    Invoke-Producer $legacy @() 'legacy format 1'
    Assert-NoOutputs $legacy
    $wrongPackage = New-Fixture 'wrong-package-identity'
    $manifest = Join-Path $wrongPackage.stage 'package.manifest'
    Write-Fixture $manifest ((Get-Content -LiteralPath $manifest -Raw).Replace('version=1.2.3-test', 'version=8.9.0'))
    Invoke-Producer $wrongPackage @() 'identity does not match'
    Assert-NoOutputs $wrongPackage
    $foreign = New-Fixture 'foreign-output'
    $foreignArchive = Join-Path $foreign.build 'corresponding-source.zip'
    Write-Fixture $foreignArchive 'unrelated existing file'
    $foreignHash = Get-TestHash $foreignArchive
    Invoke-Producer $foreign @('-Replace') 'both producer-owned output files'
    Assert-Test ((Get-TestHash $foreignArchive) -ceq $foreignHash) 'Foreign output was overwritten.'
    $foreignDir = New-Fixture 'foreign-directory'
    $foreignPath = Join-Path $foreignDir.build 'corresponding-source.zip'
    Write-Fixture (Join-Path $foreignPath 'sentinel.txt') 'foreign directory content'
    Invoke-Producer $foreignDir @('-Replace') 'both producer-owned output files'
    Assert-Test ((Get-Content -LiteralPath (Join-Path $foreignPath 'sentinel.txt') -Raw) -ceq 'foreign directory content') 'Foreign directory was overwritten.'
    $outside = New-Fixture 'invalid-output-bound'
    $outside.build = Join-Path $outside.root 'outside'
    Invoke-Producer $outside @() 'exactly <repo>/build/<preset>'
    Assert-Test (-not (Test-Path -LiteralPath $outside.build)) 'Invalid output boundary was created.'
    # Exercise readback on a prior pair with changed uncompressed bytes. Its outer
    # digest is updated only inside this explicitly synthetic replacement fixture.
    $tampered = New-Fixture 'zip-readback'
    Invoke-Producer $tampered
    $zipPath = Join-Path $tampered.build 'corresponding-source.zip'
    $stream = [IO.File]::Open($zipPath, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Update)
    try {
        $entry = $archive.GetEntry('engine/main.cc'); $entry.Delete()
        $entry = $archive.CreateEntry('engine/main.cc'); $entry.ExternalAttributes = 0
        $writer = $entry.Open()
        try { $changed = [IO.File]::ReadAllBytes((Join-Path $tampered.root 'engine/main.cc')); $changed[0] = 88; $writer.Write($changed, 0, $changed.Length) }
        finally { $writer.Dispose() }
    } finally { $archive.Dispose(); $stream.Dispose() }
    $manifestPath = Join-Path $tampered.build 'source-manifest.json'
    $document = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $document.archive.sha256 = Get-TestHash $zipPath; $document.archive.bytes = (Get-Item -LiteralPath $zipPath).Length
    Write-Fixture $manifestPath ($document | ConvertTo-Json -Depth 8)
    Invoke-Producer $tampered @('-Replace') 'ZIP byte digest mismatch'
    if ($ForceFailure) { throw 'Injected producer-test assertion failure.' }
    Write-Fixture (Join-Path $run 'commands.json') ($results.ToArray() | ConvertTo-Json -Depth 6)
    Write-Host "RESULT PASS source producer tests ($($results.Count) real child executions; synthetic inputs only)."
    Write-Host "Evidence: $run"
    exit 0
} catch {
    Write-Fixture (Join-Path $run 'commands.json') ($results.ToArray() | ConvertTo-Json -Depth 6)
    Write-Host ('RESULT FAIL ' + $_.Exception.Message)
    Write-Host "Evidence: $run"
    exit 14
}
