// EpoCanva W1 -- product settings applied by the real English engine.
//
// Two deployments in non-ASCII, space-containing directories:
//   A: completion enabled (3), spelling suggestions enabled (2)
//   B: completion disabled, spelling suggestions disabled
// The same keys must produce different candidate lists, which is the evidence that the
// settings patch written by the product layer reaches librime. The test uses the same
// pinned rime.dll and the same executor as the rest of the Windows suite; it does not
// register a text service and does not touch the real user profile.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <variant>

#include "engine_executor/test_support.h"
#include "product_io.h"
#include "product_settings.h"

namespace {

using epocanva::tests::engine_executor::Check;
using epocanva::tests::engine_executor::MakeTextKey;
using epocanva::tests::engine_executor::RequireTextKey;
using epocanva::tests::engine_executor::SubmitCreateOrFail;
using epocanva::tests::engine_executor::SubmitDispatchOrFail;
using epocanva::tests::engine_executor::TakeOrFail;
using epocanva::tests::engine_executor::TextKey;
using epocanva::windows::engine::CandidateOutcome;
using epocanva::windows::engine::CommandResult;
using epocanva::windows::engine::CommandStatus;
using epocanva::windows::engine::ControllerOptions;
using epocanva::windows::engine::DeploymentOptions;
using epocanva::windows::engine::DispatchCommandOutcome;
using epocanva::windows::engine::DispatchOutcome;
using epocanva::windows::engine::DispatchStatus;
using epocanva::windows::engine::EngineExecutor;
using epocanva::windows::engine::EngineState;
using epocanva::windows::engine::ExecutorOptions;
using epocanva::windows::engine::ExecutorRuntimeReport;
using epocanva::windows::engine::RuntimeOptions;
using epocanva::windows::engine::SessionCreateStatus;
using epocanva::windows::engine::StartStatus;
using epocanva::windows::engine::StopStatus;
using epocanva::windows::input::InputPermission;

struct Arguments {
  std::filesystem::path rime_dll;
  std::filesystem::path data_dir;
  std::filesystem::path lexicon;
  std::filesystem::path run_root;
};

epocanva::windows::engine::RuntimeOptions MakeRuntime(const std::filesystem::path& rime_dll,
                                                      const std::filesystem::path& shared,
                                                      const std::filesystem::path& user,
                                                      const std::filesystem::path& log) {
  RuntimeOptions runtime;
  runtime.rime_dll_path = rime_dll;
  runtime.shared_data_dir = shared;
  runtime.user_data_dir = user;
  runtime.log_dir = log;
  DeploymentOptions deployment;
  deployment.config_file_name = "default.yaml";
  deployment.schema_files.push_back(shared / "epocanva_english.schema.yaml");
  runtime.deployment = deployment;
  return runtime;
}

bool CopyFile(const std::filesystem::path& from, const std::filesystem::path& to) {
  std::error_code ec;
  std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing,
                             ec);
  return !ec;
}

bool PrepareLayout(const Arguments& arguments, const std::filesystem::path& base,
                   const epocanva::product::ProductSettings& settings,
                   std::filesystem::path* shared, std::filesystem::path* user,
                   std::filesystem::path* log) {
  std::error_code ec;
  std::filesystem::remove_all(base, ec);
  *shared = base / L"共享 数据";
  *user = base / L"用户 数据";
  *log = base / L"日志";
  std::filesystem::create_directories(*shared / "lexicon", ec);
  std::filesystem::create_directories(*user, ec);
  std::filesystem::create_directories(*log, ec);
  if (ec) {
    Check(false, "cannot create the non-ASCII engine layout");
    return false;
  }
  bool ok = true;
  ok = CopyFile(arguments.data_dir / "default.yaml", *shared / "default.yaml") && ok;
  ok = CopyFile(arguments.data_dir / "epocanva_english.schema.yaml",
                *shared / "epocanva_english.schema.yaml") && ok;
  ok = CopyFile(arguments.lexicon, *shared / "lexicon" / "epocanva_en_alpha.tsv") && ok;
  if (!ok) {
    Check(false, "cannot copy the product data into the test layout");
    return false;
  }
  const std::string patch = epocanva::product::BuildSchemaPatch(settings);
  std::wstring why;
  if (!epocanva::product::WriteTextAtomically(*user / "epocanva_english.custom.yaml",
                                              patch, &why)) {
    Check(false, "cannot write the Rime user patch: " +
                     epocanva::product::WideToUtf8(why));
    return false;
  }
  return true;
}

bool HasCandidate(const EngineState& state, const std::string& text) {
  for (const auto& candidate : state.candidates) {
    if (epocanva::tests::engine_executor::AsciiOf(candidate.text_utf16_code_units) ==
        text) {
      return true;
    }
  }
  return false;
}

void DumpCandidates(const EngineState& state, const std::string& label) {
  std::cout << "  " << label << " raw="
            << epocanva::tests::engine_executor::AsciiOf(
                   state.raw_input.utf16_code_units)
            << " candidates=";
  for (const auto& candidate : state.candidates) {
    std::cout << "[" << epocanva::tests::engine_executor::AsciiOf(
                               candidate.text_utf16_code_units)
              << "]";
  }
  std::cout << "\n";
}

bool TypeText(EngineExecutor& executor, epocanva::windows::engine::ClientId client,
              const std::string& text, HKL layout, EngineState* last_state,
              const std::string& label) {
  for (const char character : text) {
    const TextKey key = MakeTextKey(static_cast<wchar_t>(character), layout);
    if (!RequireTextKey(key, character, label + " key " + std::string(1, character))) {
      return false;
    }
    const CommandResult result = TakeOrFail(
        executor,
        SubmitDispatchOrFail(executor, client, key.event, key.text,
                             label + " dispatch " + std::string(1, character)),
        60000, label + " dispatch result " + std::string(1, character));
    Check(result.status == CommandStatus::kOk, label + ": command result is kOk");
    const auto* payload = std::get_if<DispatchCommandOutcome>(&result.payload);
    Check(payload != nullptr, label + ": payload is a dispatch outcome");
    if (payload == nullptr) {
      return false;
    }
    const DispatchOutcome& outcome = payload->outcome;
    Check(outcome.status == DispatchStatus::kDispatchedHandled ||
              outcome.status == DispatchStatus::kDispatchedUnhandled,
          label + ": the engine processed the key",
          epocanva::windows::engine::DescribeDispatchStatus(outcome.status));
    if (outcome.state_status != epocanva::windows::engine::SessionStateStatus::kOk ||
        !outcome.state.valid) {
      Check(false, label + ": state is valid after the key");
      return false;
    }
    *last_state = outcome.state;
  }
  return true;
}

bool RunScenario(const Arguments& arguments, const std::filesystem::path& base,
                 const epocanva::product::ProductSettings& settings,
                 bool expect_completion, bool expect_spelling,
                 const std::string& label) {
  std::filesystem::path shared;
  std::filesystem::path user;
  std::filesystem::path log;
  if (!PrepareLayout(arguments, base, settings, &shared, &user, &log)) {
    return false;
  }
  ExecutorOptions options;
  options.runtime = MakeRuntime(arguments.rime_dll, shared, user, log);
  options.capacity = 32;
  EngineExecutor executor(options);
  const StartStatus started = executor.Start();
  Check(started == StartStatus::kOk, label + ": executor starts",
        epocanva::windows::engine::DescribeStartStatus(started));
  if (started != StartStatus::kOk) {
    return false;
  }

  const ControllerOptions controller{true, InputPermission::kAllowed};
  const HKL layout = ::GetKeyboardLayout(0);

  auto create_client = [&](epocanva::windows::engine::ClientId* client,
                           const std::string& what) {
    CommandResult created = TakeOrFail(
        executor,
        SubmitCreateOrFail(executor, "epocanva_english", controller, client,
                           what + " create"),
        60000, what + " create result");
    Check(created.status == CommandStatus::kOk, what + ": create command is kOk");
    const auto* payload =
        std::get_if<epocanva::windows::engine::CreateClientOutcome>(&created.payload);
    Check(payload != nullptr && payload->client.valid(),
          what + ": create returned a client");
    return payload != nullptr && payload->client.valid();
  };
  auto close_client = [&](epocanva::windows::engine::ClientId client,
                          const std::string& what) {
    epocanva::windows::engine::RequestId request;
    const auto submitted = executor.TryCloseClient(client, &request);
    Check(submitted == epocanva::windows::engine::SubmitStatus::kAccepted,
          what + ": close accepted");
    if (submitted == epocanva::windows::engine::SubmitStatus::kAccepted) {
      CommandResult closed = TakeOrFail(executor, request, 60000, what + " close result");
      Check(closed.status == CommandStatus::kOk, what + ": close command is kOk");
    }
  };

  epocanva::windows::engine::ClientId completion_client;
  if (!create_client(&completion_client, label + " completion client")) {
    executor.StopAndJoin();
    return false;
  }
  EngineState completion_state;
  if (!TypeText(executor, completion_client, "hel", layout, &completion_state,
                label + " hel")) {
    executor.StopAndJoin();
    return false;
  }
  Check(!completion_state.candidates.empty(), label + ": hel has a menu");
  if (!completion_state.candidates.empty()) {
    Check(epocanva::tests::engine_executor::AsciiOf(
              completion_state.candidates.front().text_utf16_code_units) == "hel",
          label + ": candidate 0 is the raw input");
  }
  if (expect_completion) {
    if (completion_state.candidates.size() <= 1) {
      DumpCandidates(completion_state, label + " hel");
    }
    Check(completion_state.candidates.size() > 1,
          label + ": completion on adds candidates");
    Check(completion_state.candidates.size() <= 8,
          label + ": the eight-candidate ceiling holds");
  } else {
    Check(completion_state.candidates.size() == 1,
          label + ": completion off leaves only the raw candidate");
  }
  close_client(completion_client, label + " completion client");

  epocanva::windows::engine::ClientId typo_client;
  if (!create_client(&typo_client, label + " typo client")) {
    executor.StopAndJoin();
    return false;
  }
  EngineState typo_state;
  // "hellp" has no lexicon prefix (completion cannot explain a second candidate);
  // it is one substitution away from "hello", so a spelling suggestion can.
  if (!TypeText(executor, typo_client, "hellp", layout, &typo_state,
                label + " hellp")) {
    executor.StopAndJoin();
    return false;
  }
  if (expect_spelling) {
    if (typo_state.candidates.size() <= 1) {
      DumpCandidates(typo_state, label + " hellp");
    }
    Check(typo_state.candidates.size() > 1,
          label + ": spelling on adds a suggestion");
    Check(typo_state.candidates.size() <= 4,
          label + ": the suggestion ceiling holds");
    Check(!typo_state.candidates.empty() &&
              epocanva::tests::engine_executor::AsciiOf(
                  typo_state.candidates.front().text_utf16_code_units) == "hellp",
          label + ": candidate 0 is the typo itself");
  } else {
    Check(typo_state.candidates.size() == 1,
          label + ": spelling off leaves only the raw candidate");
  }
  close_client(typo_client, label + " typo client");

  const StopStatus stopped = executor.StopAndJoin();
  Check(stopped == StopStatus::kOk, label + ": executor stops cleanly");
  const ExecutorRuntimeReport report = executor.RuntimeReport();
  Check(report.live_sessions_after_cleanup == 0,
        label + ": no live session after cleanup");
  epocanva::tests::engine_executor::CheckBalance(report.api_counts, label);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string option = argv[index];
    if (option == "--rime-dll" && index + 1 < argc) {
      arguments.rime_dll = argv[++index];
    } else if (option == "--data-dir" && index + 1 < argc) {
      arguments.data_dir = argv[++index];
    } else if (option == "--lexicon" && index + 1 < argc) {
      arguments.lexicon = argv[++index];
    } else if (option == "--run-root" && index + 1 < argc) {
      arguments.run_root = argv[++index];
    }
  }
  if (arguments.rime_dll.empty() || arguments.data_dir.empty() ||
      arguments.lexicon.empty() || arguments.run_root.empty()) {
    std::cout << "FAIL: --rime-dll, --data-dir, --lexicon and --run-root are required\n";
    return 2;
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(arguments.rime_dll, ec) ||
      !std::filesystem::is_regular_file(arguments.data_dir / "default.yaml", ec) ||
      !std::filesystem::is_regular_file(arguments.data_dir /
                                        "epocanva_english.schema.yaml", ec) ||
      !std::filesystem::is_regular_file(arguments.lexicon, ec)) {
    std::cout << "FAIL: engine or product data is missing\n";
    return 2;
  }
  std::filesystem::create_directories(arguments.run_root, ec);
  if (ec) {
    std::cout << "FAIL: cannot create the run root\n";
    return 2;
  }

  epocanva::product::ProductSettings completion_only;
  completion_only.completion_enabled = true;
  completion_only.completion_candidates = 7;
  completion_only.spelling_enabled = false;
  completion_only.spelling_candidates = 3;
  epocanva::product::ProductSettings spelling_only;
  spelling_only.completion_enabled = false;
  spelling_only.completion_candidates = 5;
  spelling_only.spelling_enabled = true;
  spelling_only.spelling_candidates = 3;
  epocanva::product::ProductSettings disabled;
  disabled.completion_enabled = false;
  disabled.completion_candidates = 5;
  disabled.spelling_enabled = false;
  disabled.spelling_candidates = 3;

  const bool completion_ok = RunScenario(arguments, arguments.run_root / L"设置 完成",
                                         completion_only, true, false,
                                         "completion-only");
  Check(completion_ok, "completion-only scenario completed");
  const bool spelling_ok = RunScenario(arguments, arguments.run_root / L"设置 拼写",
                                       spelling_only, false, true, "spelling-only");
  Check(spelling_ok, "spelling-only scenario completed");
  const bool disabled_ok = RunScenario(arguments, arguments.run_root / L"设置 关闭",
                                       disabled, false, false, "settings-off");
  Check(disabled_ok, "settings-off scenario completed");

  if (epocanva::tests::engine_executor::g_failed != 0) {
    std::cout << "FAILED: " << epocanva::tests::engine_executor::g_failed
              << " check(s)\n";
    return 14;
  }
  std::cout << "RESULT PASS\n";
  return 0;
}
