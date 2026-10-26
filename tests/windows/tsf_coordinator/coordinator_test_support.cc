// EpoCanva P3.8 -- shared scaffolding implementation. Test code only; see the header.
#include "coordinator_test_support.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

#include "engine_executor/controlled_backend.h"
#include "engine_executor_internal.h"
#include "engine_executor/test_support.h"

namespace epocanva {
namespace tsf {
namespace coordinator {
namespace test {

namespace executor_tests = epocanva::tests::engine_executor;

int g_total = 0;
int g_failed = 0;

void Check(bool condition, const std::string& what, const std::string& got) {
  ++g_total;
  if (condition) {
    std::printf("[coordinator-win] PASS %s\n", what.c_str());
    return;
  }
  ++g_failed;
  std::printf("[coordinator-win] FAIL %s", what.c_str());
  if (!got.empty()) {
    std::printf(" (got: %s)", got.c_str());
  }
  std::printf("\n");
}

void CheckOutcome(const CoordinatorSnapshot& snapshot, WorkOutcome want,
                  const std::string& what) {
  Check(snapshot.outcome == want, what, DescribeWorkOutcome(snapshot.outcome));
}

void CheckDocText(const std::wstring& actual, const std::wstring& expected,
                  const std::string& what) {
  bool equal = actual == expected;
  ++g_total;
  if (equal) {
    std::printf("[coordinator-win] PASS %s\n", what.c_str());
    return;
  }
  ++g_failed;
  std::size_t offset = 0;
  while (offset < actual.size() && offset < expected.size() &&
         actual[offset] == expected[offset]) {
    ++offset;
  }
  std::printf(
      "[coordinator-win] FAIL %s (got %zu utf16 units, want %zu, first difference at "
      "offset %zu)\n",
      what.c_str(), actual.size(), expected.size(), offset);
}

std::string DescribeSnapshot(const CoordinatorSnapshot& snapshot) {
  std::string text;
  text += std::string("phase=") + DescribeWorkPhase(snapshot.phase);
  text += std::string(" outcome=") + DescribeWorkOutcome(snapshot.outcome);
  text += std::string(" cause=") + DescribeRejectCause(snapshot.cause);
  text += std::string(" gate=") + DescribeCoordinatorGate(snapshot.gate);
  text += " bound=" + std::to_string(snapshot.bound ? 1 : 0);
  text += " closed=" + std::to_string(snapshot.closed ? 1 : 0);
  text += " gen=" + std::to_string(snapshot.identity.generation);
  text += " caret=" + std::to_string(snapshot.tracked_caret_utf16);
  text += " composition=" + std::to_string(snapshot.composition_active ? 1 : 0);
  text += " works=" + std::to_string(snapshot.works_accepted);
  text += " taken=" + std::to_string(snapshot.engine_results_taken);
  text += " plans=" + std::to_string(snapshot.plans_submitted);
  text += " applied=" + std::to_string(snapshot.edits_applied);
  text += " refused=" + std::to_string(snapshot.edits_refused);
  text += " stale=" + std::to_string(snapshot.edits_stale);
  text += std::string(" permission=") + edit::DescribeInputPermission(snapshot.permission);
  text += " pqueries=" + std::to_string(snapshot.permission_queries_begun) + "/" +
          std::to_string(snapshot.permission_queries_completed);
  text += " prechecks=" + std::to_string(snapshot.permission_rechecks);
  text += " owned_comp=" + std::to_string(snapshot.owned_composition_active ? 1 : 0);
  text += " retiring=" + std::to_string(snapshot.retirement.retiring_bindings) +
          "/" + std::to_string(snapshot.retirement.capacity);
  text += " retire_paused=" + std::to_string(snapshot.retirement.admission_paused ? 1 : 0);
  text += " retire_settled=" + std::to_string(snapshot.retirement.settled) +
          " blocked=" + std::to_string(snapshot.retirement.blocked) +
          " close=" + std::to_string(snapshot.retirement.close_requests) + "/" +
          std::to_string(snapshot.retirement.close_results_taken);
  text += std::string(" cleanup=") +
          DescribeCleanupState(snapshot.retirement.last_cleanup_state);
  text += " ext_term=" + std::to_string(snapshot.retirement.externally_terminated) +
          " end_calls=" + std::to_string(snapshot.retirement.cleanup_end_calls) +
          "/" + std::to_string(snapshot.retirement.cleanup_end_failures);
  text += " last_owned=" +
          std::to_string(snapshot.retirement.last_cleanup_owned_at_start ? 1 : 0) +
          " last_live=" +
          std::to_string(snapshot.retirement.last_cleanup_live_at_start ? 1 : 0) +
          " last_ext=" +
          std::to_string(snapshot.retirement.last_cleanup_external_at_start ? 1 : 0) +
          " last_ended=" +
          std::to_string(snapshot.retirement.last_cleanup_ended_before_at_start ? 1 : 0) +
          " last_notif=" +
          std::to_string(
              snapshot.retirement.last_cleanup_termination_notifications_at_start) +
          "/" +
          std::to_string(
              snapshot.retirement.last_cleanup_termination_notifications_after_call) +
          " last_retire=" + std::to_string(snapshot.retirement.last_cleanup_sequence) +
          " last_ctx=" + std::to_string(snapshot.retirement.last_cleanup_context_id) +
          " last_gen=" + std::to_string(snapshot.retirement.last_cleanup_generation) +
          " last_plan=" +
          std::to_string(snapshot.retirement.last_cleanup_plan_submitted ? 1 : 0) +
          " last_plan_id=" +
          std::to_string(snapshot.retirement.last_cleanup_plan_session_id) +
          " last_end=" +
          std::to_string(snapshot.retirement.last_cleanup_end_issued ? 1 : 0) +
          " last_hr=0x" + [&] {
            char buffer[16] = {};
            std::snprintf(buffer, sizeof(buffer), "%08lX",
                          static_cast<unsigned long>(snapshot.retirement.last_cleanup_end_hr));
            return std::string(buffer);
          }();
  return text;
}

bool ParseEngineArgs(int argc, char** argv, Options* options, std::string* case_name) {
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--case") == 0 && index + 1 < argc) {
      *case_name = argv[++index];
    } else if (std::strcmp(argv[index], "--rime-dll") == 0 && index + 1 < argc) {
      options->rime_dll = argv[++index];
    } else if (std::strcmp(argv[index], "--fixture-dir") == 0 && index + 1 < argc) {
      options->fixture_dir = argv[++index];
    } else if (std::strcmp(argv[index], "--spelling-fixture-dir") == 0 && index + 1 < argc) {
      options->spelling_fixture_dir = argv[++index];
    } else if (std::strcmp(argv[index], "--run-root") == 0 && index + 1 < argc) {
      options->run_root = argv[++index];
    }
  }
  return !case_name->empty();
}

bool CreateControlledHarness(ControlledHarness* harness) {
  auto* backend = new executor_tests::ControlledBackend();
  harness->backend = backend;
  epocanva::windows::engine::ExecutorOptions options;
  options.capacity = 8;
  harness->executor = epocanva::windows::engine::EngineExecutorTestFactory::Create(
      options, std::unique_ptr<epocanva::windows::engine::EngineBackend>(backend));
  if (harness->executor->Start() != epocanva::windows::engine::StartStatus::kOk) {
    Check(false, "controlled executor started", "Start != Ok");
    return false;
  }
  const auto controller_options = epocanva::windows::engine::ControllerOptions{
      true, epocanva::windows::input::InputPermission::kAllowed};
  for (epocanva::windows::engine::ClientId* client : {&harness->client_a, &harness->client_b}) {
    epocanva::windows::engine::RequestId request = executor_tests::SubmitCreateOrFail(
        *harness->executor, "controlled", controller_options, client, "controlled client");
    if (!request.valid()) {
      return false;
    }
    executor_tests::TakeOrFail(*harness->executor, request, 10000, "controlled create");
  }
  return true;
}

bool CreateRealHarness(const Options& options, const char* leaf, RealHarness* harness) {
  std::filesystem::path shared_dir;
  std::filesystem::path user_dir;
  std::filesystem::path log_dir;
  if (!executor_tests::PrepareRunRoot(options, leaf, &shared_dir, &user_dir, &log_dir) ||
      !executor_tests::StageFixtures(options, shared_dir)) {
    Check(false, "real engine: fixture preparation", "");
    return false;
  }
  if (!executor_tests::PrepareDeployment(options, shared_dir, user_dir, log_dir)) {
    Check(false, "real engine: deployment preparation", "");
    return false;
  }
  epocanva::windows::engine::ExecutorOptions executor_options;
  executor_options.runtime =
      executor_tests::BaseOptions(options, shared_dir, user_dir, log_dir);
  executor_options.capacity = 8;
  harness->executor = std::make_unique<EngineExecutor>(executor_options);
  if (harness->executor->Start() != epocanva::windows::engine::StartStatus::kOk) {
    Check(false, "real engine: executor started", "Start != Ok");
    return false;
  }
  const auto controller_options = epocanva::windows::engine::ControllerOptions{
      true, epocanva::windows::input::InputPermission::kAllowed};
  epocanva::windows::engine::RequestId request = executor_tests::SubmitCreateOrFail(
      *harness->executor, "epocanva_test_english", controller_options,
      &harness->client_a, "real engine client");
  if (!request.valid()) {
    return false;
  }
  executor_tests::TakeOrFail(*harness->executor, request, 30000, "real engine create");
  return true;
}

epocanva::windows::engine::ClientId AddRealClient(RealHarness* harness) {
  epocanva::windows::engine::ClientId client{};
  epocanva::windows::engine::RequestId add_request = executor_tests::SubmitCreateOrFail(
      *harness->executor, "epocanva_test_english",
      epocanva::windows::engine::ControllerOptions{
          true, epocanva::windows::input::InputPermission::kAllowed},
      &client, "real engine extra client");
  if (!add_request.valid()) {
    return client;
  }
  executor_tests::TakeOrFail(*harness->executor, add_request, 30000, "real engine extra");
  return client;
}

DriveResult DriveToCompletion(ContextCoordinator& coordinator, TestHost& host,
                              EngineExecutor& executor, DWORD timeout_ms) {
  DriveResult driven;
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    coordinator.Pump();
    driven.final_state = coordinator.Snapshot();
    const bool work_idle = driven.final_state.phase == WorkPhase::kIdle;
    const bool retirements_idle =
        driven.final_state.retirement.retiring_bindings == 0;
    if (work_idle && retirements_idle) {
      driven.completed = true;
      return driven;
    }
    const ULONGLONG now = ::GetTickCount64();
    if (now >= deadline) {
      return driven;
    }
    const DWORD slice =
        static_cast<DWORD>(deadline - now < 200 ? deadline - now : 200);
    if (!work_idle && driven.final_state.phase == WorkPhase::kEnginePending) {
      // No TSF edit is outstanding for this work item, so a bounded executor wait on this
      // STA cannot starve an edit callback; it only avoids a spin.
      executor.WaitForResult(driven.final_state.engine_request,
                             std::chrono::milliseconds(slice));
    } else if (!work_idle &&
               driven.final_state.phase == WorkPhase::kEditPending) {
      // EditPending: dispatch the STA's messages until the edit's completion event (set
      // by the real callback, on success and refusal alike) is signalled.
      host.PumpUntil(coordinator.EditCompletionEvent(), slice);
    } else if (!work_idle &&
               driven.final_state.phase == WorkPhase::kPermissionPending) {
      // PermissionPending: dispatch the STA's messages until the real read session's
      // completion event is signalled (a manual test source releases this synchronously
      // through the cases' own driver, so this branch simply yields the slice).
      HANDLE event = coordinator.PermissionCompletionEvent();
      if (event != nullptr) {
        host.PumpUntil(event, slice);
      } else {
        ::SleepEx(slice, TRUE);
      }
    } else {
      // The current work item is idle but a retired binding is not terminal. Wait on the
      // real fact retirement exposes: its cleanup edit, its close request, or its
      // pre-retirement engine request. No sleep decides completion.
      const RetirementSnapshot& retirement = driven.final_state.retirement;
      if (retirement.pending_edit_event != nullptr) {
        host.PumpUntil(retirement.pending_edit_event, slice);
      } else if (retirement.pending_close_request.valid()) {
        executor.WaitForResult(retirement.pending_close_request,
                               std::chrono::milliseconds(slice));
      } else if (retirement.pending_engine_request.valid()) {
        executor.WaitForResult(retirement.pending_engine_request,
                               std::chrono::milliseconds(slice));
      } else {
        host.DispatchQueuedMessages();
        ::SleepEx(slice, TRUE);
      }
    }
  }
}

DriveResult TypeKeyAndDrive(ContextCoordinator& coordinator, TestHost& host,
                            EngineExecutor& executor,
                            const epocanva::windows::input::KeyEventSnapshot& event,
                            const epocanva::windows::input::KeyText& text,
                            const std::string& label, DWORD timeout_ms) {
  WorkId id;
  const SubmitStatus submitted = coordinator.SubmitKey(event, text, &id);
  Check(submitted == SubmitStatus::kAccepted, label + ": submit accepted",
        DescribeSubmitStatus(submitted));
  if (submitted != SubmitStatus::kAccepted) {
    DriveResult driven;
    driven.final_state = coordinator.Snapshot();
    return driven;
  }
  DriveResult driven = DriveToCompletion(coordinator, host, executor, timeout_ms);
  Check(driven.completed, label + ": work reached a terminal state",
        DescribeSnapshot(driven.final_state));
  return driven;
}

int Finish() {
  std::printf("[coordinator-win] RESULT %d/%d checks, %d failed\n", g_total - g_failed,
              g_total, g_failed);
  if (g_failed == 0) {
    std::printf("RESULT PASS coordinator-win %d checks\n", g_total);
    return kExitOk;
  }
  return kExitCheckFailed;
}

}  // namespace test
}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva
