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

using LexemeId = std::uint64_t;
constexpr std::size_t kMaxWordRecords = 2048;  // aggregate records
constexpr std::size_t kMaxPairRecords = 2048;  // aggregate records
constexpr std::size_t kMaxCommitLexemes = 8;   // admitted dictionary words
constexpr std::size_t kMaxStoreBytes = 262144;
constexpr std::uint64_t kMaxLearningCount = 1000000000;

enum class PrivacyClass { kUnknown = 0, kNormal, kSensitive };
enum class Status {
  kOk = 0,
  kDenied,
  kStale,
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
};
struct Selection {
  std::uint64_t context_target = 0;
  std::uint64_t generation = 0;
  bool host_write_confirmed = false;
  LexemeId previous_id = 0;
  std::vector<LexemeId> lexeme_ids;
};
struct WordCount { LexemeId id = 0; std::uint64_t count = 0; };
struct PairCount {
  LexemeId previous_id = 0;
  LexemeId id = 0;
  std::uint64_t count = 0;
};
struct Snapshot {
  std::uint64_t revision = 0;
  std::vector<WordCount> words;
  std::vector<PairCount> pairs;
};

// Catalog contains only IDs resolved from admitted dictionary rows, never hashes
// of arbitrary input. IDs and aggregate counts remain personal data, not anonymised
// data. Namespace and catalog SHA-256 isolate language/schema/dictionary revisions.
struct Catalog {
  std::string name_space;
  std::string sha256;
  std::vector<LexemeId> admitted_ids;
};

// Not internally concurrent: persistence adapters must hold their transaction lock
// across load/record/serialize. Every read/write takes a fresh access decision.
// The caller holds the host permission read-lock while recording a confirmed edit.
// One previous dictionary ID is the entire context; no text, time or host identity
// is accepted or retained. Record once per confirmed host edit, not per key event.
class LearningStore {
 public:
  static std::unique_ptr<LearningStore> Create(Catalog catalog);
  const Catalog& catalog() const { return catalog_; }
  bool Admits(LexemeId id) const;
  Status Record(const Selection& selection, const Access& current);
  Status Read(const Access& current, Snapshot* out) const;
  Status Serialize(const Access& current, std::string* out) const;
  // A successful import atomically replaces this object. Version 1 has word
  // counts only; it is migrated in memory and serialized as version 2. Malformed
  // records and wrong catalog identity never partially replace live counts.
  Status Load(std::string_view bytes, const Access& current);
  // Explicit user action. Permission to delete is independent of focused fields.
  Status Clear(bool explicit_user_action);
  // Export is an explicit user action and contains the same bounded ID/count data.
  Status Export(bool explicit_user_action, std::string* out) const;

 private:
  explicit LearningStore(Catalog catalog);
  std::string Encode() const;
  Catalog catalog_;
  Snapshot snapshot_;
};

}  // namespace learning
}  // namespace epocanva
#endif
