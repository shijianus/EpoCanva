<#
.SYNOPSIS
    Reproducible native build entry for the librime Windows baselines.

.DESCRIPTION
    Two explicit, separately named modes, selected with -Mode. The default is the
    unmodified P1.2 baseline, so no invocation that does not ask for the component
    build can silently produce one:

      baseline           (default) Configures, builds and installs the four required
                         librime submodules plus librime itself, from unmodified
                         sources, into build/librime-baseline/.

      component-probe    P1.4. Builds the SAME pinned librime sources with the same
                         option set, plus EpoCanva's probe component under
                         engine/rime_components/probe compiled INTO rime.dll, into
                         build/librime-component-probe/. It reuses the dependency
                         libraries produced by a completed baseline run (identical
                         configuration, asserted below) instead of rebuilding them,
                         and it injects the component through CMake's documented
                         CMAKE_PROJECT_rime_INCLUDE hook -- no upstream file is
                         edited and no upstream directory is created.

      english-completion P2.1. Same pinned sources and same option set, plus the
                         English language core (engine/languages/en) and the English
                         Translator (engine/rime_components/english) compiled INTO
                         rime.dll, into build/librime-english/. The probe component
                         is NOT compiled in this mode, so no F6 behaviour exists in
                         the resulting engine. Dependencies are reused from the
                         baseline run exactly as in component-probe mode.

    All modes use Release / x64 / static-dependencies / shared-rime / static-CRT.

    This script does NOT download anything and does NOT install software.
    Source acquisition is a separate, explicitly recorded step; see
    docs/build/librime-windows-baseline.md section "Acquisition".

    Every native command's exit code is checked. A failure throws with the stage name
    so a partially completed run can never be reported as green.

.PARAMETER Mode
    'baseline' (default), 'component-probe' or 'english-completion'. Selects the
    build root, the artefact set and which EpoCanva sources are compiled into
    rime.dll.

.PARAMETER RepoRoot
    Repository root. Defaults to the parent of this script's directory.

.PARAMETER BoostRoot
    Directory that contains the boost/ header tree (the extracted boost_1_89_0 root).
    Defaults to build/librime-baseline/dependencies/boost/boost_1_89_0.

.PARAMETER Skip
    Names to skip: marisa, yaml-cpp, leveldb, opencc, rime. Ignored in the two
    component modes, which never rebuild the four dependencies; they reuse them
    from build/librime-baseline/prefix and fail if they are not there.

    A comma-separated list arrives as ONE array element when this script is
    started through "powershell.exe -File script.ps1 -Skip a,b,c", because -File
    does not split arguments. The value is therefore re-split on commas below, so
    both "-Skip marisa,yaml-cpp" and "-Skip marisa -Skip yaml-cpp" behave the
    same. Without that normalisation a typo like -Skip rime,opencc would silently
    skip nothing and rebuild the whole chain.

.PARAMETER Fresh
    Delete the dependency and librime build directories this script owns before
    configuring, so no stale CMake cache can keep an older flag set. Only
    directories under the mode's own build root are ever removed; no source tree is
    touched. Without it, an existing cache whose settings differ from the requested
    ones aborts the run instead of silently reusing them.

.PARAMETER Verbose_Build
    Pass --verbose to each native build so the real cl.exe command lines land in the
    logs. The static CRT is then proven from those command lines and from the
    linker directives inside the produced objects, not from an echoed option.
#>
[CmdletBinding()]
param(
    [ValidateSet('baseline', 'component-probe', 'english-completion')]
    [string]$Mode = 'baseline',
    [string]$RepoRoot,
    [string]$BoostRoot,
    [string[]]$Skip = @(),
    [switch]$Fresh,
    [switch]$Verbose_Build
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

$script:LogDir = $null
$script:StepIndex = 0

function Write-Head {
    param([string]$Text)
    Write-Host ''
    Write-Host "=== $Text ===" -ForegroundColor Cyan
}

function Invoke-BaselineStep {
    <#
      Runs one native command, appends combined output to a numbered log file,
      and returns stdout lines. Throws with the stage name on a non-zero exit code
      or on a missing executable.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Stage,
        [Parameter(Mandatory)][string]$Exe,
        [Parameter()][string[]]$Arguments = @()
    )

    $resolved = Get-Command $Exe -ErrorAction SilentlyContinue
    if (-not $resolved) {
        throw "stage '$Stage': executable not found: $Exe"
    }
    # Get-Item on a bare name like 'dumpbin.exe' resolves against the current
    # directory, not PATH, so the command lookup result is what gives the full path.
    $display = $resolved.Source

    $script:StepIndex++
    $logName = '{0:D2}-{1}.log' -f $script:StepIndex, ($Stage -replace '[^A-Za-z0-9_.-]', '_')
    $logPath = Join-Path $script:LogDir $logName
    # The format operator must be inside parentheses here: in argument position a
    # bare `-f` is parsed as a parameter name of Write-Host (-ForegroundColor).
    Write-Host ("[step {0:D2}] {1}" -f $script:StepIndex, $Stage)
    Write-Host "        exe : $display"
    foreach ($a in $Arguments) { Write-Host "        arg : $a" }

    # Native tools write progress and warnings on stderr. Under
    # $ErrorActionPreference='Stop' PowerShell turns those into terminating errors
    # (NativeCommandError), which would mask real exit codes, so the preference is
    # relaxed only for the duration of the call.
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        # & with an array never re-parses the arguments; PowerShell quotes each element.
        $out = & $display @Arguments 2>&1 | ForEach-Object { $_.ToString() }
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }

    $header = @(
        "stage       : $Stage"
        "executable  : $display"
        "arguments   :"
    ) + ($Arguments | ForEach-Object { "  |$_" }) + @("exit code   : $code", '')
    ($header + $out) | Set-Content -Path $logPath -Encoding UTF8

    if ($code -ne 0) {
        Write-Host "---- last 40 lines of $logName ----" -ForegroundColor Red
        $out | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }
        throw "stage '$Stage' failed with exit code $code (log: $logName)"
    }
    return $out
}

function Test-PathOrThrow {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$What)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "$What not found: $Path"
    }
}

function Assert-SameString {
    param([string]$Expected, [string]$Actual, [string]$What)
    if ($Expected -ne $Actual) {
        throw "${What}: expected '$Expected' but observed '$Actual'"
    }
}

function Get-CacheEntry {
    param([Parameter(Mandatory)][string]$BuildDir,
          [Parameter(Mandatory)][string]$Key)
    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    Test-PathOrThrow $cache "CMake cache under $BuildDir"
    $line = @(Get-Content -LiteralPath $cache |
        Where-Object { $_ -match "^$([regex]::Escape($Key)):" } |
        Select-Object -First 1)
    if ($line.Count -eq 0) { return $null }
    return ($line[0] -split '=', 2)[1]
}

# A successful configure does not prove the intended option took effect: a typo in
# an option name is silently ignored by CMake. Each dependency therefore re-reads
# its own cache and fails the stage when a value differs from what was requested.
function Assert-CacheValue {
    param([Parameter(Mandatory)][string]$BuildDir,
          [Parameter(Mandatory)][hashtable]$Expect,
          [Parameter(Mandatory)][string]$Stage)
    foreach ($key in ($Expect.Keys | Sort-Object)) {
        $want = $Expect[$key].Replace('\', '/')
        $got = Get-CacheEntry -BuildDir $BuildDir -Key $key
        if ($null -eq $got) {
            throw "${Stage}: cache entry '$key' is absent -- the option name is not defined by this project"
        }
        if ($got.Replace('\', '/') -ne $want) {
            throw "${Stage}: cache entry '$key' = '$got', requested '$want'"
        }
        Write-Host "  cache ok : $key = $got"
    }
}

function Test-BinaryContainsAscii {
    <#
      Byte-exact substring search over a PE file. Latin1 (codepage 28591) maps
      bytes 0x00-0xff one-to-one to characters, so IndexString over the decoded
      text is a genuine byte scan for an ASCII needle and stays fast on a 2.6 MB
      image. Used to prove a compiled-in component's string literals really are in
      the produced DLL -- and, in the baseline, that they are not.
    #>
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Needle)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $text = [System.Text.Encoding]::GetEncoding(28591).GetString($bytes)
    return ($text.IndexOf($Needle, [System.StringComparison]::Ordinal) -ge 0)
}

function Assert-NoEmbeddedBuildPaths {
    <#
      Byte-level release-path rule applied before an engine is consumed by the
      product. PDB files are not distribution inputs; binaries are never edited.
    #>
    param([Parameter(Mandatory)][string[]]$Artifacts,
          [Parameter(Mandatory)][string]$Stage)
    $needles = @($RepoRoot, $RepoRoot.Replace('/', '\'), $RepoRoot.Replace('\', '/'),
                 'D:\Users\', 'C:\Users\', 'OneDrive') | Sort-Object -Unique
    if ($env:USERNAME) { $needles += $env:USERNAME }
    foreach ($artifact in $Artifacts) {
        $bytes = [System.IO.File]::ReadAllBytes($artifact)
        $text = [System.Text.Encoding]::UTF8.GetString($bytes)
        $wide = [System.Text.Encoding]::Unicode.GetString($bytes)
        foreach ($needle in ($needles | Where-Object { $_ })) {
            if ($text.IndexOf($needle, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -or
                $wide.IndexOf($needle, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                throw ("{0}: embedded build path found in {1}; path normalization did not cover this input" -f $Stage, $artifact)
            }
        }
    }
    Write-Host ("  path hygiene ok : {0} compiled artefact(s) contain no local build path" -f $Artifacts.Count)
}

# Where a library actually came from, rather than merely that something was found.
function Assert-CacheUnder {
    param([Parameter(Mandatory)][string]$BuildDir,
          [Parameter(Mandatory)][string[]]$Keys,
          [Parameter(Mandatory)][string]$Root,
          [Parameter(Mandatory)][string]$Stage)
    $rootNorm = $Root.Replace('\', '/').TrimEnd('/').ToLowerInvariant()
    foreach ($key in $Keys) {
        $got = Get-CacheEntry -BuildDir $BuildDir -Key $key
        if ([string]::IsNullOrWhiteSpace($got)) {
            throw "${Stage}: '$key' resolved to nothing; the dependency was not located"
        }
        if ($got.Replace('\', '/').ToLowerInvariant().StartsWith($rootNorm) -eq $false) {
            throw "${Stage}: '$key' points outside the intended root ($Root): $got"
        }
        Write-Host "  provenance ok : $key = $got"
    }
}

# ---------------------------------------------------------------------------
# 0. layout
# ---------------------------------------------------------------------------

if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}
$RepoRoot = (Get-Item -LiteralPath $RepoRoot).FullName

$RimeSrc   = Join-Path $RepoRoot 'third_party\librime'
# $Baseline is this run's own build root; the name is kept for both modes because
# every downstream path is derived from it. The two modes never share a root, which
# is what keeps build/librime-baseline/ untouched by a component build.
$BuildRel = $null
$BaselineRoot = Join-Path $RepoRoot 'build\librime-baseline'
$IsComponentMode = ($Mode -ne 'baseline')
# Which EpoCanva sources the hook compiles in, and the label it prints in its
# ATTACHED line. The build entry asserts exactly one ATTACHED line for THIS mode's
# label, so a mode that attached nothing cannot be reported as a component build
# and one mode's log can never satisfy another mode's assertion.
$ComponentNames = @()
$HookLabel = ''
$ComponentDllNeedles = @()
$ComponentObjNames = @()
if ($Mode -eq 'component-probe') {
    $BuildRel = 'build\librime-component-probe'
    $ComponentNames = @('probe')
    $HookLabel = 'P1.4'
    $ComponentObjNames = @('probe_module.obj', 'probe_processor.obj')
    # 'epocanva_probe' is a substring of 'epocanva_probe_processor', so requiring
    # both needles proves the module name and the class name are each present.
    $ComponentDllNeedles = @('epocanva_probe', 'epocanva_probe_processor')
} elseif ($Mode -eq 'english-completion') {
    $BuildRel = 'build\librime-english'
    $ComponentNames = @('english')
    $HookLabel = 'P2.1'
    # Two object libraries contribute objects here: the component itself and the
    # language core it calls. Naming EVERY translation unit is what proves the
    # completion code, not just a stub, reached the engine: english_case.obj was
    # added for P2.2 and english_token.obj for P2.3, english_space_policy.obj and
    # english_space_processor.obj for P2.5, spelling_suggester.obj for P2.6, and a
    # list that lagged behind the source would let a build that dropped a layer
    # still pass.
    # NOTE for the record: no DLL string needle is claimed for english_token.cc or
    # for spelling_suggester.cc. Their literals belong to the *Name() helpers, which
    # the translator does not call, so the linker discards them; those layers are
    # certified instead by this per-object check, by the object count below, and by
    # the runtime counters that only they can produce (protected-text and
    # illegal-word-form refusals, and the eight distinct spelling skip states, on
    # real keystrokes).
    $ComponentObjNames = @('english_module.obj', 'english_translator.obj',
                           'english_space_processor.obj',
                           'english_lexicon.obj', 'english_case.obj',
                           'english_token.obj', 'english_space_policy.obj',
                           'spelling_suggester.obj', 'prefix_completer.obj',
                           'morphology_rules.obj', 'language_support.obj')
    # 'epocanva_english' alone is also a substring of 'epocanva_english_translator',
    # so these two needles together prove BOTH the module name and the component
    # name were emitted into the image. The third is the P2.5 Processor's
    # registration name, which english_module.cc passes to Registry::Register: unlike
    # english_token.cc's discarded helpers, that literal is live, so its absence
    # would mean the space policy was not compiled into this engine at all. The
    # fourth is P2.6's candidate type, a live literal in the Translator's Query().
    $ComponentDllNeedles = @('epocanva_english', 'epocanva_english_translator',
                             'epocanva_english_space', 'epocanva_english_spelling')
} else {
    $BuildRel = 'build\librime-baseline'
}
$script:RunStart = Get-Date
$Baseline = Join-Path $RepoRoot $BuildRel
$DepBuild  = Join-Path $Baseline 'dependencies'
$RimeBuild = Join-Path $Baseline 'rime'
$RimeInst  = Join-Path $RimeBuild 'install'
$Logs      = Join-Path $Baseline 'logs'
$LockFile  = Join-Path $RepoRoot 'third_party\sources.lock.json'
$EngineReceiptPath = Join-Path $Baseline 'engine-build-receipt.json'
$EngineIdentityStart = $null

function Write-EngineReceipt {
    param(
        [Parameter(Mandatory)][ValidateSet('in_progress', 'built')][string]$Status,
        [AllowNull()][string]$SourceSha256,
        [AllowNull()][string]$RimeSha256
    )
    $receipt = [ordered]@{
        format        = 1
        status        = $Status
        source_sha256 = $SourceSha256
        rime_sha256   = $RimeSha256
        flags         = [ordered]@{
            logging          = $false
            external_plugins = $false
            tests            = $false
        }
    }
    $json = $receipt | ConvertTo-Json -Depth 4
    [System.IO.File]::WriteAllText(
        $EngineReceiptPath, $json + [Environment]::NewLine,
        [System.Text.UTF8Encoding]::new($false))
}

# A component build does not rebuild the dependency chain: rebuilding it with a
# second set of flags is exactly how two engines drift apart. It reuses the
# libraries the baseline mode installed, and refuses to continue when they are
# not there rather than letting find_library() reach out to a system copy.
if ($IsComponentMode) {
    $Prefix = Join-Path $BaselineRoot 'prefix'
} else {
    $Prefix = Join-Path $Baseline 'prefix'
}

if (-not $BoostRoot) {
    $BoostRoot = Join-Path $BaselineRoot 'dependencies\boost\boost_1_89_0'
}

# The P1.4 project-side CMake adaptation and the EpoCanva source trees it compiles
# in. One root for every component, plus the language core the English component
# calls into.
$ComponentHook = Join-Path $RepoRoot 'tools\cmake\epocanva_rime_compiled_in_components.cmake'
$ReleasePathsHook = Join-Path $RepoRoot 'tools\cmake\epocanva_release_paths.cmake'
$ComponentRoot = Join-Path $RepoRoot 'engine\rime_components'
$EnCoreDir     = Join-Path $RepoRoot 'engine\languages\en'
Test-PathOrThrow $ReleasePathsHook 'MSVC release-path CMake hook'

foreach ($d in @($Baseline, $RimeBuild, $Logs)) {
    if (-not (Test-Path -LiteralPath $d)) { New-Item -ItemType Directory -Path $d | Out-Null }
}
if ($Mode -eq 'baseline') {
    foreach ($d in @($DepBuild, $Prefix)) {
        if (-not (Test-Path -LiteralPath $d)) { New-Item -ItemType Directory -Path $d | Out-Null }
    }
}
$script:LogDir = $Logs

# A product engine receipt is invalidated before any English build work starts.
# The identity helper hashes the actual project-side engine inputs and the pinned,
# clean upstream checkouts; the receipt itself is intentionally not an input.
if ($Mode -eq 'english-completion') {
    $identityHelper = Join-Path $RepoRoot 'tools\product\build_identity.ps1'
    Test-PathOrThrow $identityHelper 'product engine identity helper'
    . $identityHelper
    $EngineIdentityStart = Get-ProductEngineIdentity -RepoRoot $RepoRoot
    Write-EngineReceipt -Status 'in_progress' `
        -SourceSha256 $EngineIdentityStart.source_sha256 -RimeSha256 $null
    Write-Host ("  engine receipt : status=in_progress source_sha256={0}" -f
        $EngineIdentityStart.source_sha256)
}

# Preserve previous command evidence before reusing numbered log names.
$previousLogs = @(Get-ChildItem -LiteralPath $Logs -Filter '*.log' -File -ErrorAction SilentlyContinue)
if ($previousLogs.Count -gt 0) {
    $archive = Join-Path $Baseline ('log-history\' + (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssfffZ'))
    New-Item -ItemType Directory -Path $archive | Out-Null
    foreach ($previousLog in $previousLogs) {
        Move-Item -LiteralPath $previousLog.FullName -Destination $archive
    }
}

# cl.exe, MSBuild and CMake write messages in the console output code page. If
# PowerShell decodes them as UTF-8 the logs become mojibake, so decode with the
# code page the tools actually use. MSBuild is additionally pinned to English so
# the archived logs are readable on any locale.
try {
    $oemCp = [int](& cmd.exe /c 'echo %OEMCP%' 2>$null |
        Where-Object { $_ -match '^\d+$' } | Select-Object -First 1)
    if ($oemCp) {
        [Console]::OutputEncoding = [System.Text.Encoding]::GetEncoding($oemCp)
    }
} catch {
    Write-Host 'note: could not set the console output encoding; logs may be transcoded'
}
$env:VSLANG = '1033'

if ($Fresh) {
    # These directories are build output produced by this script, nothing else. A
    # component build owns only its own rime tree: the baseline dependencies under
    # build/librime-baseline/ are inputs it reuses, never something it deletes.
    $owned = @($RimeBuild)
    if ($Mode -eq 'baseline') {
        $owned = @((Join-Path $DepBuild 'marisa'), (Join-Path $DepBuild 'yaml-cpp'),
            (Join-Path $DepBuild 'leveldb'), (Join-Path $DepBuild 'opencc')) + $owned
    }
    foreach ($d in $owned) {
        if (Test-Path -LiteralPath $d) {
            Write-Host "fresh: removing $d"
            Remove-Item -LiteralPath $d -Recurse -Force
        }
    }
}

Write-Head "librime native build -- mode: $Mode"
Write-Host "repo root : $RepoRoot"
Write-Host "build root: $Baseline"
Write-Host "prefix    : $Prefix"
if ($IsComponentMode) {
    Write-Host "components: $($ComponentNames -join ', ') (hook label $HookLabel)"
    Write-Host "hook      : $ComponentHook"
    Write-Host "note      : the four dependency libraries are NOT rebuilt here; they "
    Write-Host "            are read from build/librime-baseline/prefix and their "
    Write-Host "            configuration is re-asserted below."
}

# ---------------------------------------------------------------------------
# 1. toolchain: VS2022 (major 17) x64, CMake bundled with that instance
# ---------------------------------------------------------------------------

Write-Head 'toolchain'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
Test-PathOrThrow $vswhere 'vswhere.exe'

# Read-only query of the local install list, restricted to the VS2022 major version.
$candidates = & $vswhere -all -products '*' -version '[17.0,18.0)' -property installationPath 2>&1
if ($LASTEXITCODE -ne 0) { throw "vswhere failed with exit code $LASTEXITCODE" }
$preferred = $candidates | Where-Object { $_ -match '\\2022\\Professional$' } | Select-Object -First 1
if (-not $preferred) {
    $preferred = $candidates | Where-Object { $_ -match '\\2022\\(Professional|Enterprise|Community|BuildTools)$' } | Select-Object -First 1
}
if (-not $preferred) { throw 'no Visual Studio 2022 (major version 17) instance found' }
$VsRoot = $preferred

$devShellDll = Join-Path $VsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Test-PathOrThrow $devShellDll 'Microsoft.VisualStudio.DevShell.dll'
Import-Module $devShellDll
Enter-VsDevShell -VsInstallPath $VsRoot -SkipAutomaticLocation `
    -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

$CMake = Join-Path $VsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$CTest = Join-Path $VsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
Test-PathOrThrow $CMake 'CMake bundled with the selected VS instance'
Test-PathOrThrow $CTest  'CTest bundled with the selected VS instance'

foreach ($t in 'cl.exe', 'link.exe', 'dumpbin.exe', 'msbuild.exe') {
    if (-not (Get-Command $t -ErrorAction SilentlyContinue)) {
        throw "developer command '$t' is missing after entering the VS dev shell"
    }
}

Write-Host "VS install  : $VsRoot"
Write-Host "cmake       : $CMake"
Invoke-BaselineStep 'toolchain.cmake-version' $CMake @('--version') | Select-Object -First 1 | ForEach-Object { Write-Host "  $_" }
Invoke-BaselineStep 'toolchain.ctest-version' $CTest @('--version') | Select-Object -First 1 | ForEach-Object { Write-Host "  $_" }
# Read the compiler identity from the binary, because `cl.exe` with no arguments
# prints its banner on stderr and exits 1 -- running it as a "step" would fail.
$clPath = (Get-Command cl.exe).Source
Write-Host ("cl          : {0} (version {1})" -f $clPath, (Get-Item $clPath).VersionInfo.FileVersion)
$Generator = 'Visual Studio 17 2022'

# ---------------------------------------------------------------------------
# 2. identity: the checkout must be exactly what sources.lock.json pins
# ---------------------------------------------------------------------------

Write-Head 'source identity'

Test-PathOrThrow $LockFile 'third_party/sources.lock.json'
# -Encoding UTF8 is required: the lock file is UTF-8 with CJK content, and
# Windows PowerShell 5.1 reads it as the ANSI code page by default, which makes
# ConvertFrom-Json fail on the mangled bytes.
$lock = Get-Content -LiteralPath $LockFile -Raw -Encoding UTF8 | ConvertFrom-Json
$pkg = $lock.packages | Where-Object { $_.name -eq 'librime' } | Select-Object -First 1
if (-not $pkg) { throw "sources.lock.json has no 'librime' package entry" }
$ExpectedCommit = $pkg.pinned_commit

$Git = (Get-Command git.exe -ErrorAction SilentlyContinue)
if (-not $Git) { throw 'git.exe is required to verify the pinned checkout' }

$head = (& $Git.Source -C $RimeSrc rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw "git rev-parse HEAD failed in $RimeSrc (exit $LASTEXITCODE)" }
Assert-SameString $ExpectedCommit $head 'librime HEAD vs sources.lock.json pinned_commit'
Write-Host "librime HEAD matches lock : $head"

$describe = (& $Git.Source -C $RimeSrc describe --tags).Trim()
Write-Host "librime tag description    : $describe"

$dirty = @(& $Git.Source -C $RimeSrc status --porcelain)
if ($LASTEXITCODE -ne 0) { throw "git status failed in $RimeSrc (exit $LASTEXITCODE)" }
if ($dirty.Count -ne 0) {
    throw "librime worktree is not clean ($($dirty.Count) entries); refusing to build over modified upstream sources"
}
Write-Host 'librime worktree            : clean (no upstream modifications)'

# The four submodules this configuration compiles. glog and googletest are
# deliberately absent: ENABLE_LOGGING=OFF removes find_package(Glog REQUIRED)
# (third_party/librime/CMakeLists.txt:76-83) and BUILD_TEST=OFF removes
# find_package(GTest REQUIRED) (:118-119), so neither needs to be fetched.
$RequiredSubpaths = @('deps/yaml-cpp', 'deps/leveldb', 'deps/marisa-trie', 'deps/opencc')
foreach ($want in $RequiredSubpaths) {
    if (-not ($pkg.declared_submodules | Where-Object { $_.path -eq $want })) {
        throw "sources.lock.json declares no submodule for '$want'"
    }
}
Write-Host "submodules required by this configuration : $($RequiredSubpaths -join ', ')"

foreach ($sub in $pkg.declared_submodules) {
    $state = (& $Git.Source -C $RimeSrc submodule status -- $sub.path).Trim()
    if ($LASTEXITCODE -ne 0) { throw "git submodule status failed for $($sub.path)" }
    $isFetched = ($RequiredSubpaths -contains $sub.path)
    if ($isFetched) {
        if ($state -match '^-') {
            throw "required submodule '$($sub.path)' is not initialised; run: git -C `"$RimeSrc`" submodule update --init --depth 1 $($sub.path)"
        }
        if ($state -match '^\+') {
            throw "submodule '$($sub.path)' HEAD does not equal the gitlink recorded by librime ($state)"
        }
    }
    Write-Host ("submodule {0,-20} {1}  [{2}]" -f $sub.path, $state.Split(' ')[0], $(if ($isFetched) { 'required' } else { 'not built with this configuration' }))
}

# Each fetched submodule must sit on the commit librime recorded, and on the commit
# sources.lock.json says was verified, with a clean worktree of its own.
foreach ($sub in $pkg.declared_submodules) {
    if ($RequiredSubpaths -notcontains $sub.path) { continue }
    $subDir = Join-Path $RimeSrc ($sub.path -replace '/', '\')
    $subHead = (& $Git.Source -C $subDir rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw "git rev-parse HEAD failed in $subDir (exit $LASTEXITCODE)" }
    Assert-SameString $sub.recorded_commit $subHead "submodule $($sub.path) HEAD vs lock recorded_commit"
    $subDirty = @(& $Git.Source -C $subDir status --porcelain)
    if ($LASTEXITCODE -ne 0) { throw "git status failed in $subDir (exit $LASTEXITCODE)" }
    if ($subDirty.Count -ne 0) { throw "submodule '$($sub.path)' worktree is not clean ($($subDirty.Count) entries)" }
    Write-Host ("  verified + clean : {0} @ {1}" -f $sub.path, $subHead)
}

# ---------------------------------------------------------------------------
# 3. Boost: header-only tree, verified by version header, no build step here
# ---------------------------------------------------------------------------

Write-Head 'Boost (headers only)'

$BoostVersionH = Join-Path $BoostRoot 'boost\version.hpp'
Test-PathOrThrow $BoostVersionH 'Boost version header'
$versionText = Get-Content -LiteralPath $BoostVersionH -Raw
if ($versionText -notmatch '#define\s+BOOST_VERSION\s+(\d+)') {
    throw "cannot read BOOST_VERSION from $BoostVersionH"
}
$BoostVersion = $Matches[1]
if ($BoostVersion -ne '108900') { throw "BOOST_VERSION is $BoostVersion, expected 108900 (1.89.0)" }
Write-Host "Boost tree     : $BoostRoot"
Write-Host "BOOST_VERSION  : $BoostVersion"
foreach ($lib in 'regex', 'interprocess', 'signals2', 'uuid', 'unordered', 'algorithm', 'crc', 'range') {
    $srcCount = 0
    $srcDir = Join-Path $BoostRoot "libs\$lib\src"
    if (Test-Path -LiteralPath $srcDir) {
        $srcCount = @(Get-ChildItem -LiteralPath $srcDir -Filter '*.cpp' -ErrorAction SilentlyContinue).Count
    }
    Write-Host ("  libs/{0,-14} compiled sources = {1}" -f $lib, $srcCount)
}
$BoostLicense = Join-Path $BoostRoot 'LICENSE_1_0.txt'
Test-PathOrThrow $BoostLicense 'Boost LICENSE_1_0.txt'
$BoostLicenseHash = (Get-FileHash -LiteralPath $BoostLicense -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "Boost LICENSE_1_0.txt sha256 = $BoostLicenseHash"

# ---------------------------------------------------------------------------
# 4. shared CMake arguments
# ---------------------------------------------------------------------------

$FlagOverrides = Join-Path $RimeSrc 'cmake'
$CommonArgs = @(
    '-G', $Generator
    '-A', 'x64'
    ('-DCMAKE_GENERATOR_INSTANCE:PATH=' + $VsRoot)
    '-DCMAKE_CONFIGURATION_TYPES:STRING=Release'
    '-DCMAKE_BUILD_TYPE:STRING=Release'
    ('-DCMAKE_USER_MAKE_RULES_OVERRIDE:PATH=' + (Join-Path $FlagOverrides 'c_flag_overrides.cmake'))
    ('-DCMAKE_USER_MAKE_RULES_OVERRIDE_CXX:PATH=' + (Join-Path $FlagOverrides 'cxx_flag_overrides.cmake'))
    ('-DCMAKE_PROJECT_INCLUDE:PATH=' + $ReleasePathsHook)
    '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>'
    ('-DCMAKE_INSTALL_PREFIX:PATH=' + $Prefix)
    '-DBUILD_SHARED_LIBS:BOOL=OFF'
)
# The Visual Studio generator is multi-config, so the configuration is selected at
# build time; --verbose is what makes the real cl.exe command lines reach the log.
$BuildArgs = @('--config', 'Release')
if ($Verbose_Build) { $BuildArgs += '--verbose' }

# CMake ignores most -D values when a cache already exists, and
# CMAKE_USER_MAKE_RULES_OVERRIDE is read only by the very first configure of a
# build directory. Reusing a directory built with different flags therefore
# silently produces a different CRT than requested, so it is refused instead.
function Assert-NoStaleCache {
    param([Parameter(Mandatory)][string]$BuildDir,
          [Parameter(Mandatory)][string]$Stage)
    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cache)) { return }
    $gen = Get-CacheEntry -BuildDir $BuildDir -Key 'CMAKE_GENERATOR'
    $cfgTypes = Get-CacheEntry -BuildDir $BuildDir -Key 'CMAKE_CONFIGURATION_TYPES'
    $cxxRelease = Get-CacheEntry -BuildDir $BuildDir -Key 'CMAKE_CXX_FLAGS_RELEASE'
    if ($gen -and $gen -ne $Generator) {
        throw "${Stage}: $BuildDir was configured with generator '$gen', not '$Generator'. Rerun with -Fresh."
    }
    if ($cfgTypes -and $cfgTypes -ne 'Release') {
        throw "${Stage}: $BuildDir was configured with CMAKE_CONFIGURATION_TYPES='$cfgTypes'. Rerun with -Fresh."
    }
    if ($cxxRelease -and $cxxRelease -notmatch '/MT') {
        throw "${Stage}: $BuildDir already has CMAKE_CXX_FLAGS_RELEASE='$cxxRelease', which is not the static CRT. The make-rules override only applies to a fresh build directory. Rerun with -Fresh."
    }
    Write-Host "  reusing an existing cache that already matches this configuration"
}

function New-Dependency {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$SourceDir,
        [Parameter(Mandatory)][string[]]$ExtraArgs,
        [hashtable]$Expect = @{}
    )
    $bin = Join-Path $DepBuild $Name
    if (-not (Test-Path -LiteralPath $bin)) { New-Item -ItemType Directory -Path $bin | Out-Null }
    Assert-NoStaleCache -BuildDir $bin -Stage $Name

    Write-Head "dependency: $Name"
    $cfg = @('-S', $SourceDir, '-B', $bin) + $CommonArgs + $ExtraArgs
    Invoke-BaselineStep "$Name.configure" $CMake $cfg | Out-Null
    $Expect['CMAKE_INSTALL_PREFIX'] = $Prefix
    $Expect['CMAKE_CONFIGURATION_TYPES'] = 'Release'
    Assert-CacheValue -BuildDir $bin -Expect $Expect -Stage $Name
    Invoke-BaselineStep "$Name.build" $CMake (@('--build', $bin) + $BuildArgs) | Out-Null
    Invoke-BaselineStep "$Name.install" $CMake (@('--install', $bin, '--config', 'Release')) | Out-Null
    return $bin
}

# -File does not split an argument list: "powershell.exe -File this.ps1 -Skip
# marisa,yaml-cpp" binds ONE element, "marisa,yaml-cpp". Re-splitting here makes
# the -Skip contract the same whether the caller used -File or -Command, and stops
# a comma-list from silently skipping nothing (which would rebuild the chain and
# look like a normal run).
$SkipNorm = @($Skip | ForEach-Object { $_ -split ',' } |
    ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' } |
    ForEach-Object { $_.ToLowerInvariant() })
$unknownSkip = @($SkipNorm | Where-Object {
    $_ -notin @('marisa', 'yaml-cpp', 'leveldb', 'opencc', 'rime') })
if ($unknownSkip.Count -ne 0) {
    throw "-Skip accepts marisa, yaml-cpp, leveldb, opencc, rime; got: $($unknownSkip -join ', ')"
}
function Test-Skipped { param([string]$Name) return ($SkipNorm -contains $Name.ToLowerInvariant()) }

# A component build does not rebuild the dependency chain: rebuilding it with a
# second set of flags is exactly how two engines drift apart. It reuses the
# libraries the baseline mode installed, and this refuses to continue when they are
# not there rather than letting find_library() reach out to a system copy -- the
# rime configure step re-passes CMAKE_PREFIX_PATH=$Prefix and the cache values are
# asserted to lie under it further down, the same way the baseline run asserts them.
if ($IsComponentMode) {
    $SkipNorm = @('marisa', 'yaml-cpp', 'leveldb', 'opencc')
    foreach ($l in 'marisa.lib', 'yaml-cpp.lib', 'leveldb.lib', 'opencc.lib') {
        Test-PathOrThrow (Join-Path $Prefix "lib\$l") `
            "baseline dependency $l (run -Mode baseline first)"
    }
    Test-PathOrThrow (Join-Path $Prefix 'include\yaml-cpp') 'baseline dependency headers'
    Write-Host "reusing the baseline dependency libraries from $Prefix"

    $componentInputs = @($ComponentHook, $ComponentRoot)
    foreach ($name in $ComponentNames) {
        $dir = Join-Path $ComponentRoot $name
        $componentInputs += @(Get-ChildItem -LiteralPath $dir -File -Recurse |
            ForEach-Object { $_.FullName })
    }
    if ($ComponentNames -contains 'english') {
        $componentInputs += @(Get-ChildItem -LiteralPath $EnCoreDir -File -Recurse |
            ForEach-Object { $_.FullName })
    }
    # Every file the mode compiles in must exist before configuring: a renamed
    # source would otherwise produce an engine that is quietly missing a feature.
    foreach ($f in $componentInputs) {
        if (-not (Test-Path -LiteralPath $f)) {
            throw "component build input missing: $f"
        }
    }
    Write-Host ("  component inputs : {0} file(s) verified" -f $componentInputs.Count)
}

# ---------------------------------------------------------------------------
# 5. dependency build order
# ---------------------------------------------------------------------------

$marisaSrc = Join-Path $RimeSrc 'deps\marisa-trie'
if (-not (Test-Skipped 'marisa')) {
    New-Dependency -Name 'marisa' -SourceDir $marisaSrc -ExtraArgs @(
        '-DENABLE_TOOLS:BOOL=OFF',
        '-DBUILD_TESTING:BOOL=OFF',
        '-DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON'
    ) -Expect @{
        ENABLE_TOOLS      = 'OFF'
        BUILD_SHARED_LIBS = 'OFF'
    } | Out-Null
} else { Write-Host 'skipping marisa' }

$yamlSrc = Join-Path $RimeSrc 'deps\yaml-cpp'
if (-not (Test-Skipped 'yaml-cpp')) {
    New-Dependency -Name 'yaml-cpp' -SourceDir $yamlSrc -ExtraArgs @(
        '-DBUILD_TESTING:BOOL=OFF',
        '-DYAML_CPP_BUILD_CONTRIB:BOOL=OFF',
        '-DYAML_CPP_BUILD_TESTS:BOOL=OFF',
        '-DYAML_CPP_BUILD_TOOLS:BOOL=OFF',
        '-DYAML_MSVC_SHARED_RT:BOOL=OFF',
        '-DYAML_CPP_INSTALL:BOOL=ON',
        '-DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON'
    ) -Expect @{
        YAML_BUILD_SHARED_LIBS = 'OFF'
        YAML_MSVC_SHARED_RT    = 'OFF'
        YAML_CPP_BUILD_TESTS   = 'OFF'
        YAML_CPP_BUILD_TOOLS   = 'OFF'
        YAML_CPP_BUILD_CONTRIB = 'OFF'
    } | Out-Null
} else { Write-Host 'skipping yaml-cpp' }

$leveldbSrc = Join-Path $RimeSrc 'deps\leveldb'
if (-not (Test-Skipped 'leveldb')) {
    New-Dependency -Name 'leveldb' -SourceDir $leveldbSrc -ExtraArgs @(
        '-DLEVELDB_BUILD_TESTS:BOOL=OFF',
        '-DLEVELDB_BUILD_BENCHMARKS:BOOL=OFF',
        '-DLEVELDB_INSTALL:BOOL=ON'
    ) -Expect @{
        LEVELDB_BUILD_TESTS     = 'OFF'
        LEVELDB_BUILD_BENCHMARKS = 'OFF'
        LEVELDB_INSTALL         = 'ON'
        BUILD_SHARED_LIBS       = 'OFF'
    } | Out-Null
} else { Write-Host 'skipping leveldb' }

$openccSrc = Join-Path $RimeSrc 'deps\opencc'
if (-not (Test-Skipped 'opencc')) {
    $hook = Join-Path $RepoRoot 'tools\cmake\opencc_external_marisa_includes.cmake'
    Test-PathOrThrow $hook 'external-marisa CMake project hook'

    # Measured at these pins: OpenCC 1.1.9's USE_SYSTEM_MARISA=ON branch compiles
    # against the pinned marisa 0.3.1, but only with two additions that upstream
    # performs for the bundled copy alone -- an include directory
    # (deps/opencc/src/CMakeLists.txt:6-8) and a link directory for the bare
    # `marisa` name it links (:118). Both are supplied through CMake's documented
    # CMAKE_PROJECT_<projectName>_INCLUDE hook, so no upstream file is edited.
    # marisa 0.3.1's public headers use std::string_view and nested namespaces, so
    # OpenCC must be compiled at C++17; its own CMakeLists sets -std=c++14 for GNU
    # and Clang and nothing for MSVC (deps/opencc/CMakeLists.txt:155-178).
    $openccExtra = @(
        ('-DCMAKE_PREFIX_PATH:PATH=' + $Prefix)
        '-DCMAKE_CXX_STANDARD:STRING=17'
        '-DUSE_SYSTEM_MARISA:BOOL=ON'
        '-DBUILD_TESTING:BOOL=OFF'
        '-DENABLE_GTEST:BOOL=OFF'
        '-DENABLE_BENCHMARK:BOOL=OFF'
        '-DBUILD_PYTHON:BOOL=OFF'
        '-DBUILD_DOCUMENTATION:BOOL=OFF'
        ('-DCMAKE_PROJECT_opencc_INCLUDE:PATH=' + $hook)
        ('-DEPOCANVA_MARISA_ROOT:PATH=' + $Prefix)
    )

    # deps/opencc/data/CMakeLists.txt:1 is find_package(PythonInterp REQUIRED), and
    # :39-44 declares add_custom_target(Dictionaries ALL ...), whose output the
    # install rules at :150-155 reference. Dictionary generation is therefore part
    # of OpenCC's default build-and-install, not an optional extra. This entry does
    # not install an interpreter; it uses one that is already present.
    $py = Get-Command python.exe -ErrorAction SilentlyContinue
    if (-not $py) {
        throw 'OpenCC needs a Python interpreter at configure time (deps/opencc/data/CMakeLists.txt:1). None is on PATH. This build entry installs no software; provide an interpreter and rerun.'
    }
    $pyVersion = (& $py.Source --version 2>&1 | Select-Object -First 1) -join ' '
    $openccExtra += ('-DPYTHON_EXECUTABLE:FILEPATH=' + $py.Source)
    Write-Host "Python for OpenCC : $($py.Source) ($pyVersion)"

    $openccBin = Join-Path $DepBuild 'opencc'
    New-Dependency -Name 'opencc' -SourceDir $openccSrc -ExtraArgs $openccExtra -Expect @{
        USE_SYSTEM_MARISA    = 'ON'
        ENABLE_GTEST         = 'OFF'
        ENABLE_BENCHMARK     = 'OFF'
        BUILD_PYTHON         = 'OFF'
        BUILD_SHARED_LIBS    = 'OFF'
        CMAKE_CXX_STANDARD   = '17'
        CMAKE_INSTALL_PREFIX = $Prefix
    } | Out-Null

    # The one marisa OpenCC links must be the pinned 0.3.1 in the prefix, never the
    # 0.2.6 tree that OpenCC vendors in its own source directory.
    $libmarisa = Get-CacheEntry -BuildDir $openccBin -Key 'LIBMARISA'
    if ([string]::IsNullOrWhiteSpace($libmarisa)) {
        throw 'opencc: USE_SYSTEM_MARISA=ON but find_library(LIBMARISA) found nothing'
    }
    if ($libmarisa.Replace('\', '/').ToLowerInvariant().StartsWith($Prefix.Replace('\', '/').ToLowerInvariant()) -eq $false) {
        throw "opencc: LIBMARISA came from outside the project prefix: $libmarisa"
    }
    Write-Host "  opencc linked marisa : $libmarisa"

    # OpenCC's install set must not overwrite that file; it would silently swap the
    # marisa version librime later resolves.
    $stray = @(Get-ChildItem -LiteralPath (Join-Path $Prefix 'lib') -Filter 'marisa*' -File)
    $pinned = @(Get-ChildItem -LiteralPath (Join-Path $Prefix 'lib') -Filter 'marisa.lib' -File)
    if ($stray.Count -ne 1 -or $pinned.Count -ne 1) {
        throw "prefix/lib must contain exactly one marisa library; found: $(($stray | ForEach-Object Name) -join ', ')"
    }
    # Prove the bundled 0.2.6 really was not built: with USE_SYSTEM_MARISA=ON,
    # deps/opencc/CMakeLists.txt:209-211 must not add_subdirectory(deps/marisa-0.2.6),
    # so no marisa project may exist anywhere in OpenCC's own build tree.
    $bundledMarisaProjects = @(Get-ChildItem -LiteralPath $openccBin -Recurse -Filter 'marisa.vcxproj' -File -ErrorAction SilentlyContinue)
    $bundledMarisaMentions = @(Select-String -LiteralPath (Join-Path $Logs ((Get-ChildItem $Logs -Filter '*opencc.build*.log')[0].Name)) -Pattern 'marisa-0\.2\.6' -ErrorAction SilentlyContinue)
    if ($bundledMarisaProjects.Count -ne 0 -or $bundledMarisaMentions.Count -ne 0) {
        throw "opencc built its bundled marisa-0.2.6 despite USE_SYSTEM_MARISA=ON ($($bundledMarisaProjects.Count) project(s), $($bundledMarisaMentions.Count) log line(s))"
    }
    Write-Host '  opencc bundled marisa-0.2.6 : not configured, not built, not referenced in the build log'
    Write-Host ("  prefix/lib marisa copies : " + (($stray | ForEach-Object { $_.Name }) -join ', '))
} else { Write-Host 'skipping opencc' }

# ---------------------------------------------------------------------------
# 6. librime itself -- unmodified sources, out-of-tree build
# ---------------------------------------------------------------------------

if (-not (Test-Skipped 'rime')) {
    Write-Head 'librime'
    $rimeArgs = @(
        '-S', $RimeSrc, '-B', $RimeBuild,
        '-G', $Generator, '-A', 'x64'
        ('-DCMAKE_GENERATOR_INSTANCE:PATH=' + $VsRoot)
        '-DCMAKE_CONFIGURATION_TYPES:STRING=Release'
        '-DCMAKE_BUILD_TYPE:STRING=Release'
        ('-DCMAKE_PROJECT_INCLUDE:PATH=' + $ReleasePathsHook)
        '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>'
        ('-DCMAKE_PREFIX_PATH:PATH=' + $Prefix)
        ('-DBOOST_ROOT:PATH=' + $BoostRoot)
        ('-DBoost_INCLUDE_DIR:PATH=' + $BoostRoot)
        ('-DCMAKE_INSTALL_PREFIX:PATH=' + $RimeInst)
        '-DBUILD_SHARED_LIBS:BOOL=ON'
        '-DBUILD_STATIC:BOOL=ON'
        '-DBUILD_TEST:BOOL=OFF'
        '-DBUILD_DATA:BOOL=OFF'
        '-DBUILD_SAMPLE:BOOL=OFF'
        '-DBUILD_SEPARATE_LIBS:BOOL=OFF'
        '-DBUILD_MERGED_PLUGINS:BOOL=ON'
        '-DENABLE_LOGGING:BOOL=OFF'
        '-DENABLE_EXTERNAL_PLUGINS:BOOL=OFF'
        '-DINSTALL_PRIVATE_HEADERS:BOOL=OFF'
        '-DENABLE_ASAN:BOOL=OFF'
        '-DENABLE_THREADING:BOOL=ON'
        '-DENABLE_TIMESTAMP:BOOL=ON'
    )
    # No RIME_PLUGINS environment variable -> plugins/CMakeLists.txt:24-40 finds no
    # plugin directories and builds none.
    if (Test-Path env:RIME_PLUGINS) { Remove-Item env:RIME_PLUGINS }
    if ($IsComponentMode) {
        # EpoCanva's own components are compiled INTO this target through CMake's
        # documented CMAKE_PROJECT_<projectName>_INCLUDE injection point, which
        # exists for "injecting custom code into project builds without modifying
        # their source". Upstream's own plugin mechanism was checked first and has
        # no entry that accepts an out-of-tree source path:
        #   plugins/CMakeLists.txt:29-30 strips every directory prefix off
        #     ENV{RIME_PLUGINS} and turns spaces into ';',
        #   plugins/CMakeLists.txt:33-39 otherwise globs third_party/librime/plugins/*,
        #   sample/CMakeLists.txt:36 states the official instruction is
        #     "link or copy the directory to plugins/" -- i.e. it would create
        #     entries inside the upstream checkout, which AGENTS.md section 7 forbids.
        # The hook does not touch kDefaultModules; the front end names the module in
        # RimeTraits.modules instead (docs/adr/0004-compiled-in-rime-components.md).
        #
        # EPOCANVA_RIME_COMPONENT_NAMES is passed as a single ';'-separated value:
        # CMake reads a :-list in one -D as a list, and a space-joined form would be
        # unsafe because this repository's path contains spaces.
        $rimeArgs += @(
            ('-DCMAKE_PROJECT_rime_INCLUDE:PATH=' + $ComponentHook)
            ('-DEPOCANVA_RIME_COMPONENT_ROOT:PATH=' + $ComponentRoot)
            ('-DEPOCANVA_RIME_COMPONENT_NAMES:STRING=' + ($ComponentNames -join ';'))
            ('-DEPOCANVA_RIME_HOOK_LABEL:STRING=' + $HookLabel)
            ('-DEPOCANVA_BOOST_INCLUDE_DIR:PATH=' + $BoostRoot)
        )
        if ($ComponentNames -contains 'english') {
            $rimeArgs += ('-DEPOCANVA_EN_CORE_DIR:PATH=' + $EnCoreDir)
        }
    }

    Assert-NoStaleCache -BuildDir $RimeBuild -Stage 'librime'
    $rimeConfigureOut = Invoke-BaselineStep 'rime.configure' $CMake $rimeArgs

    Assert-CacheValue -BuildDir $RimeBuild -Stage 'librime' -Expect @{
        BUILD_SHARED_LIBS         = 'ON'
        BUILD_STATIC              = 'ON'
        BUILD_TEST                = 'OFF'
        BUILD_DATA                = 'OFF'
        BUILD_SAMPLE              = 'OFF'
        BUILD_SEPARATE_LIBS       = 'OFF'
        ENABLE_LOGGING            = 'OFF'
        ENABLE_EXTERNAL_PLUGINS   = 'OFF'
        INSTALL_PRIVATE_HEADERS   = 'OFF'
        ENABLE_ASAN               = 'OFF'
        CMAKE_CONFIGURATION_TYPES = 'Release'
    }
    # Provenance: every located dependency must be the one this build installed.
    # A find_path()/find_library() hit from a stray system copy would otherwise
    # look like a successful configure.
    Assert-CacheUnder -BuildDir $RimeBuild -Stage 'librime' -Root $Prefix -Keys @(
        'YamlCpp_LIBRARY', 'YamlCpp_INCLUDE_PATH',
        'LevelDb_LIBRARY', 'LevelDb_INCLUDE_PATH',
        'Marisa_LIBRARY', 'Marisa_INCLUDE_PATH',
        'Opencc_LIBRARY', 'Opencc_INCLUDE_PATH'
    )
    $boostDir = Get-CacheEntry -BuildDir $RimeBuild -Key 'Boost_INCLUDE_DIR'
    Write-Host "  provenance ok : Boost_INCLUDE_DIR = $boostDir"
    if ($boostDir.Replace('\','/').ToLowerInvariant().StartsWith($BoostRoot.Replace('\','/').ToLowerInvariant()) -eq $false) {
        throw "librime: Boost came from outside the pinned tree: $boostDir"
    }
    $boostLibs = Get-CacheEntry -BuildDir $RimeBuild -Key 'Boost_LIBRARIES'
    Write-Host "  Boost_LIBRARIES = '$boostLibs' (empty is expected: the headers librime uses are header-only in Boost 1.89.0)"
    $iconv = Get-CacheEntry -BuildDir $RimeBuild -Key 'ICONV_LIBRARY'
    Write-Host "  ICONV_LIBRARY   = '$iconv'"

    if ($IsComponentMode) {
        # The hook variables must have actually landed in the cache: a typo in a
        # -D name is silently ignored by CMake, and a component build that silently
        # did not happen would produce a baseline-looking DLL.
        $componentCacheExpect = @{
            EPOCANVA_RIME_COMPONENT_ROOT  = $ComponentRoot
            EPOCANVA_RIME_COMPONENT_NAMES = ($ComponentNames -join ';')
            EPOCANVA_RIME_HOOK_LABEL      = $HookLabel
            CMAKE_PROJECT_rime_INCLUDE    = $ComponentHook
            EPOCANVA_BOOST_INCLUDE_DIR    = $BoostRoot
            BUILD_MERGED_PLUGINS          = 'ON'
            ENABLE_EXTERNAL_PLUGINS       = 'OFF'
        }
        if ($ComponentNames -contains 'english') {
            $componentCacheExpect['EPOCANVA_EN_CORE_DIR'] = $EnCoreDir
        }
        Assert-CacheValue -BuildDir $RimeBuild -Stage 'librime-component' `
            -Expect $componentCacheExpect
        # The hook's own attach step runs as a deferred call at the end of the
        # top-level directory scope. If it did not report attaching, the component
        # is not in the target and the build must not be reported as a component
        # build. The label is mode-specific, so a P1.4 attach cannot certify a
        # P2.1 build.
        $attached = @($rimeConfigureOut |
            Select-String -SimpleMatch ("EpoCanva {0} ATTACHED:" -f $HookLabel))
        if ($attached.Count -ne 1) {
            # Parenthesised: -f binds tighter than + in PowerShell, so an
            # unparenthesised concatenation formats only its last fragment and the
            # {0} placeholder prints literally.
            throw (("librime-component: the component hook did not report " +
                    "attaching exactly once for label $HookLabel (found {0}); " +
                    "configure output is in the log") -f $attached.Count)
        }
        Write-Host ('  ' + $attached[0].Line.Trim())
        $armed = @($rimeConfigureOut |
            Select-String -SimpleMatch 'component hook armed')
        if ($armed.Count -ne 1) {
            throw 'librime-component: the hook was not armed for project rime'
        }
    }

    Invoke-BaselineStep 'rime.build' $CMake (@('--build', $RimeBuild) + $BuildArgs) | Out-Null

    if ($IsComponentMode) {
        # The component's objects must exist, by name. Their CRT ABI tag is checked
        # together with every other artefact in the post-build verification section
        # below. Listing them individually matters: "some extra .obj appeared" would
        # also be true if CMake produced a stray target, whereas these names prove
        # the specific translation units were compiled.
        $componentObjs = @(Get-ChildItem -LiteralPath $RimeBuild -Recurse -Filter '*.obj' -File |
            Where-Object { $_.FullName -match '\\epocanva-rime-components\\' -and
                           $_.Name -in $ComponentObjNames })
        $missing = @($ComponentObjNames | Where-Object {
            $_ -notin ($componentObjs | ForEach-Object Name) })
        if ($missing.Count -ne 0) {
            throw ("librime-component: expected component objects missing under " +
                   "$RimeBuild\epocanva-rime-components: $($missing -join ', ')")
        }
        $componentObjs | ForEach-Object { Write-Host ("  component object : " +
            $_.FullName.Replace($RepoRoot, '<repo>')) }
        Write-Host ("  component objects: {0} of {1} expected present" -f `
            $componentObjs.Count, $ComponentObjNames.Count)
    }
    Invoke-BaselineStep 'rime.install' $CMake @('--install', $RimeBuild, '--config', 'Release') | Out-Null
} else { Write-Host 'skipping rime' }

# ---------------------------------------------------------------------------
# 7. post-conditions: prove architecture, CRT and dependency provenance
# ---------------------------------------------------------------------------

Write-Head 'post-build verification'

if (Test-Skipped 'rime') {
    Write-Host "skipping the rime.dll verification because 'rime' was in -Skip" -ForegroundColor Yellow
    return
}

$RimeDll = $null
foreach ($candidate in @((Join-Path $RimeInst 'lib\rime.dll'), (Join-Path $RimeInst 'bin\rime.dll'))) {
    if (Test-Path -LiteralPath $candidate) { $RimeDll = $candidate; break }
}
if (-not $RimeDll) {
    throw "rime.dll not found under $RimeInst (looked in lib/ and bin/)"
}
# Upstream installs the shared object with install(TARGETS rime DESTINATION
# ${CMAKE_INSTALL_FULL_LIBDIR}) (src/CMakeLists.txt:95), so on Windows both the
# DLL and the import library land in lib/, while tools/CMakeLists.txt:40-42 puts
# rime_deployer/rime_dict_manager/rime_patch.exe in bin/. Those exes therefore do
# not find rime.dll without an explicit search-path rule -- recorded for the phase
# that wires up the adapter, not fixed here.
$RimeLib = Join-Path $RimeInst 'lib\rime.lib'
Test-PathOrThrow $RimeLib 'rime import library'
Write-Host ("rime.dll           : " + $RimeDll.Replace($RepoRoot, '<repo>'))
Write-Host ("rime.dll size      : " + (Get-Item $RimeDll).Length + ' bytes')

$hdr = Invoke-BaselineStep 'verify.dumpbin-headers' 'dumpbin.exe' @('/headers', $RimeDll)
$machineLine = @($hdr | Where-Object { $_ -match 'machine \(' }) | Select-Object -First 1
if (-not $machineLine) { throw 'could not read the machine type from dumpbin /headers output' }
Write-Host ("rime.dll machine : " + $machineLine.Trim())
if ($machineLine -notmatch 'x64') { throw "rime.dll is not an x64 image: $($machineLine.Trim())" }

$deps = Invoke-BaselineStep 'verify.dumpbin-dependents' 'dumpbin.exe' @('/dependents', $RimeDll)
$depList = @($deps | Where-Object { $_ -match '^\s{4}\S+\.dll\s*$' } | ForEach-Object { $_.Trim() })
Write-Host 'rime.dll imported DLLs:'
$depList | ForEach-Object { Write-Host "  $_" }

$vcRuntime = @($depList | Where-Object { $_ -match '^(VCRUNTIME|MSVCP|ucrt|vcruntime)' })
if ($vcRuntime.Count -ne 0) {
    throw "static CRT did NOT take effect: rime.dll still imports the VC runtime DLL(s): $($vcRuntime -join ', ')"
}
Write-Host 'no VC runtime DLL imported -> static CRT confirmed at the binary level'

$thirdParty = @($depList | Where-Object { $_ -match '^(marisa|opencc|leveldb|yaml-cpp|glog|gtest|icudt|icuuc|icuin)' })
if ($thirdParty.Count -ne 0) {
    throw "dependencies were built as DLLs, violating the static-dependency rule: $($thirdParty -join ', ')"
}

# --- CRT evidence from the linker directives actually emitted into the objects ---
# cl writes an ABI tag into every translation unit: /FAILIFMISMATCH:RuntimeLibrary=<name>
# plus /DEFAULTLIB directives. Reading them with dumpbin is evidence taken from the
# produced binaries, not from a cache entry or an echoed option, and the linker
# refuses to mix them -- which is exactly the failure mode we must not paper over.
# Note the directives are spelled without ".lib" and not in a fixed case
# (/DEFAULTLIB:LIBCMT, /DEFAULTLIB:libcpmt), hence the case-insensitive counts.
function Get-CrtFromDirectives {
    param([Parameter(Mandatory)][string]$Artifact)
    $resolved = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if (-not $resolved) { throw 'dumpbin.exe is not available' }
    $d = & $resolved.Source /directives $Artifact 2>&1 | ForEach-Object { $_.ToString() }
    if ($LASTEXITCODE -ne 0) { throw "dumpbin /directives failed on $Artifact (exit $LASTEXITCODE)" }
    $text = $d -join "`n"
    [pscustomobject]@{
        Artifact      = Split-Path $Artifact -Leaf
        MT_Static     = ([regex]::Matches($text, 'RuntimeLibrary=MT_StaticRelease', 'IgnoreCase')).Count
        MD_Dynamic    = ([regex]::Matches($text, 'RuntimeLibrary=MD_DynamicRelease', 'IgnoreCase')).Count
        DebugCRT      = ([regex]::Matches($text, 'RuntimeLibrary=(MTd_StaticDebug|MDd_DynamicDebug)', 'IgnoreCase')).Count
        defaultLibcmt = ([regex]::Matches($text, '/DEFAULTLIB:\s*"?libcmt"?', 'IgnoreCase')).Count
        defaultMsvcrt = ([regex]::Matches($text, '/DEFAULTLIB:\s*"?msvcrt"?\.?lib?', 'IgnoreCase')).Count
    }
}

Write-Host ''
Write-Host 'CRT ABI tags per built artefact (Release/static expects MT_StaticRelease only):'
$depLibs = Get-ChildItem -LiteralPath (Join-Path $Prefix 'lib') -Filter '*.lib' -File -ErrorAction SilentlyContinue
$crtRows = @()
foreach ($lib in $depLibs) { $crtRows += Get-CrtFromDirectives -Artifact $lib.FullName }
$rimeObj = @(Get-ChildItem -LiteralPath $RimeBuild -Recurse -Filter '*.obj' -File -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\rime\.dir\\' } | Select-Object -First 3)
foreach ($o in $rimeObj) { $crtRows += Get-CrtFromDirectives -Artifact $o.FullName }
if ($IsComponentMode) {
    # The component objects are checked with the same ABI-tag rule as the engine and
    # its dependencies, so a mixed CRT cannot pass this script. The count is derived
    # from the mode's expected translation-unit list rather than hardcoded, and the
    # objects must be exactly those: one extra or one missing is a build-graph
    # change, not a detail.
    $probeObjRows = @(Get-ChildItem -LiteralPath $RimeBuild -Recurse -Filter '*.obj' -File |
        Where-Object { $_.FullName -match '\\epocanva-rime-components\\' } |
        ForEach-Object { Get-CrtFromDirectives -Artifact $_.FullName })
    if ($probeObjRows.Count -ne $ComponentObjNames.Count) {
        throw ("librime-component: expected {0} component objects to check, found {1}" -f `
            $ComponentObjNames.Count, $probeObjRows.Count)
    }
    $crtRows += $probeObjRows
}
$crtRows += Get-CrtFromDirectives -Artifact $RimeLib
$crtRows += Get-CrtFromDirectives -Artifact $RimeDll
foreach ($r in $crtRows) {
    Write-Host ("  {0,-28} MT_Static={1} MD_Dynamic={2} DebugCRT={3} DEFAULTLIB:libcmt={4} DEFAULTLIB:msvcrt={5}" -f `
        $r.Artifact, $r.MT_Static, $r.MD_Dynamic, $r.DebugCRT, $r.defaultLibcmt, $r.defaultMsvcrt)
}
$bad = @($crtRows | Where-Object { $_.MD_Dynamic -ne 0 -or $_.DebugCRT -ne 0 -or $_.defaultMsvcrt -ne 0 })
if ($bad.Count -ne 0) {
    $names = ($bad | ForEach-Object { $_.Artifact }) -join ', '
    throw "CRT is not uniform: these artefacts carry a dynamic or debug CRT ABI tag: $names"
}
$noTag = @($crtRows | Where-Object { $_.MT_Static -eq 0 })
if ($noTag.Count -ne 0) {
    Write-Host ("  note: these have no RuntimeLibrary tag at all (import libraries and PE files do not carry one): " +
        (($noTag | ForEach-Object { $_.Artifact }) -join ', '))
}

# --- and the same conclusion from the real cl command lines ---
$rimeBuildLogs = @(Get-ChildItem -LiteralPath $Logs -Filter '*rime.build*.log')
if ($rimeBuildLogs.Count -eq 0) {
    throw 'no rime build log was written; cannot verify the cl command line'
}
$logText = ($rimeBuildLogs | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
$mt = [regex]::Matches($logText, '(?<![-/\w])/MT[d]?(?![\w])').Count
$md = [regex]::Matches($logText, '(?<![-/\w])/MD[d]?(?![\w])').Count
Write-Host ''
Write-Host "cl command-line CRT switches in the rime build log : /MT=$mt  /MD=$md"
if ($md -ne 0) { throw "found /MD in the recorded cl command lines; CRT would be mixed" }

# /MT == 0 has two very different causes, and the old code conflated them:
#   (a) the static CRT never reached cl -- a real failure, or
#   (b) an up-to-date incremental build, where MSBuild re-linked or skipped
#       everything and printed no cl command line at all. That is NOT a failure,
#       and requiring -Fresh to avoid it would force a full rebuild every run.
# The two are separated by whether any object was actually (re)written during this
# run, which is observable independently of the log.
$rimeObjsAll = @(Get-ChildItem -LiteralPath $RimeBuild -Recurse -Filter '*.obj' -File `
    -ErrorAction SilentlyContinue)
$recompiledObjs = @($rimeObjsAll | Where-Object { $_.LastWriteTime -ge $script:RunStart })
Write-Host ("objects written by this run: {0} of {1} in the rime build tree" -f `
    $recompiledObjs.Count, $rimeObjsAll.Count)
if ($mt -eq 0) {
    if ($recompiledObjs.Count -ne 0) {
        # Parenthesised: PowerShell's -f binds tighter than +, so without the outer
        # parentheses only the last fragment is formatted and {0} prints literally.
        throw (("cl compiled {0} object(s) this run but no /MT appeared in the " +
                'build log: the static CRT did not reach the compiler. Rerun with ' +
                '-Verbose_Build.') -f $recompiledObjs.Count)
    }
    # Nothing was recompiled, so the log legitimately contains no cl command line.
    # The CRT conclusion then rests entirely on the content-derived ABI tags above,
    # which were read from the artefacts on disk and do not depend on the log.
    $mtTagged = @($crtRows | Where-Object { $_.MT_Static -gt 0 })
    if ($mtTagged.Count -eq 0) {
        throw 'incremental build with no /MT in the log AND no MT_StaticRelease ABI tag on any artefact: nothing certifies the static CRT'
    }
    Write-Host ("  incremental: no object was recompiled this run, so the cl command-line " +
        "sample is empty by construction.")
    Write-Host (("  CRT certified instead from MT_StaticRelease directives on " +
        "{0} artefact(s) listed above; the log sample is not the evidence in " +
        "this case.") -f $mtTagged.Count)
} else {
    Write-Host "  cl command lines observed this run; /MT present and /MD absent."
}

foreach ($cache in (Get-ChildItem -LiteralPath $Baseline -Recurse -Filter 'CMakeCache.txt')) {
    $lines = Get-Content -LiteralPath $cache.FullName
    foreach ($k in 'YamlCpp_LIBRARY', 'LevelDb_LIBRARY', 'Marisa_LIBRARY', 'Opencc_LIBRARY',
        'Boost_INCLUDE_DIR', 'Boost_LIBRARIES', 'PYTHON_EXECUTABLE',
        'CMAKE_MSVC_RUNTIME_LIBRARY', 'CMAKE_CXX_FLAGS_RELEASE') {
        $hit = $lines | Where-Object { $_ -match "^$k" } | Select-Object -First 1
        if ($hit) { Write-Host ("[{0}] {1}" -f $cache.Directory.FullName.Replace($Baseline, $BuildRel), $hit) }
    }
    Write-Host ''
}

if ($IsComponentMode) {
    Write-Head 'component identity'
    $BaselineDll = Join-Path $BaselineRoot 'rime\install\lib\rime.dll'
    Test-PathOrThrow $BaselineDll 'baseline rime.dll (required for the identity comparison)'
    $hereHash = (Get-FileHash -LiteralPath $RimeDll -Algorithm SHA256).Hash.ToLowerInvariant()
    $thereHash = (Get-FileHash -LiteralPath $BaselineDll -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Host ("  component rime.dll sha256 : $hereHash")
    Write-Host ("  baseline  rime.dll sha256 : $thereHash")
    if ($hereHash -eq $thereHash) {
        throw 'the component DLL is byte-identical to the baseline: nothing was compiled in'
    }
    foreach ($needle in $ComponentDllNeedles) {
        if (-not (Test-BinaryContainsAscii -Path $RimeDll -Needle $needle)) {
            throw "the component string '$needle' is not present in the built rime.dll"
        }
        if (Test-BinaryContainsAscii -Path $BaselineDll -Needle $needle) {
            throw "the baseline rime.dll unexpectedly contains the string '$needle'"
        }
        Write-Host ("  identity ok : '$needle' present in the component DLL, absent from the baseline")
    }
    # The component must not widen the DLL's export table: reaching it goes through
    # librime's own module get_api() slot, not a new EpoCanva ABI (ADR-0004).
    $expHere = @(Invoke-BaselineStep 'verify.exports-component' 'dumpbin.exe' @('/exports', $RimeDll) |
        ForEach-Object { if ($_ -match '^\s+\d+\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+(\S+)') { $Matches[1] } } |
        Sort-Object -Unique)
    $expThere = @(Invoke-BaselineStep 'verify.exports-baseline' 'dumpbin.exe' @('/exports', $BaselineDll) |
        ForEach-Object { if ($_ -match '^\s+\d+\s+[0-9a-fA-F]+\s+[0-9a-fA-F]+\s+(\S+)') { $Matches[1] } } |
        Sort-Object -Unique)
    Write-Host ("  export name count : component {0}  baseline {1}" -f $expHere.Count, $expThere.Count)
    $added = @($expHere | Where-Object { $expThere -notcontains $_ })
    $removed = @($expThere | Where-Object { $expHere -notcontains $_ })
    if ($added.Count -ne 0) {
        throw ('the component added exported symbols, which would be a new ABI surface: ' +
               (($added | Select-Object -First 10) -join ', '))
    }
    if ($removed.Count -ne 0) {
        throw ('the component build lost exported symbols the baseline provides: ' +
               (($removed | Select-Object -First 10) -join ', '))
    }
    Write-Host '  export table unchanged -- the compiled-in component adds no new ABI'
}

# The check that guards a real phase requirement: -Mode baseline is the default and
# must keep producing an unmodified engine, so no component mode may have written
# its settings into the original baseline tree. Read from the cache on disk rather
# than from what this run intended. (Each mode already owns a distinct build root
# under build/, so the reverse direction needs no test.)
$baselineCache = Join-Path $BaselineRoot 'rime\CMakeCache.txt'
if ($IsComponentMode -and (Test-Path -LiteralPath $baselineCache)) {
    $leaked = @(Get-Content -LiteralPath $baselineCache | Where-Object {
        $_ -match '^EPOCANVA_' -or $_ -match '^CMAKE_PROJECT_rime_INCLUDE' })
    if ($leaked.Count -ne 0) {
        throw ("build/librime-baseline's rime cache carries EpoCanva component " +
               "settings: $($leaked -join ', ')")
    }
    Write-Host '  baseline isolation: build/librime-baseline rime cache has no EPOCANVA_* and no CMAKE_PROJECT_rime_INCLUDE entry'
}

Write-Head 'artefacts'
foreach ($sub in 'bin', 'lib', 'include') {
    $dir = Join-Path $RimeInst $sub
    if (-not (Test-Path -LiteralPath $dir)) { continue }
    Get-ChildItem -LiteralPath $dir -File | ForEach-Object {
        Write-Host ("  {0}\{1}  {2} bytes" -f $sub, $_.Name, $_.Length)
    }
}

# upstream side effect worth recording: third_party/librime/tools/CMakeLists.txt:56-68
# copies data/minimal/* into the build output directory whenever
# BUILD_SHARED_LIBS=ON, independently of BUILD_DATA=OFF. Those word lists carry a
# licence status that has NOT been cleared -- cangjie5.dict.yaml says "License: GPL"
# in its own header -- so anything under build/ must not be published while the
# distribution review is open.
$copiedData = @(Get-ChildItem -LiteralPath (Join-Path $RimeBuild 'bin') -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in '.yaml', '.txt' })
if ($copiedData.Count -ne 0) {
    Write-Host ''
    Write-Host ("NOTE: upstream copied {0} bundled data file(s) into the build tree:" -f $copiedData.Count)
    $copiedData | ForEach-Object {
        Write-Host ("  {0}" -f $_.FullName.Replace($Baseline, $BuildRel))
    }
    Write-Host '  data/minimal word lists are NOT licence-cleared; do not publish this tree.'
}

Assert-NoEmbeddedBuildPaths -Artifacts @($RimeDll) -Stage 'installed-rime'
if ($Mode -eq 'english-completion') {
    $identityEnd = Get-ProductEngineIdentity -RepoRoot $RepoRoot
    if ($EngineIdentityStart.source_sha256 -ne $identityEnd.source_sha256) {
        throw 'English source inputs changed during the build; no successful receipt issued.'
    }
    $receiptHash = (Get-FileHash -LiteralPath $RimeDll -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-EngineReceipt -Status 'built' -SourceSha256 $identityEnd.source_sha256 -RimeSha256 $receiptHash
    Write-Host "verified English engine receipt: $EngineReceiptPath"
}

Write-Host ''
if ($IsComponentMode) {
    Write-Host ("librime {0} build completed successfully: [{1}] compiled into rime.dll" -f `
        $Mode, ($ComponentNames -join '+')) -ForegroundColor Green
    Write-Host 'see the component identity section for the object and export evidence.'
} else {
    Write-Host 'P1.2 native baseline build completed successfully (mode: baseline).' -ForegroundColor Green
}
