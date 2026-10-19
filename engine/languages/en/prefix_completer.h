// EpoCanva P2.3 -- prefix completion over the English lexicon: token-gated.
//
// Layer rules are the same as english_lexicon.h: no librime header, no platform
// header, no Session, no input history, no network, no writes, no logging.
//
// UNITS (AGENTS.md section 5): a prefix is measured in UTF-8 code units. For the
// alphabet this layer accepts -- 52 ASCII letters, 2 delimiters -- that equals
// bytes. `k`, every result index and every offset below is a count or a position
// in ENTRIES, or a code-unit index, never a bit offset into a string.
//
// THE FOUR TEXTS (english_case.h): raw input -> lookup key (lowercase copy,
// delimiters preserved) -> canonical dictionary word -> display/commit text. The
// raw input is never modified; the index is never copied per case, because the key
// is derived into a local string and the lexicon stores lowercase words.
//
// THREE ANSWERS, KEPT SEPARATE (P2.3 brief section 3): the shape of the input
// (english_token.h), its casing (english_case.h) and what the index contained
// (this file). Collapsing them into "found nothing" is exactly how a protection
// rule and a missing word list come to look identical.
//
// DELIBERATELY SIMPLE (P2.1 brief section 5, unchanged): the index is the sorted
// vector the lexicon already owns; a prefix lookup is two binary searches plus a
// top-k selection over the matched slice. No Trie, no FST, no database framework.
#ifndef EPOCANVA_EN_PREFIX_COMPLETER_H_
#define EPOCANVA_EN_PREFIX_COMPLETER_H_

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

// Longest prefix accepted by a query, in UTF-8 code units (bytes). One limit,
// shared with the token layer, so "too long to query" and "too long to be a word"
// can never disagree.
constexpr std::size_t kMaxPrefixBytes = kMaxTokenBytes;

// Upper bound on how many completions one query may return, regardless of the `k`
// a caller asks for. It exists so a mistaken caller cannot make this layer copy a
// whole word list; it is not a product-level paging policy.
constexpr std::size_t kMaxCompletions = 64;

// The single reason a query produced the candidates it did -- or none. Derived
// from the two gates (token shape, then casing) plus the index result, so a caller
// counts one thing per query instead of re-deriving this switch.
enum class CompletionOutcome {
  kCompleted = 0,   // at least one completion beyond the original text
  kNoMatch,         // a lookup really ran and the index has no such prefix
  kEmptyInput,      // nothing typed
  kInputTooLong,    // over kMaxPrefixBytes code units; the input was not cut down
  kNonAscii,        // a byte >= 0x80 -- refused, not decoded or normalised
  kProtectedText,   // a digit or technical mark: stop completing this token
  kUnsupportedByte, // printable ASCII with no rule here (space, ;, ", ...)
  kIllegalWordForm, // apostrophe/hyphen used outside the limited pattern
  kMixedCase,       // recognised letters, unrecognised casing -- keep the original
};

const char* CompletionOutcomeName(CompletionOutcome outcome);

struct Completion {
  // The dictionary's own spelling, as stored (lowercase, delimiters included).
  std::string canonical;
  // The canonical word rendered in the raw input's case shape. This is the text a
  // selected candidate COMMITS.
  std::string display;
  std::uint32_t weight = 0;
};

struct CompletionResult {
  // Structure of the raw input, carried through so a test can read the stop class
  // and the delimiter offset without re-analysing. The raw text itself is not
  // stored here.
  TokenAnalysis token{};
  CaseStyle case_style = CaseStyle::kEmpty;
  CompletionOutcome outcome = CompletionOutcome::kEmptyInput;
  // False unless a binary-search range was actually taken: a refused input and a
  // lookup that matched nothing are different facts and must not read the same.
  bool lookup_performed = false;
  // Entries in the matched range that survived the drop of the row whose display
  // text would repeat the raw input, BEFORE the k-limit was applied.
  std::size_t match_count = 0;
  // How many rows in the range were dropped as repeats of the raw input.
  // range size == match_count + deduplicated, which the tests assert.
  std::size_t deduplicated = 0;
  // Total order: weight descending, then canonical word ascending by byte value.
  // Case never re-ranks: ranking is computed on canonical text only.
  std::vector<Completion> completions;
};

// Orders two entries the way every completion list in this project is ordered.
// Locale-free by construction: byte comparison on canonical text, never strcoll.
bool CompletionBefore(const LexiconEntry& a, const LexiconEntry& b);

// Half-open index range [start, end) of the entries whose word starts with `key`,
// in the lexicon's ascending-byte layout. Split out of CompletePrefix and made
// public because the range BOUND is the one thing P2.3 could have broken silently:
// P2.1 bounded the range at `key + '{'`, which is only correct while every byte
// that can FOLLOW the prefix is below '{'. Entries now contain U+0027 and U+002D,
// which do happen to sit below it -- but that is luck about a particular word
// list, not a property of the algorithm. These two functions use the prefix
// successor instead, which is correct for any entry alphabet whose bytes are
// below 0xFF, and the tests exercise both edges of every delimiter boundary.
// `key` must be non-empty; every caller gets here through ToLookupKey, which
// refuses an empty input.
std::size_t PrefixRangeStart(const std::vector<LexiconEntry>& entries,
                             const std::string& key);
std::size_t PrefixRangeEnd(const std::vector<LexiconEntry>& entries,
                           const std::string& key);

// Completes `raw_input`, which is NEVER modified by this call: it is received as a
// view into the caller's buffer and only read.
//
// A token that passes the structural gate AND has a completable casing is looked
// up through the lowercase key and rendered in that casing. Anything else returns
// no completions with its own `outcome` value, and nothing is cleaned, trimmed,
// truncated, re-cased or had a stop byte removed first.
//
// k == 0 yields an empty list but still runs both gates, so `outcome` and `token`
// are trustworthy either way.
CompletionResult CompletePrefix(const EnglishLexicon& lexicon,
                                std::string_view raw_input,
                                std::size_t k);

}  // namespace en
}  // namespace epocanva

#endif  // EPOCANVA_EN_PREFIX_COMPLETER_H_
