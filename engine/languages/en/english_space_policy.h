// EpoCanva P2.5 -- what the space bar means when an English composition is on
// screen, expressed as decisions rather than as side effects.
//
// LAYER: language core. No librime header, no platform header, no Context, no
// Session, no history, no network, no writes, no logging. That is what makes the
// rules below unit-testable in the top-level Debug project, where the engine's
// whole key-handling stack is absent.
//
// WHY A POLICY FILE AT ALL. The engine adapter
// (engine/rime_components/english/english_space_processor.cc) has to answer three
// questions on every key event, and each of them is easy to get subtly wrong:
//
//   1. Is this event even a space the user means as "write a space"?
//      Ctrl+Space and Alt+Space are somebody else's hotkey; a key RELEASE is not a
//      key press at all. Accepting those would swallow system bindings.
//   2. Is there anything to commit, and was it committed already?
//      librime's own space binding (FluidEditor, src/rime/gear/editor.cc:191 ->
//      Editor::Confirm at :87-90) commits the composition but never produces a
//      space character. P2.5 wants "word, then exactly one U+0020", and wants the
//      word committed ONCE -- so the two facts "composing" and "an explicit
//      selection is pending" have to be named, not guessed.
//   3. Which of the two does this case select?
//      Kept here as data (CaseCommitsComposition / CaseEmitsSpace) so the adapter
//      contains no policy branches and the mapping itself can be tested.
//
// WHAT THIS IS NOT: a segmenter. There is no word-boundary detection, no
// dictionary-driven splitting and no re-flowing of already committed text. The
// space is a commit trigger with a literal output, which is precisely the range
// the P2.5 brief sets (no correction, no prediction, no learning).
//
// PRIVACY (AGENTS.md section 2): no text crosses into this layer. The inputs are a
// keysym, a modifier mask, and two booleans derived from engine state. Nothing is
// stored, so there is nothing to leak and no way to "remember the previous word".
//
// UNITS (AGENTS.md section 5): `keysym` is a 32-bit X11 key symbol, `mask` is a
// bitmask of the modifier bits below -- never a key code and never a character
// count. The space this policy ultimately emits is one UTF-8 code unit (U+0020,
// 0x20), the same byte as its keysym because printable-ASCII keysyms equal code
// points (include/X11/keysymdef.h:528-601).
#ifndef EPOCANVA_EN_SPACE_POLICY_H_
#define EPOCANVA_EN_SPACE_POLICY_H_

namespace epocanva {
namespace en {

// XK_space. Same numeric value as the byte it emits, for the reason above; the
// two are still different things (an event filter and a text payload), so they are
// declared separately below.
constexpr int kSpaceKeysym = 0x0020;

// The one character this policy is allowed to emit by itself: U+0020, one UTF-8
// code unit. No other punctuation is produced here -- there is no automatic
// punctuation, no smart quote and no spacing insertion.
constexpr char kSpaceCharacter = '\x20';

// Modifier bit values, transcribed from third_party/librime/src/rime/key_table.h:
// 14-38. Transcribed rather than included because this header must compile inside
// the root project too; if upstream ever changes a bit, the core unit test that
// pins these numbers fails, which is the desired outcome -- a silent drift is not.
constexpr unsigned kShiftMaskBit = 1u << 0;   // kShiftMask
constexpr unsigned kLockMaskBit = 1u << 1;    // kLockMask
constexpr unsigned kControlMaskBit = 1u << 2;  // kControlMask
constexpr unsigned kAltMaskBit = 1u << 3;      // kAltMask == kMod1Mask
constexpr unsigned kSuperMaskBit = 1u << 26;   // kSuperMask
constexpr unsigned kReleaseMaskBit = 1u << 30;  // kReleaseMask

// Bits that make a space somebody else's key combination. Shift and Lock are NOT
// in this set: shift+space still types a space, and caps-lock state must not
// change what the space bar commits.
constexpr unsigned kSpaceBlockingMask =
    kControlMaskBit | kAltMaskBit | kSuperMaskBit;

// What kind of space event arrived. Four named answers because the observable
// behaviour differs for each, and "it was not a space" must not be conflated with
// "it was a space we refused".
enum class SpaceKeyEvent {
  kNotSpace = 0,        // a different key: this policy has nothing to say
  kKeyRelease,         // the space bar going up -- not a press
  kBlockedByModifier,  // ctrl / alt / super held: leave it to the host or the
                       // front end's own bindings
  kPlainSpace,         // the space bar, with at most shift and/or lock
};

// Pure classification of one key event. `keysym` and `mask` are exactly what
// rime::KeyEvent reports (key_event.h:29-34 reads the same bits).
SpaceKeyEvent ClassifySpaceKey(int keysym, unsigned mask);

// The case the space bar resolves to, once it is known to be a plain space press.
// Named per the P2.5 brief's rules A, B and C, and split into A and B even though
// both commit: "nothing was explicitly selected" and "a selection is waiting to be
// committed" are different facts about what gets committed -- the raw token versus
// the chosen completion -- and a test has to be able to tell them apart.
enum class SpaceCase {
  kNotApplicable = 0,  // not a plain space press
  kCommitTokenThenSpace,  // A: composing, no explicit selection pending
  kCommitSelectionThenSpace,  // B: composing, an explicit selection is pending
  kSpaceOnly,  // C: nothing composing
};

// `composing` is Context::IsComposing() at the moment of the event
// (src/rime/context.h:35; it is true when the raw input or the composition is
// non-empty -- src/rime/context.cc:48-50).
// `selection_pending` means "some segment of the current composition carries a
// status of Segment::kSelected or higher", i.e. RimeApi::select_candidate() has
// confirmed a candidate that has not been committed yet. It is NOT a remembered
// copy of a previous word: it is re-derived from live engine state per event.
//
// The state combination (not composing, selection pending) cannot occur -- a
// selection lives in the composition, and committing it clears the composition.
// It is answered anyway (kSpaceOnly) so that a future caller that misreads the
// engine state cannot get an undefined answer, and a core test pins that answer.
SpaceCase DecideSpaceCase(bool composing, bool selection_pending);

// The two actions a case selects, as data. The engine adapter calls these instead
// of re-implementing the table, so the mapping is unit-testable and the adapter has
// no policy branch of its own.
bool CaseCommitsComposition(SpaceCase selected_case);
bool CaseEmitsSpace(SpaceCase selected_case);

}  // namespace en
}  // namespace epocanva

#endif  // EPOCANVA_EN_SPACE_POLICY_H_
