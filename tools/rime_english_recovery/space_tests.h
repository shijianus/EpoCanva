// EpoCanva P2.5-recovery -- what the space bar commits, and continuous multi-word input.
//
// THE THREE RULES, as decided by engine/languages/en/english_space_policy.h and carried
// out by engine/rime_components/english/english_space_processor.cc:
//   A  composing, no explicit selection pending -> commit the RAW composition string,
//      then one U+0020. Case A commits Context::input(), the same bytes
//      RimeApi::get_input() returns, NOT the preedit and NOT a highlighted candidate.
//   B  composing, an explicit selection pending  -> commit the confirmed composition
//      (the selected completion) exactly once, then one U+0020.
//   C  nothing composing                        -> one U+0020 and nothing else.
// A space press with Ctrl, Alt or Super held, and a key RELEASE, are not the user's space:
// the processor refuses them (kNoop) so the event continues down the chain.
//
// WHY THE COMMIT STRING LOOKS DOUBLED AND WHY COUNTS ARE NECESSARY. Session::OnCommit
// APPENDS (src/rime/service.cc:19,55-58), so one space hands the caller the single string
// "hel ". "The word was committed once and then one space" is therefore not readable from
// the text: it is settled by the component's own case counters and the commit-issued
// counters, which this mode asserts as identities.
//
// SCOPE OF THIS TEST. Keys enter through RimeApi::process_key() with librime's own
// modifier bits, and every asserted string comes from get_input()/get_context()/
// get_commit(). This is NOT a Windows TSF or host-application test: there is no IME
// context, no document and no physical keyboard, and nothing here is evidence about how a
// host consumes a commit. There is also no cross-commit editing here: the space policy
// never deletes or rewrites text that was already committed, and BackSpace on an empty
// composition is left to the host (asserted as refused, not as our own behaviour).
//
// The transcript this mode prints is the concatenation of the strings get_commit()
// actually returned -- the test never inserts a space or a word into it.
#ifndef EPOCANVA_RECOVERY_SPACE_TESTS_H_
#define EPOCANVA_RECOVERY_SPACE_TESTS_H_

#include "recovery_test_support.h"

namespace epocanva {
namespace recovery {

ExitCode RunSpaceCommitMode(Harness& h);

// Falsification control: it claims that a space after "hel" commits the top completion
// ("hello "), which case A forbids. It must exit 14. Without it, a space test that only
// looked at "some text was committed" could not be distinguished from one that accepted a
// highlighted candidate on the space bar.
ExitCode RunSpaceSelfCheckFailMode(Harness& h);

}  // namespace recovery
}  // namespace epocanva

#endif  // EPOCANVA_RECOVERY_SPACE_TESTS_H_
