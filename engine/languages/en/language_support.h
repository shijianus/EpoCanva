// EpoCanva English language algorithms, C++17; no Rime/platform dependency.
//
// CONTRACT: suggestions only. The caller keeps the original candidate at menu
// entry 0, deduplicates text against its existing prefix/one-edit candidates and
// decides whether anything is committed. Nothing here rewrites the input, logs,
// opens files, retains input history or performs network I/O. Build once, query
// the immutable object concurrently; caller-owned statistics must not mutate
// during a call. Privacy-classified fields MUST use allow_learning=false.
//
// SCOPE: existing ASCII token/case/protection rules remain unchanged. Generated
// possessives may end in an ASCII apostrophe, and expansions may contain spaces;
// that does NOT broaden EnglishLexicon's accepted grammar. Morphology is bounded
// orthographic lemma-to-form generation, not POS/stress inference or an English
// grammar checker. See morphology_rules.h for the precise rule-data scope.
//
// FREQUENCY: LexiconEntry::weight is an attestation rank, NOT corpus frequency.
// This API never reads it. FrequencyData is an independent, explicitly labelled
// input. No frequency data is bundled here. The default product therefore lacks
// production frequency coverage; a fixture never becomes production data.
#ifndef EPOCANVA_EN_LANGUAGE_SUPPORT_H_
#define EPOCANVA_EN_LANGUAGE_SUPPORT_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../../keyboard/keyboard_model.h"
#include "english_case.h"
#include "english_lexicon.h"
#include "english_token.h"

namespace epocanva {
namespace en {

using LexemeId = std::uint64_t;
constexpr LexemeId kNoLexemeId = 0;

// Stable across dictionary order/weights and process lifetimes. FNV-1a-64 over
// "epocanva.en.lexeme/v1/" then canonical ASCII dictionary-entry bytes; a zero
// hash is mapped to 1. Returns 0 for an invalid canonical entry. Create checks
// collisions. This is NOT encryption/anonymisation and must not hash raw input
// for persistence. Learning only uses IDs resolved to admitted dictionary rows.
LexemeId StableLexemeId(std::string_view canonical);

enum class CandidateType {
  kMorphology = 0,
  kSegmentation,
  kAbbreviation,
  kSpelling,
  kContraction,
  kPrediction,
};
const char* CandidateTypeName(CandidateType type);

// Half-open offsets into the ORIGINAL query, in UTF-8 code units (bytes), not
// offsets into Candidate::text, UTF-16 units, graphemes or screen-cell columns.
struct SegmentUtf8Byte {
  std::size_t begin_utf8_byte = 0;
  std::size_t end_utf8_byte = 0;
};

struct Candidate {
  CandidateType type = CandidateType::kMorphology;
  std::string text;  // selected display/commit text, in the input's ASCII case
  double score = 0.0;  // larger first; deterministic text/type tie breakers
  std::vector<SegmentUtf8Byte> segments_utf8_byte;
  // Morphology: attested surface ID when present; otherwise admitted lemma ID
  // for legacy generated forms (including unattested possessive surfaces).
  // Segmentation: one ID per input segment. Spelling: corrected dictionary row
  // ID. Expansion: one ID per output word, with 0 for a word absent from the
  // dictionary. Never learn 0 or mistake a lemma fallback for surface identity.
  std::vector<LexemeId> lexeme_ids;
  std::uint32_t morphology_forms = 0;  // MorphologyForm bit mask, see its header
  unsigned edit_distance = 0;  // spelling only: optimal-string-alignment edits
  unsigned keyboard_cost = 0;  // spelling only: hundredths of an edit cost
  // Additive input/type feature in natural-log score units, NOT a calibrated
  // probability. Independent of base/learned/context counts and corpus totals.
  // Kept separate so a consumer can add its own language-model log probability
  // exactly once instead of adding it to the legacy combined score above.
  double match_log_score = 0.0;
};

enum class FrequencySource {
  kUnavailable = 0,
  kTestFixture,
  kProductionCorpus,
};
struct FrequencyRecord {
  LexemeId id = kNoLexemeId;
  std::uint64_t count = 0;
};
struct FrequencyData {
  FrequencySource source = FrequencySource::kUnavailable;
  // Required for a supplied dataset. Caller-attested provenance, not a claim
  // that this library has independently audited a corpus or its redistribution.
  std::string provenance;
  std::vector<FrequencyRecord> entries;
};

struct LearnedFrequency {
  LexemeId id = kNoLexemeId;
  std::uint64_t count = 0;
};
struct ContextFrequency {
  LexemeId previous_id = kNoLexemeId;
  LexemeId id = kNoLexemeId;
  std::uint64_t count = 0;
};
constexpr std::size_t kMaxRankingRecords = 4096;  // statistics records, not bytes
struct RankingInputs {
  LexemeId previous_id = kNoLexemeId;
  bool allow_learning = false;
  const std::vector<LearnedFrequency>* learned = nullptr;
  const std::vector<ContextFrequency>* context = nullptr;
};

// Separate base/learned/context channels, with natural logarithms:
// base = log(1 + independent count), learned = 0.75*log(1 + learned count),
// context = 1.25*log(1 + bigram count). Missing counts contribute zero.
// Snapshot records may be unsorted; duplicates or more than kMaxRankingRecords
// in total reject the ENTIRE learned/context snapshot (base scores survive).
// Unknown IDs are ignored, never enrolled. allow_learning=false does not read
// either vector. These are ranking features, not calibrated probabilities.
struct ScoreBreakdown {
  bool known_lexeme = false;
  bool has_base_frequency = false;
  bool ranking_inputs_valid = true;
  std::uint64_t base_count = 0;
  std::uint64_t learned_count = 0;
  std::uint64_t context_count = 0;
  double base = 0.0;
  double learned = 0.0;
  double context = 0.0;
  double total = 0.0;
};

enum class LanguageSupportBuildStatus {
  kOk = 0,
  kLexemeIdCollision,
  kInvalidFrequencySource,
  kMissingFrequencyProvenance,
  kEmptyFrequencyData,
  kInvalidFrequencyRecord,
  kDuplicateFrequencyRecord,
  kUnknownFrequencyLexeme,
  kFrequencyCountOverflow,
};
struct LanguageSupportBuildInfo {
  LanguageSupportBuildStatus status = LanguageSupportBuildStatus::kOk;
  std::size_t lexeme_count = 0;  // dictionary rows
  std::size_t frequency_count = 0;  // independently counted dictionary rows
  std::size_t generated_form_count = 0;  // lemma/form pairs, not bytes
};
const char* LanguageSupportBuildStatusName(LanguageSupportBuildStatus status);

constexpr std::size_t kMaxLanguageCandidates = 64;  // candidates
constexpr std::size_t kMaxMorphologyMatches = 4096;  // examined lemma/form pairs
constexpr std::size_t kMaxSpellingCells = 250000;  // evaluated DP cells
constexpr std::size_t kMaxSegmentationTransitions = 100000;  // path extensions
constexpr std::size_t kMaxSegmentationWords = 8;  // words per full-input path
constexpr std::size_t kMaxSegmentationCandidates = 8;  // paths per DP state
struct LanguageSupportOptions {
  bool enable_morphology = true;
  bool enable_segmentation = true;
  bool enable_abbreviation = true;  // includes the explicit contraction rules
  bool enable_spelling = true;
  std::size_t max_candidates = 16;
  std::size_t morphology_match_budget = 512;
  std::size_t spelling_cell_budget = 60000;
  std::size_t segmentation_transition_budget = 20000;
  std::size_t max_segmentation_words = kMaxSegmentationWords;
  std::size_t prediction_entry_budget = 4096;  // known frequency-covered rows
  // Historical default is explicit; callers with no supported active-layout
  // mapping must select kUnknown/kNeutral, never infer QWERTY from language.
  keyboard::Layout keyboard_layout = keyboard::Layout::kUsQwerty;
};

enum class LanguageSupportOutcome {
  kQueried = 0,
  kEmptyInput,
  kInputTooLong,
  kNonAscii,
  kProtectedText,
  kUnsupportedByte,
  kIllegalWordForm,
  kMixedCase,
};
enum class LanguageFeatureStatus {
  kDisabled = 0,
  kNotApplicable,
  kFrequencyUnavailable,
  kNoMatch,
  kCandidates,
  kBudgetExhausted,  // candidates, if any, are valid but not exhaustive
};
struct LanguageSupportResult {
  TokenAnalysis token{};
  CaseStyle case_style = CaseStyle::kEmpty;
  LanguageSupportOutcome outcome = LanguageSupportOutcome::kEmptyInput;
  FrequencySource frequency_source = FrequencySource::kUnavailable;
  bool production_frequency_supplied = false;  // declared source, NOT audit
  bool ranking_inputs_valid = true;
  bool recognized_word_form = false;  // exact dictionary OR generated form
  LanguageFeatureStatus morphology = LanguageFeatureStatus::kDisabled;
  LanguageFeatureStatus segmentation = LanguageFeatureStatus::kDisabled;
  LanguageFeatureStatus abbreviation = LanguageFeatureStatus::kDisabled;
  LanguageFeatureStatus spelling = LanguageFeatureStatus::kDisabled;
  LanguageFeatureStatus prediction = LanguageFeatureStatus::kDisabled;
  std::size_t prediction_entries_examined = 0;
  std::size_t morphology_matches_examined = 0;
  std::size_t spelling_cells = 0;
  std::size_t segmentation_transitions = 0;
  std::vector<Candidate> candidates;
};

class LanguageSupport {
 public:
  // Copies/indexes immutable dictionary and rule data once. The input lexicon
  // need not outlive this object. A malformed supplied frequency dataset fails
  // atomically; unavailable data is a valid, explicitly degraded configuration.
  static std::unique_ptr<LanguageSupport> Create(
      const EnglishLexicon& lexicon, const FrequencyData& frequencies = {},
      LanguageSupportBuildInfo* info = nullptr);
  ~LanguageSupport();
  LanguageSupport(const LanguageSupport&) = delete;
  LanguageSupport& operator=(const LanguageSupport&) = delete;

  // Does not call or suppress CompletePrefix/SuggestSpelling; can run alongside
  // both. Original text is excluded. Known words/forms are not spell-corrected
  // or segmented. Spelling accepts 4..32 ASCII-letter UTF-8 bytes and <=2 OSA
  // edits. Morphology score = surface ranking total (lemma fallback if absent)
  // - 0.25, plus 1.0 when the complete query is that lemma (exact-lemma forms
  // precede mere prefix-derived forms when no frequency/context separates them).
  // Spelling score = ranking total - 4*edit_distance - 0.005*keyboard_cost.
  // Keyboard costs: normal edit 100, selected-layout neighbour substitution 60,
  // adjacent input-byte transposition 80; neutral/unknown/invalid layouts give
  // no neighbour discount. Minimum edit count wins before cost within each
  // word's alignment. Overall candidate ordering still incorporates frequency.
  // match_log_score retains only the above morphology/spelling input terms;
  // segmentation uses -0.75 per inserted space, without any corpus normalizer.
  // Expansion and prediction match_log_score are zero.
  //
  // Segmentation is top-k DP with (input byte boundary, last lexeme ID, word
  // count) state, so learned bigrams do not invalidate optimal substructure.
  // Only independently frequency-covered words are edges; single-byte words
  // must be a/i. Score = sum(ScoreLexeme.total - log(total_count + covered_rows))
  // minus 0.75 per inserted space. No OOV guesses or partial-input paths.
  //
  // Budgets are hard-clamped to the constants above, zero means no work for
  // that feature. max_candidates==0 still reports gates but performs no search.
  // All input/segment arithmetic is in UTF-8 bytes. No token is truncated.
  LanguageSupportResult Suggest(
      std::string_view raw_input, const RankingInputs& ranking = {},
      const LanguageSupportOptions& options = {}) const;

  // Next-word suggestions from known, independently frequency-covered rows.
  // No input text, automatic space or commit; lowercase text and empty input
  // segments. Missing frequency gives kFrequencyUnavailable, never weight-based
  // fallback. Scans at most min(prediction_entry_budget, 65536) rows in canonical
  // byte order and reports kBudgetExhausted if the scan was incomplete.
  LanguageSupportResult Predict(
      const RankingInputs& ranking = {},
      const LanguageSupportOptions& options = {}) const;

  ScoreBreakdown ScoreLexeme(LexemeId id,
                            const RankingInputs& ranking = {}) const;
  bool HasLexeme(LexemeId id) const;
  FrequencySource frequency_source() const;

 private:
  struct Impl;
  explicit LanguageSupport(std::unique_ptr<const Impl> impl);
  std::unique_ptr<const Impl> impl_;
};

}  // namespace en
}  // namespace epocanva

#endif  // EPOCANVA_EN_LANGUAGE_SUPPORT_H_
