// EpoCanva P3.11 -- implementation of the non-interactive lab checks.
#include "lab_self_test.h"

#include <windows.h>

#include <cstdio>
#include <memory>
#include <string>

#include "lab_window.h"
#include "scenario_runner.h"

namespace epocanva {
namespace tsf {
namespace lab {

namespace {

int g_total = 0;
int g_failed = 0;

void Check(bool condition, const std::string& what,
           const std::string& got = std::string()) {
  ++g_total;
  if (condition) {
    std::printf("[lab-win] PASS %s\n", what.c_str());
    return;
  }
  ++g_failed;
  std::printf("[lab-win] FAIL %s", what.c_str());
  if (!got.empty()) {
    std::printf(" (got: %s)", got.c_str());
  }
  std::printf("\n");
}

std::string HrText(HRESULT hr) {
  char buffer[24] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08lX",
                static_cast<unsigned long>(hr));
  return buffer;
}

bool RunWindowLifecycle() {
  LabWindowOptions options;
  options.show = false;
  options.start_session = false;
  options.use_window_focus = false;

  HWND top = nullptr;
  HWND document_a = nullptr;
  HWND document_b = nullptr;
  {
    std::string error;
    std::unique_ptr<LabWindow> window = LabWindow::Create(options, &error);
    Check(window != nullptr, "window lifecycle: lab window created", error);
    if (window == nullptr) {
      return false;
    }
    top = window->hwnd();
    document_a = window->document_window(0);
    document_b = window->document_window(1);
    Check(top != nullptr && ::IsWindow(top),
          "window lifecycle: top-level HWND exists");
    Check(document_a != nullptr && ::IsWindow(document_a) &&
              document_b != nullptr && ::IsWindow(document_b),
          "window lifecycle: both document HWNDs exist");
    Check(window->nccreate_messages() == 1 &&
              window->create_messages() == 1,
          "window lifecycle: WM_NCCREATE then WM_CREATE ran once",
          "nccreate=" + std::to_string(window->nccreate_messages()) +
              " create=" + std::to_string(window->create_messages()));
    Check(window->status_window() != nullptr &&
              ::IsWindow(window->status_window()),
          "window lifecycle: status control exists");
    Check(::GetWindowLongPtrW(top, GWLP_USERDATA) != 0,
          "window lifecycle: the state pointer is bound during creation");
    Check(window->classes_registered(),
          "window lifecycle: the window classes are registered");
    window->Destroy();
    Check(!::IsWindow(top), "window lifecycle: the top-level HWND is destroyed");
    Check(!::IsWindow(document_a) && !::IsWindow(document_b),
          "window lifecycle: both document HWNDs are destroyed");
    Check(window->ncdestroy_messages() == 1,
          "window lifecycle: WM_NCDESTROY ran once",
          std::to_string(window->ncdestroy_messages()));
    Check(!window->classes_registered(),
          "window lifecycle: the window classes were released");
    Check(!window->create_rejected(),
          "window lifecycle: the window procedure did not reject creation",
          window->create_error());
  }
  Check(LabWindow::LiveWindowCount() == 0,
        "window lifecycle: no live lab window remains",
        std::to_string(LabWindow::LiveWindowCount()));
  Check(!LabWindow::ClassesRegistered(),
        "window lifecycle: the class registry is empty");

  // Re-create after the unregister to prove the class path can be entered again.
  {
    std::string error;
    std::unique_ptr<LabWindow> second = LabWindow::Create(options, &error);
    Check(second != nullptr,
          "window lifecycle: a second window registers the classes again", error);
    if (second != nullptr) {
      Check(second->nccreate_messages() == 1 &&
                second->create_messages() == 1,
            "window lifecycle: the second window ran creation messages once");
      second->Destroy();
    }
  }
  Check(LabWindow::LiveWindowCount() == 0,
        "window lifecycle: the second window is gone",
        std::to_string(LabWindow::LiveWindowCount()));
  return g_failed == 0;
}

bool CheckCleanupReports(LabSession* session) {
  if (session == nullptr) {
    Check(false, "resource balance: the session still exists");
    return false;
  }
  Check(session->start_status() == engine::StartStatus::kOk,
        "resource balance: the executor started",
        engine::DescribeStartStatus(session->start_status()));
  Check(session->stop_status() == engine::StopStatus::kOk,
        "resource balance: the executor stopped and joined",
        engine::DescribeStopStatus(session->stop_status()));
  const engine::ExecutorRuntimeReport& runtime = session->runtime_report();
  Check(runtime.state == engine::ExecutorState::kStopped,
        "resource balance: the executor reached Stopped");
  Check(runtime.runtime_start_status == engine::RuntimeStartStatus::kOk,
        "resource balance: the engine runtime started");
  Check(runtime.runtime_shutdown_status == engine::RuntimeShutdownStatus::kOk,
        "resource balance: the engine runtime shut down");
  Check(runtime.runtime_info.initialize_calls == 1 &&
            runtime.runtime_info.finalize_calls == 1,
        "resource balance: initialize/finalize are paired",
        "initialize=" + std::to_string(runtime.runtime_info.initialize_calls) +
            " finalize=" + std::to_string(runtime.runtime_info.finalize_calls));
  Check(runtime.client_close_failures == 0,
        "resource balance: no client close failed");
  Check(runtime.live_sessions_after_cleanup == 0,
        "resource balance: no engine session remained",
        std::to_string(runtime.live_sessions_after_cleanup));
  Check(runtime.api_counts.balanced(),
        "resource balance: engine API acquire/free counters are balanced");

  const edit::test::TestHostCleanupReport& cleanup = session->cleanup_report();
  Check(cleanup.shutdown_called, "resource balance: the TSF host was shut down");
  Check(cleanup.com_initialized && cleanup.com_uninitialized,
        "resource balance: COM initialized and uninitialized");
  Check(cleanup.window_created && cleanup.window_destroyed,
        "resource balance: the host message window was destroyed");
  Check(cleanup.thread_manager_created && cleanup.thread_manager_deactivated &&
            cleanup.thread_manager_released,
        "resource balance: the thread manager was deactivated and released");
  Check(cleanup.documents_created == 2 && cleanup.documents_destroyed == 2,
        "resource balance: both documents were created and destroyed",
        std::to_string(cleanup.documents_created) + " vs " +
            std::to_string(cleanup.documents_destroyed));
  Check(cleanup.all_documents_disconnected &&
            cleanup.all_document_managers_released &&
            cleanup.all_contexts_released,
        "resource balance: contexts and document managers were released");
  Check(cleanup.document_managers_still_enumerated == 0,
        "resource balance: no document manager remained enumerated",
        std::to_string(cleanup.document_managers_still_enumerated));
  Check(cleanup.store_instances_alive == 0,
        "resource balance: no text store remained alive",
        std::to_string(cleanup.store_instances_alive));
  Check(cleanup.advised_store_sinks_alive == 0,
        "resource balance: no store sink remained advised",
        std::to_string(cleanup.advised_store_sinks_alive));
  Check(cleanup.forced_store_sink_releases == 0,
        "resource balance: no sink had to be force-released",
        std::to_string(cleanup.forced_store_sink_releases));
  Check(cleanup.store_reference_count_after_shutdown == 0,
        "resource balance: the last store reference count is zero",
        std::to_string(cleanup.store_reference_count_after_shutdown));

  const LabSnapshot snapshot = session->Snapshot();
  Check(snapshot.retire_drain_complete && !snapshot.retire_drain_timed_out,
        "resource balance: the retirement drain completed before StopAndJoin",
        "complete=" + std::to_string(snapshot.retire_drain_complete ? 1 : 0) +
            " timed_out=" + std::to_string(snapshot.retire_drain_timed_out ? 1 : 0));
  Check(snapshot.retirement.retiring_bindings == 0,
        "resource balance: no retiring binding remained",
        std::to_string(snapshot.retirement.retiring_bindings));
  Check(snapshot.client_usage_after_drain.live == 0,
        "resource balance: no live engine client remained before StopAndJoin",
        std::to_string(snapshot.client_usage_after_drain.live));
  Check(snapshot.client_usage_after_drain.created ==
            snapshot.client_usage_after_drain.closed,
        "resource balance: every created engine client reached the closed state",
        std::to_string(snapshot.client_usage_after_drain.created) + " vs " +
            std::to_string(snapshot.client_usage_after_drain.closed));
  Check(snapshot.capacity_after_drain.occupied == 0,
        "resource balance: no executor result slot remained occupied",
        std::to_string(snapshot.capacity_after_drain.occupied));
  Check(snapshot.client_usage_after_drain.close_requests ==
            snapshot.client_usage_after_drain.close_results_taken,
        "resource balance: every accepted close result was consumed",
        std::to_string(snapshot.client_usage_after_drain.close_requests) + "/" +
            std::to_string(snapshot.client_usage_after_drain.close_results_taken));
  for (int index = 0; index < 2; ++index) {
    Check(snapshot.association_cleared[index],
          std::string("resource balance: association ") +
              (index == 0 ? "A" : "B") + " was cleared",
          HrText(snapshot.association_clear_hr[index]));
    Check(SUCCEEDED(snapshot.association_clear_hr[index]),
          std::string("resource balance: clearing association ") +
              (index == 0 ? "A" : "B") + " succeeded",
          HrText(snapshot.association_clear_hr[index]));
    Check(snapshot.association_previous_released[index],
          std::string("resource balance: previous manager for ") +
              (index == 0 ? "A" : "B") + " was released");
  }
  Check(snapshot.engine_clients_created >= 1,
        "resource balance: at least one engine client was created",
        std::to_string(snapshot.engine_clients_created));
  return g_failed == 0;
}

bool RunScenarios(const LabConfig& config) {
  LabWindowOptions options;
  options.config = config;
  options.show = false;
  options.start_session = true;
  options.use_window_focus = false;

  std::string error;
  std::unique_ptr<LabWindow> window = LabWindow::Create(options, &error);
  Check(window != nullptr, "scenarios: hidden lab window created", error);
  if (window == nullptr) {
    return false;
  }
  Check(window->session_ready(), "scenarios: TSF session is ready",
        window->startup_error());
  if (!window->session_ready()) {
    window->Destroy();
    return false;
  }
  ScenarioRunner* runner = window->runner();
  Check(runner != nullptr, "scenarios: the fixed scenario runner exists");
  if (runner == nullptr) {
    window->Destroy();
    return false;
  }
  runner->SetProgress([](const std::string& line) {
    std::printf("[lab-win] evidence %s\n", line.c_str());
  });

  bool all_ok = true;
  for (int scenario = 1; scenario <= 9; ++scenario) {
    const ScenarioResult result =
        runner->Run(static_cast<ScenarioId>(scenario));
    Check(result.ok(),
          std::string("scenario ") + ScenarioRunner::Name(
              static_cast<ScenarioId>(scenario)) + " completed",
          result.detail);
    if (!result.ok()) {
      all_ok = false;
      break;
    }
  }

  LabSession* session = window->session();
  session->Shutdown();
  CheckCleanupReports(session);
  window->ShutdownSession();
  window->Destroy();
  window.reset();
  Check(LabWindow::LiveWindowCount() == 0,
        "scenarios: no live lab window remains",
        std::to_string(LabWindow::LiveWindowCount()));
  return all_ok && g_failed == 0;
}

}  // namespace

int RunLabSelfTest(const LabConfig& config, const std::string& case_name) {
  g_total = 0;
  g_failed = 0;
  std::printf("[lab-win] self-test case=%s\n", case_name.c_str());
  bool ok = true;
  if (case_name == "window-lifecycle" || case_name == "all") {
    ok = RunWindowLifecycle() && ok;
  }
  if (case_name == "scenarios" || case_name == "all") {
    ok = RunScenarios(config) && ok;
  }
  if (case_name != "window-lifecycle" && case_name != "scenarios" &&
      case_name != "all") {
    std::printf("[lab-win] unknown case: %s\n", case_name.c_str());
    return 2;
  }
  std::printf("[lab-win] RESULT %s %d checks, %d failed\n",
              g_failed == 0 ? "PASS" : "FAIL", g_total, g_failed);
  if (g_failed == 0 && ok) {
    std::printf("RESULT PASS tsf_lab %d checks\n", g_total);
    return 0;
  }
  return 5;
}

}  // namespace lab
}  // namespace tsf
}  // namespace epocanva
