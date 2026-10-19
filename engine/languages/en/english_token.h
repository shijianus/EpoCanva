// EpoCanva P2.3 -- the shape of one English token, and whether it may be queried.
//
// LAYER: language core. No librime header, no platform header, no Session, no
// input history, no network, no writes, no logging of any kind.
//
// WHAT THIS FILE OWNS: STRUCTURE -- which ASCII byte classes appear, and where.
// It does NOT own case (english_case.h) and does NOT own the index
// (english_lexicon.h). They are three files rather than one function returning a
// bool because the product behaviour differs per reason even though the visible
// result is often the same "only the text I typed":
//
//   "don'"        letters then one delimiter -> QUERY it (the accepted partial)
//   "user@x"      contains '@' -> STOP. Technical text must not have its tail
//                 completed: "user" plus a completion corrupts a credential.
//   "he2"         contains a digit -> STOP, for the same reason, different class.
//   "rock'n'roll" two delimiters -> STOP. Outside the limited word form.
//   "'hello"      leading delimiter -> STOP.
//   "don't-x"     apostrophe AND hyphen -> STOP.
//
// Every stop reason is counted separately so "completion silently not working"
// stays diagnosable instead of being inferred from an empty candidate list.
//
// LIMITED BY DESIGN (P2.3 brief section 2): at most ONE ASCII delimiter per
// token, apostrophe XOR hyphen-minus, never leading, never trailing in a complete
// entry. No Unicode curly quotes, no dash normalisation, no automatic punctuation
// replacement, no URL/e-mail/code parser. A stop byte is REPORTED as a class,
// never stripped, translated or "cleaned up" before querying.
//
// PRIVACY (AGENTS.md section 2): nothing here echoes the user's bytes. The
// analysis reports a byte CLASS and a code-unit OFFSET, never the character or
// the surrounding text; the raw input is not copied or stored.
//
// UNITS (AGENTS.md section 5): all offsets and lengths are UTF-8 code units,
// which for the ASCII bytes this layer inspects equals bytes. `stop_offset` is a
// code-unit index into the raw input, not a grapheme position.
#ifndef EPOCANVA_EN_TOKEN_H_
#define EPOCANVA_EN_TOKEN_H_

#include <cstddef>
#include <string_view>

#include "english_case.h"

namespace epocanva {
namespace en {

// One length limit for one reason: a longer input is not queried and is never
// truncated. Aliased to the case layer's constant so the two cannot drift.
constexpr std::size_t kMaxTokenBytes = kMaxCaseInputBytes;

// The only two delimiter bytes this project recognises, by ASCII code point.
constexpr char kApostrophe = '\'';  // U+0027 APOSTROPHE
constexpr char kHyphenMinus = '-';  // U+002D HYPHEN-MINUS

// The byte classes this layer distinguishes. `kOtherAscii` is everything
// printable ASCII that is neither a letter, a digit, one of the two delimiters,
// nor one of the protected marks below -- it stops the query as
// `kUnsupportedByte` rather than being silently tolerated.
enum class ByteClass {
  kLetter = 0,  // A-Z or a-z; case is english_case.h's business
  kDigit,       // 0-9
  kDelimiter,   // ' or -
  kProtectedMark,  // one of the technical-text marks below
  kOtherAscii,     // space, ; " ! ( ) [ ] { } < > | ~ ^ * ` , and controls
  kNonAscii,       // any byte >= 0x80 -- not decoded, not guessed
};

ByteClass ClassifyTokenByte(char c);
const char* ByteClassName(ByteClass klass);

// Digits and the marks listed in the P2.3 brief section 4:
//   0-9 @ . _ : / \ ? & = # % +
// True for all of them. This is a conservative completion policy, not syntax
// recognition: a token containing one of these is assumed to be technical text
// and is left exactly as typed.
bool IsProtectedAsciiByte(char c);

// True for the two delimiter bytes only.
bool IsTokenDelimiter(char c);

// Where the delimiter bytes of a token sit. Structural, case-independent. Once
// non-letter, non-delimiter bytes have been excluded (which
// AnalyzeEnglishToken does first), "internal" means letters on both sides.
enum class DelimiterForm {
  kNone = 0,      // no delimiter byte
  kInternal,      // exactly one, not at either end
  kTrailing,      // exactly one, as the last byte
  kLeading,       // exactly one, as the first byte
  kMoreThanOne,   // two or more delimiter bytes
  kBothKinds,     // an apostrophe AND a hyphen-minus appear
};

// Why a raw input is or is not sent to the index.
enum class QueryDecision {
  kQuery = 0,       // a queryable prefix of a limited English token
  kEmpty,           // nothing typed
  kTooLong,         // more than kMaxTokenBytes code units; never truncated
  kNotAscii,        // a byte >= 0x80 is present
  kProtected,       // a digit or protected mark is present: technical text
  kUnsupportedByte, // printable ASCII this layer has no rule for
  kIllegalForm,     // delimiters present but not in the limited pattern
};

// Which class of byte stopped the query. A class, never a character.
enum class StopClass {
  kNone = 0,
  kNoBytesTyped,
  kLength,
  kNonAsciiByte,
  kDigit,
  kPunctuationMark,
  kOtherAsciiByte,
  kMisplacedDelimiter,
  kExtraDelimiter,
  kMixedDelimiterKinds,
};

// The whole answer to "what is this input". The caller keeps ownership of the
// text; this struct holds no copy of it.
struct TokenAnalysis {
  QueryDecision decision = QueryDecision::kEmpty;
  DelimiterForm delimiter = DelimiterForm::kNone;
  StopClass stop_class = StopClass::kNone;
  // Code-unit offset of the byte that decided the outcome: the first protected
  // mark, the first non-ASCII byte, or the offending delimiter. npos when the
  // decision is kEmpty or kTooLong.
  std::size_t stop_offset = std::string_view::npos;
  // Code-unit offset of the token's single delimiter, npos when there is none.
  // For an accepted query this says WHERE the apostrophe or hyphen sits, which
  // is what tells "don'" (trailing, still queryable) from "don't" (internal,
  // a complete entry). It is also where a rejected delimiter was found.
  std::size_t delimiter_offset = std::string_view::npos;
  // Length of the raw input in UTF-8 code units, always filled in -- an
  // over-long input is reported at its real length, never as a truncated one.
  std::size_t length_bytes = 0;
  // The input already matches the complete-token pattern: letters, optionally
  // one internal delimiter, letters. "don'" is NOT a complete entry.
  bool is_full_entry_form = false;
  // A lookup may be performed. Equals (decision == QueryDecision::kQuery) and is
  // spelled out because the brief asks that "is this a complete entry" and "is
  // this a queryable prefix" be separate answers.
  bool is_query_prefix_form = false;
};

// Classifies `raw_input` structurally. Pure and total: every input gets exactly
// one decision, and the order below is part of the contract because one input can
// satisfy several refusals at once ("he2@" is protected in two ways).
//   1. empty                                -> kEmpty
//   2. length > kMaxTokenBytes              -> kTooLong  (checked before any
//      content rule, so an over-long input is never classified by a prefix of it)
//   3. a byte >= 0x80                       -> kNotAscii
//   4. a digit or protected mark            -> kProtected
//   5. other unrecognised ASCII             -> kUnsupportedByte
//   6. delimiter form not one of
//      {none, internal, trailing}           -> kIllegalForm
//   7. otherwise                            -> kQuery
TokenAnalysis AnalyzeEnglishToken(std::string_view raw_input);

// The lexicon's complete-entry grammar, shared with the query layer on purpose:
// a word that can be LOADED must also be findable, and vice versa.
//   [a-z]+  |  [a-z]+ '[' [a-z]+  |  [a-z]+ '-' [a-z]+
// Lowercase only -- the word list stores canonical lowercase spellings -- and at
// most kMaxTokenBytes bytes INCLUDING the delimiter.
bool IsDictionaryEntryForm(std::string_view word);

// Case-insensitive form of the same shape: "Don't" has the form even though the
// lexicon stores it lowercase. A trailing delimiter is not a complete entry.
bool IsCompleteTokenShape(std::string_view text);

// Letters then exactly one delimiter and nothing after it: "don'", "co-".
// The only partial state the query layer accepts.
bool IsTrailingDelimiterPrefix(std::string_view text);

const char* DelimiterFormName(DelimiterForm form);
const char* QueryDecisionName(QueryDecision decision);
const char* StopClassName(StopClass stop_class);

}  // namespace en
}  // namespace epocanva

#endif  // EPOCANVA_EN_TOKEN_H_
