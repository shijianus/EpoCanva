# Clean-worktree verification plan

**Status**: plan + execution record. The plan below was executed once against
the stabilization round's final code state (`f8ecb6940bd5dfac4595226a3f9aa900d3f00d3f`
— the D commit plus the one-file payload-pin fix §13.4 of
`docs/reports/phase-e-followup-implementation.md` records; the worktree's
tested tree was verified byte-identical to that commit) in a fresh detached
worktree on a local NTFS volume. Results are recorded in that report §13.3:
root 81/81 with one declared skip and two pilot-lexicon cases absent because
the generated P2.4 word list is not committed; backend 58/58 after
provisioning the default `build/backend-training/` input; product 233/233
with 2 declared skips after the payload-pin fix; staging TSV hash
byte-identical. The run also exposed a pre-existing defect: the release-gate
payload pins had been computed over stale CRLF working-tree bytes and failed
on any compliant fresh checkout — fixed by the commit referenced above. The
worktree still does not prove a from-zero Tier 2 input cache; §1's tiering
statement below is unchanged.

The original plan text is kept below so the next round can repeat it verbatim.

## 1. What "clean" can and cannot prove

A fresh `git worktree` proves the **source tree** is self-contained: every
CMake file, source file, manifest and test script is committed, and no build
output lives in the source tree.

It does **not** by itself prove a from-zero backend/product build. Those suites
read ignored, pinned inputs that are not in Git:

```text
third_party/librime/                       (checkout at the locked commit)
build/librime-english/rime/install/lib/rime.dll
build/librime-baseline/rime/install/lib/rime.dll
build/backend-training-budget/{en,zh,fr,ja,ko}/model.tsv
build/backend-chinese-ranked/shared/*
build/backend-data-acquire/zh-fr/upstream/french/fr_FR/{fr.aff,fr.dic}
build/backend-japanese-data/lexicon.bin
build/product-data/epocanva_en_alpha.tsv
```

The plan therefore has two tiers:

- **Tier 1 — source-clean**: worktree + root configure/build/test. No external
  inputs. This proves the committed source and the manifest/staging gates.
- **Tier 2 — resource-clean**: the same worktree plus a read-only, hash-verified
  copy of the pinned inputs from an immutable cache. This proves the backend and
  product trees do not depend on *unverified* untracked files; it still depends
  on externally provisioned inputs. Report that dependency explicitly.

If the requirement is a literal "no untracked file is read", Tier 2 is
**BLOCKED**: the repository does not yet contain or reproduce those inputs. The
fix is the CI input cache described in `docs/ci/windows-ci.md`, not copying
arbitrary files from the original workspace.

## 2. Create the worktree

Run from the original checkout after the commits exist and the working tree is
clean:

```sh
git status --porcelain=v1 --untracked-files=all   # must be empty
git rev-parse HEAD                               # record the tip SHA
git worktree add --detach <clean-path> <tip-sha>
git -C <clean-path> status --porcelain=v1 --untracked-files=all  # must be empty
```

`<clean-path>` must be outside the original checkout (for example
`D:\epocanva-clean-<sha>`), on a local NTFS volume, not a synced folder. Do not
nest it under the original repository; a nested worktree makes "which `build/`
did this use?" ambiguous.

Record:

```sh
git -C <clean-path> rev-parse HEAD
git -C <clean-path> diff --check
git -C <clean-path> ls-files | grep -E '(^|/)(CMakeLists\.txt|.*\.cmake)$'
```

Confirm the new files are tracked, not merely present:

```sh
git -C <clean-path> ls-files --error-unmatch \
  engine/candidates/CMakeLists.txt \
  engine/candidates/candidate_contract.cc \
  engine/candidates/candidate_contract.h \
  engine/locale/backend_abi_values.h \
  engine/locale/backend_manifest.cc \
  engine/locale/backend_manifest.h \
  tests/candidate_contract/CMakeLists.txt \
  tests/locale_registry/assert_manifest.cmake \
  data/locale/backend-bindings.json
```

A missing path means a commit group is incomplete; fix the commit group, not
the worktree.

## 3. Configure and build the three trees, serially

Use the VS 2022 bundled CMake/CTest. Run from `<clean-path>`; all outputs stay
under `<clean-path>/build/`.

### 3.1 Root (Tier 1)

```sh
cmake --preset vs2022-x64-debug
cmake --build build/vs2022-x64-debug --config Debug --parallel 8
ctest --test-dir build/vs2022-x64-debug -C Debug \
  --no-tests=error --output-on-failure --output-junit build/root-junit.xml
```

Expected: **83 total, 82 pass, 0 fail, 1 skip**
(`learning.repository.symlinks`, symlink privilege). Exit 0.

Manifest freshness is part of this run:

```sh
ctest --test-dir build/vs2022-x64-debug -C Debug \
  -R "^locale_registry\.manifest" --output-on-failure
git -C <clean-path> status --porcelain=v1 --untracked-files=all  # must be empty
```

The manifest test exports to `build/.../manifest` and compares bytes; it must
not modify `data/locale/backend-bindings.json`. If `git status` is non-empty,
the gate wrote into the source tree or a generated file was committed by
accident.

### 3.2 Backend (Tier 2)

Provision the pinned inputs first. Copy from a read-only cache, not from the
original worktree if the cache exists:

```sh
robocopy <cache>\build <clean-path>\build /E /NFL /NDL /NJH /NJS
robocopy <cache>\third_party\ <clean-path>\third_party\ /E /NFL /NDL /NJH /NJS
```

Verify every copied file against the pins recorded in `tests/backend/profiles.cc`
and the model manifests **before** building. A hash mismatch stops here; do not
continue with a "maybe it is fine" input.

```sh
cmake -S tools/backend -B build/backend-service \
  -G "Visual Studio 17 2022" -A x64 \
  -DEPOCANVA_BACKEND_WITH_FRENCH_DICTIONARY=ON
cmake --build build/backend-service --config Release --parallel 8
ctest --test-dir build/backend-service -C Release \
  --no-tests=error --output-on-failure --output-junit build/backend-junit.xml
```

Expected: **58 total, 58 pass, 0 fail**. Exit 0.

### 3.3 Windows Product (Tier 2)

Stage the product resources from the same verified inputs:

```sh
python -B tools/backend/stage_resources.py \
  --product-root build/product-resources/product --oov error
```

Expected: 13 profiles, 145 inputs, `resources.tsv` 2,983 bytes, SHA-256
`f6acd2eaf5c0a660404944d5ddcaaad1477f369035247b057e24c2fb543e9ffd`.

```sh
cmake -S platforms/windows/engine_bridge -B build/windows-product \
  -G "Visual Studio 17 2022" -A x64 \
  -DEPOCANVA_BUILD_PRODUCT=ON \
  -DEPOCANVA_PRODUCT_BACKEND_ROOT="$PWD/build/product-resources/product" \
  -DEPOCANVA_PRODUCT_LEXICON="$PWD/build/product-data/epocanva_en_alpha.tsv" \
  -DEPOCANVA_RIME_DLL="$PWD/build/librime-english/rime/install/lib/rime.dll" \
  -DEPOCANVA_BASELINE_RIME_DLL="$PWD/build/librime-baseline/rime/install/lib/rime.dll" \
  -DEPOCANVA_PRODUCT_WITH_FRENCH_DICTIONARY=ON
cmake --build build/windows-product --config Release --parallel 8
ctest --test-dir build/windows-product -C Release \
  --no-tests=error --output-on-failure --output-junit build/product-junit.xml
```

Expected: **235 total, 233 pass, 0 fail, 2 skip**
(`learning.repository.symlinks`, `windows.tsf_context_observer.real_focus`).
Exit 0.

The product suite takes about 14 minutes. Do not run it concurrently with the
root or backend suite as primary evidence; serial order is part of the plan.

## 4. Post-run checks

```sh
git -C <clean-path> status --porcelain=v1 --untracked-files=all
git -C <clean-path> diff --check
```

Both must be clean/empty. A build artefact under the source tree is a failure
even if every test passed.

Check that the CMake source list is complete:

```sh
git -C <clean-path> ls-files '**/CMakeLists.txt' '**/*.cmake' | wc -l
```

Compare with the original checkout's tracked set; the number must match. If the
clean worktree configures but a target is missing, an `add_subdirectory` or
source file is untracked.

## 5. Evidence to record

- `<clean-path>` tip SHA and `git status` output (empty).
- Exact configure/build/ctest commands and exit codes.
- The three JUnit XML files (under `build/`, not committed).
- `resources.tsv` size and SHA-256.
- The manifest freshness CTest output and the post-run `git status`.
- Any skipped test names and the reason.
- For Tier 2, the input cache identity/hashes and the statement that these are
  external pinned inputs, not committed source.

## 6. Failure handling

| Symptom | Meaning | Action |
| --- | --- | --- |
| Configure fails on a missing target/source | a commit group omitted a file | fix the commit group; do not add the file only in the worktree |
| Root tests pass, backend/product fail on a missing model/dictionary | Tier 2 input not provisioned or hash mismatch | verify the cache; do not copy from the original workspace silently |
| Manifest CTest fails | `data/locale/backend-bindings.json` is stale or the export is nondeterministic | regenerate from the C++ probe, review the diff, amend the commit |
| `git status` non-empty after tests | test wrote into the source tree | stop; find the writer; it must write under `build/` |
| Product timing test fails under load | bounded readiness not sufficient | record the text-free diagnostics; do not raise the timeout first |

## 7. Cleanup

After archiving the JUnit XML and the text-free logs:

```sh
git worktree remove <clean-path>
git worktree prune
```

`git worktree remove` refuses a dirty worktree; that refusal is useful evidence.
Do not use `rm -rf` on a worktree that contains unrecorded logs. Do not create a
nested repository or a second `.git`.
