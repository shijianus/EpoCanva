#include "learning_events.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace epocanva {
namespace ranking {
namespace {

double Saturate(double value, double limit) {
  if (!(value > 0.0)) return 0.0;  // also normalises NaN to zero
  return value > limit ? limit : value;
}

bool IsValidLanguage(std::string_view language) {
  if (language.size() < 2 || language.size() > 8) return false;
  for (char ch : language) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))) return false;
  }
  return true;
}

bool IsValidCandidateId(std::string_view id) {
  return !id.empty() && id.size() <= 128;
}

}  // namespace

const char* EventKindName(EventKind kind) {
  switch (kind) {
    case EventKind::kSelected: return "selected";
    case EventKind::kRejected: return "rejected";
    case EventKind::kSuperseded: return "superseded";
    case EventKind::kManualWordAdded: return "manual_word_added";
    case EventKind::kManualWordRemoved: return "manual_word_removed";
  }
  return "selected";
}

const char* EventStatusName(EventStatus status) {
  switch (status) {
    case EventStatus::kApplied: return "applied";
    case EventStatus::kIgnoredStale: return "ignored_stale";
    case EventStatus::kIgnoredUnattributable: return "ignored_unattributable";
    case EventStatus::kDeniedSensitive: return "denied_sensitive";
    case EventStatus::kDeniedUnknownPrivacy: return "denied_unknown_privacy";
    case EventStatus::kLearningDisabled: return "learning_disabled";
    case EventStatus::kInvalidArgument: return "invalid_argument";
    case EventStatus::kNotTracked: return "not_tracked";
  }
  return "invalid_argument";
}

LearningEventModel::LearningEventModel() = default;

void LearningEventModel::SetEnabled(bool enabled) { enabled_ = enabled; }

std::string LearningEventModel::Key(std::string_view language, std::string_view id) {
  std::string key;
  key.reserve(language.size() + 1 + id.size());
  key.append(language);
  key.push_back('\x1f');  // unit separator; cannot occur in either validated part
  key.append(id);
  return key;
}

void LearningEventModel::DecayEntry(Entry* entry, std::uint64_t now_ticks) const {
  if (now_ticks <= entry->preference.last_tick) return;
  const std::uint64_t delta = now_ticks - entry->preference.last_tick;
  const double years = static_cast<double>(delta) /
                       static_cast<double>(half_life_ticks_);
  const double factor = std::exp2(-years);
  // A manual word is a direct user instruction; decay applies to it as well so
  // stale preferences do not outlive their usefulness, but it starts at the
  // saturation bound.
  entry->preference.positive *= factor;
  entry->preference.negative *= factor;
  entry->preference.last_tick = now_ticks;
}

void LearningEventModel::AdvanceTo(std::uint64_t now_ticks) {
  if (now_ticks <= newest_tick_) return;
  for (auto& item : entries_) DecayEntry(&item.second, now_ticks);
  newest_tick_ = now_ticks;
}

void LearningEventModel::EvictIfNeeded() {
  if (entries_.size() < kMaxLearnedEntries) return;
  auto victim = entries_.end();
  for (auto it = entries_.begin(); it != entries_.end(); ++it) {
    if (victim == entries_.end()) {
      victim = it;
      continue;
    }
    // Manual entries are the user's explicit instruction; prefer evicting an
    // inferred entry. Among equals, evict the oldest update, then the smallest
    // ordinal so eviction is deterministic.
    const bool candidate_manual = it->second.preference.manual;
    const bool victim_manual = victim->second.preference.manual;
    if (victim_manual && !candidate_manual) {
      victim = it;
      continue;
    }
    if (candidate_manual && !victim_manual) continue;
    if (it->second.preference.last_tick < victim->second.preference.last_tick) {
      victim = it;
    } else if (it->second.preference.last_tick == victim->second.preference.last_tick &&
               it->second.key_ordinal < victim->second.key_ordinal) {
      victim = it;
    }
  }
  if (victim != entries_.end()) entries_.erase(victim);
}

bool LearningEventModel::SetPreference(std::string_view language,
                                       std::string_view candidate_id,
                                       const Preference& value) {
  if (!IsValidLanguage(language) || !IsValidCandidateId(candidate_id)) return false;
  if (!std::isfinite(value.positive) || !std::isfinite(value.negative)) return false;
  const std::string key = Key(language, candidate_id);
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    if (entries_.size() >= kMaxLearnedEntries) {
      auto victim = entries_.end();
      for (auto candidate = entries_.begin(); candidate != entries_.end(); ++candidate) {
        if (candidate->second.preference.manual) continue;
        if (victim == entries_.end() ||
            candidate->second.preference.last_tick <
                victim->second.preference.last_tick ||
            (candidate->second.preference.last_tick ==
                 victim->second.preference.last_tick &&
             candidate->second.key_ordinal < victim->second.key_ordinal)) {
          victim = candidate;
        }
      }
      if (victim == entries_.end()) return false;  // all entries are user-authored
      entries_.erase(victim);
    }
    Entry entry;
    entry.key_ordinal = next_key_ordinal_++;
    it = entries_.emplace(key, entry).first;
  }
  Preference& preference = it->second.preference;
  preference.positive = Saturate(value.positive, kMaxPersonalBoost);
  preference.negative = Saturate(value.negative, kMaxNegativePenalty);
  preference.last_tick = value.last_tick;
  preference.manual = value.manual;
  if (value.last_tick > newest_tick_) newest_tick_ = value.last_tick;
  return true;
}

EventStatus LearningEventModel::Apply(const LearningEvent& event) {  if (!IsValidLanguage(event.language) || !IsValidCandidateId(event.candidate_id)) {
    return EventStatus::kInvalidArgument;
  }
  if (event.privacy == PrivacyClass::kSensitive) return EventStatus::kDeniedSensitive;
  if (event.privacy != PrivacyClass::kNormal) {
    return EventStatus::kDeniedUnknownPrivacy;
  }
  if (!enabled_) return EventStatus::kLearningDisabled;

  const bool manual = event.kind == EventKind::kManualWordAdded ||
                      event.kind == EventKind::kManualWordRemoved;
  if (!manual) {
    if (!event.attributable) return EventStatus::kIgnoredUnattributable;
    if (event.generation != generation_) return EventStatus::kIgnoredStale;
  }

  AdvanceTo(event.now_ticks);

  const std::string key = Key(event.language, event.candidate_id);
  auto it = entries_.find(key);
  if (event.kind == EventKind::kManualWordRemoved) {
    if (it == entries_.end() || !it->second.preference.manual) {
      return EventStatus::kNotTracked;
    }
    entries_.erase(it);
    return EventStatus::kApplied;
  }

  if (it == entries_.end()) {
    EvictIfNeeded();
    Entry entry;
    entry.preference.last_tick = event.now_ticks;
    entry.key_ordinal = next_key_ordinal_++;
    it = entries_.emplace(key, entry).first;
  }

  Preference& preference = it->second.preference;
  preference.last_tick = event.now_ticks;
  switch (event.kind) {
    case EventKind::kSelected:
      preference.positive = Saturate(preference.positive + kSelectionGain, kMaxPersonalBoost);
      break;
    case EventKind::kRejected:
      preference.negative = Saturate(preference.negative + kRejectionGain, kMaxNegativePenalty);
      break;
    case EventKind::kSuperseded:
      preference.negative = Saturate(preference.negative + kSupersededGain, kMaxNegativePenalty);
      break;
    case EventKind::kManualWordAdded:
      preference.positive = kMaxPersonalBoost;
      preference.manual = true;
      break;
    case EventKind::kManualWordRemoved:
      break;  // handled above
  }
  return EventStatus::kApplied;
}

bool LearningEventModel::Lookup(std::string_view language, std::string_view candidate_id,
                                Preference* out) const {
  if (out == nullptr) return false;
  const auto it = entries_.find(Key(language, candidate_id));
  if (it == entries_.end()) return false;
  *out = it->second.preference;
  return true;
}

void LearningEventModel::Clear() {
  entries_.clear();
  next_key_ordinal_ = 0;
  newest_tick_ = 0;
}

}  // namespace ranking
}  // namespace epocanva
