// EpoCanva P2.2 -- ASCII letter-case classification and display mapping.
//
// Layer rules are the same as english_lexicon.h: no librime header, no platform
// header, no Session, no input history, no network, no writes.
//
// SCOPE, stated narrowly on purpose: this is an **ASCII A-Z / a-z** mapping with
// four recognised shapes. It is NOT Unicode case folding, NOT title-casing, NOT
// locale-aware casing and NOT proper-noun or brand-name case recovery. Anything
// outside the 52 ASCII letters is reported, never repaired. std::tolower /
// std::toupper are not used anywhere: both take an int and are locale-dependent,
// and a locale that could change how 'i' maps is exactly what this file must not
// inherit.
//
// P2.3 ADDITION, and its limit: the two ASCII word delimiters (' and -) are
// CASE-TRANSPARENT. They are neither letters nor "unsupported bytes": they carry
// no case, they keep their position, and they do not break a run of lowercase
// letters. That is what makes Don't and CO-OPERATE completable while Co-Operate
// is not -- see the rules below. Structure (where a delimiter may sit, how many)
// is english_token.h's decision, not this file's: a shape that is structurally
// illegal must never reach a lookup just because its case looked ordinary.
//
// THE FOUR TEXTS this phase distinguishes (do not collapse them):
//   raw input   - exactly what the user's keys produced; never modified, never
//                 lowercased in place, and always what candidate 0 commits.
//   lookup key  - raw input forced to lowercase; ONLY used to address the index.
//   canonical   - the dictionary's own spelling, which lexicon.tsv keeps lowercase.
//   display     - canonical mapped through the raw input's case shape; this is the
//                 text a selected candidate commits.
//
// UNITS (AGENTS.md section 5): all lengths in UTF-8 code units, which for the ASCII
// letters handled here equals bytes.
#ifndef EPOCANVA_EN_ENGLISH_CASE_H_
#define EPOCANVA_EN_ENGLISH_CASE_H_

#include <cstddef>
#include <string>
#include <string_view>

namespace epocanva {
namespace en {

// Longest input any case-aware entry point accepts, in UTF-8 code units (bytes).
// Same limit as the prefix and the word list, deliberately one constant.
constexpr std::size_t kMaxCaseInputBytes = 64;

// The classification result. The first five values describe a shape;
// kNotAlphabetic and kTooLong are input-validation outcomes that carry no shape,
// and they are kept distinct from kMixed because the product behaviour differs:
// kMixed means "recognised letters, unrecognised pattern, do not guess", while
// kNotAlphabetic means "not even a letter-only word".
enum class CaseStyle {
  kEmpty = 0,
  kLowercase,        // "hel"
  kInitialCapital,   // "H", "Hel"
  kUppercase,        // "HEL" (two letters or more)
  kMixed,            // "HeL", "hEL", "iPh" -- letters, but no recognised shape
  kNotAlphabetic,    // any byte outside A-Z / a-z, including non-ASCII
  kTooLong,          // more than kMaxCaseInputBytes bytes
};

const char* CaseStyleName(CaseStyle style);

// True for the 52 ASCII letters only.
bool IsAsciiLetter(char c);

// Classification, in this exact priority order. The order is what removes the
// single-letter ambiguity: 'H' alone is initial_capital, not "all uppercase",
// because rule 1 is tested before rule 2 and both are tested before the
// all-lowercase rule that would otherwise ignore it.
//   0. empty                                  -> kEmpty
//   1. more than kMaxCaseInputBytes bytes      -> kTooLong   (never truncated)
//   2. any byte that is neither A-Z / a-z nor one of the two case-transparent
//      delimiters                             -> kNotAlphabetic
//   2b. no letter byte at all (so "--", "'", "@") -> kNotAlphabetic. Guarded
//      because rules 5 and 7 are both vacuously true of a letter-free string and
//      would otherwise report a bare delimiter as uppercase.
//   3. single 'A'-'Z'                          -> kInitialCapital
//   4. single 'a'-'z'                          -> kLowercase
//   5. letters only after the guard, all upper -> kUppercase        ("HEL", "DON'T")
//   6. first letter 'A'-'Z', every LATER letter 'a'-'z'
//                                             -> kInitialCapital  ("Hel", "Don't", "Co-operate")
//   7. every letter 'a'-'z'                   -> kLowercase        ("hel", "don't")
//   8. any other letter combination           -> kMixed  ("HeL", "Co-Operate", "don'T")
// Rule 6 looks at LETTERS, not at bytes: the delimiter after 'Don' does not reset
// anything, so the letter following an apostrophe is still "a later letter" and
// must be lowercase. That is the whole reason "Co-Operate" is kMixed rather than
// kInitialCapital -- this layer never capitalises after a delimiter on its own
// (P2.3 brief section 3).
CaseStyle ClassifyAsciiCase(std::string_view text);

// True only for the three shapes that may be completed: kLowercase,
// kInitialCapital, kUppercase.
bool IsCompletableCase(CaseStyle style);

// Lowercase form of `text`, which the caller must have already checked is
// within the length limit and structurally legal. This is the LOOKUP KEY: it is a
// copy, so the raw input is never rewritten. ASCII letters are lowered; the two
// case-transparent delimiters pass through UNCHANGED, because the index stores
// "don't" with its apostrophe. Returns false and leaves `out` untouched for any
// other byte.
bool ToLookupKey(std::string_view text, std::string* out);

// Renders the canonical dictionary word `canonical` in the shape `style` asks for.
// Only the three completable styles are meaningful; for anything else the function
// returns `canonical` unchanged rather than inventing a transformation, so a
// caller bug cannot silently produce a third casing the user never typed.
// ASCII-only by construction.
std::string ApplyCaseStyle(std::string_view canonical, CaseStyle style);

}  // namespace en
}  // namespace epocanva

#endif  // EPOCANVA_EN_ENGLISH_CASE_H_
