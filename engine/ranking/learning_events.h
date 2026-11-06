// In-memory preference model driven by explicit, attributable learning events.
//
// Deliberate structural choices, not policy comments:
//   * There is no event kind for a key press, a text commit or a backspace. A
//     deletion cannot lower a candidate's score because the API cannot express
//     it. Callers that want to record a correction must prove attribution to a
//     candidate identity and edit range; unattributed corrections are ignored.
//   * Every candidate-scoped event carries a generation. An event produced for
//     an older candidate menu is rejected as stale instead of applied to the
//     current one.
//   * Sensitive and Unknown privacy classes are refused before any mutation.
//   * Positive and negative accumulations saturate at fixed bounds, so a noisy
//     input sequence cannot permanently bury a legitimate candidate.
//   * All state decays with a documented half-life; nothing is permanent except
//     an explicit user-added word.
//
// This layer holds no personal data on disk. Persistence is a separate concern
// and must not be added here by making this class write files.
//
// A "tick" is a caller-supplied monotonic counter in an arbitrary unit. The
// caller must keep the unit constant per model instance; ticks are never
// interpreted as wall-clock time inside this class.
#ifndef EPOCANVA_RANKING_LEARNING_EVENTS_H_
#define EPOCANVA_RANKING_LEARNING_EVENTS_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace epocanva {
namespace ranking {

constexpr std::size_t kMaxLearnedEntries = 512;
constexpr double kMaxPersonalBoost = 2.0;   // ranking points, additive
constexpr double kMaxNegativePenalty = 1.5; // ranking points, subtracted
constexpr double kSelectionGain = 0.6;
constexpr double kRejectionGain = 0.5;
constexpr double kSupersededGain = 0.15;
constexpr std::uint64_t kDefaultHalfLifeTicks = 1000;

// Mirrors the learning store's privacy classification. kUnknown is refused, not
// treated as normal: a caller that could not classify the field must not learn.
enum class PrivacyClass { kUnknown = 0, kNormal, kSensitive };

enum class EventKind {
  // User explicitly picked this candidate (click, hotkey, or explicit accept).
  kSelected = 0,
  // User explicitly rejected this suggestion.
  kRejected,
  // User picked a different candidate from the same menu. Weak negative.
  kSuperseded,
  // User added a personal word directly. Strong positive, never decayed away.
  kManualWordAdded,
  // User removed a personal word. Removes the entry; it is not a penalty.
  kManualWordRemoved,
};

const char* EventKindName(EventKind kind);

struct LearningEvent {
  EventKind kind = EventKind::kSelected;
  std::string language;      // primary language subtag, e.g. "en"
  std::string candidate_id;  // stable identity, never display text
  // Generation of the candidate menu the event refers to. Candidate-scoped
  // events with any other generation are ignored as stale.
  std::uint64_t generation = 0;
  std::uint64_t now_ticks = 0;
  // For candidate-scoped kinds: the caller proves the event belongs to this
  // candidate identity. Manual word events do not need it.
  bool attributable = false;
  PrivacyClass privacy = PrivacyClass::kUnknown;
};

enum class EventStatus {
  kApplied = 0,
  kIgnoredStale,
  kIgnoredUnattributable,
  kDeniedSensitive,
  kDeniedUnknownPrivacy,
  kLearningDisabled,
  kInvalidArgument,
  kNotTracked,
};

const char* EventStatusName(EventStatus status);

// Per-key aggregate. Values are ranking points, already saturated.
struct Preference {
  double positive = 0.0;  // [0, kMaxPersonalBoost]
  double negative = 0.0;  // [0, kMaxNegativePenalty]
  std::uint64_t last_tick = 0;
  bool manual = false;
};

class LearningEventModel {
 public:
  LearningEventModel();

  // Enabling is independent of existing state: disabling stops new writes but
  // does not erase what the user already taught the model.
  void SetEnabled(bool enabled);
  bool enabled() const { return enabled_; }

  // Advance every entry's decay to `now_ticks`. Called internally by Apply, so
  // callers normally do not need it. Ticks older than the newest one seen are a
  // no-op (the model never runs time backwards).
  void AdvanceTo(std::uint64_t now_ticks);

  EventStatus Apply(const LearningEvent& event);

  // Persistence adapter, not an event: install a previously stored summary without
  // counting a new selection, applying a gain or inventing attribution. Values are
  // clamped to the same saturation bounds as Apply; non-finite input is rejected.
  // Returns false for an invalid key or when the bounded table is full of manual
  // entries (a user-authored entry is never evicted to make room for imported data).
  bool SetPreference(std::string_view language, std::string_view candidate_id,
                     const Preference& value);

  // Returns false and leaves `out` untouched when nothing is known.
  bool Lookup(std::string_view language, std::string_view candidate_id,
              Preference* out) const;

  // Explicit user action: forget everything and return to default ordering.
  void Clear();

  std::size_t size() const { return entries_.size(); }
  std::uint64_t newest_tick() const { return newest_tick_; }
  std::uint64_t generation() const { return generation_; }
  // Bumped whenever the candidate menu changes; events must carry the current
  // value to be applied.
  void SetGeneration(std::uint64_t generation) { generation_ = generation; }

 private:
  struct Entry {
    Preference preference;
    std::uint64_t key_ordinal = 0;  // stable eviction tie-break
  };
  static std::string Key(std::string_view language, std::string_view candidate_id);
  void DecayEntry(Entry* entry, std::uint64_t now_ticks) const;
  void EvictIfNeeded();

  std::unordered_map<std::string, Entry> entries_;
  std::uint64_t next_key_ordinal_ = 0;
  std::uint64_t newest_tick_ = 0;
  std::uint64_t generation_ = 0;
  std::uint64_t half_life_ticks_ = kDefaultHalfLifeTicks;
  bool enabled_ = true;
};

}  // namespace ranking
}  // namespace epocanva
#endif  // EPOCANVA_RANKING_LEARNING_EVENTS_H_
