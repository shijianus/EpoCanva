// EpoCanva P3.3 -- what one keyboard event means, as a pure function.
//
// THIS FILE DECIDES, IT DOES NOT ACT. DecideKey takes an immutable event, an immutable
// translation result and an immutable context, and returns a value. It does not read the
// global keyboard state, read a file, call an engine, send a window message, insert text,
// start or end a composition, or remember anything between calls. Two calls with equal inputs
// return equal outputs; a test in tests/windows/input_decision asserts exactly that.
//
// "ENGINE CANDIDATE" IS NOT "HANDLED" (P3.3 brief section 4). The kEngine* dispositions mean
// "if an engine session existed, this is the event that may be offered to it". They do not
// mean the key was consumed, that pfEaten was set, that text was inserted, or that a
// composition changed. This round has no sink, no engine and no host-text edit, and there is
// deliberately no `key_eaten` field anywhere in this header for a later reader to misread.
// Whether an event is actually handed to the engine, and whether the host is told it was
// eaten, are later rounds' decisions.
//
// THE ACCEPTED CHARACTER SET IS THE CURRENT ENGLISH SCHEMA'S ALPHABET, not "printable
// ASCII". The schema at tests/fixtures/english_completion/epocanva_test_english.schema.yaml
// lists:
//     abcdefghijklmnopqrstuvwxyz
//     ABCDEFGHIJKLMNOPQRSTUVWXYZ
//     0123456789
//     ' @ . _ : / \ ? & = # % + -
// Digits and technical marks are forwarded, NOT dropped: P2.3's protection rule needs them
// inside the composition so the token layer can refuse to complete a credential, an e-mail
// address or a version number as a whole. This layer must not restrict the composition to
// characters that can produce completions. Everything else printable ASCII (space from a
// non-space key, comma, semicolon, brackets, and so on) is unsupported here and is reported
// as such; with a composition active it becomes an explicit boundary-coordination demand
// rather than a silent pass-through.
//
// MODIFIER POLICY, deliberately narrow:
//   Control, Alt or Windows held (including the Control+Alt shape of AltGr)
//       -> kShortcutPassThrough. AltGr is NOT supported as text this round.
//   Shift on a letter/digit/mark
//       -> allowed; translation decides the character ('A' vs 'a').
//   Shift on Space, Enter, BackSpace or Escape
//       -> kUnsupportedModifierCombination. This round does not define Shift+Space or
//          Shift+Enter product rules, so it must not invent one.
//   Caps Lock
//       -> never a blocker. It only affects the translated character.
//
// SPECIAL KEYS. Only the four unmodified keys the brief names are classified:
//   Space      -> engine candidate. P2.5 already defines "commit what is composing, then
//                 exactly one U+0020", including the nothing-composing case.
//   Enter      -> engine candidate only while composing; otherwise the host keeps its
//                 newline semantics, because this round has no commit/newline rule.
//   Escape     -> engine candidate only while composing; otherwise the host keeps Escape.
//   BackSpace  -> engine candidate only while composing. Without a composition it is never
//                 offered to the engine (P3.3 brief section 5), so the host keeps deletion.
//
// DEAD KEYS. A negative ToUnicodeEx return never becomes text. This layer does not use the
// spacing character the API may have written into the buffer. Without a composition the event
// passes to the host; with a composition it is an unresolved boundary and is reported as
// kCompositionBoundaryRequired. Nothing here clears a dead key or loops the translation API.
//
// PRECEDENCE. DecideKey checks in this order, and the order is part of the contract:
//   1. key-up                         -> pass-through, kKeyRelease
//   2. permission == kForbidden       -> pass-through, kPermissionForbidden
//   3. permission == kUnknown         -> pass-through, kPermissionUnknown
//   4. input disabled                 -> pass-through, kInputDisabled
//   5. Control/Alt/Windows held       -> kShortcutPassThrough, kShortcutModifier
//   6. engine not ready               -> pass-through, kEngineNotReady
//   7. one of the four virtual keys   -> the special-key rules above
//   8. the translation result         -> text candidate, pass-through or boundary demand
// Permission is checked before the shortcut because a privacy refusal is the more specific
// fact about the field; both end in pass-through. Key-up is checked first because it is a
// structural fact, not a policy choice. A key-up is never translated into the English path.
#ifndef EPOCANVA_WINDOWS_INPUT_KEY_DECISION_H_
#define EPOCANVA_WINDOWS_INPUT_KEY_DECISION_H_

#include <cstdint>

#include "key_event.h"

namespace epocanva {
namespace windows {
namespace input {

// What the event should be offered to. There is intentionally no "consume" or "handled"
// value: this round cannot promise either.
enum class KeyDisposition {
  kPassThroughToHost = 0,      // the host keeps the key; this layer does nothing
  kShortcutPassThrough,        // a Control/Alt/Windows chord; OS or host owns it
  kEngineTextCandidate,        // one accepted ASCII scalar; may be offered to the engine
  kEngineSpaceCandidate,       // unmodified Space; P2.5's policy would receive it
  kEngineEnterCandidate,       // unmodified Enter while composing
  kEngineBackSpaceCandidate,   // unmodified BackSpace while composing
  kEngineEscapeCandidate,      // unmodified Escape while composing
  // A composition exists and something arrived that this round cannot place inside or
  // around it (unsupported printable text, a dead key, or a malformed result). The explicit
  // demand is that a later round decides the commit/insert order; no claim is made that a
  // pass-through would be correct.
  kCompositionBoundaryRequired,
};

// Why the disposition was chosen. kNone accompanies a successful text candidate; the other
// values name one fact, never a rejected character.
enum class KeyDecisionReason {
  kNone = 0,
  kKeyRelease,                    // key-up event
  kInputDisabled,                 // service disabled for this context
  kPermissionForbidden,           // privacy-classified field
  kPermissionUnknown,             // field could not be classified
  kEngineNotReady,                // no engine session
  kShortcutModifier,              // Control/Alt/Windows (AltGr included) held
  kUnsupportedModifierCombination,// Shift held with Space/Enter/BackSpace/Escape
  kNoComposition,                 // Enter/Escape with nothing composing
  kBackSpaceWithoutComposition,   // BackSpace with nothing composing
  kNoText,                        // translation returned 0
  kDeadKey,                       // translation returned < 0
  kTranslationFailed,             // adapter refused or failed the translation
  kMultipleUtf16CodeUnits,        // > 1 code unit: rejected whole, not truncated
  kNonAsciiCharacter,             // a single code unit >= U+0080
  kUnsupportedCharacter,          // ASCII outside the schema alphabet
};

// The class of an accepted ASCII scalar. Delimiters and protected marks are distinct because
// the English token layer treats them differently; both are forwarded.
enum class TextClass {
  kNone = 0,
  kAsciiLetter,     // A-Z or a-z
  kAsciiDigit,      // 0-9
  kTokenDelimiter,  // ' or - (the two bytes english_token.h calls delimiters)
  kProtectedMark,   // @ . _ : / \ ? & = # % + (P2.3's technical marks)
};

// The answer. `ascii_code_point` is meaningful only for kEngineTextCandidate and carries the
// accepted scalar value, not a byte offset. It is part of the decision because a later engine
// adapter must append exactly this character; it must never be written to a log.
struct KeyDecision {
  KeyDisposition disposition = KeyDisposition::kPassThroughToHost;
  KeyDecisionReason reason = KeyDecisionReason::kNone;
  TextClass text_class = TextClass::kNone;
  std::uint32_t ascii_code_point = 0;

  // True for the five kEngine* dispositions. This says "may be offered", never "handled".
  bool is_engine_candidate() const;
  // True exactly when disposition == kCompositionBoundaryRequired.
  bool requires_composition_boundary() const;
};

// The pure decision. `event` and `text` are read, never modified; `context` is read. The
// function allocates nothing beyond the returned value and has no other effect.
KeyDecision DecideKey(const KeyEventSnapshot& event, const KeyText& text,
                      const InputContext& context);

// The exact schema alphabet, as one ASCII string. Kept here so a test can enumerate it and
// compare against a hand-written list; it is not used as a lookup table.
extern const char kEnglishSchemaAlphabet[];

// Classify one Unicode scalar value. Returns TextClass::kNone for anything outside the
// accepted set (including non-ASCII, controls and space).
TextClass ClassifyForwardableAscii(std::uint32_t ascii_code_point);
bool IsForwardableAscii(std::uint32_t ascii_code_point);

// Stable ASCII names for logs. They never contain input text.
const char* DescribeKeyDisposition(KeyDisposition disposition);
const char* DescribeKeyDecisionReason(KeyDecisionReason reason);
const char* DescribeTextClass(TextClass text_class);

// Value comparison, used by the idempotence tests and by any later adapter that wants to
// assert it produced the same decision twice.
bool operator==(const KeyDecision& left, const KeyDecision& right);
bool operator!=(const KeyDecision& left, const KeyDecision& right);

}  // namespace input
}  // namespace windows
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_INPUT_KEY_DECISION_H_
