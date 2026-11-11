#include "learning_store.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace epocanva::learning;
int failures = 0;
void Check(bool condition, const char* label) {
  if (!condition) { std::cerr << "FAIL " << label << '\n'; ++failures; }
}
Catalog TestCatalog() {
  Catalog catalog;
  catalog.name_space = "en.test.v1";
  catalog.sha256 = std::string(64, 'a');
  catalog.admitted_ids = {1, 2, 3, 4, 5};
  catalog.profile_id = "3";
  catalog.language = "en";
  catalog.half_life_ms = 1000;  // short half-life so decay cases are exact
  return catalog;
}
Access Normal(std::uint64_t now = 0) {
  Access access;
  access.privacy = PrivacyClass::kNormal;
  access.learning_enabled = true;
  access.context_target = 20;
  access.generation = 30;
  access.now_tick_ms = now;
  return access;
}
Selection Selected() {
  Selection selection;
  selection.context_target = 20;
  selection.generation = 30;
  selection.host_write_confirmed = true;
  selection.previous_id = 1;
  selection.lexeme_ids = {2, 3};
  selection.normalized_identity = "hello world";
  return selection;
}
std::string Encoded(LearningStore& store) {
  std::string bytes;
  Check(store.Export(true, &bytes) == Status::kOk, "export for comparison");
  return bytes;
}
const PreferenceRecord* Preference(const Snapshot& snapshot, const std::string& id) {
  for (const auto& row : snapshot.preferences)
    if (row.candidate_id == id) return &row;
  return nullptr;
}
void Privacy() {
  auto store = LearningStore::Create(TestCatalog());
  const auto before = Encoded(*store);
  for (auto privacy : {PrivacyClass::kUnknown, PrivacyClass::kSensitive}) {
    auto access = Normal(); access.privacy = privacy;
    Check(store->Record(Selected(), access) == Status::kDenied, "privacy denies write");
    Snapshot out; out.words.push_back({4, 4});
    Check(store->Read(access, &out) == Status::kDenied && out.words.empty(),
          "privacy clears read");
    std::string bytes = "previous";
    Check(store->Serialize(access, &bytes) == Status::kDenied && bytes.empty(),
          "privacy denies serialization");
    Check(store->Load(before, access) == Status::kDenied, "privacy denies import");
    Check(Encoded(*store) == before, "privacy leaves counts unchanged");
  }
  auto paused = Normal();
  paused.learning_enabled = false;
  Check(store->Record(Selected(), paused) == Status::kDenied,
        "paused recording denies write");
  Snapshot read;
  Check(store->Read(paused, &read) == Status::kOk,
        "paused recording still reads existing preferences");
  Check(store->Load(before, paused) == Status::kOk,
        "paused recording can load the durable view for ranking");
  std::string bytes;
  Check(store->Serialize(paused, &bytes) == Status::kDenied,
        "paused recording cannot serialize a new write");
  auto access = Normal(); access.context_target = 0;
  Check(store->Record(Selected(), access) == Status::kDenied, "zero target denied");
  access = Normal(); access.generation = 0;
  Check(store->Record(Selected(), access) == Status::kDenied, "zero generation denied");
  Check(Encoded(*store) == before, "denials have no mutation");
}
void Identity() {
  auto catalog = TestCatalog(); catalog.admitted_ids.push_back(1);
  Check(!LearningStore::Create(catalog), "duplicate catalog ID refused");
  catalog = TestCatalog(); catalog.admitted_ids.push_back(0);
  Check(!LearningStore::Create(catalog), "zero catalog ID refused");
  catalog = TestCatalog(); catalog.name_space = "../en";
  Check(!LearningStore::Create(catalog), "namespace traversal refused");
  catalog = TestCatalog(); catalog.sha256 = "sha";
  Check(!LearningStore::Create(catalog), "unbound catalog refused");
  catalog = TestCatalog(); catalog.profile_id = "bad profile";
  Check(!LearningStore::Create(catalog), "invalid profile ID refused");
  catalog = TestCatalog(); catalog.language = "EN";
  Check(!LearningStore::Create(catalog), "non-canonical language refused");
  catalog = TestCatalog(); catalog.half_life_ms = 0;
  Check(!LearningStore::Create(catalog), "zero half-life refused");
  auto store = LearningStore::Create(TestCatalog());
  auto selected = Selected(); selected.context_target++;
  Check(store->Record(selected, Normal()) == Status::kStale, "target mismatch");
  selected = Selected(); selected.generation++;
  Check(store->Record(selected, Normal()) == Status::kStale, "generation mismatch");
  selected = Selected(); selected.host_write_confirmed = false;
  Check(store->Record(selected, Normal()) == Status::kStale, "unconfirmed edit");
  selected = Selected(); selected.lexeme_ids = {2, 99};
  const auto before = Encoded(*store);
  Check(store->Record(selected, Normal()) == Status::kInvalidLexeme,
        "unknown lexical ID");
  Check(Encoded(*store) == before, "unknown trailing ID not partially learned");
  selected.lexeme_ids = {0};
  Check(store->Record(selected, Normal()) == Status::kInvalidLexeme, "zero lexical ID");
  selected = Selected(); selected.previous_id = 99;
  Check(store->Record(selected, Normal()) == Status::kInvalidLexeme,
        "unknown context ID");
  selected = Selected(); selected.candidate_id = "bad key";
  Check(store->Record(selected, Normal()) == Status::kInvalidFormat,
        "candidate ID with whitespace refused");
  selected = Selected(); selected.candidate_id = std::string("bad\x01key");
  Check(store->Record(selected, Normal()) == Status::kInvalidFormat,
        "candidate ID control byte refused");
  selected = Selected(); selected.normalized_identity = std::string("bad\x01word");
  Check(store->Record(selected, Normal()) == Status::kInvalidFormat,
        "identity control byte refused");
  selected = Selected(); selected.normalized_identity = std::string("\xC0\xAF", 2);
  Check(store->Record(selected, Normal()) == Status::kInvalidFormat,
        "malformed UTF-8 identity refused");
  Check(!CandidateKey({}).size(), "empty candidate key refused");
  Check(CandidateKey({2, 3}) == "lex:2+3", "stable candidate key");
  Check(!CandidateKey({2, 0}).size(), "zero in candidate key refused");
  Check(!CandidateKey(std::vector<LexemeId>(9, 1)).size(),
        "oversized candidate key refused");
}
void Counts() {
  auto store = LearningStore::Create(TestCatalog());
  Check(store->Record(Selected(), Normal()) == Status::kOk, "record selection");
  Check(store->Record(Selected(), Normal()) == Status::kOk, "record aggregate");
  Snapshot snapshot;
  Check(store->Read(Normal(), &snapshot) == Status::kOk, "read counts");
  Check(snapshot.revision == 2 && snapshot.words.size() == 2 &&
        snapshot.pairs.size() == 2, "bounded context per committed word");
  Check(snapshot.words[0].id == 2 && snapshot.words[0].count == 2, "word aggregate");
  Check(snapshot.pairs[0].previous_id == 1 && snapshot.pairs[0].id == 2 &&
        snapshot.pairs[0].count == 2, "previous word pair");
  Check(snapshot.pairs[1].previous_id == 2 && snapshot.pairs[1].id == 3,
        "internal word pair");
  const auto* preference = Preference(snapshot, "lex:2+3");
  Check(preference != nullptr, "selection creates a candidate preference");
  Check(preference != nullptr && preference->positive_units == 1200 &&
        preference->negative_units == 0 && !preference->manual,
        "two selections accumulate 1.2 points");
  Check(preference != nullptr && preference->normalized_identity == "hello world",
        "normalized identity preserved");
  auto other = LearningStore::Create(TestCatalog());
  Check(other->Load(Encoded(*store), Normal()) == Status::kOk, "round trip parse");
  Check(Encoded(*other) == Encoded(*store), "deterministic round trip");
  Snapshot again;
  Check(store->Read(Normal(), &again) == Status::kOk,
        "repeated read succeeds");
  Check(again.revision == snapshot.revision &&
        again.preferences.size() == snapshot.preferences.size(),
        "repeated read is deterministic");
  auto different_catalog = TestCatalog(); different_catalog.name_space = "fr.test.v1";
  auto different = LearningStore::Create(different_catalog);
  Check(different->Load(Encoded(*store), Normal()) == Status::kCatalogMismatch,
        "language isolation");
  different_catalog = TestCatalog(); different_catalog.sha256.assign(64, 'b');
  different = LearningStore::Create(different_catalog);
  Check(different->Load(Encoded(*store), Normal()) == Status::kCatalogMismatch,
        "dictionary isolation");
  different_catalog = TestCatalog(); different_catalog.profile_id = "4";
  different = LearningStore::Create(different_catalog);
  Check(different->Load(Encoded(*store), Normal()) == Status::kCatalogMismatch,
        "profile isolation");
  different_catalog = TestCatalog(); different_catalog.half_life_ms = 2000;
  different = LearningStore::Create(different_catalog);
  Check(different->Load(Encoded(*store), Normal()) == Status::kCatalogMismatch,
        "decay configuration isolation");
}
void Migration() {
  auto store = LearningStore::Create(TestCatalog());
  std::string legacy_v1 = "epocanva-learning\t1\nnamespace\ten.test.v1\ncatalog-sha256\t" +
      std::string(64, 'a') + "\nrevision\t5\nw\t2\t2\n";
  Check(store->Load(legacy_v1, Normal()) == Status::kOk, "v1 migration");
  Snapshot snapshot;
  store->Read(Normal(), &snapshot);
  Check(snapshot.revision == 5 && snapshot.words.size() == 1 &&
        snapshot.words[0].count == 2 && snapshot.pairs.empty(),
        "v1 counts preserved without invented pairs");
  const auto* migrated = Preference(snapshot, "lex:2");
  Check(migrated != nullptr && migrated->positive_units == 1200 &&
        !migrated->manual, "v1 counts become bounded preferences");
  const auto v3 = Encoded(*store);
  Check(v3.find("epocanva-learning\t3\n") == 0, "v3 serialization");
  Check(v3.find("profile-id\t3\n") != std::string::npos, "profile identity persisted");
  Check(v3.find("language\ten\n") != std::string::npos, "language identity persisted");
  Check(v3.find("decay-unit\tms\n") != std::string::npos, "decay unit documented");
  Check(v3.find("half-life-ms\t1000\n") != std::string::npos,
        "half-life persisted");
  auto second = LearningStore::Create(TestCatalog());
  std::string legacy_v2 = "epocanva-learning\t2\nnamespace\ten.test.v1\ncatalog-sha256\t" +
      std::string(64, 'a') + "\nrevision\t7\nw\t2\t4\nb\t1\t2\t3\n";
  Check(second->Load(legacy_v2, Normal()) == Status::kOk, "v2 migration");
  Snapshot migrated_v2;
  second->Read(Normal(), &migrated_v2);
  Check(migrated_v2.revision == 7 && migrated_v2.words.size() == 1 &&
        migrated_v2.pairs.size() == 1, "v2 words and pairs preserved");
  Check(Preference(migrated_v2, "lex:2") != nullptr &&
        Preference(migrated_v2, "lex:2")->positive_units == kMaxPositiveUnits,
        "v2 count saturates the preference bound");
  const auto before = Encoded(*second);
  Check(second->Load(legacy_v1 + "b\t1\t2\t9\n", Normal()) == Status::kInvalidFormat,
        "v1 cannot include v2 pair records");
  Check(Encoded(*second) == before, "failed migration atomic");
  auto future = before;
  future.replace(future.find('\t') + 1, 1, "4");
  Check(second->Load(future, Normal()) == Status::kUnsupportedVersion,
        "future version refused");
  std::uint32_t version = 0;
  Check(SerializedFormatVersion(before, &version) && version == 3,
        "version probe reads v3");
  Check(!SerializedFormatVersion("not a store\n", &version),
        "version probe refuses foreign text");
}
void Malformed() {
  auto store = LearningStore::Create(TestCatalog());
  store->Record(Selected(), Normal());
  const auto before = Encoded(*store);
  const auto empty = Encoded(*LearningStore::Create(TestCatalog()));
  const std::string suffixes[] = {"w\t2\t0\n", "w\t0\t2\n", "w\t99\t2\n",
      "w\t2\t01\n", "w\t2\t1000000001\n", "w\t2\t18446744073709551616\n",
      "w\t2\t2\nw\t2\t3\n", "b\t1\t2\t1\nb\t1\t2\t2\n", "b\t1\t2\n",
      "w\t2\t1\textra\n", "arbitrary user content\n", "\n"};
  for (const auto& suffix : suffixes) {
    Check(store->Load(empty + suffix, Normal()) != Status::kOk, "malformed rejected");
    Check(Encoded(*store) == before, "malformed atomic");
  }
  // A v3 file with a duplicate candidate key, out-of-range units, a bad identity, or
  // a non-admitted lexeme ID must be rejected rather than partially applied.
  const std::string bad_v3[] = {
      "p\tlex:2\t\t2\t600\t0\t0\t0\np\tlex:2\t\t2\t600\t0\t0\t0\n",
      "p\tlex:2\t\t2\t2001\t0\t0\t0\n",
      "p\tlex:2\t\t2\t600\t1501\t0\t0\n",
      "p\tlex:2\t\t2\t600\t0\t0\t2\n",
      "p\tbad key\t\t2\t600\t0\t0\t0\n",
      "p\tlex:2\tbad\x01word\t2\t600\t0\t0\t0\n",
      "p\tlex:2\t\t99\t600\t0\t0\t0\n",
      "p\tlex:2\t\t2,2\t600\t0\t0\t0\n",
      "p\tlex:2\t\t0\t600\t0\t0\t0\n",
      "p\tlex:2\t\t2\t600\n",
  };
  for (const auto& suffix : bad_v3) {
    Check(store->Load(empty + suffix, Normal()) != Status::kOk,
          "malformed v3 rejected");
    Check(Encoded(*store) == before, "malformed v3 atomic");
  }
  Check(store->Load(before.substr(0, before.size() - 1), Normal()) ==
        Status::kInvalidFormat, "truncated final row rejected");
  Check(store->Load(std::string(kMaxStoreBytes + 1, 'x'), Normal()) ==
        Status::kLimitExceeded, "oversized file bounded");
  const auto revision_start = empty.find("revision\t") + std::string("revision\t").size();
  auto exhausted = empty;
  exhausted.replace(revision_start, 1, std::to_string(std::numeric_limits<std::uint64_t>::max()));
  Check(store->Load(exhausted, Normal()) == Status::kOk, "max revision parse");
  Check(store->Record(Selected(), Normal()) == Status::kRevisionExhausted,
        "revision cannot wrap");
}
void Bounded() {
  auto catalog = TestCatalog();
  catalog.admitted_ids.clear();
  for (std::uint64_t id = 1; id <= 5000; ++id) catalog.admitted_ids.push_back(id);
  auto store = LearningStore::Create(catalog);
  for (std::uint64_t id = 1; id <= 3000; ++id) {
    Selection selected{20, 30, true, id + 1, {id}, {}, {}};
    Check(store->Record(selected, Normal()) == Status::kOk, "bounded record");
  }
  Snapshot snapshot;
  store->Read(Normal(), &snapshot);
  Check(snapshot.words.size() == kMaxWordRecords, "word record cap retained");
  Check(snapshot.pairs.size() == kMaxPairRecords, "pair record cap retained");
  Check(snapshot.preferences.size() == kMaxPreferenceRecords,
        "preference record cap retained");
  Check(snapshot.revision == 3000, "bounded revision");
  const auto encoded = Encoded(*store);
  Check(encoded.size() < kMaxStoreBytes, "format bounded bytes");
  auto loaded = LearningStore::Create(catalog);
  Check(loaded->Load(encoded, Normal()) == Status::kOk, "full store round trip");
  Selection too_many{20, 30, true, 0, std::vector<LexemeId>(kMaxCommitLexemes + 1, 1), {}, {}};
  Check(store->Record(too_many, Normal()) == Status::kLimitExceeded,
        "commit words bounded");
}
void Feedback() {
  auto store = LearningStore::Create(TestCatalog());
  auto access = Normal(1000);
  Check(store->RecordFeedback(FeedbackKind::kRejected, "cand-reject", "", {}, true, 30,
                              access) == Status::kOk, "rejection recorded");
  Check(store->RecordFeedback(FeedbackKind::kRejected, "cand-reject", "", {}, true, 30,
                              access) == Status::kOk, "rejection accumulates");
  Snapshot snapshot;
  store->Read(access, &snapshot);
  const auto* rejected = Preference(snapshot, "cand-reject");
  Check(rejected != nullptr && rejected->negative_units == 1000,
        "two rejections accumulate one negative point");
  for (int index = 0; index < 50; ++index)
    (void)store->RecordFeedback(FeedbackKind::kRejected, "cand-reject", "", {}, true, 30,
                                access);
  store->Read(access, &snapshot);
  rejected = Preference(snapshot, "cand-reject");
  Check(rejected != nullptr && rejected->negative_units == kMaxNegativeUnits,
        "rejection saturates");
  Check(store->RecordFeedback(FeedbackKind::kSuperseded, "cand-superseded", "", {}, true,
                              30, access) == Status::kOk, "superseded recorded");
  store->Read(access, &snapshot);
  const auto* superseded = Preference(snapshot, "cand-superseded");
  Check(superseded != nullptr && superseded->negative_units == kSupersededGainUnits,
        "superseded is weaker than rejection");
  Check(store->RecordFeedback(FeedbackKind::kRejected, "cand-attrib", "", {}, false, 30,
                              access) == Status::kUnattributable,
        "unattributable rejection refused");
  Check(store->RecordFeedback(FeedbackKind::kRejected, "cand-stale", "", {}, true, 29,
                              access) == Status::kStale,
        "stale generation refused");
  Check(store->RecordFeedback(FeedbackKind::kRejected, "cand-paused", "", {}, true, 30,
                              [] {
                                auto value = Normal();
                                value.learning_enabled = false;
                                return value;
                              }()) == Status::kDenied,
        "paused recording refuses new negative feedback");
}
void Decay() {
  auto store = LearningStore::Create(TestCatalog());
  Check(store->Record(Selected(), Normal(1000)) == Status::kOk, "record at tick");
  Snapshot snapshot;
  store->Read(Normal(1000), &snapshot);
  Check(Preference(snapshot, "lex:2+3") != nullptr &&
        Preference(snapshot, "lex:2+3")->positive_units == 600, "initial positive");
  store->Read(Normal(2000), &snapshot);
  Check(Preference(snapshot, "lex:2+3")->positive_units == 300,
        "one half-life halves the preference");
  store->Read(Normal(3000), &snapshot);
  Check(Preference(snapshot, "lex:2+3")->positive_units == 150,
        "two half-lives quarter the preference");
  store->Read(Normal(500), &snapshot);
  Check(Preference(snapshot, "lex:2+3")->positive_units == 600,
        "clock rollback does not decay or increase a preference");
  store->Read(Normal(1000 + kMaxDecayDeltaMilliseconds + 1), &snapshot);
  Check(Preference(snapshot, "lex:2+3")->positive_units == 0,
        "pathological forward jump clamps to zero, not underflow");
  // A migrated record has no timestamp; the first read establishes a baseline instead
  // of decaying from the Unix epoch and erasing it.
  auto migrated = LearningStore::Create(TestCatalog());
  std::string legacy = "epocanva-learning\t2\nnamespace\ten.test.v1\ncatalog-sha256\t" +
      std::string(64, 'a') + "\nrevision\t1\nw\t2\t2\n";
  Check(migrated->Load(legacy, Normal(1000)) == Status::kOk, "migrated store");
  Snapshot first;
  migrated->Read(Normal(2000), &first);
  Check(Preference(first, "lex:2")->positive_units == 600,
        "migrated preference decays from its recorded baseline");
  migrated->Read(Normal(3000), &first);
  Check(Preference(first, "lex:2")->positive_units == 300,
        "migrated preference continues decaying from the same baseline");
}
void Manual() {
  auto catalog = TestCatalog();
  catalog.admitted_ids.clear();
  for (std::uint64_t id = 1; id <= 600; ++id) catalog.admitted_ids.push_back(id);
  auto store = LearningStore::Create(catalog);
  auto access = Normal(1000);
  Check(store->RecordFeedback(FeedbackKind::kManualWordAdded, "manual:hello", "hello", {},
                              false, 0, access, false) == Status::kDenied,
        "manual add requires explicit action");
  Check(store->RecordFeedback(FeedbackKind::kManualWordAdded, "manual:hello", "hello", {},
                              false, 0, access, true) == Status::kOk,
        "manual word added");
  Snapshot snapshot;
  store->Read(access, &snapshot);
  const auto* manual = Preference(snapshot, "manual:hello");
  Check(manual != nullptr && manual->manual && manual->positive_units == kMaxPositiveUnits,
        "manual word starts at the positive bound and is flagged");
  Check(store->RecordFeedback(FeedbackKind::kRejected, "manual:hello", "", {}, true, 30,
                              access) == Status::kOk,
        "manual word can receive negative feedback");
  store->Read(access, &snapshot);
  Check(Preference(snapshot, "manual:hello")->manual &&
        Preference(snapshot, "manual:hello")->negative_units == kRejectionGainUnits,
        "manual flag survives negative feedback");
  // Fill the preference table with manual entries, then verify an inferred entry cannot
  // evict one: ordinary eviction must refuse rather than delete user-authored words.
  for (std::uint64_t id = 1; id < kMaxPreferenceRecords; ++id) {
    const std::string candidate = "manual:" + std::to_string(id);
    Check(store->RecordFeedback(FeedbackKind::kManualWordAdded, candidate, "", {id},
                                false, 0, access, true) == Status::kOk,
          "manual table fill");
  }
  Check(store->RecordFeedback(FeedbackKind::kRejected, "inferred:new", "", {}, true, 30,
                              access) == Status::kLimitExceeded,
        "inferred entry cannot evict a manual entry");
  Snapshot full;
  store->Read(access, &full);
  Check(full.preferences.size() == kMaxPreferenceRecords,
        "manual table remains bounded");
  auto paused = Normal(2000);
  paused.learning_enabled = false;
  Check(store->RecordFeedback(FeedbackKind::kManualWordRemoved, "manual:hello", "", {},
                              false, 0, paused, true) == Status::kOk,
        "manual removal works while recording is paused");
  Check(store->RecordFeedback(FeedbackKind::kManualWordRemoved, "manual:hello", "", {},
                              false, 0, paused, true) == Status::kNotFound,
        "removing a missing manual word reports not found");
  Check(store->RecordFeedback(FeedbackKind::kManualWordRemoved, "inferred:new", "", {},
                              false, 0, paused, true) == Status::kNotFound,
        "removing a non-manual entry is refused");
}
void ExportDelete() {
  auto store = LearningStore::Create(TestCatalog());
  store->Record(Selected(), Normal());
  std::string bytes;
  Check(store->Export(false, &bytes) == Status::kDenied && bytes.empty(),
        "export explicit only");
  Check(store->Clear(false) == Status::kDenied, "delete explicit only");
  Check(store->Export(true, &bytes) == Status::kOk &&
        bytes.find("p\tlex:2+3") != bytes.npos, "export real data");
  Check(store->Clear(true) == Status::kOk, "delete independent of focused field");
  Snapshot snapshot;
  store->Read(Normal(), &snapshot);
  Check(snapshot.words.empty() && snapshot.pairs.empty() &&
        snapshot.preferences.empty() && snapshot.revision == 2,
        "clear removes records and increments revision");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::string which = argv[1];
  if (which == "--force-failure") { Check(false, "intentional control"); }
  else if (which == "privacy") Privacy();
  else if (which == "identity") Identity();
  else if (which == "counts") Counts();
  else if (which == "migration") Migration();
  else if (which == "malformed") Malformed();
  else if (which == "bounded") Bounded();
  else if (which == "feedback") Feedback();
  else if (which == "decay") Decay();
  else if (which == "manual") Manual();
  else if (which == "export") ExportDelete();
  else return 2;
  if (failures) return 1;
  std::cout << "PASS " << which << '\n';
  return 0;
}
