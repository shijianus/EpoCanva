#ifndef EPOCANVA_PREDICTION_MODEL_H_
#define EPOCANVA_PREDICTION_MODEL_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace epocanva {
namespace prediction {

using LexemeId = std::uint64_t;
constexpr LexemeId kEosId = 0;  // Boundary outcome only; never a lexicon row.
constexpr std::uint32_t kModelVersion = 1;
constexpr std::uint32_t kStableIdVersion = 1;
constexpr char kNormalizationId[] = "ascii-lower-utf8-v1";
constexpr char kStableIdScheme[] = "fnv1a64-language-normalized-v1";
constexpr std::size_t kMaxLexemeUtf8Bytes = 256;
constexpr std::size_t kMaxLanguageIdBytes = 32;
constexpr std::size_t kMaxSuggestions = 64;
constexpr std::size_t kMaxContinuationWords = 4;
constexpr std::size_t kMaxPersonalRecords = 4096;
constexpr double kBigramPrior = 8.0;
constexpr double kTrigramPrior = 4.0;
constexpr double kPersonalPrior = 16.0;

// Strict Unicode scalar UTF-8: no overlong encodings, surrogates, truncated
// scalars, or values above U+10FFFF. All text offsets/counts here are UTF-8
// code units (bytes); collection ordinals count records, never text units.
bool IsValidUtf8(std::string_view utf8);
bool IsValidLanguageId(std::string_view language_id);
// ASCII A-Z -> a-z only; other scalar bytes are preserved, NOT NFC/casefold.
// Rejects empty/>256-byte tokens, Unicode whitespace and C0/C1 controls.
// On failure clears out. No input is retained; out may alias the input string.
bool NormalizeLexemeUtf8(std::string_view utf8, std::string* out);
// Requires already-normalized text. Returns 0 on invalid input. FNV-1a-64 over
// "epocanva.prediction.lexeme/v1/" + language_id + "/" + normalized_utf8;
// offset 14695981039346656037, prime 1099511628211; hash 0 maps to 1.
// This namespace is NOT en::StableLexemeId. Map admitted dictionary rows by
// canonical text; never cast legacy IDs or hash arbitrary input for persistence.
LexemeId StableLexemeId(std::string_view language_id,
                       std::string_view normalized_utf8);

enum class SourceKind { kUnavailable = 0, kTestFixture, kProductionCorpus };
enum class TokenKind { kWord = 0, kPunctuation };
enum class Status {
  kOk = 0,
  kModelUnavailable,
  kIoError,
  kInvalidFormat,
  kUnsupportedVersion,
  kInvalidUtf8,
  kInvalidLexeme,
  kDuplicateRecord,
  kUnknownId,
  kCountOverflow,
  kInconsistentTotals,
  kFingerprintMismatch,
  kLimitExceeded,
  kTestFixtureDenied,
  kInvalidContext,
  kInvalidOptions,
  kInvalidPersonalCounts,
  kBudgetExhausted,
  kBeamPruned,
};
const char* StatusName(Status status);

struct LexemeRecord {
  LexemeId id = 0;
  std::string language_id;
  std::string normalized_utf8;
  TokenKind kind = TokenKind::kWord;
};
struct UnigramRecord { LexemeId id = 0; std::uint64_t count = 0; };
struct BigramRecord {
  LexemeId previous_id = 0;
  LexemeId id = 0;
  std::uint64_t count = 0;
};
struct TrigramRecord {
  LexemeId previous2_id = 0;
  LexemeId previous1_id = 0;
  LexemeId id = 0;
  std::uint64_t count = 0;
};
struct ModelMetadata {
  std::uint32_t version = kModelVersion;
  std::uint32_t stable_id_version = kStableIdVersion;
  std::string normalization_id = kNormalizationId;
  SourceKind source = SourceKind::kUnavailable;
  // Caller-attested SHA-256 of the training source/provenance manifest, exactly
  // 64 lowercase hex ASCII bytes. Not a corpus licence or authenticity claim.
  std::string source_fingerprint;
  std::uint64_t lexeme_count = 0;  // lexical records, excluding EOS
  std::uint64_t unigram_total = 0;  // observed token/boundary occurrences
  std::uint64_t bigram_total = 0;   // observed adjacent pairs
  std::uint64_t trigram_total = 0;  // observed adjacent triples
};
struct ModelRecords {
  ModelMetadata metadata;
  std::vector<LexemeRecord> lexicon;
  std::vector<UnigramRecord> unigrams;
  std::vector<BigramRecord> bigrams;
  std::vector<TrigramRecord> trigrams;
};
struct BuildInfo {
  Status status = Status::kOk;
  std::size_t line_ordinal = 0;  // 1-based TSV line; 0 if not applicable
  bool source_fingerprint_matched = false;
  bool payload_fingerprint_verified = false;
};
struct LoadOptions {
  // If nonempty, MUST match source_fingerprint. Trusted packaging/service code
  // supplies this value; a self-declared source fingerprint is not verification.
  std::string expected_source_fingerprint;
  std::size_t max_file_utf8_bytes = 256u * 1024u * 1024u;
  bool allow_test_fixture = false;
};

// Ephemeral caller-owned aggregate ID/count snapshot, distinct from static
// corpus counts. No raw text, app identity, timestamp, file, or retention here.
// Passing nullptr disables personal scoring WITHOUT disabling static context.
// Caller MUST gate every snapshot against learning consent, sensitive-field
// policy and current focus/generation. IDs/counts are personal, not anonymous.
// The snapshot must not mutate during a query. Prepared once per API call,
// never per candidate/beam node, and never cached in the shared Model.
struct PersonalCounts {
  std::vector<UnigramRecord> unigrams;
  std::vector<BigramRecord> bigrams;
  std::vector<TrigramRecord> trigrams;
};
using Context = std::vector<LexemeId>;  // <=2 IDs, oldest -> newest
struct ScoreResult {
  Status status = Status::kModelUnavailable;
  double static_probability = 0.0;
  double probability = 0.0;
  double log_probability = 0.0;  // natural logarithm, larger is better
  bool personal_applied = false;
  bool context_backed_off = false;
};
struct PredictOptions {
  std::size_t max_suggestions = 8;  // 0..64; larger values are invalid
  // 1..4 emitted lexical tokens; terminal punctuation consumes one slot.
  std::size_t max_words = 1;
  std::size_t beam_width = 16;  // 1..64 active paths; finite beam is explicit
  // Ranked index entries visited ACROSS the whole query, including filtered
  // or duplicate IDs. 0 means exhaustive/exact next-token search, not no work.
  std::size_t max_index_entries = 0;
  std::string language_id;  // empty = all languages; otherwise exact ID filter
};
struct Suggestion {
  std::vector<LexemeId> lexeme_ids;  // EOS never exposed as a commit token
  // One normalized UTF-8 token per ID. Host owns language-specific joining,
  // case restoration, spacing, original candidate preservation and commit.
  std::vector<std::string> tokens_utf8;
  double log_probability = 0.0;  // sum of conditional natural log probabilities
  bool terminal = false;  // EOS or explicit punctuation; never extended
  bool ended_with_eos = false;
};
struct PredictResult {
  Status status = Status::kModelUnavailable;
  bool complete = false;  // exact top-k only if no budget/beam pruning occurred
  bool budget_exhausted = false;
  bool beam_pruned = false;
  bool context_backed_off = false;
  std::size_t index_entries_visited = 0;
  std::size_t candidates_scored = 0;
  std::size_t personal_preparations = 0;  // 0 or 1 for the entire query
  std::vector<Suggestion> suggestions;
};

class Model {
 public:
  // Atomic construction. Load opens only this one local model file, never query
  // input. Binary/unknown header versions, invalid UTF-8/counts/IDs/checksums,
  // noncanonical TSV ordering and inconsistent totals are rejected.
  static std::shared_ptr<const Model> Load(
      const std::filesystem::path& path, const LoadOptions& options = {},
      BuildInfo* info = nullptr);
  // Accepts unsorted records; validates, owns and indexes a complete copy once.
  static std::shared_ptr<const Model> FromRecords(
      const ModelRecords& records, BuildInfo* info = nullptr);
  static std::shared_ptr<const Model> Unavailable();
  ~Model();
  Model(const Model&) = delete;
  Model& operator=(const Model&) = delete;

  bool available() const;
  const ModelMetadata& metadata() const;
  const std::vector<LexemeRecord>& lexemes() const;  // ascending stable ID
  const LexemeRecord* Find(LexemeId id) const;  // immutable; lifetime of Model
  const LexemeRecord* Find(std::string_view language_id,
                           std::string_view normalized_utf8) const;
  std::uint64_t UnigramCount(LexemeId id) const;  // 0 if absent; no smoothing
  // Unknown context IDs back off to the known suffix; EOS resets context.
  // >2 context IDs is invalid. Unknown target IDs are rejected; EOS is scoreable.
  ScoreResult Score(const Context& context, LexemeId id,
                    const PersonalCounts* personal = nullptr) const;
  // Empty/nullopt prefix = next token. Prefix is strict UTF-8, normalized once
  // with the same ASCII-only rule, then matched at UTF-8 byte boundaries.
  // No files/network/logging/persistence or shared mutable query state.
  PredictResult Predict(std::optional<std::string_view> prefix_utf8,
                        const Context& context,
                        const PredictOptions& options = {},
                        const PersonalCounts* personal = nullptr) const;

 private:
  struct Impl;
  explicit Model(std::unique_ptr<const Impl> impl);
  std::unique_ptr<const Impl> impl_;
};

}  // namespace prediction
}  // namespace epocanva
#endif  // EPOCANVA_PREDICTION_MODEL_H_
