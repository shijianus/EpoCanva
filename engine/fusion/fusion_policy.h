// Versioned fusion policy: per-source normalization parameters and admission
// states for the candidate fusion layer (E4).
//
// This module is the ONLY place fusion ranking parameters live. The parameters
// are derived from the recorded candidate evaluation (build/candidate-eval
// baseline rounds and the merge-probe round documented in
// docs/reports/phase-e-multilingual-fusion.md); each source's parameters carry
// an explicit calibration status derived from that recorded evidence, never
// from intuition.
//
// Two admission states exist:
//   * kCalibrated     — the source's raw score scale is mapped onto the common
//                       "fusion points" interval by the recorded parameters;
//                       the source may win the default fusion.
//   * kRestrictedFallback — no recorded evaluation justifies a mapping yet.
//                       The source keeps its producer order inside the
//                       fallback band and can never displace a calibrated row
//                       in the default fusion. This is fail-closed: the
//                       absence of evidence lowers the source's reach instead
//                       of inventing parameters.
//
// Units: fusion scores are dimensionless "fusion points" in [0, 1000]. The
// interval is bounded so no unbounded source scale can dominate the order.
// All text parameters are UTF-8 code units (bytes); counts are item counts.
#ifndef EPOCANVA_FUSION_FUSION_POLICY_H_
#define EPOCANVA_FUSION_FUSION_POLICY_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "candidate_contract.h"

namespace epocanva {
namespace fusion {

constexpr std::uint32_t kFusionPolicyVersion = 1;
constexpr double kFusionScoreMin = 0.0;
constexpr double kFusionScoreMax = 1000.0;
// Band layout inside [0, 1000]: calibrated sources rank inside the calibrated
// band, restricted sources are clamped into the fallback band, and the raw
// tier sits above both because it is a hard rule, not a score.
constexpr double kCalibratedBandFloor = 400.0;
constexpr double kFallbackBandFloor = 100.0;

enum class Calibration : std::uint32_t {
  kCalibrated = 0,
  kRestrictedFallback = 1,
};

// Normalization parameters for one candidate source, derived from recorded
// evaluation data. `linear`/`offset` map a source's raw score onto fusion
// points: fusion = linear * raw + offset, clamped to the source's band. A
// source without a defensible mapping carries rank_only = true: its fusion
// score is derived from its producer ordinal inside the fallback band, so the
// backend's producer order is preserved verbatim and no invented scale is
// introduced.
struct SourcePolicy {
  candidates::Source source = candidates::Source::kRaw;
  Calibration calibration = Calibration::kRestrictedFallback;
  bool rank_only = true;
  double linear = 0.0;
  double offset = 0.0;
  // Recorded evidence for these parameters: the evaluation report or probe
  // that produced them. Never empty; the report cites the same string.
  std::string evidence;
};

// The versioned policy itself. Every field is fixed by code below; there is
// intentionally no parser, so a second machine-readable copy cannot drift from
// what the runtime actually executes. data/eval/candidate-v1/fusion-policy.json
// mirrors these values for tooling, and its content test pins the equality.
struct FusionPolicy {
  std::uint32_t version = kFusionPolicyVersion;
  // Dedup key version: "dedup-v1" folds ASCII A-Z case for en/fr/ko word and
  // expansion sources only (the same fold the candidate contract already
  // makes), preserves every other scalar, and never applies NFKC or full
  // Unicode case folding. See DedupKey().
  std::uint32_t dedup_version = 1;
};

// The single policy instance. Parameters are recorded evidence, not constants
// of nature: changing them requires a new recorded evaluation and a version
// bump of data/eval/candidate-v1/fusion-policy.json.
const FusionPolicy& Policy();

// The policy entry for one source; every Source value has an entry.
const SourcePolicy& SourcePolicyFor(candidates::Source source);

// Normalizes one raw source score onto fusion points. `ordinal` is the
// candidate's producer insertion ordinal and `producer_count` the menu size at
// collection; rank_only sources derive their points from the ordinal. Returns
// a value in [kFusionScoreMin, kFusionScoreMax]; invalid inputs yield the
// fallback floor, never NaN.
double NormalizeToPoints(const SourcePolicy& policy, double raw_score,
                         std::uint32_t ordinal, std::uint32_t producer_count);

// The dedup key under the policy's dedup_version. Word/expansion surfaces in
// the ASCII-case languages (en, fr, ko) fold A-Z like the candidate contract;
// every other scalar and every other source preserves bytes exactly. Invalid
// UTF-8 yields an empty key, which the merger treats as fail-closed.
std::string DedupKey(const std::string& language_tag, candidates::Source source,
                     std::string_view text_utf8);

}  // namespace fusion
}  // namespace epocanva
#endif  // EPOCANVA_FUSION_FUSION_POLICY_H_
