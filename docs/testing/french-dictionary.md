# Native French dictionary backend verification

## Scope and status

This component executes the **complete pinned Hunspell library** against the
unaltered French `.aff`/`.dic` pair. It is not a second, partial affix parser, a
fixed candidate list, or a corpus-frequency model. It changes no root CMake,
product, frontend, Windows TSF or learning implementation.

Verified on 2026-11-02 with Windows x64, Visual Studio 2022/MSVC
19.44.35209.0, Windows SDK 10.0.26100.0, CMake 3.31.6-msvc6 and Windows
PowerShell 5.1. C++17, extensions disabled, `/permissive-`; project-owned code
uses `/W4 /WX`. The embedding project's CRT selection is inherited. Current
loader implementation requires Windows/MSVC; this is not an Android port.

Latest headless results: **Debug 14/14, Release 14/14**, both CTest exit **0**.
Tests require exactly status-only `RESULT PASS` on stdout and empty stderr.
The source/notice verifier also exited **0**, with all **956** extracted
upstream files byte-identical to the pinned source archive. No package was
installed, no Git repository was nested, and no system configuration changed.

## Source and dictionary identity

`data/manifests/backend-hunspell.sources.json` is the scoped dependency manifest.
It records individual source/header/notice hashes as well as the archive pin.

- Official repository: `https://github.com/hunspell/hunspell`.
- Full commit: `7d725f1929a4189a3b8188649af6a6b3e302f469`, confirmed by the
  official GitHub commit API, not inferred from a tag or filename.
- Archive: `https://codeload.github.com/hunspell/hunspell/tar.gz/7d725f1929a4189a3b8188649af6a6b3e302f469`.
- Archive SHA-256: `36fada04821da689b478f8c782014831741001f28f94ecc0239ae6efc080690d`.
- Archive size: 598883 bytes.
- Extraction: `build/backend-french-dependency/hunspell-7d725f1929a4189a3b8188649af6a6b3e302f469/`.

The separately acquired LibreOffice/Grammalecte **French Classique 7.7** pair,
revision `4f1df64b2e8e14751aa7c2205d170ca016071315`, is consumed in place.

| File | SHA-256 | Bytes |
| --- | --- | ---: |
| `build/backend-data-acquire/zh-fr/upstream/french/fr_FR/fr.aff` | `c176610cd5dc4846806a65ddd029f422d87978bf58f224aa44222662a16a2de5` | 199870 |
| `build/backend-data-acquire/zh-fr/upstream/french/fr_FR/fr.dic` | `b78a868e31dd6e373b6c3217969afb898a9acde828a5e7ef97308da42218c88c` | 1236490 |
| `build/backend-data-acquire/zh-fr/upstream/french/fr_FR/README_dict_fr.txt` | `4a25abf740ad6ba5e8d0cfc0d2ffadc3c13c303f8fe389f323b061a389350f83` | 2758 |

The `.dic` first line is **84782**, while the actual nonempty dictionary entry
record count is **84749**. This is valid: upstream `src/hunspell/hashmgr.cxx`
lines 621–661 uses that line as an allocation hint; lines 669–764 read the actual
records and reject only grossly unreasonable hints. Do not truncate the file,
invent rows, or reject this official pair because the numbers differ. Neither
number is a word frequency or a corpus token count.

The full `.aff` contains `FLAG long`, `FULLSTRIP`, `NEEDAFFIX`, `FORBIDDENWORD`,
`CIRCUMFIX`, `KEEPCASE`, `NOSUGGEST`, prefix/suffix rules and `ICONV`, `OCONV`,
`MAP`, `REP`, `BREAK`. The native parser executes them all. See the pair's
`fr.aff` lines 19–235 for conversion/special flags and the subsequent affix
rules. No source/dictionary patch is applied.

The SHA-256 pins are integrity checks against official HTTPS downloads, **not**
an upstream digital signature or an independently published checksum.

## Interface and integration

Public header: `engine/french_dictionary/dictionary.h`.
Namespace: `epocanva::french_dictionary`.
CMake target: `epocanva_french_dictionary`.

An aggregate backend build can add `engine/french_dictionary` as a subdirectory
and link that target. No root integration change is made by this component.
`EPOCANVA_HUNSPELL_ROOT` defaults to the prepared, pinned source path. Configuration
requires the matching archive next to that source directory, verifies every
compiled file and every license file, and performs **no downloads or installs**.
Only the 11 `.cxx` translation units in upstream `src/hunspell` are compiled;
upstream command-line tools, examples and build/install project are not used.

`Dictionary::Load(aff_path, dic_path, ExpectedHashes, LoadInfo*)` returns a
`shared_ptr<const Dictionary>`, or null with a fixed failure status. The main
backend must supply **both explicit paths and both trusted manifest hashes**.
A self-declared sibling name or a caller-controlled hash is not trust.
`Dictionary::Unavailable()` supplies an explicit unavailable provider.

- `Spell(word)` returns correctness, forbidden/compound bits and the native root.
- `Suggest(query, max)` returns native spelling/accent corrections.
- `Analyze(word, max)` returns opaque native morphology records, not invented
  grammatical labels. Legal tab-separated morphology fields are preserved.
- `Stem(word, max)` returns native stems.
- `Generate(word, valid_example, max)` uses native morphological generation,
  requires both words to be accepted, then rechecks **every** returned form with
  native spell checking. It never blindly appends suffixes.
- `source()` reports availability, verified hash pair, native revision and the
  two explicitly distinguished dictionary record counts. No frequency/model
  availability is inferred from dictionary presence.

All query text is strict UTF-8; text counts and slicing are in **UTF-8 code units
(bytes)**. The current input cap is 256 UTF-8 bytes. Results are capped at 64
records; zero requests make no native engine call. Result texts over 4096 UTF-8
bytes or invalid/control-bearing results are withheld with `truncated=true`.
`truncated` describes wrapper filtering/capping, **not** native search
completeness. Normal French case and accent handling stays in Hunspell; no ASCII
lowercasing/pass-through substitutes for it.

`Spell`/`Analyze`/`Stem` preserve whole-token safety, and queries containing URL,
mail, filesystem, numeric or punctuated-code markers are refused without
correcting a word-like suffix. Spaces/control text and the native XML command
channel are refused. Letter-only identifiers cannot reliably be distinguished
from French words here: caller field/context privacy and code-mode admission
remain mandatory. The wrapper does not introduce its own frequency, ID, history
or learning namespace. Phrase prediction remains a separately sourced n-gram
model; multiword corrections returned by Hunspell are not a language model.

## Loading, privacy and concurrency

Loading opens only the two local static files. It verifies full SHA-256 values
using Windows BCrypt, checks a bounded UTF-8 file envelope, and then constructs
Hunspell. Checks include a declared `SET UTF-8`, valid `.dic` allocation hint,
nonempty records, file/physical-line bounds and absence of invalid UTF-8/NUL.
**This is not a general untrusted-affix validator.** Hunspell has no public
complete parse-success result: only reviewed, pinned data is supported; an
attacker-supplied hash does not make malformed affix syntax safe or accepted.

Ordinary drive paths and UTF-16 Windows paths are converted to Hunspell's
explicit UTF-8 long-path form. UNC/device paths, mapped network drives,
alternate data streams, name-surrogate reparse redirects and files requiring
cloud recall are rejected. Already resident OneDrive files with non-name-
surrogate cloud tags are not treated as symlinks. Read handles deny ordinary
write/delete sharing until native loading finishes, closing the ordinary
leaf-file replacement race. This is not an OS sandbox against a malicious
process racing ancestor paths or modifying its own address space; packaging
must protect the static data tree.

The shared object is **logically immutable, not lock-free**. Every native query
uses an internal per-instance mutex. Hunspell scratch/state is private; no
native types, mutable-word APIs or trace callback escape the header. Query
inputs/results are transient and caller-owned. There are no history, learning,
file-write, network, telemetry or clipboard operations in the query path.
`HUNSPELL_WARNING_ON` is explicitly undefined for the native build, and tracing
is disabled. No process-global stdout/stderr redirect is installed.

The main/native caller must gate **every** call for password, one-time-code,
sensitive and unknown fields before passing text. This library cannot infer the
host field classification. It neither persists text nor provides a learning
bypass. Static file loading is separate from runtime user learning.

Windows TSF DLL offline behavior is architectural: a normal non-packaged DLL in
an arbitrary host process is **not** OS-sandboxed from networking. Android can
make the offline guarantee platform-enforced by omitting
`android.permission.INTERNET`; this Windows component is not evidence of that
Android packaging step.

### Sources verified for the behavior above

All Hunspell references below are under the exact extracted revision directory.

- `src/hunspell/hunspell.hxx` lines 117–125: constructor/path encoding;
  133–153: string spell/suggest; 172–203: analyze/stem/generate;
  205–225: mutation APIs deliberately not exposed; 250–254: optional tracing.
- `src/hunspell/Makefile.am` lines 1–10: the native library source set;
  `CMakeLists.txt` lines 6–17: static-library visibility definitions.
- `src/hunspell/csutil.cxx` lines 106–124: UTF-8 long-path to UTF-16 file open;
  158–222: BMP limitation of native UTF-8 case conversion.
- `src/hunspell/atypes.hxx` lines 40–49: disabled warning function;
  103–109: native time budgets. They do **not** promise a hard deadline.
- `src/hunspell/hunspell.cxx` lines 198–226: native initialization;
  1978–2029: generation, native timed filtering and pattern analysis.
- `src/hunspell/suggestmgr.cxx` lines 1926–1969: generation depends on actual
  dictionary morphology data, not just affix flags.
- SDK 10.0.26100.0 `um/stringapiset.h` lines 104–112: `GetStringTypeW`;
  `um/winnls.h` lines 170–225: classification bits. Actual tests confirmed
  precomposed `é` has `C1_ALPHA` **and** `C3_NONSPACING|C3_DIACRITIC`; letter
  classification must win over those mark flags. Otherwise initial accents fail.
- SDK `shared/bcrypt.h` lines 1595–1605: single-shot SHA-256 input/output byte
  counts; `um/fileapi.h` lines 90–107: explicit Win32 read/share-mode API.

## Real test coverage and honest limits

| Case | Checks |
| --- | --- |
| `production_load` | Full pinned pair, hash/source metadata, actual 84749 records, broad real vocabulary and complex directive presence |
| `accents` | `ecole → école`, `francais → français`, `eleve → élève`, `coeur → cœur`, `garcon → garçon`, `l'ecole → l’école`; precomposed/decomposed accents and all caps |
| `plurals` | Correct `chevaux`, `travaux`, `bijoux`, `bateaux`, `yeux`, `journaux`, `écoles`; invalid forms; `écoleses → écoles`; native stemming |
| `conjugation` | `mangeons`, `mangeraient`, `finissons`, `allaient`, `sommes`, `étaient`, `faisaient`, `construisirent`; invalid conjugations; `construirent → construisirent`; `mangeons → manger` |
| `native_parity` | Direct native/wrapper correction, analysis, stem and generation comparisons on fixed public examples |
| `protection` | URLs, mail, paths, filenames, code, digits, spaces, controls, embedded NUL, XML mutation payload refusal |
| `bounds` | Zero/one/oversized record caps; byte cap with multibyte text; invalid/truncated/overlong UTF-8 and supplementary-scalar refusal |
| `availability` | Explicit unavailable provider reports no candidates or false availability |
| `failures` | Missing file, bad/missing paired hashes, changed payload, empty data, invalid count/encoding/UTF-8, overlong line, network/device-style path refusal and explicit non-sibling pair |
| `complex_rules` | Supplementary synthetic fixture covering long flags, FULLSTRIP, NEEDAFFIX, FORBIDDENWORD, CIRCUMFIX, KEEPCASE, NOSUGGEST, prefix/suffix cross-products, conversions, MAP/REP/BREAK and successful tagged Generate |
| `concurrency` | Six threads sharing one instance, 120 iterations, actual correction/bounds and spell/analyze/stem checks |
| `static_load` | Full real pair loaded from a Unicode directory, then files removed; subsequent queries succeed without rereads/writes |
| negative controls | Deliberately failing assertion exits **1**; unknown case exits **2**; CMake verifies both exact exits |

The production pair contains **no grammatical morphology tags**. Native
`Generate(école, maisons)`, `Generate(manger, finissons)` and
`Generate(cheval, bateaux)` return empty lists. The wrapper preserves this,
and tests compare against the actual native API. This does not negate its real
morphological spelling and stemming capability, but **does not provide a full
French conjugator**. A tagged supplementary fixture verifies that Generate is
actually wired, not stubbed.

`travails` is valid in this dictionary (`travail/X.S.`); tests do not overwrite
that choice with a generic `-al → -aux` rule. Likewise the engine does not
necessarily suggest `chevaux` for `chevals`; tests verify actual validity and
real observed corrections rather than fabricated expected candidates.

Native suggestion search is time-limited. Marginal alternatives can differ
between Debug/Release or subsequent calls. There is no exhaustive top-k,
probability, stable full-list ordering across timing cutoffs or hard runtime
budget claim. Original-input preservation, ranking fusion and protected-field
handling belong to the main backend.

## Preparation and exact commands

Only preparation may acquire the official pinned archive. Existing verified
files are reused, not silently replaced. Missing tools are blockers; the script
does not install them. Full original source remains byte-clean; `third_party/`
is untouched.

From the repository root, with an existing CMake executable on PATH (or invoke
its already installed absolute path), reproduction commands are

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/backend/prepare_french.ps1 -Download
cmake -S tests/french_dictionary -B build/backend-french-dictionary/tests -G "Visual Studio 17 2022" -A x64
cmake --build build/backend-french-dictionary/tests --config Debug --parallel 4
ctest --test-dir build/backend-french-dictionary/tests -C Debug --output-on-failure
cmake --build build/backend-french-dictionary/tests --config Release --parallel 4
ctest --test-dir build/backend-french-dictionary/tests -C Release --output-on-failure
```

`-Download` is explicit preparation permission, not a CMake flag. Without it,
preparation is verification-only when the archive already exists. French
acquisition is independent: `-SkipDictionaryCheck` deliberately reports the
pair as unverified rather than fabricating it. To configure the library alone,
use `-S engine/french_dictionary -B build/backend-french-dictionary`.

Commands actually executed used absolute workspace/tool paths; exact expanded
build/test commands and real exit codes are retained in the ignored artifact
`build/backend-french-dictionary/final-test-results.txt`. No machine-specific
compiler path is committed here. The executed sequence was

1. `git rev-parse --is-inside-work-tree`: **0**, already a repository.
2. Tool probes `command -v cmake` and `command -v cl.exe`: not on Git Bash PATH;
   found existing Visual Studio CMake/compiler installations without installing
   anything. `cmake --version`: **0**, 3.31.6-msvc6.
3. `curl --fail --location --proto '=https' --tlsv1.2 --output <build/.../commit.json>
   https://api.github.com/repos/hunspell/hunspell/commits/7d725f1929a4189a3b8188649af6a6b3e302f469`:
   **0**, exact SHA confirmed.
4. Equivalent `curl` acquisition of the pinned codeload archive: **0**; SHA-256
   and safe archive member paths checked before extraction. An initial overly
   strict archive-root check rejected its own directory entry (**1**) before
   extraction; corrected path validation passed (**0**).
5. `powershell.exe -NoProfile -ExecutionPolicy Bypass -File
   tools/backend/prepare_french.ps1`: **0**, re-run after builds; all 956 source
   files and both dictionary files/notice verified. Actual initial HTTP
   acquisition used the commands above; the script's missing-cache download
   branch was not separately exercised.
6. Standalone library configure/build and test-project configure: **0** each.
7. Latest Debug build: **0**; Debug CTest: **0**, 14/14, 22.73 seconds with strict
   stdout/stderr checks. Latest Release build: **0**; Release CTest: **0**,
   14/14, 14.17 seconds with the same checks.
8. Additional direct `native_parity` processes in Debug and Release: **0** each;
   stdout exactly `RESULT PASS`, stderr empty. Production French file path set,
   bytes, modification timestamps and hashes unchanged before/after.
9. Selected socket/HTTP call-site scan across 32 wrapper/native source/header
   files: **0**, no matches. This is a source-review check, not packet capture,
   a universal dependency proof or an OS sandbox demonstration.

Earlier failing tests are not omitted: the first suite run exited **8** with
9 failures and caught the NLS initial-accent bug; the second exited **8** with
5 failures due to unverified expectations (`travails`, elision, native
suggestion count/parity and a missing tagged-fixture morphology field). A first
Release run exited **8** for an invalid assumption of identical marginal
candidate lists across time cutoffs. Corrections were checked against native
source and fixed-data probes. Final Debug and Release were both re-executed.

## Licensing and release gate

The upstream tri-license explicitly offers **LGPL-2.1-or-later** in
`license.hunspell` lines 41–51 and `src/hunspell/hunspell.hxx` lines 23–33.
That option is recorded; no upstream notice is edited. The full original
`COPYING`, `COPYING.LESSER`, `COPYING.MPL`, `license.hunspell` and
`license.myspell` remain in the source archive and are individually hashed.
`phonet.cxx`/`.hxx` also retain Bjoern Jacke's LGPL version 2.1 wording and his
2007-07-26 tri-license grant note; those file-specific notices are not erased
or rewritten as if they independently say “or later”.

Current headless linking is static. Before distribution, include the exact
library source, notices and applicable LGPL text plus the wrapper/build/source
or relinkable objects needed to rebuild against a modified library. See
`COPYING.LESSER` §§4–6, particularly §6(a), not a claim that static linking
removes source/relink obligations. A future shared link would still need
notices, corresponding-source compliance and an actually replaceable library.

French data remains **MPL-2.0**, not relicensed as LGPL or as project code. Its
complete notice/license and paired data must be included by the data/source
release component. The task's cached instructions and current on-disk project
licensing record differ; this component selects only the verified dependency
option and does not modify the project license or authorize release.

**Not complete in this scoped task:** final product/source-release assembly,
artifact-specific licensing/relink verification, native main C ABI integration,
real-host TSF tests, Android packaging and full French tagged-conjugation data.
Full source has been retained for the later corresponding-source bundle, but
**this is not a stable/public release approval**. No project-license header,
root license change, manifest outside the assigned one, or product build edit
is made here.

## Files in this change

- `engine/french_dictionary/dictionary.h`
- `engine/french_dictionary/dictionary.cc`
- `engine/french_dictionary/CMakeLists.txt`
- `tests/french_dictionary/main.cc`
- `tests/french_dictionary/CMakeLists.txt`
- `tests/french_dictionary/assert_exit.cmake`
- `tools/backend/prepare_french.ps1`
- `data/manifests/backend-hunspell.sources.json`
- `docs/testing/french-dictionary.md`

All acquired dependencies, probes, generated headers, fixtures, executables and
verification logs stay below `build/backend-french-dependency/` or
`build/backend-french-dictionary/`. Normal preparation/runtime does not duplicate
the production pair; test-only scratch copies are clearly under the test build.
