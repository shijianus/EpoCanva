// EpoCanva P3.11 -- the reusable TSF side of the interactive integration lab.
//
// WHAT THIS OWNS. One real P3.7 TSF host (ITfThreadMgr, two document managers, two
// ITextStoreACP documents), one real English-engine executor, the P3.10 observer and
// registry, the P3.8 coordinator and the observed-binding controller. The two lab
// documents A/B are associated with their own HWNDs through ITfThreadMgr::AssociateFocus;
// the association's returned previous manager is released with the documented ownership
// rule. Nothing here registers an input method, installs a keyboard sink, calls SendInput,
// or writes to another application.
//
// THREADING. Every COM object, every TSF call, every document access and every engine
// submission runs on the one STA thread that created this object. The engine executor owns
// its own worker thread; only value commands and copied results cross that boundary.
//
// FOCUS IS TWO FACTS, DISPLAYED SEPARATELY. `FocusDocumentProgrammatically` calls the real
// ITfThreadMgr::SetFocus and then waits for the observer to bind. `EnsureDocumentWindowFocus`
// is the interactive path: it gives the document child HWND real Win32 focus, then the
// application-side handler calls ITfThreadMgr::SetFocus, then it waits for the binding.
// A successful SetFocus is never reported as an OnSetFocus notification: the lab records
// whether the bind followed a real thread-manager notification or Pump's GetFocus
// revalidation, and the UI shows both.
//
// TEARDOWN. Clear both AssociateFocus associations (releasing the returned previous
// managers), release the observer's sinks, close the coordinator, stop and join the
// executor, release the registry/contexts and only then shut the TSF host down and destroy
// the windows. The cleanup report is kept for the self-test after the objects are gone.
#ifndef EPOCANVA_TOOLS_WINDOWS_TSF_LAB_LAB_SESSION_H_
#define EPOCANVA_TOOLS_WINDOWS_TSF_LAB_LAB_SESSION_H_

#include <windows.h>
#include <msctf.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "context_observer.h"
#include "context_registry.h"
#include "coordinator.h"
#include "engine_executor.h"
#include "engine_update.h"
#include "input_controller.h"
#include "observed_binding.h"
#include "permission_query.h"
#include "test_host.h"
#include "test_text_store.h"

namespace epocanva {
namespace tsf {
namespace lab {

namespace engine = ::epocanva::windows::engine;
namespace input = ::epocanva::windows::input;

struct LabConfig {
  std::string schema_id = "epocanva_test_english";
  std::filesystem::path rime_dll;
  std::filesystem::path fixture_dir;
  std::filesystem::path spelling_fixture_dir;
  // The session creates/uses <run_root>/engine-executor-run, which the executor test
  // scaffolding recreates empty. Nothing is written into the source tree.
  std::filesystem::path run_root;
  std::size_t executor_capacity = 16;
  DWORD engine_create_timeout_ms = 30000;
  DWORD step_timeout_ms = 30000;
};

struct LabDocumentSnapshot {
  std::wstring text_utf16;
  LONG selection_start_utf16 = 0;
  LONG selection_end_utf16 = 0;
  std::uint64_t context_id = 0;
  std::uint64_t generation = 0;
  bool registry_ok = false;
  bool read_only = false;
  std::uint64_t text_length_utf16 = 0;
  std::uint64_t text_mutations = 0;
  std::uint64_t set_text_calls = 0;
  std::uint64_t composition_start_calls = 0;
  std::uint64_t composition_update_calls = 0;
  std::uint64_t composition_end_calls = 0;
};

struct LabSnapshot {
  bool ready = false;
  std::string session_error;

  std::array<LabDocumentSnapshot, 2> documents;

  int win32_focus = -1;
  int tsf_focus = -1;
  std::uint64_t focus_calls = 0;
  HRESULT last_set_focus_hr = S_OK;
  int last_focus_target = -1;

  bool bound = false;
  bool closed = false;
  coordinator::CoordinatorGate gate = coordinator::CoordinatorGate::kOpen;
  edit::ContextIdentity identity{};
  edit::InputPermission permission = edit::InputPermission::kUnknown;
  coordinator::WorkKind work_kind = coordinator::WorkKind::kNone;
  coordinator::WorkPhase phase = coordinator::WorkPhase::kIdle;
  coordinator::WorkOutcome outcome = coordinator::WorkOutcome::kNone;
  coordinator::RejectCause cause = coordinator::RejectCause::kNone;
  std::uint64_t work_id = 0;
  std::uint64_t work_generation = 0;
  engine::RequestId engine_request{};
  edit::EditStatus last_edit_status = edit::EditStatus::kRejected;
  edit::EditRejectReason last_edit_reason = edit::EditRejectReason::kNone;
  LONG tracked_caret_utf16 = 0;
  bool composition_active = false;
  LONG composition_start_utf16 = -1;
  LONG composition_length_utf16 = 0;
  std::uint64_t works_accepted = 0;
  std::uint64_t engine_results_taken = 0;
  std::uint64_t plans_submitted = 0;
  std::uint64_t edits_applied = 0;
  std::uint64_t edits_refused = 0;
  std::uint64_t edits_stale = 0;
  std::uint64_t permission_queries_begun = 0;
  std::uint64_t permission_queries_completed = 0;
  std::uint64_t permission_rechecks = 0;

  context::ContextObserverSnapshot observer;
  coordinator::ObservedBindingController::Diagnostics binding;
  std::uint64_t engine_clients_created = 0;
  std::string engine_error;

  // P3.12: the executor's live-client accounting and the coordinator's bounded retirement
  // ledger. These are the facts the UI and the self-test assert instead of inferring client
  // lifetime from StopAndJoin.
  engine::ClientUsage client_usage;
  engine::CapacityUsage capacity_usage;
  coordinator::RetirementSnapshot retirement;
  // Recorded by Shutdown after the explicit drain and before StopAndJoin.
  bool retire_drain_complete = false;
  bool retire_drain_timed_out = false;
  engine::ClientUsage client_usage_after_drain;
  engine::CapacityUsage capacity_after_drain;

  bool engine_state_valid = false;
  engine::EngineState engine_state;
  // "notification", "revalidation", "initial-adoption", or "none".
  std::string last_bind_path;

  bool association_cleared[2] = {false, false};
  HRESULT association_clear_hr[2] = {S_OK, S_OK};
  bool association_previous_released[2] = {false, false};
};

// One real TSF/engine session over two documents. Not copyable or movable.
class LabSession {
 public:
  explicit LabSession(LabConfig config);
  ~LabSession();

  LabSession(const LabSession&) = delete;
  LabSession& operator=(const LabSession&) = delete;

  // Creates the host, both documents, the executor, the observer/controller and the two
  // window associations. On failure every partially created object is released and the
  // error is text-free. `document_windows` must outlive this session.
  bool Start(const std::array<HWND, 2>& document_windows, std::string* error);
  // Release-only and idempotent. Copies the host cleanup and executor runtime reports
  // before destroying the objects, so they remain inspectable after teardown.
  void Shutdown();
  bool ready() const { return ready_; }

  // Non-blocking UI/scenario pump: dispatch this thread's messages, advance the observer
  // and coordinator, and call the UI tick. Never sleeps to guess a result.
  void PumpOnce(DWORD timeout_ms);

  // Real focus paths. Both end by waiting for the observer-created binding to this
  // document's context identity. They do not fabricate an observer notification.
  bool FocusDocumentProgrammatically(int index, std::string* error);
  bool EnsureDocumentWindowFocus(int index, std::string* error);
  // Called by the document child's WM_SETFOCUS handler. Records the Win32 focus and asks
  // the real thread manager to focus this document.
  void OnDocumentWindowFocused(int index);
  bool WaitForBinding(int index, DWORD timeout_ms, std::string* error);
  bool WaitForRebind(int index, std::uint64_t minimum_generation, DWORD timeout_ms,
                     std::string* error);

  struct DriveResult {
    bool terminal = false;
    bool cancelled = false;
    bool timed_out = false;
    coordinator::CoordinatorSnapshot final_state;
  };
  DriveResult DriveUntilTerminal(DWORD timeout_ms);
  DriveResult DriveUntilPhase(coordinator::WorkPhase phase, DWORD timeout_ms);

  // Fixed-fixture input, submitted explicitly. The KeyText is already translated; no
  // keyboard layout, WM_CHAR, ToUnicodeEx or SendInput participates.
  coordinator::SubmitStatus SubmitFixedText(int index, wchar_t character,
                                             coordinator::WorkId* id);
  coordinator::SubmitStatus SubmitFixedKey(int index, std::uint32_t virtual_key,
                                           coordinator::WorkId* id);
  coordinator::SubmitStatus SubmitCandidate(int index,
                                            std::size_t candidate_index,
                                            coordinator::WorkId* id);

  // Read-only engine refresh (`kRefreshState`) for the composition/candidate display.
  // Never processes a key. Fails cleanly when no client is known or a work is pending.
  bool RefreshEngineState(DWORD timeout_ms, std::string* error);
  bool FindCandidate(const std::string& ascii_utf8, std::size_t* index) const;

  // Host-truth changes for the fixed scenarios. These are the P3.7 test store's documented
  // controls; they do not hand the coordinator a permission verdict. Permission is always
  // re-resolved from the real context metadata by the production PermissionSource.
  bool SetReadOnly(int index, bool read_only);
  bool SetMutationFault(int index, edit::test::MutationFault fault);
  bool SetAsyncLocks(int index, bool enabled);
  void SetHoldGrantMessages(bool hold);
  std::size_t ReleaseHeldGrantMessages();

  // A real read/write edit session from a DIFFERENT real TfClientId, used by the external
  // edit scenario. Text is fixed fixture text supplied by the caller.
  bool InsertExternalText(int index, const std::wstring& text, std::string* error);

  // Value-only state for the window and the self-test.
  LabSnapshot Snapshot();
  const edit::test::TestHostCleanupReport& cleanup_report() const {
    return cleanup_report_;
  }
  const engine::ExecutorRuntimeReport& runtime_report() const {
    return runtime_report_;
  }
  engine::StartStatus start_status() const { return start_status_; }
  engine::StopStatus stop_status() const { return stop_status_; }
  bool shutdown_complete() const { return shutdown_complete_; }
  const LabConfig& config() const { return config_; }
  std::uint64_t engine_clients_created() const;
  std::uint64_t external_edits_run() const { return external_edits_run_; }
  HRESULT external_edit_last_hr() const { return external_edit_last_hr_; }

  void SetUiTick(std::function<void()> tick) { ui_tick_ = std::move(tick); }
  void SetCancelCheck(std::function<bool()> check) { cancel_check_ = std::move(check); }

 private:
  class LabEngineClients;
  friend class LabEngineClients;

  bool StartExecutor(std::string* error);
  bool AssociateDocuments(std::string* error);
  void ClearAssociations();
  // Retires the current binding (if any) and advances the coordinator's retirement ledger
  // with real completion facts, bounded by the configured step timeout. Called during
  // Shutdown after the observer is gone, so it can never rebind.
  void DrainRetirementsBeforeStop();
  bool PumpRetirementsUntilIdle(DWORD timeout_ms);
  void DispatchOnly();
  bool IsTsfFocused(int index);
  bool ReadDocument(int index, LabDocumentSnapshot* snapshot);
  bool PumpUntilEvent(HANDLE event, DWORD timeout_ms, bool* cancelled);
  bool Cancelled() const { return cancel_check_ && cancel_check_(); }

  LabConfig config_;
  std::array<HWND, 2> document_windows_{};
  std::array<edit::test::TestDocument*, 2> documents_{};
  std::uint64_t engine_clients_created_cache_ = 0;
  engine::ClientUsage client_usage_cache_{};
  engine::CapacityUsage capacity_cache_{};
  coordinator::RetirementSnapshot retirement_cache_{};
  bool retire_drain_complete_ = false;
  bool retire_drain_timed_out_ = false;
  engine::ClientUsage client_usage_after_drain_{};
  engine::CapacityUsage capacity_after_drain_{};

  std::unique_ptr<edit::test::TestHost> host_;
  std::unique_ptr<engine::EngineExecutor> executor_;
  std::unique_ptr<context::PermissionSource> permission_;
  std::unique_ptr<context::ContextRegistry> registry_;
  std::unique_ptr<coordinator::ContextCoordinator> coordinator_;
  std::unique_ptr<LabEngineClients> clients_;
  std::unique_ptr<context::ContextObserver> observer_;
  std::unique_ptr<coordinator::ObservedBindingController> controller_;

  TfClientId external_client_id_ = 0;
  std::uint64_t external_edits_run_ = 0;
  HRESULT external_edit_last_hr_ = S_OK;

  std::array<bool, 2> associated_{{false, false}};
  bool association_cleared_[2] = {false, false};
  HRESULT association_clear_hr_[2] = {S_OK, S_OK};
  bool association_previous_released_[2] = {false, false};
  HRESULT set_focus_hr_ = S_OK;
  std::uint64_t focus_calls_ = 0;
  int last_focus_target_ = -1;
  std::string last_bind_path_ = "none";

  std::uint64_t last_bound_context_id_ = 0;
  std::uint64_t last_bound_generation_ = 0;
  engine::ClientId bound_client_{};
  bool engine_state_valid_ = false;
  engine::EngineState engine_state_;
  std::string engine_error_;

  bool ready_ = false;
  bool shutdown_complete_ = false;
  std::string session_error_;
  engine::StartStatus start_status_ = engine::StartStatus::kOk;
  engine::StopStatus stop_status_ = engine::StopStatus::kNotStarted;
  edit::test::TestHostCleanupReport cleanup_report_;
  engine::ExecutorRuntimeReport runtime_report_;

  std::function<void()> ui_tick_;
  std::function<bool()> cancel_check_;
};

}  // namespace lab
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TOOLS_WINDOWS_TSF_LAB_LAB_SESSION_H_
