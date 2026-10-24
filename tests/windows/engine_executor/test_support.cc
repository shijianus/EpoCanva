// EpoCanva P3.5 -- shared test scaffolding implementation. See test_support.h.
#include "test_support.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <system_error>
#include <vector>

#include "key_text_translation.h"
#include "keyboard_state.h"

namespace epocanva {
namespace tests {
namespace engine_executor {

using epocanva::windows::engine::ApiCallCounts;
using epocanva::windows::engine::ClientId;
using epocanva::windows::engine::CommandResult;
using epocanva::windows::engine::ControllerOptions;
using epocanva::windows::engine::DescribeSubmitStatus;
using epocanva::windows::engine::DescribeTakeStatus;
using epocanva::windows::engine::DescribeWaitStatus;
using epocanva::windows::engine::EngineExecutor;
using epocanva::windows::engine::RequestId;
using epocanva::windows::engine::RuntimeOptions;
using epocanva::windows::engine::SubmitStatus;
using epocanva::windows::engine::TakeStatus;
using epocanva::windows::engine::WaitStatus;
using epocanva::windows::input::KeyEventSnapshot;
using epocanva::windows::input::KeyText;
using epocanva::windows::input::KeyTransition;
using epocanva::windows::input::KeyboardStateSnapshot;
using epocanva::windows::input::KeyTranslationResult;
using epocanva::windows::input::TranslateKeyEvent;

int g_total = 0;
int g_failed = 0;

void Check(bool condition, const std::string& what, const std::string& got) {
  ++g_total;
  if (condition) {
    std::printf("[executor-win] ok   %s\n", what.c_str());
    return;
  }
  ++g_failed;
  if (got.empty()) {
    std::printf("[executor-win] FAIL %s\n", what.c_str());
  } else {
    std::printf("[executor-win] FAIL %s  (got: %s)\n", what.c_str(), got.c_str());
  }
}

std::string AsciiOf(const std::u16string& text) {
  std::string out;
  for (const char16_t unit : text) {
    out.push_back(unit >= 0x20 && unit < 0x7F ? static_cast<char>(unit) : '?');
  }
  return out;
}

std::string AsciiOf(const std::wstring& text) {
  std::string out;
  for (const wchar_t unit : text) {
    out.push_back(unit >= 0x20 && unit < 0x7F ? static_cast<char>(unit) : '?');
  }
  return out;
}

std::u16string U16(const std::string& ascii) {
  return std::u16string(ascii.begin(), ascii.end());
}

void CheckText(const std::u16string& actual, const std::string& expected,
               const std::string& what) {
  Check(actual == U16(expected), what, AsciiOf(actual));
}

std::string CountText(std::uint64_t value) {
  return std::to_string(static_cast<unsigned long long>(value));
}

std::string DescribeCounts(const ApiCallCounts& counts) {
  return "status " + CountText(counts.status_acquired) + "/" +
         CountText(counts.status_freed) + " context " +
         CountText(counts.context_acquired) + "/" + CountText(counts.context_freed) +
         " commit " + CountText(counts.commit_acquired) + "/" +
         CountText(counts.commit_freed) + " config " +
         CountText(counts.schema_config_acquired) + "/" +
         CountText(counts.schema_config_closed) + " session " +
         CountText(counts.create_session_calls) + "/" +
         CountText(counts.destroy_session_calls) + " key " +
         CountText(counts.process_key_calls) + " select " +
         CountText(counts.select_candidate_calls) + " deploy " +
         CountText(counts.deploy_calls);
}

bool PrepareRunRoot(const Options& options, const char* leaf,
                    std::filesystem::path* shared_dir,
                    std::filesystem::path* user_dir,
                    std::filesystem::path* log_dir) {
  if (options.run_root.filename() != "engine-executor-run") {
    std::printf(
        "[executor-win] FAIL run-root: the leaf must be named engine-executor-run\n");
    return false;
  }
  std::error_code ec;
  if (std::filesystem::exists(options.run_root, ec)) {
    std::filesystem::remove_all(options.run_root, ec);
    if (ec) {
      std::printf("[executor-win] FAIL run-root: cannot remove %s (%s)\n",
                  options.run_root.string().c_str(), ec.message().c_str());
      return false;
    }
  }
  *shared_dir = options.run_root / "shared";
  *user_dir = options.run_root / "user";
  *log_dir = options.run_root / "log";
  for (const std::filesystem::path& dir : {*shared_dir, *user_dir, *log_dir}) {
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      std::printf("[executor-win] FAIL run-root: cannot create %s (%s)\n",
                  dir.string().c_str(), ec.message().c_str());
      return false;
    }
  }
  std::printf("[executor-win] run-root %s (recreated empty, case %s)\n",
              options.run_root.string().c_str(), leaf);
  return true;
}

namespace {

bool CopyInto(const std::filesystem::path& source, const std::filesystem::path& dest) {
  std::error_code ec;
  std::filesystem::create_directories(dest.parent_path(), ec);
  if (ec) {
    std::printf("[executor-win] FAIL fixture: cannot create %s\n",
                dest.parent_path().string().c_str());
    return false;
  }
  std::filesystem::copy_file(source, dest,
                             std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    std::printf("[executor-win] FAIL fixture: %s -> %s (%s)\n",
                source.string().c_str(), dest.string().c_str(), ec.message().c_str());
    return false;
  }
  return true;
}

}  // namespace

bool StageFixtures(const Options& options, const std::filesystem::path& shared_dir) {
  const std::filesystem::path completion = options.fixture_dir;
  const std::filesystem::path spelling = options.spelling_fixture_dir;
  if (!CopyInto(completion / "default.yaml", shared_dir / "default.yaml") ||
      !CopyInto(completion / "epocanva_test_english.schema.yaml",
                shared_dir / "epocanva_test_english.schema.yaml") ||
      !CopyInto(completion / "lexicon.tsv",
                shared_dir / "lexicon" / "epocanva_test_lexicon.tsv") ||
      !CopyInto(spelling / "epocanva_spell_english.schema.yaml",
                shared_dir / "epocanva_spell_english.schema.yaml") ||
      !CopyInto(spelling / "lexicon_spell.tsv",
                shared_dir / "lexicon" / "epocanva_spell_lexicon.tsv")) {
    return false;
  }
  std::printf("[executor-win] fixtures staged from the fixed completion and spelling fixtures\n");
  return true;
}

RuntimeOptions BaseOptions(const Options& options, const std::filesystem::path& shared_dir,
                           const std::filesystem::path& user_dir,
                           const std::filesystem::path& log_dir) {
  RuntimeOptions runtime;
  runtime.rime_dll_path = options.rime_dll;
  runtime.shared_data_dir = shared_dir;
  runtime.user_data_dir = user_dir;
  runtime.log_dir = log_dir;
  return runtime;
}

bool PrepareDeployment(const Options& options, const std::filesystem::path& shared_dir,
                       const std::filesystem::path& user_dir,
                       const std::filesystem::path& log_dir) {
  using epocanva::windows::engine::DeploymentOptions;
  using epocanva::windows::engine::DescribeRuntimeShutdownStatus;
  using epocanva::windows::engine::DescribeRuntimeStartStatus;
  using epocanva::windows::engine::RimeRuntime;
  using epocanva::windows::engine::RuntimeInfo;
  using epocanva::windows::engine::RuntimeShutdownStatus;
  using epocanva::windows::engine::RuntimeStartStatus;

  RuntimeOptions runtime = BaseOptions(options, shared_dir, user_dir, log_dir);
  DeploymentOptions deployment;
  deployment.config_file_name = "default.yaml";
  deployment.schema_files.push_back(shared_dir / "epocanva_test_english.schema.yaml");
  deployment.schema_files.push_back(shared_dir / "epocanva_spell_english.schema.yaml");
  runtime.deployment = deployment;

  RimeRuntime preparation;
  const RuntimeStartStatus started = preparation.Start(runtime);
  Check(started == RuntimeStartStatus::kOk,
        "fixture deployment runtime starts", DescribeRuntimeStartStatus(started));
  if (started != RuntimeStartStatus::kOk) {
    std::printf("[executor-win] preparation diagnostic: %s\n",
                preparation.Info().last_start_diagnostic.c_str());
    return false;
  }
  const RuntimeInfo info = preparation.Info();
  Check(info.deploy_calls == 3, "preparation deployed default + two schemas",
        CountText(info.deploy_calls));
  const std::filesystem::path build_dir = user_dir / "build";
  Check(std::filesystem::is_regular_file(build_dir / "default.yaml"),
        "compiled default.yaml exists in the prepared user directory");
  Check(std::filesystem::is_regular_file(build_dir / "epocanva_test_english.schema.yaml"),
        "compiled completion schema exists in the prepared user directory");
  Check(std::filesystem::is_regular_file(build_dir / "epocanva_spell_english.schema.yaml"),
        "compiled spelling schema exists in the prepared user directory");

  const RuntimeShutdownStatus stopped = preparation.Shutdown();
  Check(stopped == RuntimeShutdownStatus::kOk, "preparation runtime shuts down",
        DescribeRuntimeShutdownStatus(stopped));
  Check(preparation.Info().finalize_calls == 1,
        "preparation called finalize exactly once",
        CountText(preparation.Info().finalize_calls));
  CheckBalance(preparation.ApiCounts(), "preparation");
  return g_failed == 0;
}

void CheckBalance(const ApiCallCounts& counts, const std::string& label) {
  Check(counts.balanced(), label + ": API acquire/free pairing", DescribeCounts(counts));
  Check(counts.create_session_calls == counts.destroy_session_calls,
        label + ": create_session/destroy_session pairing",
        CountText(counts.create_session_calls) + " vs " +
            CountText(counts.destroy_session_calls));
}

KeyEventSnapshot MakeKeyEvent(std::uint32_t virtual_key, bool shift, bool control,
                              bool alt, bool windows, KeyTransition transition) {
  KeyEventSnapshot event;
  event.virtual_key = virtual_key;
  event.scan_code = ::MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC);
  event.transition = transition;
  event.repeat_count = 1;
  event.previously_down = false;
  event.modifiers.shift = shift;
  event.modifiers.control = control;
  event.modifiers.alt = alt;
  event.modifiers.windows = windows;
  return event;
}

TextKey MakeTextKey(wchar_t character, HKL layout) {
  TextKey out;
  const SHORT scan = ::VkKeyScanExW(character, layout);
  if (scan == -1) {
    out.diagnostic = "the current layout cannot type this fixture character";
    return out;
  }
  const BYTE virtual_key = static_cast<BYTE>(scan & 0xFF);
  const BYTE shift_state = static_cast<BYTE>((scan >> 8) & 0xFF);
  if ((shift_state & 0x06u) != 0) {
    out.diagnostic = "the fixture character needs Ctrl/Alt on this layout";
    return out;
  }
  std::array<std::uint8_t, epocanva::windows::input::kKeyboardStateByteCount> bytes{};
  if ((shift_state & 0x01u) != 0) {
    bytes[VK_SHIFT] = 0x80;
  }
  const KeyboardStateSnapshot snapshot =
      KeyboardStateSnapshot::FromExplicitState(bytes, layout);

  out.event.virtual_key = virtual_key;
  out.event.scan_code = ::MapVirtualKeyExW(virtual_key, MAPVK_VK_TO_VSC, layout);
  out.event.is_extended = false;
  out.event.transition = KeyTransition::kKeyDown;
  out.event.repeat_count = 1;
  out.event.previously_down = false;
  out.event.modifiers = snapshot.modifiers();

  const KeyTranslationResult result = TranslateKeyEvent(out.event, snapshot);
  out.text = result.text;
  out.ok = result.ok_for_text();
  out.diagnostic = result.diagnostic;
  return out;
}

bool RequireTextKey(const TextKey& key, char expected, const std::string& label) {
  Check(key.ok, label + ": real ToUnicodeEx produced text", key.diagnostic);
  if (!key.ok) {
    return false;
  }
  CheckText(key.text.utf16_code_units, std::string(1, expected),
            label + ": translated character is exact");
  return true;
}

CommandResult TakeOrFail(EngineExecutor& executor, RequestId request, int timeout_ms,
                         const std::string& label) {
  CommandResult result;
  const WaitStatus waited =
      executor.WaitForResult(request, std::chrono::milliseconds(timeout_ms));
  Check(waited == WaitStatus::kReady, label + ": result is ready",
        DescribeWaitStatus(waited));
  if (waited != WaitStatus::kReady) {
    return result;
  }
  const TakeStatus taken = executor.TakeResult(request, &result);
  Check(taken == TakeStatus::kTaken, label + ": result is taken",
        DescribeTakeStatus(taken));
  if (taken == TakeStatus::kTaken) {
    Check(result.request == request, label + ": result carries the requested id");
  }
  return result;
}

RequestId SubmitCreateOrFail(EngineExecutor& executor, const std::string& schema_id,
                             const ControllerOptions& options, ClientId* client,
                             const std::string& label) {
  RequestId request;
  const SubmitStatus submitted =
      executor.TryCreateClient(schema_id, options, &request, client);
  Check(submitted == SubmitStatus::kAccepted, label + ": create accepted",
        DescribeSubmitStatus(submitted));
  return submitted == SubmitStatus::kAccepted ? request : RequestId{};
}

RequestId SubmitEvaluateOrFail(EngineExecutor& executor, ClientId client,
                               const KeyEventSnapshot& event, const KeyText& text,
                               const std::string& label) {
  RequestId request;
  const SubmitStatus submitted = executor.TryEvaluate(client, event, text, &request);
  Check(submitted == SubmitStatus::kAccepted, label + ": evaluate accepted",
        DescribeSubmitStatus(submitted));
  return submitted == SubmitStatus::kAccepted ? request : RequestId{};
}

RequestId SubmitDispatchOrFail(EngineExecutor& executor, ClientId client,
                               const KeyEventSnapshot& event, const KeyText& text,
                               const std::string& label) {
  RequestId request;
  const SubmitStatus submitted = executor.TryDispatch(client, event, text, &request);
  Check(submitted == SubmitStatus::kAccepted, label + ": dispatch accepted",
        DescribeSubmitStatus(submitted));
  return submitted == SubmitStatus::kAccepted ? request : RequestId{};
}

RequestId SubmitAcceptOrFail(EngineExecutor& executor, ClientId client,
                             std::size_t candidate_index, const std::string& label) {
  RequestId request;
  const SubmitStatus submitted =
      executor.TryAcceptCandidate(client, candidate_index, &request);
  Check(submitted == SubmitStatus::kAccepted, label + ": candidate acceptance accepted",
        DescribeSubmitStatus(submitted));
  return submitted == SubmitStatus::kAccepted ? request : RequestId{};
}

RequestId SubmitRefreshOrFail(EngineExecutor& executor, ClientId client,
                              const std::string& label) {
  RequestId request;
  const SubmitStatus submitted = executor.TryRefreshState(client, &request);
  Check(submitted == SubmitStatus::kAccepted, label + ": refresh accepted",
        DescribeSubmitStatus(submitted));
  return submitted == SubmitStatus::kAccepted ? request : RequestId{};
}

}  // namespace engine_executor
}  // namespace tests
}  // namespace epocanva
