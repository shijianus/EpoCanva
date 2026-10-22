// EpoCanva P3.3 -- one key-down translated to UTF-16 through ToUnicodeEx, with no state change.
//
// THE CALL. ToUnicodeEx(virtual_key, scan_code, snapshot.key_state_bytes(),
// buffer, buffer_capacity, kToUnicodeNoKeyboardStateChangeFlag, snapshot.layout()).
// The declaration is the installed SDK's winuser.h:1273-1285; the flag is bit 2, whose
// documented meaning is "keyboard state is not changed (Windows 10, version 1607 and newer)".
// That version floor is a real minimum: this adapter makes NO claim to work on Windows
// versions before 10.0.1607, and no fallback that would change keyboard state is implemented.
// Bit 0 (menu active) and bit 1 (translate break events) are NOT set; every other bit is
// reserved by the API and is not set either.
//
// WHY THE FLAG MATTERS. Without bit 2 ToUnicodeEx changes the kernel-mode keyboard buffer:
// dead keys, ligatures and Alt+Numpad entry are affected, and a prediction call for a key the
// host will also translate would double-apply a dead key. With bit 2 the call is a query. A
// test on a layout that has a dead key checks the property directly: translate the dead key,
// then translate 'a' with a fresh state; 'a' must still be plain 'a'.
//
// RETURN HANDLING, exactly the documented contract:
//   < 0   dead key. The API may have written a SPACING character into the buffer. This
//         adapter discards the buffer and reports kDeadKey; the spacing character is never
//         input. It does not call ToUnicodeEx again to clear the dead key.
//   = 0   no translation, nothing written. kNoText.
//   > 0   exactly `returned` UTF-16 code units are meaningful. The buffer is NOT assumed to
//         be NUL-terminated and may contain extra invalid characters beyond the return value;
//         only the returned count is copied. If the count is not smaller than the buffer,
//         the result is refused as kSystemFailure rather than trusted as complete.
//
// STRICT CONVERSION. The accepted units are converted to UTF-8 through P3.2's
// ConvertUtf16CodeUnitsToUtf8Bytes (the same source file, no local codec). A conversion
// failure clears the whole result and reports kInvalidText; no half-converted text escapes.
// This adapter deliberately does NOT decide that non-ASCII or multi-unit text is unsupported:
// it returns the exact units so key_decision.h can reject the whole result and, while a
// composition exists, report an explicit boundary demand. It never takes the first unit.
//
// NO SIDE EFFECTS. No SendInput, no keybd_event, no SetKeyboardState, no layout switch, no
// clipboard, no engine call. A key-up event is refused without calling the API at all.
#ifndef EPOCANVA_WINDOWS_INPUT_KEY_TEXT_TRANSLATION_H_
#define EPOCANVA_WINDOWS_INPUT_KEY_TEXT_TRANSLATION_H_

#include <windows.h>

#include <cstdint>
#include <string>

#include "key_event.h"
#include "keyboard_state.h"

namespace epocanva {
namespace windows {
namespace input {

// winuser.h wFlags bit 2; the value is transcribed from the ToUnicodeEx documentation.
constexpr UINT kToUnicodeNoKeyboardStateChangeFlag = 0x0004u;

// The adapter's own buffer, in UTF-16 CODE UNITS. Not an API limit: it is large enough for
// any keyboard-layout result this project expects, and a result that fills it is refused
// because truncation cannot be distinguished from an exact fit.
constexpr int kTranslationBufferUtf16CodeUnits = 32;

struct KeyTranslationResult {
  KeyText text;                       // what key_decision.h consumes
  int raw_return_value = 0;           // ToUnicodeEx's int: <0 dead, 0 none, >0 units
  std::string utf8_bytes;             // strict P3.2 conversion; kText only
  std::uint32_t os_error = 0;         // carried from a failed conversion; else 0
  std::string diagnostic;             // API + stage + counts; never the text
  bool system_call_made = false;      // false for key-up and missing-layout refusals
  bool ok_for_text() const { return text.status == KeyTextStatus::kText; }
};

// Translate one key-down. `event` supplies the virtual-key and scan codes; `snapshot` supplies
// the 256-byte state and the HKL. The translation does not use event.modifiers (the state
// bytes are authoritative) and does not use repeat_count or previously_down.
KeyTranslationResult TranslateKeyEvent(const KeyEventSnapshot& event,
                                       const KeyboardStateSnapshot& snapshot);

}  // namespace input
}  // namespace windows
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_INPUT_KEY_TEXT_TRANSLATION_H_
