# Prediction-core contract and verification

## Scope

`engine/prediction/model.h` is the shared C++17 lexical-model contract. It does not
replace the English language algorithms, existing engine ABI, Rime, learning
repository or platform service. `epocanva_prediction_core` is an independent target
and inherits its parent's CRT. No production model is bundled. Test data is
explicitly marked `test-fixture`; its acceptance is opt-in when loading a file.

`Model::FromRecords` and `Model::Load` return `shared_ptr<const Model>` or fail
atomically with `BuildInfo`. `Model::Unavailable()` provides an explicit unavailable
object; it does not invent frequencies. The catalog is shared through `lexemes()`
(stable-ID ascending), `Find(id)` and `Find(language_id, normalized_utf8)`.
`UnigramCount(id)` exposes exact observed corpus counts (zero if absent), never
an inverse of a smoothed score or a personal aggregate.

## Stable lexical IDs and UTF-8 units

Version 1 is FNV-1a-64, offset basis `14695981039346656037`, multiplication prime
`1099511628211`, modulo 2^64, over these concatenated bytes:

```
epocanva.prediction.lexeme/v1/<language_id>/<normalized_utf8>
```

A zero hash maps to 1. ID 0 is EOS, never a lexical row. Language IDs have 2..8
lowercase ASCII letters followed by zero or more `-` separated segments of 1..8
lowercase ASCII letters/digits, at most 32 ASCII bytes in total. IDs do not depend
on dictionary order, corpus counts, platform or process. Collisions/duplicates
are rejected, not merged. These are not the legacy English algorithm's IDs:
integration must map admitted dictionary rows by canonical text, never cast the
old IDs. Neither ID scheme anonymizes personal information.

The sole accepted normalizer identifier is `ascii-lower-utf8-v1`. It folds ASCII
A..Z only and preserves every other valid scalar's bytes. It is deliberately NOT
NFC, Unicode casefold, locale-specific lowercase or Windows invariant lowercase.
Training and native lookup must use this exact rule. A provider may create
separate reviewed lookup aliases that resolve to existing catalog IDs; it must
not rewrite catalog text or silently choose a different normalizer. Unknown
normalizer identifiers require a future coordinated format version.

Lexical rows contain 1..256 UTF-8 code units (bytes), not words with embedded
spaces. UTF-8 must encode Unicode scalar values: reject overlong/truncated
encodings, isolated continuation bytes, surrogates and values above U+10FFFF.
Reject U+0000..U+001F, U+007F..U+009F and Unicode White_Space (U+0020, U+00A0,
U+1680, U+2000..U+200A, U+2028, U+2029, U+202F, U+205F, U+3000; other whitespace
is already within the control ranges). Apostrophes, hyphens and non-ASCII letters
are allowed. A `punct` row is an explicitly declared terminal punctuation token;
no language-dependent tokenizer or punctuation classifier is hidden in the core.

All text offsets and text counts in this API are UTF-8 bytes. Context/record/path
ordinals count IDs or records, not UTF-16 units, graphemes or caret/screen cells.
The integration layer owns UTF-16 conversion and caret arithmetic.

## Deterministic TSV model file, version 1

UTF-8, LF only, final LF required, no BOM, CR, comments, escaping, empty lines or
optional/reordered headers. Every decimal integer is canonical unsigned decimal:
no signs, whitespace or leading zeroes except the integer `0`. SHA-256 values
are exactly 64 lowercase ASCII hexadecimal bytes.

Exactly these eleven header lines precede the payload:

```
EPOCANVA_PREDICTION_TSV\t1
id_scheme\tfnv1a64-language-normalized-v1
normalization\tascii-lower-utf8-v1
source\ttest-fixture
source_sha256\t<source fingerprint>
payload_sha256\t<payload SHA-256>
lexemes\t<number of lexical rows>
unigram_total\t<sum of all U counts>
bigram_total\t<sum of all B counts>
trigram_total\t<sum of all T counts>
data
```

Here `\t` means one literal TAB. `source` is exactly `test-fixture` or
`production-corpus`. The source fingerprint is the SHA-256 of the training
source/provenance manifest, supplied by the training/packaging owner. The loader
validates its syntax and, when `LoadOptions.expected_source_fingerprint` is
nonempty, requires an exact match. A self-declared source fingerprint alone is
not source verification, a licence grant or authentication. Packaging must supply
trusted expected identity. The payload SHA-256 is recomputed over the exact bytes
AFTER `data\n`, through the final payload LF. It detects payload mismatch; an
editable checksum is not a signature. `BuildInfo` distinguishes these two checks.

Payload records are grouped L, U, B, T, in that order. Within each group numeric
ID tuples are strictly ascending (NOT lexicographic decimal-string order):

```
L\t<id>\t<language_id>\tword|punct\t<normalized_utf8>
U\t<id>\t<count>
B\t<previous_id>\t<id>\t<count>
T\t<previous2_id>\t<previous1_id>\t<id>\t<count>
```

`previous2_id` is older than `previous1_id`. U may target EOS 0; B and T may end
in EOS 0, but their history IDs must be nonzero admitted lexical IDs. EOS has no
L row. L rows can lack U observations: they receive smoothed unseen probability.
All count rows are positive uint64 values. There must be at least one lexical
row and at least one unigram occurrence. B and T groups may be absent.

Validation is atomic and checks:

- header/model/hash/normalizer version and exact columns/order;
- canonical IDs recalculated from language plus normalized text;
- invalid UTF-8, duplicate IDs/tuples and unknown references;
- checked uint64 additions (no saturation/wraparound), declared row and count totals;
- for each B history, outgoing sum <= its U count; for each B target, incoming
  sum <= its U count;
- for each T history pair, outgoing sum <= its B count; for each T suffix pair,
  incoming sum <= its B count;
- source match when required, exact payload SHA-256, file byte limit and explicit
  test-fixture opt-in.

A trainer counting U, adjacent B and adjacent T per sentence, optionally appending
EOS, satisfies these invariants. Do not prepend a reserved BOS or let EOS become
a history ID. Missing/invalid files do not become empty production models.
Binary or unknown version headers are rejected; v1 has no binary format.

`FromRecords` accepts unsorted record vectors and applies the same semantic
checks, but it has no file payload to hash. It does not manufacture provenance.

## Fixed score and lookup contract

Context has at most two stable IDs, oldest first. Unknown IDs and EOS reset the
usable history, retaining a subsequent known suffix. Static context is independent
of learning permission. If `personal == nullptr`, static bigrams and trigrams
still contribute.

Let V be lexical row count plus one EOS outcome. For an outcome w, history a and
older history b, the fixed v1 conditional probabilities are:

```
P1(w)     = (U(w) + 1) / (unigram_total + V)
P2(w|a)   = (B(a,w) + 8 * P1(w)) / (sum_x B(a,x) + 8)
P3(w|b,a) = (T(b,a,w) + 4 * P2(w|a)) / (sum_x T(b,a,x) + 4)
```

Use only the orders for which history exists. Absent successor rows have zero
counts and exactly back off; unseen catalog outcomes retain nonzero probability.
This is additive unigram smoothing plus fixed-prior interpolated backoff, not
Kneser-Ney or a trained neural model. `Score` returns the static probability, the
final probability and its natural logarithm. Unknown target IDs are errors;
EOS is scoreable even when its corpus count is zero.

Optional personal counts form distinct ephemeral channels. Begin Q with the full
static conditional P. Then, in order, apply personal U, the available personal B
history, and the available personal T history:

```
Q = (personal_channel_count(w) + 16 * Q) / (personal_channel_total + 16)
```

An absent channel is the identity. Each channel is an ID/count snapshot, not raw
text; channels may be sparse and need not have the static corpus's cross-order
marginal consistency. Duplicates, unknown IDs, EOS histories, zero counts,
overflow or more than 4096 combined records reject the entire query. Personal
inputs are prepared/indexed once per API call, not once per candidate or beam
node. Nothing is cached in the shared model. Caller-owned snapshots must not
mutate during the call.

## Ranking, prefix, budgets and continuation

Static global unigram ranks and per-history B/T successor ranks are constructed
once. Personal rank lists are query-local. Search merges count-ranked lists with
nonnegative scoring coefficients; it never scans the first 4096 alphabetical
rows as an approximation to best candidates. A conservative remaining-score
bound permits early exact termination. Equal scores use ascending stable ID,
not insertion order, locale collation or unordered-map iteration. Prefix queries
validate and normalize once, then match whole valid UTF-8 byte prefixes. Language
filtering is exact; the score remains the same global conditional probability.

`max_index_entries == 0` means exhaustive/exact next-token search. A nonzero
budget counts ranked entries visited across the entire query, including duplicate
or filtered IDs. If completeness cannot be established before the budget runs
out, `budget_exhausted=true`, `complete=false`, status `kBudgetExhausted`. There
is no silent truncation. `max_suggestions` is 0..64; `max_words` is 1..4 emitted
lexical tokens, including punctuation; beam width is 1..64. Out-of-range options
are errors, not silent clamps.

Continuation is bounded beam search with cumulative conditional log probability.
EOS is not an initial empty suggestion and is never emitted in `tokens_utf8`.
It terminates an existing path and contributes its score. Explicit punctuation
also terminates; it is emitted and consumes one token slot. Other endpoints reach
the requested token bound. Beam pruning is explicit (`beam_pruned=true`,
`complete=false`, status `kBeamPruned` unless the budget was also exhausted).
Results do not claim a globally optimal four-token sequence after pruning. A
budget-exhausted continuation may return a shorter partial path; it is marked
incomplete and is not claimed to have reached EOS or the requested token bound.
Equal path scores use lexicographically ascending stable-ID sequences. Tokens
are returned separately: the host owns language-specific joining, spacing,
case restoration, original-candidate preservation, presentation and commits.

## Privacy and integration responsibilities

The immutable model retains only its admitted static catalog, provenance and
aggregate static corpus counts. `Predict`, `Score` and catalog lookup perform no
file access, network I/O, logging, learning or persistence. `Load` reads one local
model at initialization; there is no per-key file query. The core does not accept
raw commit history, clipboard, app identity, field names, timestamps or a mutable
learning store. Personal IDs/counts remain personal data.

The service must gate access to personal snapshots against current consent,
privacy classification, focus and generation, and exclude password/OTP/other
sensitive fields. It must prevent stale asynchronous output and release its own
query buffers. Passing nullptr disables personal scoring without suppressing
static context. The core cannot enforce host admission or zeroize strings owned
by the caller. It supplies suggestions, never changes host text automatically.

On Windows a normal TSF DLL loaded into arbitrary host processes is not
OS-sandboxed away from the network: the offline property here is architectural
(no socket/network calls), to be verified with the full dependency/build policy.
On Android omitting `android.permission.INTERNET` supplies the platform-enforced
network restriction; this platform-neutral core does not itself set that manifest.

## Verification, 2026-11-02

### Environment and scope

Baseline HEAD was `4d9bcd3`. Environment: Windows x64, Visual Studio 2022,
MSVC 19.44.35209.0, SDK 10.0.26100.0, bundled CMake/CTest 3.31.6-msvc6,
Python 3.14.3. No tools/packages were installed or downloaded. Git Bash did not
have CMake, Ninja, GCC, Clang or cl on PATH (`command -v cmake`, `command -v ninja`,
`command -v clang++`, `command -v g++`, `command -v cl` produced no path); the
existing bundled CMake was found from existing build cache records and invoked
by its absolute machine-local executable path. Python was already available.

Source files added in this task, and no others:

- `engine/prediction/model.h`
- `engine/prediction/model.cc`
- `engine/prediction/CMakeLists.txt`
- `tests/prediction_core/main.cc`
- `tests/prediction_core/make_fixtures.py`
- `tests/prediction_core/CMakeLists.txt`
- `docs/testing/prediction-core.md`

Existing English, engine ABI, Rime, root CMake, unrelated dirty files and other
agents' new backend paths were not edited. No git commit, push, staging, reset or
other worktree manipulation was performed. The source-tree licensing instructions
and supplied task context differed on the selected project licence; this task
adds/changes no licence text and depends on neither interpretation.

All generated libraries, test executables, generated TSVs and the temporary
embedding probe live under `build/backend-prediction-core`. The only build-only
probe source was `build/backend-prediction-core/embedding/CMakeLists.txt`; it calls
`add_subdirectory` for both owned source directories, requests the static MSVC CRT,
and asserts that the library inherits it and disables C++ extensions. The runtime
library properties in generated projects were actually checked: standalone uses
MultiThreadedDLL/MultiThreadedDebugDLL; embedding uses
MultiThreaded/MultiThreadedDebug. No machine-specific compiler path is committed.

### Commands and observed outcomes

The executed absolute paths below use these abbreviations solely to keep machine
usernames out of this document (the recorded commands used their expanded values):

- `$ROOT`: the checked-out EpoCanva workspace absolute path;
- `$CMAKE`: the installed Visual Studio 2022 Professional bundled
  `Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`;
- `$CTEST`: `ctest.exe` in the same directory.

Commands run in order:

```sh
"$CMAKE" --version
"$CMAKE" -S "$ROOT/engine/prediction" -B "$ROOT/build/backend-prediction-core/library" -G "Visual Studio 17 2022" -A x64
"$CMAKE" --build "$ROOT/build/backend-prediction-core/library" --config Release
"$CMAKE" -S "$ROOT/tests/prediction_core" -B "$ROOT/build/backend-prediction-core" -G "Visual Studio 17 2022" -A x64
"$CMAKE" --build "$ROOT/build/backend-prediction-core" --config Release
"$CMAKE" --build "$ROOT/build/backend-prediction-core" --config Release
"$CTEST" --test-dir "$ROOT/build/backend-prediction-core" -C Release --output-on-failure
"$CMAKE" --build "$ROOT/build/backend-prediction-core" --config Debug
"$CTEST" --test-dir "$ROOT/build/backend-prediction-core" -C Debug --output-on-failure
"$ROOT/build/backend-prediction-core/Release/epocanva_prediction_core_tests.exe" --unit "$ROOT/build/backend-prediction-core/fixtures"
"$CMAKE" -S "$ROOT/build/backend-prediction-core/embedding" -B "$ROOT/build/backend-prediction-core/embedding-out" -G "Visual Studio 17 2022" -A x64
"$CMAKE" --build "$ROOT/build/backend-prediction-core/embedding-out" --config Release
"$CTEST" --test-dir "$ROOT/build/backend-prediction-core/embedding-out" -C Release --output-on-failure
git -C "$ROOT" diff --check -- engine/prediction tests/prediction_core docs/testing/prediction-core.md
```

The first test-target Release build exited **1**: the test oracle's initializer
list mixed const/nonconst snapshot pointers and MSVC could not deduce its type.
Explicit matching const pointer types fixed the harness. No tests ran in that
failed chain. The next Release build exited **0**. All other commands above
exited **0**. Model-library standalone compilation succeeded on its first attempt.
No compiler warning was printed in the successful model/test builds.

Observed CTest outcomes:

| Configuration | Actual result | Observed wall time |
|---|---|---|
| standalone Release | 3/3 passed | 0.63 s |
| standalone Debug | 3/3 passed | 1.57 s |
| embedded Release, static CRT | 3/3 passed | 0.58 s |

A final repeat after generalizing the fixture output guard to support any valid
repository `build/<preset>` embedding also ran the following commands, all exit 0:

```sh
"$CMAKE" --build "$ROOT/build/backend-prediction-core" --config Release
"$CTEST" --test-dir "$ROOT/build/backend-prediction-core" -C Release -V
"$CTEST" --test-dir "$ROOT/build/backend-prediction-core" -C Debug --output-on-failure
```

The repeat passed 3/3 in Release (0.52 s) and Debug (1.48 s). Verbose Release
output explicitly observed unit exit 0, intentional false assertion exit 1 and
unknown arguments exit 2 with their correct marker policies.

The direct Release executable reported `RESULT PASS prediction core checks=7337`.
The CTest wrapper is the existing `tests/infrastructure/assert_probe.cmake`, read
before reuse. It requires the exact numeric exit and an explicit marker policy:
unit=0 with PASS; intentionally false assertion=1 without PASS; unknown
arguments=2 without PASS. `WILL_FAIL` and unconditional-success scripts are not
used. The suite's assertion-failure and argument-failure cases actually ran in
all three configurations.

The Python fixture generator runs with `-B` (no source-tree bytecode output) and
is restricted to this repository's `build/<preset>` tree. It generated 25 TSV
files: one valid isolated test model and 24 malformed variants. No generated data
is labelled production. An additional explicit owned-file trailing-whitespace
check found zero issues across all seven files; git's tracked diff check also
passed (new untracked source files are covered by the explicit check).

### Evidence covered

- Independent direct-record probability equations and exhaustive ranking oracle,
  not production `Score` as its own expected answer; 12 seeded generated corpora
  exercise prefix, language, context, personal and top-k combinations.
- Contextual static ranking with personal learning disabled; unknown/sparse
  histories; suffix fallback after EOS; add-one probability for unseen lexemes;
  sums of conditional outcome probabilities; deterministic reordered inputs.
- 6,002 lexical rows with global, context and personal winners beyond alphabetical
  entry 4096; global/successor ranked indexes find those winners before visiting
  4096 entries. Hard work budgets report incomplete; a one-entry proven winner
  can still report exact completion. Returning 64 results is verified.
- English, Chinese, French, Japanese and Korean tokens; independent Python FNV
  identity answers; invalid Unicode scalars, overlong/truncated encodings,
  whitespace/control restrictions, exact ASCII-only normalization, non-NFC
  identity and UTF-8 byte prefix matching.
- Independent exhaustive enumeration of all unpruned three-token continuations;
  emitted punctuation and nonemitted EOS termination; <=4 token paths, explicit
  beam pruning, global query budget, one personal preparation for the entire beam.
- Model/header/ID/normalizer versions; duplicate/unknown IDs; positive counts;
  uint64 overflow; declared and cross-order totals; SHA-256 payload checking
  against Python hashlib; expected source identity; missing/binary/invalid files;
  canonical text/integer/order/column/newline syntax; test-fixture opt-in.
- Deleting a copied fixture after loading still permits correct queries: model
  query paths do not reopen the model file.
- Eight threads issue 1,600 alternating personalized/static queries against one
  immutable shared model without cross-query personal leakage. This is a
  concurrency functional test, not ThreadSanitizer evidence.

### Limits and integration blockers

The normalizer is intentionally ASCII-only, not general Unicode normalization.
The provider owns alias normalization, contextual ID resolution, display casing,
joining and admission; legacy English IDs require deliberate text mapping.
A sparse prefix may require scanning all ranked outcomes when no work budget is
set; there is no promise of fixed query latency. Every bounded search explicitly
reports incomplete. Finite continuation beams are approximate and reported as
such. TSV loading builds in-memory count maps and indexes once; large production
corpora need separately measured memory/latency budgets.

No production corpus, model-quality benchmark, signed package/source verification,
full host privacy integration, TSF integration, Android build or non-MSVC compiler
run was performed in this task. The offline/thread-safe claims are limited to
this core and the tests above. Source fingerprints attest caller-supplied identity;
they do not confer corpus licensing or authenticate the model. A trusted trainer,
packager and host privacy/consent integration remain prerequisites for production
use. The standalone CMake entry points require no downloads and are ready for the
coordinator's separate root/service integration.
