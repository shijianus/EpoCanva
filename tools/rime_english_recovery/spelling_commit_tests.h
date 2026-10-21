// EpoCanva P2.6-recovery -- what a suggestion does to the commit boundary.
//
// P2.6 must not change P2.5: a space commits the composing text and then exactly one U+0020,
// a highlight is not an acceptance, and an explicit selection is committed once. What this
// mode adds is those same rules with a SUGGESTION on screen instead of a completion, because
// "the corrected word" and "the word that was typed" look identical in the commit string if
// nobody checks which one the engine was holding.
//
// Every preedit, candidate, comment and commit asserted here is read back from RimeApi. The
// transcript is built only from what get_commit() returned, so a space this program believes
// it typed cannot appear in it. Nothing synthesises a candidate and nothing appends a
// character to an observed string (AGENTS.md section 3).
#ifndef EPOCANVA_RECOVERY_SPELLING_COMMIT_TESTS_H_
#define EPOCANVA_RECOVERY_SPELLING_COMMIT_TESTS_H_

#include "recovery_test_support.h"

namespace epocanva {
namespace recovery {

// Suggestions on screen, then Return / Space / highlight / explicit selection, in all three
// accepted case shapes, across two sessions, and the ordinary word that follows a commit.
ExitCode RunSpellingCommitMode(Harness& h);

}  // namespace recovery
}  // namespace epocanva

#endif  // EPOCANVA_RECOVERY_SPELLING_COMMIT_TESTS_H_
