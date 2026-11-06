#ifndef EPOCANVA_DATA_STORE_LEARNING_STORE_H_
#define EPOCANVA_DATA_STORE_LEARNING_STORE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace epocanva {
namespace learning {

// A lexeme ID is a stable ID resolved from an admitted dictionary/model row. It is not a
// hash of arbitrary user input and it is not a candidate-menu token (those are transient).
using LexemeId = std::uint64_t;

constexpr std::size_t kMaxWordRecords = 2048;      // aggregate word records
constexpr std::size_t kMaxPairRecords = 2048;      // aggregate context-pair records
constexpr std::size_t kMaxPreferenceRecords = 448; // candidate preference records
constexpr std::size_t kMaxCommitLexemes = 8;       // admitted dictionary words per commit
constexpr std::size_t kMaxStoreBytes = 262144;
constexpr std::uint64_t kMaxLearningCount = 1000000000;

// Preference values are integers in thousandths of a ranking point ("milli-points").
// This keeps persistence deterministic and avoids floating-point formatting in the file.
constexpr std::uint64_t kPreferenceUnit = 1000;
constexpr std::uint64_t kSelectionGainUnits = 600;   // +0.6 ranking points
constexpr std::uint64_t kRejectionGainUnits = 500;   // +0.5 negative points
constexpr std::uint64_t kSupersededGainUnits = 150;  // +0.15 weak negative points
constexpr std::uint64_t kMaxPositiveUnits = 2000;    // +2.0 ranking points
constexpr std::uint64_t kMaxNegativeUnits = 1500;    // -1.5 ranking points

// Decay unit: one tick is one millisecond of UTC wall-clock time since the Unix epoch.
// The store never moves a record's timestamp backwards. A caller that supplies an older
// tick (system clock rollback) causes a no-op decay; the next positive tick decays from
// the preserved timestamp. Deltas beyond kMaxDecayDeltaMilliseconds are clamped so a
// pathological clock jump cannot overflow arithmetic.
constexpr std::uint64_t kDefaultHalfLifeMilliseconds = 30ull * 24 * 60 * 60 * 1000;
constexpr std::uint64_t kMaxHalfLifeMilliseconds = 10ull * 365 * 24 * 60 * 60 * 1000;
constexpr std::uint64_t kMaxDecayDeltaMilliseconds = 100ull * 365 * 24 * 60 * 60 * 1000;

enum class PrivacyClass { kUnknown = 0, kNormal, kSensitive };
enum class Status {
  kOk = 0,
  kDenied,
  kStale,
  kUnattributable,
  kNotFound,
  kInvalidCatalog,
  kInvalidLexeme,
  kInvalidFormat,
  kUnsupportedVersion,
  kCatalogMismatch,
  kLimitExceeded,
  kRevisionExhausted,
  kIoError,
  kBusy,
};
const char* StatusName(Status status);

struct Access {
  PrivacyClass privacy = PrivacyClass::kUnknown;
  bool learning_enabled = false;
  std::uint64_t context_target = 0;
  std::uint64_t generation = 0;
  // Caller-supplied UTC milliseconds since the Unix epoch. 0 means "do not advance decay".
  // The value is never interpreted as a host identifier or a text offset.
  std::uint64_t now_tick_ms = 0;
};

// One confirmed selection. candidate_id is a stable logical identity, not display text
// and not a candidate-menu token. When empty, it is derived from lexeme_ids as
// "lex:<id>[+<id>...]". normalized_identity is an optional normalized lexical surface
// (UTF-8); it is personal data when present and is never a keystroke stream or context.
struct Selection {
  std::uint64_t context_target = 0;
  std::uint64_t generation = 0;
  bool host_write_confirmed = false;
  LexemeId previous_id = 0;
  std::vector<LexemeId> lexeme_ids;
  std::string candidate_id;
  std::string normalized_identity;
};

struct WordCount { LexemeId id = 0; std::uint64_t count = 0; };
struct PairCount {
  LexemeId previous_id = 0;
  LexemeId id = 0;
  std::uint64_t count = 0;
};

// Candidate-scoped positive/negative summary. Both values are milli-ranking-points in
// [0, kMaxPositiveUnits] / [0, kMaxNegativeUnits]. Snapshot copy returned to callers is
// already decayed to Access::now_tick_ms; the durable copy keeps the raw value and its
// last timestamp.
struct PreferenceRecord {
  std::string candidate_id;
  std::string normalized_identity;
  std::vector<LexemeId> lexeme_ids;
  std::uint64_t positive_units = 0;
  std::uint64_t negative_units = 0;
  std::uint64_t last_tick_ms = 0;
  bool manual = false;
};

struct Snapshot {
  std::uint64_t revision = 0;
  std::vector<WordCount> words;
  std::vector<PairCount> pairs;
  std::vector<PreferenceRecord> preferences;
};

enum class FeedbackKind {
  kSelected = 0,
  kRejected,
  kSuperseded,
  kManualWordAdded,
  kManualWordRemoved,
};

const char* FeedbackKindName(FeedbackKind kind);

// Catalog contains only IDs resolved from admitted dictionary rows, never hashes of
// arbitrary input. IDs and aggregate counts remain personal data, not anonymised data.
// name_space and sha256 isolate language/profile/dictionary revisions. profile_id is a
// stable product profile identity (for example "3"), while language is the short primary
// BCP 47 subtag ("en"); a full BCP 47 tag is never stored in the language field.
struct Catalog {
  std::string name_space;
  std::string sha256;
  std::vector<LexemeId> admitted_ids;
  std::string profile_id = "legacy";
  std::string language = "en";
  std::uint64_t half_life_ms = kDefaultHalfLifeMilliseconds;
};

// True when name_space/sha256/profile/language/half-life form a usable identity. Does not
// require admitted_ids; explicit deletion and corruption cleanup only need the identity.
bool ValidCatalogIdentity(const Catalog& catalog);

// Derives a stable candidate key from admitted lexeme IDs: "lex:<id>[+<id>...]".
// Returns an empty string for an empty, oversized, zero or duplicate ID list.
std::string CandidateKey(const std::vector<LexemeId>& ids);

// Whole-millisecond UTC wall clock, or 0 when the clock cannot be represented safely.
std::uint64_t CurrentTickMilliseconds();

// Recognises the serialized schema version without mutating a store. Returns false when
// the header is not an EpoCanva learning store.
bool SerializedFormatVersion(std::string_view bytes, std::uint32_t* out_version);

// Not internally concurrent: persistence adapters must hold their transaction lock across
// load/record/serialize. Every write takes a fresh access decision. Reads require a
// Normal field; writes additionally require learning_enabled. Pausing recording therefore
// stops new writes but does not hide preferences the user already taught the model.
// One previous dictionary ID is the entire context; no text, host identity or field name
// is accepted or retained. Record once per confirmed host edit, not per key event.
class LearningStore {
 public:
  static std::unique_ptr<LearningStore> Create(Catalog catalog);
  const Catalog& catalog() const { return catalog_; }
  bool Admits(LexemeId id) const;
  Status Record(const Selection& selection, const Access& current);
  // Rejected/superseded feedback is weak negative evidence and requires an attributable
  // candidate menu generation. Manual add/remove require explicit_user_action. Manual
  // removal is allowed while recording is paused: deleting a user-authored entry is a
  // user action, not new learning.
  Status RecordFeedback(FeedbackKind kind, std::string_view candidate_id,
                        std::string_view normalized_identity,
                        const std::vector<LexemeId>& lexeme_ids, bool attributable,
                        std::uint64_t generation, const Access& current,
                        bool explicit_user_action = false);
  Status Read(const Access& current, Snapshot* out) const;
  Status Serialize(const Access& current, std::string* out) const;
  // A successful Load atomically replaces this object. Versions 1 and 2 are migrated in
  // memory and serialized as version 3 on the next write. Malformed records, wrong catalog
  // identity and out-of-range values never partially replace live counts.
  Status Load(std::string_view bytes, const Access& current);
  // Replaces the in-memory aggregates with a previously read snapshot. Used by the
  // backend session to import one repository read without a second serialization format.
  Status Import(const Snapshot& snapshot, const Access& current);
  // Explicit user action. Permission to delete is independent of focused fields. Clears
  // aggregates, preferences and the decay metadata; revision advances.
  Status Clear(bool explicit_user_action);
  // Export is an explicit user action and contains the same bounded data.
  Status Export(bool explicit_user_action, std::string* out) const;

  // Advances every preference to now_tick_ms. Ticks older than a record's timestamp are a
  // no-op (time never runs backwards). Called by Record/RecordFeedback; exposed for tests
  // and for adapters that persist a decayed view.
  void AdvanceTo(std::uint64_t now_tick_ms);

 private:
  explicit LearningStore(Catalog catalog);
  std::string Encode() const;
  Catalog catalog_;
  Snapshot snapshot_;
};

}  // namespace learning
}  // namespace epocanva
#endif
