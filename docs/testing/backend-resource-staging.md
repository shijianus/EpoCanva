# Local backend resource staging

`tools/backend/stage_resources.py` assembles the reviewed five-language corpus
baseline and conversion data for internal product integration. It uses Python's
standard library, reads local files only, and does not invoke downloads,
subprocesses, installation, training, or product configuration changes.

Run from any working directory; relative arguments are relative to the tool's
checkout, not the current directory. The product root is required and used
exactly as supplied. It must be a strict descendant of `build/<preset>/`.

```sh
python -B tools/backend/stage_resources.py \
  --product-root build/ime-resources/product --oov error
```

The Python build entrypoint is

```python
stage_resources(product_root, *, repo_root=None, oov="skip", check_only=False)
```

It returns the deterministic staging-manifest dictionary. `repo_root` defaults
to the script's checkout. Missing/mismatching resources raise `StagingError`;
filesystem failures raise `OSError`. The CLI returns 0 on success, 1 on an
admission/provenance/output failure, and argparse's 2 on invalid arguments.
`--check-only` verifies the complete dependency inventory and target conflicts
without creating output. There is no `--download`, `--force`, or trust-current
hash option. Use `-B` for import/test commands so Python bytecode stays out of
the source tree.

## Verification and local dependency inventory

The tool's `REVIEWED_REPORTS` binds the existing experiment's report bytes by
SHA-256. Those reviewed reports provide expected resource hashes. Resource
identity is never inferred by hashing an arbitrary current file and calling it
verified. A new experiment requires explicit review and changing those anchors;
the CLI cannot silently trust a replacement report beside a replacement model.

| Input | Independent expected identity and checks |
| --- | --- |
| `build/backend-training-budget/{en,zh,fr,ja,ko}/model.tsv` | Reviewed `summary.json` whole-file SHA-256 and byte count; reviewed per-language report source/payload fingerprints, token totals and lexical-record count |
| Training corpus and splits | Repository `backend-ja-corpus.sources.json` prepared-corpus hashes; reviewed report input/split hashes; attribution JSONL reconstructed from train/dev/test and matched to the report's attribution digest/count |
| Japanese segmentation catalog | Reviewed Japanese report `catalog_sha256` |
| `build/backend-japanese-data/lexicon.bin` | Reviewed `lexicon.manifest.json` output SHA-256/byte count; revision/source list cross-checked against repository Mozc pins; every acquired pinned Mozc payload checked locally |
| French `fr.aff` / `fr.dic` | Repository acquisition manifest and independent French provider manifest agree; both original file hashes checked; no rewriting or dictionary expansion |
| `build/backend-chinese-ranked/shared/*.yaml` | Reviewed ranked manifest hashes; original Terra/Zhuyin hashes and corpus hash cross-checked with repository acquisition pins; project schemas checked against the ranked manifest |
| Hunspell | Repository archive pin and every enumerated library/notice file hash; no rebuilding, downloads, or claim that these files verify a product binary |
| Tatoeba/Mozc/French/Chinese notices | Existing pinned license/source evidence and author references copied or derived; project full AGPL text verified against its licensing decision record |

The manifest inventories 144 local inputs for the reviewed resource set.
Missing corpus archives, split files, source dictionary rows, notices, reports,
models, or provider files fail staging. This deliberately requires the existing
local preparation closure rather than merely the final binary/model files.
Repository manifests and project-authored definitions are the trust inputs;
their recorded digests support reproducibility, not upstream authorship or
cryptographic signature claims. Historical reports retain their original
machine-local path strings, but the tool never uses those strings to discover
or read arbitrary filesystem paths.

## Output contract

`data/backend/resources.tsv` has strict UTF-8, LF endings including a final LF,
and no BOM. Its first two lines are

```text
epocanva-backend-resources\t1
profile_id\tlanguage\tinput_mode\toutput_mode\tkeyboard_layout\tvariant_id\tmodel_path\tmodel_sha256\tconversion_path\tconversion_sha256\tauxiliary_path\tauxiliary_sha256\tabbreviation_path\tabbreviation_sha256
```

Here `\t` denotes one actual TAB. There are exactly 14 columns, at most 32
profile records and at most 65,536 UTF-8 code units (bytes). All paths are
slash-separated and relative to the product root. Optional absent resources
use `-` in both path and hash columns. A model path/hash is always required.

The current enum values are read from `engine/include/epocanva_backend.h`, not
invented in the staging tool. Profile IDs are fixed as follows.

| ID | Language | Input mode | Output mode | Keyboard | Variant |
| ---: | --- | --- | --- | --- | --- |
| 1 | en | Latin | Latin | US QWERTY | en-US |
| 2 | zh | Pinyin | Hanzi | US QWERTY | zh-pinyin |
| 3 | fr | Latin | Latin | French AZERTY | fr-FR |
| 4 | ja | Romaji | Kanji | US QWERTY | ja-JP |
| 5 | ko | TwoSet | Hangul | US QWERTY | ko-KR |
| 6 | zh | Zhuyin | Hanzi | US QWERTY | zh-zhuyin |
| 7 | ja | Romaji | Hiragana | US QWERTY | ja-JP |
| 8 | ja | Romaji | Katakana | US QWERTY | ja-JP |
| 9 | ja | native Kana | Kanji | US QWERTY | ja-JP |
| 10 | ko | native Hangul via API LATIN mode | Hangul | US QWERTY | ko-KR |
| 11 | en | Latin | Latin | German QWERTZ | en-US |
| 12 | en | Latin | Latin | Dvorak | en-US |
| 13 | en | Latin | Latin | French AZERTY | en-US |

Native Korean uses the currently admitted `EPOCANVA_BACKEND_LATIN` enum for
already composed UTF-8 Hangul; the current `ModeValid` implementation admits
LATIN/TWO_SET for Korean. Native Japanese uses `EPOCANVA_BACKEND_KANA`.
Multiple profiles reference the same model and conversion bytes. `en-US` is the
requested profile label; the Tatoeba model does not establish an independently
curated US dialect corpus. No en-GB model or distinct Hans/Hant resources are
supplied or claimed.

Chinese profiles 2 and 6 bind `data/epocanva_backend_terra.dict.yaml` as their
`conversion_path`/SHA-256. The backend uses that hash-pinned Terra dictionary for
bounded compact-pinyin initial decoding (`nh` → candidate 你好); Rime consumes
the same file for full-pinyin conversion. A failed or missing binding disables
the initialism source instead of silently discovering an unverified file under
the Rime data directory.

The tool writes

- `data/backend/{language}/model.tsv` for all five languages.
- `data/backend/ja/lexicon.bin`, `data/backend/fr/fr.aff` and `fr.dic`.
- `data/backend/{en,fr}/abbreviations.tsv`.
- `data/epocanva_backend_pinyin.schema.yaml`,
  `data/epocanva_backend_zhuyin.schema.yaml`,
  `data/epocanva_backend_terra.dict.yaml`, and `data/zhuyin.yaml`.
- `data/backend/notices/`, including all available pinned texts, model author
  references, separate Chinese-weighting author references, and a clear
  residual-blocker notice.
- `data/backend/provenance/`, retaining reviewed reports, source manifests, and
  project shorthand definitions, plus `data/backend/staging-manifest.json`.

Neither root `default.yaml` nor `data/default.yaml` is written. Existing English
schemas, dictionaries, defaults, runtime state and foreign product files remain
unchanged. The complete generated file list, SHA-256 and byte count are in the
staging manifest; its own digest is intentionally not self-referential.

## Abbreviation definitions and partial keys

Project-authored `data/backend/abbreviations/en.tsv` preserves all 36 original
`kExpansions` mappings from `engine/languages/en/language_support.cc`, including
contractions. This scoped change leaves the existing backend implementation
untouched. The definitions store canonical text; staging looks up the actual
same-language `word` rows in the verified model and emits their decimal IDs.
EOS, punctuation, missing words and synthetic re-hashed IDs are never emitted.

The four French project-authored mappings are modest, opt-in suggestions with
local evidence, not a comprehensive French shorthand corpus.

| Key → expansion | Locally retained evidence |
| --- | --- |
| `min` → `minute` | Tatoeba sentence 1892588, author `pjer`, explicitly explains this abbreviation |
| `mme` → `madame` | Pinned Grammalecte `fr.dic` entry `Mme/S.`; Tatoeba 10556204, author `Igider`, uses the title |
| `dr` → `docteur` | Pinned Grammalecte `fr.dic` entry `Dr/S.`; Tatoeba 10556204, author `Igider`, uses the title |
| `c.-à-d.` → `c'est-à-dire` | Tatoeba 12965516, author `menaud`, uses the shorthand; the expansion is a project-authored mapping |

These sentence records are in the independently pinned prepared French corpus;
their author, sentence URL and license attribution accompany the staged model.
No evidence was fetched from the network. All 36 English and four French
expansions are in-vocabulary in the reviewed model. Default `--oov skip` records
missing words and reports each skipped rule on stderr; `--oov error` refuses
the complete operation before writing.

Generated resource syntax is exactly

```text
EPOCANVA_ABBREVIATIONS_TSV\t1
language\tkey\tdecimal-model-ID[ SPACE decimal-model-ID ...]
```

There is no column-name row after the magic. The native contract is documented
in `engine/prediction/abbreviation.h`: full keys populate the prefix trie and
support partial-key matches. Staging does not generate truncated key aliases,
preselect candidates, alter corpus frequencies or replace the model-based
initialism/context decoder. The native test loads these exact resources,
checks all emitted expansions, and exercises `asa` → the `asap` mapping,
`mm` → the `mme` mapping and the whole-scalar `c.-à` prefix.

## Output safety and repeatability

Admission rejects URLs, UNC/device paths, mapped/substituted Windows drives,
`..`, ambiguous Windows names, ADS, symlinks, junctions and cloud-recall files.
Resident OneDrive cloud-tagged local files are admitted. Resource-source preset
directories cannot be selected as product roots. Every output stays under the
exact admitted root; there are no source-data, third-party, profile or system
writes.

The update policy is deliberately conservative: identical existing bytes are
accepted; every changed existing target is refused, including a file listed
in this tool's earlier manifest. Choose a new exact product root for changed
resources or explicitly reconcile an owned tree outside this tool. There is no
silent adoption or overwrite of changed foreign files.

All target conflicts are checked before any output write. Payloads are copied
to a temporary directory inside the selected root and their expected digests
checked again. Files are published with non-replacing hard links to those
private copies, never links to source resources. The staging manifest is
published last. A competing leaf creation cannot be overwritten; ordinary
publication errors remove only files created by that run. The target
filesystem must support same-volume hard links (verified here on local NTFS).
This is not a hostile-process filesystem sandbox or a whole-directory atomic
transaction. Protect the build tree from malicious ancestor replacement and
serialize product assembly.

The inventory has no timestamps or output-root identity, so identical input
bytes produce the same staging bytes at different roots. A changed current API
header or staging tool changes the inventory even when model bytes remain the
same; refusing a changed prior manifest is intentional.

## Integration handoff

The product/config owners should call the CLI before packaging/deploying the
exact product root, or call `stage_resources(...)` from their build controller.
Read `data/backend/resources.tsv`, resolve paths against that root, and pass
the explicit hashes into the profile/resource API. Enable the decoder's
abbreviation feature in the existing product settings when appropriate; this
tool does not edit those settings.

Chinese conversion uses the staged Rime shared data in `root/data`. The product
owner must merge/select both schema IDs in its own English-preserving default
configuration and run its existing local Rime deployment. No precompiled
prisms, user dictionaries or unrelated `default.yaml` are copied. French needs
the verified Hunspell-backed build enabled; native Kana/Hangul profiles must
use the existing UTF-8 backend input API. Product executable/DLL provenance,
runtime feature gating, privacy admission and real-host TSF tests remain the
respective owners' integration tasks.

The Windows TSF offline guarantee is architectural: an ordinary DLL loaded by
an arbitrary host process is not OS-sandboxed away from networking. Android's
separate network restriction can be platform-enforced by omitting
`android.permission.INTERNET`; this staging tool does not verify Android
packaging.

## Tests and observed results

Environment: Windows x64, Python 3.14.3, MSVC 19.44.35209.0, Windows SDK
10.0.26100.0, installed Visual Studio CMake. `command -v cmake` exited 1 under
Git Bash; the already installed CMake/CTest paths recorded by the existing
local cache were used. No packages or system settings were installed/changed.

```sh
EPOCANVA_STAGING_REAL_TESTS=1 python -B -m unittest discover \
  -s tests/backend_resource_staging -p 'test_*.py' -v

python -B tools/backend/stage_resources.py \
  --product-root build/backend-resource-staging/verification/product --oov error

cmake -S tests/backend_resource_staging \
  -B build/backend-resource-staging/native \
  -G "Visual Studio 17 2022" -A x64 \
  -DEPOCANVA_STAGED_PRODUCT_ROOT=<absolute-checkout>/build/backend-resource-staging/verification/product
cmake --build build/backend-resource-staging/native --config Release --parallel 4
ctest --test-dir build/backend-resource-staging/native -C Release --output-on-failure

python -B tools/backend/stage_resources.py \
  --product-root build/ime-resources/product --oov error
```

Observed on 2026-11-02: Python suite **exit 0, 37 tests, 36 passes and one
skip**. The real symlink creation test was skipped because Windows returned
`WinError 1314` (required privilege unavailable); junction/reparse/cloud-recall
admission also has passing mocked metadata tests. This does not claim that a
real symlink or junction redirection test ran on this host. The complete real
resource test stages twice, compares deterministic manifests and every output
hash, then corrupts an output and requires rejection while its source remains
unchanged. Other tests cover independent expected-hash corruption, report
replacement, path traversal/device/ADS/source-tree rejection, OOV handling,
UTF-8 byte bounds, decoder trie/record bounds, 32-row/64-KiB exact boundaries,
foreign/owned target conflicts and competing leaf creation.

Native configure, Release build and CTest each **exited 0**; CTest **1/1 passed**.
This native probe uses current `Model::Load` and `Decoder::Build/Decode` against
staged production files. It is not a Windows host/product integration test.
Both actual staging commands above exited **0**. All fixtures/build outputs
are under `build/backend-resource-staging/`; the integration data is under
`build/ime-resources/product/`.

The verified integration output contains 13 profile records and 55 files.

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `data/backend/resources.tsv` | 2983 | `f6acd2eaf5c0a660404944d5ddcaaad1477f369035247b057e24c2fb543e9ffd` |
| `data/backend/staging-manifest.json` | 51219 | `de9b86a22a0a37c8789c3ce6698d594802cbf7688a4a5096c8aec505f032eabb` |
| English abbreviation resource | 1693 | `8d1295f2b337ba2c92b194a5e292bda60073abd4ae7c1d8f6327633c2d5e6b3f` |
| French abbreviation resource | 141 | `452f710342bfac8e0f7bf89fa22c3e9e859f823365f96d61c67a396044210e54` |

## Remaining distribution blockers

Staging succeeds for internal development; it does not grant public-release
approval. The generated manifest sets `public_distribution_authorized` to
false and preserves the existing source records' boundaries. Terra dictionary
attribution/share-alike compatibility, Tatoeba rights and final attribution
presentation, mixed Mozc data notices, French MPL data delivery, static
Hunspell corresponding source/relink materials, and artifact-specific source
release verification remain required. The notices directory is useful evidence
and material, not a complete legal/source-release decision.

Chinese prediction remains the reviewed character-token corpus baseline;
Japanese uses approximate dictionary-assisted segmentation; Korean has a
smaller corpus. Packaging, native memory/latency, English capability coverage
and cross-app behavior still need the owners' product acceptance checks.

## Files in this scoped change

- `tools/backend/stage_resources.py`
- `data/backend/abbreviations/en.tsv`
- `data/backend/abbreviations/fr.tsv`
- `tests/backend_resource_staging/test_stage_resources.py`
- `tests/backend_resource_staging/CMakeLists.txt`
- `tests/backend_resource_staging/main.cc`
- `docs/testing/backend-resource-staging.md`

No main backend implementation/header/CMake, product configuration/CMake,
policy manifest or third-party source was edited by this task.
