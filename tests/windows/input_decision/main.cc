// EpoCanva P3.3 -- the pure decision verification program.
//
// WHAT IT JUDGES. Only DecideKey and the character table: no window station, no message
// queue, no keyboard, no engine. Every expectation is hand-written below; none is produced
// by running the implementation and copying its answer. The Windows API adapter has its own
// separate program (tests/windows/keyboard_translation) and its own named CTest cases, so a
// green run here is never reported as evidence about a real keyboard layout.
//
// MODES
//   decisions        the whole pure table, the precedence rules and the idempotence checks
//   self-check-fail  a deliberately false claim, must exit 14 with no RESULT PASS
//
// EXIT CODES: 0 pass; 2 usage; 5 a check failed; 14 the self-check-fail mode's expected
// failure. RESULT PASS on stdout is the CTest marker and is printed only when nothing failed.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "key_decision.h"

namespace {

using epocanva::windows::input::DecideKey;
using epocanva::windows::input::DescribeKeyDecisionReason;
using epocanva::windows::input::DescribeKeyDisposition;
using epocanva::windows::input::DescribeKeyTextStatus;
using epocanva::windows::input::DescribeTextClass;
using epocanva::windows::input::InputContext;
using epocanva::windows::input::InputPermission;
using epocanva::windows::input::KeyDecision;
using epocanva::windows::input::KeyDecisionReason;
using epocanva::windows::input::KeyDisposition;
using epocanva::windows::input::KeyEventSnapshot;
using epocanva::windows::input::KeyText;
using epocanva::windows::input::KeyTextStatus;
using epocanva::windows::input::KeyTransition;
using epocanva::windows::input::ModifierSnapshot;
using epocanva::windows::input::TextClass;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitCheckFailed = 5;
constexpr int kExitSelfCheckFailed = 14;

int g_total = 0;
int g_failed = 0;

void Check(bool condition, const std::string& what, const std::string& got) {
  ++g_total;
  if (condition) {
    std::printf("[input-pure] ok   %s\n", what.c_str());
  } else {
    ++g_failed;
    std::printf("[input-pure] FAIL %s  (got: %s)\n", what.c_str(), got.c_str());
  }
}

int FinishMode() {
  if (g_failed != 0) {
    std::printf("[input-pure] RESULT FAIL (%d checks, %d failed)\n", g_total, g_failed);
    return kExitCheckFailed;
  }
  std::printf("[input-pure] RESULT PASS (%d checks, %d failed)\n", g_total, g_failed);
  return kExitOk;
}

std::string UnitHex(std::uint32_t code_point) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "U+%04X", code_point);
  return buffer;
}

std::string DecisionText(const KeyDecision& decision) {
  std::string out = std::string(DescribeKeyDisposition(decision.disposition)) + "/" +
                    DescribeKeyDecisionReason(decision.reason);
  if (decision.text_class != TextClass::kNone) {
    out += "/";
    out += DescribeTextClass(decision.text_class);
  }
  if (decision.ascii_code_point != 0) {
    out += "/";
    out += UnitHex(decision.ascii_code_point);
  }
  return out;
}

KeyEventSnapshot Down(std::uint32_t virtual_key) {
  KeyEventSnapshot event;
  event.virtual_key = virtual_key;
  event.scan_code = 0x1E;  // arbitrary non-zero make code; the pure layer never reads it
  event.transition = KeyTransition::kKeyDown;
  event.repeat_count = 1;
  event.previously_down = false;
  return event;
}

KeyEventSnapshot Up(std::uint32_t virtual_key) {
  KeyEventSnapshot event = Down(virtual_key);
  event.transition = KeyTransition::kKeyUp;
  return event;
}

KeyText TextOf(std::u16string units) {
  KeyText text;
  text.status = KeyTextStatus::kText;
  text.utf16_code_units = std::move(units);
  return text;
}

KeyText TextOf(char16_t unit) { return TextOf(std::u16string(1, unit)); }

KeyText NoText() {
  KeyText text;
  text.status = KeyTextStatus::kNoText;
  return text;
}

KeyText DeadKey() {
  KeyText text;
  text.status = KeyTextStatus::kDeadKey;
  return text;
}

InputContext Context(bool composition_active) {
  InputContext context;
  context.input_enabled = true;
  context.engine_ready = true;
  context.composition_active = composition_active;
  context.permission = InputPermission::kAllowed;
  return context;
}

void Expect(const KeyDecision& decision, KeyDisposition disposition, KeyDecisionReason reason,
            const std::string& label) {
  Check(decision.disposition == disposition && decision.reason == reason, label,
        DecisionText(decision));
}

void ExpectCandidate(const KeyDecision& decision, TextClass text_class,
                     std::uint32_t ascii_code_point, const std::string& label) {
  Check(decision.disposition == KeyDisposition::kEngineTextCandidate &&
            decision.reason == KeyDecisionReason::kNone &&
            decision.text_class == text_class &&
            decision.ascii_code_point == ascii_code_point,
        label, DecisionText(decision));
}

void ExpectPassThrough(const KeyText& text, bool composition_active,
                       KeyDecisionReason reason, const std::string& label) {
  const KeyDecision decision = DecideKey(Down(0x41), text, Context(composition_active));
  Expect(decision, KeyDisposition::kPassThroughToHost, reason, label);
}

void ExpectKeyPassThrough(std::uint32_t virtual_key, bool composition_active,
                          KeyDecisionReason reason, const std::string& label) {
  const KeyDecision decision =
      DecideKey(Down(virtual_key), NoText(), Context(composition_active));
  Expect(decision, KeyDisposition::kPassThroughToHost, reason, label);
}

void ExpectBoundary(const KeyText& text, KeyDecisionReason reason, const std::string& label) {
  const KeyDecision decision = DecideKey(Down(0x41), text, Context(true));
  Expect(decision, KeyDisposition::kCompositionBoundaryRequired, reason, label);
}

// --- the character table --------------------------------------------------------------
void RunSchemaTable() {
  std::printf("[input-pure] mode decisions: the English schema alphabet\n");

  Check(epocanva::windows::input::kVirtualKeyBack == 0x08 &&
            epocanva::windows::input::kVirtualKeyReturn == 0x0D &&
            epocanva::windows::input::kVirtualKeyEscape == 0x1B &&
            epocanva::windows::input::kVirtualKeySpace == 0x20,
        "the four virtual-key codes match winuser.h:479,487,509,516", "constants drifted");

  const std::string alphabet(epocanva::windows::input::kEnglishSchemaAlphabet);
  Check(alphabet.size() == 76,
        "the schema alphabet literal has 76 ASCII characters (52 letters + 10 digits + 14 marks)",
        std::to_string(alphabet.size()) + " characters");

  bool in_literal[128] = {};
  bool duplicates = false;
  for (const char ch : alphabet) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    if (byte >= 128 || in_literal[byte]) {
      duplicates = true;
    } else {
      in_literal[byte] = true;
    }
  }
  Check(!duplicates, "the schema alphabet contains no duplicate and no non-ASCII byte",
        "duplicate or byte >= 0x80");

  bool table_matches = true;
  std::string mismatch;
  for (std::uint32_t code_point = 0; code_point < 128; ++code_point) {
    const bool expected = in_literal[code_point];
    if (epocanva::windows::input::IsForwardableAscii(code_point) != expected) {
      table_matches = false;
      mismatch += UnitHex(code_point);
      mismatch += " ";
    }
  }
  Check(table_matches,
        "IsForwardableAscii accepts exactly the 76 schema characters and rejects the other 52",
        mismatch.empty() ? "unexpected mismatch" : mismatch);

  int letters = 0;
  int digits = 0;
  int delimiters = 0;
  int marks = 0;
  for (std::uint32_t code_point = 0; code_point < 128; ++code_point) {
    switch (epocanva::windows::input::ClassifyForwardableAscii(code_point)) {
      case TextClass::kAsciiLetter:
        ++letters;
        break;
      case TextClass::kAsciiDigit:
        ++digits;
        break;
      case TextClass::kTokenDelimiter:
        ++delimiters;
        break;
      case TextClass::kProtectedMark:
        ++marks;
        break;
      case TextClass::kNone:
        break;
    }
  }
  Check(letters == 52 && digits == 10 && delimiters == 2 && marks == 12,
        "the classes partition the alphabet as 52/10/2/12",
        std::to_string(letters) + "/" + std::to_string(digits) + "/" +
            std::to_string(delimiters) + "/" + std::to_string(marks));
  std::printf("[input-pure] evidence alphabet=%zu letters=%d digits=%d delimiters=%d marks=%d\n",
              alphabet.size(), letters, digits, delimiters, marks);

  // Every accepted character becomes a text candidate with its own class and value.
  bool all_candidates = true;
  std::string candidate_detail;
  for (std::size_t index = 0; index < alphabet.size(); ++index) {
    const std::uint32_t code_point = static_cast<unsigned char>(alphabet[index]);
    const KeyDecision decision =
        DecideKey(Down(0x41), TextOf(static_cast<char16_t>(code_point)), Context(false));
    if (decision.disposition != KeyDisposition::kEngineTextCandidate ||
        decision.reason != KeyDecisionReason::kNone ||
        decision.ascii_code_point != code_point ||
        decision.text_class != epocanva::windows::input::ClassifyForwardableAscii(code_point)) {
      all_candidates = false;
      candidate_detail += UnitHex(code_point);
      candidate_detail += "->";
      candidate_detail += DecisionText(decision);
      candidate_detail += " ";
    }
  }
  Check(all_candidates, "every schema character is a text candidate carrying its code point",
        candidate_detail.empty() ? "unexpected mismatch" : candidate_detail);

  // The two letters the brief names explicitly, plus the marks in both spellings.
  ExpectCandidate(DecideKey(Down(0x41), TextOf(u"a"), Context(false)), TextClass::kAsciiLetter,
                  0x61, "a is a lowercase letter candidate");
  ExpectCandidate(DecideKey(Down(0x41), TextOf(u"A"), Context(false)), TextClass::kAsciiLetter,
                  0x41, "A is an uppercase letter candidate (Shift is not a blocker)");
  ExpectCandidate(DecideKey(Down(0x32), TextOf(u"0"), Context(false)), TextClass::kAsciiDigit,
                  0x30, "0 is a digit candidate and is not dropped");
  ExpectCandidate(DecideKey(Down(0x32), TextOf(u"9"), Context(false)), TextClass::kAsciiDigit,
                  0x39, "9 is a digit candidate");
  ExpectCandidate(DecideKey(Down(0x41), TextOf(u"'"), Context(false)),
                  TextClass::kTokenDelimiter, 0x27, "apostrophe is a token delimiter");
  ExpectCandidate(DecideKey(Down(0x41), TextOf(u"-"), Context(false)),
                  TextClass::kTokenDelimiter, 0x2D, "hyphen-minus is a token delimiter");
  for (const char mark : std::string("@._:/\\?&=#%+")) {
    const std::uint32_t code_point = static_cast<unsigned char>(mark);
    const KeyDecision decision =
        DecideKey(Down(0x41), TextOf(static_cast<char16_t>(code_point)), Context(false));
    ExpectCandidate(decision, TextClass::kProtectedMark, code_point,
                    "technical mark " + UnitHex(code_point) + " is forwarded as protected");
  }
}

// --- the special keys -----------------------------------------------------------------
void RunSpecialKeys() {
  std::printf("[input-pure] the four special keys\n");

  const KeyDecision space =
      DecideKey(Down(epocanva::windows::input::kVirtualKeySpace), NoText(), Context(false));
  Expect(space, KeyDisposition::kEngineSpaceCandidate, KeyDecisionReason::kNone,
         "unmodified Space is a space candidate even with nothing composing");
  Expect(DecideKey(Down(epocanva::windows::input::kVirtualKeySpace), NoText(), Context(true)),
         KeyDisposition::kEngineSpaceCandidate, KeyDecisionReason::kNone,
         "unmodified Space is a space candidate while composing");

  Expect(DecideKey(Down(epocanva::windows::input::kVirtualKeyReturn), NoText(), Context(true)),
         KeyDisposition::kEngineEnterCandidate, KeyDecisionReason::kNone,
         "unmodified Enter while composing is an Enter candidate");
  ExpectKeyPassThrough(epocanva::windows::input::kVirtualKeyReturn, false,
                       KeyDecisionReason::kNoComposition,
                       "Enter without a composition keeps host newline semantics");

  Expect(DecideKey(Down(epocanva::windows::input::kVirtualKeyBack), NoText(), Context(true)),
         KeyDisposition::kEngineBackSpaceCandidate, KeyDecisionReason::kNone,
         "unmodified BackSpace while composing is a BackSpace candidate");
  ExpectKeyPassThrough(epocanva::windows::input::kVirtualKeyBack, false,
                       KeyDecisionReason::kBackSpaceWithoutComposition,
                       "BackSpace without a composition is never offered to the engine");

  Expect(DecideKey(Down(epocanva::windows::input::kVirtualKeyEscape), NoText(), Context(true)),
         KeyDisposition::kEngineEscapeCandidate, KeyDecisionReason::kNone,
         "unmodified Escape while composing is an Escape candidate");
  ExpectKeyPassThrough(epocanva::windows::input::kVirtualKeyEscape, false,
                       KeyDecisionReason::kNoComposition,
                       "Escape without a composition keeps host semantics");

  // The virtual key decides before the translation, so a stray translated character cannot
  // turn Space into a text candidate.
  Expect(DecideKey(Down(epocanva::windows::input::kVirtualKeySpace), TextOf(u"x"), Context(true)),
         KeyDisposition::kEngineSpaceCandidate, KeyDecisionReason::kNone,
         "Space stays a space candidate even if a translation result is supplied");

  // Shift on a special key is deliberately not given a product rule.
  KeyEventSnapshot shifted_space = Down(epocanva::windows::input::kVirtualKeySpace);
  shifted_space.modifiers.shift = true;
  Expect(DecideKey(shifted_space, NoText(), Context(true)),
         KeyDisposition::kPassThroughToHost,
         KeyDecisionReason::kUnsupportedModifierCombination,
         "Shift+Space is an unsupported combination, not a product rule");

  KeyEventSnapshot shifted_enter = Down(epocanva::windows::input::kVirtualKeyReturn);
  shifted_enter.modifiers.shift = true;
  Expect(DecideKey(shifted_enter, NoText(), Context(true)),
         KeyDisposition::kPassThroughToHost,
         KeyDecisionReason::kUnsupportedModifierCombination,
         "Shift+Enter is an unsupported combination, not a product rule");

  KeyEventSnapshot shifted_back = Down(epocanva::windows::input::kVirtualKeyBack);
  shifted_back.modifiers.shift = true;
  Expect(DecideKey(shifted_back, NoText(), Context(true)),
         KeyDisposition::kPassThroughToHost,
         KeyDecisionReason::kUnsupportedModifierCombination,
         "Shift+BackSpace is an unsupported combination");

  KeyEventSnapshot shifted_escape = Down(epocanva::windows::input::kVirtualKeyEscape);
  shifted_escape.modifiers.shift = true;
  Expect(DecideKey(shifted_escape, NoText(), Context(true)),
         KeyDisposition::kPassThroughToHost,
         KeyDecisionReason::kUnsupportedModifierCombination,
         "Shift+Escape is an unsupported combination");

  // Caps Lock is not a modifier for the space bar.
  KeyEventSnapshot caps_space = Down(epocanva::windows::input::kVirtualKeySpace);
  caps_space.modifiers.caps_lock = true;
  Expect(DecideKey(caps_space, NoText(), Context(false)),
         KeyDisposition::kEngineSpaceCandidate, KeyDecisionReason::kNone,
         "Caps Lock does not block the space bar");

  // A key-up is not a space press.
  Expect(DecideKey(Up(epocanva::windows::input::kVirtualKeySpace), NoText(), Context(true)),
         KeyDisposition::kPassThroughToHost, KeyDecisionReason::kKeyRelease,
         "a Space key-up is a release, not a space candidate");
}

// --- shortcuts ------------------------------------------------------------------------
void RunShortcuts() {
  std::printf("[input-pure] shortcuts and modifier policy\n");

  KeyEventSnapshot ctrl_c = Down(0x43);
  ctrl_c.modifiers.control = true;
  Expect(DecideKey(ctrl_c, TextOf(u"c"), Context(false)),
         KeyDisposition::kShortcutPassThrough, KeyDecisionReason::kShortcutModifier,
         "Ctrl+C is a shortcut, not text");

  KeyEventSnapshot ctrl_v = Down(0x56);
  ctrl_v.modifiers.control = true;
  Expect(DecideKey(ctrl_v, TextOf(u"v"), Context(false)),
         KeyDisposition::kShortcutPassThrough, KeyDecisionReason::kShortcutModifier,
         "Ctrl+V is a shortcut, not text");

  KeyEventSnapshot ctrl_space = Down(epocanva::windows::input::kVirtualKeySpace);
  ctrl_space.modifiers.control = true;
  Expect(DecideKey(ctrl_space, NoText(), Context(false)),
         KeyDisposition::kShortcutPassThrough, KeyDecisionReason::kShortcutModifier,
         "Ctrl+Space is a shortcut, not a space candidate");

  KeyEventSnapshot alt_a = Down(0x41);
  alt_a.modifiers.alt = true;
  Expect(DecideKey(alt_a, TextOf(u"a"), Context(false)),
         KeyDisposition::kShortcutPassThrough, KeyDecisionReason::kShortcutModifier,
         "Alt+A is a shortcut, not text");

  KeyEventSnapshot win_a = Down(0x41);
  win_a.modifiers.windows = true;
  Expect(DecideKey(win_a, TextOf(u"a"), Context(false)),
         KeyDisposition::kShortcutPassThrough, KeyDecisionReason::kShortcutModifier,
         "Win+A is a shortcut, not text");

  KeyEventSnapshot altgr_a = Down(0x41);
  altgr_a.modifiers.control = true;
  altgr_a.modifiers.alt = true;
  Expect(DecideKey(altgr_a, TextOf(u"@"), Context(false)),
         KeyDisposition::kShortcutPassThrough, KeyDecisionReason::kShortcutModifier,
         "Control+Alt (the AltGr shape) is not treated as supported text input");

  KeyEventSnapshot shift_a = Down(0x41);
  shift_a.modifiers.shift = true;
  ExpectCandidate(DecideKey(shift_a, TextOf(u"A"), Context(false)), TextClass::kAsciiLetter,
                  0x41, "Shift on a letter is allowed; translation decides the character");

  KeyEventSnapshot caps_a = Down(0x41);
  caps_a.modifiers.caps_lock = true;
  ExpectCandidate(DecideKey(caps_a, TextOf(u"A"), Context(false)), TextClass::kAsciiLetter,
                  0x41, "Caps Lock does not block a text candidate");
}

// --- key-up, repeat and idempotence ---------------------------------------------------
void RunReleaseRepeatAndIdempotence() {
  std::printf("[input-pure] release, repeat metadata and idempotence\n");

  Expect(DecideKey(Up(0x41), TextOf(u"a"), Context(true)), KeyDisposition::kPassThroughToHost,
         KeyDecisionReason::kKeyRelease,
         "a key-up with a text result is still not sent to the text path");
  Expect(DecideKey(Up(0x41), DeadKey(), Context(true)), KeyDisposition::kPassThroughToHost,
         KeyDecisionReason::kKeyRelease,
         "a key-up with a dead-key result is still a release, not a boundary demand");

  const KeyText text = TextOf(u"a");
  const InputContext context = Context(true);

  KeyEventSnapshot first_press = Down(0x41);
  first_press.repeat_count = 1;
  first_press.previously_down = false;
  KeyEventSnapshot fifth_repeat = Down(0x41);
  fifth_repeat.repeat_count = 5;
  fifth_repeat.previously_down = true;

  const KeyDecision first = DecideKey(first_press, text, context);
  const KeyDecision repeated = DecideKey(fifth_repeat, text, context);
  Check(first == repeated,
        "repeat_count 1 and 5 with different previous-state produce the same one decision",
        DecisionText(first) + " vs " + DecisionText(repeated));
  Check(first.disposition == KeyDisposition::kEngineTextCandidate,
        "a repeated key is still exactly one text candidate, never five",
        DecisionText(first));

  KeyEventSnapshot space_repeat = Down(epocanva::windows::input::kVirtualKeySpace);
  space_repeat.repeat_count = 4;
  space_repeat.previously_down = true;
  Expect(DecideKey(space_repeat, NoText(), Context(false)),
         KeyDisposition::kEngineSpaceCandidate, KeyDecisionReason::kNone,
         "a repeated Space is one space candidate, not four");

  // The same value evaluated repeatedly: same answer, and the inputs are untouched.
  const KeyDecision second = DecideKey(first_press, text, context);
  const KeyDecision third = DecideKey(first_press, text, context);
  Check(first == second && second == third,
        "the same input evaluated three times yields identical decisions",
        DecisionText(first) + " / " + DecisionText(second) + " / " + DecisionText(third));
  Check(text.utf16_code_units == std::u16string(u"a") &&
            first_press.virtual_key == 0x41 && first_press.repeat_count == 1 &&
            context.composition_active && context.permission == InputPermission::kAllowed,
        "the decision left the event, translation and context unchanged",
        "an input field changed");
}

// --- unsupported translation results --------------------------------------------------
void RunUnsupportedText() {
  std::printf("[input-pure] dead keys, no text, multi-unit and non-ASCII results\n");

  ExpectPassThrough(NoText(), false, KeyDecisionReason::kNoText,
                    "a zero-unit translation is a pass-through, not empty text input");
  ExpectPassThrough(NoText(), true, KeyDecisionReason::kNoText,
                    "a zero-unit translation while composing is still not a boundary demand");

  KeyText dead_with_spacing = DeadKey();
  dead_with_spacing.utf16_code_units = u"\u00B4";  // the spacing character the API may write
  ExpectPassThrough(dead_with_spacing, false, KeyDecisionReason::kDeadKey,
                    "a dead key without a composition passes through and exposes no text");
  const KeyDecision dead_decision =
      DecideKey(Down(0x41), dead_with_spacing, Context(false));
  Check(dead_decision.ascii_code_point == 0 && dead_decision.text_class == TextClass::kNone,
        "the dead key's spacing character is never taken as input",
        DecisionText(dead_decision));
  ExpectBoundary(dead_with_spacing, KeyDecisionReason::kDeadKey,
                 "a dead key while composing demands boundary coordination");

  ExpectPassThrough(TextOf(u"ab"), false, KeyDecisionReason::kMultipleUtf16CodeUnits,
                    "two translated code units are rejected whole, not truncated to 'a'");
  ExpectBoundary(TextOf(u"ab"), KeyDecisionReason::kMultipleUtf16CodeUnits,
                 "two translated code units while composing demand boundary coordination");

  ExpectPassThrough(TextOf(std::u16string(u"\xD83D\xDE00")), false,
                    KeyDecisionReason::kMultipleUtf16CodeUnits,
                    "a surrogate pair is two code units and is rejected whole");
  ExpectPassThrough(TextOf(u"\u00E9"), false, KeyDecisionReason::kNonAsciiCharacter,
                    "a single non-ASCII code unit (e-acute) is rejected");
  ExpectBoundary(TextOf(u"\u00E9"), KeyDecisionReason::kNonAsciiCharacter,
                 "non-ASCII while composing demands boundary coordination");
  ExpectBoundary(TextOf(std::u16string(u"\xD83D\xDE00")),
                 KeyDecisionReason::kMultipleUtf16CodeUnits,
                 "a supplementary character while composing demands boundary coordination");

  ExpectPassThrough(TextOf(u","), false, KeyDecisionReason::kUnsupportedCharacter,
                    "comma is outside the schema and is not silently forwarded");
  ExpectBoundary(TextOf(u","), KeyDecisionReason::kUnsupportedCharacter,
                 "unsupported printable ASCII while composing demands boundary coordination");
  ExpectPassThrough(TextOf(u" "), false, KeyDecisionReason::kUnsupportedCharacter,
                    "a space produced by a non-Space key is not accepted as token text");
  ExpectPassThrough(TextOf(u"\u0001"), false, KeyDecisionReason::kUnsupportedCharacter,
                    "a control character is not accepted as text");

  ExpectPassThrough(TextOf(u""), false, KeyDecisionReason::kTranslationFailed,
                    "kText with zero units is an inconsistent result, refused");

  KeyText not_key_down;
  not_key_down.status = KeyTextStatus::kNotKeyDown;
  ExpectPassThrough(not_key_down, false, KeyDecisionReason::kTranslationFailed,
                    "a not-key-down translation result is refused on a down event");

  KeyText no_layout;
  no_layout.status = KeyTextStatus::kNoLayout;
  ExpectPassThrough(no_layout, false, KeyDecisionReason::kTranslationFailed,
                    "a missing layout is a translation failure");

  KeyText invalid;
  invalid.status = KeyTextStatus::kInvalidText;
  ExpectPassThrough(invalid, false, KeyDecisionReason::kTranslationFailed,
                    "ill-formed UTF-16 is a translation failure");

  KeyText system_failure;
  system_failure.status = KeyTextStatus::kSystemFailure;
  ExpectPassThrough(system_failure, false, KeyDecisionReason::kTranslationFailed,
                    "a system failure is a translation failure");
  ExpectPassThrough(system_failure, true, KeyDecisionReason::kTranslationFailed,
                    "a system failure while composing is not reported as printable content");
}

// --- context precedence ---------------------------------------------------------------
void RunContextPrecedence() {
  std::printf("[input-pure] permission, enabled flag and engine readiness\n");

  InputContext forbidden = Context(true);
  forbidden.permission = InputPermission::kForbidden;
  Expect(DecideKey(Down(0x41), TextOf(u"a"), forbidden), KeyDisposition::kPassThroughToHost,
         KeyDecisionReason::kPermissionForbidden,
         "a forbidden field never produces a text candidate");

  InputContext unknown = Context(true);
  unknown.permission = InputPermission::kUnknown;
  Expect(DecideKey(Down(0x41), TextOf(u"a"), unknown), KeyDisposition::kPassThroughToHost,
         KeyDecisionReason::kPermissionUnknown,
         "an unknown permission is conservative and never produces a text candidate");

  InputContext disabled = Context(false);
  disabled.input_enabled = false;
  Expect(DecideKey(Down(0x41), TextOf(u"a"), disabled), KeyDisposition::kPassThroughToHost,
         KeyDecisionReason::kInputDisabled, "a disabled service passes text through");

  InputContext not_ready = Context(true);
  not_ready.engine_ready = false;
  Expect(DecideKey(Down(0x41), TextOf(u"a"), not_ready), KeyDisposition::kPassThroughToHost,
         KeyDecisionReason::kEngineNotReady,
         "an engine that is not ready cannot create a text candidate");
  Expect(DecideKey(Down(epocanva::windows::input::kVirtualKeySpace), NoText(), not_ready),
         KeyDisposition::kPassThroughToHost, KeyDecisionReason::kEngineNotReady,
         "an engine that is not ready cannot create a space candidate");

  InputContext forbidden_release = forbidden;
  Expect(DecideKey(Up(0x41), TextOf(u"a"), forbidden_release),
         KeyDisposition::kPassThroughToHost, KeyDecisionReason::kKeyRelease,
         "a key-up is reported as a release before the permission is considered");

  InputContext forbidden_shortcut = forbidden;
  KeyEventSnapshot ctrl_a = Down(0x41);
  ctrl_a.modifiers.control = true;
  Expect(DecideKey(ctrl_a, TextOf(u"a"), forbidden_shortcut),
         KeyDisposition::kPassThroughToHost, KeyDecisionReason::kPermissionForbidden,
         "a privacy refusal is reported before a shortcut modifier");

  InputContext disabled_forbidden = Context(false);
  disabled_forbidden.input_enabled = false;
  disabled_forbidden.permission = InputPermission::kForbidden;
  Expect(DecideKey(Down(0x41), TextOf(u"a"), disabled_forbidden),
         KeyDisposition::kPassThroughToHost, KeyDecisionReason::kPermissionForbidden,
         "permission is reported before the enabled flag");

  InputContext not_ready_shortcut = Context(false);
  not_ready_shortcut.engine_ready = false;
  Expect(DecideKey(ctrl_a, TextOf(u"a"), not_ready_shortcut),
         KeyDisposition::kShortcutPassThrough, KeyDecisionReason::kShortcutModifier,
         "a shortcut is reported before engine readiness");
}

// --- composition boundary and flags ---------------------------------------------------
void RunBoundaryAndFlags() {
  std::printf("[input-pure] composition boundary and decision flags\n");

  ExpectCandidate(DecideKey(Down(0x41), TextOf(u"a"), Context(true)),
                  TextClass::kAsciiLetter, 0x61,
                  "an accepted character while composing is a text candidate");
  ExpectCandidate(DecideKey(Down(0x41), TextOf(u"a"), Context(false)),
                  TextClass::kAsciiLetter, 0x61,
                  "an accepted character without a composition is a text candidate");

  const KeyDecision text_decision = DecideKey(Down(0x41), TextOf(u"a"), Context(true));
  Check(text_decision.is_engine_candidate() && !text_decision.requires_composition_boundary(),
        "a text candidate reports the candidate flag and no boundary demand",
        DecisionText(text_decision));

  const KeyDecision space_decision =
      DecideKey(Down(epocanva::windows::input::kVirtualKeySpace), NoText(), Context(true));
  Check(space_decision.is_engine_candidate() &&
            !space_decision.requires_composition_boundary(),
        "a space candidate reports the candidate flag", DecisionText(space_decision));

  const KeyDecision boundary_decision = DecideKey(Down(0x41), TextOf(u","), Context(true));
  Check(!boundary_decision.is_engine_candidate() &&
            boundary_decision.requires_composition_boundary(),
        "a boundary demand is not an engine candidate", DecisionText(boundary_decision));

  const KeyDecision pass_decision = DecideKey(Down(0x41), TextOf(u"a"), Context(false));
  InputContext forbidden = Context(false);
  forbidden.permission = InputPermission::kForbidden;
  const KeyDecision refused = DecideKey(Down(0x41), TextOf(u"a"), forbidden);
  Check(!pass_decision.requires_composition_boundary() &&
            !refused.requires_composition_boundary(),
        "ordinary candidates and refusals carry no boundary demand",
        DecisionText(pass_decision) + " / " + DecisionText(refused));

  // The translator's own status names are stable and text-free.
  Check(std::string(DescribeKeyTextStatus(KeyTextStatus::kText)) == "text" &&
            std::string(DescribeKeyTextStatus(KeyTextStatus::kDeadKey)) == "dead-key" &&
            std::string(DescribeKeyTextStatus(KeyTextStatus::kNoText)) == "no-text",
        "translation status names are distinct and stable",
        DescribeKeyTextStatus(KeyTextStatus::kText));
}

int RunDecisions() {
  RunSchemaTable();
  RunSpecialKeys();
  RunShortcuts();
  RunReleaseRepeatAndIdempotence();
  RunUnsupportedText();
  RunContextPrecedence();
  RunBoundaryAndFlags();
  return FinishMode();
}

// --- the falsifiability control -------------------------------------------------------
int RunSelfCheckFail() {
  std::printf("[input-pure] mode self-check-fail: one deliberately false claim\n");
  // The false claim: a dead key is delivered as text. Every real mode pins the opposite, so
  // this must fail here -- exit 14, no RESULT PASS. Without this control, a green run whose
  // checks had all become tautologies would be indistinguishable.
  const KeyDecision decision = DecideKey(Down(0x41), DeadKey(), Context(false));
  Check(decision.disposition == KeyDisposition::kEngineTextCandidate,
        "deliberately asserted that a dead key becomes a text candidate",
        DecisionText(decision));
  std::printf("[input-pure] RESULT FAIL (self-check-fail control, %d checks, %d failed)\n",
              g_total, g_failed);
  return g_failed == 0 ? kExitOk : kExitSelfCheckFailed;
}

}  // namespace

int main(int argc, char** argv) {
  std::string mode;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--mode" && index + 1 < argc) {
      mode = argv[++index];
    } else {
      std::printf("usage: input_decision_tests --mode decisions|self-check-fail\n");
      return kExitUsage;
    }
  }
  if (mode == "decisions") {
    return RunDecisions();
  }
  if (mode == "self-check-fail") {
    return RunSelfCheckFail();
  }
  std::printf("usage: input_decision_tests --mode decisions|self-check-fail\n");
  return kExitUsage;
}
