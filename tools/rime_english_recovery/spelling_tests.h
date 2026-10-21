// EpoCanva P2.6-recovery -- one-edit spelling suggestions: gates, candidates, ranking.
//
// CONTRACT TABLE. Everything below was read out of the surviving sources named in the
// right-hand column, not from a log and not from the old harness. Where the fixture
// documentation and the sources agree, that agreement is what this file asserts.
//
//  | fact                       | value / rule                                        | source |
//  | -------------------------- | --------------------------------------------------- | ------ |
//  | length window              | 4 .. 32 UTF-8 code units; the too-long test runs    | spelling_suggester.h:88-89, |
//  |                            | BEFORE the too-short one and before any content     | .cc:126-133 |
//  |                            | rule, so a 200-byte token is classified by its      |
//  |                            | real length and never cut down first                |
//  | admitted edits             | deletion, insertion (a-z only), substitution,       | spelling_suggester.h:10-20, |
//  |                            | transposition of ADJACENT unequal letters; exactly   | .cc:29-77 |
//  |                            | one of them; no second pass, no composition of two  |
//  | suppressions, in this      | (1) feature off (2) no usable word list (3) empty    | spelling_suggester.h:29-57, |
//  | fixed order                | (4) too long (5) too short (6) token gate: protected | .cc:107-177 |
//  |                            | / non-ASCII / unsupported byte / any delimiter,      | english_token.h:71-76,148-157 |
//  |                            | (7) mixed case (8) the canonical key IS a row        | english_case.h, |
//  |                            | (9) an ordinary completion exists -- judged on       | english_lexicon.h |
//  |                            | `result.match_count`, which excludes the original    | english_translator.cc:475-486 |
//  |                            | text and is taken BEFORE the budget cut              |
//  | suggestion budget          | 3, and only ever lowerable: `max_suggestions` is     | english_diagnostics.h:73-79, |
//  |                            | clamped to EPOCANVA_ENGLISH_MAX_SUGGESTIONS_DEFAULT  | english_translator.cc:302-310 |
//  | candidate ceiling          | menu page_size 8, and completions and suggestions    | english_diagnostics.h:65-71, |
//  |                            | are mutually exclusive per query, so a suggestion    | fixture schema + README §5 |
//  |                            | menu is at most 1 + 3 items                          |
//  | ranking                    | weight descending, then canonical text ascending by  | spelling_suggester.cc:226-237, |
//  |                            | byte value; case never re-ranks                      | prefix_completer.h:92-99 |
//  | label a front end can see  | suggestions carry the fixed comment "spell"; the     | english_diagnostics.h:47-53, |
//  |                            | original text and every completion carry none, so    | english_translator.cc:488-494 |
//  |                            | the two kinds ARE distinguishable through the C API  |
//  | case shapes                | lowercase / initial-capital / uppercase are rendered | spelling_suggester.cc:149-153, |
//  |                            | in the shape that was typed; anything else is        | english_translator.cc:500-512 |
//  |                            | refused as mixed-case and never guessed              |
//  | commit semantics           | unchanged from P2.5: a space with no explicit        | english_space_policy.h:88-118, |
//  |                            | selection commits the raw text plus one U+0020; a     | recovery space_tests.cc |
//  |                            | highlight is not a selection; a selection commits     |
//  |                            | the selected text plus one U+0020                     |
//  | feature off               | the schema names no `enable_suggestions`, so the      | english_translator.cc:291-301, |
//  |                            | suggestion path is never entered and every one of     | fixture README §7 |
//  |                            | the 19 `spelling_*` counters sums to 0                |
//  | no usable word list        | reported as `lexicon-unavailable`, never folded into  | spelling_suggester.h:35-36, |
//  |                            | `no-candidate`                                        | english_translator.cc:368-378 |
//
// EXPECTATIONS AND WHERE THEY COME FROM. Two independent routes, and both are asserted:
//   1. literal menus and comment columns, hand-derived from the fixture's 16 controlled
//      rows exactly as tests/fixtures/english_spelling/README.md section 4 documents them;
//   2. a reference in spelling_tests.cc that walks the SAME 16 rows in the REVERSE
//      direction -- "is this row exactly one admitted edit away from this input?" -- restates
//      the gate order above, and never calls the suggester, the completer, the case layer
//      or the token layer. The forward implementation generates variants and looks them up;
//      the reference compares a row against the input. Agreement between the two is
//      evidence, agreement between the engine and itself would not be.
// The reference is also checked against the documented "(weight,edit)" table, so a mistake
// in the reference is caught rather than inherited -- the incident's own patch table had a
// row whose state codes did not match its input length, which is exactly the class of error
// this triangle detects.
//
// WHAT THIS IS NOT. No automatic correction is claimed or tested: nothing here replaces
// what the user typed unless the user selected the suggestion. The weights are the fixture's
// hand-made numbers for making ranking assertable; they are not error probabilities.
#ifndef EPOCANVA_RECOVERY_SPELLING_TESTS_H_
#define EPOCANVA_RECOVERY_SPELLING_TESTS_H_

#include "recovery_test_support.h"

namespace epocanva {
namespace recovery {

// The gate table and the candidate list, per keystroke. Types every probe one character at
// a time, and for each frame reads the counters, the menu and the comment column back from
// the API: the named state the core reported, the number of suggestions that reached the
// screen, whether any variant was generated at all, and that no word list was re-read.
ExitCode RunSpellingMode(Harness& h);

}  // namespace recovery
}  // namespace epocanva

#endif  // EPOCANVA_RECOVERY_SPELLING_TESTS_H_
