// EpoCanva P3.3 -- the immutable facts one keyboard event contributes to a decision.
//
// LAYER: platform-neutral data. This header includes no <windows.h>, calls no API, reads no
// global keyboard state, opens no file, starts no engine and writes nothing. It exists so the
// decision rules in key_decision.h can be compiled and tested without a window station, a
// message queue or a keyboard. The Windows adapter that fills these fields lives in
// keyboard_state.{h,cc} and key_text_translation.{h,cc}.
//
// UNITS (AGENTS.md section 5). Every quantity below names its unit:
//   virtual_key       a Win32 virtual-key CODE (VK_* value), never a character.
//   scan_code         a hardware scan CODE. The make code for a key-down event; bit 15 is
//                     the key-up marker in the ToUnicodeEx contract, and this layer never
//                     translates a key-up event, so a down event must carry it clear.
//   repeat_count      a COUNT of repeats as reported by the host, not a number of actions.
//   ascii_code_point  one Unicode scalar value in the ASCII range (U+0000..U+007F); it is
//                     not an offset and not a length.
//   utf16_code_units  UTF-16 CODE UNITS, i.e. what ToUnicodeEx returns and what TSF and the
//                     host edit controls speak. It is not a grapheme cluster count.
//
// WHAT A REPEAT IS. "repeat_count = 5" means the host reports that the same key is still
// held down; it is METADATA. DecideKey returns exactly one decision for one callback and
// never expands a repeat into five actions (P3.3 brief section 7). A caller that wants five
// text insertions must receive five callbacks from the host.
//
// WHAT "PREVIOUSLY DOWN" IS. The host's previous-key-state bit for this virtual key, carried
// because the brief requires it to be representable. It does not change the policy result:
// repeat and previous-state are facts about delivery, not about what the key means.
//
// MODIFIERS. Left and right instances are collapsed (Shift/Control/Alt/Windows are each one
// bool) because this round has no product rule that distinguishes them. AltGr is deliberately
// NOT a field: Windows reports it as Control+Alt, and this round does not support it as text
// input (P3.3 brief section 6). caps_lock is the TOGGLE state, not a modifier: it is carried
// because it changes what ToUnicodeEx returns for a letter, not because it blocks a key.
#ifndef EPOCANVA_WINDOWS_INPUT_KEY_EVENT_H_
#define EPOCANVA_WINDOWS_INPUT_KEY_EVENT_H_

#include <cstdint>
#include <string>

namespace epocanva {
namespace windows {
namespace input {

// Virtual-key codes transcribed from the installed SDK rather than including <windows.h>:
// winuser.h:479 VK_BACK 0x08, :487 VK_RETURN 0x0D, :509 VK_ESCAPE 0x1B, :516 VK_SPACE 0x20.
// They are key CODES; the characters they may produce are a separate question answered by
// translation. A unit test pins all four numbers, so an accidental edit cannot pass silently.
constexpr std::uint32_t kVirtualKeyBack = 0x08;
constexpr std::uint32_t kVirtualKeyReturn = 0x0D;
constexpr std::uint32_t kVirtualKeyEscape = 0x1B;
constexpr std::uint32_t kVirtualKeySpace = 0x20;

// Which edge of the key this callback describes.
enum class KeyTransition {
  kKeyDown = 0,
  kKeyUp = 1,
};

// The modifier facts of the event.
struct ModifierSnapshot {
  bool shift = false;
  bool control = false;
  bool alt = false;
  bool windows = false;
  bool caps_lock = false;  // toggle state; not a modifier and not a blocker by itself
};

// One host callback, frozen. Everything here is data supplied by the adapter; nothing is
// read again while a decision is made, so evaluating the same value twice cannot observe a
// different keyboard.
struct KeyEventSnapshot {
  std::uint32_t virtual_key = 0;  // Win32 virtual-key code
  std::uint32_t scan_code = 0;    // hardware scan code (make code for a down event)
  bool is_extended = false;       // extended-key flag, carried for a future TSF adapter
  KeyTransition transition = KeyTransition::kKeyDown;
  std::uint32_t repeat_count = 1;  // host repeat count; metadata only, never expanded
  bool previously_down = false;    // host previous-key-state bit; metadata only
  ModifierSnapshot modifiers;
};

// What this field is allowed to do with typed text. This is the field-level privacy
// classification, not the service's enabled flag:
//   kAllowed    ordinary editable field
//   kForbidden  password / one-time-code / otherwise privacy-classified
//   kUnknown    the caller could not classify it
// kUnknown is NOT treated as kAllowed. It is conservative: no engine candidate is produced,
// because forwarding is the step that would start composition or learning. A future
// learning layer must treat both non-kAllowed values as "do not learn".
enum class InputPermission {
  kAllowed = 0,
  kForbidden = 1,
  kUnknown = 2,
};

// The non-keyboard facts a decision needs. Defaults are deliberately the conservative ones:
// a default context describes a disabled service in a field of unknown permission with no
// engine, which can only produce a pass-through.
struct InputContext {
  bool input_enabled = false;       // the service is enabled for this context
  bool engine_ready = false;        // an engine session exists and can accept input
  bool composition_active = false;  // a composition currently exists in the host
  InputPermission permission = InputPermission::kUnknown;
};

// The outcome of translating one key into text. The translation itself is NOT done here; it
// is done by key_text_translation.{h,cc}, and the pure decision layer only reads the result.
//
// kText carries exactly the code units ToUnicodeEx reported -- no NUL terminator is assumed,
// no trailing buffer contents are copied, and a multi-code-unit result is not truncated to
// its first unit. The "whole result or nothing" rule is enforced in DecideKey.
enum class KeyTextStatus {
  kText = 0,             // one or more UTF-16 code units, exactly the returned count
  kNoText,               // ToUnicodeEx returned 0: nothing was written
  kDeadKey,              // ToUnicodeEx returned < 0: a dead key; its buffer is not input
  kNotKeyDown,           // the adapter refused a key-up event without calling the API
  kNoLayout,             // no input locale identifier was supplied
  kInvalidText,          // the returned code units were not well-formed UTF-16
  kSystemFailure,        // the translation call or its buffer handling failed
};

struct KeyText {
  KeyTextStatus status = KeyTextStatus::kNoText;
  // Meaningful only when status == kText. Length is the exact returned code unit count.
  std::u16string utf16_code_units;
};

// A stable, ASCII name for a translation status, for logs that must not carry text.
const char* DescribeKeyTextStatus(KeyTextStatus status);

}  // namespace input
}  // namespace windows
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_INPUT_KEY_EVENT_H_
