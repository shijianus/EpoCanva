#include "fusion_policy.h"

#include <algorithm>
#include <cmath>

namespace epocanva {
namespace fusion {
namespace {

// Recorded-parameter table. Evidence keys refer to the runs documented in
// docs/reports/phase-e-multilingual-fusion.md §"Fusion parameter derivation":
//
//   eval/candidate-v1-r3  — the recorded candidate-menu evaluation round
//       (build/candidate-eval/report.json identity, evaluator schema v3): it
//       established that prediction, conversion (Rime page order, Japanese
//       decoder cost, pinyin initialism log-score), word/morphology (lexical
//       log-count + match penalty) and expansion (log-probability + match
//       log-score) rows are each produced on their own log-like scale and are
//       NOT comparable across sources as raw numbers.
//
//   merge/producer-probe-r1 — the merge probe: replaying the recorded menus
//       through NormalizeToPoints and re-ranking reproduced the recorded
//       winner for every measured case with rank_only sources (parameter-free
//       ordinal mapping) and left raw-first intact. That probe is the recorded
//       basis for shipping rank_only = true for every non-raw source this
//       round: it is honest about not owning a fitted cross-source scale, and
//       it keeps every single-language menu byte-identical (no regression is
//       even possible), while fusion across sources now has a deterministic,
//       versioned, evidence-cited order.
//
// A future round may fit linear/offset per source from a recorded corpus and
// flip calibration to kCalibrated; the version bump is mandatory then.
//
// Raw is special: the user's own input is a hard tier handled by the merger,
// and its points value only exists so a malformed menu cannot produce NaN.
// Function-local statics, not constexpr: the evidence field is a std::string
// and C++17 has no constexpr string on this toolchain.
const SourcePolicy& RawPolicy() {
  static const SourcePolicy policy{candidates::Source::kRaw,
                                   Calibration::kRestrictedFallback, true, 0.0, 0.0,
                                   "eval/candidate-v1-r3: raw is the hard tier"};
  return policy;
}
const SourcePolicy& WordPolicy() {
  static const SourcePolicy policy{candidates::Source::kWord,
                                   Calibration::kRestrictedFallback, true, 0.0, 0.0,
                                   "merge/producer-probe-r1"};
  return policy;
}
const SourcePolicy& SpellingPolicy() {
  static const SourcePolicy policy{candidates::Source::kSpelling,
                                   Calibration::kRestrictedFallback, true, 0.0, 0.0,
                                   "merge/producer-probe-r1"};
  return policy;
}
const SourcePolicy& MorphologyPolicy() {
  static const SourcePolicy policy{candidates::Source::kMorphology,
                                   Calibration::kRestrictedFallback, true, 0.0, 0.0,
                                   "merge/producer-probe-r1"};
  return policy;
}
const SourcePolicy& ExpansionPolicy() {
  static const SourcePolicy policy{candidates::Source::kExpansion,
                                   Calibration::kRestrictedFallback, true, 0.0, 0.0,
                                   "merge/producer-probe-r1"};
  return policy;
}
const SourcePolicy& ConversionPolicy() {
  static const SourcePolicy policy{candidates::Source::kConversion,
                                   Calibration::kRestrictedFallback, true, 0.0, 0.0,
                                   "merge/producer-probe-r1"};
  return policy;
}
const SourcePolicy& PredictionPolicy() {
  static const SourcePolicy policy{candidates::Source::kPrediction,
                                   Calibration::kRestrictedFallback, true, 0.0, 0.0,
                                   "merge/producer-probe-r1"};
  return policy;
}

const SourcePolicy* const kPolicies[] = {&RawPolicy(),       &WordPolicy(),
                                         &SpellingPolicy(),  &MorphologyPolicy(),
                                         &ExpansionPolicy(), &ConversionPolicy(),
                                         &PredictionPolicy()};
static_assert(sizeof(kPolicies) / sizeof(kPolicies[0]) == 7,
              "every candidates::Source value needs a policy row");

}  // namespace

const FusionPolicy& Policy() {
  static const FusionPolicy policy;
  return policy;
}

const SourcePolicy& SourcePolicyFor(candidates::Source source) {
  const auto value = static_cast<std::uint32_t>(source);
  if (value < sizeof(kPolicies) / sizeof(kPolicies[0])) return *kPolicies[value];
  return WordPolicy();  // defensive: unknown source uses a fallback row
}

double NormalizeToPoints(const SourcePolicy& policy, double raw_score,
                         std::uint32_t ordinal, std::uint32_t producer_count) {
  if (!std::isfinite(raw_score)) return kFallbackBandFloor;
  double points = 0.0;
  if (policy.rank_only) {
    // Parameter-free ordinal mapping inside the fallback band: producer order
    // is the prior, exactly the rule the backend already applies, and no
    // uncalibrated raw scale is ever added. A singleton or empty menu still
    // lands on the band floor.
    const double count = producer_count == 0 ? 1.0 : static_cast<double>(producer_count);
    const double position = std::min(static_cast<double>(ordinal), count - 1.0);
    points = kFallbackBandFloor *
             (count <= 1.0 ? 0.0 : 1.0 - position / (count - 1.0));
  } else {
    points = policy.linear * raw_score + policy.offset;
  }
  if (!std::isfinite(points)) return kFallbackBandFloor;
  return std::clamp(points, kFusionScoreMin, kFusionScoreMax);
}

std::string DedupKey(const std::string& language_tag, candidates::Source source,
                     std::string_view text_utf8) {
  // dedup-v1: fold ASCII A-Z only for exact-word surfaces in the ASCII-case
  // languages, mirroring the candidate contract's NormalizeIdentity fold. No
  // NFKC, no full case folding, no NFC — the contract tests already pin that
  // NFC and NFD stay distinct, and the same honesty applies here.
  const bool foldable =
      (source == candidates::Source::kWord || source == candidates::Source::kExpansion ||
       source == candidates::Source::kMorphology) &&
      (language_tag == "en" || language_tag == "fr" || language_tag == "ko");
  std::string key;
  key.reserve(text_utf8.size());
  for (const char byte : text_utf8) {
    const auto value = static_cast<unsigned char>(byte);
    if (value < 0x20 || value == 0x7f) return {};  // control scalars fail closed
    if (foldable && value >= 'A' && value <= 'Z') key.push_back(static_cast<char>(value - 'A' + 'a'));
    else key.push_back(byte);
  }
  // UTF-8 shape check only when folding applied; other scalars are preserved
  // exactly, so byte equality remains byte equality.
  if (foldable) {
    std::size_t offset = 0;
    while (offset < key.size()) {
      const auto lead = static_cast<unsigned char>(key[offset]);
      std::size_t length = 0;
      if (lead < 0x80) length = 1;
      else if (lead >= 0xc2 && lead <= 0xdf) length = 2;
      else if (lead >= 0xe0 && lead <= 0xef) length = 3;
      else if (lead >= 0xf0 && lead <= 0xf4) length = 4;
      else return {};
      if (offset + length > key.size()) return {};
      for (std::size_t index = 1; index < length; ++index) {
        if ((static_cast<unsigned char>(key[offset + index]) & 0xc0) != 0x80) return {};
      }
      offset += length;
    }
  }
  return key;
}

}  // namespace fusion
}  // namespace epocanva
