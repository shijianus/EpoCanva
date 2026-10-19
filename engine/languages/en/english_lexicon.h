// EpoCanva P2.1 -- English lexicon: parsing and the read-only index.
//
// LAYER (docs/adr/0004-compiled-in-rime-components.md / P2.1 brief section 3):
// this is the language core. It includes no librime header and no platform
// header, touches no Session, keeps no input history, performs no network I/O
// and never writes a file. It is compiled twice on purpose: standalone into the
// core unit test binary, and into rime.dll alongside the Translator that uses
// it. Both compilations read the same two source files (see CMakeLists.txt).
//
// UNITS (AGENTS.md section 5): every length and limit here is in UTF-8 code
// units, which for the a-z-only alphabet this layer accepts is the same thing as
// bytes. There is no UTF-16, grapheme or screen-cell arithmetic in this file.
//
// LOAD-ONCE: the index is immutable after construction and holds no file handle.
// Callers are expected to load a given path at most once; the English Translator
// caches by resolved path so a keystroke never re-reads or rebuilds anything.
//
// PRIVACY (AGENTS.md section 2): this layer emits no log of any kind. Lexicon
// words are product data, not user input; nothing here records what a user typed.
#ifndef EPOCANVA_EN_LEXICON_H_
#define EPOCANVA_EN_LEXICON_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "english_token.h"

namespace epocanva {
namespace en {

// Longest word this layer accepts, in UTF-8 code units (bytes). For the accepted
// alphabet (ASCII letters and the two delimiters) code units equal bytes; a
// non-ASCII byte is rejected as kIllegalWord rather than measured in some other
// unit. Aliased so "a word too long to load", "a prefix too long to query" and "a
// token too long to classify" can never become three different numbers.
constexpr std::size_t kMaxWordBytes = kMaxTokenBytes;

// Every rejected condition is a distinct value: a caller must be able to tell
// "the file is not there" from "the file is malformed" from "the file is empty",
// because a failed load must never be mistaken for a successfully loaded empty
// lexicon.
enum class LexiconStatus {
  kOk = 0,
  kFileNotFound,        // the path does not exist or is not a regular file
  kFileUnreadable,      // exists, but opening/reading failed
  kEmptyFile,           // zero bytes -- not the same as "loaded, no entries"
  kBlankLine,           // an interior line with no content (see the contract below)
  kMissingField,        // no tab separator
  kTooManyFields,       // more than one tab
  kIllegalWord,         // empty, or a byte outside the word alphabet (see below)
  kWordTooLong,         // more than kMaxWordBytes bytes
  kDuplicateWord,       // the same word twice
  kIllegalWeight,       // empty, or a byte outside 0-9, or a leading zero
  kWeightOverflow,      // numerically larger than uint32_t can hold
  // P2.3 appended, deliberately at the end: every value above keeps its number so
  // an older reading of a recorded status name still means the same thing. The
  // bytes of such a word are all legal word-alphabet bytes -- the failure is WHERE
  // the apostrophe or hyphen sits: leading, trailing, more than one, or an
  // apostrophe and a hyphen in the same word.
  kIllegalWordForm,       // legal alphabet bytes, illegal word form
};

const char* LexiconStatusName(LexiconStatus status);

// Where a load stopped. `line` is the 1-based line number in the source text; 0
// when the failure is not attributable to one line (missing file, empty file).
// It names no word and quotes no content.
struct LexiconLoadInfo {
  LexiconStatus status = LexiconStatus::kOk;
  std::size_t line = 0;
  std::size_t entry_count = 0;
};

struct LexiconEntry {
  std::string word;
  std::uint32_t weight = 0;
};

// Grammar of one line, fixed by this phase:
//
//   word TAB weight NEWLINE
//
//   word    [a-z]+  or  [a-z]+ '[' [a-z]+  or  [a-z]+ '-' [a-z]+, at most
//           kMaxWordBytes bytes INCLUDING the delimiter. This is exactly
//           english_token.h's IsDictionaryEntryForm, called from there rather than
//           restated here, so a word that can be LOADED is also a word that can be
//           FOUND by a query -- if the two grammars were written twice they could
//           drift, and the symptom would be a word list that loads cleanly and
//           never completes.
//   weight  one or more decimal digits, no leading zero (a bare "0" is legal),
//           value <= 4294967295
//
//   - LF and CRLF are both accepted; a lone CR is not a line terminator.
//   - ONE newline at end of file is allowed and does not create a blank line.
//   - an interior blank line is a hard error (kBlankLine). It is not skipped:
//     silently accepting structural noise is how a truncated word list looks
//     healthy.
//   - no comment syntax, no leading or trailing whitespace on a field, no BOM.
//     Anything else in the file is an error with its line number.
class EnglishLexicon {
 public:
  // Parses the whole file content. Returns nullptr unless the content is valid
  // in full; `info` always receives the outcome. On success the entry vector is
  // sorted by word in ascending byte order, which is the index layout the
  // prefix range search needs.
  static std::unique_ptr<EnglishLexicon> FromText(const std::string& text,
                                                  LexiconLoadInfo* info);

  // Reads a regular file completely and parses it. A read failure is reported
  // as its own status and never collapses into "loaded an empty lexicon".
  // Takes a path object rather than a string so no caller has to guess which
  // encoding std::filesystem would assume for a narrow string on Windows.
  static std::unique_ptr<EnglishLexicon> FromFile(
      const std::filesystem::path& path, LexiconLoadInfo* info);

  EnglishLexicon(const EnglishLexicon&) = delete;
  EnglishLexicon& operator=(const EnglishLexicon&) = delete;

  // Ascending byte order by `word`; unique words, so this is a total order.
  const std::vector<LexiconEntry>& entries() const { return entries_; }
  std::size_t size() const { return entries_.size(); }

  // Exact membership by canonical (already-lowercase) word.
  //
  // P2.6 added this because spelling suggestions must answer "is this whole word
  // in the list?" and "is this generated variant in the list?" many times per
  // query. Doing either by scanning entries() would be O(size) per variant, which
  // on the 42,356-row pilot list is the difference between a keystroke and a
  // stutter; the loader already guarantees ascending byte order and unique words,
  // so a binary search is exact, order-dependent only in the same way the prefix
  // range search is, and needs no second index and no extra storage.
  //
  // Returns nullptr when the word is absent. The pointer addresses this object's
  // own vector, which is immutable after construction, so it stays valid for the
  // lexicon's lifetime. `canonical_word` must be a lookup key -- lowercase, in the
  // accepted alphabet; this performs no classification, and a word that was never
  // loadable can therefore never be found here either (the loader and
  // IsDictionaryEntryForm share one grammar by construction).
  const LexiconEntry* Find(std::string_view canonical_word) const;

 private:
  EnglishLexicon() = default;
  std::vector<LexiconEntry> entries_;
};

}  // namespace en
}  // namespace epocanva

#endif  // EPOCANVA_EN_LEXICON_H_
