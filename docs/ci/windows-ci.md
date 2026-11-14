# Minimal Windows CI — design and executable workflow

**Status**: contract gate and serial hosted `root-unit-tests` are implemented;
backend, product, installed-host acceptance and full quality/stress acceptance
remain design-only/BLOCKED. The new root job has not yet executed remotely for
this working-tree change. `.github/workflows/windows-ci.yml` uses the official
`windows-2022` image and a non-gating `windows-latest` canary. It registers only
jobs that need no external Tier 2 build inputs. The section 3.2–3.4 jobs require
the self-hosted runner with the hash-pinned immutable input cache from section 2;
they are still not enabled, and enabling them is a separate reviewed change.

Deviations of the enabled workflow from the original design text, recorded so
this document stays the single description of what runs:

- The contract gate runs on the GitHub-hosted runner instead of a
  self-hosted `epocanva-contract` labelled runner; section 3.1 already allowed
  "hosted or self-hosted". The official gate pins `windows-2022` — the image
  family the toolchain was verified against — and a `windows-latest` canary
  job (continue-on-error) watches for image drift without becoming evidence.
- The contract CTest selection is no longer a regex over a pattern: every
  contract test carries the `contract-gate` label, the build tree's real
  inventory (from `ctest --show-only=json-v1`) must equal the version-
  controlled `tools/ci/contract-tests.json` exactly (missing, duplicate or
  unregistered names all fail), and only then does `ctest -L contract-gate`
  execute the suite. The build step compiles the aggregate target
  `epocanva_contract_gate_tests` instead of a hand-maintained target list.
- The whitespace gate is no longer the bare `git diff --check` (which examined
  a clean checkout's working tree and could not fail): it is
  `tools/ci/check_whitespace.py`, event-aware — `pull_request` compares
  merge-base(base, head)..head, `push` compares before..after, branch
  creation falls back to the default-branch merge-base, and any range that
  cannot be determined fails closed. Checkout uses `fetch-depth: 0` so the
  comparison objects always exist.
- Workflow hardening: `actions/checkout` v7.0.1, `actions/setup-python`
  v7.1.0 and `actions/upload-artifact` v7.0.2 are pinned by full commit SHA;
  checkout sets `persist-credentials: false`; Python is pinned to 3.14; the
  job logs CMake/CTest/MSVC/PowerShell/git versions; `permissions`,
  `timeout-minutes` and same-branch `concurrency` stay minimal.
- The manifest regeneration of section 3.1 step 4 runs explicitly through the
  built locale probe (`--manifest build/ci/manifest/backend-bindings.json`)
  plus `git diff --no-index --exit-code`; the `locale_registry.manifest` CTest
  byte-comparison still runs inside the CTest step as before.
- There is no `schedule` trigger: the only scheduled job in the design is the
  self-hosted nightly stress, which is not enabled.
- The hosted runner sets `core.autocrlf=false`/`core.eol=lf` before checkout;
  `.gitattributes` `eol=lf` already forces LF, so this is defensive.

The hosted root job runs after `contract-gate`, configures/builds every root
Debug target, exports the actual CTest registration, executes the complete suite
serially, and checks the exact registered/executed name set with zero failures,
errors or skipped cases using `tools/ci/verify_root_junit.py`. Logs, commit,
runner/image/tool versions, process exit codes and JUnit are uploaded even on
failure. This is a root-unit layer, not backend/product/installed-host acceptance.
No untrusted pull request is sent to a privileged self-hosted runner.

The aggregate release gate now requires `acceptance-ci-provenance` for the exact
candidate with all six layers/jobs (`contract-gate`, `root-unit-tests`, `backend`,
`windows-product`, `windows-integration-acceptance`,
`full-quality-stress-acceptance`). Missing infrastructure or jobs remain BLOCKED;
the new hosted job does not waive or fabricate any of them.

## 1. Constraints that shape the pipeline

1. **No network I/O at runtime.** CI may fetch the repository and pinned CI
   tooling, but the product and tests never download models, dictionaries or
   telemetry. A job that cannot run without network is a failed job, not a
   skipped one.
2. **Untracked, pinned inputs are required.** The backend and product suites
   read ignored build inputs (`third_party/librime` built DLLs, the five
   production models, Chinese schemas/Terra dictionary, French `.aff/.dic`,
   Japanese `lexicon.bin`, the product English lexicon). A clean checkout does
   not contain them. The design uses a **self-hosted Windows runner with an
   immutable, hash-pinned input cache**; a stock GitHub-hosted runner cannot
   assemble these inputs offline and is therefore not a supported backend/
   product target.
3. **Privacy.** Every CI test uses synthetic fixtures. Uploaded logs must never
   contain candidate text, preedit, committed text, keystrokes, field names or
   user paths from a real profile. The diagnostics added in this round print
   only status names, aggregate counts, stages and Win32 codes.
4. **No parallel full-suite evidence.** Contract gate, backend and product are a
   serial chain (`needs:`), not three concurrent jobs. Nightly stress repeats
   one case at a time on the same runner. Parallel full suites previously
   produced one unexplained `learning.repository.persistence` `io-error`; the
   pipeline must not reproduce that noise as its primary evidence.
5. **Build hygiene.** All outputs stay under `build/<preset>/` in the checkout.
   The source tree must be byte-identical before and after the job.

## 2. Runner and input cache

| Item | Requirement |
| --- | --- |
| OS | Windows Server 2022 or Windows 11 x64 |
| Toolchain | Visual Studio 2022 Professional/Build Tools, MSVC 14.44, Windows SDK 10.0.26100 |
| CMake | 3.31.6-msvc6 (VS-bundled) or newer; do not put a machine path in the repo |
| Python | 3.11+ (standard library only for the staging tests) |
| Git | 2.52+ with `core.autocrlf=false` (the repository pins LF) |
| Cache root | `D:\epocanva-inputs\<sources-lock-sha256>` on a local NTFS volume, **not** a synced folder |

The cache root mirrors the checkout: repo-relative paths below live under
`<cache>\build\...` or `<cache>\third_party\...`. The cache contains exactly the
ignored inputs the suites pin:

```text
third_party/librime/                       (checkout at the locked commit)
build/librime-english/rime/install/lib/rime.dll
build/librime-baseline/rime/install/lib/rime.dll
build/backend-training-budget/{en,zh,fr,ja,ko}/model.tsv
build/backend-chinese-ranked/shared/{default.yaml,epocanva_backend_pinyin.schema.yaml,
  epocanva_backend_terra.dict.yaml,epocanva_backend_zhuyin.schema.yaml,zhuyin.yaml}
build/backend-data-acquire/zh-fr/upstream/french/fr_FR/{fr.aff,fr.dic}
build/backend-japanese-data/lexicon.bin
build/product-data/epocanva_en_alpha.tsv
```

A `resources` job (not shown as a per-PR job) builds or refreshes this cache
from the pinned sources, verifies every file against the SHA-256 values already
recorded in the test sources/manifests, and publishes `inputs.sha256`. Every
downstream job runs `tools/ci/verify_inputs.ps1` (to be added with the workflow)
and fails before building when a hash differs. A cache refresh is an explicit
maintainer action, never an automatic download during a test job.

If a future change makes these inputs reproducible from committed sources alone,
the self-hosted requirement can be removed; until then, claiming a from-zero
clean-checkout backend/product build would be false.

## 3. Job graph

```text
contract-gate  ──needs──>  backend  ──needs──>  windows-product
                                                    │
nightly-stress (schedule, independent, own runner)  ┘
```

### 3.1 Contract gate (hosted or self-hosted, ~5 minutes)

Checks, in order:

1. `git diff --check` — no whitespace errors.
2. Configure the root preset and build only the two target groups.
3. `ctest -R "^(locale_registry|candidate_contract)\."` — 5 locale tests
   (including the deterministic manifest comparison and the falsifiable stale
   rejection) and 3 candidate tests (including the ABI-mirror checks).
4. Regenerate the manifest into `build/ci/manifest/` and byte-compare with
   `data/locale/backend-bindings.json`; the CTest does this, and the explicit
   copy makes the artefact reviewable.
5. `python -B -m unittest discover -s tests/backend_resource_staging -p 'test_*.py'`
   — manifest-only staging, drift/duplicate/absent/wrong-digest negatives.
   The real-resource test is excluded here and runs in the `backend` job after
   the input cache is verified.
6. `git status --porcelain --untracked-files=all` must be empty. Build outputs
   live under the ignored `/build/` tree; a manifest or staging output in the
   source tree fails the gate.
7. `git diff --exit-code -- data/locale/backend-bindings.json` after the
   regeneration step; a stale committed manifest is a failure, not a warning.

The gate does not need the external input cache.

### 3.2 Backend (self-hosted, ~10 minutes)

1. Verify `D:\epocanva-inputs\<sha>\inputs.sha256`.
2. Link or copy the cache into `build/` with `robocopy` (copy, not move; the
   cache stays immutable).
3. Configure `tools/backend` with `EPOCANVA_BACKEND_WITH_FRENCH_DICTIONARY=ON`,
   build Release, run the full 58-test suite serially.
4. Run `EPOCANVA_STAGING_REAL_TESTS=1 python -B -m unittest ...` — the real
   five-language staging test compares deterministic manifests and every output
   hash, then corrupts an output and requires rejection.
5. `git status --porcelain` must be empty.

### 3.3 Windows Product (self-hosted, ~20 minutes)

1. Same input verification and copy.
2. Configure `platforms/windows/engine_bridge` with
   `EPOCANVA_BUILD_PRODUCT=ON`, the backend resource root, the product lexicon,
   the English Rime DLL and `EPOCANVA_PRODUCT_WITH_FRENCH_DICTIONARY=ON`.
3. Build Release, run the full 235-test suite serially. Two environment-limited
   skips (`learning.repository.symlinks`, `windows.tsf_context_observer.real_focus`)
   are reported as skips, never as passes.
4. Upload `build/windows-product/product-build-receipt.json` on success.
5. `git status --porcelain` must be empty.

### 3.4 Nightly stress (self-hosted, scheduled, ~60 minutes)

Repeat one case at a time, serial, with a bounded per-run timeout:

| Case | Repetitions | Purpose |
| --- | ---: | --- |
| `backend.profiles.real_acceptance` | 10 | historical flake; real five-language slice |
| `learning.repository.persistence` | 50 | v1→v3 migration and atomic replacement |
| `learning.repository.corruption` | 20 | quarantine and recovery |
| `product.integration.sink_settings_timing` | 20 | shared readiness deadline |
| `product.integration.sink_lease_lock` | 20 | shared engine lease and release |

After the ten serial profile runs, run the same binary 4×4 concurrently **as a
stress diagnostic, not as release evidence**. A failure there is a finding; a
pass does not upgrade the serial result.

On failure, upload only:

- `build/**/Testing/Temporary/LastTest.log`
- `build/**/Testing/Temporary/LastTestsFailed.log`
- `build/ci-logs/*.log` written by the loop wrapper (stdout/stderr of the test)

Do not upload `%APPDATA%`, `%LOCALAPPDATA%`, real user profiles, clipboard data or
any captured input. The stress wrapper must not add logging that prints candidate
text; the existing diagnostics are text-free by construction.

## 4. Executable workflow

The contract-gate job is now committed as `.github/workflows/windows-ci.yml`
(see the status note above for its deviations from the text below). The full
workflow text below remains the design for the complete pipeline; the backend,
product and nightly-stress jobs are **not** in the committed file.
`EPOCANVA_INPUT_CACHE` is a runner-level variable, not a repository secret.

```yaml
name: windows-ci

on:
  pull_request:
  push:
    branches: [main, "feat/**"]
  schedule:
    - cron: "0 18 * * *"   # 02:00 Asia/Taipei, nightly
  workflow_dispatch:

permissions:
  contents: read

concurrency:
  group: windows-ci-${{ github.workflow }}-${{ github.ref }}
  cancel-in-progress: true

jobs:
  contract-gate:
    runs-on: [self-hosted, windows, x64, epocanva-contract]
    timeout-minutes: 20
    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 1
      - name: Locate VS CMake
        shell: pwsh
        run: |
          $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
          $cmake = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -find Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
          if (-not $cmake) { throw "VS CMake not found" }
          "EPOCANVA_CMAKE=$cmake" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
      - name: Whitespace gate
        shell: pwsh
        run: git diff --check
      - name: Configure
        shell: pwsh
        run: |
          & $env:EPOCANVA_CMAKE --preset vs2022-x64-debug
      - name: Build contract targets
        shell: pwsh
        run: |
          & $env:EPOCANVA_CMAKE --build build/vs2022-x64-debug --config Debug `
            --target epocanva_locale_registry_tests epocanva_candidate_contract_tests `
            --parallel 4
      - name: Contract CTests
        shell: pwsh
        run: |
          & (Join-Path (Split-Path $env:EPOCANVA_CMAKE) "ctest.exe") `
            --test-dir build/vs2022-x64-debug -C Debug `
            -R "^(locale_registry|candidate_contract)\." --output-on-failure
      - name: Python staging unit tests
        shell: pwsh
        run: python -B -m unittest discover -s tests/backend_resource_staging -p "test_*.py"
      - name: Source tree must stay clean
        shell: pwsh
        run: |
          $status = git status --porcelain --untracked-files=all
          if ($status) { $status; throw "source tree changed during the gate" }

  backend:
    needs: contract-gate
    runs-on: [self-hosted, windows, x64, epocanva-inputs]
    timeout-minutes: 30
    env:
      EPOCANVA_INPUT_CACHE: ${{ vars.EPOCANVA_INPUT_CACHE }}
    steps:
      - uses: actions/checkout@v4
      - name: Verify and copy pinned inputs
        shell: pwsh
        run: |
          if (-not $env:EPOCANVA_INPUT_CACHE) { throw "input cache is not configured" }
          & "$env:EPOCANVA_INPUT_CACHE\verify_inputs.ps1"
          robocopy "$env:EPOCANVA_INPUT_CACHE\build" build /E /XD inputs /NFL /NDL /NJH /NJS
          robocopy "$env:EPOCANVA_INPUT_CACHE\third_party" third_party /E /NFL /NDL /NJH /NJS
          if ($LASTEXITCODE -ge 8) { throw "input copy failed: $LASTEXITCODE" }
      - name: Configure backend
        shell: pwsh
        run: |
          $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
          $cmake = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -find Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
          "EPOCANVA_CMAKE=$cmake" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
          & $cmake -S tools/backend -B build/backend-service -G "Visual Studio 17 2022" -A x64 `
            -DEPOCANVA_BACKEND_WITH_FRENCH_DICTIONARY=ON
      - name: Build
        shell: pwsh
        run: |
          & $env:EPOCANVA_CMAKE --build build/backend-service --config Release --parallel 8
      - name: Backend CTests
        shell: pwsh
        run: |
          & (Join-Path (Split-Path $env:EPOCANVA_CMAKE) "ctest.exe") `
            --test-dir build/backend-service -C Release --no-tests=error --output-on-failure
      - name: Real staging test
        shell: pwsh
        run: |
          $env:EPOCANVA_STAGING_REAL_TESTS = "1"
          python -B -m unittest discover -s tests/backend_resource_staging -p "test_*.py"
      - name: Upload logs on failure
        if: failure()
        uses: actions/upload-artifact@v4
        with:
          name: backend-logs-${{ github.run_attempt }}
          path: |
            build/backend-service/Testing/Temporary/LastTest.log
            build/backend-service/Testing/Temporary/LastTestsFailed.log
          if-no-files-found: ignore

  windows-product:
    needs: backend
    runs-on: [self-hosted, windows, x64, epocanva-inputs]
    timeout-minutes: 45
    env:
      EPOCANVA_INPUT_CACHE: ${{ vars.EPOCANVA_INPUT_CACHE }}
    steps:
      - uses: actions/checkout@v4
      - name: Verify and copy pinned inputs
        shell: pwsh
        run: |
          & "$env:EPOCANVA_INPUT_CACHE\verify_inputs.ps1"
          robocopy "$env:EPOCANVA_INPUT_CACHE\build" build /E /XD inputs /NFL /NDL /NJH /NJS
          robocopy "$env:EPOCANVA_INPUT_CACHE\third_party" third_party /E /NFL /NDL /NJH /NJS
          if ($LASTEXITCODE -ge 8) { throw "input copy failed: $LASTEXITCODE" }
      - name: Stage backend resources
        shell: pwsh
        run: |
          python -B tools/backend/stage_resources.py `
            --product-root build/product-resources/product --oov error
      - name: Configure product
        shell: pwsh
        run: |
          $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
          $cmake = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -find Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
          "EPOCANVA_CMAKE=$cmake" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
          & $cmake -S platforms/windows/engine_bridge -B build/windows-product `
            -G "Visual Studio 17 2022" -A x64 `
            -DEPOCANVA_BUILD_PRODUCT=ON `
            -DEPOCANVA_PRODUCT_BACKEND_ROOT="$PWD/build/product-resources/product" `
            -DEPOCANVA_PRODUCT_LEXICON="$PWD/build/product-data/epocanva_en_alpha.tsv" `
            -DEPOCANVA_RIME_DLL="$PWD/build/librime-english/rime/install/lib/rime.dll" `
            -DEPOCANVA_BASELINE_RIME_DLL="$PWD/build/librime-baseline/rime/install/lib/rime.dll" `
            -DEPOCANVA_PRODUCT_WITH_FRENCH_DICTIONARY=ON
      - name: Build
        shell: pwsh
        run: |
          & $env:EPOCANVA_CMAKE --build build/windows-product --config Release --parallel 8
      - name: Product CTests
        shell: pwsh
        run: |
          & (Join-Path (Split-Path $env:EPOCANVA_CMAKE) "ctest.exe") `
            --test-dir build/windows-product -C Release --no-tests=error --output-on-failure
      - name: Upload receipt
        if: success()
        uses: actions/upload-artifact@v4
        with:
          name: product-build-receipt-${{ github.run_attempt }}
          path: build/windows-product/product-build-receipt.json
          if-no-files-found: error
      - name: Upload logs on failure
        if: failure()
        uses: actions/upload-artifact@v4
        with:
          name: product-logs-${{ github.run_attempt }}
          path: |
            build/windows-product/Testing/Temporary/LastTest.log
            build/windows-product/Testing/Temporary/LastTestsFailed.log
          if-no-files-found: ignore

  nightly-stress:
    if: github.event_name == 'schedule' || github.event_name == 'workflow_dispatch'
    runs-on: [self-hosted, windows, x64, epocanva-stress]
    timeout-minutes: 120
    env:
      EPOCANVA_INPUT_CACHE: ${{ vars.EPOCANVA_INPUT_CACHE }}
    steps:
      - uses: actions/checkout@v4
      - name: Verify and copy pinned inputs
        shell: pwsh
        run: |
          & "$env:EPOCANVA_INPUT_CACHE\verify_inputs.ps1"
          robocopy "$env:EPOCANVA_INPUT_CACHE\build" build /E /XD inputs /NFL /NDL /NJH /NJS
          robocopy "$env:EPOCANVA_INPUT_CACHE\third_party" third_party /E /NFL /NDL /NJH /NJS
          if ($LASTEXITCODE -ge 8) { throw "input copy failed: $LASTEXITCODE" }
      - name: Build all three trees once
        shell: pwsh
        run: |
          $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
          $cmake = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -find Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
          if (-not $cmake) { throw "VS CMake not found" }
          "EPOCANVA_CMAKE=$cmake" | Out-File $env:GITHUB_ENV -Append -Encoding utf8
          "EPOCANVA_CTEST=$(Join-Path (Split-Path $cmake) 'ctest.exe')" |
            Out-File $env:GITHUB_ENV -Append -Encoding utf8
          & $cmake --preset vs2022-x64-debug
          & $cmake --build build/vs2022-x64-debug --config Debug `
            --target epocanva_learning_repository_tests --parallel 8
          & $cmake -S tools/backend -B build/backend-service -G "Visual Studio 17 2022" -A x64 `
            -DEPOCANVA_BACKEND_WITH_FRENCH_DICTIONARY=ON
          & $cmake --build build/backend-service --config Release --parallel 8
          python -B tools/backend/stage_resources.py `
            --product-root build/product-resources/product --oov error
          & $cmake -S platforms/windows/engine_bridge -B build/windows-product `
            -G "Visual Studio 17 2022" -A x64 `
            -DEPOCANVA_BUILD_PRODUCT=ON `
            -DEPOCANVA_PRODUCT_BACKEND_ROOT="$PWD/build/product-resources/product" `
            -DEPOCANVA_PRODUCT_LEXICON="$PWD/build/product-data/epocanva_en_alpha.tsv" `
            -DEPOCANVA_RIME_DLL="$PWD/build/librime-english/rime/install/lib/rime.dll" `
            -DEPOCANVA_BASELINE_RIME_DLL="$PWD/build/librime-baseline/rime/install/lib/rime.dll" `
            -DEPOCANVA_PRODUCT_WITH_FRENCH_DICTIONARY=ON
          & $cmake --build build/windows-product --config Release --parallel 8
      - name: Repeat stress cases
        shell: pwsh
        run: |
          New-Item -ItemType Directory -Force build/ci-logs | Out-Null
          $ctest = $env:EPOCANVA_CTEST
          function Repeat([string]$Dir, [string]$Config, [string]$Name, [int]$Count) {
            for ($i = 1; $i -le $Count; $i++) {
              & $ctest --test-dir $Dir -C $Config --no-tests=error `
                -R "^$Name$" --output-on-failure `
                2>&1 | Tee-Object -FilePath "build/ci-logs/$Name-$i.log"
              if ($LASTEXITCODE -ne 0) { throw "$Name failed on iteration $i" }
            }
          }
          Repeat "build/backend-service" "Release" "backend.profiles.real_acceptance" 10
          Repeat "build/vs2022-x64-debug" "Debug" "learning.repository.persistence" 50
          Repeat "build/vs2022-x64-debug" "Debug" "learning.repository.corruption" 20
          Repeat "build/windows-product" "Release" "product.integration.sink_settings_timing" 20
          Repeat "build/windows-product" "Release" "product.integration.sink_lease_lock" 20
      - name: Upload stress logs on failure
        if: failure()
        uses: actions/upload-artifact@v4
        with:
          name: stress-logs-${{ github.run_attempt }}
          path: |
            build/ci-logs/*.log
            build/backend-service/Testing/Temporary/LastTest.log
            build/windows-product/Testing/Temporary/LastTest.log
          if-no-files-found: ignore
```

## 5. Workflow notes

- `shell: pwsh` on GitHub Actions fails a step when the **last** native command
  exits non-zero. Multi-command steps above rely on that; when enabling the
  workflow, add `$PSNativeCommandUseErrorActionPreference = $true` (PowerShell
  7.3+) to the top of each step, or check `$LASTEXITCODE` explicitly after every
  `cmake`, `ctest`, `git` and `python` invocation. Do not let a failed build be
  masked by a later successful command.
- `robocopy` treats exit codes 0–7 as success and 8+ as failure; the copy steps
  check exactly that.
- `EPOCANVA_INPUT_CACHE` is a runner-level repository variable, not a secret and
  not a path committed to the repository.
- The workflow is inactive until `.github/workflows/windows-ci.yml` is added in a
  reviewed change. This document alone does not create CI.

## 6. What this pipeline does not claim

- It does not make a clean checkout self-sufficient for backend/product; the
  pinned input cache is still an external, hash-verified prerequisite.
- It does not run a real registered TSF host or Android. Those remain separate
  acceptance jobs with their own evidence rules.
- It does not convert a skipped test into a pass. `ctest` exit code 0 with skips
  is reported with the skip count; release evidence requires the specific cases
  to execute.
- It does not add retries. A flaky case is reported with its text-free
  diagnostics and must be diagnosed, not retried into green.

## 7. Rollback

Deleting `.github/workflows/windows-ci.yml` disables the pipeline; no product
code depends on it. The `tools/ci/verify_inputs.ps1` helper is additive and can
remain for manual clean-worktree verification.
