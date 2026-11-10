// Deterministic cross-source candidate fusion (E4).
//
// The merger receives candidates that already carry a valid internal contract
// (engine/candidates/candidate_contract.h) and produces one ordered, deduped
// menu. Design rules, all testable and all tested:
//
//   1. Fail closed. A candidate whose contract is missing, invalid, or lacks
//      any required field (stable_id, language_tag, scheme_id, source, match,
//      commit, learnable consistency) aborts the whole merge with an error —
//      the backend refuses the query instead of publishing a guessed row.
//   2. Raw protection. Candidates whose source is kRaw form the hard tier:
//      they always precede every suggestion, exactly like the existing
//      backend contract, and no fusion score can displace them. A raw row is
//      never merged away by a non-raw row.
//   3. No uncalibrated addition. Cross-source order uses fusion points from
//      the versioned policy (fusion_policy.h); raw source scores are never
//      added to each other. Sources without recorded calibration stay in the
//      fallback band and cannot win against a calibrated row, but they keep
//      their producer order.
//   4. Deterministic dedup. Rows sharing a policy dedup key merge into one,
//      keeping: raw protection (a raw twin suppresses the non-raw twin only
//      by the raw tier rule, never silently), exact expansions (a merged row
//      is exact if ANY provenance row was exact), the most reliable source
//      (lower Source enum value = more exact match semantics; the record kept
//      is the best-ranked provenance row), and the full provenance list
//      (every merged row's stable_id, source, producer ordinal and fusion
//      points) for auditability.
//   5. Provenance order is deterministic: ties fall back to producer
//      insertion ordinal, then stable_id, so repeated runs of the same input
//      produce the same winner and the same provenance.
//
// Units: input offsets are UTF-8 code units (bytes) as in the contract;
// fusion points are dimensionless; ordinals are item counts.
#ifndef EPOCANVA_FUSION_FUSION_MERGER_H_
#define EPOCANVA_FUSION_FUSION_MERGER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "candidate_contract.h"
#include "fusion_policy.h"

namespace epocanva {
namespace fusion {

// Provenance of one merged row: the surviving contract plus every duplicate
// that was folded into it. `stable_id` values are unique across the list
// unless two producers emitted the identical admitted identity, which dedup
// then reports as one row with two provenance entries — auditable, not lost.
struct Provenance {
  std::string stable_id;
  candidates::Source source = candidates::Source::kRaw;
  std::uint32_t producer_insertion_ordinal = 0;
  double fusion_points = 0.0;
  bool exact_expansion = false;
};

struct MergedCandidate {
  // The surviving contract record. Validated before the merge starts.
  candidates::Record record;
  double fusion_points = 0.0;
  bool exact_expansion = false;
  // Provenance rows, best first: the survivor itself is provenance[0].
  std::vector<Provenance> provenance;
  // Number of raw-tier rows in the menu (diagnostic; the raw tier is a hard
  // rule the platform layer already enforces by keeping raw first).
  bool is_raw_tier = false;
};

enum class MergeStatus {
  kOk = 0,
  kMissingContract,    // a candidate without a valid record
  kInvalidContract,    // contract present but Validate() refuses it
  kTooManyCandidates,  // input above the backend's own menu cap
};

const char* MergeStatusName(MergeStatus status);

struct MergeResult {
  MergeStatus status = MergeStatus::kOk;
  // Failure reason is a stable ASCII token; never candidate text.
  std::string reason;
  // Best first: raw tier rows, then calibrated rows, then fallback rows;
  // within a band by fusion points desc, then producer ordinal asc, then
  // stable_id asc. Raw rows keep their producer order.
  std::vector<MergedCandidate> merged;
  std::size_t deduplicated = 0;  // rows folded away by dedup
};

// Upper bound matches backend_session::Session::kMaxCandidates; the backend
// truncates before fusion, so anything larger is a programming error.
constexpr std::size_t kMaxMergeCandidates = 64;

// Inputs: `records` must be parallel to `ordinals` and `exact` (exact-expansion
// flags from the backend adapters; false where the source has no notion of
// exact keys). `ordinals` are the producer insertion ordinals captured at
// collection, before any sorting or truncation.
MergeResult Merge(const std::vector<candidates::Record>& records,
                  const std::vector<std::uint32_t>& ordinals,
                  const std::vector<bool>& exact);

}  // namespace fusion
}  // namespace epocanva
#endif  // EPOCANVA_FUSION_FUSION_MERGER_H_
