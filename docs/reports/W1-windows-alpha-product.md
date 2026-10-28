# W1 -- Windows English Alpha product shell, real settings and offline package

Base HEAD: `6bb21244b8db7b4002b71c1b45b7391d08b40ab0`.
Product source commit: `508e663` (the final W1 package was rebuilt after the W1 commits,
so its embedded commit and `package.manifest` both name `508e663`).
W1 local commits: `7f67046` (UTF-8 engine paths), `f168e5d` (engine host deployment
request), `e9e149a` (settings shell + product TSF DLL), `508e663` (offline staging +
hand-off report). This report is refreshed in a follow-up documentation commit; nothing
was pushed or tagged.

W1 is an **internal alpha** productization step, not a system input method release and not
the final Alpha. The settings shell and the unregistered product TSF DLL do not make system
input available; W2/W3 own the keyboard sink, candidate UI, registration and installer.

> **Status update (W2-B).** The historical statements below that say "no candidate UI" were
> true for W1. W2-A added the real key sink and W2-B added the native candidate window and
> the TSF candidate-list UIElement; see `docs/reports/W2-B-candidates.md` and
> `docs/windows/candidate-ui-contract.md`. The product is still unregistered, so W1's
> "not a system IME" conclusion is unchanged.

## 1. Environment (sanitized)

| Item | Observed |
| --- | --- |
| OS | Windows 11 x64, build 26200 |
| Display | 150% per-monitor scaling; the settings window ran at 144 DPI |
| Toolchain | Visual Studio 2022 installation discovered by vswhere, its bundled CMake 3.31.6, MSVC 19.44, x64, static CRT `/MT` |
| Engine | existing English `rime.dll` from the P2.x build, pinned librime 1.17.0 (`33e7814`); no engine rebuild was performed in W1 |
| Test data source | pinned ESDB checkout `build/third-party-fetch/esdb`, commit `7bbe293425d48a1210bf17aaf156b28ade5c2de1` |
| Registration | none; no registry write, no keyboard sink, no system input language change |

## 2. Wiring checklist result

| Seam | Finding | W1 outcome |
| --- | --- | --- |
| Product TSF DLL vs test variant | the P3.1 `epocanva_tsf.dll` was a COM lifecycle skeleton; the hosted test DLL added `engine_host.cc` and a test seam | a separate product DLL compiles the same TSF sources + `engine_host.cc` + product configuration, with no test seam and only the two COM exports |
| `SetEngineHostConfig` | only `test_seam.cc` and broker tests called it | the product DLL resolves its module directory and user root in `DllGetClassObject` (outside DllMain) and installs the config; tests still use the hosted seam |
| Paths | no product layout; the bridge refused every non-ASCII path | product root = module directory; user data = `%LOCALAPPDATA%\EpoCanva`; bridge now converts wide paths to UTF-8 for librime (`u8path` in `third_party/librime/src/rime/common.h:84-96`) |
| Settings | none; schema supports `english/max_completions`, `enable_suggestions`, `max_suggestions` | native settings window + atomic INI-like file + generated Rime user patch + deploy marker; save/read-back verified |
| Candidate UI | none in the product; only the lab | shown read-only as unavailable in W1; no fake toggle |
| Natural keyboard / single flight / Busy / Retiring / QueueFull | implemented in `tsf_coordinator`, not wired to the product DLL (no key sink, session, focus, candidate UI) | explicitly W2 |
| STA result notification | executor worker/result model exists; no TSF sink in the product | W2 |
| Registration / uninstall | no `DllRegisterServer` / `DllUnregisterServer`, no installer | W3; W1 loaded the product DLL only by absolute path in tests |
| Data distribution | only the 54-row fixture and the test-only 42k pilot existed | independent product lexicon build from the pinned public-domain source, with provenance, notices and a whitelist package; pilot file not read or copied by the product tool |

## 3. Design choices and rejected alternatives

1. **Native Win32 settings window.** `EpoCanva.exe` is a small Win32 GUI with standard
   controls, a per-monitor-v2 manifest, tab navigation, 150% DPI layout and no theme system.
   A large UI framework was rejected: the settings surface is small and the product must
   stay easy to audit.
2. **Settings storage.** A fixed `[settings]` key=value file, written to a same-directory
   temporary file, flushed and replaced with `MoveFileExW(REPLACE_EXISTING |
   WRITE_THROUGH)`. SHA-256 is BCrypt CNG. A JSON/YAML parser or a local digest was not
   written. `GetPrivateProfileStringW` was rejected after testing: its per-path cache
   rewrote/truncated the atomically replaced file during read-back.
3. **Engine projection.** Saving writes settings.ini, a generated Rime user patch
   (`epocanva_english.custom.yaml`) and a marker containing the SHA-256 of the exact
   settings bytes. The product TSF DLL deploys the schema at its next activation and
   removes the marker only after a successful start. Always deploying at every activation
   was rejected as unnecessary latency; silently copying data into a shared ASCII directory
   was rejected as a path workaround.
4. **UTF-8 paths.** librime itself accepts UTF-8 and converts with `u8path`; the old ASCII
   refusal was a bridge policy, not an engine limit. The bridge now converts each native
   wide path with `WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS)` and uses `u8path` when
   reconstructing paths for diagnostics and compiled-schema checks. This was verified with
   spaces, Chinese characters and a real engine session.
5. **Product data.** The product lexicon is generated by `tools/data/build_product_lexicon.py`
   from the pinned Alt12Dicts `2of12full.txt`; the tool verifies the git HEAD, a clean
   worktree and the file hash, then reuses the P2.4 converter. It records source commit,
   input/output SHA-256, conversion policy and exclusion tally in a shipped provenance file.
   The P2.4 pilot output is test-only and is not read by this tool. **The bytes are identical
   to the pilot because the source and conversion are identical**; this is disclosed, not
   hidden. Redistribution outside the internal alpha is still an owner/legal decision.
6. **Packaging.** `tools/product/stage_product.ps1` copies an explicit whitelist, refuses
   PDB/test/log/scratch artefacts, scans text payloads for machine paths, copies the actual
   upstream licence files and writes a SHA-256 `package.manifest`. Copying `build/` as a
   tree was rejected.
7. **No system registration.** The W1 package contains a loadable product TSF DLL, but W1
   does not install it, register its CLSID or create a keyboard sink. Calling W1 "system
   input works" would be false.

## 4. Files added / modified

Added:

- `platforms/windows/product/CMakeLists.txt`
- `platforms/windows/product/VERSION`
- `platforms/windows/product/EpoCanva.manifest`
- `platforms/windows/product/EpoCanva.rc`
- `platforms/windows/product/ui_ids.h`
- `platforms/windows/product/app_main.cc`
- `platforms/windows/product/settings_window.{h,cc}`
- `platforms/windows/product/product_paths.{h,cc}`
- `platforms/windows/product/product_io.{h,cc}`
- `platforms/windows/product/product_settings.{h,cc}`
- `platforms/windows/product/product_manifest.{h,cc}`
- `platforms/windows/product/product_diagnostics.{h,cc}`
- `platforms/windows/product/product_engine_config.{h,cc}`
- `platforms/windows/product/tests/product_unit_tests.cc`
- `platforms/windows/product/tests/product_engine_tests.cc`
- `platforms/windows/product/tests/ui_tests.cc`
- `data/product/default.yaml`
- `data/product/epocanva_english.schema.yaml`
- `data/product/NOTICE.txt`
- `data/product/lexicon/README.txt`
- `tools/data/build_product_lexicon.py`
- `tools/product/build_product.ps1`
- `tools/product/stage_product.ps1`
- `docs/reports/W1-windows-alpha-product.md` (this file)

Modified:

- `platforms/windows/engine_bridge/rime_runtime.{h,cc}` -- UTF-8 path conversion
- `platforms/windows/engine_bridge/CMakeLists.txt` -- `EPOCANVA_BUILD_PRODUCT`
- `platforms/windows/tsf/engine_host.{h,cc}` -- optional deployment request in the host config
- `platforms/windows/tsf/text_service.cc` -- retire the marker after a successful engine start
- `platforms/windows/tsf/dll_main.cc` -- product config step in `DllGetClassObject`
- `README.md` -- W1 status, product tree and build entry

## 5. Exact commands and observed exit codes

Run from the repository root. `<repo>` is the working tree; machine-local absolute paths
are recorded in the git-ignored `docs/reports/environment.local.md`.

| Command | Exit |
| --- | --- |
| `python tools/data/build_product_lexicon.py --source-root build/third-party-fetch/esdb --out-dir build/product-data` | 0 |
| `python tools/data/build_product_lexicon.py --source-root build/third-party-fetch/esdb --out-dir build/product-data --check` | 0 (byte-identical) |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/product/build_product.ps1` | 0; CTest 126 registered, 125 passed, 1 environment-limited skip, 0 failed |
| `ctest --test-dir build/vs2022-x64-debug -C Debug --output-on-failure --no-tests=error` | 0 (8 root core/infrastructure tests) |
| `cmake --build build/windows-tsf/test --config Release` then `ctest ...` | 0; 3/3 TSF lifecycle tests |
| `tools/test_english_integration.ps1 ... -PilotLexicon <pilot tsv> -BuildDir build/english-regression-w1 -LogTag w1` | 0; 35/35 English cases |
| `powershell ... tools/product/stage_product.ps1 -Replace` | 0; 21 payload files, 3,988,669 bytes at the observed build; warning about rime.dll embedded paths |
| staged `EpoCanva.exe --diagnose <report>` | 0; `package=ready`, 21/21 manifest files verified |
| staged UI automation (`epocanva_ui_tests.exe`) | 0; `RESULT PASS`; 1372x1052 window at 144 DPI, Tab traversal, Save/read-back, About dialog, screenshot |
| non-ASCII install + user path diagnose | 0; `package=ready` |
| non-ASCII install + user path product TSF activation (`tsf_lifecycle_hosted_compat --mode threadmgr`) | 0; 16/16 checks, compiled schema 0/true/2 (corrected in W2-A: the same-scenario artefacts show settings 0/true/2 and compiled 0/true/2; the W1 text said 1), marker removed |
| missing `rime.dll` diagnose | 2; `package=incomplete`, `manifest_failures=1` |
| missing product lexicon diagnose | 2; `package=incomplete`, `manifest_failures=1` |
| corrupt settings diagnose | 0; `package=ready`, `settings=corrupt` |
| product TSF activation with missing `rime.dll` | 6 (activation failed); marker kept, no compiled schema |
| read-only install (all payload files read-only) + staged UI automation | 0; `RESULT PASS`; no install file content changed and no new install file |
| `dumpbin /imports` on the three payload binaries | no `WS2_32`, `WININET`, `WINHTTP`, `URLMON` or `DNSAPI` imports |
| `dumpbin /exports epocanva_tsf.dll` | exactly `DllCanUnloadNow` and `DllGetClassObject` |

The ACL variant that denies write directly on `EpoCanva.exe` could not be executed on this
host: Windows refused `CreateProcess` with error 5 even though read/execute were allowed.
The accepted read-only test therefore marked every payload file read-only and proved that
the UI run changed no install file and created none; this is recorded so the limitation is
not mistaken for a passing ACL test.

## 6. Test evidence

- **Product CTest:** 126 cases, 125 passed, 1 skipped (`windows.tsf_context_observer.real_focus`,
  environment-limited), 0 failed. This includes the existing engine bridge, executor,
  coordinator, context/observer, hosted TSF, product unit, product engine and product UI
  groups.
- **English regression:** 35/35 formal English integration cases, including pilot manifest
  identity, spelling toggles and real lexicon cases.
- **Root and standalone TSF:** 8/8 root cases; 3/3 P3.1 lifecycle cases.
- **GUI:** the screenshot at `build/product/run/stage-ui-final/settings.png` was captured
  from a real `EpoCanva.exe` process. The window reports `Package ready - 21 files verified`,
  version `0.1.0-w1 (508e663)`, Windows 11 x64, and the saved settings (completion off,
  3 candidates; spelling on, 2 suggestions). The UI test additionally exercised Tab focus
  changes, DPI awareness, the About/licences dialog, Save, process exit and relaunch.
- **Settings persistence evidence:** `build/product/run/stage-ui-final/user/settings.ini`,
  `build/product/run/stage-ui-final/user/engine/epocanva_english.custom.yaml` and
  `.../engine/deploy-required`. The marker token was checked to equal SHA-256 of the
  exact settings bytes.
- **Product TSF deployment:** after activation, the user data contained
  `engine/build/default.yaml` and `engine/build/epocanva_english.schema.yaml`; the compiled
  schema carried the non-default patch values and the marker was gone. With `rime.dll`
  removed, activation failed and the marker remained.
- **Package contents:** 21 verified payload files: `EpoCanva.exe`, `epocanva_tsf.dll`,
  `rime.dll`, `NOTICE.txt`, `VERSION.txt`, `data/default.yaml`,
  `data/epocanva_english.schema.yaml`, `data/lexicon/README.txt`, the 42,356-row TSV, its
  provenance and `licenses/*` (11 files). `package.manifest` itself is the 22nd file on
  disk and is not one of its own rows. No PDB, test DLL, log, scratch or object file is
  present.
- **Lexicon identity:** TSV SHA-256 `6e46587d8029d8c9cf17b20d33983d051caa6e5e70a3da6573707d25400aae6b`,
  502,333 bytes, 42,356 rows, ASCII, LF; matches the provenance and the package manifest.
  Source commit `7bbe293...`, source SHA-256 `76dc8ed...`.

## 7. Unfinished items and risks

1. **No system input yet.** No CLSID registration, no `ITfKeyEventSink`, no focus/context
   binding, no natural keyboard dispatch, no single-flight/Busy/Retiring wiring in the
   product DLL and no candidate UI. These are W2. Do not describe W1 as a usable system IME.
2. **No installer/uninstaller.** No `EpoCanva-Setup.exe`, no signing, no uninstall path.
   W3. Installation/uninstall tests must use a dedicated VM; this host was not modified.
3. **Word-list redistribution is unresolved.** The source is public domain in the US with a
   request for acknowledgment; the project licence is undecided. The package is internal
   alpha only. The product TSV is byte-identical to the test pilot because the source and
   conversion are identical; the product build independently verified the source and did
   not read/copy the pilot file. Owner/legal sign-off is required before any external
   distribution.
4. **Embedded build paths in `rime.dll`.** The staging scan found 68 occurrences of the
   local build prefix (from upstream `__FILE__` exception strings in darts/marisa/librime).
   No text payload has a machine path and no PDB is packaged, but this is a public-release
   blocker. The correct fix is a pinned rebuild with MSVC `/pathmap`, not binary editing.
5. **Deployment repeats after an executor restart in one process.** The broker's config
   snapshot still contains the deployment request; a later start redeploys. It is correct
   but adds latency. W2 should add a one-shot deployment state in the broker.
6. **DPI coverage.** 150% was exercised. 100/125% follow the same scaling path but were not
   re-run on this host; >150% and small work areas are not supported by the fixed layout
   (no scrollbar). This must be revisited before a public alpha.
7. **Settings format migrations.** Only format 1 exists. A newer/unknown format is reported
   and defaults are shown; saving overwrites it. A migration policy is a W3 concern.
8. **UI language.** The shell is English-only; that matches the English Alpha, but the
   Chinese-facing documentation and future localization are not in this build.

## 8. W2/W3 hand-off

W2 should wire the existing coordinator to the product DLL: session creation after
activation, `ITfKeyEventSink`/key translation, focus/context observation, single-flight and
Busy/Retiring/QueueFull outcomes, insertion/edit boundaries, and the candidate UI. The
product config already supplies absolute data/user/log paths and a one-shot deployment
request; W2 should add the one-shot broker state and drive the schema/session from the
configured engine.

W3 should add registration/unregistration, an installer, uninstall, signing and the VM-based
install matrix. Do not use this host's system input settings or registry for those tests.

## 9. Hand-off artefacts

| Artefact | Repository-relative path |
| --- | --- |
| Settings EXE | `build/product/stage/EpoCanva-0.1.0-w1/EpoCanva.exe` |
| Product TSF DLL | `build/product/stage/EpoCanva-0.1.0-w1/epocanva_tsf.dll` |
| Full offline stage | `build/product/stage/EpoCanva-0.1.0-w1/` |
| Window screenshot | `build/product/run/stage-ui-final/settings.png` |
| Saved settings evidence | `build/product/run/stage-ui-final/user/` |
| Product test logs | `build/product/logs/` |
| Product data | `build/product-data/` (generated, not committed) |

The `build/` tree is not distributable; the stage directory is the only packaging output
from W1. The system input method, candidate UI and installer remain future work.
