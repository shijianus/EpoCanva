# Japanese and Korean composition cores

These are independently implemented and tested C++17 composition algorithms for
parallel multilingual preparation. Product priority remains **English > Chinese >
French > Japanese > Korean**. This work does not replace or downgrade the English,
Chinese, or French paths.

There are **no dictionaries, lexicon candidates, Kana-to-Kanji conversion, Korean
word prediction, TSF support, or demonstrated product support** in these cores.
The kana spelling rules and Hangul alphabet rules below are algorithms, not word
lists. A passing core suite proves the documented composition behavior; it does
not prove that a host application can type Japanese or Korean through EpoCanva.

## Environment and files

Validation on 2026-11-02 used the installed Visual Studio 2022 Professional CMake
from an existing local CMake cache: CMake/CTest 3.31.6-msvc6, generator `Visual
Studio 17 2022`, x64, MSVC 19.44.35209.0, and Windows SDK 10.0.26100.0 on Windows
10.0.26200. No tool installation, system configuration change, elevation, network
request, downloaded data, or Git operation was used. Compiler and build tool
paths remain machine-local; no compiler path is committed here.

Files added for this work:

- `engine/languages/ja/romaji_composer.h`
- `engine/languages/ja/romaji_composer.cc`
- `engine/languages/ko/two_set_composer.h`
- `engine/languages/ko/two_set_composer.cc`
- `tests/composition_core/CMakeLists.txt`
- `tests/composition_core/main.cc`
- `tests/composition_core/test_support.h`
- `tests/composition_core/japanese_tests.cc`
- `tests/composition_core/korean_tests.cc`
- `tests/composition_core/invariant_tests.cc`
- `tests/composition_core/assert_result.cmake`
- `docs/testing/japanese-korean-core.md`

The composition task changes no root, shared, or product CMake file, C ABI, Rime
code, or platform adapter. Other tasks may separately aggregate these tests.

## C++ API and units

The independent types are `epocanva::ja::RomajiComposer` and
`epocanva::ko::TwoSetComposer`; their headers define their own `InputResult` and
`Preedit` types. Neither includes the other language or a shared engine header.

| Operation | Contract |
| --- | --- |
| `PushAscii(char ascii_byte)` | Accepts one printable ASCII byte, 0x20 through 0x7e. Returns `kAccepted`, `kRejectedByte`, or `kInputLimit`. Rejection leaves composition and undo state unchanged. |
| `GetPreedit()` | Returns a value snapshot of all outstanding text, not text already sent to a host. |
| `Backspace()` | Removes exactly one accepted ASCII-byte input step and reconstructs the preceding state. Returns false without mutation when empty. |
| `Cancel()` | Drops all outstanding text, pending state, and transient history. |
| `CommitUtf8()` | Returns strict UTF-8, clears outstanding state/history, and performs no host insertion. A second commit returns empty text. |
| Japanese `SetMode(KanaMode)` / `GetMode()` | Supports `kHiragana` and `kKatakana`. A change rerenders all outstanding kana, retains literal case, and does not add an input step. Commit/cancel retain the selected mode. |

`Preedit::text_utf8` is strict UTF-8. `caret_utf8_bytes`,
`active_begin_utf8_bytes`, and `active_size_utf8_bytes` are all **UTF-8 code units
(bytes)**. The caret is always at the UTF-8 byte end. The active span ends there
too; its extent can be zero. No grapheme, UTF-16, screen-cell, or host-document
offsets are exposed. Tests verify that the active span starts at a UTF-8 byte
boundary. Hangul L/V/T ordinals inside the Korean algorithm are alphabet ordinals,
not text offsets.

`input_step_count` counts accepted ASCII-byte input steps since commit/cancel,
including punctuation and digits. Each core accepts at most `kMaxInputSteps =
4096`; another printable byte returns `kInputLimit`. Invalid bytes return
`kRejectedByte` even at the limit. Backspace frees one input step. Test output
labels `assertion_count` and `failure_count`; neither counts characters or bytes.

The API intentionally accepts no raw Unicode input, control characters, tab,
newline, NUL, or DEL. Callers must route those events separately. It has no caret
movement, selection editing, host event interpretation, or UTF-16 conversion.

## Japanese state machine

The source of truth is the spelling-rule table in
`engine/languages/ja/romaji_composer.cc:18` and incremental transitions at
`romaji_composer.cc:131` and `romaji_composer.cc:164`. Rules match ASCII letters
case-insensitively. Resolved kana is appended as valid Unicode scalars; a possible
spelling prefix remains visible as its original ASCII bytes until resolved.

Supported spellings include the basic voiced/unvoiced rows; `shi/si`, `chi/ti`,
`tsu/tu`, `fu/hu`, `ji/zi`; yoon such as `kya`, `shu/syu`, `cha/tya/cya`, and
`ja/jya/zya`; small kana through `x` and `l` (`xa/la`, `xya/lya`,
`xtu/xtsu/ltu/ltsu`, `xwa/lwa`, `xka/lka`, `xke/lke`); and explicit foreign-sound
rules for `f`, `v`, `ts`, `th`, `dh`, `wh`, `she`, `je`, and `che` forms. The rule
table gives the full finite spelling set. There is no syllabic `m` alias or
attempt to guess a word's spelling.

Repeated supported onsets in `bcdfghjkmprstvwyz` emit a sokuon, retaining the
second onset as the next prefix: `gakkou` becomes `がっこう`, `issho` becomes
`いっしょ`, and `kk` is visible/committed as `っk`. The `tc` prefix waits for `h`,
so `matcha` becomes `まっちゃ`; `tca` instead preserves `tc` and produces `tcあ`.
Repeated `n` has its own rule, and `q`, `x`, and `l` do not create a sokuon.

Nasal boundaries are deliberately specified rather than delegated to a platform
IME:

| Input | Outstanding preedit / committed result |
| --- | --- |
| `n` | Visible `n`; commit produces `ん`. |
| `na`, `nya` | `な`, `にゃ`; vowel/`y` continuations use the pending onset. |
| `n'a`, `n'ya` | `んあ`, `んや`; the apostrophe ends and is consumed by a nasal. |
| `n `, `n1`, `n!` | `ん `, `ん1`, `ん!`; the boundary is preserved. |
| `nn` | Immediately shows one `ん`; commit also produces exactly one `ん`. The second `n` may be reused, but is invisible and does not add an active byte span. |
| `nna`, `nnya` | `んな`, `んにゃ`; reuse the second `n` before an immediate vowel/`y`. |
| `nn'a` | `んあ`; the apostrophe consumes optional reuse. |
| `nnka`, `nn ` | `んか`, `ん `; a non-vowel/non-`y` continuation drops optional reuse. |
| `nnn`, `nnnn` | Commit produces `んん` in either case. |
| `ny` | Visible and committed as literal `ny`; it is an incomplete non-nasal prefix. |

Only the pending ASCII prefix is the Japanese active span. Resolved kana and
unrecognized literal bytes remain outstanding text in the same composition.
Unknown leading bytes are preserved with their original case, then the remaining
suffix is retried. Thus `Qka12,sh!n?` becomes `Qか12,sh!ん?`, and `kompyu-ta-`
becomes `こmぴゅーたー`. Recognized kana may still follow a preserved literal.
A hyphen always produces the prolonged-sound mark `ー`; other ASCII punctuation
and digits are literal boundaries. An apostrophe away from a nasal is literal.

Commit converts a bare pending `n` to the nasal but preserves other incomplete
prefixes, such as `SH`, `ny`, or `xts`, byte-for-byte. Cancel drops all of them.
Backspace undoes an input step: `kya -> ky -> k -> empty`, and
`nna -> nn -> n -> empty` with preedit `んな -> ん -> n -> empty`. It also
restores pending state across punctuation or an apostrophe.

Katakana mode maps emitted hiragana scalars to the parallel katakana block;
literals remain unchanged. `konpyu-ta-` becomes `コンピューター`. Changing the
mode rerenders outstanding resolved kana and leaves pending Roman bytes intact.
Backspace after a mode change still removes the last accepted input byte, using
the current mode; mode changes are not undo steps.

## Korean state machine

`engine/languages/ko/two_set_composer.cc:15` defines the standard two-set key
mapping, `two_set_composer.cc:62` the seven compound-vowel transitions, and
`two_set_composer.cc:84` the eleven final clusters. The active state consists of
an optional initial, optional medial, and optional final. Modern syllables use
Unicode Hangul composition at `two_set_composer.cc:217`. These references are to
the actual implementation; no behavior of a platform IME or external dependency
is assumed.

| ASCII keys | Jamo in key order |
| --- | --- |
| `r s e f a q t d w c z x v g` | `ㄱ ㄴ ㄷ ㄹ ㅁ ㅂ ㅅ ㅇ ㅈ ㅊ ㅋ ㅌ ㅍ ㅎ` |
| `R E Q T W` | `ㄲ ㄸ ㅃ ㅆ ㅉ` |
| `k o i O j p u P h y n b m l` | `ㅏ ㅐ ㅑ ㅒ ㅓ ㅔ ㅕ ㅖ ㅗ ㅛ ㅜ ㅠ ㅡ ㅣ` |
| `hk ho hl nj np nl ml` | `ㅘ ㅙ ㅚ ㅝ ㅞ ㅟ ㅢ` |

Only `R/E/Q/T/W/O/P` differ from their lowercase keys; other uppercase keys map
to their lowercase counterparts. Every ASCII letter is a Korean key in this API.
Digits and ASCII punctuation flush the active syllable/Jamo and are preserved
literally. For example `gksrmf123! dkssud?` becomes `한글123! 안녕?`; this is not
an English-text detection or language-switching mechanism.

There is no inserted `ㅇ`: `k` alone is `ㅏ`, whereas `dk` is `아`. An incomplete
initial or standalone vowel remains a compatibility Jamo on commit. A consonant
after a standalone vowel starts an initial rather than becoming that vowel's
final. Initial-only consonants do not form final clusters with one another.

Compound finals are `ㄳ ㄵ ㄶ ㄺ ㄻ ㄼ ㄽ ㄾ ㄿ ㅀ ㅄ`. A following vowel moves
a simple final to the new onset, or splits a cluster and moves its second
consonant: `rkrk -> 가가`, `rkrtk -> 각사`, `ekfrk -> 달가`. A shifted simple
final stays tense when moved: `rkRk -> 가까`, `rkTk -> 가싸`. `ㄸ/ㅃ/ㅉ` cannot
be finals and start a new initial instead.

Repeated unshifted consonants **do not synthesize tense consonants**, whether
initials or finals. `rrk -> ㄱ가`, `rkrrk -> 각가`, and `rkttk -> 갓사`; use `R`
or `T` for `ㄲ` or `ㅆ`. Likewise, unsupported vowel sequences remain separate:
`kl -> ㅏㅣ`, `jl -> ㅓㅣ`, `hkl -> ㅘㅣ`, and `njl -> ㅝㅣ`. There is no
three-input alias for `ㅙ/ㅞ`; their direct combinations are `ho/np`.

Backspace reconstructs the preceding accepted-input state, including syllable
migration. The input trace `rhkrtk9` produces `곽사9`; repeated backspace shows
`곽사 -> 곿 -> 곽 -> 과 -> 고 -> ㄱ -> empty`. Shifted `O` and `R` each count
as a single input step, so one backspace removes that whole shifted Jamo.
The Korean active span is one syllable or compatibility Jamo (three UTF-8 bytes),
or empty after a literal boundary. Earlier syllables are still outstanding until
commit/cancel. Commit is a hard boundary: after committing `rkr` as `각`, a new
`k` yields `ㅏ`, without moving the previous final.

## Implementation choices and privacy

Push operations advance actual incremental state machines. Backspace replays
bounded transient ASCII history, rather than guessing how to remove a Unicode
scalar. This preserves exact history semantics even for yoon, final migration,
shifted keys, punctuation boundaries, and Japanese mode rerendering. The maximum
history is 4,096 accepted ASCII-byte steps; backspace and Japanese mode changes
are linear in that bound. Replay was chosen over per-step full-text snapshots,
which would retain quadratic text storage. No timing/performance guarantee is
claimed.

The only runtime includes are the C++ standard string/string-view/utility and
size types plus each core's own header. There is no platform API, Rime API,
third-party implementation, dictionary access, network call, persistence, or
logging. State is per instance. History exists only in memory for this active
composition. Commit/cancel clear its logical contents but do not promise secure
erasure of allocator memory. Test diagnostics print synthetic case identifiers
and labelled assertion counts, never input or composed text.

No OS-enforced network guarantee is claimed for these cores. A normal Windows
TSF DLL in an arbitrary host would rely on architecture and build/review policy;
the OS does not sandbox such a DLL away from the network. Android can enforce a
network restriction by omitting `android.permission.INTERNET`. Neither platform
integration is implemented or demonstrated by this work.

## Independent tests and observed commands

`tests/composition_core/CMakeLists.txt` is a self-contained project when used as
the source directory. It can also be added as a subdirectory by an aggregate
project: `project()` and `enable_testing()` are standalone-only, target names are
unique, and the aggregate parent's CRT choice is inherited. The aggregate parent
must enable testing itself. All targets require C++17 with extensions disabled;
MSVC builds use `/permissive- /utf-8 /W4 /WX`.

The target names are `epocanva_ja_composition_core`,
`epocanva_ko_composition_core`, and `epocanva_multilingual_composition_tests`.
There are five CTest cases:

| CTest case | What it checks | Observed child exit / counts per configuration |
| --- | --- | --- |
| `composition.core.japanese` | Authored expected kana, incremental states, n/n'/nn boundaries, modes, mixed input, prefix/literal handling, input-step undo, cancel and commit. | Exit 0; `assertion_count=3092 failure_count=0`. |
| `composition.core.korean` | Authored key/Jamo and word examples, all seven compound vowels, all eleven final clusters and migration/undo, repeat/shift behavior, literals, and every modern syllable. | Exit 0; `assertion_count=177948 failure_count=0`. |
| `composition.core.invariants` | Independent strict UTF-8 decoder with malformed controls, all 9,025 printable-ASCII pairs per core, 32 deterministic 96-input-step walks per core with exact undo/replay, all invalid-byte values, 4,096-step limits and recovery, and instance isolation. | Exit 0; `assertion_count=873628 failure_count=0`. |
| `composition.core.intentional_failure` | Real composition `ka` is intentionally compared with wrong expected kana `き` through the same assertion helper as the other suites. | Exit **1**; exactly `assertion_count=3 failure_count=1`, with the named false assertion. |
| `composition.core.rejects_unknown_argument` | An unknown suite flag must be refused. | Exit **2**, explicit argument error and no passing summary. |

The Korean exhaustive test enumerates independent key sequences in Unicode
alphabet order, decodes the actual UTF-8 result, and compares against successive
scalars U+AC00 through U+D7A3. It covers all **11,172 syllable cases** without
calling the implementation's composition formula or reading its rule tables.
The hand-authored migration traces separately verify transitions between those
syllables. Invariant replay checks complement these expected-output tests; they
are not used as the only oracle.

The wrapper requires both an exact child exit and an anchored summary. It does
not use CTest `WILL_FAIL`, so a crash, timeout, missing executable, swallowed
argument, or unrelated failure cannot satisfy the intentional-failure control.

Actual validation used the absolute cached executable and absolute repository
paths. The commands below replace only the machine-specific prefix with
`<VS-CMake-bin>` and `<repo>`; arguments, ordering, configurations and observed
exits are preserved. These placeholders are not shell variables to copy verbatim.
All generated build/test files stayed under `build/multi-composition-core`.

```text
"<VS-CMake-bin>/cmake.exe" --version
  exit 0; 3.31.6-msvc6
"<VS-CMake-bin>/ctest.exe" --version
  exit 0; 3.31.6-msvc6
"<VS-CMake-bin>/cmake.exe" -S "<repo>/tests/composition_core" -B "<repo>/build/multi-composition-core" -G "Visual Studio 17 2022" -A x64
  exit 0
"<VS-CMake-bin>/cmake.exe" --build "<repo>/build/multi-composition-core" --config Debug --parallel 2
  exit 0
"<VS-CMake-bin>/ctest.exe" --test-dir "<repo>/build/multi-composition-core" -C Debug --output-on-failure -V
  initial exit 8; 4/5 cases passed; Japanese fixture assertions failed
"<VS-CMake-bin>/cmake.exe" --build "<repo>/build/multi-composition-core" --config Debug --parallel 2
  exit 0 after correcting fixtures
"<VS-CMake-bin>/ctest.exe" --test-dir "<repo>/build/multi-composition-core" -C Debug --output-on-failure -V
  exit 0; 5/5 cases passed
"<VS-CMake-bin>/cmake.exe" --build "<repo>/build/multi-composition-core" --config Release --parallel 2
  exit 0
"<VS-CMake-bin>/ctest.exe" --test-dir "<repo>/build/multi-composition-core" -C Release --output-on-failure -V
  exit 0; 5/5 cases passed
```

The first Japanese run correctly failed on two fixture input-step counts and a
fixture that assumed an undocumented `m` nasal alias. The counts were corrected;
the loanword now uses `konpyu-ta-`, and a separate regression requires `m` in
`kompyu-ta-` to remain literal. Core implementations did not need a change to
satisfy those corrections. The final Debug and Release suites each executed
1,054,668 non-control assertions, plus the separate failure/argument controls.

Exploratory `rg`/`ls` probes returned exit 2 when scoped directories did not yet
exist; those were discovery results, not passing test phases. CMake/CTest were
found in existing local caches, so no missing-tool workaround was necessary.

## Remaining work and limits

There is no blocker for using these as standalone algorithms under the stated
contract. Product or TSF support still needs a separately approved integration
phase: key/event routing, focus and privacy admission, composition lifecycle,
host insertion/cancellation, UTF-8-byte to UTF-16-code-unit boundary handling,
and cross-application verification. No C ABI or platform adapter is supplied
here. A future Kana-to-Kanji or lexicon feature needs actual licensed data,
provider design, and independent verification; none is implied by these results.

This work verifies the installed MSVC Debug and Release configurations. It does
not claim GCC/Clang, Android, platform-IME equivalence, complete romanization
alias coverage, historical Hangul, or all possible long input sequences. Unicode
normalization of host-provided text and raw Unicode input are outside this API.
Language priority is `English > Chinese > French > Spanish > Japanese > Korean > German`
(see `docs/product/requirements.md` §3 and ADR 0005; the five-language order printed
in earlier revisions of this document is superseded).
