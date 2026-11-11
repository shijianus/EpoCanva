// Real model -> executor -> coordinator -> real TSF TextStore. No registration.
#include <windows.h>
#include <inputscope.h>
#include <filesystem>
#include <iostream>
#include <thread>
#include "engine_executor.h"
#include "coordinator.h"
#include "host_context.h"
#include "product_backend_resources.h"
#include "product_settings.h"
#include "test_host.h"
#include "tsf_coordinator/coordinator_test_support.h"
namespace engine = epocanva::windows::engine;
namespace coord = epocanva::tsf::coordinator;
namespace ctx = epocanva::tsf::context;
namespace edit = epocanva::tsf::edit;
namespace product = epocanva::product;
namespace {
int failures = 0;
void Check(bool ok, const char* name) { std::cout << (ok ? "PASS " : "FAIL ") << name << '\n'; if (!ok) ++failures; }
engine::CommandResult Take(engine::EngineExecutor& executor, engine::RequestId id) {
  Check(executor.WaitForResult(id, std::chrono::seconds(30)) == engine::WaitStatus::kReady, "executor result ready");
  engine::CommandResult result;
  Check(executor.TakeResult(id, &result) == engine::TakeStatus::kTaken, "executor result consumed once");
  return result;
}
bool Drive(coord::ContextCoordinator& coordinator, edit::test::TestHost& host) {
  const auto deadline = ::GetTickCount64() + 30000;
  do {
    coordinator.Pump(); host.DispatchQueuedMessages();
    if (coordinator.Snapshot().phase == coord::WorkPhase::kIdle) return true;
    ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 1, QS_ALLINPUT);
  } while (::GetTickCount64() < deadline);
  return false;
}
}
int main(int argc, char** argv) {
  if (argc != 4) return 2;
  try {
    product::ProductPaths paths;
    paths.product_root = std::filesystem::absolute(std::filesystem::u8path(argv[1]));
    paths.data_dir = paths.product_root / L"data";
    engine::ExecutorOptions options;
    std::wstring why;
    Check(product::LoadBackendResources(paths, &options.backend, &why), "real resource manifest loads");
    if (!options.backend.enabled) return 1;
    options.backend.max_candidates = 32;
    options.backend.completion_enabled = options.backend.spelling_enabled =
        options.backend.morphology_enabled = options.backend.abbreviation_enabled = true;
    const auto run = std::filesystem::absolute(std::filesystem::u8path(argv[3]));
    std::filesystem::create_directories(run / L"user");
    std::filesystem::create_directories(run / L"logs");
    // Existing compiled Chinese artifact is explicitly supplied by the test build; no source
    // directory or real user directory is modified by configure/build/test.
    options.runtime.rime_dll_path = std::filesystem::absolute(std::filesystem::u8path(argv[2]));
    std::filesystem::create_directories(run / L"shared");
    for (const auto& file : std::filesystem::directory_iterator(paths.data_dir))
      if (file.is_regular_file()) std::filesystem::copy_file(file.path(), run / L"shared" / file.path().filename(),
          std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(std::filesystem::path(EPOCANVA_TEST_REPO_ROOT).lexically_normal() / L"data/product/default.yaml",
        run / L"shared/default.yaml", std::filesystem::copy_options::overwrite_existing);
    options.runtime.shared_data_dir = run / L"shared";
    options.runtime.user_data_dir = run / L"user";
    options.runtime.log_dir = run / L"logs";
    engine::DeploymentOptions deployment;
    deployment.config_file_name = "default.yaml";
    deployment.schema_files = {options.runtime.shared_data_dir / L"epocanva_backend_pinyin.schema.yaml",
                               options.runtime.shared_data_dir / L"epocanva_backend_zhuyin.schema.yaml"};
    options.runtime.deployment = deployment;
    engine::EngineExecutor executor(options);
    const auto start_status = executor.Start();
    const auto runtime_report = executor.RuntimeReport();
    Check(start_status == engine::StartStatus::kOk, "unified executor starts with real models");
    if (start_status != engine::StartStatus::kOk) {
      std::cout << "runtime_start_status=" << static_cast<int>(runtime_report.runtime_start_status)
                << " executor_state=" << static_cast<int>(runtime_report.state) << "\n";
      return 1;
    }
    std::string error;
    auto host = edit::test::TestHost::Create(&error);
    Check(host != nullptr, "real TSF test host created"); if (!host) return 1;
    struct Case { std::uint32_t profile; const char* raw; const char16_t* expected; };
    // Profiles 11-13 are the same English variant on QWERTZ, Dvorak and AZERTY. A correct
    // prefix must produce the same completion; the layout affects misspelling cost only.
    const Case cases[] = {{1,"hel",u"hello"},{11,"hel",u"hello"},{12,"hel",u"hello"},
                          {13,"hel",u"hello"},{2,"nihao",u"你好"},{2,"nh",u"你好"},{3,"francais",u"français"},
                          {3,u8"françai",u"français"},
                          {4,"nihon",u"日本"},{5,"dkssud",u"안녕"},{6,"su3cl3",u"你好"},
                          {7,"nihon",u"にほん"},{8,"nihon",u"ニホン"},{9,u8"にほん",u"日本"},
                          {10,u8"안녕",u"안녕"}};
    for (const auto& item : cases) {
      const auto raw = engine::CaptureEngineText(item.raw).text.utf16_code_units;
      edit::test::TestDocumentOptions doc_options;
      doc_options.initial_text_utf16.assign(raw.begin(), raw.end());
      doc_options.initial_selection_start_utf16 = doc_options.initial_selection_end_utf16 = static_cast<LONG>(raw.size());
      auto* document = host->CreateDocument(doc_options, &error);
      Check(document != nullptr, "language document created"); if (!document) continue;
      coord::test::TestTargetRevalidator target;
      auto permission = ctx::CreateRealPermissionSource(host->client_id());
      coord::ContextCoordinator coordinator(executor, host->client_id(), *permission);
      coordinator.AttachTargetRevalidator(&target); coordinator.SetSuggestionMode(true);
      engine::BackendAccess access{document->context_id(), 1, std::make_shared<std::atomic<bool>>(true)};
      engine::RequestId request; engine::ClientId client;
      Check(executor.TryCreateProfileClient(item.profile, access, &request, &client) == engine::SubmitStatus::kAccepted,
            "profile client admitted");
      Check(Take(executor, request).ok(), "profile client created");
      Check(coordinator.BindObserved(document->context(), access.context_target, access.generation,
          static_cast<LONG>(raw.size()), client) == coord::BindStatus::kOk, "real document bound");
      coordinator.SetBackendAccess(access); coordinator.RefreshPermission(); Drive(coordinator, *host);
      const auto captured = epocanva::tsf::product::ReadHostContext(document->context(), host->client_id(),
          static_cast<LONG>(raw.size()), target.CreateWriteGuard(), access.context_target, access.generation);
      Check(captured.valid && captured.token_utf8 == item.raw, "bounded host text exact including Unicode");
      engine::BackendWork work; work.operation = engine::BackendOperation::kObserveHostText;
      work.text_utf8 = item.raw; work.event_token = 1;
      coord::WorkId id;
      Check(coordinator.SubmitBackendWork(work, &id) == coord::SubmitStatus::kAccepted, "host-confirmed input submitted");
      Check(Drive(coordinator, *host), "real candidate query completes");
      const auto menu = coordinator.candidate_snapshot();
      std::size_t ordinal = menu.items.size();
      for (std::size_t i = 0; i < menu.items.size(); ++i)
        if (menu.items[i].text_utf16_code_units == item.expected && menu.items[i].kind != coord::CandidateKind::kLiteral) ordinal = i;
      if (ordinal == menu.items.size()) std::cout << "profile=" << item.profile << " candidates=" << menu.items.size()
          << " outcome=" << static_cast<int>(coordinator.Snapshot().outcome) << '\n';
      Check(ordinal != menu.items.size(), "real language candidate exists");
      Check(document->text_utf16() == doc_options.initial_text_utf16, "suggestions preserve host text");
      if (ordinal != menu.items.size()) {
        coord::CandidateSelectionRequest selection{menu.version, access.context_target, access.generation,
                                                   ordinal, static_cast<LONG>(raw.size())};
        Check(coordinator.SubmitSuggestionReplacement(selection, &id) == coord::SubmitStatus::kAccepted,
              "explicit selection uses backend token");
        Check(Drive(coordinator, *host), "selection edit and ACK complete");
        Check(coordinator.Snapshot().outcome == coord::WorkOutcome::kApplied, "only TSF write success is Applied");
        const std::u16string expected(item.expected);
        Check(document->text_utf16() == std::wstring(expected.begin(), expected.end()), "real committed text replaces exactly once");
        Check(coordinator.SubmitSuggestionReplacement(selection, &id) == coord::SubmitStatus::kCandidateStale,
              "old selection cannot replay commit");
      }
      coordinator.InvalidateBinding();
      const auto deadline = ::GetTickCount64() + 5000;
      while (coordinator.Snapshot().retirement.retiring_bindings && ::GetTickCount64() < deadline) {
        coordinator.Pump(); host->DispatchQueuedMessages(); std::this_thread::yield();
      }
      coordinator.Close();
    }
    // Committed Han text followed by the Latin pinyin/romaji the user is typing must split
    // at the script boundary: "你好shi" is context "你好" plus the current token "shi".
    edit::test::TestDocumentOptions mixed;
    mixed.initial_text_utf16 = L"你好shi";
    mixed.initial_selection_start_utf16 = mixed.initial_selection_end_utf16 =
        static_cast<LONG>(mixed.initial_text_utf16.size());
    auto* mixed_document = host->CreateDocument(mixed, &error);
    Check(mixed_document != nullptr, "mixed-script document created");
    if (mixed_document != nullptr) {
      coord::test::TestTargetRevalidator mixed_target;
      const auto captured = epocanva::tsf::product::ReadHostContext(
          mixed_document->context(), host->client_id(),
          static_cast<LONG>(mixed.initial_text_utf16.size()),
          mixed_target.CreateWriteGuard(), mixed_document->context_id(), 1);
      Check(captured.valid && captured.token_utf8 == "shi" &&
                captured.preceding_utf8 == u8"你好" &&
                captured.token_start_utf16 == 2,
            "committed CJK context and the latin input token split at the script boundary");
    }
    // The apostrophe the English core admits inside "don't" must not end the token:
    // otherwise the contraction is split into context plus "t" and cannot be completed.
    edit::test::TestDocumentOptions contraction;
    contraction.initial_text_utf16 = L"I don't";
    contraction.initial_selection_start_utf16 = contraction.initial_selection_end_utf16 =
        static_cast<LONG>(contraction.initial_text_utf16.size());
    auto* contraction_document = host->CreateDocument(contraction, &error);
    Check(contraction_document != nullptr, "contraction document created");
    if (contraction_document != nullptr) {
      coord::test::TestTargetRevalidator contraction_target;
      const auto captured = epocanva::tsf::product::ReadHostContext(
          contraction_document->context(), host->client_id(),
          static_cast<LONG>(contraction.initial_text_utf16.size()),
          contraction_target.CreateWriteGuard(), contraction_document->context_id(), 1);
      Check(captured.valid && captured.token_utf8 == "don't" &&
                captured.preceding_utf8 == "I " &&
                captured.token_start_utf16 == 2,
            "internal apostrophe stays inside the current english token");
    }
    // The reader may not acquire sensitive text even though the caller supplies an allowed
    // target guard. Field metadata is independently authoritative under the read cookie.
    edit::test::TestDocumentOptions secret; secret.initial_text_utf16 = L"never-read";
    secret.initial_selection_start_utf16 = secret.initial_selection_end_utf16 = 10;
    secret.input_scopes = {IS_PASSWORD};
    auto* sensitive = host->CreateDocument(secret, &error);
    coord::test::TestTargetRevalidator target;
    auto read = epocanva::tsf::product::ReadHostContext(sensitive->context(), host->client_id(), 10,
        target.CreateWriteGuard(), sensitive->context_id(), 1);
    Check(!read.valid && read.token_utf8.empty() && read.preceding_utf8.empty(), "password host context refused without text");
    host->Shutdown();
    Check(executor.StopAndJoin() == engine::StopStatus::kOk, "unified backend sessions and worker close");
  } catch (...) { Check(false, "unexpected exception"); }
  std::cout << (failures ? "RESULT FAIL" : "RESULT PASS") << " unified TSF backend failures=" << failures << '\n';
  return failures ? 1 : 0;
}
