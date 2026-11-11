// Local lexical provider for English, French and Korean dictionary candidates.
// Pure queries over immutable catalog/rule indices. No persistence, raw history,
// network I/O, telemetry, content logs or automatic commits.
#ifndef EPOCANVA_LEXICAL_SERVICE_H_
#define EPOCANVA_LEXICAL_SERVICE_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../keyboard/keyboard_model.h"
#include "text.h"

namespace epocanva::prediction { class Model; struct LoadOptions; }

namespace epocanva::lexical {

using LexemeId = std::uint64_t;  // prediction::StableLexemeId, not a second hash.
constexpr std::size_t kMaxCandidates = 64;  // includes the original candidate
constexpr std::size_t kMaxRankingRecords = 4096;  // statistics records
constexpr std::size_t kMaxPrefixEntriesExamined = 4096;  // indexed dictionary rows

enum class CandidateType {
  kOriginal = 0, kExact, kCompletion, kSpelling, kAccent,
  kMorphology, kSegmentation, kAbbreviation, kContraction,
};
const char* CandidateTypeName(CandidateType type);

struct InputUtf8ByteSpan {
  std::size_t begin_utf8_byte = 0;
  std::size_t end_utf8_byte = 0;
};
struct Candidate {
  CandidateType type = CandidateType::kOriginal;
  std::string text_utf8;
  std::vector<LexemeId> lexeme_ids;
  // Half-open spans in ORIGINAL raw input UTF-8 code units (bytes), not in the
  // normalized key or output. Segmentation has one span per input word.
  std::vector<InputUtf8ByteSpan> input_utf8_byte_spans;
  double score = 0.0;  // legacy combined lexical/count ranking; NOT a probability
  std::uint32_t morphology_forms = 0;  // existing en::MorphologyForm bits
  unsigned edit_distance = 0;  // existing ASCII English OSA edit count
  // Provider-independent additive input/type feature in natural-log score
  // units, NOT a calibrated probability. Contains no unigram, learned, context
  // or corpus-normalization term. Combine this with a language-model log
  // probability ONCE; do not add the legacy score above to that probability.
  // Original/exact/completion/abbreviation/contraction: 0. Accent: -0.5.
  // Morphology: -0.25, plus 1 for an exact English source lemma. Spelling:
  // -4*OSA edits -0.005*keyboard cost (hundredths of an edit). Segmentation:
  // -0.75 per inserted space. Attestation gates are identical for both scores.
  double match_log_score = 0.0;
};

// The caller supplies an admitted ID-only snapshot, never raw text/history.
// allow_learning=false never dereferences either pointer; privacy gating remains
// the caller's responsibility. Unknown IDs are ignored; duplicate/oversized
// snapshots reject all personal features. Nothing is retained between queries.
struct LearnedCount { LexemeId id = 0; std::uint64_t count = 0; };
struct ContextCount {
  LexemeId previous_id = 0;
  LexemeId id = 0;
  std::uint64_t count = 0;
};
struct RankingInputs {
  bool allow_learning = false;
  LexemeId previous_id = 0;
  const std::vector<LearnedCount>* learned = nullptr;
  const std::vector<ContextCount>* context = nullptr;
};
struct QueryOptions {
  std::size_t max_candidates = 16;  // clamped to kMaxCandidates; zero does no work
  // Prefix index traversal examines at most this many dictionary rows; bounded
  // partial results report exhaustion rather than pretending to be exhaustive.
  std::size_t prefix_entry_budget = 512;
  std::size_t accent_entry_budget = 256;
  std::size_t morphology_match_budget = 512;
  std::size_t spelling_cell_budget = 60000;
  std::size_t segmentation_transition_budget = 20000;
  bool enable_morphology = true;
  bool enable_spelling = true;
  bool enable_segmentation = true;
  bool enable_abbreviation = true;
  bool enable_accents = true;
  // Preserve legacy US-QWERTY ranking for existing callers. A backend with an
  // unknown/unmapped active layout must explicitly use kUnknown/kNeutral.
  keyboard::Layout keyboard_layout = keyboard::Layout::kUsQwerty;
};
enum class QueryStatus {
  kOk = 0, kEmptyInput, kInputTooLong, kInvalidUtf8, kUnsupportedLanguage,
  kDictionaryUnavailable, kUnicodeUnavailable, kNormalizationFailure,
  kProtectedText, kMixedCase, kInvalidWord,
};
const char* QueryStatusName(QueryStatus status);
struct QueryResult {
  QueryStatus status = QueryStatus::kOk;
  bool ranking_inputs_valid = true;
  bool prefix_budget_exhausted = false;
  bool accent_budget_exhausted = false;
  bool morphology_budget_exhausted = false;
  bool spelling_budget_exhausted = false;
  bool segmentation_budget_exhausted = false;
  bool english_frequency_available = false;
  std::size_t prefix_entries_examined = 0;  // dictionary rows, not bytes
  std::size_t accent_entries_examined = 0;  // dictionary rows, not bytes
  std::size_t morphology_matches_examined = 0;  // lemma/form pairs
  std::size_t spelling_cells = 0;  // dynamic-programming cells
  std::size_t segmentation_transitions = 0;  // path extensions
  std::vector<Candidate> candidates;
};

enum class BuildStatus {
  kOk = 0, kEmptyCatalog, kUnsupportedLanguage, kInvalidCatalogWord,
  kLexemeIdMismatch, kDuplicateLexeme,
  kUnicodeUnavailable, kNormalizationFailure, kEnglishCoreFailure,
  kModelLoadFailure,
};
const char* BuildStatusName(BuildStatus status);
struct BuildInfo {
  BuildStatus status = BuildStatus::kOk;
  std::size_t catalog_entries = 0;  // admitted dictionary rows
  std::size_t english_ascii_entries = 0;
  std::size_t french_accent_entries = 0;
  std::size_t skipped_catalog_entries = 0;  // punctuation/protected/other language
  std::size_t unicode_unavailable_entries = 0;  // non-Windows-only degradation
  std::size_t alias_collisions = 0;  // distinct known IDs sharing a lookup alias
  // Status name only, never a file path, word or model content.
  std::string dependency_status;
};

class LexicalService {
 public:
  // Copies and indexes the model's known catalog once. The model need not
  // outlive the provider. Model text and IDs remain unchanged. Native NFC/
  // invariant lowercase builds LOOKUP ALIASES only, never new identities.
  // Exact model-normalized text wins, then aliases by unigram count descending
  // and stable ID ascending. Punctuation, unsupported-language and protected
  // non-word rows are not lexical candidates; diagnostics count those rows.
  static std::unique_ptr<LexicalService> Create(
      const prediction::Model& model, BuildInfo* info = nullptr);
  // Delegates to the prediction model's versioned text loader. V1 explicitly
  // rejects binary; no second dictionary wire format or ID namespace exists.
  static std::unique_ptr<LexicalService> FromFile(
      const std::filesystem::path& path, BuildInfo* info = nullptr);
  static std::unique_ptr<LexicalService> FromFile(
      const std::filesystem::path& path, const prediction::LoadOptions& options,
      BuildInfo* info = nullptr);
  ~LexicalService();
  LexicalService(const LexicalService&) = delete;
  LexicalService& operator=(const LexicalService&) = delete;

  // For valid bounded nonempty input, candidate zero is the EXACT raw bytes,
  // kOriginal, with EMPTY lexeme_ids. An admitted exact dictionary candidate may
  // deliberately share its text but has kExact plus a known ID. Other display
  // texts are deduplicated. Invalid UTF-8/oversized inputs produce no candidates.
  // Non-ASCII en/fr get normalized lexical completion/accent support without
  // pretending the existing ASCII English morphology/spelling accepts Unicode.
  // English generated forms require an admitted surface word and carry that
  // SURFACE ID for model/context scoring, not the source lemma. Core-marked
  // possessives may instead retain an admitted lemma ID when their actual base
  // is admitted but the possessive surface is absent. Unattested nonpossessive
  // forms are never offered just because a source lemma is known.
  // Korean completion requires real dictionary rows; composition is elsewhere.
  QueryResult Suggest(std::string_view language, std::string_view raw_utf8,
                      const QueryOptions& options = {},
                      const RankingInputs& ranking = {}) const;

  // Resolves whole input to an admitted catalog ID, never hashes OOV/raw input
  // into a learnable ID. Technical/mixed-case tokens are refused consistently.
  LexemeId FindExact(std::string_view language, std::string_view raw_utf8) const;
  bool HasLexeme(LexemeId id) const;
  std::size_t size() const;  // catalog records

 private:
  struct Impl;
  explicit LexicalService(std::unique_ptr<const Impl> impl);
  std::unique_ptr<const Impl> impl_;
};

}  // namespace epocanva::lexical
#endif
