# candidate-v1 — Tier A synthetic candidate-menu evaluation corpus

**What this is.** The committed Tier A corpus for the E6 candidate-menu
evaluation baseline (`docs/testing/candidate-evaluation-baseline.md`): 15
project-authored, hand-written cases across the five implemented languages,
replayed through the real backend by `tests/candidate_evaluation/candidate_eval_cli.cc`
and measured by `tools/backend/evaluate_candidates.py`.

**Provenance.** Every case is authored by this project for evaluation only.
No case comes from user input, a composition log, a keystroke capture, a
clipboard, or any real user content. The licence is `AGPL-3.0-only`
(project-owned synthetic data). The `source` field is the corpus identity
`epocanva-synthetic-v1`, not a URL.

**Where every expectation comes from.** No `expected` surface was written from
opinion or fitted from an unverified run. Each case's expected surface is the
target of a pinned assertion that already exists in the verified backend suite
`tests/backend/profiles.cc`, executed against the real staged resources:

| Case id | Profile | Expectation pinned by |
| --- | --- | --- |
| `en-prefix-hel-0001` | 1 (en-US QWERTY) | `kConversions` row 1 (`hel`→`hello`) and the `Bindings` admitted-id assertion |
| `en-prefix-wor-0002` | 12 (en-US Dvorak) | `Bindings`: `Find(second, "world", true)` with one admitted lexeme id |
| `zh-pinyin-nihao-0001` | 2 (zh pinyin) | `kConversions` row 4 (`nihao`→`你好`), Rime-owned row |
| `zh-pinyin-nh-0002` | 2 | `PinyinInitialisms`: CONVERSION row with two admitted character ids |
| `zh-pinyin-nh-context-0003` | 2 | `PinyinInitialisms`: same `nh` under imported context 我们; expectation presence, not rank |
| `zh-zhuyin-su3cl3-0004` | 6 (zh zhuyin) | `kConversions` row 5; scheme stays `unverified` in the locale registry and is reported separately by the evaluator |
| `fr-prefix-francais-0001` | 3 (fr-FR AZERTY) | `kConversions` row 2 (`francais`→`français`) |
| `fr-prefix-ecol-0002` | 3 | `kConversions` row 3 (`écol`→`école`) |
| `fr-prefix-cafe-0003` | 3 | `Edits`: `Find(accented, "café", true)` — real admitted candidate for Unicode input |
| `ja-romaji-nihonn-kanji-0001` | 4 (ja romaji→kanji) | `kConversions` row 6 (`nihonn`→`日本`) |
| `ja-romaji-nihonn-hiragana-0002` | 7 | `kConversions` row 7 |
| `ja-romaji-nihonn-katakana-0003` | 8 | `kConversions` row 8 |
| `ja-kana-nihonn-kanji-0004` | 9 (ja kana→kanji) | `kConversions` row 11 (`にほん`→`日本`) |
| `ko-two-set-dkssud-0001` | 5 (ko two-set) | `kConversions` row 13 (`dkssud`→`안녕`) |
| `ko-hangul-annyeong-0002` | 10 (ko native Hangul) | `kConversions` row 14 (`안녕`→`안녕`); binding 10 declares scheme `two_set` with ABI input mode LATIN |

`profile_id` values are the committed binding ids from
`data/locale/backend-bindings.json` (1–13); the evaluator refuses a case whose
binding does not own its declared language and scheme.

**Deliberate exclusions.** English abbreviation-expansion and next-word cases
are not in this corpus: their expected leaders are computed by the model at
test time (the abbreviation oracle in `profiles.cc`), not authored pins, and a
baseline case whose expectation is fitted from observed output would not be an
independent signal. They can be added when an authored pin exists.

**What this corpus is not.** It is far below the E4 entry gate minimums
(1,001 cases per language; 500 for Korean — see the design document §9). It
proves the measurement plumbing, the raw-first contract checks, determinism
and the recorded baseline numbers; it is **not** a natural-language quality
claim, and `evaluate_candidates.py` withholds the quality claim accordingly
(`quality_gate.met == false`, `claim: baseline-only`).

**Reproduction.** From the repository root, with the pinned Tier 2 inputs and
staged backend resources in place (see
`docs/testing/backend-resource-staging.md`):

```sh
python -B tools/backend/evaluate_candidates.py \
  --cases data/eval/candidate-v1/cases \
  --manifest data/eval/candidate-v1/manifest.json \
  --resources <staged-backend-resource-root> \
  --harness <build>/candidate-evaluation-tests/Release/epocanva_candidate_eval_cli.exe \
  --output build/candidate-eval/report.json \
  --mode baseline \
  --expected-repo-commit <optional 40-hex assertion against the real HEAD> \
  --rime-dll build/librime-english/rime/install/lib/rime.dll \
  --rime-dll-sha256 <pinned DLL SHA-256> \
  --rime-shared build/backend-chinese-ranked/shared
```

The evaluator derives `repo_commit` itself from `git rev-parse --verify HEAD`;
`--expected-repo-commit` is an assertion that fails the run on mismatch, never
a source of identity.

The results JSONL files under `build/candidate-eval/runs/` are the only files
that carry candidate surfaces; the report carries case ids, status names,
counts and metrics only.
