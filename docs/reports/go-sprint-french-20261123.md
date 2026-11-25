# French backend performance and correctness — November 23, 2026

**Status** Targeted engineering verification passed on an uncommitted patch over `3401bf40c8701032fcf08cb8b886f2d17b50a71e`. French Tier A repeat-query P95 was **503.6883 → 9.5775 ms** with exact published candidate parity. Edited input still incurs native suggestion latency on its first query. Formal Tier B and absolute threshold acceptance are `BLOCKED` pending owner approval and measurement of a frozen candidate.

## Environment and source inspection

Windows x64 build 26200, Visual Studio 2022, MSVC 19.44.35209.0, Windows SDK 10.0.26100.0, CMake/CTest 3.31.6-msvc6, Python 3.14.3 and PowerShell 7.6.9. Local evidence is under `build/go-french-20261123`. `prior/` contains the recovered French-only diff, original profiler, report and raw results from the old worktree at `80cb0bbdd78825036e062416137fb61a06998734`. Both the old uncommitted diff and the actual `3401bf4` source were inspected. The latter called Hunspell on every French completion query. The staged product `.aff` and `.dic` match the verified dictionary pair used by the standalone probe byte-for-byte.

| ID | Finding | Location and verification |
| --- | --- | --- |
| FR-01 / P1 | Repeated native suggestion work dominates the slow French case. Backend construction loads the dictionary once. The wrapper validates each query, locks one native instance, calls `spell` and `suggest`, and copies the bounded result records in native order. | `engine/french_dictionary/dictionary.cc:340`, `engine/french_dictionary/dictionary.cc:422`, `engine/french_dictionary/dictionary.cc:489`. Opened pinned Hunspell `7d725f1929a4189a3b8188649af6a6b3e302f469` at `src/hunspell/hunspell.cxx:198`, `src/hunspell/hunspell.cxx:1225`, and `src/hunspell/suggestmgr.cxx:188`. The patched build copy uses the existing deterministic work clock. Rebuilt the recovered probe against the actual patched library and verified ordered wrapper/native parity. |
| FR-02 / P1 | The recovered cache kept a duplicate of the input and missed the direct `confirm_text` state transition. Replaying the same input after confirmation could otherwise reuse an entry from the prior composition. | Recovered `prior/french.patch`. The integrated design clears entries at composition boundaries and uses the admitted session's existing preedit. `engine/backend/backend_api.cc:190`, `engine/backend/backend_api.cc:305`, `engine/backend/profile_api.inc:138`. |
| FR-03 / P1 open | A cache miss still runs under `Registry::mutex` and the native dictionary mutex. Separate sessions can wait for a native miss. Each text edit can miss. | `engine/backend/backend_api.cc:239`, `engine/backend/backend_api.cc:1064`, `engine/french_dictionary/dictionary.cc:359`, `platforms/windows/engine_executor/unified_engine_backend.cc:298`. The first-query run and independent native profile show the remaining cost. No contention distribution or installed-host typing trace was measured in this module. |

## Files and design

| Path | Change |
| --- | --- |
| `engine/backend/backend_api.cc` | Keep one successful native correction vector for the current raw/preedit and exact candidate-record cap. Discard on input change, denied or stale access on the owning thread, cancel, selection, consumed acknowledgement, and session destruction. Move the accepted raw string into session state after the flow accepts it. |
| `engine/backend/profile_api.inc` | Apply the same access check and explicitly discard on confirmed host text. Sensitive confirmation also follows the existing composition reset path. |
| `tests/backend/context_regressions.cc` | Exercise real Hunspell fixture output, ordering, lexical IDs, UTF-8 byte offsets, scores and fresh tokens. Exercise context rescoring, caps, composition changes, Sensitive and Unknown denial, changed target/generation, replacement session, cancel, selection, rejected acknowledgement and confirmed host text. |
| `docs/reports/go-sprint-french-20261123.md` | Record verification and remaining limitations. |
| `build/go-french-20261123/compare_french.py` | Local development comparison of complete published records and CTest registration against raw JUnit. |

The flow checks the caller thread, normal privacy, context target and generation on every admitted operation (`engine/backend_session/session.cc:95`). The existing registry lock also serializes cache access. Repeating the exact input in the same composition can hit even as the flow reissues tokens. A cap change or unsuccessful dictionary call requires a fresh result. The existing ranking and publish pipeline runs for every query with live corpus context and learning permissions. Lexical identity, provenance, scores, completeness flags and ordered candidate membership were compared independently. The cache owns no ranked menu, selection token or separate copy of input. A per-resource history would cross session boundaries, and a lower Hunspell work budget could change candidate membership. The one-entry design instead preserves native work and exact output on every miss. Runtime offline enforcement is architectural for the ordinary Windows in-process TSF DLL. Android enforces the network boundary by omitting `android.permission.INTERNET`.

## Measurements

Same Windows machine, x64 Release, one configure and link graph, identical staged product resources, three checked-in French Tier A cases. The before executable was saved before editing. The warm protocol sets the same input before each query in one session per case, with 20 warm-up queries and 8 measured queries per case. First-query sampling used one query per case with zero warm-up. Percentiles use nearest rank. All repeated and first-query candidate records matched before and after, including stable IDs, source/match metadata and exact scores. Both recovered old runs also matched this baseline's candidate records. Measurements describe those three fixtures.

| Metric | Baseline ms | Patched ms |
| --- | ---: | ---: |
| Warm P50, 24 samples | 35.7294 | 4.9220 |
| Warm P95, 24 samples | 503.6883 | 9.5775 |
| Warm P99, 24 samples | 504.5723 | 10.8067 |
| `fr-prefix-francais-0001` warm median / first | 492.3459 / 476.4627 | 8.4907 / 520.8632 |
| `fr-prefix-ecol-0002` warm median / first | 31.2358 / 32.1220 | 4.5800 / 27.9794 |
| `fr-prefix-cafe-0003` warm median / first | 36.4624 / 34.7270 | 4.0383 / 36.1210 |

The freshly linked standalone probe measured one-time wrapper/native loads of 72.0936 / 56.2504 ms. For the slowest case, eight-query medians were **0.0472 ms spell, 283.1485 ms native suggest and 283.6960 ms wrapper suggest**. For the other cases native medians were 18.8909 and 24.7253 ms. The wrapper and native calls returned exactly the same ordered result records for every probe query. Whole-process peak working set for the warm evaluation was 428,306,432 → 428,351,488 bytes. No sustained growth or lock-wait measurement was taken. The single fresh query per case shows the cost remains and does not establish a new-input latency trend. The earlier report's P95 values use a different calculation or report revision. Nearest-rank recalculation of its recovered JSONL gives 539.1706 → 7.6644 ms.

## Commands and results

Each `*.command.json` in the evidence folder contains the exact executable and argument vector, working directory and observed exit. CMake/CTest denote the Visual Studio instance's binaries. `F` denotes `build/go-french-20261123`, `P` the staged product resource root, `OLD` the recovered worktree, and `K` the three-family CTest selector recorded in the ledger. The executable for each replay is the matching saved before/after build. Log and JSON names identify both runs.

| Invocation | Exit | Ledger and raw evidence |
| --- | ---: | --- |
| `git -C OLD diff HEAD -- engine/backend/backend_api.cc tests/backend/context_regressions.cc tests/french_dictionary/CMakeLists.txt` | 0 | `prior-diff.command.json`, `prior/french.patch` |
| `cmake -S tools/backend -B F/backend -G "Visual Studio 17 2022" -A x64 -DCMAKE_GENERATOR_INSTANCE=<VS> -DEPOCANVA_BACKEND_WITH_FRENCH_DICTIONARY=ON -DEPOCANVA_HUNSPELL_ROOT=<staged> -DEPOCANVA_FRENCH_AFF_PATH=<staged> -DEPOCANVA_FRENCH_DIC_PATH=<staged>` | 1 | `configure.*`. The staged source tree was present and the sibling `downloads` archive was missing at configure time. A subsequent `Test-Path -LiteralPath <staged-archive> -PathType Leaf` check still returned false. |
| Same configure using the old worktree's pinned archive/source directory and dictionary pair | 0 | `configure-retry.*`. CMake verified the archive and pinned extracted source hashes. |
| `cmake --build F/backend --config Release --target epocanva_candidate_eval_cli --parallel 4` | 0 | `build-before.*`, saved baseline executable |
| `cmake --build F/backend --config Release --target epocanva_candidate_eval_cli epocanva_backend_context_regressions epocanva_french_dictionary_tests epocanva_backend_profiles --parallel 4` | 0 | `build-after.*`, saved patched evaluator |
| Both saved executables with `--resources P --cases data/eval/candidate-v1/cases/fr.jsonl --language fr --output <JSONL> --repeat 8 --warmup-queries 20` | 0, 0 | `before-warm.*`, `after-warm.*` |
| Both saved executables with the same inputs and `--repeat 1 --warmup-queries 0` | 0, 0 | `before-first.*`, `after-first.*` |
| `OLD/build/go-quality-fr/Release/epocanva_french_profile.exe F/native-profile.json` | 0 | `native-profile.*` |
| `vcvars64.bat`, then `cl.exe` using Release MSVC C++17 `/MT`, current patched Hunspell and adapter static libraries, then the rebuilt profiler with `F/native-profile-fresh.json` | 0, 0, 0 | `fresh-profile-vcvars.command.json`, `fresh-profile-build.*`, `fresh-profile-run.*`, `fresh-profile-sha256.json` |
| `ctest --test-dir F/backend -C Release -N --show-only=json-v1 -R K` | 0 | `target-inventory.*` |
| `ctest --test-dir F/backend -C Release -R K --no-tests=error --output-on-failure --output-junit F/target.junit.xml` | 0 | `target-ctest.*`, `target.junit.xml`. **20 registered, 20 passed, 0 failed, 0 errors, 0 skipped**. Context regression ran 1054 checks. The negative test observed executable exit 1 as specified. |
| Separate configure with `-DEPOCANVA_BACKEND_WITH_FRENCH_DICTIONARY=OFF`, build Release context regression, CTest with `-R ^backend[.]abi[.]context_regressions` and raw JUnit | 0, 0, 0 | `no-french-configure.*`, `no-french-build.*`, `no-french-ctest.*`, `no-french.junit.xml`. **2 passed, 0 failed, 0 skipped**. |
| `python -B F/compare_french.py OLD/build/go-quality-backend` | 1, then 0 | `compare-schema-first.*` records the initial parser error. CTest omits the `errors` attribute. The corrected tool checks every executed testcase for error, failure and skip elements and enforces zero failures/disabled/skips in the suite. `compare.*` and `comparison.json` contain the corrected output. |
| `python -B F/compare_french.py F/prior` | 0 | `compare-snapshot.*`, `comparison.json` |
| `git diff --check -- engine/backend tests/backend docs/reports/go-sprint-french-20261123.md` | 0 | Local whitespace check |

`inputs-before.json`, `inputs-after.json`, `prior-sha256.json`, `fresh-profile-sha256.json` and `development-evidence.json` identify the local evidence bytes. The comparison also includes both evaluator hashes and the test inventory and JUnit hashes. These files bind development observations to local binaries. Release-candidate identity and formal acceptance require the main build and approved production protocols.

## Remaining Verification

- Full backend and product Release suites are owned by the main integration run. This module ran the targeted CTest selections above.
- French first-query and changing-input latency and lock contention remain open for installed-host or targeted profiling under the approved performance protocol.
- Formal Tier B, approved absolute thresholds, released product binaries and installed-host evidence remain gates for the unified candidate. The current module results use the three checked-in Tier A fixtures.
