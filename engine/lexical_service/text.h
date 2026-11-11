// Immutable, local lexical candidates. No network, persistence or content logs.
// Unicode NFC/casing support is native Windows; other platforms explicitly
// report kUnicodeUnavailable for non-ASCII rather than claiming Unicode tables.
#ifndef EPOCANVA_LEXICAL_TEXT_H_
#define EPOCANVA_LEXICAL_TEXT_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace epocanva::lexical {

constexpr std::size_t kMaxTextUtf8Bytes = 4096;
constexpr std::size_t kMaxWordUtf8Bytes = 256;
constexpr std::size_t kNoUtf8Byte = static_cast<std::size_t>(-1);

enum class TextStatus {
  kOk = 0,
  kInvalidUtf8,
  kInputTooLong,
  kUnicodeUnavailable,
  kNativeFailure,
  kInvalidWord,
  kProtectedText,
  kMixedCase,
};
const char* TextStatusName(TextStatus status);

struct TextResult {
  TextStatus status = TextStatus::kOk;
  std::string text_utf8;  // UTF-8 code units (bytes); empty on failure.
  std::size_t error_utf8_byte = kNoUtf8Byte;
};

// All offsets are original UTF-8 code units (bytes), never UTF-16 units,
// grapheme clusters or screen-cell columns. Invalid text is never repaired.
TextStatus ValidateUtf8(std::string_view text_utf8,
                        std::size_t* error_utf8_byte = nullptr);
bool IsUtf8ByteBoundary(std::string_view text_utf8, std::size_t offset_utf8_byte);
bool IsAscii(std::string_view text_utf8);
bool HasNativeUnicodeSupport();
TextResult NormalizeNfcUtf8(std::string_view text_utf8);
// NFC -> invariant lowercase -> NFC. This is the catalog's case-insensitive
// lookup mapping, NOT Unicode full case folding (e.g. no promise ss == sharp-s).
TextResult FoldUtf8(std::string_view text_utf8);
TextResult UpperUtf8(std::string_view text_utf8);

enum class CaseStyle { kUncased, kLower, kInitialCapital, kUpper, kMixed };
struct WordAnalysis {
  TextStatus status = TextStatus::kOk;
  CaseStyle case_style = CaseStyle::kUncased;
  std::string normalized_utf8;
  std::string folded_utf8;
};
// BMP letters/combining marks classified by Windows NLS, with internal
// apostrophe/hyphen joiners. Supplementary scalars are valid in text helpers but
// this lexical grammar does not claim their classification. Rejects technical
// punctuation/digits, symbols and whitespace as whole tokens. A
// trailing joiner is allowed only for an interactive prefix. Mixed case is
// reported and preserved by callers, never guessed into a different case.
WordAnalysis AnalyzeWordUtf8(std::string_view raw_utf8,
                             bool allow_trailing_joiner = false);
TextResult ApplyCaseStyleUtf8(std::string_view canonical_utf8, CaseStyle style);

}  // namespace epocanva::lexical
#endif
