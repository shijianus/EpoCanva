# French composition support core

This is an independently tested C++17 support library, **not a delivered French
input method**. `epocanva::fr::kProductReady` is always false. There is no bundled
French dictionary, Rime schema/component, TSF integration, shared ABI change,
learning store, or platform dependency. No third-party code/data was added or
fetched. Product activation still requires independently verified data, Rime,
TSF, privacy admission and cross-application behavior.

The task brief takes precedence over the older multilingual plan's integration
order: **English > Chinese > French > Japanese > Korean**. English implementation,
learning and integration remain with their owners. The on-disk AGENTS licensing
section now records the owner's AGPL choice, while the supplied context retained
older wording; this task introduces no licensing decision or licence header.

## Owned files and integration boundary

- `engine/languages/fr/french_core.h`: public C++ API and bounded contracts.
- `engine/languages/fr/french_core.cc`: UTF-8 validation and French algorithms.
- `tests/french_core/CMakeLists.txt`: standalone and `add_subdirectory` test entry.
- `tests/french_core/main.cc`: positive, negative, property and deliberate failure tests.
- `tests/french_core/assert_exit.cmake`: exact-exit/marker wrapper.
- `docs/testing/french-core.md`: this scope and verification record.

The French agent made no root/shared/product CMake, Rime, platform, third-party,
or C ABI edits. The coordinating agent can include the test directory in an
aggregate build. `project()` and `enable_testing()` run only for standalone use;
parent projects enable testing themselves. Targets `epocanva_fr_core` and
`epocanva_french_core_tests` have French-specific names and C++17 without extensions.
MSVC uses `/permissive- /W4 /WX /utf-8`; other compilers use strict warnings. CRT
selection is inherited from the parent, with no independent `/MT` or `/MD` policy.

## API and units

All text is UTF-8. Every caret and half-open span uses **UTF-8 code units (bytes)**
in the original text unless explicitly described as normalized output. No UTF-16
code-unit or screen-cell arithmetic is exposed. No C/platform ABI is defined here.

| API | Contract |
| --- | --- |
| `ValidateUtf8(text)` | Strict scalar decoding; reports the lead UTF-8 byte of an ill-formed sequence. |
| `ComposeLetter(accent, scalar)` / `SpacingAccent(accent)` | Optional scalar result from the declared character table; unsupported values return no result. |
| `NormalizeFrench(text)` | Only adjacent supported ASCII base + combining-mark composition. |
| `ToLowerFrench(text)` / `ToUpperFrench(text)` | Limited French mapping after the above normalization. |
| `SegmentCombiningClusters(text)` | Half-open original UTF-8 byte spans using the limited cluster definition below. |
| `ClassifyToken(text)` | Whole-token French word, URL, email, number, whitespace, or other. |
| `SegmentTokens(text)` | Original UTF-8 byte spans covering the entire input, including punctuation and whitespace. |
| `Composition` | Owned transient text, UTF-8 byte caret, pending accent; explicit insertion, caret, Backspace and Cancel operations. |
| `SuggestAccents(query, dictionary, max_suggestions)` | Suggestions from caller data only, with explicit source status and deterministic ranking. |
| `InflectPlural(lemma, rule)` | Caller-selected orthographic plural rule, without lexical/gender guessing. |

Text operations accept at most 4,096 UTF-8 bytes. Dictionary words, suggestion
queries and inflection lemmas accept at most 256 input UTF-8 bytes. Oversized input
is rejected, never truncated. Dictionary limits are 4,096 records and 1,048,576
aggregate word UTF-8 bytes. `max_suggestions` is a candidate count from 0 to 64.

The decoder rejects lone continuations, C0/C1 overlong leads, bad continuation
bytes, truncation, overlong encodings, surrogates, F5..FF leads and values above
U+10FFFF. NUL and Unicode noncharacters remain valid scalars, but word/protection
grammar does not treat controls as letters or permit controls inside URLs/emails.
Error results contain no partially transformed text or partial spans.

## Composition, normalization and case scope

The linguistic composition table is finite and explicit. It is a character rule
table, not a dictionary or predetermined candidate menu.

| Accent | Combining scalar | Lowercase composition | Uppercase counterpart |
| --- | --- | --- | --- |
| Acute | U+0301 | e → é | E → É |
| Grave | U+0300 | a/e/u → à/è/ù | A/E/U → À/È/Ù |
| Circumflex | U+0302 | a/e/i/o/u → â/ê/î/ô/û | A/E/I/O/U → Â/Ê/Î/Ô/Û |
| Diaeresis | U+0308 | a/e/i/o/u/y → ä/ë/ï/ö/ü/ÿ | A/E/I/O/U/Y → Ä/Ë/Ï/Ö/Ü/Ÿ |
| Cedilla | U+0327 | c → ç | C → Ç |

Diaeresis a/o supports loanword spelling. Case conversion also covers ASCII and
œ/Œ and æ/Æ. Other scalars are preserved, without full Unicode case folding or
multi-scalar case expansion. An acute on `a` is deliberately unsupported.

Normalization consumes only the listed adjacent pairs, preserving other bytes.
For example, `e + U+0301` becomes `é`; `e + U+0301 + U+0308` becomes
`é + U+0308`; `e + U+0327 + U+0301` remains unchanged. There is no canonical mark
reordering, general decomposition, compatibility normalization, or claim of full
Unicode NFC. Calling the supported normalizer twice is idempotent.

A **limited combining cluster** is one scalar followed by consecutive marks in
U+0300..U+036F. A leading run of those marks is one cluster. Other combining
ranges, variation selectors, emoji modifiers, ZWJ sequences and regional-indicator
pairs receive no extended-grapheme treatment. This is not Unicode UAX #29.

`Composition::SetText` preserves exact validated input bytes and accepts only a
limited-cluster boundary, including UTF-8 byte 0 and the end of the text. Invalid
input, invalid caret and size errors leave the old state unchanged.

- Dead accent + supported letter inserts its composed scalar.
- Dead accent + space inserts the spacing accent (´, `, ^, ¨, or ¸), consuming
  that space; remaining insertion text survives.
- Dead accent + unsupported letter inserts the spacing accent plus original text.
- Repeating the same dead key emits its spacing accent and disarms it; selecting
  a different dead key emits the previous spacing accent and arms the new one.
- Empty insertion preserves the pending accent. Successful nonempty insertion
  normalizes the whole composition buffer under the declared scope and places
  the UTF-8 byte caret after the inserted cluster. If inserting before a leading
  combining-mark run, the caret follows that run after composition.
- Backspace first disarms a pending accent. Otherwise it removes the entire
  preceding limited cluster. Precomposed `é` and decomposed `e + U+0301` each
  delete as one cluster; neither exposes an intermediate unaccented `e`.
- Cancel clears text, pending accent and UTF-8 byte caret. It is state cancellation,
  not a promise of cryptographic memory erasure or a host commit operation.

Insertion checks the intermediate UTF-8 byte size before normalization; an input
that would exceed 4,096 bytes before shrinking is rejected atomically. `SetCaret`
retains a pending accent, so the next insertion resolves at the selected boundary.
The mutable `Composition` object requires caller synchronization; stateless APIs
can be called independently while their caller data remains unchanged.

## Word grammar and protected text

French word grammar uses the supported letters above, plus ASCII letters. ASCII
apostrophe and U+2019, hyphen-minus and U+2010/U+2011 are allowed only between
letter runs. Examples accepted by the grammar include `l'été`, `aujourd’hui`,
`arc-en-ciel`, and `ŒUVRE`. Leading/trailing joiners, doubled joiners, digits,
unsupported combining sequences, and arbitrary punctuation are not French words.
This is spelling-shape recognition, not evidence that a token exists in a dictionary.

Protection is intentionally conservative and is not an RFC parser:

- URLs: ASCII scheme starting with a letter, then letters/digits/`+.-`, then `://`;
  also case-insensitive `www.` and `mailto:` prefixes. Incomplete prefixed URLs
  are protected too. Whitespace/control scalars disqualify this classification.
- Emails: exactly one `@` with nonempty text on either side and no whitespace or
  controls. An in-progress domain such as `ete@x` is protected. Deliverability is
  not checked, and no online lookup is made.
- Numbers: optional `+`, `-`, or U+2212; ASCII digits; dot/comma/U+00A0/U+202F
  separators only between digits; optional `e/E` exponent with ASCII sign and
  digits; optional final `%`. Group widths and decimal meanings are not inferred.
  `.5`, `1e`, `1,,2`, and trailing separators fall outside the declared grammar.

`SegmentTokens` splits on ASCII whitespace and U+00A0/U+202F. A protected chunk
stays intact (including possible trailing sentence punctuation for URL/email
safety). Other chunks have ASCII `.,;:!?()[]{}"` peeled from their edges and the
entire remaining chunk is classified. Internal malformed joiners or unsupported
letters do not become smaller accepted word fragments. NBSP-grouped numbers are
recognized whole by `ClassifyToken`; segmentation separates their numeric pieces
and whitespace. Every emitted span still refers to original UTF-8 bytes, including
decomposed input. This is deliberately a bounded tokenizer, not general linguistic
word-breaking or run-together word segmentation.

## Dictionary-dependent suggestions

`DictionaryInput` carries a source status, name, version, licence declaration and
caller word/rank records. There are only three data states:

- `kUnavailable`: default empty source and records; returns `kDictionaryUnavailable`.
- `kTestFixture`: explicitly labeled original unit-test material.
- `kCallerSuppliedUnverified`: data supplied by the caller, with no audit implied.

All supplied records and metadata are validated on every call before results are
returned. Invalid UTF-8, illegal word grammar, missing source fields, excessive
bounds or an inconsistent source state reject the dataset atomically. Metadata
fields are at most 256 UTF-8 bytes, nonblank, without ASCII controls. A zero result
limit still validates the whole dataset. Queries are gated before dictionary work.
There is no file loading, download, embedded lexicon, cache, or persistence API.

A candidate must match the query scalar-for-scalar, ignoring supported case, except
that an unaccented ASCII query letter may gain a listed accent. Existing accents
are preserved, joiners remain exact, and `oe` never silently becomes `œ`. The
normalization-equivalent original is excluded. Lowercase, initial-capital and
all-uppercase query styles are preserved; mixed case is explicitly refused.
Dictionary case is canonicalized to the query style, so this API is for admitted
ordinary word forms, not a proper-name/brand-case preservation engine.

Candidates are deduplicated after output case conversion, retaining the largest
caller rank. Ordering is descending rank, then fewer changed limited grapheme
clusters, then UTF-8 byte lexical ordering. Rank is only a caller hint, not a
frequency claim. Suggestions are never automatically committed or substituted.
Original-text candidate management, privacy admission and any learning belong to
the integration caller. This core performs no learning even when data is supplied.

Test words exist only inside `tests/french_core/main.cc`, labeled as test fixtures;
none enter the core library or establish production coverage/licensing. Accents
provide a limited spelling suggestion capability; edit-distance correction,
contextual choice, accent removal/replacement and lexical disambiguation are not
implemented.

## Declared plural scope

`InflectPlural` accepts one unjoined French lemma and an explicit `PluralRule`.
It preserves lower/title/upper case, normalizes supported decomposed pairs, and
rejects mixed case and compound inflection. It generates an orthographic form,
not an assertion that the input is a real noun or that the chosen lexical class
is correct.

| Rule | Behavior | Tested example |
| --- | --- | --- |
| `kRegularS` | Append s; final s/x/z stays invariant | école → écoles; prix → prix; nez → nez |
| `kEauX` | Require eau, append x | bateau → bateaux |
| `kAuEuX` | Require au/eu, append x | tuyau → tuyaux; jeu → jeux |
| `kOuX` | Require ou, append x | chou → choux |
| `kAlAux` | Require al, replace with aux | cheval → chevaux |
| `kAilAux` | Require ail, replace with aux | travail → travaux |
| `kInvariant` | Return the normalized lemma | caller-selected invariant adjective class |

The caller must select exception classes using verified lexical information.
`bleu/pneu`, `trou`, `bal`, and `chandail` are tested with `kRegularS`; the core
never guesses these classes from a word list. A mismatched suffix returns
`kRuleNotApplicable`. There is no gender inference, masculine/feminine generation,
verb conjugation, reverse lemmatization, irregular lexical substitution, or compound
agreement. A misleading automatically selected suffix rule was intentionally
rejected in favor of an explicit API contract.

## Verification record

Environment observed on 2026-11-02: Windows x64; installed Visual Studio 2022
Professional; MSVC 19.44.35209.0; Windows SDK 10.0.26100.0; CMake/CTest
3.31.6-msvc6; MSBuild 17.14.10. No tool installation, system configuration change,
elevation, network operation or Git commit was performed. The shared worktree
already contained unrelated modifications.

PATH probes `command -v cmake`, `command -v ctest`, and `command -v cl` found no
PATH entries. They are available through the existing Visual Studio installation;
`vswhere.exe` and `rg --files -g cmake.exe -g ctest.exe -g VsDevCmd.bat -g
vcvars64.bat -g ninja.exe` located the actual files. `vswhere -latest` identified a
Community instance, but the installed CMake binaries were under Professional;
configuration explicitly selected that installed Professional instance. The
initial combined probe exited 2 because its optional D-drive install-directory
wildcard did not exist; tool discovery then exited 0.

Commands use the existing tools, with no machine-specific compiler path committed
into build configuration. For the record below, `$repo` denotes the repository,
`$vs` the observed Visual Studio 2022 Professional installation, and `$bin` is
`$vs/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin`:

```sh
"$bin/cmake.exe" --version
"$bin/ctest.exe" --version
"$bin/cmake.exe" -S "$repo/tests/french_core" -B "$repo/build/multi-french-core" \
  -G "Visual Studio 17 2022" -A x64 "-DCMAKE_GENERATOR_INSTANCE=$vs"
"$bin/cmake.exe" --build "$repo/build/multi-french-core" --config Debug --parallel 2
"$bin/ctest.exe" --test-dir "$repo/build/multi-french-core" -C Debug \
  --output-on-failure --output-junit "$repo/build/multi-french-core/french-core-debug.xml"
```

Initial version/configure/build/CTest commands each exited **0**. Initial Debug
CTest: **9/9 passed, zero failed, zero skipped**. The final isolated verification
receipt under `build/multi-french-core/verification.json` records exact local
argument arrays, observed exits, tool versions, source hashes and test executable
hashes; build logs and JUnit remain under that same isolated build tree.

Final verification on the unchanged reviewed sources:

| Command | Observed exit | Result |
| --- | --- | --- |
| CMake and CTest `--version` | 0 each | 3.31.6-msvc6 |
| Configure with the command above | 0 | Installed VS Professional, x64 |
| CMake build `--config Debug --parallel 2` | 0 | Strict MSVC warnings; no diagnostics |
| CTest `-C Debug --output-on-failure --output-junit .../french-core-debug.xml` | 0 | 9/9; zero failed/skipped |
| `Debug/epocanva_french_core_tests.exe force_failure` | 1 | Intended wrong assertion; `RESULT FAIL` |
| `Debug/epocanva_french_core_tests.exe not_a_mode` | 2 | Argument rejection; `INVALID MODE` |
| CMake build `--config Release --parallel 2` | 0 | Strict MSVC warnings; no diagnostics |
| CTest `-C Release --output-on-failure --output-junit .../french-core-release.xml` | 0 | 9/9; zero failed/skipped |
| `Release/epocanva_french_core_tests.exe force_failure` | 1 | Intended wrong assertion; `RESULT FAIL` |
| `Release/epocanva_french_core_tests.exe not_a_mode` | 2 | Argument rejection; `INVALID MODE` |
| `git diff --check -- engine/languages/fr tests/french_core docs/testing/french-core.md` | 0 | Tracked-diff whitespace check; new-file whitespace separately checked by the final review script |

The Release build/CTest command arguments are identical to the Debug examples
except `Debug` → `Release` and `french-core-debug.xml` →
`french-core-release.xml`. Final runs used `TMP/TEMP/TMPDIR` under
`build/multi-french-core/tmp`; the driver called subprocesses with argument arrays,
checked each real return code, and wrote only inside the isolated build tree.

Test executable SHA-256:

- Debug: `d684a9c0fab52a47713730cf6444902ed0f8ec6f1135a2e37e7c380be481b54c`.
- Release: `c2bca6c711a1a024b674d4c9f6a8c529a206922fcdae5f19eabbd884f88b0ace`.

Exact registered tests:

1. `french.core.utf8`
2. `french.core.accents_case`
3. `french.core.tokens`
4. `french.core.composition`
5. `french.core.suggestions`
6. `french.core.plurals`
7. `french.core.properties`
8. `french.core.assertions_can_fail`
9. `french.core.rejects_unknown_mode`

Tests cover every declared accent/case mapping, invalid scalar sequences,
normalization scope boundaries, original UTF-8 byte spans, apostrophes/hyphens,
URL/email/numeric protection, mid-buffer insertion, scalar-interior/cluster-interior
caret rejection, pending-accent behavior, deletion, cancellation, aliasing,
length limits, dictionary provenance rejection/dedup/ranking/case, lexical-class
plural exceptions, deterministic malformed-byte trials and combining-cluster
properties. The deliberate failure uses the actual normalizer with an intentionally
wrong expectation; the wrapper requires exact exit 1 and `RESULT FAIL`. Unknown
mode requires exact exit 2. Crashes, missing executables and unexpected success
cannot satisfy those controls. Tests use ordinary comparisons, not disabled-in-
Release `assert`, and report only assertion locations/counts, not text content.

Aggregate/root/product Release tests are the coordinator's responsibility and are
not claimed by this standalone record.

## Unfinished integration and privacy scope

Production French data with pinned source/version/hash/licence and redistribution
review remains absent. Rime schema/component integration, real engine behavior,
TSF composition/commit/cancellation, stale candidate handling, language switching,
privacy-classified field admission, cross-host consistency, and complete French
language coverage remain unverified. These are blockers before product activation;
fixture tests do not satisfy them. The exported C++ API is support-only and not a
promise of stable platform ABI.

Runtime implementation has no file/network/logging/persistence calls. This is an
architectural property of the inspected core, not an OS sandbox guarantee. An
ordinary Windows TSF DLL in an arbitrary host cannot claim OS-enforced network
isolation; future Windows integration must preserve the no-socket architectural
policy through dependency review/build policy. Android can enforce lack of
network access by omitting `android.permission.INTERNET`; this work contains no
Android implementation. No persistent learning API exists here, so sensitive
field learning cannot be enabled by this module.
