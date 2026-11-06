// Unified candidate contract.
//
// One internal record that every language candidate source can produce from,
// regardless of whether the text came from the English lexicon, a statistical
// prediction, a Rime conversion, a spelling corrector or an abbreviation.
//
// This is deliberately NOT a new C ABI structure. The public v1 candidate
// record keeps its layout and its learning key (`lex:<id>[+<id>...]`), which
// schema v3 already persists. An additive read API can be versioned later; the
// contract below is the source of truth that such an API would serialize.
//
// Stable identity rules:
//   * language and input scheme are always part of the identity, so the same
//     surface in two languages cannot collide;
//   * a learnable candidate is identified by its content-derived catalog lexeme
//     IDs, never by display-text hashing;
//   * a non-learnable candidate (raw fallback) gets a language+scheme-scoped
//     text hash that is deterministic but must not be persisted as learning;
//   * dictionary/model ordinal numbers are never part of an ID;
//   * Unicode case is folded for English only (ASCII A-Z); this layer does not
//     claim NFC/NFD equivalence or full case folding, and tests state that.
//
// Units: `text_utf8` and `normalized_identity` are UTF-8 code units (bytes);
// `lexeme_ids` count catalog records, never text units.
#ifndef EPOCANVA_CANDIDATES_CANDIDATE_CONTRACT_H_
#define EPOCANVA_CANDIDATES_CANDIDATE_CONTRACT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace epocanva {
namespace candidates {

constexpr std::size_t kMaxLexemeIds = 8;
// Identity text uses the same byte bound as a candidate surface; the learning
// layer separately caps the descriptive normalized field it persists at 96.
constexpr std::size_t kMaxIdentityUtf8Bytes = 4096;
constexpr std::size_t kMaxLanguageTagBytes = 16;
constexpr std::size_t kMaxSchemeIdBytes = 32;
// Admitted ABI enum ranges. These mirror epocanva_backend.h; the backend only
// opens a session after ModeValid() admits the pair, and Validate refuses an
// out-of-range record so a future producer cannot invent an identity value.
constexpr std::uint32_t kFirstLanguageValue = 1;   // EPOCANVA_BACKEND_EN
constexpr std::uint32_t kLastLanguageValue = 5;    // EPOCANVA_BACKEND_KO
constexpr std::uint32_t kFirstInputModeValue = 1;  // EPOCANVA_BACKEND_LATIN
constexpr std::uint32_t kLastInputModeValue = 7;   // EPOCANVA_BACKEND_KANA

// Mirrors EPOCANVA_BACKEND_{RAW,WORD,SPELLING,MORPHOLOGY,EXPANSION,CONVERSION,
// PREDICTION}. The contract does not include the C ABI header; the backend
// adapter maps its public kind onto this enum and a test pins the values.
enum class Source : std::uint32_t {
  kRaw = 0,
  kWord = 1,
  kSpelling = 2,
  kMorphology = 3,
  kExpansion = 4,
  kConversion = 5,
  kPrediction = 6,
};

enum class Match : std::uint32_t {
  kRaw = 0,
  kPrefix = 1,        // the typed token is a prefix of the candidate
  kFuzzy = 2,         // spelling correction or edit-distance match
  kAbbreviation = 3,  // an explicit key expands to the candidate
  kContext = 4,       // next-word/next-unit prediction
  kConversion = 5,    // transliteration or script conversion
};

// What selecting this candidate means to the host. `kReplaceInput` replaces the
// current raw/preedit input; `kInsert` appends text at the caret.
enum class Commit : std::uint32_t {
  kInsert = 0,
  kReplaceInput = 1,
};

// Availability states mirror the locale layer's scheme states. A produced
// candidate is kAvailable; kUnverified/kUnavailable exist so a future adapter
// cannot describe an unimplemented scheme as working.
enum class Availability : std::uint32_t {
  kAvailable = 0,
  kUnverified = 1,
  kUnavailable = 2,
};

struct Identity {
  std::string language_tag;   // lowercase ASCII primary subtag, e.g. "en"
  std::string scheme_id;      // lowercase ASCII scheme, e.g. "latin"
  std::uint32_t language = 0;  // EPOCANVA_BACKEND_{EN..KO} value
  std::uint32_t input_mode = 0;  // EPOCANVA_BACKEND_{LATIN..KANA} value
  std::vector<std::uint64_t> lexeme_ids;  // catalog IDs; empty = not learnable
  std::string normalized_identity;        // language-specific, bounded
};

struct Record {
  Identity identity;
  std::string text_utf8;
  Source source = Source::kRaw;
  Match match = Match::kRaw;
  Commit commit = Commit::kReplaceInput;
  Availability availability = Availability::kAvailable;
  double score = 0.0;
  // 0 means "no calibrated confidence is claimed". Values are never fabricated
  // from the ranking score.
  double source_confidence = 0.0;
  // True only when the candidate carries admitted lexeme IDs and is therefore
  // a legal schema-v3 learning identity. Raw fallbacks are never learnable.
  bool learnable = false;
  // Match range in the session's raw input, in UTF-8 code units (bytes).
  // Validate() can only check ordering because it does not carry the input
  // bytes; backend_session::Session::Publish enforces offset <= raw.size() and
  // a UTF-8 code-unit boundary on both ends immediately before publication.
  std::size_t input_begin_utf8_byte = 0;
  std::size_t input_end_utf8_byte = 0;
  std::string stable_id;
};

// ASCII A-Z to a-z for English; every other language preserves scalars exactly.
// Returns an empty string for empty/invalid/oversized text (including embedded
// NUL, C0/C1 controls, malformed UTF-8 and non-scalar values).
std::string NormalizeIdentity(std::string_view language_tag, std::string_view text_utf8);

// The schema-v3 learning key: "lex:<id>[+<id>...]" or empty when the candidate
// has no admitted IDs. This string is what backend_session already persists.
std::string LearningCandidateId(const std::vector<std::uint64_t>& lexeme_ids);

// Canonical, cross-language-safe candidate ID:
//   "cand:v1:<language>:<scheme>:lex:<id>[+<id>...]"
//   "cand:v1:<language>:<scheme>:text:<fnv1a64-hex>"
// Source engine is metadata, not identity: the same admitted lexeme from a
// lexicon and from prediction has the same ID.
std::string StableId(const Identity& identity);

// Builds a record from the backend's public candidate kind. Returns false only
// for an unknown kind or invalid text; `out` is cleared on failure.
bool Describe(std::string language_tag, std::string scheme_id,
              std::uint32_t language_value, std::uint32_t input_mode_value,
              std::uint32_t abi_kind, std::string_view text_utf8,
              std::vector<std::uint64_t> lexeme_ids, double score,
              Record* out);

// Injectable invariants. Returns false with a stable, text-free reason.
bool Validate(const Record& record, std::string* reason);

}  // namespace candidates
}  // namespace epocanva
#endif  // EPOCANVA_CANDIDATES_CANDIDATE_CONTRACT_H_
