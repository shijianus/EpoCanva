// EpoCanva P2.5 -- implementation of the space policy. See the header for the
// rules and for why each answer is named instead of being a bool.
//
// This file performs no I/O, allocates nothing, stores nothing and includes
// nothing from librime or from the platform.
#include "english_space_policy.h"

namespace epocanva {
namespace en {

SpaceKeyEvent ClassifySpaceKey(int keysym, unsigned mask) {
  // Judged on the keysym first: a key that is not the space bar is not this
  // policy's business whatever the modifier state, and letters must keep reaching
  // the speller untouched.
  if (keysym != kSpaceKeysym) {
    return SpaceKeyEvent::kNotSpace;
  }
  // Release before modifier: "the space bar came up" is the more specific reason
  // this event is not a press, and both answers refuse the event anyway.
  if ((mask & kReleaseMaskBit) != 0u) {
    return SpaceKeyEvent::kKeyRelease;
  }
  if ((mask & kSpaceBlockingMask) != 0u) {
    return SpaceKeyEvent::kBlockedByModifier;
  }
  // shift and lock are deliberately ignored here: shift+space is still a space.
  return SpaceKeyEvent::kPlainSpace;
}

SpaceCase DecideSpaceCase(bool composing, bool selection_pending) {
  if (!composing) {
    // C -- and also the answer for the impossible (not composing, selection
    // pending) pair: with no composition there is nothing that could be committed
    // twice, so refusing to invent one is the safe reading.
    return SpaceCase::kSpaceOnly;
  }
  return selection_pending ? SpaceCase::kCommitSelectionThenSpace
                           : SpaceCase::kCommitTokenThenSpace;
}

bool CaseCommitsComposition(SpaceCase selected_case) {
  return selected_case == SpaceCase::kCommitTokenThenSpace ||
         selected_case == SpaceCase::kCommitSelectionThenSpace;
}

bool CaseEmitsSpace(SpaceCase selected_case) {
  // Every case this policy reaches ends in exactly one U+0020: that is the whole
  // point of the space bar. kNotApplicable is the only case that emits nothing.
  return selected_case != SpaceCase::kNotApplicable;
}

}  // namespace en
}  // namespace epocanva
