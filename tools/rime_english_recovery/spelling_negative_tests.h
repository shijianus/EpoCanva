// EpoCanva P2.6-recovery -- the spelling group's load-failure path and its falsification
// controls.
//
// WHY A SEPARATE FILE. The two controls and the unavailable-word-list case assert different
// kinds of thing from the gate table: one is a state that must never be folded into "no
// suggestions", the others exist to prove the group can go red. Keeping them here also keeps
// spelling_tests.cc free of deliberate mistakes, so nothing that is meant to fail can be
// mistaken for an expectation of the product.
#ifndef EPOCANVA_RECOVERY_SPELLING_NEGATIVE_TESTS_H_
#define EPOCANVA_RECOVERY_SPELLING_NEGATIVE_TESTS_H_

#include "recovery_test_support.h"

namespace epocanva {
namespace recovery {

// The schema asked for suggestions and there is no usable word list. That is ONE named state,
// and it must not arrive as "nothing you typed is misspelled": the menu still offers the typed
// text, so only the counters and the load status separate the two. Run against the schema this
// program generated during staging -- the surviving original with exactly two indented keys
// rewritten, pointing at a three-row list whose third row repeats its first.
ExitCode RunSpellingLexiconUnavailableMode(Harness& h);

// Control 1 for the group: the right three suggestions in the wrong order. A menu check that
// compared only the SET would keep passing here, which is the degradation this catches.
ExitCode RunSpellingSuggestionRankingSelfCheckFailMode(Harness& h);

// Control 2 for the group: claims that the completion "heli -> helium" carries the suggestion
// label. If labels ever stopped separating a suggestion from a completion, every other case in
// the group would lose the only distinction a front end can see -- so this claim must fail.
ExitCode RunSpellingLabelSelfCheckFailMode(Harness& h);

}  // namespace recovery
}  // namespace epocanva

#endif  // EPOCANVA_RECOVERY_SPELLING_NEGATIVE_TESTS_H_
