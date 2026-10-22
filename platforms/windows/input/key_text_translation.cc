// EpoCanva P3.3 -- the non-mutating ToUnicodeEx adapter.
//
// No state mutation, no text logging, no layout change. The return-value rules are documented
// in key_text_translation.h; this file applies them and nothing else.
#include "key_text_translation.h"

#include <array>
#include <cstddef>
#include <string>

#include "text_encoding.h"

namespace epocanva {
namespace windows {
namespace input {
namespace {

KeyTranslationResult Refuse(KeyTextStatus status, const char* stage) {
  KeyTranslationResult result;
  result.text.status = status;
  result.diagnostic = std::string("ToUnicodeEx stage=") + stage;
  return result;
}

}  // namespace

KeyTranslationResult TranslateKeyEvent(const KeyEventSnapshot& event,
                                       const KeyboardStateSnapshot& snapshot) {
  // A release is not part of the English text path this round, and translating it would be a
  // second API observation of the same physical key. Refuse before any call.
  if (event.transition == KeyTransition::kKeyUp) {
    return Refuse(KeyTextStatus::kNotKeyDown, "key-up-skipped");
  }
  if (!snapshot.has_layout()) {
    return Refuse(KeyTextStatus::kNoLayout, "layout-missing");
  }

  // Filled with a value no real translation is expected to contain, so a test that reads
  // beyond the returned count would see the sentinel rather than plausible text.
  std::array<wchar_t, kTranslationBufferUtf16CodeUnits> buffer{};
  buffer.fill(static_cast<wchar_t>(0xFFFF));

  KeyTranslationResult result;
  result.system_call_made = true;
  const int raw = ::ToUnicodeEx(
      static_cast<UINT>(event.virtual_key), static_cast<UINT>(event.scan_code),
      reinterpret_cast<const BYTE*>(snapshot.key_state_bytes().data()),
      buffer.data(), static_cast<int>(buffer.size()), kToUnicodeNoKeyboardStateChangeFlag,
      snapshot.layout());
  result.raw_return_value = raw;

  if (raw < 0) {
    // The buffer may hold the spacing form of the dead key. It is not input; drop it whole.
    result.text.status = KeyTextStatus::kDeadKey;
    result.text.utf16_code_units.clear();
    result.diagnostic = "ToUnicodeEx stage=dead-key raw=" + std::to_string(raw);
    return result;
  }
  if (raw == 0) {
    result.text.status = KeyTextStatus::kNoText;
    result.text.utf16_code_units.clear();
    result.diagnostic = "ToUnicodeEx stage=no-text raw=0";
    return result;
  }
  if (raw >= static_cast<int>(buffer.size())) {
    // The API cannot write more than cchBuff, so a return equal to the capacity is either an
    // exact fit or a truncated result; they are indistinguishable. Refuse instead of claiming
    // a complete translation.
    result.text.status = KeyTextStatus::kSystemFailure;
    result.text.utf16_code_units.clear();
    result.diagnostic = "ToUnicodeEx stage=translate raw=" + std::to_string(raw) +
                        " capacity=" + std::to_string(buffer.size()) + " refused";
    return result;
  }

  // Only the returned count is meaningful (the buffer may contain extra invalid characters
  // past it), so the length is explicit and never a NUL scan.
  result.text.utf16_code_units.clear();
  result.text.utf16_code_units.reserve(static_cast<std::size_t>(raw));
  for (int index = 0; index < raw; ++index) {
    // P3.2's header asserts sizeof(wchar_t) == 2; this is a unit-preserving widening, not a
    // conversion.
    result.text.utf16_code_units.push_back(static_cast<char16_t>(buffer[index]));
  }

  const std::wstring utf16_code_units(buffer.data(), static_cast<std::size_t>(raw));
  const text::Utf8ConversionResult converted =
      text::ConvertUtf16CodeUnitsToUtf8Bytes(utf16_code_units);
  if (!converted.ok()) {
    // Nothing half-converted escapes, and the diagnostic is the P3.2 one (text-free).
    result.text.status = KeyTextStatus::kInvalidText;
    result.text.utf16_code_units.clear();
    result.utf8_bytes.clear();
    result.os_error = converted.os_error;
    result.diagnostic = "ToUnicodeEx stage=convert raw=" + std::to_string(raw) + " " +
                        converted.diagnostic;
    return result;
  }

  result.text.status = KeyTextStatus::kText;
  result.utf8_bytes = converted.utf8_bytes;
  result.diagnostic = "ToUnicodeEx stage=translate raw=" + std::to_string(raw) +
                      " utf8-bytes=" + std::to_string(result.utf8_bytes.size());
  return result;
}

}  // namespace input
}  // namespace windows
}  // namespace epocanva
