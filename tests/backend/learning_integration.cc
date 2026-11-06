// Real persistent-preference integration: a v3 store written through the repository is
// read back by the backend's real candidate pipeline, re-orders a real completion menu,
// survives a backend restart, and still applies when recording is paused.
#include "epocanva_backend.h"
#include "engine/data_store/windows_learning_repository.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
namespace learn = epocanva::learning;
int failures = 0;
void Check(bool value, const char* label) {
  if (!value) { ++failures; std::cerr << "FAIL " << label << '\n'; }
}
template <class T> T Init() {
  T value{};
  value.struct_size_bytes = sizeof(T);
  value.abi_version = 1;
  return value;
}
epocanva_utf8_view View(const std::string& text) {
  return {text.data(), text.size()};
}
struct Row {
  std::string text;
  std::uint64_t token = 0;
  std::vector<std::uint64_t> lexeme_ids;
  std::uint32_t kind = 0;
};
struct Menu {
  std::vector<Row> rows;
};
Menu Query(epocanva_backend_session_handle session, const epocanva_backend_request& access,
           const std::string& input) {
  Menu menu;
  Check(epocanva_backend_set_input(session, &access, View(input)) == EPOCANVA_OK,
        "set input succeeds");
  auto query = Init<epocanva_backend_query>();
  query.context = access.context;
  query.learning_enabled = access.learning_enabled;
  query.query_kind = EPOCANVA_BACKEND_COMPLETION;
  query.max_candidates = 32;
  query.max_continuation_words = 1;
  query.search_budget = 100000;
  Check(epocanva_backend_query_candidates(session, &query) == EPOCANVA_OK,
        "completion query succeeds");
  auto snapshot = Init<epocanva_backend_snapshot>();
  auto status = epocanva_backend_read_snapshot(session, &access, &snapshot, nullptr, 0);
  Check(status == EPOCANVA_BUFFER_TOO_SMALL || status == EPOCANVA_OK,
        "snapshot sizing");
  std::string bytes(static_cast<std::size_t>(snapshot.required_utf8_bytes), '\0');
  Check(epocanva_backend_read_snapshot(session, &access, &snapshot, bytes.data(),
                                       bytes.size()) == EPOCANVA_OK,
        "snapshot copy");
  for (std::uint32_t ordinal = 0; ordinal < snapshot.candidate_count; ++ordinal) {
    auto row = Init<epocanva_backend_candidate>();
    Check(epocanva_backend_read_candidate(session, &access, snapshot.snapshot_token,
          ordinal, &row, nullptr, 0) == EPOCANVA_BUFFER_TOO_SMALL,
          "candidate sizing");
    std::string text(static_cast<std::size_t>(row.required_utf8_bytes), '\0');
    Check(epocanva_backend_read_candidate(session, &access, snapshot.snapshot_token,
          ordinal, &row, text.data(), text.size()) == EPOCANVA_OK,
          "candidate copy");
    Row parsed;
    parsed.text = std::move(text);
    parsed.token = row.candidate_token;
    parsed.kind = row.kind;
    parsed.lexeme_ids.assign(row.lexeme_ids, row.lexeme_ids + row.lexeme_count);
    menu.rows.push_back(std::move(parsed));
  }
  return menu;
}
std::size_t IndexOf(const Menu& menu, const std::string& text) {
  for (std::size_t index = 0; index < menu.rows.size(); ++index)
    if (menu.rows[index].text == text) return index;
  return menu.rows.size();
}
std::string ReadDigest(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string digest;
  input >> digest;
  return digest;
}
epocanva_backend_handle OpenBackend(const std::string& model_path,
                                    const std::string& digest,
                                    const std::string& learning_root) {
  std::vector<epocanva_backend_resource> resources;
  auto resource = Init<epocanva_backend_resource>();
  resource.language = EPOCANVA_BACKEND_EN;
  resource.model_path = View(model_path);
  resource.model_sha256 = View(digest);
  resources.push_back(resource);
  auto options = Init<epocanva_backend_options>();
  options.resources = resources.data();
  options.resource_count = static_cast<std::uint32_t>(resources.size());
  options.allow_test_fixture = 1;
  options.learning_root = View(learning_root);
  epocanva_backend_handle backend = 0;
  Check(epocanva_backend_open(&options, &backend) == EPOCANVA_OK && backend != 0,
        "backend opens with a learning root");
  return backend;
}
epocanva_backend_session_handle OpenSession(epocanva_backend_handle backend,
                                            const epocanva_context& context,
                                            std::uint32_t learning_enabled) {
  auto options = Init<epocanva_backend_session_options>();
  options.context = context;
  options.language = EPOCANVA_BACKEND_EN;
  options.input_mode = EPOCANVA_BACKEND_LATIN;
  options.learning_enabled = learning_enabled;
  epocanva_backend_session_handle session = 0;
  Check(epocanva_backend_session_open(backend, &options, &session) == EPOCANVA_OK &&
        session != 0, "English session opens");
  return session;
}
epocanva_backend_request Access(const epocanva_context& context,
                                std::uint32_t learning_enabled) {
  auto access = Init<epocanva_backend_request>();
  access.context = context;
  access.learning_enabled = learning_enabled;
  return access;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--force-failure") {
    Check(false, "intentional integration failure");
    return 1;
  }
  if (argc != 4) return 2;
  const std::string model_path = std::filesystem::absolute(std::filesystem::u8path(argv[1])).u8string();
  const std::string digest = ReadDigest(std::filesystem::u8path(argv[2]));
  const std::filesystem::path scratch =
      std::filesystem::absolute(std::filesystem::u8path(argv[3]));
  std::error_code error;
  std::filesystem::remove_all(scratch, error);
  std::filesystem::create_directories(scratch, error);
  const std::string learning_root = (scratch / L"learning").u8string();
  std::filesystem::create_directories(std::filesystem::u8path(learning_root), error);
  const epocanva_context context{77, 5, EPOCANVA_PRIVACY_NORMAL, 0};

  auto backend = OpenBackend(model_path, digest, learning_root);
  if (!backend) return 1;
  auto session = OpenSession(backend, context, 1);
  auto access = Access(context, 1);
  const Menu baseline = Query(session, access, "w");
  Check(!baseline.rows.empty() && baseline.rows.front().kind == EPOCANVA_BACKEND_RAW,
        "raw candidate is first");
  std::vector<const Row*> non_raw;
  for (const auto& row : baseline.rows)
    if (row.kind != EPOCANVA_BACKEND_RAW && !row.lexeme_ids.empty())
      non_raw.push_back(&row);
  if (non_raw.size() < 3) {
    std::cerr << "insufficient real candidates for the preference test: "
              << non_raw.size() << '\n';
    for (const auto& row : baseline.rows)
      std::cerr << "candidate=" << row.text << " kind=" << row.kind << '\n';
    return 1;
  }
  const Row& displaced = *non_raw[0];
  const Row& promoted = *non_raw[1];
  Check(IndexOf(baseline, promoted.text) > IndexOf(baseline, displaced.text),
        "baseline place has the promoted candidate below the displaced one");
  const std::string displaced_key = learn::CandidateKey(displaced.lexeme_ids);
  const std::string promoted_key = learn::CandidateKey(promoted.lexeme_ids);
  Check(!displaced_key.empty() && !promoted_key.empty(),
        "real candidates have stable learning identities");

  learn::Catalog catalog;
  catalog.name_space = "backend.en.legacy.v1";
  catalog.sha256 = digest;
  catalog.admitted_ids = displaced.lexeme_ids;
  catalog.admitted_ids.insert(catalog.admitted_ids.end(), promoted.lexeme_ids.begin(),
                              promoted.lexeme_ids.end());
  catalog.profile_id = "legacy";
  catalog.language = "en";
  learn::WindowsLearningRepository repository(learning_root);
  learn::Access write_access{learn::PrivacyClass::kNormal, true, 77, 5,
                             learn::CurrentTickMilliseconds()};
  const learn::PrivacyCallback allow = [write_access] { return write_access; };
  std::cerr << "seed displaced key=" << displaced_key << " text=" << displaced.text
            << " ids=" << displaced.lexeme_ids.size() << " promoted key="
            << promoted_key << " text=" << promoted.text << " ids="
            << promoted.lexeme_ids.size() << '\n';
  const learn::Status rejected = repository.RecordFeedback(catalog,
      learn::FeedbackKind::kRejected, displaced_key, "", displaced.lexeme_ids, true, 5,
      false, allow);
  if (rejected != learn::Status::kOk)
    std::cerr << "rejection status=" << learn::StatusName(rejected) << '\n';
  Check(rejected == learn::Status::kOk, "persisted explicit rejection");
  const learn::Status promoted_status = repository.RecordFeedback(catalog,
      learn::FeedbackKind::kManualWordAdded, promoted_key, promoted.text,
      promoted.lexeme_ids, false, 0, true, allow);
  if (promoted_status != learn::Status::kOk)
    std::cerr << "promotion status=" << learn::StatusName(promoted_status) << '\n';
  Check(promoted_status == learn::Status::kOk, "persisted manual preference");
  learn::Snapshot stored;
  Check(repository.LoadSnapshot(catalog, allow, &stored) == learn::Status::kOk &&
        stored.preferences.size() == 2, "both preferences persist");

  const Menu personalized = Query(session, access, "w");
  Check(IndexOf(personalized, promoted.text) < IndexOf(personalized, displaced.text),
        "persisted preference reorders the real completion menu");
  Check(!personalized.rows.empty() &&
        personalized.rows.front().kind == EPOCANVA_BACKEND_RAW,
        "personal ranking preserves the raw candidate first");
  Check(epocanva_backend_session_close(session) == EPOCANVA_OK, "session closes");
  Check(epocanva_backend_close(backend) == EPOCANVA_OK, "backend closes");

  // Restart the backend and open a session with recording paused. Existing preferences
  // must still affect ranking; only new writes are stopped.
  backend = OpenBackend(model_path, digest, learning_root);
  session = OpenSession(backend, context, 0);
  access = Access(context, 0);
  const Menu paused = Query(session, access, "w");
  Check(IndexOf(paused, promoted.text) < IndexOf(paused, displaced.text),
        "preferences survive a backend restart and apply while recording is paused");
  learn::Snapshot after_paused;
  Check(repository.LoadSnapshot(catalog, allow, &after_paused) == learn::Status::kOk &&
        after_paused.preferences.size() == 2,
        "paused query did not remove or add durable state");
  Check(repository.DeleteAll(true) == learn::Status::kOk,
        "explicit clear removes the local learning store");
  learn::Snapshot after_clear;
  Check(repository.LoadSnapshot(catalog, allow, &after_clear) == learn::Status::kOk &&
        after_clear.preferences.empty(), "clear leaves no durable preference");
  const Menu cleared = Query(session, access, "w");
  Check(IndexOf(cleared, displaced.text) < IndexOf(cleared, promoted.text),
        "clearing restores the default candidate order");
  Check(epocanva_backend_session_close(session) == EPOCANVA_OK, "paused session closes");
  Check(epocanva_backend_close(backend) == EPOCANVA_OK, "paused backend closes");
  std::filesystem::remove_all(scratch, error);
  if (failures != 0) return 1;
  std::cout << "RESULT PASS\n";
  return 0;
}
