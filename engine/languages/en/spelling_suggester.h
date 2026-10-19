// EpoCanva P2.6 -- one-edit spelling suggestions. SUGGEST ONLY: nothing in this
// layer replaces, deletes or re-orders what the user typed, and the caller decides
// whether a suggestion is ever shown.
//
// LAYER: language core, same rules as english_lexicon.h -- no librime header, no
// platform header, no Session, no input history, no learning data, no network, no
// writes, no logging. It is compiled twice (into rime.dll and into the standalone
// core test binary) from these same two files.
//
// WHAT "one edit" means here, and what it deliberately is not:
//   deletion      remove exactly one letter            "hcello" -> "hello"
//   insertion     add exactly one a-z letter           "helo"   -> "hello"
//   substitution  change exactly one letter to another "hxllo"  -> "hello"
//   transposition swap exactly one pair of ADJACENT letters,
//                 and only when the two letters differ ("ehllo"  -> "hello")
// No second pass, no recursion, no composition of two edits, and no claim that this
// is a general Damerau-Levenshtein implementation: a general edit distance with
// costs would also accept e.g. a deletion plus an insertion as distance 2, and this
// layer never even computes that. The window is exactly the four operations above,
// each applied once to the input.
//
// WHY THE SWAP GUARD EXISTS: transposing two equal adjacent letters produces the
// input itself ("helllo" -> "helllo"). That is not a correction, and counting it
// would inflate variants_generated and let a "suggestion" that changes nothing
// reach the menu. The rule is stated twice on purpose -- skip equal pairs, and drop
// any variant that equals the input -- because they are different checks: the first
// is about the operation, the second about the result.
//
// GATES, evaluated in this fixed order so every empty answer has exactly ONE name
// (SpellingOutcome). Collapsing them into "empty list" is what this project's
// earlier phases learned to forbid: "the word is already correct", "the word list
// failed to load", "the feature is off", "a completion was available" and "nothing
// is one edit away" are five different facts and a user-visible behaviour each.
//   1. enabled                    -> kDisabled
//   2. lexicon present            -> kLexiconUnavailable   (NEVER reported as
//      "no suggestions available": a broken word list must stay diagnosable)
//   3. non-empty                  -> kEmptyInput
//   4. length <= kMaxSuggestionWordBytes -> kTooLong   (checked before the short
//      test and before any content rule, so a 200-byte token is classified by its
//      real length, never by a prefix of it)
//   5. length >= kMinSuggestionWordBytes -> kTooShort
//   6. token gate (english_token.h): protected / non-ASCII / unsupported byte /
//      any delimiter -> kProtectedInput, kNonAsciiInput, kUnsupportedByte,
//      kDelimitedForm. This is why "don't" and "C:\tmp\x" never get corrected:
//      the apostrophe is a delimiter, and a technical mark stops the run.
//   7. casing (english_case.h): anything but lowercase / initial_capital /
//      uppercase -> kMixedCase  (never guessed, never normalised)
//   8. canonical key IS a lexicon word -> kWordInLexicon   (the spelling is already
//      in the list: there is nothing to correct, and offering "corrections" of a
//      correct word is the failure mode this gate exists to prevent)
//   9. prefix completion produced at least one candidate -> kCompletionAvailable
//      ("current prefix completion was empty" is judged on the COMPLETION count,
//      which by construction excludes the original-text candidate: the original is
//      always present in the menu, so a gate that counted menu size could never
//      fire. The caller passes that number, not a bool it guessed.)
//  10. generate, look each variant up exactly, rank, cut to k
//      -> kSuggested, or kNoCandidate when no variant is in the list
//
// LOOKUP DISCIPLINE: every existence test goes through EnglishLexicon::Find, a
// binary search in the index the caller already loaded. Nothing here opens a file,
// rebuilds anything, or scans the word list to compute distances. The count is
// reported in lexicon_lookups so "bounded work per keystroke" stays checkable.
//
// UNITS (AGENTS.md section 5): lengths are UTF-8 code units. Within the accepted
// window the input is ASCII letters only, so a code unit is one letter and one
// byte; every position index below is a code-unit offset. Nothing in this file
// interprets UTF-8 sequences or grapheme clusters -- a byte >= 0x80 is gate 6.
#ifndef EPOCANVA_EN_SPELLING_SUGGESTER_H_
#define EPOCANVA_EN_SPELLING_SUGGESTER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "english_case.h"
#include "english_lexicon.h"
#include "english_token.h"

namespace epocanva {
namespace en {

// The window P2.6 admits, in UTF-8 code units (letters, since gate 6 guarantees
// letters only). Shorter than 4 and a "correction" is usually a legitimate word the
// list happens not to have; longer than 32 and the variant count grows while the
// input is far more likely to be an identifier than a typo.
constexpr std::size_t kMinSuggestionWordBytes = 4;
constexpr std::size_t kMaxSuggestionWordBytes = 32;

// Hard ceiling on how many suggestions one call may hand back regardless of `k`, so
// a mistaken caller cannot make this layer materialise a whole word list. Not a
// paging policy: the engine's own menu budget is enforced by the caller.
constexpr std::size_t kMaxSuggestions = 16;

// The 26 letters an insertion or substitution may introduce, as a count used by the
// variant arithmetic in the report. Fixed and locale-free: 'a'..'z' by value.
constexpr int kAsciiLetterCount = 26;

// Which single operation produced a variant. When two different operations can reach
// the same dictionary word, the label is the FIRST one in the generation order
// documented in SuggestSpelling (deletion, insertion, substitution, transposition);
// it says "this word is reachable by this operation", not "this was the user's
// typo", and no consumer may present it as the latter.
enum class SpellingEdit {
  kNone = 0,
  kDeletion,
  kInsertion,
  kSubstitution,
  kTransposition,
};

// The single answer for one call: either suggestions, or the one gate that stopped
// the run, or "the run happened and found nothing". See the gate table at the top of
// this file -- the order is part of the contract, because one input can fail several
// gates at once ("h3llo" is short of nothing but is protected twice over).
enum class SpellingOutcome {
  kSuggested = 0,     // at least one variant was in the word list
  kNoCandidate,       // the run happened; no generated variant exists in the list
  kDisabled,          // the schema does not enable suggestions
  kLexiconUnavailable,  // no usable index: NEVER the same answer as kNoCandidate
  kEmptyInput,        // nothing typed
  kTooShort,          // under kMinSuggestionWordBytes code units
  kTooLong,           // over kMaxSuggestionWordBytes code units; input not cut down
  kProtectedInput,    // a digit or technical mark: technical text is never corrected
  kNonAsciiInput,     // a byte >= 0x80: refused, not decoded, not transliterated
  kUnsupportedByte,   // printable ASCII with no rule here (space, ';', ',' ...)
  kDelimitedForm,     // an apostrophe or hyphen is present: outside this round
  kMixedCase,         // letters, unrecognised casing -- not guessed
  kWordInLexicon,     // the canonical word is already in the list: nothing to fix
  kCompletionAvailable,  // ordinary prefix completion produced candidates
};

struct SpellingSuggestion {
  // The dictionary's own spelling, exactly as stored (lowercase). This is a word
  // that exists in the lexicon; the core test re-derives the whole list
  // independently to prove it.
  std::string canonical;
  // `canonical` rendered in the raw input's case shape, reusing english_case.h so a
  // suggestion looks the way a completion would. This is what a SELECTED suggestion
  // commits; the raw input itself is never rewritten.
  std::string display;
  std::uint32_t weight = 0;
  SpellingEdit edit = SpellingEdit::kNone;
};

struct SpellingResult {
  CaseStyle case_style = CaseStyle::kEmpty;
  SpellingOutcome outcome = SpellingOutcome::kDisabled;
  // True only when the generation pass ran, i.e. outcome is kSuggested or
  // kNoCandidate. Every gate that stopped earlier leaves this false, so a caller can
  // tell "searched and found nothing" from "did not search" without parsing a name.
  bool search_ran = false;
  std::size_t length_bytes = 0;      // the raw input's real length, always filled
  std::size_t variants_generated = 0;  // distinct candidate strings examined
  std::size_t lexicon_lookups = 0;     // Find() calls made -- the bounded-work witness
  std::size_t duplicates_dropped = 0;  // variants already found by an earlier edit
  std::size_t words_in_lexicon = 0;    // variants that really were rows
  // Ordered by weight descending, then canonical word ascending by byte value --
  // the same total order every completion list in this project uses. Case never
  // re-ranks: ranking is on canonical text only.
  std::vector<SpellingSuggestion> suggestions;
};

// Suggests spellings for `raw_input`, which is received as a view, only ever read,
// and never modified or normalised in place.
//
// `lexicon` may be null: that is gate 2 and yields kLexiconUnavailable rather than
// an empty-but-successful answer. `completion_candidates` is the number of ordinary
// prefix completions the caller's own CompletePrefix call produced for the SAME raw
// input, counting only candidates beyond the original text (gate 9). `enabled` is
// the schema switch (gate 1).
//
// k == 0 returns an empty list but still evaluates all gates and still reports the
// outcome it reached, so the reason is trustworthy either way. `k` is clamped to
// kMaxSuggestions.
//
// Generation order, fixed because it decides the SpellingEdit label of a variant
// reachable two ways: deletion (position 0..n-1), insertion (position 0..n, letter
// 'a'..'z'), substitution (position 0..n-1, letter != the one there), transposition
// (adjacent pairs, equal pairs skipped). Variants equal to the input are dropped at
// generation time. Distinct variants are looked up with EnglishLexicon::Find; a
// variant found twice is counted once and recorded in duplicates_dropped.
SpellingResult SuggestSpelling(const EnglishLexicon* lexicon,
                               std::string_view raw_input,
                               std::size_t completion_candidates,
                               std::size_t k,
                               bool enabled);

const char* SpellingEditName(SpellingEdit edit);
const char* SpellingOutcomeName(SpellingOutcome outcome);

}  // namespace en
}  // namespace epocanva

#endif  // EPOCANVA_EN_SPELLING_SUGGESTER_H_
