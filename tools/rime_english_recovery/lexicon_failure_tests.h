// EpoCanva P2.1-recovery -- the word-list LOAD failure paths.
//
// WHY THIS GROUP EXISTS. A prefix completer can fail in a way that looks exactly like
// success: when the word list is missing or malformed, the component still hands back the
// text the user typed, so a test that only reads the candidate list sees "one candidate,
// equal to my input" in five different situations -- working completion of a word with no
// completions, a load that never found a file, a file that stopped parsing at a bad row, a
// schema that configured no path, and (on the baseline engine) no component at all. The
// acceptance here is therefore always the SAME triple: the candidate list, the load
// counters, and the load status NAME.
//
// WHAT IS INJECTED, AND HOW. Each mode stages a different word list at the path the schema
// names -- inside its own run directory -- and never edits tests/fixtures/ (which is why no
// run can "forget to restore" a fixture). One case removes the staged file entirely, which
// is the missing-file condition; the other three copy a fixture whose third row is
// respectively a repeated word, a byte the word alphabet never accepts, and a shape no
// dictionary entry may have. Every case runs in its own process, so the
// process-lifetime word-list cache cannot carry a previous case's result into it.
//
// THE STATUS STRINGS AND LINE NUMBERS come from the parser as it stands
// (engine/languages/en/english_lexicon.cc): LexiconStatusName returns "file-not-found",
// "duplicate-word", "illegal-word" and "illegal-word-form"; FromFile reports line 0 for a
// missing file; the sequential per-row checks report the 1-based line of the offending row;
// and the duplicate check runs AFTER the index sort, reporting the line of one member of
// the duplicate pair -- which is why the duplicate case below asserts membership in the set
// of lines that actually hold that word in the fixture, rather than one number the parser
// does not promise.
//
// UNITS (AGENTS.md section 5): error lines are 1-based source line numbers in the word-list
// file; entry counts are rows; every counter read here is an event count.
#ifndef EPOCANVA_RECOVERY_LEXICON_FAILURE_TESTS_H_
#define EPOCANVA_RECOVERY_LEXICON_FAILURE_TESTS_H_

#include <vector>

#include "recovery_test_support.h"

namespace epocanva {
namespace recovery {

// One load failure, as it must be observed. `acceptable_error_lines` carries the 1-based
// lines this fixture can legitimately report: one entry for the row-scanning failures, and
// two for the duplicate case (the duplicate row and the row it duplicates). It is never
// empty, and a single-element vector is an exact equality claim.
struct LoadFailureExpectation {
  const char* label;
  const char* fixture_lexicon;  // nullptr = the staged word list must not exist at all
  const char* status_name;
  std::vector<long long> acceptable_error_lines;
};

// missing-lexicon
ExitCode RunMissingLexiconMode(Harness& h);
// duplicate-entry: lexicon_bad_duplicate.tsv
ExitCode RunBadLexiconDuplicateMode(Harness& h);
// illegal-word: lexicon_bad_illegal.tsv (a byte the word alphabet never contains)
ExitCode RunBadLexiconIllegalMode(Harness& h);
// illegal-word-form: lexicon_bad_illegal_form.tsv (legal bytes, illegal shape)
ExitCode RunBadLexiconIllegalFormMode(Harness& h);

// The control that keeps this group honest: it runs the same verification path against the
// GOOD 54-row fixture while claiming a load failure. The load succeeds, so the diagnostics
// disagree and the run must end in an assertion failure (exit 14) with no success marker.
// A green completion menu alongside that failure is the point: it shows the case failed on
// the load claim, not on a broken engine, a wrong DLL or a bad argument.
ExitCode RunLexiconFailureSelfCheckFailMode(Harness& h);

}  // namespace recovery
}  // namespace epocanva

#endif  // EPOCANVA_RECOVERY_LEXICON_FAILURE_TESTS_H_
