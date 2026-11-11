#include "fusion_merger.h"

#include <algorithm>
#include <map>
#include <utility>

namespace epocanva {
namespace fusion {
namespace {

// Reliability order for choosing the survivor among cross-source duplicates.
// Lower Source value wins: raw beats word beats spelling etc. This is the
// same ordering the public kind enum already encodes (the more exact match
// semantics have the lower value), so no second reliability table exists.
bool MoreReliable(candidates::Source a, candidates::Source b) {
  return static_cast<std::uint32_t>(a) < static_cast<std::uint32_t>(b);
}

char UnitSeparator() { return '\x1f'; }

}  // namespace

const char* MergeStatusName(MergeStatus status) {
  switch (status) {
    case MergeStatus::kOk: return "ok";
    case MergeStatus::kMissingContract: return "missing-contract";
    case MergeStatus::kInvalidContract: return "invalid-contract";
    case MergeStatus::kTooManyCandidates: return "too-many-candidates";
  }
  return "invalid-contract";
}

MergeResult Merge(const std::vector<candidates::Record>& records,
                  const std::vector<std::uint32_t>& ordinals,
                  const std::vector<bool>& exact) {
  MergeResult result;
  if (records.size() != ordinals.size() || records.size() != exact.size()) {
    result.status = MergeStatus::kInvalidContract;
    result.reason = "input-vector-length-mismatch";
    return result;
  }
  if (records.empty()) return result;
  if (records.size() > kMaxMergeCandidates) {
    result.status = MergeStatus::kTooManyCandidates;
    result.reason = "input-above-menu-cap";
    return result;
  }

  // Fail-closed contract gate: every row must carry a complete, self-consistent
  // contract, and producer ordinals must be unique (they are the collection-
  // time truth the deterministic order below is built on).
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto& record = records[index];
    if (record.stable_id.empty() || record.identity.language_tag.empty() ||
        record.identity.scheme_id.empty()) {
      result.status = MergeStatus::kMissingContract;
      result.reason = "missing-required-contract-field";
      return result;
    }
    std::string reason;
    if (!candidates::Validate(record, &reason)) {
      result.status = MergeStatus::kInvalidContract;
      result.reason = std::move(reason);
      return result;
    }
    for (std::size_t prior = 0; prior < index; ++prior) {
      if (ordinals[prior] == ordinals[index]) {
        result.status = MergeStatus::kInvalidContract;
        result.reason = "duplicate-producer-ordinal";
        return result;
      }
    }
  }

  // Producer-rank sequence for suggestions: the rank derives from the
  // collection-time producer ordinal, never from vector position, so a
  // perturbed transport order cannot change any fusion points.
  std::vector<std::size_t> by_ordinal(records.size());
  for (std::size_t index = 0; index < records.size(); ++index) by_ordinal[index] = index;
  std::sort(by_ordinal.begin(), by_ordinal.end(),
            [&](std::size_t a, std::size_t b) { return ordinals[a] < ordinals[b]; });
  std::vector<std::uint32_t> suggestion_rank(records.size(), 0);
  std::uint32_t suggestion_count = 0;
  for (const auto index : by_ordinal) {
    if (records[index].source != candidates::Source::kRaw) {
      suggestion_rank[index] = suggestion_count++;
    }
  }

  // Score every row once. Raw rows are the hard tier; suggestion rows take
  // rank-only fusion points from the versioned policy.
  struct Row {
    double points = 0.0;
    bool raw = false;
  };
  std::vector<Row> rows(records.size());
  for (std::size_t index = 0; index < records.size(); ++index) {
    rows[index].raw = records[index].source == candidates::Source::kRaw;
    if (rows[index].raw) {
      rows[index].points = kFusionScoreMax;  // tier rule, not a comparable score
      continue;
    }
    const auto& source_policy = SourcePolicyFor(records[index].source);
    rows[index].points = NormalizeToPoints(source_policy, records[index].score,
                                           suggestion_rank[index], suggestion_count);
  }

  // Dedup key scope: language + scheme + tier class + policy text key. Language
  // and scheme are part of the candidate identity (the same surface in two
  // languages cannot collide), and a raw row never merges into a suggestion.
  // Cross-source duplicates of the same surface within one language merge into
  // one row that keeps the most reliable source and the full provenance.
  std::vector<std::string> keys(records.size());
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto& record = records[index];
    const auto text_key = DedupKey(record.identity.language_tag, record.source,
                                   record.text_utf8);
    if (text_key.empty()) {
      // Control-scalar or malformed text cannot pass the contract gate above
      // (Validate refuses it), so an empty key here is a defensive failure.
      result.status = MergeStatus::kInvalidContract;
      result.reason = "dedup-key-unavailable";
      return result;
    }
    keys[index] = record.identity.language_tag + UnitSeparator() +
                  record.identity.scheme_id + UnitSeparator() +
                  (rows[index].raw ? "raw" : "sugg") + UnitSeparator() + text_key;
  }

  // Group rows by key, first appearance order preserved.
  std::map<std::string, std::size_t> group_of_key;
  std::vector<std::vector<std::size_t>> groups;
  for (std::size_t index = 0; index < records.size(); ++index) {
    const auto found = group_of_key.find(keys[index]);
    if (found == group_of_key.end()) {
      group_of_key.emplace(keys[index], groups.size());
      groups.push_back({index});
    } else {
      groups[found->second].push_back(index);
      ++result.deduplicated;
    }
  }

  // Build one merged entry per group. Survivor: the most reliable source;
  // within a source, the backend's own merge precedence (higher score, then
  // lexeme IDs ascending, then lower producer ordinal). Group fusion points
  // come from the earliest row (rank-only points descend with rank, so the
  // maximum is the first collected row's points).
  for (const auto& group : groups) {
    std::size_t survivor = group.front();
    for (const auto index : group) {
      const auto& candidate = records[index];
      const auto& best = records[survivor];
      if (MoreReliable(candidate.source, best.source) ||
          (candidate.source == best.source &&
           (candidate.score > best.score ||
            (candidate.score == best.score &&
             (candidate.identity.lexeme_ids < best.identity.lexeme_ids ||
              (candidate.identity.lexeme_ids == best.identity.lexeme_ids &&
               ordinals[index] < ordinals[survivor])))))) {
        survivor = index;
      }
    }
    double group_points = rows[group.front()].points;
    for (const auto index : group) group_points = (std::max)(group_points, rows[index].points);

    MergedCandidate merged;
    merged.record = records[survivor];
    merged.is_raw_tier = rows[survivor].raw;
    merged.fusion_points = group_points;
    merged.exact_expansion = false;
    for (const auto index : group) merged.exact_expansion = merged.exact_expansion || exact[index];
    merged.provenance.push_back({records[survivor].stable_id, records[survivor].source,
                                 ordinals[survivor], rows[survivor].points,
                                 exact[survivor]});
    std::vector<std::size_t> folded;
    for (const auto index : group)
      if (index != survivor) folded.push_back(index);
    std::sort(folded.begin(), folded.end(),
              [&](std::size_t a, std::size_t b) { return ordinals[a] < ordinals[b]; });
    for (const auto index : folded) {
      merged.provenance.push_back({records[index].stable_id, records[index].source,
                                   ordinals[index], rows[index].points, exact[index]});
    }
    result.merged.push_back(std::move(merged));
  }

  // Deterministic total order: raw tier first (producer order), then exact
  // expansions (the backend's producer sort already leads with them; the rule
  // here is defense in depth), then points desc, then producer ordinal asc,
  // then stable id asc. The producer ordinal is the collection-time truth, so
  // the order cannot depend on the transport order of the input vectors.
  std::sort(result.merged.begin(), result.merged.end(),
            [](const MergedCandidate& a, const MergedCandidate& b) {
              if (a.is_raw_tier != b.is_raw_tier) return a.is_raw_tier;
              if (a.exact_expansion != b.exact_expansion) return a.exact_expansion;
              if (a.fusion_points != b.fusion_points) return a.fusion_points > b.fusion_points;
              const auto a_ordinal = a.provenance.front().producer_insertion_ordinal;
              const auto b_ordinal = b.provenance.front().producer_insertion_ordinal;
              if (a_ordinal != b_ordinal) return a_ordinal < b_ordinal;
              return a.record.stable_id < b.record.stable_id;
            });
  return result;
}

}  // namespace fusion
}  // namespace epocanva
