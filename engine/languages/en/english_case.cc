// EpoCanva P2.2 -- ASCII letter-case implementation.
//
// No <cctype> and no std::tolower/std::toupper: both are locale-dependent, and the
// mapping here must be the same on every machine. Everything is done by byte-range
// comparison against 'A'..'Z' and 'a'..'z'.
#include "english_case.h"

namespace epocanva {
namespace en {
namespace {

constexpr char kA = 'A';
constexpr char kZ = 'Z';
constexpr char ka = 'a';
constexpr char kz = 'z';
// The two case-transparent delimiters, duplicated here as literals rather than
// included from english_token.h so the case layer stays a leaf: english_token.h
// depends on this file's kMaxCaseInputBytes, not the other way round.
constexpr char kApostrophe = '\'';
constexpr char kHyphenMinus = '-';

bool IsUpper(char c) { return c >= kA && c <= kZ; }
bool IsLower(char c) { return c >= ka && c <= kz; }

char ToLowerAscii(char c) { return IsUpper(c) ? static_cast<char>(c - kA + ka) : c; }
char ToUpperAscii(char c) { return IsLower(c) ? static_cast<char>(c - ka + kA) : c; }

}  // namespace

const char* CaseStyleName(CaseStyle style) {
  switch (style) {
    case CaseStyle::kEmpty:
      return "empty";
    case CaseStyle::kLowercase:
      return "lowercase";
    case CaseStyle::kInitialCapital:
      return "initial-capital";
    case CaseStyle::kUppercase:
      return "uppercase";
    case CaseStyle::kMixed:
      return "mixed-case";
    case CaseStyle::kNotAlphabetic:
      return "not-alphabetic";
    case CaseStyle::kTooLong:
      return "input-too-long";
  }
  return "unknown-style";
}

bool IsAsciiLetter(char c) { return IsUpper(c) || IsLower(c); }

namespace {
// Case-transparent word delimiters (P2.3). Whether a token MAY contain one, and
// where, is english_token.h's structural decision; this file only refuses to let
// a delimiter interrupt a run of same-cased letters, so "don't" is lowercase and
// "Co-Operate" is mixed rather than two independent words.
bool IsCaseTransparentDelimiter(char c) {
  return c == kApostrophe || c == kHyphenMinus;
}
}  // namespace

CaseStyle ClassifyAsciiCase(std::string_view text) {
  // Rule 0.
  if (text.empty()) {
    return CaseStyle::kEmpty;
  }
  // Rule 1, before any content check: an over-long input is refused rather than
  // truncated, so it must not be classified by its first 64 bytes.
  if (text.size() > kMaxCaseInputBytes) {
    return CaseStyle::kTooLong;
  }
  // Rule 2: a byte that is neither an ASCII letter nor a case-transparent
  // delimiter makes the whole input un-classifiable as a case shape. Checked
  // before the shape rules so "he1" is reported as "not letters" rather than as
  // some casing of "he" -- nothing here strips a byte and then carries on.
  // Rule 2b counts letters so that a delimiter-only input cannot be mistaken for
  // a shape: rules 5 and 7 are both vacuously true of "'".
  std::size_t letter_count = 0;
  char first_letter = 0;
  bool first_letter_upper = false;
  bool any_upper = false;
  bool any_lower = false;
  bool letter_after_first_upper = false;
  for (const char c : text) {
    if (IsCaseTransparentDelimiter(c)) {
      continue;
    }
    if (!IsAsciiLetter(c)) {
      return CaseStyle::kNotAlphabetic;
    }
    if (letter_count == 0) {
      first_letter = c;
      first_letter_upper = IsUpper(c);
    } else if (IsUpper(c)) {
      letter_after_first_upper = true;
    }
    ++letter_count;
    if (IsUpper(c)) {
      any_upper = true;
    } else {
      any_lower = true;
    }
  }
  if (letter_count == 0) {
    return CaseStyle::kNotAlphabetic;
  }
  // Rules 3 and 4: the single-letter cases, ahead of the run-length tests so that
  // 'H' cannot be read as "all uppercase" and lose its initial-capital meaning.
  if (letter_count == 1 && text.size() == 1) {
    return first_letter_upper ? CaseStyle::kInitialCapital : CaseStyle::kLowercase;
  }
  // Rule 5.
  if (!any_lower) {
    return CaseStyle::kUppercase;
  }
  // Rule 6: the first LETTER is capital and every later letter is lowercase;
  // delimiters carry no case and do not reset the run, so "Don't" passes here and
  // "Co-Operate" does not.
  if (first_letter_upper && !letter_after_first_upper) {
    return CaseStyle::kInitialCapital;
  }
  // Rule 7.
  if (!any_upper) {
    return CaseStyle::kLowercase;
  }
  // Rule 8: letters, but no recognised shape ("HeL", "hEL", "iPh", "don'T",
  // "Co-Operate"). Not guessed, not repaired.
  return CaseStyle::kMixed;
}

bool IsCompletableCase(CaseStyle style) {
  return style == CaseStyle::kLowercase || style == CaseStyle::kInitialCapital ||
         style == CaseStyle::kUppercase;
}

bool ToLookupKey(std::string_view text, std::string* out) {
  if (out == nullptr || text.empty() || text.size() > kMaxCaseInputBytes) {
    return false;
  }
  std::string key;
  key.reserve(text.size());
  for (const char c : text) {
    if (IsAsciiLetter(c)) {
      key.push_back(ToLowerAscii(c));
      continue;
    }
    // Delimiters are copied verbatim: the index holds "don't", so the key must
    // keep the apostrophe in the same code-unit position.
    if (c == kApostrophe || c == kHyphenMinus) {
      key.push_back(c);
      continue;
    }
    return false;
  }
  *out = std::move(key);
  return true;
}

std::string ApplyCaseStyle(std::string_view canonical, CaseStyle style) {
  // An unrecognised shape must not invent a casing. Callers decide policy before
  // getting here; this guard is what stops a wiring bug from silently shipping
  // text in a case the user never typed.
  if (!IsCompletableCase(style)) {
    return std::string(canonical);
  }
  std::string display;
  display.reserve(canonical.size());
  for (std::size_t i = 0; i < canonical.size(); ++i) {
    const char c = canonical[i];
    if (style == CaseStyle::kUppercase) {
      display.push_back(ToUpperAscii(c));
      continue;
    }
    if (style == CaseStyle::kInitialCapital) {
      // Only the WHOLE word's first byte is uppercased. A delimiter does not
      // restart the word, so "co-operate" renders "Co-operate" and never
      // "Co-Operate" (P2.3 brief section 3).
      display.push_back(i == 0 ? ToUpperAscii(c) : c);
      continue;
    }
    display.push_back(c);  // kLowercase: the canonical spelling as stored.
  }
  return display;
}

}  // namespace en
}  // namespace epocanva
