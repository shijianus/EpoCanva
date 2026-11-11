// Standalone French support algorithms. C++17; no Rime or platform dependency.
#ifndef EPOCANVA_FR_FRENCH_CORE_H_
#define EPOCANVA_FR_FRENCH_CORE_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace epocanva {
namespace fr {

// All text, caret offsets and half-open span offsets use UTF-8 code units
// (bytes). No API here accepts UTF-16 offsets or screen-cell columns.
constexpr std::size_t kMaxTextUtf8Bytes = 4096;
constexpr std::size_t kMaxWordUtf8Bytes = 256;
constexpr std::size_t kMaxDictionaryEntries = 4096;  // dictionary records
constexpr std::size_t kMaxDictionaryUtf8Bytes = 1024 * 1024;
constexpr std::size_t kMaxAccentSuggestions = 64;  // candidates
constexpr std::size_t kNoUtf8Byte = static_cast<std::size_t>(-1);

enum class Status {
  kOk,
  kInvalidUtf8,
  kInputTooLong,
  kInvalidCaretUtf8Byte,
  kInvalidDeadAccent,
  kEmptyInput,
  kNotFrenchWord,
  kProtectedToken,
  kUnsupportedCase,
  kDictionaryUnavailable,
  kInvalidDictionary,
  kInvalidLimit,
  kUnsupportedInflection,
  kRuleNotApplicable,
};

struct Utf8Validation {
  Status status = Status::kOk;
  // Lead byte of the first ill-formed sequence, or kNoUtf8Byte on success or
  // an input-size rejection. Valid Unicode noncharacters and NUL are scalars.
  std::size_t error_utf8_byte = kNoUtf8Byte;
};
Utf8Validation ValidateUtf8(std::string_view text_utf8);

struct TextResult {
  Status status = Status::kOk;
  std::string text_utf8;  // empty on error; no replacement characters inserted
};

enum class DeadAccent { kNone, kAcute, kGrave, kCircumflex, kDiaeresis, kCedilla };
// Supported linguistic table (and uppercase counterparts): acute e; grave
// a/e/u; circumflex a/e/i/o/u; diaeresis a/e/i/o/u/y; cedilla c. Diaeresis a/o
// covers loanwords. An unsupported pair returns nullopt, never a guessed letter.
std::optional<char32_t> ComposeLetter(DeadAccent accent, char32_t letter);
std::optional<char32_t> SpacingAccent(DeadAccent accent);

// Only replaces an adjacent ASCII base + one supported combining accent by
// its table entry. Does not reorder marks, decompose characters, compose a
// second accent, normalize compatibility forms, or implement Unicode NFC.
TextResult NormalizeFrench(std::string_view text_utf8);
// Normalizes the above pairs first; maps ASCII, the table's accented letters,
// and oe/ae ligatures U+0153/U+0152 and U+00E6/U+00C6. Other scalars survive.
TextResult ToLowerFrench(std::string_view text_utf8);
TextResult ToUpperFrench(std::string_view text_utf8);

struct Utf8ByteSpan {
  std::size_t begin_utf8_byte = 0;
  std::size_t end_utf8_byte = 0;
};
struct ClusterSegmentation {
  Status status = Status::kOk;
  std::vector<Utf8ByteSpan> clusters;
};
// LIMITED combining clusters: one scalar plus consecutive U+0300..U+036F
// marks; a leading run of those marks is one cluster. Other marks, variation
// selectors, emoji modifiers, ZWJ sequences and regional indicators are separate
// scalars/clusters. This is deliberately not UAX #29 grapheme segmentation.
ClusterSegmentation SegmentCombiningClusters(std::string_view text_utf8);

enum class TokenKind { kFrenchWord, kUrl, kEmail, kNumber, kWhitespace, kOther };
struct TokenAnalysis {
  Status status = Status::kOk;
  TokenKind kind = TokenKind::kOther;
};
// Whole-token grammar: supported French letters, with ASCII apostrophe / U+2019
// or hyphen-minus / U+2010 / U+2011 ONLY between letters. No leading/trailing or
// doubled joiners. Normalizes supported pairs before checking. URL/email/number
// recognition is conservative protection, not an RFC parser or numeric evaluator.
TokenAnalysis ClassifyToken(std::string_view token_utf8);
struct TokenSpan {
  Utf8ByteSpan utf8_bytes;
  TokenKind kind = TokenKind::kOther;
};
struct TokenSegmentation {
  Status status = Status::kOk;
  std::vector<TokenSpan> tokens;
};
// Splits on ASCII whitespace and U+00A0/U+202F. Protected non-whitespace chunks
// remain whole. Otherwise peels ASCII sentence punctuation from chunk edges,
// and classifies the entire remaining chunk. Does not split invalid mixed words
// into apparently correct dictionary fragments. Spans always cover original bytes.
TokenSegmentation SegmentTokens(std::string_view text_utf8);

class Composition {
 public:
  // Preserves the exact supplied bytes. A caret must be a boundary under
  // SegmentCombiningClusters, including 0 and text_utf8.size(). Errors are atomic.
  Status SetText(std::string_view text_utf8, std::size_t caret_utf8_byte);
  Status SetCaret(std::size_t caret_utf8_byte);
  // Inserts valid UTF-8, resolves a pending accent against the first scalar,
  // then applies NormalizeFrench to the buffer. Space emits the spacing accent;
  // unsupported letters emit spacing accent + original text. An empty insert
  // leaves the pending accent alone. The resulting caret follows the inserted
  // combining cluster, including any leading marks already at the insertion.
  Status InsertUtf8(std::string_view text_utf8);
  // Repeating the same dead key emits its spacing form and clears pending state.
  // A different dead key emits the old spacing form and arms the new one.
  Status BeginDeadAccent(DeadAccent accent);
  // Pending accent first, otherwise remove one limited combining cluster
  // immediately before the UTF-8 byte caret. At byte 0 this is a successful no-op.
  Status Backspace();
  void Cancel();

  const std::string& text_utf8() const { return text_utf8_; }
  std::size_t caret_utf8_byte() const { return caret_utf8_byte_; }
  DeadAccent pending_accent() const { return pending_accent_; }

 private:
  Status InsertResolvedUtf8(std::string_view text_utf8);
  std::string text_utf8_;
  std::size_t caret_utf8_byte_ = 0;
  DeadAccent pending_accent_ = DeadAccent::kNone;
};

enum class DictionarySourceStatus {
  kUnavailable,
  kTestFixture,
  kCallerSuppliedUnverified,
};
struct DictionarySource {
  DictionarySourceStatus status = DictionarySourceStatus::kUnavailable;
  // Caller declarations, NOT a licence/redistribution or production-data audit.
  std::string name;
  std::string version;
  std::string licence;
};
struct DictionaryEntry {
  std::string word_utf8;
  std::uint64_t rank = 0;  // caller ranking hint, not claimed corpus frequency
};
struct DictionaryInput {
  DictionarySource source;
  std::vector<DictionaryEntry> entries;
};
struct AccentSuggestion {
  std::string text_utf8;
  std::uint64_t rank = 0;
  std::size_t changed_grapheme_clusters = 0;  // limited clusters defined above
};
struct AccentSuggestionResult {
  Status status = Status::kOk;
  DictionarySourceStatus source_status = DictionarySourceStatus::kUnavailable;
  std::vector<AccentSuggestion> suggestions;
};
// No dictionary is bundled. Each call validates the whole supplied dataset
// atomically (bounds above; nonempty provenance and records; French word grammar).
// Adds accents only where the query has the corresponding unaccented ASCII
// letter; never removes/changes an existing accent, expands oe to a ligature,
// changes joiners, or returns the normalized original. Lower/title/upper case is
// preserved; mixed case is refused. Duplicates retain the largest caller rank.
// Order: rank descending, fewer changed clusters, then UTF-8 byte lexical order.
// max_suggestions is a candidate count in [0, kMaxAccentSuggestions].
AccentSuggestionResult SuggestAccents(std::string_view query_utf8,
                                     const DictionaryInput& dictionary,
                                     std::size_t max_suggestions = 8);

// Lexical classes MUST be selected by the caller; exceptions are not inferred.
// These are orthographic transformations, not gender/POS/semantic inference.
enum class PluralRule {
  kRegularS,  // append s, or invariant for a lemma already ending in s/x/z
  kEauX,      // eau -> eaux
  kAuEuX,    // au/eu -> aux/eux; caller excludes exceptions such as regular -s
  kOuX,      // ou -> oux; caller knows this lexeme belongs to the exception class
  kAlAux,    // al -> aux; caller excludes regular -s exceptions
  kAilAux,   // ail -> aux; caller selects the lexical class
  kInvariant,
};
// A single unjoined French lemma, at most kMaxWordUtf8Bytes. Normalizes supported
// pairs. Lower/title/upper supported; mixed case and compound inflection refused.
TextResult InflectPlural(std::string_view lemma_utf8, PluralRule rule);

// Always support-only: this module supplies no dictionary, Rime schema,
// TSF/IME integration, persistent learning, or product capability declaration.
constexpr bool kProductReady = false;

}  // namespace fr
}  // namespace epocanva
#endif  // EPOCANVA_FR_FRENCH_CORE_H_
