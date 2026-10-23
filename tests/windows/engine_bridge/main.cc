// EpoCanva P3.4 -- runtime/session verification program plus the mode dispatch. The controller
// cases live in controller_tests.cc; both files link the same bridge and share test_support.h.
//
// WHAT THIS JUDGES. The DLL that is actually loaded, the public slots it must provide, the
// explicit base and English modules, one process-global initialize, independent sessions,
// thread ownership, shutdown ordering, failure cleanup, the prepared-directory separation,
// and the caller-owned UTF-16 copies / offset statuses. Every string printed comes from the
// fixed fixtures in tests/fixtures/ -- no live user input exists in this program.
//
// EXIT CODES: 0 pass; 2 usage; 5 a check failed; 14 the self-check-fail mode's expected
// failure. RESULT PASS is printed only when nothing failed.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "engine_update.h"
#include "rime_runtime.h"
#include "rime_session.h"
#include "text_boundary_map.h"
#include "test_support.h"
#include "text_encoding.h"

namespace epocanva {
namespace tests {
namespace engine_bridge {

using epocanva::windows::engine::ApiCallCounts;
using epocanva::windows::engine::CaptureEngineText;
using epocanva::windows::engine::CursorOffsetStatus;
using epocanva::windows::engine::DescribeCursorOffsetStatus;
using epocanva::windows::engine::DescribeRuntimeShutdownStatus;
using epocanva::windows::engine::DescribeRuntimeStartStatus;
using epocanva::windows::engine::DescribeSelectionOffsetStatus;
using epocanva::windows::engine::DescribeSessionCloseStatus;
using epocanva::windows::engine::DescribeSessionCommitStatus;
using epocanva::windows::engine::DescribeSessionCreateStatus;
using epocanva::windows::engine::DescribeSessionKeyStatus;
using epocanva::windows::engine::DescribeSessionStateStatus;
using epocanva::windows::engine::EngineTextCapture;
using epocanva::windows::engine::MapPreeditOffsets;
using epocanva::windows::engine::PreeditOffsetMapping;
using epocanva::windows::engine::RimeRuntime;
using epocanva::windows::engine::RimeSession;
using epocanva::windows::engine::RuntimeInfo;
using epocanva::windows::engine::RuntimeOptions;
using epocanva::windows::engine::RuntimeShutdownStatus;
using epocanva::windows::engine::RuntimeStartStatus;
using epocanva::windows::engine::SelectionOffsetStatus;
using epocanva::windows::engine::SessionCloseStatus;
using epocanva::windows::engine::SessionCommitStatus;
using epocanva::windows::engine::SessionCreateResult;
using epocanva::windows::engine::SessionCreateStatus;
using epocanva::windows::engine::SessionKeyStatus;
using epocanva::windows::engine::DeploymentOptions;
using epocanva::windows::engine::SessionStateResult;
using epocanva::windows::engine::SessionStateStatus;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitCheckFailed = 5;
constexpr int kExitSelfCheckFailed = 14;

int g_total = 0;
int g_failed = 0;

void Check(bool condition, const std::string& what, const std::string& got) {
  ++g_total;
  if (condition) {
    std::printf("[engine-win] ok   %s\n", what.c_str());
    return;
  }
  ++g_failed;
  if (got.empty()) {
    std::printf("[engine-win] FAIL %s\n", what.c_str());
  } else {
    std::printf("[engine-win] FAIL %s  (got: %s)\n", what.c_str(), got.c_str());
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

std::wstring NormalizePath(const std::filesystem::path& path) {
  std::vector<wchar_t> buffer(1024, L'\0');
  const DWORD written = ::GetFullPathNameW(path.native().c_str(),
                                            static_cast<DWORD>(buffer.size()),
                                            buffer.data(), nullptr);
  if (written == 0 || written >= buffer.size()) {
    return std::wstring();
  }
  return std::wstring(buffer.data(), written);
}

bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right) {
  const std::wstring a = NormalizePath(left);
  const std::wstring b = NormalizePath(right);
  return !a.empty() && !b.empty() &&
         ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

int CountFilesBeneath(const std::filesystem::path& dir) {
  std::error_code ec;
  if (!std::filesystem::exists(dir, ec) || ec) {
    return 0;
  }
  int count = 0;
  std::filesystem::recursive_directory_iterator it(
      dir, std::filesystem::directory_options::skip_permission_denied, ec);
  if (ec) {
    return -1;
  }
  const std::filesystem::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) {
      return -1;
    }
    std::error_code file_ec;
    if (it->is_regular_file(file_ec) && !file_ec) {
      ++count;
    }
  }
  return count;
}

bool ParseArgs(int argc, char** argv, Options* options, std::string* error) {
  for (int i = 1; i < argc; ++i) {
    const std::string name(argv[i]);
    if (name == "--mode" || name == "--rime-dll" || name == "--baseline-dll" ||
        name == "--fixture-dir" || name == "--spelling-fixture-dir" ||
        name == "--run-root") {
      if (i + 1 >= argc) {
        *error = name + " requires a value";
        return false;
      }
      const std::string value(argv[++i]);
      if (name == "--mode") {
        options->mode = value;
      } else if (name == "--rime-dll") {
        options->rime_dll = value;
      } else if (name == "--baseline-dll") {
        options->baseline_dll = value;
      } else if (name == "--fixture-dir") {
        options->fixture_dir = value;
      } else if (name == "--spelling-fixture-dir") {
        options->spelling_fixture_dir = value;
      } else {
        options->run_root = value;
      }
      continue;
    }
    *error = "unknown argument: " + name;
    return false;
  }
  if (options->mode.empty()) {
    *error = "--mode is required";
    return false;
  }
  if (options->mode == "runtime" || options->mode == "dispatch" ||
      options->mode == "guards") {
    if (options->rime_dll.empty() || options->fixture_dir.empty() ||
        options->spelling_fixture_dir.empty() || options->run_root.empty()) {
      *error = options->mode + " mode requires --rime-dll, --fixture-dir, "
               "--spelling-fixture-dir and --run-root";
      return false;
    }
  } else if (options->mode != "state" && options->mode != "self-check-fail" &&
             options->mode != "self-check-fail-dispatch") {
    *error = "unknown mode: " + options->mode;
    return false;
  }
  return true;
}

bool PrepareRunRoot(const Options& options, const char* leaf,
                    std::filesystem::path* shared_dir,
                    std::filesystem::path* user_dir,
                    std::filesystem::path* log_dir,
                    std::filesystem::path* empty_user_dir,
                    std::filesystem::path* empty_log_dir) {
  if (options.run_root.filename() != "engine-bridge-run") {
    std::printf("[engine-win] FAIL run-root: the leaf must be named engine-bridge-run\n");
    return false;
  }
  std::error_code ec;
  if (std::filesystem::exists(options.run_root, ec)) {
    std::filesystem::remove_all(options.run_root, ec);
    if (ec) {
      std::printf("[engine-win] FAIL run-root: cannot remove %s (%s)\n",
                  options.run_root.string().c_str(), ec.message().c_str());
      return false;
    }
  }
  *shared_dir = options.run_root / "shared";
  *user_dir = options.run_root / "user";
  *log_dir = options.run_root / "log";
  *empty_user_dir = options.run_root / "empty-user";
  *empty_log_dir = options.run_root / "empty-log";
  for (const std::filesystem::path& dir :
       {*shared_dir, *user_dir, *log_dir, *empty_user_dir, *empty_log_dir}) {
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      std::printf("[engine-win] FAIL run-root: cannot create %s (%s)\n",
                  dir.string().c_str(), ec.message().c_str());
      return false;
    }
  }
  std::printf("[engine-win] run-root %s (recreated empty, case %s)\n",
              options.run_root.string().c_str(), leaf);
  return true;
}

bool CopyInto(const std::filesystem::path& source, const std::filesystem::path& dest) {
  std::error_code ec;
  std::filesystem::create_directories(dest.parent_path(), ec);
  if (ec) {
    std::printf("[engine-win] FAIL fixture: cannot create %s\n",
                dest.parent_path().string().c_str());
    return false;
  }
  std::filesystem::copy_file(source, dest,
                             std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    std::printf("[engine-win] FAIL fixture: %s -> %s (%s)\n", source.string().c_str(),
                dest.string().c_str(), ec.message().c_str());
    return false;
  }
  return true;
}

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
  std::printf("[engine-win] fixtures staged from the fixed completion and spelling fixtures\n");
  return true;
}

void CheckBalance(const ApiCallCounts& counts, const std::string& label) {
  Check(counts.balanced(), label + ": API acquire/free pairing", DescribeCounts(counts));
  Check(counts.create_session_calls == counts.destroy_session_calls,
        label + ": create_session/destroy_session pairing",
        CountText(counts.create_session_calls) + " vs " +
            CountText(counts.destroy_session_calls));
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
  RuntimeOptions runtime = BaseOptions(options, shared_dir, user_dir, log_dir);
  DeploymentOptions deployment;
  deployment.config_file_name = "default.yaml";
  deployment.schema_files.push_back(shared_dir / "epocanva_test_english.schema.yaml");
  deployment.schema_files.push_back(shared_dir / "epocanva_spell_english.schema.yaml");
  runtime.deployment = deployment;

  RimeRuntime preparation;
  const RuntimeStartStatus started = preparation.Start(runtime);
  Check(started == RuntimeStartStatus::kOk,
        "preparation runtime starts",
        DescribeRuntimeStartStatus(started));
  if (started != RuntimeStartStatus::kOk) {
    std::printf("[engine-win] preparation diagnostic: %s\n",
                preparation.Info().last_start_diagnostic.c_str());
    return false;
  }
  const RuntimeInfo info = preparation.Info();
  Check(info.deploy_calls == 3, "preparation deployed default + two schemas",
        CountText(info.deploy_calls));
  const std::filesystem::path build_dir = user_dir / "build";
  Check(std::filesystem::is_regular_file(build_dir / "default.yaml"),
        "compiled default.yaml exists in the prepared user directory");
  Check(std::filesystem::is_regular_file(
            build_dir / "epocanva_test_english.schema.yaml"),
        "compiled completion schema exists in the prepared user directory");
  Check(std::filesystem::is_regular_file(
            build_dir / "epocanva_spell_english.schema.yaml"),
        "compiled spelling schema exists in the prepared user directory");

  const RuntimeShutdownStatus stopped = preparation.Shutdown();
  Check(stopped == RuntimeShutdownStatus::kOk, "preparation runtime shuts down",
        DescribeRuntimeShutdownStatus(stopped));
  Check(preparation.Info().finalize_calls == 1,
        "preparation called finalize exactly once",
        CountText(preparation.Info().finalize_calls));
  Check(!preparation.Info().english_module_loaded,
        "English module is no longer reported loaded after finalize");
  CheckBalance(preparation.ApiCounts(), "preparation");
  return g_failed == 0;
}

void CheckStateIsEmpty(const SessionStateResult& state, const std::string& label) {
  Check(state.ok(), label + ": initial CaptureState",
        DescribeSessionStateStatus(state.status));
  if (!state.ok()) {
    return;
  }
  Check(state.state.raw_input.utf16_code_units.empty(),
        label + ": initial raw input is empty", AsciiOf(state.state.raw_input.utf16_code_units));
  Check(!state.state.is_composing, label + ": initial state is not composing");
  Check(state.state.cursor_status == CursorOffsetStatus::kNoCursor,
        label + ": non-composing cursor is no-cursor",
        DescribeCursorOffsetStatus(state.state.cursor_status));
  Check(state.state.selection_status == SelectionOffsetStatus::kNoSelection,
        label + ": non-composing selection is no-selection",
        DescribeSelectionOffsetStatus(state.state.selection_status));
  Check(state.state.candidates.empty(), label + ": initial menu is empty");
}

void RunRuntimeMode(const Options& options) {
  std::filesystem::path shared_dir;
  std::filesystem::path user_dir;
  std::filesystem::path log_dir;
  std::filesystem::path empty_user_dir;
  std::filesystem::path empty_log_dir;
  if (!PrepareRunRoot(options, "runtime", &shared_dir, &user_dir, &log_dir,
                      &empty_user_dir, &empty_log_dir) ||
      !StageFixtures(options, shared_dir)) {
    Check(false, "fixture preparation");
    return;
  }
  if (!PrepareDeployment(options, shared_dir, user_dir, log_dir)) {
    Check(false, "deployment preparation");
    return;
  }

  RimeRuntime runtime;
  const RuntimeStartStatus started =
      runtime.Start(BaseOptions(options, shared_dir, user_dir, log_dir));
  Check(started == RuntimeStartStatus::kOk, "runtime starts",
        DescribeRuntimeStartStatus(started));
  if (started != RuntimeStartStatus::kOk) {
    std::printf("[engine-win] runtime diagnostic: %s\n",
                runtime.Info().last_start_diagnostic.c_str());
    return;
  }

  const RuntimeInfo info = runtime.Info();
  std::printf("[engine-win] rime-dll %ls\n", info.loaded_dll_path.c_str());
  std::printf("[engine-win] librime %s\n", info.librime_version.c_str());
  Check(info.initialized, "runtime reports initialized");
  Check(SamePath(std::filesystem::path(info.requested_dll_path), options.rime_dll),
        "requested DLL path is the one passed on the command line",
        AsciiOf(info.requested_dll_path));
  Check(SamePath(std::filesystem::path(info.loaded_dll_path), options.rime_dll),
        "loaded DLL identity is the requested absolute path",
        AsciiOf(info.loaded_dll_path));
  Check(!info.librime_version.empty(), "librime version was read",
        info.librime_version);
  Check(info.api_complete, "all required RimeApi slots are present");
  Check(info.base_modules_registered, "core/dict/gears/default modules are registered");
  Check(info.english_module_loaded, "epocanva_english is loaded");
  Check(info.english_module_initialized >= 1,
        "epocanva_english initialize() ran", CountText(info.english_module_initialized));
  Check(info.initialize_calls == 1, "runtime initialized exactly once",
        CountText(info.initialize_calls));
  Check(info.deploy_calls == 0, "normal runtime performed no deployment",
        CountText(info.deploy_calls));
  Check(info.live_session_count == 0, "no session exists yet");

  RimeRuntime second;
  const RuntimeStartStatus second_start =
      second.Start(BaseOptions(options, shared_dir, user_dir, log_dir));
  Check(second_start == RuntimeStartStatus::kAnotherRuntimeActive,
        "a second runtime cannot initialize the process-global service",
        DescribeRuntimeStartStatus(second_start));
  Check(second.Info().initialize_calls == 0,
        "the refused second runtime never called initialize",
        CountText(second.Info().initialize_calls));

  const int files_before = CountFilesBeneath(user_dir / "build");
  Check(files_before >= 3, "prepared build directory holds the compiled artefacts",
        CountText(files_before));

  SessionCreateResult a =
      runtime.CreateSession("epocanva_test_english");
  Check(a.ok(), "session A created with the completion schema",
        std::string(DescribeSessionCreateStatus(a.status)) + ": " + a.diagnostic);
  SessionCreateResult b = runtime.CreateSession("epocanva_spell_english");
  Check(b.ok(), "session B created with the spelling schema",
        std::string(DescribeSessionCreateStatus(b.status)) + ": " + b.diagnostic);
  if (!a.ok() || !b.ok()) {
    return;
  }
  Check(a.session->schema_id() == "epocanva_test_english",
        "session A reports the requested schema id", a.session->schema_id());
  Check(b.session->schema_id() == "epocanva_spell_english",
        "session B reports the requested schema id", b.session->schema_id());
  Check(runtime.live_session_count() == 2, "two live sessions",
        CountText(runtime.live_session_count()));

  CheckStateIsEmpty(a.session->CaptureState(), "A");
  CheckStateIsEmpty(b.session->CaptureState(), "B");
  Check(a.session->ReadCommit().status == SessionCommitStatus::kNoCommit,
        "A has no commit before any key");
  Check(a.session->CaptureState().ok(), "A state read before keys");

  const std::uint64_t keys_before = runtime.ApiCounts().process_key_calls;
  const auto key_h = a.session->ProcessKeyOnce('h', 0);
  Check(key_h.status == SessionKeyStatus::kCallMade,
        "A process_key('h') was called exactly once",
        DescribeSessionKeyStatus(key_h.status));
  Check(key_h.engine_reported_handled, "A engine reported 'h' handled");
  Check(runtime.ApiCounts().process_key_calls == keys_before + 1,
        "A key call is visible in the API witness",
        CountText(runtime.ApiCounts().process_key_calls));

  const SessionStateResult state_a = a.session->CaptureState();
  Check(state_a.ok(), "A state after 'h'", DescribeSessionStateStatus(state_a.status));
  if (state_a.ok()) {
    CheckText(state_a.state.raw_input.utf16_code_units, "h", "A raw input is h");
    CheckText(state_a.state.preedit.utf16_code_units, "h", "A preedit is h");
    Check(state_a.state.is_composing, "A is composing after h");
    Check(state_a.state.cursor_status == CursorOffsetStatus::kMapped,
          "A cursor in h is mapped",
          DescribeCursorOffsetStatus(state_a.state.cursor_status));
    Check(state_a.state.cursor_utf16_code_unit_offset == 1,
          "A cursor is one UTF-16 code unit into h",
          CountText(state_a.state.cursor_utf16_code_unit_offset));
    Check(!state_a.state.candidates.empty(), "A menu is present after h");
    if (!state_a.state.candidates.empty()) {
      CheckText(state_a.state.candidates[0].text_utf16_code_units, "h",
                "A candidate 0 is the raw input h");
      Check(state_a.state.candidates[0].comment_utf16_code_units.empty(),
            "A candidate 0 carries no comment");
      Check(state_a.state.candidates.size() >= 2,
            "A menu has at least one completion",
            CountText(state_a.state.candidates.size()));
      if (state_a.state.candidates.size() >= 2) {
        CheckText(state_a.state.candidates[1].text_utf16_code_units, "hello",
                  "A candidate 1 is the real fixture completion hello");
      }
    }
  }

  const auto key_b = b.session->ProcessKeyOnce('a', 0);
  Check(key_b.status == SessionKeyStatus::kCallMade && key_b.engine_reported_handled,
        "B process_key('a') handled", DescribeSessionKeyStatus(key_b.status));
  const SessionStateResult state_b = b.session->CaptureState();
  Check(state_b.ok(), "B state after 'a'", DescribeSessionStateStatus(state_b.status));
  if (state_b.ok()) {
    CheckText(state_b.state.raw_input.utf16_code_units, "a", "B raw input is a");
  }
  const SessionStateResult state_a_again = a.session->CaptureState();
  Check(state_a_again.ok(), "A state is still readable after B input",
        DescribeSessionStateStatus(state_a_again.status));
  if (state_a_again.ok()) {
    CheckText(state_a_again.state.raw_input.utf16_code_units, "h",
              "A raw input is unchanged by B");
  }

  const int files_after = CountFilesBeneath(user_dir / "build");
  Check(files_after == files_before,
        "no deployment happened on session creation or key dispatch",
        CountText(files_before) + " -> " + CountText(files_after));

  SessionCloseStatus thread_close = SessionCloseStatus::kOk;
  SessionStateStatus thread_state = SessionStateStatus::kOk;
  SessionKeyStatus thread_key = SessionKeyStatus::kCallMade;
  RuntimeShutdownStatus thread_shutdown = RuntimeShutdownStatus::kOk;
  SessionCreateStatus thread_create = SessionCreateStatus::kOk;
  std::thread wrong_thread([&]() {
    thread_close = b.session->Close();
    thread_state = b.session->CaptureState().status;
    thread_key = b.session->ProcessKeyOnce('x', 0).status;
    thread_shutdown = runtime.Shutdown();
    thread_create = runtime.CreateSession("epocanva_test_english").status;
  });
  wrong_thread.join();
  Check(thread_close == SessionCloseStatus::kWrongThread,
        "Close from a non-owner thread is refused",
        DescribeSessionCloseStatus(thread_close));
  Check(thread_state == SessionStateStatus::kWrongThread,
        "CaptureState from a non-owner thread is refused",
        DescribeSessionStateStatus(thread_state));
  Check(thread_key == SessionKeyStatus::kWrongThread,
        "ProcessKeyOnce from a non-owner thread is refused",
        DescribeSessionKeyStatus(thread_key));
  Check(thread_shutdown == RuntimeShutdownStatus::kWrongThread,
        "Shutdown from a non-owner thread is refused",
        DescribeRuntimeShutdownStatus(thread_shutdown));
  Check(thread_create == SessionCreateStatus::kWrongThread,
        "CreateSession from a non-owner thread is refused",
        DescribeSessionCreateStatus(thread_create));
  Check(runtime.IsInitialized() && runtime.live_session_count() == 2,
        "the refused cross-thread calls left the runtime and sessions intact",
        CountText(runtime.live_session_count()));
  Check(b.session->IsOpen(), "B remains open after the refused cross-thread Close");

  const RuntimeShutdownStatus live_refused = runtime.Shutdown();
  Check(live_refused == RuntimeShutdownStatus::kLiveSessionsRemain,
        "Shutdown with live sessions is refused",
        DescribeRuntimeShutdownStatus(live_refused));
  Check(runtime.IsInitialized() && runtime.live_session_count() == 2,
        "the refused Shutdown did not finalize or unload",
        CountText(runtime.live_session_count()));
  Check(a.session->IsOpen() && b.session->IsOpen(),
        "both sessions remain usable after the refused Shutdown");

  Check(a.session->Close() == SessionCloseStatus::kOk, "A closes");
  Check(b.session->Close() == SessionCloseStatus::kOk, "B closes");
  Check(runtime.live_session_count() == 0, "no live sessions after both Close calls",
        CountText(runtime.live_session_count()));
  const SessionCloseStatus close_again = a.session->Close();
  Check(close_again == SessionCloseStatus::kAlreadyClosed,
        "closing A twice reports already-closed",
        DescribeSessionCloseStatus(close_again));

  const RuntimeShutdownStatus refused = runtime.Shutdown();
  Check(refused == RuntimeShutdownStatus::kOk, "runtime shuts down after sessions close",
        DescribeRuntimeShutdownStatus(refused));
  const RuntimeInfo after = runtime.Info();
  Check(!after.initialized, "runtime is no longer initialized");
  Check(after.finalize_calls == 1, "finalize called exactly once",
        CountText(after.finalize_calls));
  Check(!after.english_module_loaded, "English module is no longer loaded after finalize");
  Check(after.live_session_count == 0, "runtime reports no live sessions");
  Check(runtime.Shutdown() == RuntimeShutdownStatus::kAlreadyShutdown,
        "a second Shutdown reports already-shutdown");
  Check(runtime.CreateSession("epocanva_test_english").status ==
            SessionCreateStatus::kNotStarted,
        "CreateSession after Shutdown is refused");

  const ApiCallCounts main_counts = runtime.ApiCounts();
  Check(main_counts.process_key_calls == 2, "main runtime made exactly two key calls",
        CountText(main_counts.process_key_calls));
  Check(main_counts.deploy_calls == 0, "main runtime made no deployment calls",
        CountText(main_counts.deploy_calls));
  CheckBalance(main_counts, "main runtime");

  Check(CountFilesBeneath(log_dir) == 0, "the controlled log directory stayed empty",
        CountText(CountFilesBeneath(log_dir)));

  // --- failure cases -------------------------------------------------------------
  {
    RimeRuntime missing;
    RuntimeOptions bad = BaseOptions(options, shared_dir, user_dir, log_dir);
    bad.rime_dll_path = options.run_root / "missing" / "rime.dll";
    const RuntimeStartStatus status = missing.Start(bad);
    Check(status == RuntimeStartStatus::kDllLoadFailed,
          "a missing DLL fails with dll-load-failed",
          DescribeRuntimeStartStatus(status));
    Check(missing.Info().initialize_calls == 0,
          "a missing DLL never reached initialize");
  }
  {
    RimeRuntime relative;
    RuntimeOptions bad = BaseOptions(options, shared_dir, user_dir, log_dir);
    bad.rime_dll_path = std::filesystem::path("rime.dll");
    const RuntimeStartStatus status = relative.Start(bad);
    Check(status == RuntimeStartStatus::kInvalidOptions,
          "a relative DLL path is refused before loading",
          DescribeRuntimeStartStatus(status));
  }
  {
    RimeRuntime dir;
    RuntimeOptions bad = BaseOptions(options, shared_dir, user_dir, log_dir);
    bad.shared_data_dir = options.run_root / "no-such-shared-directory";
    const RuntimeStartStatus status = dir.Start(bad);
    Check(status == RuntimeStartStatus::kInvalidOptions,
          "a missing shared directory is refused",
          DescribeRuntimeStartStatus(status));
  }
  {
    RimeRuntime non_ascii;
    RuntimeOptions bad = BaseOptions(options, shared_dir, user_dir, log_dir);
    bad.user_data_dir = options.run_root / L"\u00E9ngine-user";
    const RuntimeStartStatus status = non_ascii.Start(bad);
    Check(status == RuntimeStartStatus::kInvalidOptions,
          "a non-ASCII engine directory is refused rather than guessed",
          DescribeRuntimeStartStatus(status));
  }
  {
    wchar_t system_dir[MAX_PATH] = {};
    const UINT length = ::GetSystemDirectoryW(system_dir, MAX_PATH);
    Check(length > 0 && length < MAX_PATH, "GetSystemDirectoryW returned a path");
    RimeRuntime wrong;
    RuntimeOptions bad = BaseOptions(options, shared_dir, user_dir, log_dir);
    bad.rime_dll_path = std::filesystem::path(system_dir) / "kernel32.dll";
    const RuntimeStartStatus status = wrong.Start(bad);
    Check(status == RuntimeStartStatus::kApiSymbolMissing,
          "a DLL without rime_get_api fails with api-symbol-missing",
          DescribeRuntimeStartStatus(status));
    Check(wrong.Info().initialize_calls == 0,
          "a DLL without rime_get_api never reached initialize");
  }
  if (!options.baseline_dll.empty()) {
    if (!std::filesystem::is_regular_file(options.baseline_dll)) {
      std::printf("[engine-win] UNVERIFIED baseline DLL is absent: %s\n",
                  options.baseline_dll.string().c_str());
    } else {
      RimeRuntime baseline;
      RuntimeOptions bad = BaseOptions(options, shared_dir, user_dir, log_dir);
      bad.rime_dll_path = options.baseline_dll;
      const RuntimeStartStatus status = baseline.Start(bad);
      Check(status == RuntimeStartStatus::kEnglishModuleMissing,
            "the baseline DLL fails with english-module-missing",
            DescribeRuntimeStartStatus(status));
      Check(baseline.Info().initialize_calls == 1,
            "the baseline DLL initialized once before the module check",
            CountText(baseline.Info().initialize_calls));
      Check(baseline.Info().finalize_calls == 1,
            "the failed baseline initialization was finalized",
            CountText(baseline.Info().finalize_calls));
      Check(!baseline.Info().english_module_loaded,
            "the baseline never reported the English module loaded");
      CheckBalance(baseline.ApiCounts(), "baseline failure");

      RimeRuntime recovery;
      const RuntimeStartStatus recovered =
          recovery.Start(BaseOptions(options, shared_dir, user_dir, log_dir));
      Check(recovered == RuntimeStartStatus::kOk,
            "a valid runtime starts after the baseline failure",
            DescribeRuntimeStartStatus(recovered));
      if (recovered == RuntimeStartStatus::kOk) {
        Check(recovery.Shutdown() == RuntimeShutdownStatus::kOk,
              "the recovery runtime shuts down");
      }
    }
  } else {
    std::printf("[engine-win] UNVERIFIED baseline module-missing case: no --baseline-dll\n");
  }

  {
    RimeRuntime no_schema;
    RuntimeOptions empty = BaseOptions(options, shared_dir, empty_user_dir, empty_log_dir);
    const RuntimeStartStatus status = no_schema.Start(empty);
    Check(status == RuntimeStartStatus::kOk,
          "runtime starts against an empty prepared user directory",
          DescribeRuntimeStartStatus(status));
    if (status == RuntimeStartStatus::kOk) {
      const SessionCreateResult session =
          no_schema.CreateSession("epocanva_test_missing");
      if (session.ok()) {
        (void)session.session->Close();
      }
      Check(session.status == SessionCreateStatus::kSchemaUnavailable,
            "a missing compiled schema fails with schema-unavailable",
            DescribeSessionCreateStatus(session.status));
      Check(no_schema.live_session_count() == 0,
            "the refused schema did not create a live session");
      Check(no_schema.ApiCounts().create_session_calls == 0,
            "schema_open failing happens before create_session",
            CountText(no_schema.ApiCounts().create_session_calls));
      Check(no_schema.Shutdown() == RuntimeShutdownStatus::kOk,
            "the empty-directory runtime shuts down");
      CheckBalance(no_schema.ApiCounts(), "missing schema");
    }
  }
}

void RunStateMode() {
  const EngineTextCapture ascii = CaptureEngineText("hel");
  Check(ascii.ok(), "ASCII capture succeeds");
  if (ascii.ok()) {
    Check(ascii.text.utf16_code_units == U16("hel"), "ASCII UTF-16 copy is exact",
          AsciiOf(ascii.text.utf16_code_units));
    Check(ascii.text.utf8_byte_length == 3, "ASCII byte length is 3",
          CountText(ascii.text.utf8_byte_length));
    Check(ascii.text.boundary_map.boundary_count() == 4,
          "ASCII boundary count is 4", CountText(ascii.text.boundary_map.boundary_count()));
  }

  const EngineTextCapture empty = CaptureEngineText(std::string_view());
  Check(empty.ok() && empty.text.utf16_code_units.empty(),
        "empty text is a successful empty copy");

  const EngineTextCapture two_byte = CaptureEngineText("\xC3\xA9");  // U+00E9
  Check(two_byte.ok(), "2-byte UTF-8 capture succeeds");
  if (two_byte.ok()) {
    Check(two_byte.text.utf8_byte_length == 2, "2-byte text has 2 UTF-8 bytes",
          CountText(two_byte.text.utf8_byte_length));
    Check(two_byte.text.utf16_code_units.size() == 1,
          "2-byte text is 1 UTF-16 code unit",
          CountText(two_byte.text.utf16_code_units.size()));
    Check(two_byte.text.utf16_code_units[0] == 0x00E9,
          "2-byte text decodes to U+00E9");
  }

  const EngineTextCapture four_byte = CaptureEngineText("\xF0\x9F\x98\x80");  // U+1F600
  Check(four_byte.ok(), "4-byte UTF-8 capture succeeds");
  if (four_byte.ok()) {
    Check(four_byte.text.utf8_byte_length == 4, "4-byte text has 4 UTF-8 bytes",
          CountText(four_byte.text.utf8_byte_length));
    Check(four_byte.text.utf16_code_units.size() == 2,
          "supplementary scalar becomes a surrogate pair",
          CountText(four_byte.text.utf16_code_units.size()));
  }

  const std::string with_nul("a\0b", 3);
  const EngineTextCapture embedded_nul = CaptureEngineText(with_nul);
  Check(embedded_nul.ok() && embedded_nul.text.utf16_code_units.size() == 3,
        "an embedded NUL is preserved by explicit length",
        embedded_nul.ok() ? CountText(embedded_nul.text.utf16_code_units.size()) : "failed");

  const EngineTextCapture invalid = CaptureEngineText("\x80");
  Check(!invalid.ok() && invalid.text.utf16_code_units.empty(),
        "a lone continuation byte is refused with no partial output");
  Check(invalid.status == epocanva::windows::text::ConversionStatus::kInvalidInput,
        "the refusal is invalid-input",
        epocanva::windows::text::DescribeConversionStatus(invalid.status));

  const PreeditOffsetMapping hel = MapPreeditOffsets("hel", 3, 0, 3);
  Check(hel.cursor_status == CursorOffsetStatus::kMapped &&
            hel.cursor_utf16_code_unit_offset == 3,
        "ASCII cursor maps to 3 UTF-16 units");
  Check(hel.selection_status == SelectionOffsetStatus::kMapped &&
            hel.selection_start_utf16_code_unit_offset == 0 &&
            hel.selection_end_utf16_code_unit_offset == 3,
        "ASCII selection maps to [0,3)");

  const PreeditOffsetMapping empty_mapping = MapPreeditOffsets(std::string_view(), 0, 0, 0);
  Check(empty_mapping.cursor_status == CursorOffsetStatus::kMapped &&
            empty_mapping.cursor_utf16_code_unit_offset == 0,
        "empty text maps cursor 0 to 0");
  Check(empty_mapping.selection_status == SelectionOffsetStatus::kMapped &&
            empty_mapping.selection_start_utf16_code_unit_offset == 0 &&
            empty_mapping.selection_end_utf16_code_unit_offset == 0,
        "empty text maps an empty selection to [0,0)");

  const PreeditOffsetMapping two_byte_mid = MapPreeditOffsets("\xC3\xA9", 1, 0, 2);
  Check(two_byte_mid.cursor_status == CursorOffsetStatus::kInsideSequence,
        "cursor inside a 2-byte sequence is named, not rounded",
        DescribeCursorOffsetStatus(two_byte_mid.cursor_status));
  Check(two_byte_mid.selection_status == SelectionOffsetStatus::kMapped &&
            two_byte_mid.selection_end_utf16_code_unit_offset == 1,
        "selection end 2 bytes maps to 1 UTF-16 unit");
  const PreeditOffsetMapping two_byte_inside_sel = MapPreeditOffsets("\xC3\xA9", 2, 0, 1);
  Check(two_byte_inside_sel.selection_status == SelectionOffsetStatus::kInsideSequence,
        "selection endpoint inside a sequence is named",
        DescribeSelectionOffsetStatus(two_byte_inside_sel.selection_status));
  const PreeditOffsetMapping two_byte_past = MapPreeditOffsets("\xC3\xA9", 3, 0, 3);
  Check(two_byte_past.cursor_status == CursorOffsetStatus::kOutOfRange,
        "cursor past the text is out-of-range",
        DescribeCursorOffsetStatus(two_byte_past.cursor_status));
  const PreeditOffsetMapping two_byte_sel_past = MapPreeditOffsets("\xC3\xA9", 2, 0, 3);
  Check(two_byte_sel_past.selection_status == SelectionOffsetStatus::kOutOfRange,
        "selection past the text is out-of-range",
        DescribeSelectionOffsetStatus(two_byte_sel_past.selection_status));

  const PreeditOffsetMapping emoji = MapPreeditOffsets("\xF0\x9F\x98\x80", 4, 0, 4);
  Check(emoji.cursor_status == CursorOffsetStatus::kMapped &&
            emoji.cursor_utf16_code_unit_offset == 2,
        "4-byte scalar maps to 2 UTF-16 units");
  const PreeditOffsetMapping emoji_mid = MapPreeditOffsets("\xF0\x9F\x98\x80", 2, 0, 4);
  Check(emoji_mid.cursor_status == CursorOffsetStatus::kInsideSequence,
        "cursor inside a 4-byte sequence is inside-sequence");

  const PreeditOffsetMapping no_cursor = MapPreeditOffsets("a", -1, 0, 1);
  Check(no_cursor.cursor_status == CursorOffsetStatus::kNoCursor,
        "negative cursor is the no-cursor sentinel",
        DescribeCursorOffsetStatus(no_cursor.cursor_status));
  const PreeditOffsetMapping no_selection = MapPreeditOffsets("a", 1, -1, -1);
  Check(no_selection.selection_status == SelectionOffsetStatus::kNoSelection,
        "both negative selection fields are no-selection",
        DescribeSelectionOffsetStatus(no_selection.selection_status));
  const PreeditOffsetMapping mixed_sentinel = MapPreeditOffsets("a", 1, -1, 1);
  Check(mixed_sentinel.selection_status == SelectionOffsetStatus::kSentinelMismatch,
        "one negative selection field is a sentinel mismatch, not no-selection",
        DescribeSelectionOffsetStatus(mixed_sentinel.selection_status));
  const PreeditOffsetMapping reversed = MapPreeditOffsets("abc", 3, 2, 1);
  Check(reversed.selection_status == SelectionOffsetStatus::kOrderInvalid,
        "start after end is order-invalid",
        DescribeSelectionOffsetStatus(reversed.selection_status));
  const PreeditOffsetMapping invalid_text = MapPreeditOffsets("\x80", 0, 0, 0);
  Check(invalid_text.cursor_status == CursorOffsetStatus::kTextInvalid &&
            invalid_text.selection_status == SelectionOffsetStatus::kTextInvalid,
        "invalid UTF-8 makes both mappings text-invalid");

  Check(std::string(DescribeCursorOffsetStatus(CursorOffsetStatus::kNoCursor)) ==
            "no-cursor",
        "cursor status names are stable");
  Check(std::string(DescribeSelectionOffsetStatus(
                SelectionOffsetStatus::kSentinelMismatch)) == "sentinel-mismatch",
        "selection status names are stable");
}

void RunSelfCheckFail() {
  Check(CaptureEngineText("\x80").ok(),
        "deliberately false: a lone continuation byte is accepted");
}

int FinishSelfCheck() {
  if (g_failed == 0) {
    std::printf("[engine-win] RESULT PASS (%d checks, 0 failed)\n", g_total);
    return kExitOk;
  }
  std::printf("[engine-win] RESULT FAIL (%d checks, %d failed)\n", g_total, g_failed);
  return kExitSelfCheckFailed;
}

int FinishRun() {
  if (g_failed != 0) {
    std::printf("[engine-win] RESULT FAIL (%d checks, %d failed)\n", g_total, g_failed);
    return kExitCheckFailed;
  }
  std::printf("[engine-win] RESULT PASS (%d checks, %d failed)\n", g_total, g_failed);
  return kExitOk;
}

}  // namespace engine_bridge
}  // namespace tests
}  // namespace epocanva

using namespace epocanva::tests::engine_bridge;

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!ParseArgs(argc, argv, &options, &error)) {
    std::printf("engine_bridge_tests: %s\n", error.c_str());
    std::printf(
        "usage: engine_bridge_tests --mode runtime|dispatch|guards|state|"
        "self-check-fail|self-check-fail-dispatch "
        "[--rime-dll <abs> --baseline-dll <abs> --fixture-dir <dir> "
        "--spelling-fixture-dir <dir> --run-root <.../engine-bridge-run>]\n");
    return kExitUsage;
  }

  if (options.mode == "runtime") {
    RunRuntimeMode(options);
    return FinishRun();
  }
  if (options.mode == "dispatch") {
    RunDispatchMode(options);
    return FinishRun();
  }
  if (options.mode == "guards") {
    RunGuardsMode(options);
    return FinishRun();
  }
  if (options.mode == "state") {
    RunStateMode();
    return FinishRun();
  }
  if (options.mode == "self-check-fail-dispatch") {
    RunSelfCheckFailDispatch();
    return FinishSelfCheck();
  }
  RunSelfCheckFail();
  return FinishSelfCheck();
}
