#include "learning_store.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace epocanva {
namespace learning {
namespace {

bool Allowed(const Access& access) {
  return access.privacy == PrivacyClass::kNormal && access.learning_enabled &&
         access.context_target != 0 && access.generation != 0;
}

bool ValidNamespace(std::string_view value) {
  if (value.empty() || value.size() > 64) return false;  // ASCII UTF-8 bytes
  for (const char byte : value) {
    if (!((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
          byte == '.' || byte == '_' || byte == '-')) return false;
  }
  return true;
}

bool ValidSha256(std::string_view value) {
  if (value.size() != 64) return false;
  return std::all_of(value.begin(), value.end(), [](const char byte) {
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
  });
}

bool ParseUnsigned(std::string_view bytes, std::uint64_t* out) {
  if (bytes.empty() || (bytes.size() > 1 && bytes.front() == '0')) return false;
  const auto parsed = std::from_chars(bytes.data(), bytes.data() + bytes.size(), *out);
  return parsed.ec == std::errc() && parsed.ptr == bytes.data() + bytes.size();
}

std::vector<std::string_view> Fields(std::string_view line) {
  std::vector<std::string_view> fields;
  std::size_t begin_utf8_byte = 0;
  for (;;) {
    const auto end_utf8_byte = line.find('\t', begin_utf8_byte);
    if (end_utf8_byte == std::string_view::npos) {
      fields.push_back(line.substr(begin_utf8_byte));
      return fields;
    }
    fields.push_back(line.substr(begin_utf8_byte, end_utf8_byte - begin_utf8_byte));
    begin_utf8_byte = end_utf8_byte + 1;
  }
}

void IncrementWord(std::vector<WordCount>* words, LexemeId id) {
  auto found = std::find_if(words->begin(), words->end(), [id](const WordCount& row) {
    return row.id == id;
  });
  if (found != words->end()) {
    if (found->count < kMaxLearningCount) ++found->count;
    return;
  }
  if (words->size() == kMaxWordRecords) {
    auto victim = std::min_element(words->begin(), words->end(),
        [](const WordCount& lhs, const WordCount& rhs) {
          return std::tie(lhs.count, lhs.id) < std::tie(rhs.count, rhs.id);
        });
    words->erase(victim);
  }
  words->push_back({id, 1});
}

void IncrementPair(std::vector<PairCount>* pairs, LexemeId previous_id, LexemeId id) {
  auto found = std::find_if(pairs->begin(), pairs->end(), [&](const PairCount& row) {
    return row.previous_id == previous_id && row.id == id;
  });
  if (found != pairs->end()) {
    if (found->count < kMaxLearningCount) ++found->count;
    return;
  }
  if (pairs->size() == kMaxPairRecords) {
    auto victim = std::min_element(pairs->begin(), pairs->end(),
        [](const PairCount& lhs, const PairCount& rhs) {
          return std::tie(lhs.count, lhs.previous_id, lhs.id) <
                 std::tie(rhs.count, rhs.previous_id, rhs.id);
        });
    pairs->erase(victim);
  }
  pairs->push_back({previous_id, id, 1});
}

}  // namespace

const char* StatusName(Status status) {
  switch (status) {
    case Status::kOk: return "ok";
    case Status::kDenied: return "denied";
    case Status::kStale: return "stale";
    case Status::kInvalidCatalog: return "invalid-catalog";
    case Status::kInvalidLexeme: return "invalid-lexeme";
    case Status::kInvalidFormat: return "invalid-format";
    case Status::kUnsupportedVersion: return "unsupported-version";
    case Status::kCatalogMismatch: return "catalog-mismatch";
    case Status::kLimitExceeded: return "limit-exceeded";
    case Status::kRevisionExhausted: return "revision-exhausted";
    case Status::kIoError: return "io-error";
    case Status::kBusy: return "busy";
  }
  return "invalid-status";
}

LearningStore::LearningStore(Catalog catalog) : catalog_(std::move(catalog)) {}

std::unique_ptr<LearningStore> LearningStore::Create(Catalog catalog) {
  if (!ValidNamespace(catalog.name_space) || !ValidSha256(catalog.sha256) ||
      catalog.admitted_ids.empty()) return nullptr;
  std::sort(catalog.admitted_ids.begin(), catalog.admitted_ids.end());
  if (catalog.admitted_ids.front() == 0 ||
      std::adjacent_find(catalog.admitted_ids.begin(), catalog.admitted_ids.end()) !=
          catalog.admitted_ids.end()) return nullptr;
  return std::unique_ptr<LearningStore>(new LearningStore(std::move(catalog)));
}

bool LearningStore::Admits(LexemeId id) const {
  return id != 0 && std::binary_search(catalog_.admitted_ids.begin(),
                                     catalog_.admitted_ids.end(), id);
}

Status LearningStore::Record(const Selection& selection, const Access& current) {
  if (!Allowed(current)) return Status::kDenied;
  if (!selection.host_write_confirmed ||
      selection.context_target != current.context_target ||
      selection.generation != current.generation) return Status::kStale;
  if (selection.lexeme_ids.empty() || selection.lexeme_ids.size() > kMaxCommitLexemes)
    return Status::kLimitExceeded;
  if ((selection.previous_id != 0 && !Admits(selection.previous_id)) ||
      !std::all_of(selection.lexeme_ids.begin(), selection.lexeme_ids.end(),
                   [this](LexemeId id) { return Admits(id); }))
    return Status::kInvalidLexeme;
  if (snapshot_.revision == std::numeric_limits<std::uint64_t>::max())
    return Status::kRevisionExhausted;
  Snapshot updated = snapshot_;
  LexemeId previous_id = selection.previous_id;
  for (const auto id : selection.lexeme_ids) {
    IncrementWord(&updated.words, id);
    if (previous_id != 0) IncrementPair(&updated.pairs, previous_id, id);
    previous_id = id;
  }
  ++updated.revision;
  snapshot_ = std::move(updated);
  return Status::kOk;
}

Status LearningStore::Read(const Access& current, Snapshot* out) const {
  if (out != nullptr) *out = {};
  if (!Allowed(current)) return Status::kDenied;
  if (out == nullptr) return Status::kInvalidFormat;
  *out = snapshot_;
  return Status::kOk;
}

std::string LearningStore::Encode() const {
  auto words = snapshot_.words;
  auto pairs = snapshot_.pairs;
  std::sort(words.begin(), words.end(), [](const WordCount& lhs, const WordCount& rhs) {
    return lhs.id < rhs.id;
  });
  std::sort(pairs.begin(), pairs.end(), [](const PairCount& lhs, const PairCount& rhs) {
    return std::tie(lhs.previous_id, lhs.id) < std::tie(rhs.previous_id, rhs.id);
  });
  std::string result = "epocanva-learning\t2\nnamespace\t" + catalog_.name_space +
      "\ncatalog-sha256\t" + catalog_.sha256 + "\nrevision\t" +
      std::to_string(snapshot_.revision) + "\n";
  for (const auto& row : words) {
    result += "w\t" + std::to_string(row.id) + "\t" + std::to_string(row.count) + "\n";
  }
  for (const auto& row : pairs) {
    result += "b\t" + std::to_string(row.previous_id) + "\t" +
        std::to_string(row.id) + "\t" + std::to_string(row.count) + "\n";
  }
  return result;
}

Status LearningStore::Serialize(const Access& current, std::string* out) const {
  if (out != nullptr) out->clear();
  if (!Allowed(current)) return Status::kDenied;
  if (out == nullptr) return Status::kInvalidFormat;
  *out = Encode();
  return Status::kOk;
}

Status LearningStore::Load(std::string_view bytes, const Access& current) {
  if (!Allowed(current)) return Status::kDenied;
  if (bytes.size() > kMaxStoreBytes) return Status::kLimitExceeded;
  if (bytes.empty() || bytes.back() != '\n' || bytes.find('\r') != bytes.npos ||
      bytes.find('\0') != bytes.npos) return Status::kInvalidFormat;
  std::vector<std::string_view> lines;
  std::size_t begin_utf8_byte = 0;
  while (begin_utf8_byte < bytes.size()) {
    const auto end_utf8_byte = bytes.find('\n', begin_utf8_byte);
    const auto line = bytes.substr(begin_utf8_byte, end_utf8_byte - begin_utf8_byte);
    if (line.empty()) return Status::kInvalidFormat;
    lines.push_back(line);
    if (lines.size() > kMaxWordRecords + kMaxPairRecords + 4)
      return Status::kLimitExceeded;
    begin_utf8_byte = end_utf8_byte + 1;
  }
  if (lines.size() < 4) return Status::kInvalidFormat;
  const auto header = Fields(lines[0]);
  if (header.size() != 2 || header[0] != "epocanva-learning")
    return Status::kInvalidFormat;
  const bool legacy = header[1] == "1";
  if (!legacy && header[1] != "2") return Status::kUnsupportedVersion;
  const auto name_space = Fields(lines[1]);
  const auto catalog = Fields(lines[2]);
  const auto revision = Fields(lines[3]);
  if (name_space.size() != 2 || name_space[0] != "namespace" ||
      catalog.size() != 2 || catalog[0] != "catalog-sha256" ||
      revision.size() != 2 || revision[0] != "revision")
    return Status::kInvalidFormat;
  if (name_space[1] != catalog_.name_space || catalog[1] != catalog_.sha256)
    return Status::kCatalogMismatch;
  Snapshot loaded;
  if (!ParseUnsigned(revision[1], &loaded.revision)) return Status::kInvalidFormat;
  std::set<LexemeId> word_ids;
  std::set<std::pair<LexemeId, LexemeId>> pair_ids;
  for (std::size_t record_ordinal = 4; record_ordinal < lines.size(); ++record_ordinal) {
    const auto fields = Fields(lines[record_ordinal]);
    if (fields.size() == 3 && fields[0] == "w") {
      WordCount row;
      if (!ParseUnsigned(fields[1], &row.id) || !ParseUnsigned(fields[2], &row.count) ||
          row.count == 0 || row.count > kMaxLearningCount ||
          !word_ids.insert(row.id).second) return Status::kInvalidFormat;
      if (!Admits(row.id)) return Status::kInvalidLexeme;
      loaded.words.push_back(row);
    } else if (!legacy && fields.size() == 4 && fields[0] == "b") {
      PairCount row;
      if (!ParseUnsigned(fields[1], &row.previous_id) || !ParseUnsigned(fields[2], &row.id) ||
          !ParseUnsigned(fields[3], &row.count) || row.count == 0 ||
          row.count > kMaxLearningCount ||
          !pair_ids.emplace(row.previous_id, row.id).second) return Status::kInvalidFormat;
      if (!Admits(row.previous_id) || !Admits(row.id)) return Status::kInvalidLexeme;
      loaded.pairs.push_back(row);
    } else {
      return Status::kInvalidFormat;
    }
    if (loaded.words.size() > kMaxWordRecords || loaded.pairs.size() > kMaxPairRecords)
      return Status::kLimitExceeded;
  }
  snapshot_ = std::move(loaded);
  return Status::kOk;
}

Status LearningStore::Clear(bool explicit_user_action) {
  if (!explicit_user_action) return Status::kDenied;
  if (snapshot_.revision == std::numeric_limits<std::uint64_t>::max())
    return Status::kRevisionExhausted;
  const auto revision = snapshot_.revision + 1;
  snapshot_ = {};
  snapshot_.revision = revision;
  return Status::kOk;
}

Status LearningStore::Export(bool explicit_user_action, std::string* out) const {
  if (out != nullptr) out->clear();
  if (!explicit_user_action) return Status::kDenied;
  if (out == nullptr) return Status::kInvalidFormat;
  *out = Encode();
  return Status::kOk;
}

}  // namespace learning
}  // namespace epocanva
