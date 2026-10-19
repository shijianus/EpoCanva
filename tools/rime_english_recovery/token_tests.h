// EpoCanva P2.3-recovery -- the limited word form and the technical-text protection.
//
// WHAT THIS GROUP PROVES. A prefix that ends in one ASCII delimiter (' or -) is still
// completable, in all three case shapes. A token that contains a digit or one of the
// eleven technical marks is handed back whole and is never completed, never trimmed and
// never split. A delimiter pattern outside the limited grammar (two delimiters, a leading
// one, both kinds at once) is refused as its OWN reason, not folded into "no match" and
// not folded into the byte-class refusal. The whole token reaches the translator: the
// protecting evidence is that queries_covering_part_of_input never moves, because a
// completion that appeared only after the engine cut the token off would still leave the
// final frame looking correct.
//
// UNITS, per engine/languages/en/english_token.h: stop_offset and every length here is a
// UTF-8 code-unit index into the raw input, which for the ASCII bytes this group types
// equals the byte index -- it is not a grapheme position and not a screen column. The
// frame count a protected token produces is therefore (length - stop_offset): the frames
// whose length is strictly greater than the stop byte's index. All inputs in this file are
// ASCII, so nothing here is evidence about non-ASCII text; the core reports non-ASCII as
// its own class and the engine path for those bytes is not reachable through the fixture's
// speller/alphabet.
//
// EXPECTATIONS come from tests/fixtures/english_completion/lexicon.tsv (row weights) and
// tests/fixtures/english_completion/README.md section 5.2 (the hand-derived frame
// arithmetic), not from a run and not from the functions under test.
#ifndef EPOCANVA_RECOVERY_TOKEN_TESTS_H_
#define EPOCANVA_RECOVERY_TOKEN_TESTS_H_

#include "recovery_test_support.h"

namespace epocanva {
namespace recovery {

ExitCode RunTokenProtectionMode(Harness& h);

// Falsification control for this group. It asserts the exact corruption P2.3 forbids: the
// letters of "hel@" being completed while the user is typing a technical mark. The engine
// must refuse it, so this case must exit 14 -- which is what proves the suite is checking
// candidate CONTENT and token SCOPE, and not merely the final committed string.
ExitCode RunTokenSelfCheckFailMode(Harness& h);

}  // namespace recovery
}  // namespace epocanva

#endif  // EPOCANVA_RECOVERY_TOKEN_TESTS_H_
