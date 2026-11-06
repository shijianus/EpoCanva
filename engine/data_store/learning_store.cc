#include "learning_store.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace epocanva {
namespace learning {
namespace {

constexpr std::size_t kMaxCandidateIdBytes = 192;
constexpr std::size_t kMaxIdentityBytes = 96;

bool ReadAllowed(const Access& access) {
  return access.privacy == PrivacyClass::kNormal && access.context_target != 0 &&
         access.generation != 0;
}

bool WriteAllowed(const Access& access) {
  return ReadAllowed(access) && access.learning_enabled;
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

bool ValidProfileId(std::string_view value) {
  if (value.empty() || value.size() > 64) return false;
  return std::all_of(value.begin(), value.end(), [](const char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
           (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' ||
           byte == ':' || byte == '-';
  });
}

bool ValidLanguage(std::string_view value) {
  if (value.size() < 2 || value.size() > 8) return false;
  return std::all_of(value.begin(), value.end(), [](const char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9');
  });
}

bool ValidHalfLife(std::uint64_t value) {
  return value > 0 && value <= kMaxHalfLifeMilliseconds;
}

bool IsValidUtf8(std::string_view text) {
  for (std::size_t offset = 0; offset < text.size();) {
    const auto lead = static_cast<unsigned char>(text[offset++]);
    std::uint32_t scalar = lead;
    unsigned remaining = 0;
    if (lead == 0 || lead < 0x20 || lead == 0x7f) return false;
    if (lead >= 0xc2 && lead <= 0xdf) { remaining = 1; scalar &= 0x1f; }
    else if (lead >= 0xe0 && lead <= 0xef) { remaining = 2; scalar &= 0x0f; }
    else if (lead >= 0xf0 && lead <= 0xf4) { remaining = 3; scalar &= 0x07; }
    else if (lead >= 0x80) return false;
    if (remaining > text.size() - offset) return false;
    const auto count = remaining;
    while (remaining--) {
      const auto byte = static_cast<unsigned char>(text[offset++]);
      if ((byte & 0xc0) != 0x80) return false;
      scalar = (scalar << 6) | (byte & 0x3f);
    }
    if ((count == 1 && scalar < 0x80) || (count == 2 && scalar < 0x800) ||
        (count == 3 && scalar < 0x10000) || scalar > 0x10ffff ||
        (scalar >= 0xd800 && scalar <= 0xdfff) ||
        (scalar >= 0x80 && scalar <= 0x9f)) return false;
  }
  return true;
}

bool ValidCandidateId(std::string_view value) {
  if (value.empty() || value.size() > kMaxCandidateIdBytes) return false;
  return std::all_of(value.begin(), value.end(), [](const char byte) {
    return byte >= 0x21 && byte <= 0x7e;
  });
}

bool ValidIdentity(std::string_view value) {
  if (value.size() > kMaxIdentityBytes) return false;
  if (value.empty()) return true;
  if (!IsValidUtf8(value)) return false;
  return value.find('\t') == value.npos && value.find('\n') == value.npos &&
         value.find('\r') == value.npos;
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

bool DecodeIds(std::string_view field, std::vector<LexemeId>* out) {
  out->clear();
  if (field.empty()) return true;
  std::size_t begin = 0;
  std::set<LexemeId> seen;
  for (;;) {
    const auto end = field.find(',', begin);
    const auto piece = field.substr(begin, end == std::string_view::npos
                                               ? std::string_view::npos
                                               : end - begin);
    LexemeId id = 0;
    if (!ParseUnsigned(piece, &id) || id == 0 || !seen.insert(id).second ||
        out->size() == kMaxCommitLexemes) return false;
    out->push_back(id);
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return true;
}

std::string EncodeIds(const std::vector<LexemeId>& ids) {
  std::string result;
  for (std::size_t index = 0; index < ids.size(); ++index) {
    if (index != 0) result.push_back(',');
    result += std::to_string(ids[index]);
  }
  return result;
}

std::uint64_t Saturate(std::uint64_t value, std::uint64_t limit) {
  return value > limit ? limit : value;
}

std::uint64_t DecayedUnits(std::uint64_t units, std::uint64_t last_tick_ms,
                           std::uint64_t now_tick_ms, std::uint64_t half_life_ms) {
  if (units == 0 || now_tick_ms == 0 || last_tick_ms == 0 ||
      now_tick_ms <= last_tick_ms) return units;
  std::uint64_t delta = now_tick_ms - last_tick_ms;
  if (delta > kMaxDecayDeltaMilliseconds) delta = kMaxDecayDeltaMilliseconds;
  const double factor = std::exp2(-static_cast<double>(delta) /
                                  static_cast<double>(half_life_ms));
  const double value = static_cast<double>(units) * factor;
  if (!(value > 0.0)) return 0;
  const long long rounded = std::llround(value);
  if (rounded <= 0) return 0;
  return static_cast<std::uint64_t>(rounded) > units ? units
                                                     : static_cast<std::uint64_t>(rounded);
}

void AdvanceSnapshot(Snapshot* snapshot, std::uint64_t now_tick_ms,
                     std::uint64_t half_life_ms) {
  if (now_tick_ms == 0) return;
  for (auto& row : snapshot->preferences) {
    if (row.last_tick_ms == 0) {
      // Version 1/2 migration has no timestamp. Establish the baseline now rather than
      // decaying from the Unix epoch, which would erase every migrated preference.
      row.last_tick_ms = now_tick_ms;
      continue;
    }
    if (now_tick_ms <= row.last_tick_ms) continue;  // clock rollback is a no-op
    row.positive_units = DecayedUnits(row.positive_units, row.last_tick_ms, now_tick_ms,
                                      half_life_ms);
    row.negative_units = DecayedUnits(row.negative_units, row.last_tick_ms, now_tick_ms,
                                      half_life_ms);
    row.last_tick_ms = now_tick_ms;
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

PreferenceRecord* FindPreference(Snapshot* snapshot, std::string_view candidate_id) {
  auto found = std::find_if(snapshot->preferences.begin(), snapshot->preferences.end(),
      [&](const PreferenceRecord& row) { return row.candidate_id == candidate_id; });
  return found == snapshot->preferences.end() ? nullptr : &*found;
}

bool MakeRoomForPreference(Snapshot* snapshot, std::uint64_t now_tick_ms,
                           std::uint64_t half_life_ms) {
  if (snapshot->preferences.size() < kMaxPreferenceRecords) return true;
  auto victim = snapshot->preferences.end();
  for (auto it = snapshot->preferences.begin(); it != snapshot->preferences.end(); ++it) {
    if (it->manual) continue;  // a user-authored entry is never evicted for an inferred one
    if (victim == snapshot->preferences.end()) {
      victim = it;
      continue;
    }
    const auto effective = [&](const PreferenceRecord& row) {
      const auto positive = DecayedUnits(row.positive_units, row.last_tick_ms,
                                         now_tick_ms, half_life_ms);
      const auto negative = DecayedUnits(row.negative_units, row.last_tick_ms,
                                         now_tick_ms, half_life_ms);
      return static_cast<long long>(positive) - static_cast<long long>(negative);
    };
    const auto candidate_value = effective(*it);
    const auto victim_value = effective(*victim);
    if (candidate_value < victim_value ||
        (candidate_value == victim_value && it->last_tick_ms < victim->last_tick_ms) ||
        (candidate_value == victim_value && it->last_tick_ms == victim->last_tick_ms &&
         it->candidate_id < victim->candidate_id)) {
      victim = it;
    }
  }
  if (victim == snapshot->preferences.end()) return false;  // all entries are manual
  snapshot->preferences.erase(victim);
  return true;
}

Status ApplySelectionTo(Snapshot* snapshot, const Selection& selection,
                        const Access& current, const Catalog& catalog,
                        bool require_host_confirmation) {
  if (require_host_confirmation && !selection.host_write_confirmed) return Status::kStale;
  if (selection.context_target != current.context_target ||
      selection.generation != current.generation) return Status::kStale;
  if (selection.lexeme_ids.empty() ||
      selection.lexeme_ids.size() > kMaxCommitLexemes) return Status::kLimitExceeded;
  if ((selection.previous_id != 0 &&
       !std::binary_search(catalog.admitted_ids.begin(), catalog.admitted_ids.end(),
                           selection.previous_id)) ||
      !std::all_of(selection.lexeme_ids.begin(), selection.lexeme_ids.end(),
          [&](LexemeId id) {
            return id != 0 && std::binary_search(catalog.admitted_ids.begin(),
                                                 catalog.admitted_ids.end(), id);
          })) return Status::kInvalidLexeme;
  const std::string candidate_id = selection.candidate_id.empty()
      ? CandidateKey(selection.lexeme_ids) : selection.candidate_id;
  if (!ValidCandidateId(candidate_id) ||
      !ValidIdentity(selection.normalized_identity)) return Status::kInvalidFormat;
  if (snapshot->revision == std::numeric_limits<std::uint64_t>::max())
    return Status::kRevisionExhausted;

  AdvanceSnapshot(snapshot, current.now_tick_ms, catalog.half_life_ms);
  LexemeId previous_id = selection.previous_id;
  for (const auto id : selection.lexeme_ids) {
    IncrementWord(&snapshot->words, id);
    if (previous_id != 0) IncrementPair(&snapshot->pairs, previous_id, id);
    previous_id = id;
  }

  PreferenceRecord* preference = FindPreference(snapshot, candidate_id);
  if (preference == nullptr) {
    if (!MakeRoomForPreference(snapshot, current.now_tick_ms, catalog.half_life_ms))
      return Status::kLimitExceeded;
    PreferenceRecord row;
    row.candidate_id = candidate_id;
    row.normalized_identity = selection.normalized_identity;
    row.lexeme_ids = selection.lexeme_ids;
    row.last_tick_ms = current.now_tick_ms;
    snapshot->preferences.push_back(std::move(row));
    preference = &snapshot->preferences.back();
  }
  preference->positive_units = Saturate(
      preference->positive_units + kSelectionGainUnits, kMaxPositiveUnits);
  if (preference->manual) preference->positive_units = kMaxPositiveUnits;
  if (preference->last_tick_ms == 0) preference->last_tick_ms = current.now_tick_ms;
  if (preference->lexeme_ids.empty()) preference->lexeme_ids = selection.lexeme_ids;
  if (preference->normalized_identity.empty())
    preference->normalized_identity = selection.normalized_identity;
  ++snapshot->revision;
  return Status::kOk;
}

std::size_t Digits(std::uint64_t value) {
  std::size_t count = 1;
  while (value >= 10) { value /= 10; ++count; }
  return count;
}

std::size_t EncodedSizeBound(const Snapshot& snapshot, const Catalog& catalog) {
  std::size_t bytes = 20 + 11 + catalog.name_space.size() + 16 +
      catalog.sha256.size() + 12 + catalog.profile_id.size() + 10 +
      catalog.language.size() + 14 + 14 + Digits(catalog.half_life_ms) + 10 +
      Digits(snapshot.revision);
  for (const auto& row : snapshot.words)
    bytes += 4 + Digits(row.id) + Digits(row.count);
  for (const auto& row : snapshot.pairs)
    bytes += 6 + Digits(row.previous_id) + Digits(row.id) + Digits(row.count);
  for (const auto& row : snapshot.preferences) {
    std::size_t ids = 0;
    for (std::size_t index = 0; index < row.lexeme_ids.size(); ++index) {
      if (index != 0) ++ids;
      ids += Digits(row.lexeme_ids[index]);
    }
    bytes += 10 + row.candidate_id.size() + row.normalized_identity.size() + ids +
        Digits(row.positive_units) + Digits(row.negative_units) +
        Digits(row.last_tick_ms);
  }
  return bytes;
}

std::string EncodeSnapshot(const Snapshot& snapshot, const Catalog& catalog) {
  auto words = snapshot.words;
  auto pairs = snapshot.pairs;
  auto preferences = snapshot.preferences;
  std::sort(words.begin(), words.end(), [](const WordCount& lhs, const WordCount& rhs) {
    return lhs.id < rhs.id;
  });
  std::sort(pairs.begin(), pairs.end(), [](const PairCount& lhs, const PairCount& rhs) {
    return std::tie(lhs.previous_id, lhs.id) < std::tie(rhs.previous_id, rhs.id);
  });
  std::sort(preferences.begin(), preferences.end(),
            [](const PreferenceRecord& lhs, const PreferenceRecord& rhs) {
              return lhs.candidate_id < rhs.candidate_id;
            });

  std::string result = "epocanva-learning\t3\nnamespace\t" + catalog.name_space +
      "\ncatalog-sha256\t" + catalog.sha256 + "\nprofile-id\t" + catalog.profile_id +
      "\nlanguage\t" + catalog.language + "\ndecay-unit\tms\nhalf-life-ms\t" +
      std::to_string(catalog.half_life_ms) + "\nrevision\t" +
      std::to_string(snapshot.revision) + "\n";
  for (const auto& row : words) {
    result += "w\t" + std::to_string(row.id) + "\t" + std::to_string(row.count) + "\n";
  }
  for (const auto& row : pairs) {
    result += "b\t" + std::to_string(row.previous_id) + "\t" +
        std::to_string(row.id) + "\t" + std::to_string(row.count) + "\n";
  }
  for (const auto& row : preferences) {
    result += "p\t" + row.candidate_id + "\t" + row.normalized_identity + "\t" +
        EncodeIds(row.lexeme_ids) + "\t" + std::to_string(row.positive_units) + "\t" +
        std::to_string(row.negative_units) + "\t" +
        std::to_string(row.last_tick_ms) + "\t" + (row.manual ? "1" : "0") + "\n";
  }
  return result;
}

void SynthesizeMigratedPreferences(Snapshot* snapshot, std::uint64_t baseline_tick_ms) {
  if (!snapshot->preferences.empty()) return;
  // Version 1/2 stored per-lexeme counts only. Convert the highest-count words into
  // candidate preferences; the legacy word counts remain available to the lexical ranker.
  auto words = snapshot->words;
  std::sort(words.begin(), words.end(), [](const WordCount& lhs, const WordCount& rhs) {
    if (lhs.count != rhs.count) return lhs.count > rhs.count;
    return lhs.id < rhs.id;
  });
  for (const auto& word : words) {
    if (snapshot->preferences.size() == kMaxPreferenceRecords) break;
    PreferenceRecord row;
    row.candidate_id = CandidateKey({word.id});
    row.lexeme_ids = {word.id};
    row.positive_units = Saturate(word.count * kSelectionGainUnits, kMaxPositiveUnits);
    // A migration has no event timestamp. Use the caller's current tick as the baseline
    // so the first read does not decay from the Unix epoch. Zero means "no baseline yet".
    row.last_tick_ms = baseline_tick_ms;
    snapshot->preferences.push_back(std::move(row));
  }
}

}  // namespace

const char* StatusName(Status status) {
  switch (status) {
    case Status::kOk: return "ok";
    case Status::kDenied: return "denied";
    case Status::kStale: return "stale";
    case Status::kUnattributable: return "unattributable";
    case Status::kNotFound: return "not-found";
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

const char* FeedbackKindName(FeedbackKind kind) {
  switch (kind) {
    case FeedbackKind::kSelected: return "selected";
    case FeedbackKind::kRejected: return "rejected";
    case FeedbackKind::kSuperseded: return "superseded";
    case FeedbackKind::kManualWordAdded: return "manual-word-added";
    case FeedbackKind::kManualWordRemoved: return "manual-word-removed";
  }
  return "selected";
}

bool ValidCatalogIdentity(const Catalog& catalog) {
  return ValidNamespace(catalog.name_space) && ValidSha256(catalog.sha256) &&
         ValidProfileId(catalog.profile_id) && ValidLanguage(catalog.language) &&
         ValidHalfLife(catalog.half_life_ms);
}

std::string CandidateKey(const std::vector<LexemeId>& ids) {
  if (ids.empty() || ids.size() > kMaxCommitLexemes) return {};
  std::string key = "lex:";
  for (std::size_t index = 0; index < ids.size(); ++index) {
    if (ids[index] == 0) return {};
    if (index != 0) key.push_back('+');
    key += std::to_string(ids[index]);
    if (key.size() > kMaxCandidateIdBytes) return {};
  }
  return key;
}

std::uint64_t CurrentTickMilliseconds() {
  try {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    if (millis <= 0) return 0;
    return static_cast<std::uint64_t>(millis);
  } catch (...) {
    return 0;
  }
}

bool SerializedFormatVersion(std::string_view bytes, std::uint32_t* out_version) {
  if (out_version == nullptr) return false;
  const auto newline = bytes.find('\n');
  if (newline == std::string_view::npos) return false;
  const auto header = Fields(bytes.substr(0, newline));
  if (header.size() != 2 || header[0] != "epocanva-learning") return false;
  std::uint64_t version = 0;
  if (!ParseUnsigned(header[1], &version) || version > 0xffffffffull) return false;
  *out_version = static_cast<std::uint32_t>(version);
  return true;
}

LearningStore::LearningStore(Catalog catalog) : catalog_(std::move(catalog)) {}

std::unique_ptr<LearningStore> LearningStore::Create(Catalog catalog) {
  if (!ValidCatalogIdentity(catalog) || catalog.admitted_ids.empty()) return nullptr;
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
  if (!WriteAllowed(current)) return Status::kDenied;
  Snapshot updated = snapshot_;
  const auto status = ApplySelectionTo(&updated, selection, current, catalog_, true);
  if (status != Status::kOk) return status;
  if (EncodedSizeBound(updated, catalog_) > kMaxStoreBytes)
    return Status::kLimitExceeded;
  snapshot_ = std::move(updated);
  return Status::kOk;
}

Status LearningStore::RecordFeedback(FeedbackKind kind, std::string_view candidate_id,
                                     std::string_view normalized_identity,
                                     const std::vector<LexemeId>& lexeme_ids,
                                     bool attributable, std::uint64_t generation,
                                     const Access& current, bool explicit_user_action) {
  if (kind == FeedbackKind::kManualWordRemoved) {
    if (!explicit_user_action) return Status::kDenied;
    if (current.privacy != PrivacyClass::kNormal) return Status::kDenied;
    std::string id(candidate_id);
    if (id.empty()) id = CandidateKey(lexeme_ids);
    if (!ValidCandidateId(id)) return Status::kInvalidFormat;
    Snapshot updated = snapshot_;
    auto found = std::find_if(updated.preferences.begin(), updated.preferences.end(),
        [&](const PreferenceRecord& row) { return row.candidate_id == id; });
    if (found == updated.preferences.end() || !found->manual)
      return Status::kNotFound;
    AdvanceSnapshot(&updated, current.now_tick_ms, catalog_.half_life_ms);
    found = std::find_if(updated.preferences.begin(), updated.preferences.end(),
        [&](const PreferenceRecord& row) { return row.candidate_id == id; });
    if (found == updated.preferences.end() || !found->manual)
      return Status::kNotFound;
    updated.preferences.erase(found);
    if (updated.revision == std::numeric_limits<std::uint64_t>::max())
      return Status::kRevisionExhausted;
    ++updated.revision;
    if (EncodedSizeBound(updated, catalog_) > kMaxStoreBytes)
      return Status::kLimitExceeded;
    snapshot_ = std::move(updated);
    return Status::kOk;
  }

  if (!WriteAllowed(current)) return Status::kDenied;
  std::string id(candidate_id);
  if (id.empty()) id = CandidateKey(lexeme_ids);
  if (!ValidCandidateId(id) || !ValidIdentity(normalized_identity))
    return Status::kInvalidFormat;
  if (!std::all_of(lexeme_ids.begin(), lexeme_ids.end(),
                   [&](LexemeId value) { return Admits(value); }))
    return Status::kInvalidLexeme;
  if (lexeme_ids.size() > kMaxCommitLexemes) return Status::kLimitExceeded;
  if (kind != FeedbackKind::kManualWordAdded) {
    if (!attributable) return Status::kUnattributable;
    if (generation != current.generation) return Status::kStale;
  }
  if (kind == FeedbackKind::kManualWordAdded && !explicit_user_action)
    return Status::kDenied;
  if (kind == FeedbackKind::kSelected) {
    Selection selection;
    selection.context_target = current.context_target;
    selection.generation = current.generation;
    selection.host_write_confirmed = true;
    selection.lexeme_ids = lexeme_ids;
    selection.candidate_id = id;
    selection.normalized_identity = std::string(normalized_identity);
    return Record(selection, current);
  }

  Snapshot updated = snapshot_;
  if (updated.revision == std::numeric_limits<std::uint64_t>::max())
    return Status::kRevisionExhausted;
  AdvanceSnapshot(&updated, current.now_tick_ms, catalog_.half_life_ms);
  PreferenceRecord* preference = FindPreference(&updated, id);
  if (preference == nullptr) {
    if (!MakeRoomForPreference(&updated, current.now_tick_ms, catalog_.half_life_ms))
      return Status::kLimitExceeded;
    PreferenceRecord row;
    row.candidate_id = id;
    row.normalized_identity = std::string(normalized_identity);
    row.lexeme_ids = lexeme_ids;
    row.last_tick_ms = current.now_tick_ms;
    updated.preferences.push_back(std::move(row));
    preference = &updated.preferences.back();
  }
  switch (kind) {
    case FeedbackKind::kRejected:
      preference->negative_units = Saturate(
          preference->negative_units + kRejectionGainUnits, kMaxNegativeUnits);
      break;
    case FeedbackKind::kSuperseded:
      preference->negative_units = Saturate(
          preference->negative_units + kSupersededGainUnits, kMaxNegativeUnits);
      break;
    case FeedbackKind::kManualWordAdded:
      preference->positive_units = kMaxPositiveUnits;
      preference->manual = true;
      preference->last_tick_ms = current.now_tick_ms;
      if (!normalized_identity.empty())
        preference->normalized_identity = std::string(normalized_identity);
      if (!lexeme_ids.empty()) preference->lexeme_ids = lexeme_ids;
      break;
    case FeedbackKind::kSelected:
    case FeedbackKind::kManualWordRemoved:
      break;  // handled above
  }
  if (preference->last_tick_ms == 0) preference->last_tick_ms = current.now_tick_ms;
  ++updated.revision;
  if (EncodedSizeBound(updated, catalog_) > kMaxStoreBytes)
    return Status::kLimitExceeded;
  snapshot_ = std::move(updated);
  return Status::kOk;
}

Status LearningStore::Read(const Access& current, Snapshot* out) const {
  if (out != nullptr) *out = {};
  if (!ReadAllowed(current)) return Status::kDenied;
  if (out == nullptr) return Status::kInvalidFormat;
  *out = snapshot_;
  if (current.now_tick_ms == 0) return Status::kOk;
  for (auto& row : out->preferences) {
    if (row.last_tick_ms == 0) {
      row.last_tick_ms = current.now_tick_ms;
      continue;
    }
    if (current.now_tick_ms <= row.last_tick_ms) continue;
    row.positive_units = DecayedUnits(row.positive_units, row.last_tick_ms,
                                      current.now_tick_ms, catalog_.half_life_ms);
    row.negative_units = DecayedUnits(row.negative_units, row.last_tick_ms,
                                      current.now_tick_ms, catalog_.half_life_ms);
    row.last_tick_ms = current.now_tick_ms;
  }
  return Status::kOk;
}

std::string LearningStore::Encode() const {
  return EncodeSnapshot(snapshot_, catalog_);
}

Status LearningStore::Serialize(const Access& current, std::string* out) const {
  if (out != nullptr) out->clear();
  if (!WriteAllowed(current)) return Status::kDenied;
  if (out == nullptr) return Status::kInvalidFormat;
  *out = Encode();
  if (out->size() > kMaxStoreBytes) {
    out->clear();
    return Status::kLimitExceeded;
  }
  return Status::kOk;
}

Status LearningStore::Load(std::string_view bytes, const Access& current) {
  if (!ReadAllowed(current)) return Status::kDenied;
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
    if (lines.size() > kMaxWordRecords + kMaxPairRecords + kMaxPreferenceRecords + 10)
      return Status::kLimitExceeded;
    begin_utf8_byte = end_utf8_byte + 1;
  }
  std::uint32_t version = 0;
  if (!SerializedFormatVersion(bytes, &version)) return Status::kInvalidFormat;
  if (version < 1 || version > 3) return Status::kUnsupportedVersion;
  const auto header = Fields(lines[0]);
  if (header.size() != 2 || header[0] != "epocanva-learning")
    return Status::kInvalidFormat;

  const bool version_3 = version == 3;
  const std::size_t identity_lines = version_3 ? 8 : 4;
  if (lines.size() < identity_lines) return Status::kInvalidFormat;
  const auto name_space = Fields(lines[1]);
  const auto catalog = Fields(lines[2]);
  if (name_space.size() != 2 || name_space[0] != "namespace" ||
      catalog.size() != 2 || catalog[0] != "catalog-sha256")
    return Status::kInvalidFormat;
  if (name_space[1] != catalog_.name_space || catalog[1] != catalog_.sha256)
    return Status::kCatalogMismatch;

  std::size_t revision_line = 3;
  if (version_3) {
    const auto profile = Fields(lines[3]);
    const auto language = Fields(lines[4]);
    const auto decay_unit = Fields(lines[5]);
    const auto half_life = Fields(lines[6]);
    if (profile.size() != 2 || profile[0] != "profile-id" ||
        language.size() != 2 || language[0] != "language" ||
        decay_unit.size() != 2 || decay_unit[0] != "decay-unit" ||
        half_life.size() != 2 || half_life[0] != "half-life-ms")
      return Status::kInvalidFormat;
    if (profile[1] != catalog_.profile_id || language[1] != catalog_.language)
      return Status::kCatalogMismatch;
    if (decay_unit[1] != "ms") return Status::kInvalidFormat;
    std::uint64_t stored_half_life = 0;
    if (!ParseUnsigned(half_life[1], &stored_half_life) ||
        !ValidHalfLife(stored_half_life)) return Status::kInvalidFormat;
    if (stored_half_life != catalog_.half_life_ms) return Status::kCatalogMismatch;
    revision_line = 7;
  }
  const auto revision = Fields(lines[revision_line]);
  if (revision.size() != 2 || revision[0] != "revision")
    return Status::kInvalidFormat;
  Snapshot loaded;
  if (!ParseUnsigned(revision[1], &loaded.revision)) return Status::kInvalidFormat;
  std::set<LexemeId> word_ids;
  std::set<std::pair<LexemeId, LexemeId>> pair_ids;
  std::set<std::string> candidate_ids;
  for (std::size_t record_ordinal = identity_lines; record_ordinal < lines.size();
       ++record_ordinal) {
    const auto fields = Fields(lines[record_ordinal]);
    if (fields.size() == 3 && fields[0] == "w") {
      WordCount row;
      if (!ParseUnsigned(fields[1], &row.id) || !ParseUnsigned(fields[2], &row.count) ||
          row.count == 0 || row.count > kMaxLearningCount ||
          !word_ids.insert(row.id).second) return Status::kInvalidFormat;
      if (!Admits(row.id)) return Status::kInvalidLexeme;
      loaded.words.push_back(row);
    } else if (version >= 2 && fields.size() == 4 && fields[0] == "b") {
      PairCount row;
      if (!ParseUnsigned(fields[1], &row.previous_id) ||
          !ParseUnsigned(fields[2], &row.id) ||
          !ParseUnsigned(fields[3], &row.count) || row.count == 0 ||
          row.count > kMaxLearningCount ||
          !pair_ids.emplace(row.previous_id, row.id).second)
        return Status::kInvalidFormat;
      if (!Admits(row.previous_id) || !Admits(row.id)) return Status::kInvalidLexeme;
      loaded.pairs.push_back(row);
    } else if (version_3 && fields.size() == 8 && fields[0] == "p") {
      PreferenceRecord row;
      row.candidate_id = std::string(fields[1]);
      row.normalized_identity = std::string(fields[2]);
      if (!ValidCandidateId(row.candidate_id) ||
          !ValidIdentity(row.normalized_identity) ||
          !DecodeIds(fields[3], &row.lexeme_ids) ||
          !ParseUnsigned(fields[4], &row.positive_units) ||
          !ParseUnsigned(fields[5], &row.negative_units) ||
          !ParseUnsigned(fields[6], &row.last_tick_ms) ||
          (fields[7] != "0" && fields[7] != "1"))
        return Status::kInvalidFormat;
      if (row.positive_units > kMaxPositiveUnits ||
          row.negative_units > kMaxNegativeUnits)
        return Status::kInvalidFormat;
      if (!std::all_of(row.lexeme_ids.begin(), row.lexeme_ids.end(),
                       [&](LexemeId value) { return Admits(value); }))
        return Status::kInvalidLexeme;
      if (!candidate_ids.insert(row.candidate_id).second)
        return Status::kInvalidFormat;
      row.manual = fields[7] == "1";
      loaded.preferences.push_back(std::move(row));
    } else {
      return Status::kInvalidFormat;
    }
    if (loaded.words.size() > kMaxWordRecords ||
        loaded.pairs.size() > kMaxPairRecords ||
        loaded.preferences.size() > kMaxPreferenceRecords)
      return Status::kLimitExceeded;
  }
  if (!version_3) SynthesizeMigratedPreferences(&loaded, current.now_tick_ms);
  snapshot_ = std::move(loaded);
  return Status::kOk;
}

Status LearningStore::Import(const Snapshot& snapshot, const Access& current) {
  if (!ReadAllowed(current)) return Status::kDenied;
  if (snapshot.words.size() > kMaxWordRecords ||
      snapshot.pairs.size() > kMaxPairRecords ||
      snapshot.preferences.size() > kMaxPreferenceRecords)
    return Status::kLimitExceeded;
  std::set<LexemeId> word_ids;
  for (const auto& row : snapshot.words) {
    if (!Admits(row.id) || row.count == 0 || row.count > kMaxLearningCount ||
        !word_ids.insert(row.id).second) return Status::kInvalidLexeme;
  }
  std::set<std::pair<LexemeId, LexemeId>> pair_ids;
  for (const auto& row : snapshot.pairs) {
    if (!Admits(row.previous_id) || !Admits(row.id) || row.count == 0 ||
        row.count > kMaxLearningCount ||
        !pair_ids.emplace(row.previous_id, row.id).second)
      return Status::kInvalidLexeme;
  }
  std::set<std::string> candidate_ids;
  for (const auto& row : snapshot.preferences) {
    if (!ValidCandidateId(row.candidate_id) || !ValidIdentity(row.normalized_identity) ||
        row.positive_units > kMaxPositiveUnits ||
        row.negative_units > kMaxNegativeUnits ||
        !std::all_of(row.lexeme_ids.begin(), row.lexeme_ids.end(),
                     [&](LexemeId value) { return Admits(value); }) ||
        !candidate_ids.insert(row.candidate_id).second)
      return Status::kInvalidFormat;
  }
  snapshot_ = snapshot;
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
  if (out->size() > kMaxStoreBytes) {
    out->clear();
    return Status::kLimitExceeded;
  }
  return Status::kOk;
}

void LearningStore::AdvanceTo(std::uint64_t now_tick_ms) {
  AdvanceSnapshot(&snapshot_, now_tick_ms, catalog_.half_life_ms);
}

}  // namespace learning
}  // namespace epocanva
