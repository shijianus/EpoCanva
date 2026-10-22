// EpoCanva P3.3 -- a frozen 256-byte keyboard state and the layout it is translated with.
//
// WHAT THIS IS. One capture of the calling thread's keyboard state (GetKeyboardState, a
// 256-BYTE array) plus one explicitly named input locale identifier (HKL). The modifiers are
// derived ONCE, at construction, and exposed read-only. Translation and decision both read
// the same frozen object; nothing re-queries the keyboard halfway through a decision.
//
// WHAT THIS IS NOT. It does not set keyboard state (SetKeyboardState is never called), does
// not poll asynchronous key state (GetAsyncKeyState is never called), does not install,
// load or activate a layout, and does not read a file or the registry. The bytes are state,
// not text: they are never logged.
//
// WHY THE LAYOUT IS EXPLICIT. ToUnicodeEx takes an HKL. If the adapter silently used "the
// current layout" at translation time, a layout switch between the event and the translation
// would silently change the character. Here the caller chooses the HKL when the snapshot is
// made, and the snapshot keeps it. CaptureThreadKeyboardState() obtains the calling thread's
// active HKL at the adapter boundary, which is the one place that choice is made for the
// production path; tests pass an HKL from the installed layout list and never activate it.
//
// UNITS (AGENTS.md section 5). key_state_bytes is a 256-BYTE array indexed by virtual-key
// code. Each byte's high bit means "down"; the low bit is the toggle state (meaningful for
// CAPS LOCK, whose low bit this layer reads). The bytes are not UTF-16 code units, not
// characters and not scan codes.
#ifndef EPOCANVA_WINDOWS_INPUT_KEYBOARD_STATE_H_
#define EPOCANVA_WINDOWS_INPUT_KEYBOARD_STATE_H_

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "key_event.h"

namespace epocanva {
namespace windows {
namespace input {

// The number of BYTES GetKeyboardState fills. Fixed at 256 by the API, not by this project.
constexpr std::size_t kKeyboardStateByteCount = 256;

// Derive Shift/Control/Alt/Windows/Caps-Lock from a 256-byte keyboard state. Pure over the
// supplied bytes: it reads no global state, so a test can prove the answer comes from the
// snapshot. Left and right instances are collapsed; AltGr is Control+Alt and stays that way.
ModifierSnapshot DeriveModifierSnapshot(
    const std::array<std::uint8_t, kKeyboardStateByteCount>& key_state_bytes);

// An immutable keyboard snapshot. Copyable; every accessor is const; no setter exists.
class KeyboardStateSnapshot {
 public:
  KeyboardStateSnapshot() = default;

  // The only way to fill one. `key_state_bytes` is copied; `layout` may be null, in which
  // case has_layout() is false and translation refuses rather than guessing a layout.
  static KeyboardStateSnapshot FromExplicitState(
      std::array<std::uint8_t, kKeyboardStateByteCount> key_state_bytes, HKL layout);

  const std::array<std::uint8_t, kKeyboardStateByteCount>& key_state_bytes() const;
  HKL layout() const;
  const ModifierSnapshot& modifiers() const;
  bool has_layout() const;

 private:
  std::array<std::uint8_t, kKeyboardStateByteCount> key_state_bytes_{};
  HKL layout_ = nullptr;
  ModifierSnapshot modifiers_;
};

enum class KeyboardStateCaptureStatus {
  kOk = 0,
  kSystemCallFailed,  // GetKeyboardState returned zero
  kNoLayout,          // GetKeyboardLayout(0) returned null
};

// The outcome of one capture. On any non-kOk status the snapshot is the default (all zero,
// null layout) and must not be translated with.
struct KeyboardStateCaptureResult {
  KeyboardStateCaptureStatus status = KeyboardStateCaptureStatus::kSystemCallFailed;
  KeyboardStateSnapshot snapshot;
  std::uint32_t os_error = 0;   // GetLastError() when GetKeyboardState failed; else 0
  std::string diagnostic;        // API + stage + gle; never the 256 bytes
  bool ok() const { return status == KeyboardStateCaptureStatus::kOk; }
};

// Capture the calling thread's keyboard state and active HKL. This is the ONLY production
// path that chooses a layout, and it does so once, at the adapter boundary.
KeyboardStateCaptureResult CaptureThreadKeyboardState();

const char* DescribeKeyboardStateCaptureStatus(KeyboardStateCaptureStatus status);

}  // namespace input
}  // namespace windows
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_INPUT_KEYBOARD_STATE_H_
