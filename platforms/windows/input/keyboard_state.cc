// EpoCanva P3.3 -- the Windows keyboard-state snapshot.
//
// The only system calls in this file are GetKeyboardState and GetKeyboardLayout. There is no
// SetKeyboardState, no GetAsyncKeyState, no AttachThreadInput, no layout activation and no
// registry access. The 256 bytes are treated as state, never as text: no logging call in this
// file can print them.
#include "keyboard_state.h"

namespace epocanva {
namespace windows {
namespace input {
namespace {

bool IsDown(std::uint8_t key_state_byte) {
  return (key_state_byte & 0x80u) != 0;
}

bool IsToggled(std::uint8_t key_state_byte) {
  return (key_state_byte & 0x01u) != 0;
}

}  // namespace

ModifierSnapshot DeriveModifierSnapshot(
    const std::array<std::uint8_t, kKeyboardStateByteCount>& key_state_bytes) {
  ModifierSnapshot modifiers;
  // VK_SHIFT/VK_CONTROL/VK_MENU are documented as indices into this array that cover both
  // left and right instances (GetKeyboardState documentation, "An application can use the
  // virtual-key code constants VK_SHIFT, VK_CONTROL and VK_MENU as indices"). Windows has two
  // separate Windows keys, so both are checked explicitly.
  modifiers.shift = IsDown(key_state_bytes[VK_SHIFT]);
  modifiers.control = IsDown(key_state_bytes[VK_CONTROL]);
  modifiers.alt = IsDown(key_state_bytes[VK_MENU]);
  modifiers.windows =
      IsDown(key_state_bytes[VK_LWIN]) || IsDown(key_state_bytes[VK_RWIN]);
  // Caps Lock is a TOGGLE: the low bit is the on/off state, not the key being held.
  modifiers.caps_lock = IsToggled(key_state_bytes[VK_CAPITAL]);
  return modifiers;
}

KeyboardStateSnapshot KeyboardStateSnapshot::FromExplicitState(
    std::array<std::uint8_t, kKeyboardStateByteCount> key_state_bytes, HKL layout) {
  KeyboardStateSnapshot snapshot;
  snapshot.key_state_bytes_ = key_state_bytes;
  snapshot.layout_ = layout;
  snapshot.modifiers_ = DeriveModifierSnapshot(snapshot.key_state_bytes_);
  return snapshot;
}

const std::array<std::uint8_t, kKeyboardStateByteCount>&
KeyboardStateSnapshot::key_state_bytes() const {
  return key_state_bytes_;
}

HKL KeyboardStateSnapshot::layout() const { return layout_; }

const ModifierSnapshot& KeyboardStateSnapshot::modifiers() const { return modifiers_; }

bool KeyboardStateSnapshot::has_layout() const { return layout_ != nullptr; }

KeyboardStateCaptureResult CaptureThreadKeyboardState() {
  KeyboardStateCaptureResult result;

  std::array<std::uint8_t, kKeyboardStateByteCount> key_state_bytes{};
  if (::GetKeyboardState(reinterpret_cast<PBYTE>(key_state_bytes.data())) == FALSE) {
    result.status = KeyboardStateCaptureStatus::kSystemCallFailed;
    result.os_error = ::GetLastError();
    result.diagnostic = "GetKeyboardState stage=capture gle=" + std::to_string(result.os_error);
    return result;
  }

  const HKL layout = ::GetKeyboardLayout(0);
  if (layout == nullptr) {
    result.status = KeyboardStateCaptureStatus::kNoLayout;
    result.diagnostic = "GetKeyboardLayout stage=capture returned NULL";
    return result;
  }

  result.status = KeyboardStateCaptureStatus::kOk;
  result.snapshot = KeyboardStateSnapshot::FromExplicitState(key_state_bytes, layout);
  return result;
}

const char* DescribeKeyboardStateCaptureStatus(KeyboardStateCaptureStatus status) {
  switch (status) {
    case KeyboardStateCaptureStatus::kOk:
      return "ok";
    case KeyboardStateCaptureStatus::kSystemCallFailed:
      return "system-call-failed";
    case KeyboardStateCaptureStatus::kNoLayout:
      return "no-layout";
  }
  return "unknown";
}

}  // namespace input
}  // namespace windows
}  // namespace epocanva
