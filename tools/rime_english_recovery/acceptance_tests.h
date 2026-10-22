// EpoCanva P3.1 -- the frozen acceptance semantics of Return and Space.
//
// THE FIVE RULES this mode freezes, each against the live engine through the public C API
// and none of them implemented by this test:
//   1. A highlight alone commits nothing. highlight_candidate() moves
//      menu.highlighted_candidate_index and the composition stays exactly what was typed;
//      the commit queue reads empty both through the event's own observation and through a
//      second get_commit().
//   2. With the highlight still at its default index 0 -- which is the raw input itself,
//      because the translator offers the typed text as candidate 0 -- Return commits that
//      raw input, with no space.
//   3. After a real highlight move onto a completion, Return accepts the CURRENT highlighted
//      row: the committed bytes are that row's text, read back off the menu before the key.
//   4. After a highlight move ONLY (no selection), Space still follows the P2.5 raw-input
//      policy: the raw token plus exactly one U+0020, counted as case A. This is the same
//      rule recovery.space.commit_and_multiword pins; it is re-asserted here so the frozen
//      contract names both keys in one place.
//   5. After an explicit select_candidate(), Return and Space keep their existing behaviour:
//      Return commits the selected text with no space, Space commits it plus one U+0020
//      (case B). Neither key is redefined here; whatever the engine does today is what is
//      frozen, and the expectations were derived from the pinned sources, not from this
//      test's convenience:
//        fluid_editor binds XK_Return to Editor::CommitComposition
//        (third_party/librime/src/rime/gear/editor.cc:176), which runs
//        Context::ConfirmCurrentSelection() -- it sets Segment::status to kSelected and
//        reads the candidate at Segment::selected_index WITHOUT consulting the status
//        (src/rime/segmentation.cc:51-53), i.e. exactly the highlight -- and then, the
//        segment having been closed by the select notifier, Context::Commit().
//        Space is intercepted first by the EpoCanva space processor
//        (engine/rime_components/english/english_space_processor.cc), which decides from
//        the segment status, so a highlight never reaches it as an acceptance.
//
// WHY A SEPARATE MODE. recovery.spelling.commit_and_selection already covers default Return
// and selection-Return on the spelling fixture; recovery.space.commit_and_multiword covers
// highlight-Space on this one. What nothing pinned before this file is the pair "highlight
// then Return accepts the highlighted row" and "default Return commits the raw input" on
// the completion fixture, i.e. the two Return answers that make the difference between a
// highlight and a selection observable through the Enter key.
//
// WHAT THIS IS NOT. Not a UI test: there is no candidate window, no focus and no host
// editor here, and "highlight" means RimeApi::highlight_candidate() -- the API whose own
// contract reads "highlight a selection without committing"
// (third_party/librime/src/rime_api.h:501-502) -- never select_candidate(), which sets
// Segment::status and is a different fact.
#ifndef EPOCANVA_RECOVERY_ACCEPTANCE_TESTS_H_
#define EPOCANVA_RECOVERY_ACCEPTANCE_TESTS_H_

#include "recovery_test_support.h"

namespace epocanva {
namespace recovery {

ExitCode RunAcceptanceSemanticsMode(Harness& h);

// Falsification control: it claims that Return after a highlight commits the raw input
// ("hel"), which rule 3 forbids -- the engine commits the highlighted row ("help"). It
// must exit 14. Without it, a Return test that only checked "something was committed"
// could not be distinguished from one that ignored the highlight.
ExitCode RunAcceptanceSelfCheckFailMode(Harness& h);

}  // namespace recovery
}  // namespace epocanva

#endif  // EPOCANVA_RECOVERY_ACCEPTANCE_TESTS_H_
