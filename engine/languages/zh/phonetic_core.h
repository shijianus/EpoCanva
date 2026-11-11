// Standalone Mandarin phonetics. No Rime/platform headers, I/O or history.
#ifndef EPOCANVA_ZH_PHONETIC_CORE_H_
#define EPOCANVA_ZH_PHONETIC_CORE_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace epocanva {
namespace zh {

// All text spans, sizes and diagnostic offsets use UTF-8 code units (bytes).
// Collection ordinals/counts below select objects, never text/code units.
constexpr std::size_t kMaxInputBytes = 512;
constexpr std::size_t kMaxSyllables = 64;
constexpr std::size_t kMaxAlternatives = 32;
constexpr std::size_t kMaxSyllableBytes = 24;
constexpr std::size_t kMaxDictionaryEntries = 4096;
constexpr std::size_t kMaxDictionaryBytes = 1024 * 1024;
constexpr std::size_t kMaxCandidateBytes = 1024;
constexpr std::size_t kMaxCandidates = 64;
constexpr std::size_t kNoByteOffset = std::string_view::npos;

enum class Status {
  kOk,
  kEmpty,
  kIncomplete,
  kInvalidUtf8,
  kInvalidPhonetic,
  kProtectedInput,
  kLimitExceeded,
  kInvalidOptions,
  kAmbiguous,
  kInvalidSelection,
  kUnavailable,
  kInvalidData,
};

enum class Tone { kUnspecified, kFirst, kSecond, kThird, kFourth, kNeutral };
enum class PhoneticSystem { kPinyin, kZhuyin };
enum class InputMode { kPinyin, kZhuyinSymbols, kZhuyinStandardKeyboard };
enum class SourceStatus { kUnavailable, kCallerSupplied };

struct ByteSpan {
  std::size_t begin_byte = 0;
  std::size_t end_byte = 0;  // Exclusive; both boundaries are in the original UTF-8.
};

struct Limits {
  std::size_t max_input_bytes = 256;
  std::size_t max_syllables = 32;     // Syllable count, including a pending syllable.
  std::size_t max_alternatives = 8;  // Path count; not a text offset.
};

struct Syllable {
  ByteSpan source_bytes;
  // Lowercase, toneless ASCII key: n/l keep u distinct from v; j/q/x/y use u.
  // The standalone e-circumflex interjection uses the explicit key "eh".
  std::string canonical_pinyin;
  Tone tone = Tone::kUnspecified;
  std::string phonetic_utf8;  // Canonical spelling in the requested system, with tone.
};

struct SyllableResult {
  Status status = Status::kEmpty;
  std::size_t error_byte = kNoByteOffset;
  Syllable syllable;
};

struct PhoneticPath {
  PhoneticSystem system = PhoneticSystem::kPinyin;
  std::vector<Syllable> syllables;
  // Set only for an unfinished suffix or a trailing explicit boundary.
  std::optional<ByteSpan> pending_bytes;
  std::string phonetic_utf8;  // Complete syllables joined by ' (Pinyin) / space (Zhuyin).
};

struct SegmentationResult {
  Status status = Status::kEmpty;
  std::size_t input_bytes = 0;
  std::size_t error_byte = kNoByteOffset;
  // Includes alternatives hidden by max_alternatives; never silently de-ambiguous.
  bool ambiguous = false;
  bool alternatives_truncated = false;
  std::vector<PhoneticPath> alternatives;
};

// Explicit Mandarin mode only: not a language detector or an English fallback.
// Pinyin accepts v/ü, precomposed vowel tone marks, or one suffix digit 1..5.
// No n/l substitution, u:/ue correction, abbreviation, fuzzy spelling or corpus score.
SyllableResult ParsePinyinSyllable(std::string_view input_utf8);
SyllableResult ParseZhuyinSyllable(std::string_view input_utf8);
SegmentationResult SegmentPinyin(std::string_view input_utf8, Limits limits = {});
// Zhuyin boundaries are explicit spaces or suffix tones, not guessed word breaks.
SegmentationResult AnalyzeZhuyin(std::string_view input_utf8, Limits limits = {});

struct CommitResult {
  Status status = Status::kEmpty;
  std::string phonetic_utf8;  // Never generated Hanzi.
  std::size_t consumed_input_bytes = 0;
};

class PhoneticComposer {
 public:
  explicit PhoneticComposer(InputMode mode, Limits limits = {});
  // Transactional: rejected input leaves preedit unchanged. Keyboard mode accepts
  // direct Zhuyin here, NOT pasted ASCII key sequences; those remain protected.
  Status AppendText(std::string_view text_utf8);
  // Only for deliberate standard-Zhuyin key events; mapped punctuation is phonetic.
  // The resulting preedit/spans contain symbols, not the original keyboard bytes.
  Status TypeKey(char ascii_key);
  // Removes one complete UTF-8 scalar encoding from the end, not a grapheme cluster.
  bool Backspace();
  void Cancel() noexcept;
  const std::string& preedit_utf8() const noexcept;
  SegmentationResult Analyze() const;
  // A path ordinal is mandatory when ambiguous, even if only one path was retained.
  // Success clears preedit; any failure preserves it. An ordinal is not a text offset.
  CommitResult CommitPhonetic(
      std::optional<std::size_t> alternative_ordinal = std::nullopt);

 private:
  InputMode mode_;
  Limits limits_;
  std::string preedit_utf8_;
};

struct DictionarySchema {
  std::string schema_id;
  PhoneticSystem system = PhoneticSystem::kPinyin;
  bool tone_sensitive = true;
};

struct DictionaryEntry {
  // Explicit syllable boundaries, one full syllable per string in schema.system.
  std::vector<std::string> syllables_utf8;
  std::string text_utf8;  // Entire candidate supplied by the caller, never invented.
};

struct DictionaryData {
  SourceStatus source_status = SourceStatus::kUnavailable;
  std::string source_id;  // Caller provenance label, not independently approved.
  DictionarySchema schema;
  std::vector<DictionaryEntry> entries;
};

struct Candidate {
  std::string text_utf8;
  std::size_t source_entry_ordinal = 0;  // Row ordinal, not a text offset.
};

struct ConversionResult {
  Status status = Status::kUnavailable;
  SourceStatus source_status = SourceStatus::kUnavailable;
  bool candidates_truncated = false;
  // Always false in this core, even with caller-supplied rows.
  bool frequency_ranking_available = false;
  bool traditional_simplified_available = false;
  std::vector<Candidate> candidates;
};

// Exact whole-reading lookup in source order; no word scoring, phrase synthesis,
// script conversion, dictionary loader, network, learning or persistence.
// No source => kUnavailable, not successful Hanzi/phrase support with an empty list.
// The entire supplied table is validated before any result can be returned.
ConversionResult ConvertPhonetic(const PhoneticPath& reading,
                                 const DictionaryData& data = {},
                                 std::size_t max_candidates = 16);

}  // namespace zh
}  // namespace epocanva

#endif  // EPOCANVA_ZH_PHONETIC_CORE_H_
