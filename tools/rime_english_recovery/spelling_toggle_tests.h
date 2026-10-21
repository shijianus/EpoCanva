// EpoCanva P2.6-recovery -- the spelling feature switch, measured as a comparison.
//
// WHY THIS FILE EXISTS. tests/fixtures/english_spelling/README.md section 7 states the
// disabled half of the design: "the same word list with enable_suggestions removed gives
// exactly the P2.5 menu, and every spelling_* counter stays 0". Until now that sentence was
// carried by two DIFFERENT things: the english_completion fixture (a 54-row list, a different
// schema, and a switch that was simply never named) and the core unit test that calls the
// suggester with enabled=false. Neither is a controlled comparison. A reader could not point
// at one run where the same input over the same 16 rows produced suggestions on one side and
// nothing on the other, with the switch as the only difference between the two configurations.
// This file makes that a measured fact, and makes the failure modes visible:
//
//   * a schema whose FILENAME says "off" while its CONTENTS still say true -- caught by
//     reading the deployed config back through schema_open()/config_get_bool(), which is the
//     lookup Engine::ApplySchema() itself performs, not by re-reading the fixture;
//   * a "disabled" side that looks clean only because nothing ran at all -- caught by
//     requiring the module, the word list and the completion counters to move identically on
//     both sides;
//   * a derived twin that drifted from the source in some third line -- caught before the
//     engine is even started, by StageSpellingToggleFixture()'s two-line diff requirement.
//
// WHAT IS THE SAME ON BOTH SIDES, AND HOW THAT IS PROVED. The two schemas resolve the same
// english/lexicon_path string, the same english/max_completions, the same english/
// max_suggestions and the same menu/page_size, all read back from the deployed files. The
// staged word list is one file, copied once, and both schemas name it. Derived text is
// checked line by line: exactly two lines differ from the source -- the schema identity and
// the switch.
//
// WHAT IS NOT CLAIMED. This is not a performance trial and it reads no generated pilot data.
// It does not re-derive the per-frame gate table (spelling_tests.cc owns that, and duplicating
// it here would give two files room to disagree). It asserts the ON side's aggregate
// suggestion counts and the OFF side's per-field zeros, and the identity of everything that
// is not spelling. No automatic correction is tested or implied: on both sides the default
// commit -- Return, and the space bar -- is what the user typed, with no acceptance action in
// between. The explicit-acceptance boundary belongs to spelling_commit_tests.cc, and the two
// are not interchangeable: highlighting a row and pressing Return DOES commit the highlighted
// row, because Context::Commit() asks the composition for its selected candidate
// (src/rime/engine.cc:251-257 -> src/rime/composition.cc:102-109). "A highlight is not an
// acceptance" is a rule about EpoCanva's space processor, not about Return.
#ifndef EPOCANVA_RECOVERY_SPELLING_TOGGLE_TESTS_H_
#define EPOCANVA_RECOVERY_SPELLING_TOGGLE_TESTS_H_

#include "recovery_test_support.h"

namespace epocanva {
namespace recovery {

// Enabled vs disabled, one process, the same hand-made 16-row list, the same keystrokes.
// Verifies the deployed configuration of both schemas, then compares their menus frame by
// frame and their counters phase by phase, then checks the commit boundary on both sides.
ExitCode RunSpellingToggleAbMode(Harness& h);

// The control that keeps the comparison honest: the same two-sided setup, the same input, and
// one deliberately false claim -- that the two sides produced the same menu. If the switch
// ever stops making a difference, this is the case that says so by FAILING; a comparison whose
// wrong-claim case could pass is a comparison that is not looking at anything.
ExitCode RunSpellingToggleSelfCheckFailMode(Harness& h);

}  // namespace recovery
}  // namespace epocanva

#endif  // EPOCANVA_RECOVERY_SPELLING_TOGGLE_TESTS_H_
