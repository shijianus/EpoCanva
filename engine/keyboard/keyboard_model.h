// Project-authored ASCII-letter keyboard geometry for spelling ranking only.
// No platform/layout detection, key translation, input history or I/O. Callers
// select the active layout explicitly; unknown never implies US QWERTY.
#ifndef EPOCANVA_KEYBOARD_MODEL_H_
#define EPOCANVA_KEYBOARD_MODEL_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace epocanva::keyboard {

// Explicit stable values for platform/backend adapters. The options of legacy
// callers default to kUsQwerty, NOT to zero. Unsupported values behave like
// kUnknown: ordinary edit costs without any geometry-based discount.
enum class Layout : std::uint32_t {
  kUnknown = 0,
  kNeutral = kUnknown,
  kUsQwerty = 1,
  kFrenchAzerty = 2,
  kGermanQwertz = 3,
  kDvorak = 4,
};

namespace detail {
struct KeyPoint {
  int horizontal_quarter_key_units = 0;
  int row_ordinal = 0;  // physical key rows, not an input-text offset
  bool found = false;
};
inline KeyPoint FindKey(const std::array<std::string_view, 3>& rows,
                        char letter_ascii_byte) {
  constexpr int row_offsets_quarter_key_units[] = {0, 1, 3};
  for (std::size_t row_ordinal = 0; row_ordinal < rows.size(); ++row_ordinal) {
    // Each row UTF-8 byte represents one physical key column, including gaps.
    const auto column_utf8_byte = rows[row_ordinal].find(letter_ascii_byte);
    if (column_utf8_byte != std::string_view::npos) {
      return {static_cast<int>(column_utf8_byte) * 4 +
                  row_offsets_quarter_key_units[row_ordinal],
              static_cast<int>(row_ordinal), true};
    }
  }
  return {};
}
}  // namespace detail

// Cost in hundredths of an edit: equal bytes 0, neighbouring lowercase ASCII
// letters 60, all other substitutions 100. This is the ASCII-letter subset of
// traditional French AZERTY, German QWERTZ, US QWERTY and US Dvorak. Spaces keep
// Dvorak's punctuation-key columns; deleting them would invent neighbours.
// Adjacent means <=1 physical row and <=5 quarter-key units horizontally, the
// same row-stagger approximation as the legacy English US-QWERTY scorer. It is
// not a claim about a user's hardware, Unicode/dead keys or a custom layout.
inline unsigned SubstitutionCost(Layout layout, char expected_ascii_byte,
                                 char input_ascii_byte) {
  if (expected_ascii_byte == input_ascii_byte) return 0;
  if (expected_ascii_byte < 'a' || expected_ascii_byte > 'z' ||
      input_ascii_byte < 'a' || input_ascii_byte > 'z') return 100;
  static constexpr std::array<std::string_view, 3> qwerty{
      "qwertyuiop", "asdfghjkl", "zxcvbnm"};
  static constexpr std::array<std::string_view, 3> azerty{
      "azertyuiop", "qsdfghjklm", "wxcvbn"};
  static constexpr std::array<std::string_view, 3> qwertz{
      "qwertzuiop", "asdfghjkl", "yxcvbnm"};
  static constexpr std::array<std::string_view, 3> dvorak{
      "   pyfgcrl", "aoeuidhtns", " qjkxbmwvz"};
  const std::array<std::string_view, 3>* rows = nullptr;
  switch (layout) {
    case Layout::kUsQwerty: rows = &qwerty; break;
    case Layout::kFrenchAzerty: rows = &azerty; break;
    case Layout::kGermanQwertz: rows = &qwertz; break;
    case Layout::kDvorak: rows = &dvorak; break;
    default: return 100;  // unknown, neutral AND unsupported enum values
  }
  const auto expected = detail::FindKey(*rows, expected_ascii_byte);
  const auto input = detail::FindKey(*rows, input_ascii_byte);
  if (!expected.found || !input.found) return 100;
  const int horizontal = expected.horizontal_quarter_key_units -
                         input.horizontal_quarter_key_units;
  const int vertical = expected.row_ordinal - input.row_ordinal;
  return horizontal >= -5 && horizontal <= 5 && vertical >= -1 && vertical <= 1
             ? 60u : 100u;
}

}  // namespace epocanva::keyboard
#endif  // EPOCANVA_KEYBOARD_MODEL_H_
