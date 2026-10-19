// EpoCanva P2.6-recovery -- the rebuilt English prefix-completion regression.
//
// Every expectation in completion_tests.cc was derived from two surviving sources and
// from nothing else:
//   1. tests/fixtures/english_completion/lexicon.tsv, whose 54 rows are listed below
//      with the weights the file itself carries;
//   2. the ordering and budget contract in docs/testing/rime-english-completion.md
//      section 1 (candidate 0 is always the typed text; completions follow in weight
//      descending order, ties broken by ascending ASCII; a row equal to the typed text
//      is not offered twice; at most EPOCANVA_ENGLISH_MAX_COMPLETIONS_DEFAULT follow).
// The code under test was not consulted for any expectation, so a completer that
// changed its order or its dedup would disagree with this file rather than agree with
// itself.
#ifndef EPOCANVA_RECOVERY_COMPLETION_TESTS_H_
#define EPOCANVA_RECOVERY_COMPLETION_TESTS_H_

#include "recovery_test_support.h"

namespace epocanva {
namespace recovery {

// The golden path: real keystrokes through process_key(), the real menu, the real
// commit queue, two sessions, and the component's own counters.
ExitCode RunCompletionMode(Harness& h);

// Negative mode, run against the unmodified baseline engine: the module must be absent,
// no candidate may appear at all, and yet the composition and the commit must still
// work. 0 candidates rather than 1 is the only thing that separates "this component is
// not in the engine" from "it is in the engine and its word list is broken", because the
// fixture schema lists no other translator.
ExitCode RunExpectModuleAbsentMode(Harness& h);

// A test that cannot fail is not a test (AGENTS.md section 3). This mode types a real
// composition and then asserts a candidate list that the fixture provably does not
// produce, so the program must exit with an assertion failure and must not print its
// success marker. Registered as a CTest case that expects that failure.
ExitCode RunSelfCheckFailMode(Harness& h);

}  // namespace recovery
}  // namespace epocanva

#endif  // EPOCANVA_RECOVERY_COMPLETION_TESTS_H_
