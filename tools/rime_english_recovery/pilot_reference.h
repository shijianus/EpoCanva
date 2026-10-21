// EpoCanva P2.4-recovery -- an INDEPENDENT reference answer for prefix completion, used to
// check the engine against something that was not computed by the engine.
//
// This file reads the pilot TSV itself, keeps the rows in the order they are read, and
// answers a prefix by scanning EVERY row: collect, then order by weight descending with
// ties broken by the canonical word's ascending byte order, then render in the input's own
// case shape, then drop a rendered form that equals the typed text, then cut the tail to the
// budget. It is slow on purpose -- a full scan of 42,356 rows is the shape of an algorithm
// that cannot share a bug with a binary-search index -- and it exists only in the test
// binary.
//
// WHAT IT DELIBERATELY DOES NOT DO: it does not include prefix_completer.h, english_case.h,
// english_token.h or english_lexicon.h, and it calls none of them. If it reused the query
// core, agreeing with the engine would prove nothing. The rules it implements are the ones
// written down in the fixture documentation and the contract comments:
//   word grammar  [a-z]+ | [a-z]+ '[' [a-z]+ | [a-z]+ '-' [a-z]+, at most 64 UTF-8 code
//                 units (64: english_case.h's kMaxCaseInputBytes, restated here as a
//                 literal with its citation so this file has no dependency on the product)
//   weight        one or more ASCII digits, no leading zero, value <= 4294967295
//   order         weight descending, then canonical word ascending by unsigned bytes
//   budget        candidate 0 is the typed text, at most 7 completions follow
//   dedup         on the DISPLAY text, because "Hello" maps to the row hello and must not
//                 appear twice
//   case shapes   single 'H' is initial-capital (not uppercase); a word with no letter at
//                 all is not alphabetic; the two delimiters carry no case and do not break
//                 a run of letters, so "Co-Operate" is mixed, not initial-capital
//   refusals      mixed case, a non-alphabetic byte and an over-long input produce no
//                 completion at all; the typed text is still what commits
//
// UNITS (AGENTS.md section 5): every length here is in UTF-8 code units, which for the
// ASCII bytes in this data set equals bytes. Prefix comparison is a byte comparison.
#ifndef EPOCANVA_RECOVERY_PILOT_REFERENCE_H_
#define EPOCANVA_RECOVERY_PILOT_REFERENCE_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace epocanva {
namespace recovery {

// Restated from the product's documented contract, with the citation rather than the
// include: engine/languages/en/english_case.h defines kMaxCaseInputBytes = 64.
constexpr std::size_t kRefMaxWordBytes = 64;
constexpr std::uint64_t kRefMaxWeight = 4294967295ull;

enum class RefShape {
  kEmpty,
  kLowercase,
  kInitialCapital,
  kUppercase,
  kMixed,           // letters, but no supported shape: never completed
  kNotAlphabetic,   // some byte is neither a letter nor one of the two delimiters
  kTooLong,         // more than kRefMaxWordBytes code units: never truncated
};

const char* RefShapeName(RefShape shape);

// Classification in the priority order the contract states. Checked in that order because
// the order is what makes a single 'H' initial-capital rather than uppercase, and what
// keeps a bare delimiter out of the uppercase answer.
RefShape RefClassify(const std::string& text);

// Canonical (lowercase) form used to address the rows. Only completable shapes have a
// meaning here; anything else returns false.
bool RefLookupKey(const std::string& text, std::string* out);

// The canonical row rendered in the shape `raw` had. Returns canonical unchanged for a
// shape that is not completable, so a caller bug cannot invent a third casing.
std::string RefRender(const std::string& canonical, RefShape shape);

// One row of a word list.
struct RefRow {
  std::string word;
  std::uint32_t weight = 0;
};

// A word list read by this file alone. Load() reports the first row that breaks the
// grammar, with its 1-based line, and never repairs it: a reference that "fixed" the data
// would stop being independent of what the loader under test decided to accept.
class PilotReference {
 public:
  // Returns false and fills `error` with a named reason when any row is outside the
  // grammar, when two rows carry the same word, or when the file is not strictly
  // ascending (the order the converter documents).
  bool Load(const std::filesystem::path& tsv, std::string* error);

  std::size_t row_count() const { return rows_.size(); }
  const std::vector<RefRow>& rows() const { return rows_; }
  bool ContainsExact(const std::string& canonical_word) const;

  // The answer for a raw composition: candidate 0 is `raw_input`, then up to
  // (total_cap - 1) completions. Empty for an empty input.
  std::vector<std::string> Candidates(const std::string& raw_input,
                                      std::size_t total_cap) const;
  // The same list joined with '|', which is how the tests write menus.
  std::string Menu(const std::string& raw_input, std::size_t total_cap) const;

 private:
  std::vector<RefRow> rows_;
};

std::string RefJoinMenu(const std::vector<std::string>& items);

}  // namespace recovery
}  // namespace epocanva

#endif  // EPOCANVA_RECOVERY_PILOT_REFERENCE_H_
