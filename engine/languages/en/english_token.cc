// EpoCanva P2.3 -- token structure implementation.
//
// No <cctype>: isdigit/isalpha are locale-dependent, and the set of bytes that
// stops a completion must be identical on every machine. Everything below
// compares byte values against explicit ASCII ranges.
//
// The raw input is only ever READ. There is no copy, no lowering, no trimming
// and no removal of a stop byte: an input containing '@' is refused AS a whole,
// which is what keeps "user@example.com" from becoming "user" + some word from
// the list (P2.3 brief section 6).
#include "english_token.h"

namespace epocanva {
namespace en {
namespace {

constexpr char kA = 'A';
constexpr char kZ = 'Z';
constexpr char ka = 'a';
constexpr char kz = 'z';
constexpr char k0 = '0';
constexpr char k9 = '9';

// The protected marks of P2.3 brief section 4, in ASCII order. Digits are
// handled separately so a test can tell "a digit stopped this" from "a symbol
// stopped this" while both still mean stop.
constexpr char kProtectedMarks[] = "@._:/\\?&=#%+";

bool IsLowercaseByte(char c) { return c >= ka && c <= kz; }

}  // namespace

ByteClass ClassifyTokenByte(char c) {
  const unsigned char raw = static_cast<unsigned char>(c);
  if (raw >= 0x80) {
    // Never decoded. A byte this high could be any script's letter, and this
    // layer has no rule for it, so it is reported rather than guessed at.
    return ByteClass::kNonAscii;
  }
  if ((c >= kA && c <= kZ) || (c >= ka && c <= kz)) {
    return ByteClass::kLetter;
  }
  if (c >= k0 && c <= k9) {
    return ByteClass::kDigit;
  }
  if (c == kApostrophe || c == kHyphenMinus) {
    return ByteClass::kDelimiter;
  }
  for (const char mark : kProtectedMarks) {
    if (c == mark) {
      return ByteClass::kProtectedMark;
    }
  }
  return ByteClass::kOtherAscii;
}

const char* ByteClassName(ByteClass klass) {
  switch (klass) {
    case ByteClass::kLetter:
      return "letter";
    case ByteClass::kDigit:
      return "digit";
    case ByteClass::kDelimiter:
      return "delimiter";
    case ByteClass::kProtectedMark:
      return "protected-mark";
    case ByteClass::kOtherAscii:
      return "other-ascii";
    case ByteClass::kNonAscii:
      return "non-ascii";
  }
  return "unknown-byte-class";
}

bool IsProtectedAsciiByte(char c) {
  const ByteClass klass = ClassifyTokenByte(c);
  return klass == ByteClass::kDigit || klass == ByteClass::kProtectedMark;
}

bool IsTokenDelimiter(char c) { return ClassifyTokenByte(c) == ByteClass::kDelimiter; }

namespace {

// One left-to-right pass over the delimiter bytes, because "which delimiter made
// this illegal" has to be the FIRST one in input order, not the last.
struct DelimiterScan {
  int count = 0;
  std::size_t first_offset = std::string_view::npos;
  char first_kind = 0;
  // Offset of the delimiter that broke the one-delimiter rule: a second
  // delimiter of the same kind, or the first of the other kind.
  std::size_t offending_offset = std::string_view::npos;
  bool both_kinds = false;
};

DelimiterScan ScanDelimiters(std::string_view text) {
  DelimiterScan scan;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (!IsTokenDelimiter(c)) {
      continue;
    }
    ++scan.count;
    if (scan.count == 1) {
      scan.first_offset = i;
      scan.first_kind = c;
      continue;
    }
    if (c != scan.first_kind) {
      if (scan.offending_offset == std::string_view::npos) {
        scan.both_kinds = true;
        scan.offending_offset = i;
      }
    } else if (scan.offending_offset == std::string_view::npos) {
      scan.offending_offset = i;
    }
  }
  return scan;
}

DelimiterForm FormOf(const DelimiterScan& scan, std::size_t length) {
  if (scan.both_kinds) {
    return DelimiterForm::kBothKinds;
  }
  if (scan.count >= 2) {
    return DelimiterForm::kMoreThanOne;
  }
  if (scan.count == 0) {
    return DelimiterForm::kNone;
  }
  if (scan.first_offset == 0) {
    return DelimiterForm::kLeading;
  }
  if (scan.first_offset + 1 == length) {
    return DelimiterForm::kTrailing;
  }
  return DelimiterForm::kInternal;
}

// Shared skeleton for the three form predicates: every byte must be a letter or
// the single allowed delimiter, and the delimiters must sit in an accepted place.
// `lowercase_only` is what separates a lexicon entry ([a-z] only) from a typed
// token ("Don't" has the shape; the list still stores it lowercase).
bool HasTokenShape(std::string_view text, bool lowercase_only,
                   bool allow_trailing) {
  if (text.empty() || text.size() > kMaxTokenBytes) {
    return false;
  }
  for (const char c : text) {
    const ByteClass klass = ClassifyTokenByte(c);
    if (klass == ByteClass::kDelimiter) {
      continue;
    }
    if (klass != ByteClass::kLetter) {
      return false;
    }
    if (lowercase_only && !IsLowercaseByte(c)) {
      return false;
    }
  }
  const DelimiterScan scan = ScanDelimiters(text);
  switch (FormOf(scan, text.size())) {
    case DelimiterForm::kNone:
    case DelimiterForm::kInternal:
      return true;
    case DelimiterForm::kTrailing:
      return allow_trailing;
    case DelimiterForm::kLeading:
    case DelimiterForm::kMoreThanOne:
    case DelimiterForm::kBothKinds:
      return false;
  }
  return false;
}

}  // namespace

TokenAnalysis AnalyzeEnglishToken(std::string_view raw_input) {
  TokenAnalysis analysis;
  analysis.length_bytes = raw_input.size();
  if (raw_input.empty()) {
    analysis.decision = QueryDecision::kEmpty;
    analysis.stop_class = StopClass::kNoBytesTyped;
    return analysis;
  }
  // Length first, content second: classifying only the first 64 bytes of a
  // longer input would silently describe a prefix the user did not type.
  if (raw_input.size() > kMaxTokenBytes) {
    analysis.decision = QueryDecision::kTooLong;
    analysis.stop_class = StopClass::kLength;
    return analysis;
  }

  QueryDecision byte_decision = QueryDecision::kQuery;
  StopClass byte_stop_class = StopClass::kNone;
  std::size_t byte_stop_offset = std::string_view::npos;
  for (std::size_t i = 0; i < raw_input.size(); ++i) {
    const ByteClass klass = ClassifyTokenByte(raw_input[i]);
    if (klass == ByteClass::kLetter || klass == ByteClass::kDelimiter) {
      continue;
    }
    // Rules 3 to 5, first offender in input order wins: the earlier byte is the
    // one the user can point at.
    if (byte_decision != QueryDecision::kQuery) {
      continue;
    }
    byte_decision = (klass == ByteClass::kNonAscii)
                        ? QueryDecision::kNotAscii
                        : (klass == ByteClass::kDigit ||
                                   klass == ByteClass::kProtectedMark)
                              ? QueryDecision::kProtected
                              : QueryDecision::kUnsupportedByte;
    byte_stop_class = (klass == ByteClass::kNonAscii)
                          ? StopClass::kNonAsciiByte
                          : (klass == ByteClass::kDigit)
                                ? StopClass::kDigit
                                : (klass == ByteClass::kProtectedMark)
                                      ? StopClass::kPunctuationMark
                                      : StopClass::kOtherAsciiByte;
    byte_stop_offset = i;
  }

  const DelimiterScan scan = ScanDelimiters(raw_input);
  analysis.delimiter = FormOf(scan, raw_input.size());
  // The position is reported whatever the decision turns out to be: a rejected
  // "co--operate" and an accepted "co-" both have a delimiter the test can point
  // at, and the two answers differ only in what happens next.
  if (scan.count > 0) {
    analysis.delimiter_offset = scan.first_offset;
  }
  if (byte_decision != QueryDecision::kQuery) {
    analysis.decision = byte_decision;
    analysis.stop_class = byte_stop_class;
    analysis.stop_offset = byte_stop_offset;
    return analysis;
  }
  if (analysis.delimiter == DelimiterForm::kLeading ||
      analysis.delimiter == DelimiterForm::kMoreThanOne ||
      analysis.delimiter == DelimiterForm::kBothKinds) {
    analysis.decision = QueryDecision::kIllegalForm;
    analysis.stop_class = (analysis.delimiter == DelimiterForm::kLeading)
                              ? StopClass::kMisplacedDelimiter
                              : (analysis.delimiter == DelimiterForm::kMoreThanOne)
                                    ? StopClass::kExtraDelimiter
                                    : StopClass::kMixedDelimiterKinds;
    analysis.stop_offset = (scan.offending_offset != std::string_view::npos)
                               ? scan.offending_offset
                               : scan.first_offset;
    return analysis;
  }

  analysis.decision = QueryDecision::kQuery;
  analysis.is_query_prefix_form = true;
  analysis.is_full_entry_form = (analysis.delimiter != DelimiterForm::kTrailing);
  return analysis;
}

bool IsDictionaryEntryForm(std::string_view word) {
  return HasTokenShape(word, /*lowercase_only=*/true, /*allow_trailing=*/false);
}

bool IsCompleteTokenShape(std::string_view text) {
  return HasTokenShape(text, /*lowercase_only=*/false, /*allow_trailing=*/false);
}

bool IsTrailingDelimiterPrefix(std::string_view text) {
  if (text.empty() || text.size() > kMaxTokenBytes) {
    return false;
  }
  if (!IsTokenDelimiter(text.back())) {
    return false;
  }
  return HasTokenShape(text, /*lowercase_only=*/false, /*allow_trailing=*/true);
}

const char* DelimiterFormName(DelimiterForm form) {
  switch (form) {
    case DelimiterForm::kNone:
      return "no-delimiter";
    case DelimiterForm::kInternal:
      return "internal-delimiter";
    case DelimiterForm::kTrailing:
      return "trailing-delimiter";
    case DelimiterForm::kLeading:
      return "leading-delimiter";
    case DelimiterForm::kMoreThanOne:
      return "more-than-one-delimiter";
    case DelimiterForm::kBothKinds:
      return "both-delimiter-kinds";
  }
  return "unknown-delimiter-form";
}

const char* QueryDecisionName(QueryDecision decision) {
  switch (decision) {
    case QueryDecision::kQuery:
      return "query";
    case QueryDecision::kEmpty:
      return "empty";
    case QueryDecision::kTooLong:
      return "input-too-long";
    case QueryDecision::kNotAscii:
      return "non-ascii";
    case QueryDecision::kProtected:
      return "protected-text";
    case QueryDecision::kUnsupportedByte:
      return "unsupported-byte";
    case QueryDecision::kIllegalForm:
      return "illegal-word-form";
  }
  return "unknown-decision";
}

const char* StopClassName(StopClass stop_class) {
  switch (stop_class) {
    case StopClass::kNone:
      return "none";
    case StopClass::kNoBytesTyped:
      return "no-bytes-typed";
    case StopClass::kLength:
      return "length";
    case StopClass::kNonAsciiByte:
      return "non-ascii-byte";
    case StopClass::kDigit:
      return "digit";
    case StopClass::kPunctuationMark:
      return "punctuation-mark";
    case StopClass::kOtherAsciiByte:
      return "other-ascii-byte";
    case StopClass::kMisplacedDelimiter:
      return "misplaced-delimiter";
    case StopClass::kExtraDelimiter:
      return "extra-delimiter";
    case StopClass::kMixedDelimiterKinds:
      return "mixed-delimiter-kinds";
  }
  return "unknown-stop-class";
}

}  // namespace en
}  // namespace epocanva
