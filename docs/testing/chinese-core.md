# Chinese phonetic core verification

This phase implements standalone Mandarin phonetics in pure C++17. It does not
ship Chinese Hanzi/phrase conversion, a dictionary, language detection, a Rime
component, or a platform input method. English remains the higher-priority product
language. No English behavior was changed by this work.

## Scope and environment

New source files owned by this phase:

- `engine/languages/zh/phonetic_core.h`
- `engine/languages/zh/phonetic_core.cc`
- `tests/chinese_core/CMakeLists.txt`
- `tests/chinese_core/main.cc`
- `docs/testing/chinese-core.md`

All configure/build/test artifacts are under `build/multi-chinese-core/`. The
aggregate verification driver is generated there, not added to product sources.
No root/product CMake, shared API, component, platform, third-party or dictionary
files were edited by this phase. Other work was already present in the shared
worktree and was left untouched. No packages, system settings, git commits or
network downloads were performed.

Verified locally on 2026-11-02: Windows x64, Visual Studio 2022, MSVC
19.44.35209.0, Windows SDK 10.0.26100.0, CMake/CTest 3.31.6-msvc6. The targets
require C++17, disable language extensions and compile with `/permissive- /W4
/WX /utf-8`. Non-MSVC compiler options are declared but were not executed here.
The source contains only standard C++ headers; it does not link librime.

The supplied AGENTS context described licensing as undecided; the actual local
AGENTS.md has an existing owner-selected license decision. This discrepancy did
not require any licensing action: no license headers or license files were added.

## Public API and units

The authoritative declarations are in `engine/languages/zh/phonetic_core.h`,
namespace `epocanva::zh`. This header was sent to the integrating coordinator
before implementation.

```cpp
SyllableResult ParsePinyinSyllable(std::string_view input_utf8);
SyllableResult ParseZhuyinSyllable(std::string_view input_utf8);
SegmentationResult SegmentPinyin(std::string_view input_utf8, Limits limits = {});
SegmentationResult AnalyzeZhuyin(std::string_view input_utf8, Limits limits = {});

PhoneticComposer(InputMode mode, Limits limits = {});
Status AppendText(std::string_view text_utf8);
Status TypeKey(char ascii_key);
bool Backspace();
void Cancel() noexcept;
const std::string& preedit_utf8() const noexcept;
SegmentationResult Analyze() const;
CommitResult CommitPhonetic(
    std::optional<std::size_t> alternative_ordinal = std::nullopt);

ConversionResult ConvertPhonetic(const PhoneticPath& reading,
                                 const DictionaryData& data = {},
                                 std::size_t max_candidates = 16);
```

`ByteSpan` is a half-open original-input UTF-8 byte span. `input_bytes`,
`error_byte`, `consumed_input_bytes` and every size named `*_bytes` use UTF-8 code
units, not UTF-16 or grapheme clusters. A diagnostic with `kNoByteOffset` has no
particular offending byte. Collection ordinals/counts select paths, syllables,
candidates or source rows; they are never text offsets. The core accepts no caret
or selection offsets. A future adapter must define and verify UTF-8-to-UTF-16
conversion before exposing any of these spans to TSF.

Malformed UTF-8 is rejected, including overlong encodings, stray/truncated
continuations, surrogate encodings and values above U+10FFFF. Input limits are
checked before decoding. Failure never returns a converted prefix. Backspace
removes one complete UTF-8 scalar encoding at the end; it is not a general
Unicode grapheme-cluster editor. Cancel and successful commit clear active
preedit. They do not promise secure erasure of allocator memory.

## Implemented phonetics

Pinyin uses an explicit initial/final compatibility grammar. Canonical keys are
lowercase and toneless; n/l preserve `u` versus `v`, while j/q/x/y use ordinary
orthographic `u`. Accepted aliases include `ü`, `v`, precomposed upper/lowercase
vowel tone marks, and one suffix digit 1–5. The parser verifies tone-mark vowel
placement and rejects duplicate/mixed tone encodings. Canonical phonetic display
uses `ü` and a suffix tone digit. The standalone ê interjection has the explicit
canonical key `eh` and display `ê`.

`nu`/`nv` and `lu`/`lv` are different readings; n/l are never fuzzy substitutes.
`nue`, `lue`, `u:`, `iou`, `uei`, abbreviation-only commits and non-phonotactic
initial/final combinations are not corrections accepted by this grammar. A valid
unfinished syllable prefix reports `kIncomplete`; for example `din` can still
become `ding`, and `jua` can become `juan`. Regional extensions, syllabic nasal
interjections and automatic erhua attachment are outside this initial grammar.
The compatibility table is explicit and remains subject to native-speaker review;
passing algorithm tests is not a claim of exhaustive dialect coverage.

Segmentation constructs a syllable-edge DAG over original UTF-8 byte boundaries.
A backwards dynamic program counts complete and pending paths with saturation;
a second bounded count dimension enforces the syllable limit. It materializes
only the requested number of alternatives, in ascending syllable count with
longer leading syllables first for ties. These are structural ordering rules,
not corpus probabilities or word-frequency scoring. Complete paths take priority;
pending suffix paths are exposed when no complete parse exists.

Examples:

| Input | Phonetic interpretation |
| --- | --- |
| `xian` | Both `xian` and `xi'an`; explicit ambiguity |
| `xi'an` | Boundary forces `xi` + `an` |
| `fangan` | Includes `fang'an` and `fan'gan` |
| `xingan` | Includes `xing'an` and `xin'gan` |
| `ni3hao3` | `ni3'hao3`, preserving source byte spans |
| `zhongg` | Complete `zhong` plus pending `g` |
| `xi'` | Complete `xi` plus an unfinished boundary |

Leading/doubled apostrophes are invalid. Trailing apostrophes are pending and
cannot commit. Ambiguity remains explicit even if the alternative cap or syllable
cap hides other paths. `CommitPhonetic()` reports `kAmbiguous` until the caller
supplies a retained path ordinal; it never silently chooses a linguistic meaning.

Zhuyin uses the same Mandarin phonotactics through onset/medial/rime mappings,
including apical-vowel initials, zero-initial y/w spellings and front-rounded
vowels. Direct symbols and the standard keyboard are supported. Space or a suffix
tone establishes a syllable boundary. No word boundary is guessed from a pasted
unseparated toneless multi-syllable string. Tones 1–4 may be suffix marks; neutral
tone may be leading or trailing and is rendered before the syllable. Unspecified
tone remains distinct from an explicit first tone.

`TypeKey()` accepts deliberate standard-keyboard events only in
`kZhuyinStandardKeyboard` mode: `su3cl3` yields `ㄋㄧˇ ㄏㄠˇ`. Space maps to an
explicit first-tone mark; `6/3/4/7` map to second/third/fourth/neutral tones.
The punctuation keys `, . ; / -` are intentional phonetic mappings only in that
API. `AppendText("su3")` is protected ASCII, not keyboard replay. Its Zhuyin
preedit and source spans refer to generated symbol UTF-8 bytes, not keystroke
counts. This distinction must be preserved by any eventual event adapter.

## Protection, limits and data availability

These APIs require explicit phonetic mode; they do not identify English versus
Pinyin from bare ASCII letters. ASCII URL/email marks, mixed punctuation, controls,
spaces in Pinyin, and unsupported Unicode cause whole-input `kProtectedInput`.
No punctuation is dropped or rewritten to salvage a phonetic tail. Invalid UTF-8
is reported separately. `AppendText()` is transactional: rejection leaves existing
preedit unchanged so a host can apply its own lossless passthrough policy.
Apostrophes in Pinyin, tone digits, and intentional Zhuyin keyboard punctuation
are documented mode-specific exceptions, not a general ASCII conversion policy.

Default limits are 256 input bytes, 32 syllables including a pending syllable and
8 retained alternatives. Hard ceilings are 512 input bytes, 64 syllables, 32
alternatives and 24 bytes per single-syllable parse. Out-of-range options fail
explicitly. The graph and dynamic-programming state are bounded by those limits;
no exponential alternative enumeration is performed. Boundary tests include 64
complete syllables, rejection of the 65th, truncated ambiguities and byte-limit
checks involving multibyte vowels.

`DictionaryData` defaults to `SourceStatus::kUnavailable`. In that state conversion
returns `kUnavailable` and no candidates even if a caller accidentally populated
rows. Explicit caller-supplied data requires source/schema labels, one complete
phonetic syllable per schema element, valid UTF-8 candidate text and bounded rows.
No dictionary loader, dataset discovery or approval inference exists here.

Once a caller supplies data, lookup matches the entire canonical reading, with
exact tones unless that caller explicitly disables tone sensitivity. A schema may
encode Pinyin or Zhuyin; both resolve to the same phonetic keys. Results preserve
source-row order, deduplicate identical candidate text and report candidate
truncation. The entire supplied table is validated before any candidate result
can escape, including invalid rows after the result cap. Limits are 4,096 source
rows, 1 MiB total source/schema/reading/candidate UTF-8 bytes, 1,024 bytes per
candidate and 64 returned candidates. Input allocations already owned by the
caller are outside these limits; processing/returned copies are bounded here.

There is no shipped Hanzi or phrase coverage. A caller may supply complete phrase
rows, but the core neither composes phrases from character rows nor claims a
phrase dictionary. Frequency ranking and traditional/simplified conversion remain
explicitly unavailable even with supplied rows. Tests use synthetic ASCII labels
as candidate payloads, never a tiny Hanzi list masquerading as language support.
Source labels are caller assertions, not verified provenance, licensing or product
approval.

There is no history, learning, persistence, file access, network I/O or logging in
the runtime core. Only active preedit and caller-owned results/data contain text.
Test diagnostics report assertion source lines and counts, not input or candidate
contents. A future host must still reject sensitive fields and must not add
persistence around this core. A conventional Windows TSF DLL's network exclusion
is an architectural dependency/code policy, not an OS sandbox guarantee. Android
can enforce network exclusion by omitting `android.permission.INTERNET`; no
Android manifest or adapter was added or verified here.

## What was verified against local Rime

The local checkout HEAD equals `33e78140250125871856cdc5b42ddc6a5fcd3cd4`, matching
`third_party/sources.lock.json:36`. The checkout was clean before and after this
work. The following pinned sources were read before making integration claims:

- `third_party/librime/data/minimal/luna_pinyin.schema.yaml:70`: speller alphabet,
  space/apostrophe delimiters and spelling algebra. In particular line 77 permits
  the n/l `ve` → `ue` correction; line 89 selects the Luna dictionary and line 91
  formats `v` as `ü`. Those are schema choices, not rules imported into this core.
- `third_party/librime/src/rime/algo/syllabifier.cc:69`: prefix search through a
  `Prism`; line 106 reads schema spelling properties and line 257 begins overlap
  ambiguity handling. Its graph is not a dictionary-free Pinyin grammar that this
  phase can claim to have integrated merely by recognizing syllables.
- `third_party/librime/src/rime/algo/syllabifier.h:39`: upstream graph state;
  `BuildSyllableGraph` requires the caller's `Prism`.
- `third_party/librime/test/generate_pinyin_syllables.js:1`: an upstream random-test
  syllabary with numerical weights. Its entries/weights were not copied or used
  as language data, frequency ranking or test fixtures.

This phase builds no Rime schema, Prism, translator, simplifier or integration
component. The phonetic API is preparation for those separately approved gates.

## Executed verification

For privacy and portability, the command record below substitutes `R` for the
absolute repository root and `C` for the installed Visual Studio CMake `bin`
directory. These substitute only path prefixes; flags, configurations and observed
exit codes are recorded exactly. No compiler path is baked into source or CMake.

| Executed command (Git Bash path-prefix notation) | Observed result |
| --- | --- |
| `command -v cmake; command -v ctest; command -v ninja; command -v clang++; command -v g++; command -v cl; command -v pwsh; command -v powershell` | First seven probes had no match; only Windows PowerShell resolved. Combined command exit 0 because its final probe succeeded. No tools installed. |
| `git -C "$R/third_party/librime" rev-parse HEAD` | Exit 0; pinned SHA above |
| `git -C "$R/third_party/librime" status --porcelain` | Exit 0; empty both before and after |
| `"$C/cmake.exe" --version` and `"$C/ctest.exe" --version` | Both exit 0; 3.31.6-msvc6 |
| `"$C/cmake.exe" -S "$R/tests/chinese_core" -B "$R/build/multi-chinese-core" -G 'Visual Studio 17 2022' -A x64` | Exit 0 |
| `"$C/cmake.exe" --build "$R/build/multi-chinese-core" --config Debug --parallel 2` | Exit 0 |
| `"$C/ctest.exe" --test-dir "$R/build/multi-chinese-core" -C Debug --output-on-failure` (first run) | Exit 8; six passed, grammar expectations failed and dictionary fixture timed out |
| Same Debug build and CTest commands after fixture corrections | Both exit 0; 8/8 passed |
| Same Debug build and CTest commands after finite-grammar invariant additions | Both exit 0; 8/8 passed, 6.28 s |
| `"$C/cmake.exe" --build "$R/build/multi-chinese-core" --config Release --parallel 2` | Exit 0 |
| `"$C/ctest.exe" --test-dir "$R/build/multi-chinese-core" -C Release --output-on-failure` | Exit 0; 8/8 passed, 1.36 s |
| `"$C/ctest.exe" --test-dir "$R/build/multi-chinese-core" -C Release -V` | Exit 0; exact process exits and assertion counts below |
| `"$C/cmake.exe" -S "$R/build/multi-chinese-core/aggregate-driver" -B "$R/build/multi-chinese-core/aggregate" -G 'Visual Studio 17 2022' -A x64` | Exit 0; parent CRT-property assertions passed |
| `"$C/cmake.exe" --build "$R/build/multi-chinese-core/aggregate" --config Debug --parallel 2` | Exit 0 |
| `"$C/ctest.exe" --test-dir "$R/build/multi-chinese-core/aggregate" -C Debug --output-on-failure` | Exit 0; 8/8 passed, 2.07 s |
| `git -C "$R" diff --check -- engine/languages/zh tests/chinese_core docs/testing/chinese-core.md` | Exit 0; new files are untracked, so this alone does not validate their whitespace |
| PowerShell `Get-Content -Encoding UTF8` over the five owned files, failing on `[\\t ]+$` | Exit 0; all five new files explicitly checked for trailing whitespace |

The initial PATH probes were discovery, not a claim of unavailable compilation.
A PowerShell probe of the Community CMake path reported missing files; the host
returned exit 0 despite those PowerShell errors, so that attempt was not treated
as success. Searching existing installations located the Professional CMake and
MSVC binaries; configure then identified the actual working compiler above.

The first grammar failures were incorrect test expectations: `din` and `jua` are
unfinished legal prefixes. Tests now explicitly check `kIncomplete` and use truly
invalid `dinn`/`juae` for rejection. The dictionary test's oversized-table setup
used `vector::assign` with a reference to the vector's own row; that setup was
changed to an independent row copy. The observed first-run timeout was not counted
as a passing negative test. No grammar rejection was loosened to green the suite.

| CTest name | Assertions / expected process exit |
| --- | --- |
| `chinese.core.pinyin_grammar` | 139 / 0 |
| `chinese.core.segmentation_ambiguity` | 68 / 0 |
| `chinese.core.zhuyin_state` | 10,244 / 0 |
| `chinese.core.composition_edits` | 61 / 0 |
| `chinese.core.bounds_utf8_protection` | 234 / 0 |
| `chinese.core.caller_dictionary` | 41 / 0 |
| `chinese.core.assertions_can_fail` | One intentionally false assertion / exactly 1 |
| `chinese.core.rejects_unknown_arguments` | Invalid mode / exactly 2 |

The six functional suites execute 10,787 assertions. They include positive and
negative phonotactics, ambiguity hidden by bounds, n/l and u/ü distinctions,
apostrophes, tone marks, keyboard mappings, editing, neutral tones, malformed
UTF-8, original byte spans, whole-token protection, malformed late dictionary
rows, data budgets and unavailable data. Zhuyin tests enumerate all 22 × 4 × 14
onset/medial/rime combinations and check accepted readings, typeable scalar-byte
prefixes, all five tones and canonical-display round trips. Test assertions are
ordinary code, not `assert`, so Release cannot disable them. The existing
`tests/infrastructure/assert_probe.cmake` compares exact process exits and marker
presence; launch failure, timeout or a crash cannot satisfy a negative test.

`tests/chinese_core/CMakeLists.txt` works standalone and through
`add_subdirectory()`. It calls `project()`/`enable_testing()` only when standalone,
uses the unique `epocanva_zh_core` and `epocanva_chinese_core_tests` targets and
never overrides a parent's CRT policy. The build-only aggregate driver selected
`MultiThreaded$<$<CONFIG:Debug>:Debug>` and asserted that both targets inherited
that property before compiling and running all eight tests. No product behavior
is inferred from that isolated aggregate test.

## Unfinished product gates

1. Approve dictionary/schema sources, their licenses, hashes, manifests, provenance
   and intended Traditional/Simplified coverage. Until then actual Hanzi/phrase
   coverage and frequency data remain unavailable.
2. Review Mandarin grammar variants and tone policy with native-speaker input,
   including regional/interjection/erhua policy and any later normalization.
   Composing arbitrary Unicode grapheme clusters is not implemented here.
3. Approve product event routing and explicit language mode selection while
   preserving English priority. Hosts must distinguish pasted technical text from
   deliberate Zhuyin keyboard events and define a lossless rejection policy.
4. Implement separately reviewed Rime/shared-API/platform adapters, UTF-8/UTF-16
   boundaries, candidate UI and sensitive-field admission. None were exercised by
   these tests, and this core is not registered as a shipping input method.
5. Supply approved phrase/frequency/script-conversion data and algorithms before
   claiming contextual Chinese composition, frequency learning or traditional–
   simplified conversion. Caller-supplied exact-row lookup does not close them.
6. Run product and cross-app regressions through the integrating coordinator's
   build, plus other compiler/platform validation if those targets are approved.
   This report verifies the standalone core and isolated aggregate build only.
