#include "adaptive_ranker.h"

#include <algorithm>
#include <cmath>

namespace epocanva {
namespace ranking {
namespace {

bool IsValidPrimaryLanguage(std::string_view language) {
  if (language.size() < 2 || language.size() > 8) return false;
  for (char ch : language) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))) return false;
  }
  return true;
}

char LowerAscii(char ch) {
  return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

// Strict UTF-8 decoding, one scalar at a time. Invalid input is rejected by the
// caller; nothing is repaired or replaced, because a repaired prefix would change
// which candidate looks like the best match.
bool NextScalar(std::string_view text, std::size_t* offset, std::uint32_t* scalar) {
  if (*offset >= text.size()) return false;
  const unsigned char lead = static_cast<unsigned char>(text[*offset]);
  std::size_t length = 0;
  std::uint32_t value = 0;
  if (lead < 0x80) {
    value = lead;
    length = 1;
  } else if (lead >= 0xc2 && lead <= 0xdf) {
    value = lead & 0x1f;
    length = 2;
  } else if (lead >= 0xe0 && lead <= 0xef) {
    value = lead & 0x0f;
    length = 3;
  } else if (lead >= 0xf0 && lead <= 0xf4) {
    value = lead & 0x07;
    length = 4;
  } else {
    return false;
  }
  if (*offset + length > text.size()) return false;
  for (std::size_t i = 1; i < length; ++i) {
    const unsigned char byte = static_cast<unsigned char>(text[*offset + i]);
    if ((byte & 0xc0) != 0x80) return false;
    value = (value << 6) | (byte & 0x3f);
  }
  if ((length == 2 && value < 0x80) || (length == 3 && value < 0x800) ||
      (length == 4 && value < 0x10000) || value > 0x10ffff ||
      (value >= 0xd800 && value <= 0xdfff)) {
    return false;
  }
  *scalar = value;
  *offset += length;
  return true;
}

bool IsValidUtf8(std::string_view text) {
  std::size_t offset = 0;
  std::uint32_t scalar = 0;
  while (offset < text.size()) {
    if (!NextScalar(text, &offset, &scalar)) return false;
  }
  return true;
}

std::uint32_t FoldAscii(std::uint32_t scalar) {
  if (scalar < 0x80) return static_cast<std::uint32_t>(LowerAscii(static_cast<char>(scalar)));
  return scalar;  // No Unicode case folding exists in this layer.
}

double Clamp(double value, double low, double high) {
  if (!(value == value)) return low;  // NaN cannot participate in an order
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

double SpellingPenalty(const Proposal& proposal) {
  if (proposal.source != SourceKind::kSpelling) return 0.0;
  const double distance = static_cast<double>(std::min(proposal.edit_distance, 3u));
  return -distance / 3.0;  // [-1, 0]
}

}  // namespace

const char* SourceKindName(SourceKind kind) {
  switch (kind) {
    case SourceKind::kOriginal: return "original";
    case SourceKind::kExact: return "exact";
    case SourceKind::kCompletion: return "completion";
    case SourceKind::kSpelling: return "spelling";
    case SourceKind::kMorphology: return "morphology";
    case SourceKind::kAbbreviation: return "abbreviation";
    case SourceKind::kConversion: return "conversion";
    case SourceKind::kPrediction: return "prediction";
  }
  return "completion";
}

const char* RankStatusName(RankStatus status) {
  switch (status) {
    case RankStatus::kOk: return "ok";
    case RankStatus::kTooManyProposals: return "too_many_proposals";
    case RankStatus::kInputTooLong: return "input_too_long";
    case RankStatus::kInvalidInput: return "invalid_input";
    case RankStatus::kInvalidUtf8: return "invalid_utf8";
    case RankStatus::kInvalidWeights: return "invalid_weights";
  }
  return "invalid_input";
}

Weights DefaultWeights(std::string_view language) {
  Weights weights;
  if (language == "zh" || language == "ja" || language == "ko") {
    // Conversion output shares few code points with the typed key, so the
    // character-prefix feature carries little information; context dominates.
    weights.prefix = 0.5;
    weights.lexical = 1.0;
    weights.context = 2.0;
  } else if (language == "fr" || language == "es" || language == "de") {
    // Accented forms and morphology mean an exact prefix is rarer.
    weights.prefix = 2.5;
    weights.context = 1.25;
  }
  return weights;
}

bool IsValidWeights(const Weights& weights) {
  const double values[] = {weights.prefix,   weights.lexical,  weights.context,
                           weights.personal, weights.negative, weights.source,
                           weights.language};
  for (double value : values) {
    if (!std::isfinite(value)) return false;
    if (value < kMinWeight || value > kMaxWeight) return false;
  }
  return true;
}

bool AdaptiveRanker::PrefixQuality(std::string_view key_utf8, std::string_view text_utf8,
                                   double* out_quality, bool* out_exact) {
  if (out_quality == nullptr || out_exact == nullptr) return false;

  // Count key scalars first so the denominator is a scalar count, not a byte
  // count. A multibyte key must not be undervalued.
  std::size_t key_scalars = 0;
  std::size_t key_offset = 0;
  std::uint32_t scalar = 0;
  while (key_offset < key_utf8.size()) {
    if (!NextScalar(key_utf8, &key_offset, &scalar)) return false;
    ++key_scalars;
  }
  std::size_t text_offset = 0;
  std::size_t text_scalars = 0;
  while (text_offset < text_utf8.size()) {
    if (!NextScalar(text_utf8, &text_offset, &scalar)) return false;
    ++text_scalars;
  }

  std::size_t shared = 0;
  std::size_t key_read = 0;
  std::size_t text_read = 0;
  while (key_read < key_utf8.size() && text_read < text_utf8.size()) {
    std::uint32_t key_scalar = 0;
    std::uint32_t text_scalar = 0;
    const std::size_t key_pos = key_read;
    const std::size_t text_pos = text_read;
    if (!NextScalar(key_utf8, &key_read, &key_scalar)) return false;
    if (!NextScalar(text_utf8, &text_read, &text_scalar)) return false;
    if (FoldAscii(key_scalar) != FoldAscii(text_scalar)) {
      key_read = key_pos;
      text_read = text_pos;
      break;
    }
    ++shared;
  }

  *out_exact = key_scalars > 0 && key_scalars == text_scalars && shared == key_scalars;
  *out_quality = key_scalars == 0 ? 0.0
                                  : static_cast<double>(shared) /
                                        static_cast<double>(key_scalars);
  return true;
}

AdaptiveRanker::AdaptiveRanker(std::uint32_t detection_confirmations)
    : confirmations_(detection_confirmations == 0 ? 1 : detection_confirmations) {}

void AdaptiveRanker::ObserveDetection(std::string_view language) {
  if (language == latched_) {
    pending_.clear();
    streak_ = 0;
    return;
  }
  if (language == pending_) {
    ++streak_;
  } else {
    pending_.assign(language);
    streak_ = 1;
  }
  if (streak_ >= confirmations_) {
    latched_.assign(language);
    pending_.clear();
    streak_ = 0;
  }
}

void AdaptiveRanker::ResetDetection() {
  pending_.clear();
  latched_.clear();
  streak_ = 0;
}

RankResult AdaptiveRanker::Rank(const RankRequest& request,
                                const LearningEventModel* learning) const {
  RankResult result;
  if (request.proposals.size() > kMaxProposals) {
    result.status = RankStatus::kTooManyProposals;
    return result;
  }
  if (request.input_key_utf8.size() > kMaxInputKeyUtf8Bytes) {
    result.status = RankStatus::kInputTooLong;
    return result;
  }
  if (!IsValidPrimaryLanguage(request.user_language)) {
    result.status = RankStatus::kInvalidInput;
    return result;
  }
  if (!request.detected_language.empty() &&
      !IsValidPrimaryLanguage(request.detected_language)) {
    result.status = RankStatus::kInvalidInput;
    return result;
  }
  if (!IsValidWeights(request.weights)) {
    result.status = RankStatus::kInvalidWeights;
    return result;
  }
  if (!IsValidUtf8(request.input_key_utf8)) {
    result.status = RankStatus::kInvalidUtf8;
    return result;
  }

  // Effective detected language is the latched value, never the raw observation.
  const std::string& detected = latched_;

  struct Scored {
    std::size_t ordinal = 0;
    double score = 0.0;
    std::uint32_t tier = 1;
  };
  std::vector<Scored> scored;
  scored.reserve(request.proposals.size());

  for (std::size_t ordinal = 0; ordinal < request.proposals.size(); ++ordinal) {
    const Proposal& proposal = request.proposals[ordinal];
    if (!IsValidPrimaryLanguage(proposal.language) || proposal.text_utf8.empty() ||
        proposal.text_utf8.size() > kMaxDisplayTextUtf8Bytes) {
      result.status = RankStatus::kInvalidInput;
      result.ordered.clear();
      return result;
    }
    if (!IsValidUtf8(proposal.text_utf8)) {
      result.status = RankStatus::kInvalidUtf8;
      result.ordered.clear();
      return result;
    }

    const bool in_user_language = proposal.language == request.user_language;
    const bool in_detected_language = !detected.empty() && proposal.language == detected;
    if (!in_user_language && !in_detected_language) {
      ++result.dropped_foreign_language;
      continue;
    }

    double prefix_quality = 0.0;
    bool exact = false;
    if (!PrefixQuality(request.input_key_utf8, proposal.text_utf8, &prefix_quality,
                       &exact)) {
      result.status = RankStatus::kInvalidUtf8;
      result.ordered.clear();
      return result;
    }

    // language_fit is 1 for the user's language and 0 for the latched
    // auto-detected alternative, so the term is 0 or -weights.language. The
    // selected language therefore always precedes a guessed one at equal score.
    const double language_fit = in_user_language ? 1.0 : 0.0;

    double personal = 0.0;
    double negative = 0.0;
    if (request.learning_enabled && learning != nullptr &&
        !proposal.candidate_id.empty()) {
      Preference preference;
      if (learning->Lookup(proposal.language, proposal.candidate_id, &preference)) {
        personal = Clamp(preference.positive, 0.0, kMaxPersonalBoost);
        negative = Clamp(preference.negative, 0.0, kMaxNegativePenalty);
      }
    }

    const double context = Clamp(proposal.context_logp, kContextLogProbabilityFloor, 0.0);
    const double lexical = Clamp(proposal.static_weight, 0.0, 1.0);

    const double score =
        request.weights.prefix * prefix_quality +
        request.weights.lexical * lexical + request.weights.context * context +
        request.weights.personal * personal - request.weights.negative * negative +
        request.weights.source * SpellingPenalty(proposal) +
        request.weights.language * (language_fit - 1.0);

    // Tier 0 is the user's own text and is ordered ahead of every suggestion by
    // the comparator below, independently of score. This is the ranking-layer
    // expression of the product rule that original input is never displaced.
    scored.push_back({ordinal, score, exact ? 0u : 1u});
  }

  // Deduplicate identical display text, keeping the best score. The survivor
  // keeps its own proposal ordinal, which is the deterministic tie-break below.
  std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
    if (a.tier != b.tier) return a.tier < b.tier;
    if (a.score != b.score) return a.score > b.score;
    return a.ordinal < b.ordinal;
  });
  std::vector<std::string_view> seen;
  seen.reserve(scored.size());
  for (const Scored& item : scored) {
    const std::string& text = request.proposals[item.ordinal].text_utf8;
    bool duplicate = false;
    for (const std::string_view prior : seen) {
      if (prior == text) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      ++result.deduplicated;
      continue;
    }
    seen.push_back(text);
    result.ordered.push_back({item.ordinal, item.score, item.tier});
  }
  return result;
}

}  // namespace ranking
}  // namespace epocanva
