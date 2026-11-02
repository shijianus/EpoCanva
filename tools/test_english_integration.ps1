<#
.SYNOPSIS
    Formal entry point for the EpoCanva English integration test suite.

.DESCRIPTION
    One pipeline, in this order: arguments and tools, configure, build, export the
    test list CTest actually generated, CTest, then the real exit code.

    It drives the modular project under tools/rime_english_recovery. It does not
    build the engine: -EnginePrefix and -BaselinePrefix point it at two librime
    installations that a build of tools/build_librime_baseline.ps1 already produced,
    and it fails with a named reason if either does not hold the artefacts the
    project needs.

    What this script guarantees, and how:

      * It never runs a stale binary behind a failed build. There is no
        test-only stage: build failure terminates the run before CTest is
        invoked, with its own exit code.
      * It never calls an empty selection "passing". Every CTest invocation carries
        --no-tests=error, and a filtered run first asks CTest for the list that
        filter matches and stops if the answer is empty.
      * It never treats the word "PASS" appearing in a log as a result. The
        judgement is CTest's exit code plus the counts parsed out of its own
        summary line; the logs are evidence, not the verdict.
      * It never lets a registered case disappear quietly. The generated CTest list
        is compared name-by-name against the registrations parsed out of
        CMakeLists.txt, in both directions, and duplicate registrations are
        reported instead of being counted as extra coverage.
      * It never overwrites a log. Each run writes files named by -LogTag and
        refuses a path that already exists, so a failed run stays readable next to
        the run that fixed it.
      * It installs nothing, downloads nothing, edits no system PATH, rebuilds no
        dependency, and cleans nothing outside the build directory it is given.

    The pilot word list is off by default. With -PilotLexicon unset, this script
    configures the project without ever mentioning the generated data, and the
    suite is the hand-made-fixture cases plus the digest helper's own. Passing it
    names is NOT what makes the data trusted: tools/rime_english_recovery/
    CMakeLists.txt reads the pinned manifest, compares the file's name, byte count
    and SHA-256 against it, and fails the CONFIGURE on any mismatch. This script
    deliberately does not re-implement that check with a second digest
    implementation -- one judgement, not two.

.PARAMETER EnginePrefix
    Installation prefix of the ENGLISH librime build, the directory holding
    include/rime_api.h, lib/rime.lib and rime.dll. Required.

.PARAMETER BaselinePrefix
    Installation prefix of the unmodified BASELINE librime build, checked the same
    way. Required: the cross-injection cases are the ones that prove the suite is
    observing an engine binary rather than a --mode string.

.PARAMETER BuildDir
    Where to configure and build. Created if missing; a CMake cache already in it
    is reused, which is what makes a second run incremental. Nothing outside this
    directory is deleted by this script.

.PARAMETER PilotLexicon
    Absolute path of the generated pilot word list. Unset by default, which
    registers no pilot case. Set but missing, relative, or not the pinned file:
    the run fails, it does not fall back to the smaller suite.

.PARAMETER PilotOutputDir
    Directory holding the generated list and its conversion report, for the
    manifest cross-check case. Defaults to the parent of -PilotLexicon.

.PARAMETER Filter
    A CTest -R regular expression, for running one behaviour group. The filtered
    selection is verified non-empty before anything runs, and the source/list
    cross-check is then applied to the filtered names only.

.PARAMETER LogTag
    Suffix for every file this run writes. Default 'r1'. If a file for that tag
    already exists the run stops rather than overwriting it: bump the tag.

.PARAMETER LogDir
    Where the logs go. Defaults to <BuildDir>/integration-logs.

.PARAMETER RepoRoot
    Repository root. Defaults to the parent of this script's own directory, so no
    machine-specific path is ever hardcoded here.

.PARAMETER SourceDir
    The CMake project to drive. Defaults to <RepoRoot>/tools/rime_english_recovery.

.PARAMETER Generator
    CMake generator. Defaults to the one the project is documented with.

.PARAMETER Platform
    CMAKE_GENERATOR_PLATFORM. Defaults to x64, which is the only platform either
    rime.lib will link against.

.PARAMETER VsInstallPath
    A Visual Studio installation directory to use instead of the one found by
    vswhere. Use it when a specific instance is needed; the script still refuses
    to invent a toolchain, and it does not repair a missing one.

.NOTES
    Exit codes. 0 is the only success value, and the CTest stage returns its own
    code unchanged so a caller sees exactly what CTest saw.
      0  every stage succeeded and the selected cases passed
      2  invocation refused: a required argument missing, malformed or pointing at
         something absent -- or a log file already existing for this -LogTag
      3  a required tool or artefact was not found (nothing is installed to fix it)
      4  configure failed (this is where a pilot identity mismatch surfaces)
      5  build failed -- no test was run, and no previous binary was reused
      6  the generated CTest list does not match what the sources register, or the
         selection was empty
      7  the pipeline got no result to judge (CTest reported no tests at all)
      8+ the CTest exit code, returned verbatim
#>
[CmdletBinding()]
param(
    [string]$EnginePrefix = '',
    [string]$BaselinePrefix = '',
    [string]$BuildDir = '',
    [string]$PilotLexicon = '',
    [string]$PilotOutputDir = '',
    [string]$Filter = '',
    [string]$LogTag = 'r1',
    [string]$LogDir = '',
    [string]$RepoRoot = '',
    [string]$SourceDir = '',
    [string]$Generator = 'Visual Studio 17 2022',
    [string]$Platform = 'x64',
    [string]$VsInstallPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Stage = 'arguments'
$script:StepFailCodes = @{
    arguments = 2
    logs      = 2
    tools     = 3
    configure = 4
    build     = 5
    list      = 6
    nocases   = 7
}

function Stop-Run {
    param([Parameter(Mandatory)][string]$Stage,
          [Parameter(Mandatory)][string]$Message)
    $script:Stage = $Stage
    $code = $script:StepFailCodes[$Stage]
    if (-not $code) { $code = 1 }
    Write-Host ''
    Write-Host "FAILED [$Stage] exit=$code  $Message"
    Write-Host 'STOPS here; nothing after this stage ran.'
    exit $code
}

function Invoke-Stage {
    param([Parameter(Mandatory)][string]$Exe, [Parameter(Mandatory)][string[]]$Arguments)
    # Native stderr would raise under ErrorActionPreference=Stop and hide the real code.
    # The whole output is consumed before anything is trimmed, because stopping a native
    # command's pipeline early (Select-Object -First) leaves $LASTEXITCODE meaningless.
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $out = & $Exe @Arguments 2>&1 | ForEach-Object { $_.ToString() }
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $prev }
    return [pscustomobject]@{Code = $code; Lines = @($out)}
}

function Resolve-FullPath {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$What)
    # Relative arguments are resolved against the caller's working directory, so a
    # path with spaces needs no quoting convention invented here.
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    $here = (Get-Location -PSProvider FileSystem).ProviderPath
    return [System.IO.Path]::GetFullPath((Join-Path $here $Path))
}

function Test-FilePresent {
    param([Parameter(Mandatory)][string]$Path)
    return (Test-Path -LiteralPath $Path -PathType Leaf)
}

# --- 1. arguments ------------------------------------------------------------------------
if ($EnginePrefix -eq '' -or $BaselinePrefix -eq '' -or $BuildDir -eq '') {
    Write-Host 'usage: tools/test_english_integration.ps1'
    Write-Host '        -EnginePrefix  <repo>/build/librime-english/rime/install'
    Write-Host '        -BaselinePrefix <repo>/build/librime-baseline/rime/install'
    Write-Host '        -BuildDir      <repo>/build/english-recovery/test'
    Write-Host '        [[-PilotLexicon <abs path to the generated word list>] [-PilotOutputDir <dir>]'
    Write-Host '         [-Filter <ctest -R regex>] [-LogTag <suffix>] [-LogDir <dir>]'
    Write-Host '         [-RepoRoot <dir>] [-SourceDir <dir>] [-Generator <name>] [-Platform x64]'
    Write-Host '         [-VsInstallPath <dir>]]'
    Write-Host ''
    Write-Host 'Every one of -EnginePrefix, -BaselinePrefix and -BuildDir is required;'
    Write-Host 'nothing is guessed for them and the pilot data is never implied.'
    Stop-Run 'arguments' 'a required argument is missing or empty.'
}

$RepoRootSpecified = ($RepoRoot -ne '')
if (-not $RepoRootSpecified) {
    $RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
}
$RepoRoot = (Resolve-FullPath $RepoRoot 'RepoRoot')
if (-not (Test-Path -LiteralPath $RepoRoot -PathType Container)) {
    Stop-Run 'arguments' "repo root does not exist: $RepoRoot"
}
if ($SourceDir -eq '') {
    $SourceDir = Join-Path $RepoRoot 'tools\rime_english_recovery'
}
$SourceDir = (Resolve-FullPath $SourceDir 'SourceDir')
$ProjectFile = Join-Path $SourceDir 'CMakeLists.txt'
if (-not (Test-FilePresent $ProjectFile)) {
    Stop-Run 'arguments' "no CMakeLists.txt in -SourceDir: $SourceDir"
}
foreach ($required_source in @('main.cc', 'recovery_test_support.h', 'spelling_toggle_tests.cc')) {
    if (-not (Test-FilePresent (Join-Path $SourceDir $required_source))) {
        Stop-Run 'arguments' ("-SourceDir is missing $required_source, which this suite's entry " +
            'and dispatch depend on: ' + $SourceDir)
    }
}

$EnginePrefix = (Resolve-FullPath $EnginePrefix 'EnginePrefix')
$BaselinePrefix = (Resolve-FullPath $BaselinePrefix 'BaselinePrefix')
$BuildDir = (Resolve-FullPath $BuildDir 'BuildDir')
foreach ($p in @(@($EnginePrefix, 'engine prefix'), @($BaselinePrefix, 'baseline prefix'))) {
    if (-not (Test-Path -LiteralPath $p[0] -PathType Container)) {
        Stop-Run 'arguments' "$($p[1]) is not a directory: $($p[0])"
    }
}
foreach ($lib_prefix in @(@($EnginePrefix, 'ENGLISH'), @($BaselinePrefix, 'BASELINE'))) {
    $root = $lib_prefix[0]
    $label = $lib_prefix[1]
    foreach ($part in @((Join-Path $root 'include'), (Join-Path $root 'lib'))) {
        if (-not (Test-Path -LiteralPath $part -PathType Container)) {
            Stop-Run 'arguments' "the $label prefix has no $(Split-Path -Leaf $part): $root"
        }
    }
    if (-not (Test-FilePresent (Join-Path $root 'include\rime_api.h'))) {
        Stop-Run 'arguments' "the $label prefix has no include/rime_api.h: $root"
    }
    if (-not (Test-FilePresent (Join-Path $root 'lib\rime.lib'))) {
        Stop-Run 'arguments' "the $label prefix has no lib/rime.lib: $root"
    }
    $dll = @((Join-Path $root 'lib\rime.dll'), (Join-Path $root 'bin\rime.dll')) |
        Where-Object { Test-FilePresent $_ } | Select-Object -First 1
    if (-not $dll) {
        Stop-Run 'arguments' "the $label prefix has no rime.dll under lib/ or bin/: $root"
    }
    Write-Host ("  {0,-8} prefix accepted : {1}  (rime.dll at {2})" -f $label, $root, $dll)
}

if ($PilotLexicon -ne '') {
    if (-not [System.IO.Path]::IsPathRooted($PilotLexicon)) {
        Stop-Run 'arguments' "-PilotLexicon must be an absolute path, got: $PilotLexicon"
    }
    if (-not (Test-FilePresent $PilotLexicon)) {
        Stop-Run 'arguments' ('-PilotLexicon names a file that is not there: ' + $PilotLexicon +
            '. The pilot cases are NOT silently dropped when the data is absent -- generate it ' +
            'with tools/data/build_english_pilot.py (a human step; this script runs no network ' +
            'fetch) or omit -PilotLexicon to run the suite without them.')
    }
    if ($PilotOutputDir -eq '') {
        $PilotOutputDir = Split-Path -Parent $PilotLexicon
    }
    Write-Host "  pilot    lexicon       : $PilotLexicon"
    Write-Host "  pilot    output dir    : $PilotOutputDir"
    Write-Host ('  pilot    identity      : enforced by the project at configure time ' +
        '(name, bytes and SHA-256 against data/manifests/english-pilot.sources.lock.json); ' +
        'this script does not compute a second digest.')
} else {
    Write-Host '  pilot    lexicon       : not supplied -- no pilot case is registered or run'
}

if ($LogDir -eq '') { $LogDir = Join-Path $BuildDir 'integration-logs' }
$LogDir = (Resolve-FullPath $LogDir 'LogDir')
if (-not (Test-Path -LiteralPath $LogDir -PathType Container)) {
    New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
}

# --- 2. tools ----------------------------------------------------------------------------
$script:Stage = 'tools'
$Cmake_Rel = 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$Ctest_Rel = 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-FilePresent $vswhere)) {
    Stop-Run 'tools' ("vswhere is not present at $vswhere. Probe: " +
        'Test-Path "$env:ProgramFiles (x86)\Microsoft Visual Studio\Installer\vswhere.exe". ' +
        'Nothing is installed or moved onto PATH by this script to work around that.')
}
$Vs = $VsInstallPath
if ($Vs -eq '') {
    $candidates = @(& $vswhere -all -products '*' -version '[17.0,18.0)' -property installationPath)
    if ($LASTEXITCODE -ne 0 -or $candidates.Count -eq 0) {
        Stop-Run 'tools' ('vswhere found no Visual Studio 2022 instance. Probe: "' +
            $vswhere + ' -all -products * -version [17.0,18.0) -property installationPath". ' +
            'A toolchain is not repaired here; pass -VsInstallPath if the instance is elsewhere.')
    }
    # An instance is usable only if it carries the dev-shell module AND the two tools this
    # pipeline runs. On at least one machine in this project's history the instances differ
    # exactly there, so "has VS 2022" is not the question -- "which one has CMake" is.
    $usable = @($candidates | Where-Object {
        (Test-Path -LiteralPath (Join-Path $_ 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')) -and
        (Test-Path -LiteralPath (Join-Path $_ $Cmake_Rel)) -and
        (Test-Path -LiteralPath (Join-Path $_ $Ctest_Rel))
    })
    if ($usable.Count -eq 0) {
        Stop-Run 'tools' ('none of the listed VS 2022 instances bundles both the DevShell module ' +
            'and CMake/CTest under Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin. ' +
            'Instances found: ' + ($candidates -join '; ') + '. Nothing is installed or moved ' +
            'onto PATH to work around that; pass -VsInstallPath for an instance that does.')
    }
    $Vs = $usable | Select-Object -First 1
    if ($usable.Count -gt 1) {
        Write-Host ('  note: several instances qualify; using the first -- ' +
            ($usable -join '; '))
    }
} else {
    $Vs = (Resolve-FullPath $Vs 'VsInstallPath')
    if (-not (Test-FilePresent (Join-Path $Vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'))) {
        Stop-Run 'tools' "-VsInstallPath has no Common7\Tools\Microsoft.VisualStudio.DevShell.dll: $Vs"
    }
}
Write-Host "  VS instance       : $Vs"
Import-Module (Join-Path $Vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $Vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' |
    Out-Null
$env:VSLANG = '1033'

$CMake = Join-Path $Vs $Cmake_Rel
$CTest = Join-Path $Vs $Ctest_Rel
foreach ($tool in @(@('cmake', $CMake), @('ctest', $CTest))) {
    if (-not (Test-FilePresent $tool[1])) {
        Stop-Run 'tools' ($tool[0] + ' is not bundled at ' + $tool[1] + ' in that instance. ' +
            'Probe: Test-Path "' + $tool[1] + '". Nothing is downloaded or installed to substitute ' +
            'for it, and no PATH entry is written.')
    }
    $probe = Invoke-Stage $tool[1] @('--version')
    if ($probe.Code -ne 0) {
        Stop-Run 'tools' ($tool[0] + ' --version returned ' + $probe.Code + ' from ' + $tool[1])
    }
    Write-Host ('  tool ' + $tool[0] + ' found at: ' + $tool[1])
    Write-Host ('       version: ' + ($probe.Lines | Select-Object -First 1))
}

# --- log discipline ----------------------------------------------------------------------
function Write-RunLog {
    param([Parameter(Mandatory)][string]$Name, [AllowNull()]$Lines,
          [Parameter(Mandatory)][int]$Code)
    $path = Join-Path $LogDir "$Name-$LogTag.log"
    if (Test-Path -LiteralPath $path) {
        Stop-Run 'logs' ("refusing to overwrite an existing log: $path -- choose another " +
            '-LogTag so the earlier run stays readable next to this one.')
    }
    $body = @($Lines) + @('', "EXIT_CODE=$Code")
    Set-Content -LiteralPath $path -Value $body -Encoding UTF8
    Write-Host "  log               : $path"
    return $path
}

# Asks CTest, in this build directory, for the names it actually generated, optionally through
# a -R regex. The answer is returned as a name array; the raw JSON goes to its own log so a
# disagreement between this list and the sources can be read afterwards rather than re-guessed.
function Get-CtestNames {
    param([Parameter(Mandatory)][string]$LogName, [AllowEmptyString()][string]$Regex = '')
    $show_args = @('--test-dir', $BuildDir, '-C', 'Release', '-N', '--show-only=json-v1')
    if ($Regex -ne '') { $show_args += @('-R', $Regex) }
    $show = Invoke-Stage $CTest $show_args
    Write-RunLog $LogName $show.Lines $show.Code | Out-Null
    if ($show.Code -ne 0) {
        Stop-Run 'list' ("ctest -N returned $($show.Code); the generated list could not be read.")
    }
    $json = $null
    try { $json = ($show.Lines | Out-String) | ConvertFrom-Json }
    catch {
        Stop-Run 'list' ('the CTest -N --show-only=json-v1 output was not parseable JSON (' +
            "$_.)")
    }
    if ($null -eq $json -or $null -eq $json.tests) {
        Stop-Run 'list' ('the CTest -N JSON carried no tests array, so nothing can be said ' +
            'about what this build registered. Reading that as zero cases would be the wrong ' +
            'direction; it is a failure to observe at all.')
    }
    return @($json.tests | ForEach-Object { $_.name })
}

# --- 3. configure --------------------------------------------------------------------------
$script:Stage = 'configure'
$configure_args = @('-S', $SourceDir, '-B', $BuildDir, '-G', $Generator, '-A', $Platform,
    ('-DCMAKE_GENERATOR_INSTANCE:PATH=' + $Vs),
    '-DCMAKE_CONFIGURATION_TYPES:STRING=Release',
    ('-DRime_Install_Prefix:PATH=' + $EnginePrefix),
    ('-DBaseline_Rime_Prefix:PATH=' + $BaselinePrefix))
if ($PilotLexicon -ne '') {
    $configure_args += ('-DEPOCANVA_ENGLISH_PILOT_LEXICON:FILEPATH=' + $PilotLexicon)
    $configure_args += ('-DEPOCANVA_ENGLISH_PILOT_OUTPUT_DIR:PATH=' + $PilotOutputDir)
} else {
    $configure_args += '-DEPOCANVA_ENGLISH_PILOT_LEXICON:FILEPATH='
    $configure_args += '-DEPOCANVA_ENGLISH_PILOT_OUTPUT_DIR:PATH='
}
Write-Host ''
Write-Host 'configure'
$cfg = Invoke-Stage $CMake $configure_args
Write-RunLog 'configure' $cfg.Lines $cfg.Code | Out-Null
$cfg.Lines | Where-Object { $_ -match '^\s*--\s+(pilot identity|pilot lexicon|ctest total|english prefix|baseline prefix|run root|MSVC runtime)' } |
    ForEach-Object { Write-Host "    $_" }
if ($cfg.Code -ne 0) {
    $cfg.Lines | Where-Object { $_ -match 'CMake (Error|FATAL ERROR)|pilot|required input missing' } |
        Select-Object -First 12 | ForEach-Object { Write-Host "    $_" }
    Stop-Run 'configure' ("cmake -S ... -B ... returned $($cfg.Code). No build and no test was " +
        'run. A pilot identity mismatch reports here, at configure time, and is never ' +
        'accommodated by adjusting the expectation.')
}

# --- 4. build ------------------------------------------------------------------------------
$script:Stage = 'build'
Write-Host ''
Write-Host 'build'
$bld = Invoke-Stage $CMake @('--build', $BuildDir, '--config', 'Release', '--', '/v:minimal')
Write-RunLog 'build' $bld.Lines $bld.Code | Out-Null
$bld.Lines | Where-Object { $_ -match '\.vcxproj ->' } | ForEach-Object { Write-Host "    $_" }
$warn = @($bld.Lines | Where-Object { $_ -match ': (warning|error) ' }).Count
Write-Host "  diagnostics       : $warn warning/error line(s) in the build log"
if ($bld.Code -ne 0) {
    $bld.Lines | Where-Object { $_ -match ': (warning|error) ' } | Select-Object -First 15 |
        ForEach-Object { Write-Host "    $_" }
    Stop-Run 'build' ("cmake --build returned $($bld.Code). CTest was NOT invoked, so no " +
        'binary from an earlier run can be reported as the result of this run.')
}

# --- 5. the list CTest really generated ----------------------------------------------------
$script:Stage = 'list'
Write-Host ''
Write-Host 'test list'
# The FULL list is always exported and always cross-checked, even when only one group is going
# to run: a filtered invocation must not be able to hide a registration that disappeared.
$full_generated = @(Get-CtestNames -LogName 'list-all' -Regex '')
if ($full_generated.Count -eq 0) {
    Stop-Run 'list' ('CTest generated ZERO cases. An empty selection is not a pass, in any ' +
        'configuration, and nothing is run to see whether it looks green.')
}
$dupes = @($full_generated | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Name })
if ($dupes.Count -gt 0) {
    Stop-Run 'list' ('the same case name is registered more than once: ' + ($dupes -join ', ') +
        '. A duplicate registration is not an extra capability.')
}

# The registrations parsed out of CMakeLists.txt, split at the pilot-cases block so the
# conditional registrations are only expected when the pilot data was named.
$lines = Get-Content -LiteralPath $ProjectFile -Encoding UTF8
$pilot_marker = -1
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^\s*#\s*-+\s*pilot cases') { $pilot_marker = $i; break }
}
if ($pilot_marker -lt 0) {
    Stop-Run 'list' ("no '# --- pilot cases ---' marker in $ProjectFile, so conditional " +
        'registrations cannot be told from unconditional ones.')
}
$pilot_if = -1
for ($i = $pilot_marker; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^\s*if\s*\(\s*EPOCANVA_ENGLISH_PILOT_LEXICON\s*\)') { $pilot_if = $i; break }
}
if ($pilot_if -lt 0) {
    Stop-Run 'list' "no if(EPOCANVA_ENGLISH_PILOT_LEXICON) after the pilot-cases marker in $ProjectFile."
}
$registered = @()
$registered_pilot = @()
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^\s+NAME\s+([A-Za-z0-9_.\-]+)\s*$') {
        $nm = $Matches[1]
        if ($i -ge $pilot_if) { $registered_pilot += $nm } else { $registered += $nm }
    }
}
$expected = @($registered)
if ($PilotLexicon -ne '') { $expected = @($expected + $registered_pilot) }
$unexpected = @($full_generated | Where-Object { $expected -notcontains $_ })
$absent = @($expected | Where-Object { $full_generated -notcontains $_ })
Write-Host "  source registers  : $(@($registered).Count) unconditional + $(@($registered_pilot).Count) pilot-only"
Write-Host "  generated list    : $(@($full_generated).Count) case(s); this configure must register $(@($expected).Count)"
if ($absent.Count -gt 0 -or $unexpected.Count -gt 0) {
    if ($absent.Count -gt 0) {
        Write-Host ('    registered but NOT generated: ' + ($absent -join ', '))
    }
    if ($unexpected.Count -gt 0) {
        Write-Host ('    generated but NOT registered in CMakeLists.txt: ' + ($unexpected -join ', '))
    }
    Stop-Run 'list' ('the generated CTest list and the source registrations disagree. A case that ' +
        'disappeared is not coverage, and a case nobody registered is not a test.')
}
$full_generated | Sort-Object | ForEach-Object { Write-Host "    case              : $_" }

$selection = $full_generated
if ($Filter -ne '') {
    $selection = @(Get-CtestNames -LogName 'list-filtered' -Regex $Filter)
    if ($selection.Count -eq 0) {
        Stop-Run 'list' ("the -Filter '$Filter' matched none of the " +
            "$(@($full_generated).Count) generated case(s). Nothing is run and nothing is " +
            'reported green for an empty selection.')
    }
    $foreign = @($selection | Where-Object { $full_generated -notcontains $_ })
    if ($foreign.Count -gt 0) {
        Stop-Run 'list' ('the filtered list contains names the full export did not: ' +
            ($foreign -join ', '))
    }
    Write-Host "  filtered selection: $(@($selection).Count) of $(@($full_generated).Count) case(s)"
    $selection | Sort-Object | ForEach-Object { Write-Host "    selected          : $_" }
} else {
    Write-Host "  selection         : all $(@($full_generated).Count) case(s)"
}

# --- 6. CTest ------------------------------------------------------------------------------
Write-Host ''
Write-Host 'ctest'
$junit = Join-Path $LogDir "ctest-$LogTag.xml"
if (Test-Path -LiteralPath $junit) { Stop-Run 'logs' "refusing to overwrite existing JUnit: $junit" }
$run_args = @('--test-dir', $BuildDir, '-C', 'Release', '--output-on-failure', '--no-tests=error', '--output-junit', $junit)
if ($Filter -ne '') { $run_args += @('-R', $Filter) }
$run = Invoke-Stage $CTest $run_args
$run_log = Write-RunLog ('ctest' + $(if ($Filter -ne '') { '-filtered' } else { '-all' })) $run.Lines $run.Code
$run.Lines | Where-Object { $_ -match '^\s*\d+/\d+ Test|tests passed|tests failed|Failed +[\d\.]|No tests' } |
    ForEach-Object { Write-Host "    $_" }

# A second judgement on the same facts, from CTest's own summary rather than from the exit
# code alone: the run must report a count, that count must be the selection, and the failure
# count must be zero. An absent summary line is treated as no result, not as success.
$summary = $run.Lines | Where-Object { $_ -match '^(?:\s*)\d+% tests passed, (\d+) tests failed out of (\d+)' } |
    Select-Object -Last 1
if (-not $summary) {
    Stop-Run 'nocases' ('CTest gave exit code ' + $run.Code +
        ' but printed no "tests passed, N tests failed out of M" summary, so there is no ' +
        'count to check. Full output is in ' + $run_log)
}
$m = [regex]::Match($summary, '(\d+) tests failed out of (\d+)')
$failed_count = [int]$m.Groups[1].Value
$total_count = [int]$m.Groups[2].Value
if ($total_count -ne $selection.Count) {
    Stop-Run 'nocases' ("CTest ran $total_count case(s) but the exported selection held " +
        "$($selection.Count). The run judged is not the run that was checked.")
}
Write-Host "  verified counts   : $total_count selected, $failed_count failed, exit=$($run.Code)"
if ($run.Code -ne 0) {
    Write-Host ''
    Write-Host "ctest exit = $($run.Code)  ($failed_count of $total_count case(s) failed)"
    Write-Host "full output: $run_log"
    exit $run.Code
}
if ($failed_count -ne 0) {
    Stop-Run 'nocases' "CTest reported $failed_count failed case(s) while exiting 0."
}
$xmlSettings = [Xml.XmlReaderSettings]::new()
$xmlSettings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
$xmlSettings.XmlResolver = $null
$reader = [Xml.XmlReader]::Create($junit, $xmlSettings)
try {
    $xml = [Xml.XmlDocument]::new()
    $xml.XmlResolver = $null
    $xml.Load($reader)
} finally { $reader.Dispose() }
$executed = @($xml.SelectNodes('//testcase') | ForEach-Object { $_.GetAttribute('name') })
if ($executed.Count -ne $selection.Count -or
    @($executed | Sort-Object -Unique).Count -ne $executed.Count -or
    @(Compare-Object ($executed | Sort-Object) ($selection | Sort-Object)).Count -ne 0 -or
    $xml.SelectNodes('//failure | //error | //skipped').Count -ne 0) {
    Stop-Run 'nocases' 'JUnit does not contain exactly the selected, executed, passing tests.'
}
Write-Host ''
Write-Host "RESULT PASS  $total_count case(s) selected, 0 failed, ctest exit 0"
exit 0
