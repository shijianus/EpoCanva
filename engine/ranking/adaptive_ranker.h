// Bounded, deterministic adaptive candidate ranking.
//
// Score unit: "ranking points". The value is an additive log-likelihood-like
// scalar and is NOT a probability and NOT comparable across languages without
// calibration. Every input feature is clamped to a documented interval before
// it is weighted, so no unbounded source count can dominate the order.
//
// Calibration honesty: DefaultWeights() returns ENGINEERING DEFAULTS chosen from
// the existing implementation scales. They have not been fitted on a corpus and
// this module does not claim "intelligent ranking is finished". Replace them
// through offline evaluation; the type is a parameter, not a constant of nature.
//
// Text unit discipline: all text parameters are UTF-8. Prefix matching walks
// whole Unicode scalar values and never slices inside a multibyte sequence.
// Distances, counts and ordinals in this header are explicitly labelled; none of
// them is a UTF-16 code-unit or grapheme-cluster offset.
//
// The ranker reads a LearningEventModel but never writes to it or to disk.
#ifndef EPOCANVA_RANKING_ADAPTIVE_RANKER_H_
#define EPOCANVA_RANKING_ADAPTIVE_RANKER_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "learning_events.h"

namespace epocanva {
namespace ranking {

constexpr std::size_t kMaxProposals = 64;          // candidate proposals
constexpr std::size_t kMaxInputKeyUtf8Bytes = 256; // raw prefix, UTF-8 code units
constexpr std::size_t kMaxDisplayTextUtf8Bytes = 1024;
constexpr double kContextLogProbabilityFloor = -20.0;  // natural log
constexpr double kMinWeight = 0.0;
constexpr double kMaxWeight = 8.0;
constexpr std::uint32_t kDefaultDetectionConfirmations = 3;

// Where a proposal came from. Source only changes ranking through an explicit
// penalty for spelling (edit-distance derived); it never silently outranks an
// exact candidate.
enum class SourceKind {
  kOriginal = 0,  // the user's own raw text
  kExact,
  kCompletion,
  kSpelling,
  kMorphology,
  kAbbreviation,
  kConversion,
  kPrediction,
};

const char* SourceKindName(SourceKind kind);

struct Proposal {
  // Display text, exact UTF-8 bytes. Two proposals with identical bytes are
  // treated as the same candidate regardless of source or language.
  std::string text_utf8;
  // Primary language subtag ("en", "zh", "fr", ...). Required.
  std::string language;
  // Stable identity used for learning. Empty means "not learnable": the
  // candidate is still ranked but receives no personal signal.
  std::string candidate_id;
  // Normalised lexicon weight in [0,1]. Producers must normalise their own
  // counts; collection-tier weights (for example Alt12Dicts inclusion counts)
  // are NOT corpus frequencies and must not be passed as if they were.
  double static_weight = 0.0;
  // Natural-log probability from a context model, clamped here to
  // [kContextLogProbabilityFloor, 0]. 0.0 means "no context evidence", which is
  // exactly what a caller supplies when no model is installed.
  double context_logp = 0.0;
  SourceKind source = SourceKind::kCompletion;
  // ASCII OSA edit distance for spelling proposals; ignored for other sources.
  unsigned edit_distance = 0;
};

// Per-language weights. Each weight scales a feature already normalised to a
// documented interval.
struct Weights {
  double prefix = 3.0;     // scales prefix_quality in [0,1]
  double lexical = 1.5;    // scales static_weight in [0,1]
  double context = 1.0;    // scales clamped log probability in [-20,0]
  double personal = 1.0;   // scales positive preference in [0,2]
  double negative = 1.0;   // scales negative preference in [0,1.5]
  double source = 1.0;     // scales spelling penalty in [-1,0]
  double language = 1.0;   // scales (language_fit - 1) in [-1,0]
};

// Engineering defaults, deliberately conservative. CJK profiles weight context
// higher because a pinyin/CJK conversion has no useful character-prefix signal
// against the output text; that is a modelling statement, not a measured result.
Weights DefaultWeights(std::string_view language);
bool IsValidWeights(const Weights& weights);

struct RankRequest {
  // The language the user explicitly selected. Never overridden by detection.
  std::string user_language;
  // Optional automatic guess for the current token. Only ever a tie-shift.
  std::string detected_language;
  // Current raw prefix in UTF-8 code units (bytes). Empty for next-word queries.
  std::string input_key_utf8;
  std::uint64_t generation = 0;  // candidate-menu generation for learning lookups
  std::vector<Proposal> proposals;
  Weights weights = DefaultWeights("en");
  bool learning_enabled = true;
};

struct RankedCandidate {
  std::size_t proposal_ordinal = 0;  // ordinal in the input vector
  double score = 0.0;                // ranking points
  // 0 for a candidate whose text is exactly the user's raw input, 1 for every
  // other candidate. The tier is a hard rule, not a weight: no combination of
  // model, personal or context scores can displace the user's own text from the
  // first position. Within a tier the ordering is by score, then input ordinal.
  std::uint32_t tier = 1;
};

enum class RankStatus {
  kOk = 0,
  kTooManyProposals,
  kInputTooLong,
  kInvalidInput,
  kInvalidUtf8,
  kInvalidWeights,
};

const char* RankStatusName(RankStatus status);

struct RankResult {
  RankStatus status = RankStatus::kOk;
  // Best first. Tier 0 (the user's exact text) always precedes tier 1.
  std::vector<RankedCandidate> ordered;
  std::size_t dropped_foreign_language = 0;  // proposals, not text units
  std::size_t deduplicated = 0;              // merged duplicate texts
};

// Detection latching: ObservedDetection() is called once per keystroke. The
// effective detected language only changes after `confirmations` consecutive
// identical observations, so a flapping classifier cannot make the candidate
// bar reorder under the user's fingers. A user-selected language always wins.
class AdaptiveRanker {
 public:
  explicit AdaptiveRanker(std::uint32_t detection_confirmations =
                              kDefaultDetectionConfirmations);

  void ObserveDetection(std::string_view language);
  std::string_view effective_detected_language() const { return latched_; }
  void ResetDetection();

  // The ranker holds no reference to `learning`; pass nullptr for a completely
  // model-free path. Ordering remains defined without any learned state.
  RankResult Rank(const RankRequest& request, const LearningEventModel* learning) const;

  // Exposed for tests and diagnostics: prefix quality of `text` against `key`,
  // both UTF-8, measured in whole Unicode scalar values. Returns false on
  // malformed UTF-8.
  static bool PrefixQuality(std::string_view key_utf8, std::string_view text_utf8,
                            double* out_quality, bool* out_exact);

 private:
  std::uint32_t confirmations_ = kDefaultDetectionConfirmations;
  std::uint32_t streak_ = 0;
  std::string pending_;
  std::string latched_;
};

}  // namespace ranking
}  // namespace epocanva
#endif  // EPOCANVA_RANKING_ADAPTIVE_RANKER_H_
