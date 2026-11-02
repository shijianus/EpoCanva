# English Core Contract and Remaining Integration

Status: standalone English core algorithms implemented and Release-tested. This is
not a claim that they have been integrated into or verified in the Rime product.
The mainline owns the Translator adapter, learning store, corpus selection and
real-engine verification.

## Core API

`engine/languages/en/language_support.h` declares the C++17 API in
`epocanva::en`:

- `LanguageSupport::Create(lexicon, frequency_data, &build_info)` copies the
  lexicon into an immutable, build-once index. It does not retain the lexicon or
  read files. Cache one instance with its lexicon; do not create it per query.
- `Suggest(raw_input, ranking_inputs, options)` returns only suggestions; it
  neither creates nor changes the user's original candidate. Its independent
  morphology, segmentation, expansion and bounded spelling features can run
  alongside `CompletePrefix` and the legacy `SuggestSpelling`. The caller keeps
  the original candidate first and deduplicates by display text when merging.
- `Candidate` provides `type`, `text`, `score`, `segments_utf8_byte` and stable
  `lexeme_ids`. Segment ranges are half-open UTF-8 code-unit (byte) offsets into
  the original ASCII query, not offsets into expanded output, UTF-16 offsets,
  grapheme indices or screen-cell columns. Prediction candidates have no input
  spans. A morphology candidate identifies its **lemma** row, whether or not the
  generated form itself has a dictionary row; segmentation identifies each
  surface word; spelling identifies the corrected dictionary row. ID 0 means no
  admitted dictionary identity and must never be learned.
- `CandidateTypeName` returns stable candidate type strings. `CandidateType`
  currently includes `kMorphology`, `kSegmentation`, `kAbbreviation`,
  `kContraction`, `kSpelling`, and `kPrediction`.
- `Predict(ranking_inputs, options)` ranks known, independently frequency-covered
  dictionary rows. It is disabled with `kFrequencyUnavailable` when frequency
  data is absent. Its lowercase output is a suggestion only: the adapter decides
  whether to display it or how to commit it.

## Stable IDs and Ranking Inputs

`StableLexemeId` is FNV-1a-64 over the bytes of
`epocanva.en.lexeme/v1/` followed by the canonical lowercase ASCII dictionary
entry bytes. Invalid canonical entries return 0. A 0 hash is reserved and mapped
to 1; a collision among lexicon rows rejects `Create`. This is a stable join key,
**not** a cryptographic or privacy hash. It is not a suitable way to persist raw
text or unreviewed identifiers.

`FrequencyData` is separate from `LexiconEntry::weight`. `weight` remains the
legacy lexicon's coarse attestation/ranking field and is never interpreted as a
corpus count. `FrequencyData{}` is an honest supported default: source
`kUnavailable`, no frequency ranking, and segmentation/prediction report
`kFrequencyUnavailable`. A caller-supplied fixture is marked `kTestFixture`, not
production. `kProductionCorpus` records a caller-declared source only; the core
does not audit the source, its representativeness or redistribution permission.
No production corpus is bundled or supplied in this phase, so production
frequency ranking/segmentation/prediction are **not met**.

For lexeme `w`, a known independent count contributes
`base = ln(1 + base_count)`. An optional stable-ID user count contributes
`learned = 0.75 * ln(1 + learned_count)` and an optional bigram
`(previous_id, w)` contributes `context = 1.25 * ln(1 + context_count)`. These
are transparent ranking features, not probabilities. Unknown IDs are ignored.
Duplicate records or more than 4096 statistics records reject the complete
learning/context snapshot. `allow_learning=false` does not dereference either
caller-owned vector and suppresses both learned and contextual counts. The core
has no storage, input history, text-learning API or persistence behavior.
The product adapter must pass `false` for password, one-time-code and every
other privacy-classified context.

## Feature Boundaries

### Morphology

Rules are project-authored in `morphology_rules.cc`, version
`epocanva.en.morphology/v1`. The small irregular noun and verb tables and
orthographic rules do not use AGID data. The accepted Alt12Dicts word spellings
were checked only as offline spelling attestations against the already-pinned
ESDB commit `7bbe293425d48a1210bf17aaf156b28ade5c2de1`, file
`alt12dicts/2of12full.txt`; no frequencies or grammatical relationships are
claimed from that source. The AGID-dependent `2of12infl.txt`, AGID files and
other separately licensed source directories were not used. This review does not
itself grant new redistribution rights to third-party data.

Supported rules include regular `-s/-es`, consonant `-y`, `-ed`, `-ing`,
limited one-vowel-group consonant doubling, final `-ic` + `k`, explicit common
irregular forms (including be/do/go/have/eat/drink/begin/sing/swim/see/take/
give/write/run/come/make/get/say/think/buy/bring/catch/teach/feel/leave/keep/
sleep/put/cut/read), a small irregular plural table, and ASCII singular/plural
possessives. The exact table and rejection behavior are executable in
`language_support_tests.cc`.

The lexicon has no POS, stress or sense information. These rules are bounded
orthographic generation, not proof that every generated string is a valid word
or a requested grammatical category. They do not infer arbitrary stress, general
`f -> ves` / `o -> oes`, an English POS, sense or phrase grammar. Some prefix
lemmas in a large dictionary can yield spurious forms. In product use, the
Translator must set its attested-morphology policy and **must suppress every
non-possessive generated form that has no exact canonical `EnglishLexicon::Find`
row**. Possessives are the explicit exception because the current lexicon grammar
cannot load their final apostrophe. Do not learn from generated surface text:
`Candidate::lexeme_ids` intentionally identifies the known lemma. Tests also
exercise missing-from-dictionary forms to verify the generator itself, not to
assert product-vocabulary membership.

When an exact query lemma has generated forms, its forms receive a documented
+1.0 ranking preference over a mere prefix-derived form. This makes common exact
forms discoverable in a bounded menu without relying on insertion order; supplied
frequency/context scores can still outrank them. On the real 42,356-row local
product lexicon, `Suggest("walk", max_candidates=64)` returned 35 candidates;
after this preference its first five are `walk's`, `walked`, `walking`, `walks`,
`walks'`, followed by lower-scored prefix-derived forms. This observation is not
a substitute for the product adapter's attestation filter.

### Segmentation

Segmentation is a top-k dynamic program over original input byte boundaries,
last lexeme ID and number of words. It searches whole input paths only, accepts
only independently frequency-covered rows, allows single-byte `a` and `i`,
limits paths to at most eight words and charges 0.75 per inserted space. Its
score is the sum of the user-facing lexical feature score minus
`ln(total_frequency + covered_rows)` per word and the space penalties. The
per-state path bound preserves distinct previous IDs for bigram ranking. Tests
compare both unconditional and contextual results against a small, independent
exhaustive path enumerator. It never splits a known dictionary word or a known
generated wordform.

### Abbreviations and contractions

`language_support.cc` contains a project-authored, explicit whole-token table
(`en-expansion-v1`), including common chat shorthand and common ASCII
contraction de-punctuation. This is not a general abbreviation recognizer,
correction pass or automatic replacement rule. Results are opt-in suggestions.
Known expansion words receive known IDs; words absent from the dictionary carry
ID 0 and cannot be learned as known words.

### Spelling

The separate extended suggestion path uses a trie-guided restricted optimal
string alignment dynamic program with at most two insertions, deletions,
substitutions or adjacent transpositions. It has a hard ceiling of 250,000 DP
cells per call (default budget 60,000), returns only known dictionary rows, and
never runs for an existing/generated wordform, delimiter, protected text,
mixed-case input, or outside 4..32 ASCII letters. An edit costs one; keyboard
costs rank candidates only after minimum edit count: ordinary edit 100, adjacent
US-QWERTY substitution 60 and adjacent transposition 80. The layout is an
explicit ranking heuristic, not hardware/layout detection. The legacy one-edit
API is unchanged and can run in parallel; the caller resolves duplicates.

All features reuse the existing token/protection and ASCII-case contracts. No
non-ASCII normalization, transliteration, run-together Unicode splitting,
password/email/code cleanup, or input truncation is added. The original bytes
remain available to the caller unchanged.

## Test Contract and Verification

`tests/english_core/language_support_tests.cc` contains independent literal
inflection examples; known-answer stable-ID constants computed with a separate
byte-wise reference; no-frequency, bad-frequency and score-source checks; exact
learned/bigram score equations; a deliberately exhaustive segmentation oracle;
a full-matrix OSA spelling oracle over both hand-authored and generated words;
real absent-wordform tests; casing/protection tests; exact work-budget assertions;
and an injected false morphology assertion which must exit 1. These oracles do
not compute expected output by invoking the production algorithm under test.

Standalone configure and all generated files live at `build/multi-english-core`.
The direct test project compiles only `engine/languages/en` and
`tests/english_core` without Rime, platform modules, installs, downloads or
recompiling dependencies. It uses VS 2022 Professional 17 2022 x64, MSVC
19.44.35209.0, CMake 3.31.6-msvc6, C++17 with extensions disabled and
`/permissive-`.

Release verification on 2026-11-02:

- `cmake --build build/multi-english-core --config Release --target epocanva_english_core_tests epocanva_english_language_support_tests --parallel 1`: exit 0.
- `ctest --test-dir build/multi-english-core -C Release --output-on-failure -j 1`: exit 0, 8/8. Existing core: 1,566 assertions, 0 failed. New core: 639 assertions, 0 failed. Both intentionally failing assertion tests returned the expected exit 1 and were accepted by the exact-exit harness; bad arguments returned expected exit 2. The existing 42,356-word one-edit performance test passed with a 1,578.9 microsecond worst call in that Release run.
- The added product-scale budget case passed: 42,356 rows, 202,155 generated form records, 449 ms immutable-index build, six fixed queries in 23 ms total, 130,974 spelling DP cells total, each configured per-query cap observed. Frequency source was unavailable, so this did not claim production-frequency readiness.
- A fixed `walk` product-lexicon probe after the exact-lemma ranking fix returned the five expected exact-lemma forms above its prefix-derived forms. It was a separate scratch executable under the build tree, not part of the 8 CTest cases.
- Debug full-suite observation: 7/8. New core/pilot tests and all functional tests passed, but the **existing** one-edit spelling pilot wall-clock limit reported 33,617 microseconds over its fixed 25,000-microsecond threshold. The threshold was not relaxed. The corresponding Release test passed; the Debug observation remains a performance limitation, not a passing result.

The Release suite was rerun after the exact-lemma ranking correction. Tests do not execute real-engine behavior: translator ordering/merging, Rime candidate labels, opt-in defaults, attested-morphology filtering, privacy-aware storage, and current production frequency data remain mainline integration responsibilities and are not verified here.
