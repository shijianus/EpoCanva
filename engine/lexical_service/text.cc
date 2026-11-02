#include "text.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "english_token.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winnls.h>
#endif

namespace epocanva::lexical {
namespace {

// Decoding is the UTF-8 encoding algorithm, not a Unicode normalization table.
// cursor_utf8_byte always names a byte boundary in the original supplied text.
bool DecodeScalar(std::string_view text, std::size_t* cursor_utf8_byte,
                  char32_t* scalar) {
  const auto begin_utf8_byte = *cursor_utf8_byte;
  if (begin_utf8_byte >= text.size()) return false;
  const auto lead = static_cast<unsigned char>(text[begin_utf8_byte]);
  std::size_t scalar_utf8_bytes = 0;
  std::uint32_t value = 0;
  std::uint32_t minimum = 0;
  if (lead < 0x80) {
    scalar_utf8_bytes = 1;
    value = lead;
  } else if (lead >= 0xc2 && lead <= 0xdf) {
    scalar_utf8_bytes = 2;
    value = lead & 0x1fu;
    minimum = 0x80;
  } else if (lead >= 0xe0 && lead <= 0xef) {
    scalar_utf8_bytes = 3;
    value = lead & 0x0fu;
    minimum = 0x800;
  } else if (lead >= 0xf0 && lead <= 0xf4) {
    scalar_utf8_bytes = 4;
    value = lead & 0x07u;
    minimum = 0x10000;
  } else {
    return false;
  }
  if (scalar_utf8_bytes > text.size() - begin_utf8_byte) return false;
  for (std::size_t continuation_byte = 1; continuation_byte < scalar_utf8_bytes;
       ++continuation_byte) {
    const auto next = static_cast<unsigned char>(text[begin_utf8_byte + continuation_byte]);
    if ((next & 0xc0u) != 0x80u) return false;
    value = (value << 6u) | (next & 0x3fu);
  }
  if (value < minimum || value > 0x10ffff ||
      (value >= 0xd800 && value <= 0xdfff)) return false;
  *scalar = static_cast<char32_t>(value);
  *cursor_utf8_byte += scalar_utf8_bytes;
  return true;
}

TextResult Failure(TextStatus status, std::size_t error_utf8_byte = kNoUtf8Byte) {
  return {status, {}, error_utf8_byte};
}

bool IsJoiner(char32_t scalar) {
  return scalar == U'\'' || scalar == U'-' || scalar == U'\u2019' ||
         scalar == U'\u2010' || scalar == U'\u2011';
}

struct ScalarProperties {
  bool letter = false;
  bool mark = false;
  bool protected_text = false;
  bool native_failure = false;
};
ScalarProperties Properties(char32_t scalar) {
  if (scalar < 0x80) {
    const auto ascii = static_cast<char>(scalar);
    return {en::IsAsciiLetter(ascii), false,
            scalar >= 0x20 && en::IsProtectedAsciiByte(ascii), false};
  }
#ifdef _WIN32
  // This lexical grammar admits BMP letters/marks as classified by Windows
  // NLS. Supplementary scalars remain valid text and normalize correctly, but
  // are not claimed as word letters through a UTF-16-code-unit classification
  // API. In particular, surrogate flags must not become letter permission.
  if (scalar > 0xffff) return {};
  const wchar_t unit = static_cast<wchar_t>(scalar);
  WORD ctype1 = 0;
  WORD ctype3 = 0;
  // SDK 10.0.26100.0: stringapiset.h:107-112; winnls.h:176-185,210-225.
  if (!GetStringTypeW(CT_CTYPE1, &unit, 1, &ctype1) ||
      !GetStringTypeW(CT_CTYPE3, &unit, 1, &ctype3)) {
    return {false, false, false, true};
  }
  // Windows marks precomposed é with C1_ALPHA AND C3_NONSPACING/DIACRITIC.
  // Letter permission must take precedence. A non-letter needs NONSPACING or
  // VOWELMARK; DIACRITIC alone also describes spacing accents such as U+00B4.
  const bool letter = (ctype1 & C1_ALPHA) != 0;
  const bool mark = !letter && (ctype3 & (C3_NONSPACING | C3_VOWELMARK)) != 0;
  return {letter, mark, (ctype1 & C1_DIGIT) != 0, false};
#else
  return {};
#endif
}

#ifdef _WIN32
bool ToUtf16(std::string_view utf8, std::wstring* out) {
  if (utf8.empty()) { out->clear(); return true; }
  // Explicit counts: source UTF-8 bytes, destination UTF-16 code units. No
  // sentinel -1 (which could truncate embedded NUL or include a terminator).
  const int source_utf8_bytes = static_cast<int>(utf8.size());
  const int required_utf16_units = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), source_utf8_bytes, nullptr, 0);
  if (required_utf16_units <= 0) return false;
  out->resize(static_cast<std::size_t>(required_utf16_units));
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            source_utf8_bytes, out->data(), required_utf16_units) ==
         required_utf16_units;
}
TextResult FromUtf16(const std::wstring& utf16) {
  if (utf16.empty()) return {};
  if (utf16.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return Failure(TextStatus::kInputTooLong);
  }
  const int source_utf16_units = static_cast<int>(utf16.size());
  const int required_utf8_bytes = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(), source_utf16_units,
      nullptr, 0, nullptr, nullptr);
  if (required_utf8_bytes <= 0) return Failure(TextStatus::kNativeFailure);
  if (static_cast<std::size_t>(required_utf8_bytes) > kMaxTextUtf8Bytes) {
    return Failure(TextStatus::kInputTooLong);
  }
  TextResult result;
  result.text_utf8.resize(static_cast<std::size_t>(required_utf8_bytes));
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(),
                         source_utf16_units, result.text_utf8.data(),
                         required_utf8_bytes, nullptr, nullptr) != required_utf8_bytes) {
    return Failure(TextStatus::kNativeFailure);
  }
  return result;
}

bool NormalizeUtf16(const std::wstring& source, std::wstring* destination) {
  if (source.empty()) { destination->clear(); return true; }
  // SDK 10.0.26100.0 winnls.h:1359-1366,2873-2879 declares NFC and UTF-16
  // source/destination counts. The zero-buffer result is a capacity estimate;
  // retry ERROR_INSUFFICIENT_BUFFER rather than treating the estimate as exact.
  const int source_utf16_units = static_cast<int>(source.size());
  int capacity_utf16_units = NormalizeString(
      NormalizationC, source.data(), source_utf16_units, nullptr, 0);
  if (capacity_utf16_units <= 0) return false;
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (static_cast<std::size_t>(capacity_utf16_units) > kMaxTextUtf8Bytes * 4) return false;
    destination->resize(static_cast<std::size_t>(capacity_utf16_units));
    SetLastError(ERROR_SUCCESS);
    const int written_utf16_units = NormalizeString(
        NormalizationC, source.data(), source_utf16_units,
        destination->data(), capacity_utf16_units);
    if (written_utf16_units > 0) {
      destination->resize(static_cast<std::size_t>(written_utf16_units));
      return true;
    }
    if (written_utf16_units >= 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        written_utf16_units == std::numeric_limits<int>::min()) return false;
    const int required_utf16_units = -written_utf16_units;
    if (required_utf16_units <= capacity_utf16_units) return false;
    capacity_utf16_units = required_utf16_units;
  }
  return false;
}

TextResult MapNativeCase(std::string_view utf8, DWORD flags) {
  std::wstring source;
  if (!ToUtf16(utf8, &source)) return Failure(TextStatus::kNativeFailure);
  if (source.empty()) return {};
  const int source_utf16_units = static_cast<int>(source.size());
  // SDK winnls.h:260-261,2920,3087-3097. Invariant lower/upper are not full
  // Unicode CaseFolding.txt and are not compatibility/accent folding.
  const int required_utf16_units = LCMapStringEx(
      LOCALE_NAME_INVARIANT, flags, source.data(), source_utf16_units,
      nullptr, 0, nullptr, nullptr, 0);
  if (required_utf16_units <= 0 ||
      static_cast<std::size_t>(required_utf16_units) > kMaxTextUtf8Bytes * 4) {
    return Failure(TextStatus::kNativeFailure);
  }
  std::wstring mapped(static_cast<std::size_t>(required_utf16_units), L'\0');
  const int written_utf16_units = LCMapStringEx(
      LOCALE_NAME_INVARIANT, flags, source.data(), source_utf16_units,
      mapped.data(), required_utf16_units, nullptr, nullptr, 0);
  if (written_utf16_units <= 0) return Failure(TextStatus::kNativeFailure);
  mapped.resize(static_cast<std::size_t>(written_utf16_units));
  std::wstring normalized;
  if (!NormalizeUtf16(mapped, &normalized)) return Failure(TextStatus::kNativeFailure);
  return FromUtf16(normalized);
}
#endif

TextResult MapCase(std::string_view text, bool upper) {
  const auto normalized = NormalizeNfcUtf8(text);
  if (normalized.status != TextStatus::kOk) return normalized;
  if (IsAscii(normalized.text_utf8)) {
    auto output = normalized;
    for (char& byte : output.text_utf8) {
      if (upper && byte >= 'a' && byte <= 'z') byte = static_cast<char>(byte - 'a' + 'A');
      if (!upper && byte >= 'A' && byte <= 'Z') byte = static_cast<char>(byte - 'A' + 'a');
    }
    return output;
  }
#ifdef _WIN32
  return MapNativeCase(normalized.text_utf8, upper ? LCMAP_UPPERCASE : LCMAP_LOWERCASE);
#else
  return Failure(TextStatus::kUnicodeUnavailable);
#endif
}

TextResult InitialCapital(std::string_view lowercase_utf8) {
  TextResult result{TextStatus::kOk, std::string(lowercase_utf8), kNoUtf8Byte};
  std::size_t cursor_utf8_byte = 0;
  while (cursor_utf8_byte < lowercase_utf8.size()) {
    const auto begin_utf8_byte = cursor_utf8_byte;
    char32_t scalar = 0;
    if (!DecodeScalar(lowercase_utf8, &cursor_utf8_byte, &scalar)) {
      return Failure(TextStatus::kInvalidUtf8, begin_utf8_byte);
    }
    const auto original = lowercase_utf8.substr(begin_utf8_byte,
                                                cursor_utf8_byte - begin_utf8_byte);
    const auto upper = UpperUtf8(original);
    if (upper.status != TextStatus::kOk) return upper;
    if (original != upper.text_utf8) {
      result.text_utf8.replace(begin_utf8_byte, cursor_utf8_byte - begin_utf8_byte,
                               upper.text_utf8);
      if (result.text_utf8.size() > kMaxTextUtf8Bytes) return Failure(TextStatus::kInputTooLong);
      return result;
    }
  }
  return result;
}

}  // namespace

TextStatus ValidateUtf8(std::string_view text_utf8, std::size_t* error_utf8_byte) {
  if (error_utf8_byte) *error_utf8_byte = kNoUtf8Byte;
  if (text_utf8.size() > kMaxTextUtf8Bytes) return TextStatus::kInputTooLong;
  std::size_t cursor_utf8_byte = 0;
  while (cursor_utf8_byte < text_utf8.size()) {
    const auto begin_utf8_byte = cursor_utf8_byte;
    char32_t scalar = 0;
    if (!DecodeScalar(text_utf8, &cursor_utf8_byte, &scalar)) {
      if (error_utf8_byte) *error_utf8_byte = begin_utf8_byte;
      return TextStatus::kInvalidUtf8;
    }
  }
  return TextStatus::kOk;
}

bool IsUtf8ByteBoundary(std::string_view text_utf8, std::size_t offset_utf8_byte) {
  return offset_utf8_byte <= text_utf8.size() &&
         ValidateUtf8(text_utf8) == TextStatus::kOk &&
         (offset_utf8_byte == text_utf8.size() ||
          (static_cast<unsigned char>(text_utf8[offset_utf8_byte]) & 0xc0u) != 0x80u);
}
bool IsAscii(std::string_view text_utf8) {
  return std::all_of(text_utf8.begin(), text_utf8.end(),
                     [](unsigned char byte) { return byte < 0x80; });
}
bool HasNativeUnicodeSupport() {
#ifdef _WIN32
  return true;
#else
  return false;
#endif
}

TextResult NormalizeNfcUtf8(std::string_view text_utf8) {
  std::size_t error_utf8_byte = kNoUtf8Byte;
  const auto status = ValidateUtf8(text_utf8, &error_utf8_byte);
  if (status != TextStatus::kOk) return Failure(status, error_utf8_byte);
  if (IsAscii(text_utf8)) return {TextStatus::kOk, std::string(text_utf8), kNoUtf8Byte};
#ifdef _WIN32
  std::wstring source;
  std::wstring normalized;
  if (!ToUtf16(text_utf8, &source) || !NormalizeUtf16(source, &normalized)) {
    return Failure(TextStatus::kNativeFailure);
  }
  return FromUtf16(normalized);
#else
  return Failure(TextStatus::kUnicodeUnavailable);
#endif
}
TextResult FoldUtf8(std::string_view text_utf8) { return MapCase(text_utf8, false); }
TextResult UpperUtf8(std::string_view text_utf8) { return MapCase(text_utf8, true); }

WordAnalysis AnalyzeWordUtf8(std::string_view raw_utf8, bool allow_trailing_joiner) {
  WordAnalysis result;
  if (raw_utf8.size() > kMaxWordUtf8Bytes) {
    result.status = TextStatus::kInputTooLong;
    return result;
  }
  const auto normalized = NormalizeNfcUtf8(raw_utf8);
  result.status = normalized.status;
  if (result.status != TextStatus::kOk) return result;
  result.normalized_utf8 = normalized.text_utf8;
  if (result.normalized_utf8.empty()) {
    result.status = TextStatus::kInvalidWord;
    return result;
  }
  bool previous_is_letter_or_mark = false;
  bool contains_letter = false;
  bool invalid_word = false;
  std::size_t cursor_utf8_byte = 0;
  while (cursor_utf8_byte < result.normalized_utf8.size()) {
    char32_t scalar = 0;
    if (!DecodeScalar(result.normalized_utf8, &cursor_utf8_byte, &scalar)) {
      result.status = TextStatus::kNativeFailure;
      return result;
    }
    const auto properties = Properties(scalar);
    if (properties.native_failure) {
      result.status = TextStatus::kNativeFailure;
      return result;
    }
    // Detect technical text across the WHOLE token even after a nonletter;
    // never normalize/correct just the apparently word-like prefix or tail.
    if (properties.protected_text) {
      result.status = TextStatus::kProtectedText;
      return result;
    }
    if (properties.letter) {
      contains_letter = true;
      previous_is_letter_or_mark = true;
    } else if (properties.mark) {
      if (!previous_is_letter_or_mark) invalid_word = true;
    } else if (IsJoiner(scalar)) {
      if (!previous_is_letter_or_mark) invalid_word = true;
      previous_is_letter_or_mark = false;
    } else {
      invalid_word = true;
      previous_is_letter_or_mark = false;
    }
  }
  if (!contains_letter || invalid_word ||
      (!previous_is_letter_or_mark && !allow_trailing_joiner)) {
    result.status = TextStatus::kInvalidWord;
    return result;
  }
  const auto lower = FoldUtf8(result.normalized_utf8);
  const auto upper = UpperUtf8(result.normalized_utf8);
  if (lower.status != TextStatus::kOk || upper.status != TextStatus::kOk) {
    result.status = lower.status != TextStatus::kOk ? lower.status : upper.status;
    return result;
  }
  if (lower.text_utf8.size() > kMaxWordUtf8Bytes) {
    result.status = TextStatus::kInputTooLong;
    return result;
  }
  result.folded_utf8 = lower.text_utf8;
  if (lower.text_utf8 == upper.text_utf8) {
    result.case_style = CaseStyle::kUncased;
  } else if (result.normalized_utf8 == lower.text_utf8) {
    result.case_style = CaseStyle::kLower;
  } else {
    const auto initial = InitialCapital(lower.text_utf8);
    if (initial.status != TextStatus::kOk) {
      result.status = initial.status;
      return result;
    }
    if (result.normalized_utf8 == initial.text_utf8) {
      result.case_style = CaseStyle::kInitialCapital;
    } else if (result.normalized_utf8 == upper.text_utf8) {
      result.case_style = CaseStyle::kUpper;
    } else {
      result.case_style = CaseStyle::kMixed;
      result.status = TextStatus::kMixedCase;
    }
  }
  return result;
}

TextResult ApplyCaseStyleUtf8(std::string_view canonical_utf8, CaseStyle style) {
  if (style == CaseStyle::kMixed) return Failure(TextStatus::kMixedCase);
  if (style == CaseStyle::kUpper) return UpperUtf8(canonical_utf8);
  const auto lower = FoldUtf8(canonical_utf8);
  if (lower.status != TextStatus::kOk || style != CaseStyle::kInitialCapital) return lower;
  return InitialCapital(lower.text_utf8);
}

const char* TextStatusName(TextStatus status) {
  switch (status) {
    case TextStatus::kOk: return "ok";
    case TextStatus::kInvalidUtf8: return "invalid-utf8";
    case TextStatus::kInputTooLong: return "input-too-long";
    case TextStatus::kUnicodeUnavailable: return "unicode-unavailable";
    case TextStatus::kNativeFailure: return "native-normalization-failure";
    case TextStatus::kInvalidWord: return "invalid-word";
    case TextStatus::kProtectedText: return "protected-text";
    case TextStatus::kMixedCase: return "mixed-case";
  }
  return "unknown";
}

}  // namespace epocanva::lexical
