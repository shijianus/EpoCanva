#include "learning_store.h"

#include <algorithm>
#include <iostream>
#include <limits>
#include <string>

namespace {
using namespace epocanva::learning;
int failures = 0;
void Check(bool condition, const char* label) {
  if (!condition) { std::cerr << "FAIL " << label << '\n'; ++failures; }
}
Catalog TestCatalog() {
  return {"en.test.v1", std::string(64, 'a'), {1, 2, 3, 4, 5}};
}
Access Normal() { return {PrivacyClass::kNormal, true, 20, 30}; }
Selection Selected() { return {20, 30, true, 1, {2, 3}}; }
std::string Encoded(LearningStore& store) {
  std::string bytes;
  Check(store.Export(true, &bytes) == Status::kOk, "export for comparison");
  return bytes;
}
void Privacy() {
  auto store = LearningStore::Create(TestCatalog());
  const auto before = Encoded(*store);
  for (auto privacy : {PrivacyClass::kUnknown, PrivacyClass::kSensitive}) {
    auto access = Normal(); access.privacy = privacy;
    Check(store->Record(Selected(), access) == Status::kDenied, "privacy denies write");
    Snapshot out; out.words.push_back({4, 4});
    Check(store->Read(access, &out) == Status::kDenied && out.words.empty(), "privacy clears read");
    std::string bytes = "previous";
    Check(store->Serialize(access, &bytes) == Status::kDenied && bytes.empty(), "privacy denies serialization");
    Check(store->Load(before, access) == Status::kDenied, "privacy denies import");
    Check(Encoded(*store) == before, "privacy leaves counts unchanged");
  }
  auto access = Normal(); access.learning_enabled = false;
  Check(store->Record(Selected(), access) == Status::kDenied, "off denies write");
  access = Normal(); access.context_target = 0;
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
  auto store = LearningStore::Create(TestCatalog());
  auto selected = Selected(); selected.context_target++;
  Check(store->Record(selected, Normal()) == Status::kStale, "target mismatch");
  selected = Selected(); selected.generation++;
  Check(store->Record(selected, Normal()) == Status::kStale, "generation mismatch");
  selected = Selected(); selected.host_write_confirmed = false;
  Check(store->Record(selected, Normal()) == Status::kStale, "unconfirmed edit");
  selected = Selected(); selected.lexeme_ids = {2, 99};
  const auto before = Encoded(*store);
  Check(store->Record(selected, Normal()) == Status::kInvalidLexeme, "unknown lexical ID");
  Check(Encoded(*store) == before, "unknown trailing ID not partially learned");
  selected.lexeme_ids = {0};
  Check(store->Record(selected, Normal()) == Status::kInvalidLexeme, "zero lexical ID");
  selected = Selected(); selected.previous_id = 99;
  Check(store->Record(selected, Normal()) == Status::kInvalidLexeme, "unknown context ID");
}
void Counts() {
  auto store = LearningStore::Create(TestCatalog());
  Check(store->Record(Selected(), Normal()) == Status::kOk, "record selection");
  Check(store->Record(Selected(), Normal()) == Status::kOk, "record aggregate");
  Snapshot snapshot;
  Check(store->Read(Normal(), &snapshot) == Status::kOk, "read counts");
  Check(snapshot.revision == 2 && snapshot.words.size() == 2 && snapshot.pairs.size() == 2,
        "bounded context per committed word");
  Check(snapshot.words[0].id == 2 && snapshot.words[0].count == 2, "word aggregate");
  Check(snapshot.pairs[0].previous_id == 1 && snapshot.pairs[0].id == 2 &&
        snapshot.pairs[0].count == 2, "previous word pair");
  Check(snapshot.pairs[1].previous_id == 2 && snapshot.pairs[1].id == 3, "internal word pair");
  auto other = LearningStore::Create(TestCatalog());
  Check(other->Load(Encoded(*store), Normal()) == Status::kOk, "round trip parse");
  Check(Encoded(*other) == Encoded(*store), "deterministic round trip");
  auto different_catalog = TestCatalog(); different_catalog.name_space = "fr.test.v1";
  auto different = LearningStore::Create(different_catalog);
  Check(different->Load(Encoded(*store), Normal()) == Status::kCatalogMismatch, "language isolation");
  different_catalog = TestCatalog(); different_catalog.sha256.assign(64, 'b');
  different = LearningStore::Create(different_catalog);
  Check(different->Load(Encoded(*store), Normal()) == Status::kCatalogMismatch, "dictionary isolation");
}
void Migration() {
  auto store = LearningStore::Create(TestCatalog());
  std::string legacy = "epocanva-learning\t1\nnamespace\ten.test.v1\ncatalog-sha256\t" +
      std::string(64, 'a') + "\nrevision\t5\nw\t2\t7\n";
  Check(store->Load(legacy, Normal()) == Status::kOk, "v1 migration");
  Snapshot snapshot;
  store->Read(Normal(), &snapshot);
  Check(snapshot.revision == 5 && snapshot.words.size() == 1 && snapshot.words[0].count == 7 &&
        snapshot.pairs.empty(), "v1 counts preserved without invented pairs");
  Check(Encoded(*store).find("epocanva-learning\t2\n") == 0, "v2 serialization");
  const auto before = Encoded(*store);
  Check(store->Load(legacy + "b\t1\t2\t9\n", Normal()) == Status::kInvalidFormat,
        "v1 cannot include v2 pair records");
  Check(Encoded(*store) == before, "failed migration atomic");
  auto future = before; future.replace(future.find('\t') + 1, 1, "3");
  Check(store->Load(future, Normal()) == Status::kUnsupportedVersion, "future version refused");
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
  Check(store->Load(before.substr(0, before.size() - 1), Normal()) == Status::kInvalidFormat,
        "truncated final row rejected");
  Check(store->Load(std::string(kMaxStoreBytes + 1, 'x'), Normal()) == Status::kLimitExceeded,
        "oversized file bounded");
  const auto revision_start = empty.find("revision\t") + std::string("revision\t").size();
  auto exhausted = empty;
  exhausted.replace(revision_start, 1, std::to_string(std::numeric_limits<std::uint64_t>::max()));
  Check(store->Load(exhausted, Normal()) == Status::kOk, "max revision parse");
  Check(store->Record(Selected(), Normal()) == Status::kRevisionExhausted, "revision cannot wrap");
}
void Bounded() {
  auto catalog = TestCatalog(); catalog.admitted_ids.clear();
  for (std::uint64_t id = 1; id <= 5000; ++id) catalog.admitted_ids.push_back(id);
  auto store = LearningStore::Create(catalog);
  for (std::uint64_t id = 1; id <= 3000; ++id) {
    Selection selected{20, 30, true, id + 1, {id}};
    Check(store->Record(selected, Normal()) == Status::kOk, "bounded record");
  }
  Snapshot snapshot;
  store->Read(Normal(), &snapshot);
  Check(snapshot.words.size() == kMaxWordRecords && snapshot.pairs.size() == kMaxPairRecords,
        "record cap retained");
  Check(snapshot.revision == 3000, "bounded revision");
  const auto encoded = Encoded(*store);
  Check(encoded.size() < kMaxStoreBytes, "format bounded bytes");
  auto loaded = LearningStore::Create(catalog);
  Check(loaded->Load(encoded, Normal()) == Status::kOk, "full store round trip");
  Selection too_many{20, 30, true, 0, std::vector<LexemeId>(kMaxCommitLexemes + 1, 1)};
  Check(store->Record(too_many, Normal()) == Status::kLimitExceeded, "commit words bounded");
}
void ExportDelete() {
  auto store = LearningStore::Create(TestCatalog());
  store->Record(Selected(), Normal());
  std::string bytes;
  Check(store->Export(false, &bytes) == Status::kDenied && bytes.empty(), "export explicit only");
  Check(store->Clear(false) == Status::kDenied, "delete explicit only");
  Check(store->Export(true, &bytes) == Status::kOk && bytes.find("w\t2\t1\n") != bytes.npos,
        "export real data");
  Check(store->Clear(true) == Status::kOk, "delete independent of focused field");
  Snapshot snapshot;
  store->Read(Normal(), &snapshot);
  Check(snapshot.words.empty() && snapshot.pairs.empty() && snapshot.revision == 2,
        "clear removes records increments revision");
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
  else if (which == "export") ExportDelete();
  else return 2;
  if (failures) return 1;
  std::cout << "PASS " << which << '\n';
  return 0;
}
