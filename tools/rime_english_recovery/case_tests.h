// EpoCanva P2.2-recovery -- the ASCII letter-case regression: original text preserved,
// completion rendered in the shape the user typed.
//
// SCOPE, stated so it is not over-read: this is the 52-letter ASCII mapping with four
// recognised shapes (lowercase / initial-capital / uppercase / mixed). Passing it is
// NOT evidence of Unicode case folding, title-casing, locale-aware casing, sentence
// capitalisation or proper-noun case recovery -- none of which exist
// (engine/languages/en/english_case.h header comment).
//
// HOW THE EVENTS ARE SENT, and what that does and does not prove: every key here is a
// (keysym, mask) pair handed to RimeApi::process_key(). Uppercase is sent as the
// uppercase keysym ('H' = 0x48), because Speller::ProcessKeyEvent() reads only
// key_event.keycode() (third_party/librime/src/rime/gear/speller.cc:104) and consults
// shift() for the space key and nowhere else (same file, :107). The mode therefore also
// sends shift + 'h' (0x68 with mask 1) and asserts the composition came out lowercase:
// the shape of the event this test relies on is an observed fact, not an assumption.
// This is an API-level key-event test, NOT a Windows keyboard-adapter or TSF test -- no
// host application, no IME context and no physical keyboard is involved.
//
// EXPECTATIONS are derived from tests/fixtures/english_completion/lexicon.tsv and the
// classification order in engine/languages/en/english_case.h, written down before the
// engine was consulted; neither the completer nor the case function produced them.
#ifndef EPOCANVA_RECOVERY_CASE_TESTS_H_
#define EPOCANVA_RECOVERY_CASE_TESTS_H_

#include "recovery_test_support.h"

namespace epocanva {
namespace recovery {

ExitCode RunCasePreservationMode(Harness& h);

// Falsification control for this group: it asserts a menu whose completions are in the
// right SET but the wrong ORDER (the fixture weights put Hello before Help). If the
// suite ever degrades to "did the right words appear somehow", this case is the one that
// stops passing. Must exit 14 without printing the success marker.
ExitCode RunCaseSelfCheckFailMode(Harness& h);

}  // namespace recovery
}  // namespace epocanva

#endif  // EPOCANVA_RECOVERY_CASE_TESTS_H_
