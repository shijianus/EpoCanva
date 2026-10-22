// EpoCanva P3.3 -- the pure keyboard decision function.
//
// No <windows.h>, no system call, no allocation beyond the returned value, no state. The
// rules and their precedence are documented in key_decision.h; this file is the executable
// form of that table and nothing else.
#include "key_decision.h"

namespace epocanva {
namespace windows {
namespace input {
namespace {

bool AnyShortcutModifier(const ModifierSnapshot& modifiers) {
  // AltGr arrives as Control+Alt and therefore lands here on purpose: this round does not
  // treat it as text input.
  return modifiers.control || modifiers.alt || modifiers.windows;
}

KeyDecision PassThrough(KeyDecisionReason reason) {
  KeyDecision decision;
  decision.disposition = KeyDisposition::kPassThroughToHost;
  decision.reason = reason;
  return decision;
}

KeyDecision ShortcutPassThrough() {
  KeyDecision decision;
  decision.disposition = KeyDisposition::kShortcutPassThrough;
  decision.reason = KeyDecisionReason::kShortcutModifier;
  return decision;
}

// Unsupported translated content. With a composition on screen this round cannot say where
// the content belongs, so it reports an explicit boundary demand. Without one it is simply
// not ours and the host keeps the key.
KeyDecision UnsupportedContent(KeyDecisionReason reason, bool composition_active) {
  KeyDecision decision;
  decision.reason = reason;
  decision.disposition = composition_active
                             ? KeyDisposition::kCompositionBoundaryRequired
                             : KeyDisposition::kPassThroughToHost;
  return decision;
}

KeyDecision SpecialKeyCandidate(KeyDisposition disposition) {
  KeyDecision decision;
  decision.disposition = disposition;
  decision.reason = KeyDecisionReason::kNone;
  return decision;
}

}  // namespace

const char kEnglishSchemaAlphabet[] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789"
    "'@._:/\\?&=#%+-";

TextClass ClassifyForwardableAscii(std::uint32_t ascii_code_point) {
  if ((ascii_code_point >= 'A' && ascii_code_point <= 'Z') ||
      (ascii_code_point >= 'a' && ascii_code_point <= 'z')) {
    return TextClass::kAsciiLetter;
  }
  if (ascii_code_point >= '0' && ascii_code_point <= '9') {
    return TextClass::kAsciiDigit;
  }
  if (ascii_code_point == '\'' || ascii_code_point == '-') {
    return TextClass::kTokenDelimiter;
  }
  switch (ascii_code_point) {
    case '@':
    case '.':
    case '_':
    case ':':
    case '/':
    case '\\':
    case '?':
    case '&':
    case '=':
    case '#':
    case '%':
    case '+':
      return TextClass::kProtectedMark;
    default:
      return TextClass::kNone;
  }
}

bool IsForwardableAscii(std::uint32_t ascii_code_point) {
  return ClassifyForwardableAscii(ascii_code_point) != TextClass::kNone;
}

bool KeyDecision::is_engine_candidate() const {
  switch (disposition) {
    case KeyDisposition::kEngineTextCandidate:
    case KeyDisposition::kEngineSpaceCandidate:
    case KeyDisposition::kEngineEnterCandidate:
    case KeyDisposition::kEngineBackSpaceCandidate:
    case KeyDisposition::kEngineEscapeCandidate:
      return true;
    case KeyDisposition::kPassThroughToHost:
    case KeyDisposition::kShortcutPassThrough:
    case KeyDisposition::kCompositionBoundaryRequired:
      return false;
  }
  return false;
}

bool KeyDecision::requires_composition_boundary() const {
  return disposition == KeyDisposition::kCompositionBoundaryRequired;
}

KeyDecision DecideKey(const KeyEventSnapshot& event, const KeyText& text,
                      const InputContext& context) {
  // 1. A key-up is never part of the English text path and is never translated.
  if (event.transition == KeyTransition::kKeyUp) {
    return PassThrough(KeyDecisionReason::kKeyRelease);
  }

  // 2-4. Privacy and availability, most specific first. All three pass the key to the host;
  // the reason keeps "not allowed here" separable from "service off".
  if (context.permission == InputPermission::kForbidden) {
    return PassThrough(KeyDecisionReason::kPermissionForbidden);
  }
  if (context.permission == InputPermission::kUnknown) {
    return PassThrough(KeyDecisionReason::kPermissionUnknown);
  }
  if (!context.input_enabled) {
    return PassThrough(KeyDecisionReason::kInputDisabled);
  }

  // 5. A Control/Alt/Windows chord belongs to the OS or the host, never to text input.
  if (AnyShortcutModifier(event.modifiers)) {
    return ShortcutPassThrough();
  }

  // 6. Without an engine session nothing can be offered anywhere; the host keeps the key.
  if (!context.engine_ready) {
    return PassThrough(KeyDecisionReason::kEngineNotReady);
  }

  // 7. The four special keys. Shift on any of them is an unsupported combination this round
  // deliberately does not define a product rule for.
  if (event.virtual_key == kVirtualKeySpace) {
    if (event.modifiers.shift) {
      return PassThrough(KeyDecisionReason::kUnsupportedModifierCombination);
    }
    return SpecialKeyCandidate(KeyDisposition::kEngineSpaceCandidate);
  }
  if (event.virtual_key == kVirtualKeyBack) {
    if (event.modifiers.shift) {
      return PassThrough(KeyDecisionReason::kUnsupportedModifierCombination);
    }
    if (!context.composition_active) {
      return PassThrough(KeyDecisionReason::kBackSpaceWithoutComposition);
    }
    return SpecialKeyCandidate(KeyDisposition::kEngineBackSpaceCandidate);
  }
  if (event.virtual_key == kVirtualKeyReturn) {
    if (event.modifiers.shift) {
      return PassThrough(KeyDecisionReason::kUnsupportedModifierCombination);
    }
    if (!context.composition_active) {
      return PassThrough(KeyDecisionReason::kNoComposition);
    }
    return SpecialKeyCandidate(KeyDisposition::kEngineEnterCandidate);
  }
  if (event.virtual_key == kVirtualKeyEscape) {
    if (event.modifiers.shift) {
      return PassThrough(KeyDecisionReason::kUnsupportedModifierCombination);
    }
    if (!context.composition_active) {
      return PassThrough(KeyDecisionReason::kNoComposition);
    }
    return SpecialKeyCandidate(KeyDisposition::kEngineEscapeCandidate);
  }

  // 8. Everything else is decided by the translation result, never by the virtual key's
  // label. A dead key is refused as a whole: the spacing character in the API buffer is not
  // input. A multi-unit or non-ASCII result is refused as a whole too -- taking the first
  // unit would corrupt text.
  switch (text.status) {
    case KeyTextStatus::kDeadKey:
      return UnsupportedContent(KeyDecisionReason::kDeadKey, context.composition_active);
    case KeyTextStatus::kNoText:
      return PassThrough(KeyDecisionReason::kNoText);
    case KeyTextStatus::kNotKeyDown:
    case KeyTextStatus::kNoLayout:
    case KeyTextStatus::kInvalidText:
    case KeyTextStatus::kSystemFailure:
      return PassThrough(KeyDecisionReason::kTranslationFailed);
    case KeyTextStatus::kText:
      break;
  }

  if (text.utf16_code_units.empty()) {
    // Inconsistent input: kText must carry the exact returned units, and a translation that
    // produced none should have reported kNoText. Refuse rather than read nothing.
    return PassThrough(KeyDecisionReason::kTranslationFailed);
  }
  if (text.utf16_code_units.size() != 1) {
    return UnsupportedContent(KeyDecisionReason::kMultipleUtf16CodeUnits,
                              context.composition_active);
  }

  const std::uint32_t code_unit = text.utf16_code_units[0];
  if (code_unit > 0x7Fu) {
    return UnsupportedContent(KeyDecisionReason::kNonAsciiCharacter,
                              context.composition_active);
  }

  const TextClass text_class = ClassifyForwardableAscii(code_unit);
  if (text_class == TextClass::kNone) {
    return UnsupportedContent(KeyDecisionReason::kUnsupportedCharacter,
                              context.composition_active);
  }

  KeyDecision decision;
  decision.disposition = KeyDisposition::kEngineTextCandidate;
  decision.reason = KeyDecisionReason::kNone;
  decision.text_class = text_class;
  decision.ascii_code_point = code_unit;
  return decision;
}

const char* DescribeKeyTextStatus(KeyTextStatus status) {
  switch (status) {
    case KeyTextStatus::kText:
      return "text";
    case KeyTextStatus::kNoText:
      return "no-text";
    case KeyTextStatus::kDeadKey:
      return "dead-key";
    case KeyTextStatus::kNotKeyDown:
      return "not-key-down";
    case KeyTextStatus::kNoLayout:
      return "no-layout";
    case KeyTextStatus::kInvalidText:
      return "invalid-text";
    case KeyTextStatus::kSystemFailure:
      return "system-failure";
  }
  return "unknown";
}

const char* DescribeKeyDisposition(KeyDisposition disposition) {
  switch (disposition) {
    case KeyDisposition::kPassThroughToHost:
      return "pass-through-host";
    case KeyDisposition::kShortcutPassThrough:
      return "shortcut-pass-through";
    case KeyDisposition::kEngineTextCandidate:
      return "engine-text-candidate";
    case KeyDisposition::kEngineSpaceCandidate:
      return "engine-space-candidate";
    case KeyDisposition::kEngineEnterCandidate:
      return "engine-enter-candidate";
    case KeyDisposition::kEngineBackSpaceCandidate:
      return "engine-backspace-candidate";
    case KeyDisposition::kEngineEscapeCandidate:
      return "engine-escape-candidate";
    case KeyDisposition::kCompositionBoundaryRequired:
      return "composition-boundary-required";
  }
  return "unknown";
}

const char* DescribeKeyDecisionReason(KeyDecisionReason reason) {
  switch (reason) {
    case KeyDecisionReason::kNone:
      return "none";
    case KeyDecisionReason::kKeyRelease:
      return "key-release";
    case KeyDecisionReason::kInputDisabled:
      return "input-disabled";
    case KeyDecisionReason::kPermissionForbidden:
      return "permission-forbidden";
    case KeyDecisionReason::kPermissionUnknown:
      return "permission-unknown";
    case KeyDecisionReason::kEngineNotReady:
      return "engine-not-ready";
    case KeyDecisionReason::kShortcutModifier:
      return "shortcut-modifier";
    case KeyDecisionReason::kUnsupportedModifierCombination:
      return "unsupported-modifier-combination";
    case KeyDecisionReason::kNoComposition:
      return "no-composition";
    case KeyDecisionReason::kBackSpaceWithoutComposition:
      return "backspace-without-composition";
    case KeyDecisionReason::kNoText:
      return "no-text";
    case KeyDecisionReason::kDeadKey:
      return "dead-key";
    case KeyDecisionReason::kTranslationFailed:
      return "translation-failed";
    case KeyDecisionReason::kMultipleUtf16CodeUnits:
      return "multiple-utf16-code-units";
    case KeyDecisionReason::kNonAsciiCharacter:
      return "non-ascii-character";
    case KeyDecisionReason::kUnsupportedCharacter:
      return "unsupported-character";
  }
  return "unknown";
}

const char* DescribeTextClass(TextClass text_class) {
  switch (text_class) {
    case TextClass::kNone:
      return "none";
    case TextClass::kAsciiLetter:
      return "ascii-letter";
    case TextClass::kAsciiDigit:
      return "ascii-digit";
    case TextClass::kTokenDelimiter:
      return "token-delimiter";
    case TextClass::kProtectedMark:
      return "protected-mark";
  }
  return "unknown";
}

bool operator==(const KeyDecision& left, const KeyDecision& right) {
  return left.disposition == right.disposition && left.reason == right.reason &&
         left.text_class == right.text_class &&
         left.ascii_code_point == right.ascii_code_point;
}

bool operator!=(const KeyDecision& left, const KeyDecision& right) {
  return !(left == right);
}

}  // namespace input
}  // namespace windows
}  // namespace epocanva
