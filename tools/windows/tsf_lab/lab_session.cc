// EpoCanva P3.11 -- implementation of the reusable TSF lab session. See lab_session.h.
#include "lab_session.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <utility>

#include "engine_executor/test_support.h"
#include "observer_test_support.h"

namespace epocanva {
namespace tsf {
namespace lab {

namespace {

namespace ex = ::epocanva::tests::engine_executor;
namespace oct = ::epocanva::tsf::context::test;
using edit::ContextIdentity;

std::string HrText(HRESULT hr) {
  char buffer[24] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08lX",
                static_cast<unsigned long>(hr));
  return buffer;
}

bool SameComObject(IUnknown* left, IUnknown* right) {
  if (left == nullptr || right == nullptr) {
    return false;
  }
  IUnknown* left_unknown = nullptr;
  IUnknown* right_unknown = nullptr;
  left->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&left_unknown));
  right->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&right_unknown));
  const bool same = left_unknown != nullptr && right_unknown != nullptr &&
                    left_unknown == right_unknown;
  if (left_unknown != nullptr) {
    left_unknown->Release();
  }
  if (right_unknown != nullptr) {
    right_unknown->Release();
  }
  return same;
}

input::KeyEventSnapshot FixedEvent(std::uint32_t virtual_key) {
  input::KeyEventSnapshot event;
  event.virtual_key = virtual_key;
  event.scan_code = ::MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC);
  event.is_extended = false;
  event.transition = input::KeyTransition::kKeyDown;
  event.repeat_count = 1;
  event.previously_down = false;
  return event;
}

}  // namespace

// The host's fresh-client source for one observed binding. Every binding gets a new engine
// session; the coordinator's invalidation path never continues an old composition. Old
// clients are not closed here: the executor owns them and StopAndJoin closes them all. The
// lab creates one client per focus change, which is bounded by the experiment session.
class LabSession::LabEngineClients final
    : public coordinator::ObservedEngineClients {
 public:
  LabEngineClients(engine::EngineExecutor& executor, std::string schema_id,
                   engine::ControllerOptions options, DWORD timeout_ms)
      : executor_(executor),
        schema_id_(std::move(schema_id)),
        options_(options),
        timeout_ms_(timeout_ms) {}

  engine::ClientId NextClient() override {
    engine::RequestId request;
    engine::ClientId client;
    const engine::SubmitStatus submitted =
        executor_.TryCreateClient(schema_id_, options_, &request, &client);
    if (submitted != engine::SubmitStatus::kAccepted) {
      error_ = std::string("engine client create refused: ") +
               engine::DescribeSubmitStatus(submitted);
      return engine::ClientId{};
    }
    const engine::WaitStatus waited =
        executor_.WaitForResult(request, std::chrono::milliseconds(timeout_ms_));
    if (waited != engine::WaitStatus::kReady) {
      error_ = std::string("engine client create did not complete: ") +
               engine::DescribeWaitStatus(waited);
      return engine::ClientId{};
    }
    engine::CommandResult result;
    if (executor_.TakeResult(request, &result) != engine::TakeStatus::kTaken) {
      error_ = "engine client create result could not be taken";
      return engine::ClientId{};
    }
    if (!result.ok()) {
      error_ = std::string("engine client create failed: ") +
               engine::DescribeCommandStatus(result.status);
      if (!result.diagnostic.empty()) {
        error_ += " (" + result.diagnostic + ")";
      }
      return engine::ClientId{};
    }
    last_ = client;
    ++created_;
    error_.clear();
    return client;
  }

  engine::ClientId last_client() const { return last_; }
  std::uint64_t created() const { return created_; }
  const std::string& error() const { return error_; }

 private:
  engine::EngineExecutor& executor_;
  std::string schema_id_;
  engine::ControllerOptions options_;
  DWORD timeout_ms_ = 0;
  engine::ClientId last_{};
  std::uint64_t created_ = 0;
  std::string error_;
};

LabSession::LabSession(LabConfig config) : config_(std::move(config)) {}

LabSession::~LabSession() { Shutdown(); }

bool LabSession::Start(const std::array<HWND, 2>& document_windows,
                       std::string* error) {
  if (ready_) {
    if (error != nullptr) {
      *error = "the lab session is already started";
    }
    return false;
  }
  if (shutdown_complete_) {
    if (error != nullptr) {
      *error = "a shut-down lab session cannot be restarted; create a new session";
    }
    return false;
  }
  if (document_windows[0] == nullptr || document_windows[1] == nullptr) {
    if (error != nullptr) {
      *error = "both document windows are required";
    }
    return false;
  }
  document_windows_ = document_windows;

  auto fail = [&](const std::string& message) {
    session_error_ = message;
    if (error != nullptr) {
      *error = message;
    }
    Shutdown();
    return false;
  };

  std::string host_error;
  host_ = edit::test::TestHost::Create(&host_error);
  if (host_ == nullptr) {
    return fail("real TSF host creation failed: " + host_error);
  }

  edit::test::TestDocumentOptions document_options;
  document_options.initial_text_utf16 = L"";
  document_options.initial_selection_start_utf16 = 0;
  document_options.initial_selection_end_utf16 = 0;
  for (int index = 0; index < 2; ++index) {
    std::string document_error;
    documents_[index] = host_->CreateDocument(document_options, &document_error);
    if (documents_[index] == nullptr) {
      return fail("document " + std::string(index == 0 ? "A" : "B") +
                  " creation failed: " + document_error);
    }
  }

  std::string external_error;
  external_client_id_ =
      oct::ExternalClientId(host_->thread_manager_for_test(), &external_error);
  if (external_client_id_ == 0) {
    return fail("external client id unavailable: " + external_error);
  }

  if (!StartExecutor(error)) {
    return fail(session_error_);
  }

  permission_ = context::CreateRealPermissionSource(host_->client_id());
  if (permission_ == nullptr) {
    return fail("real permission source creation failed");
  }
  registry_ = std::make_unique<context::ContextRegistry>(8);
  coordinator_ = std::make_unique<coordinator::ContextCoordinator>(
      *executor_, host_->client_id(), *permission_);
  clients_ = std::make_unique<LabEngineClients>(
      *executor_, config_.schema_id,
      engine::ControllerOptions{true, input::InputPermission::kAllowed},
      config_.engine_create_timeout_ms);
  observer_ = std::make_unique<context::ContextObserver>(*registry_,
                                                        host_->client_id());
  controller_ = std::make_unique<coordinator::ObservedBindingController>(
      *observer_, *coordinator_, *registry_, *clients_);

  if (!AssociateDocuments(error)) {
    return fail(session_error_);
  }

  std::string subscribe_error;
  if (!observer_->Subscribe(host_->thread_manager_for_test(), controller_.get(),
                            &subscribe_error)) {
    return fail("context observer subscribe failed: " + subscribe_error);
  }

  // Adopt an already-focused top context if one exists. Normally there is none until the
  // user clicks a document or a scenario calls the focus path.
  ready_ = true;
  PumpOnce(0);
  session_error_.clear();
  return true;
}

bool LabSession::StartExecutor(std::string* error) {
  ex::Options options;
  options.mode = "tsf-lab";
  options.rime_dll = config_.rime_dll;
  options.fixture_dir = config_.fixture_dir;
  options.spelling_fixture_dir = config_.spelling_fixture_dir;
  options.run_root = config_.run_root;
  if (options.run_root.empty()) {
    if (error != nullptr) {
      *error = "run-root is empty";
    }
    session_error_ = "run-root is empty";
    return false;
  }
  if (options.run_root.filename() != "engine-executor-run") {
    options.run_root /= "engine-executor-run";
  }

  std::filesystem::path shared_dir;
  std::filesystem::path user_dir;
  std::filesystem::path log_dir;
  if (!ex::PrepareRunRoot(options, "tsf-lab", &shared_dir, &user_dir, &log_dir) ||
      !ex::StageFixtures(options, shared_dir)) {
    session_error_ = "engine fixture staging failed";
    if (error != nullptr) {
      *error = session_error_;
    }
    return false;
  }
  if (!ex::PrepareDeployment(options, shared_dir, user_dir, log_dir)) {
    session_error_ = "engine deployment preparation failed";
    if (error != nullptr) {
      *error = session_error_;
    }
    return false;
  }

  engine::ExecutorOptions executor_options;
  executor_options.runtime =
      ex::BaseOptions(options, shared_dir, user_dir, log_dir);
  executor_options.capacity = config_.executor_capacity;
  executor_ = std::make_unique<engine::EngineExecutor>(executor_options);
  start_status_ = executor_->Start();
  if (start_status_ != engine::StartStatus::kOk) {
    runtime_report_ = executor_->RuntimeReport();
    session_error_ = std::string("engine executor start failed: ") +
                     engine::DescribeStartStatus(start_status_);
    if (!runtime_report_.diagnostic.empty()) {
      session_error_ += " (" + runtime_report_.diagnostic + ")";
    }
    if (error != nullptr) {
      *error = session_error_;
    }
    return false;
  }
  return true;
}

bool LabSession::AssociateDocuments(std::string* error) {
  ITfThreadMgr* thread_manager = host_->thread_manager_for_test();
  if (thread_manager == nullptr) {
    session_error_ = "no thread manager for window association";
    if (error != nullptr) {
      *error = session_error_;
    }
    return false;
  }
  for (int index = 0; index < 2; ++index) {
    ITfDocumentMgr* previous = nullptr;
    const HRESULT hr = thread_manager->AssociateFocus(
        document_windows_[index],
        documents_[index]->document_manager_for_test(), &previous);
    if (previous != nullptr) {
      // AssociateFocus returns the previously associated manager AddRef'd; the caller
      // owns that reference.
      previous->Release();
      association_previous_released_[index] = true;
    } else {
      association_previous_released_[index] = true;
    }
    if (FAILED(hr)) {
      session_error_ = "AssociateFocus(document " +
                       std::string(index == 0 ? "A" : "B") +
                       ") failed hr=" + HrText(hr);
      if (error != nullptr) {
        *error = session_error_;
      }
      return false;
    }
    associated_[index] = true;
  }
  return true;
}

void LabSession::Shutdown() {
  if (shutdown_complete_) {
    return;
  }
  ready_ = false;
  if (clients_ != nullptr) {
    engine_clients_created_cache_ = clients_->created();
  }

  // A notification must never reach a destroyed controller.
  controller_.reset();
  if (observer_ != nullptr) {
    observer_->Shutdown();
    observer_.reset();
  }

  // Clear the associations while TSF and the document managers are still alive. The
  // returned previous managers carry a reference the caller must release.
  ClearAssociations();

  if (coordinator_ != nullptr) {
    // Retire the current binding explicitly and consume its engine result/close before
    // StopAndJoin. The observer is already shut down and the associations are cleared, so
    // this drain cannot rebind anything.
    coordinator_->InvalidateBinding();
    DrainRetirementsBeforeStop();
    retirement_cache_ = coordinator_->Snapshot().retirement;
    coordinator_->Close();
    coordinator_.reset();
  }
  if (executor_ != nullptr) {
    client_usage_cache_ = executor_->clients();
    capacity_cache_ = executor_->capacity();
    client_usage_after_drain_ = client_usage_cache_;
    capacity_after_drain_ = capacity_cache_;
  }
  clients_.reset();
  if (executor_ != nullptr) {
    stop_status_ = executor_->StopAndJoin();
    runtime_report_ = executor_->RuntimeReport();
    executor_.reset();
  }
  permission_.reset();
  if (registry_ != nullptr) {
    // The registry holds one context reference per identity; release them before the host
    // pops its contexts.
    registry_->ReleaseAll();
    registry_.reset();
  }
  if (host_ != nullptr) {
    host_->Shutdown();
    cleanup_report_ = host_->cleanup_report();
    host_.reset();
  }

  documents_ = {};
  external_client_id_ = 0;
  engine_state_valid_ = false;
  last_bound_context_id_ = 0;
  last_bound_generation_ = 0;
  bound_client_ = engine::ClientId{};
  ready_ = false;
  shutdown_complete_ = true;
}

void LabSession::ClearAssociations() {
  if (host_ == nullptr) {
    return;
  }
  ITfThreadMgr* thread_manager = host_->thread_manager_for_test();
  if (thread_manager == nullptr) {
    return;
  }
  // Focus is not left pointing at a document whose manager is about to be released.
  thread_manager->SetFocus(nullptr);
  for (int index = 0; index < 2; ++index) {
    if (!associated_[index]) {
      continue;
    }
    ITfDocumentMgr* previous = nullptr;
    const HRESULT hr = thread_manager->AssociateFocus(
        document_windows_[index], nullptr, &previous);
    association_clear_hr_[index] = hr;
    association_cleared_[index] = SUCCEEDED(hr);
    if (previous != nullptr) {
      previous->Release();
      association_previous_released_[index] = true;
    } else {
      association_previous_released_[index] = true;
    }
    associated_[index] = false;
  }
}

void LabSession::DispatchOnly() {
  MSG message = {};
  while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
    ::TranslateMessage(&message);
    ::DispatchMessageW(&message);
  }
}

void LabSession::DrainRetirementsBeforeStop() {
  retire_drain_complete_ = false;
  retire_drain_timed_out_ = false;
  const DWORD timeout =
      config_.step_timeout_ms == 0 ? 30000 : config_.step_timeout_ms;
  if (PumpRetirementsUntilIdle(timeout)) {
    retire_drain_complete_ = true;
  } else {
    retire_drain_timed_out_ = true;
  }
}

bool LabSession::PumpRetirementsUntilIdle(DWORD timeout_ms) {
  if (coordinator_ == nullptr || executor_ == nullptr) {
    return true;
  }
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    coordinator_->Pump();
    const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
    if (snapshot.retirement.retiring_bindings == 0) {
      return true;
    }
    if (::GetTickCount64() >= deadline) {
      return false;
    }
    const coordinator::RetirementSnapshot& retirement = snapshot.retirement;
    if (retirement.pending_edit_event != nullptr) {
      HANDLE event = retirement.pending_edit_event;
      ::MsgWaitForMultipleObjects(1, &event, FALSE, 20, QS_ALLINPUT);
      DispatchOnly();
    } else if (retirement.pending_close_request.valid()) {
      executor_->WaitForResult(retirement.pending_close_request,
                               std::chrono::milliseconds(20));
      DispatchOnly();
    } else if (retirement.pending_engine_request.valid()) {
      executor_->WaitForResult(retirement.pending_engine_request,
                               std::chrono::milliseconds(20));
      DispatchOnly();
    } else {
      ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
      DispatchOnly();
    }
  }
}

void LabSession::PumpOnce(DWORD timeout_ms) {
  if (!ready_) {
    return;
  }
  const context::ContextObserverSnapshot before = observer_->Snapshot();
  if (timeout_ms > 0) {
    ::MsgWaitForMultipleObjects(0, nullptr, FALSE, timeout_ms, QS_ALLINPUT);
  }
  MSG message = {};
  while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
    ::TranslateMessage(&message);
    ::DispatchMessageW(&message);
  }
  observer_->Pump();
  coordinator_->Pump();

  const coordinator::CoordinatorSnapshot coordinator = coordinator_->Snapshot();
  if (coordinator.bound) {
    if (coordinator.identity.context_id != last_bound_context_id_ ||
        coordinator.identity.generation != last_bound_generation_) {
      const context::ContextObserverSnapshot after = observer_->Snapshot();
      if (after.focus_notifications > before.focus_notifications) {
        last_bind_path_ = "notification";
      } else if (after.focus_revalidations > before.focus_revalidations) {
        last_bind_path_ = "revalidation";
      } else {
        last_bind_path_ = "initial-adoption";
      }
      engine_state_valid_ = false;
      last_bound_context_id_ = coordinator.identity.context_id;
      last_bound_generation_ = coordinator.identity.generation;
      bound_client_ = clients_ != nullptr ? clients_->last_client()
                                          : engine::ClientId{};
    }
  } else {
    if (last_bound_context_id_ != 0) {
      engine_state_valid_ = false;
    }
    last_bound_context_id_ = 0;
    last_bound_generation_ = 0;
    bound_client_ = engine::ClientId{};
  }
  if (ui_tick_) {
    ui_tick_();
  }
}

bool LabSession::IsTsfFocused(int index) {
  if (!ready_ && host_ == nullptr) {
    return false;
  }
  if (index < 0 || index > 1 || documents_[index] == nullptr ||
      documents_[index]->document_manager_for_test() == nullptr) {
    return false;
  }
  ITfThreadMgr* thread_manager = host_->thread_manager_for_test();
  if (thread_manager == nullptr) {
    return false;
  }
  ITfDocumentMgr* focus = nullptr;
  const HRESULT hr = thread_manager->GetFocus(&focus);
  if (FAILED(hr) || focus == nullptr) {
    return false;
  }
  const bool same = SameComObject(
      focus, documents_[index]->document_manager_for_test());
  focus->Release();
  return same;
}

void LabSession::OnDocumentWindowFocused(int index) {
  if (index < 0 || index > 1) {
    return;
  }
  last_focus_target_ = index;
  ++focus_calls_;
  if (!ready_) {
    return;
  }
  if (IsTsfFocused(index)) {
    set_focus_hr_ = S_FALSE;
    return;
  }
  ITfDocumentMgr* manager = documents_[index]->document_manager_for_test();
  set_focus_hr_ = host_->thread_manager_for_test()->SetFocus(manager);
}

bool LabSession::FocusDocumentProgrammatically(int index, std::string* error) {
  if (!ready_) {
    if (error != nullptr) {
      *error = "the lab session is not ready";
    }
    return false;
  }
  if (index < 0 || index > 1 || documents_[index] == nullptr) {
    if (error != nullptr) {
      *error = "invalid document index";
    }
    return false;
  }
  last_focus_target_ = index;
  ++focus_calls_;
  set_focus_hr_ = host_->thread_manager_for_test()->SetFocus(
      documents_[index]->document_manager_for_test());
  if (FAILED(set_focus_hr_)) {
    if (error != nullptr) {
      *error = "ITfThreadMgr::SetFocus failed hr=" + HrText(set_focus_hr_);
    }
    return false;
  }
  return WaitForBinding(index, config_.step_timeout_ms, error);
}

bool LabSession::EnsureDocumentWindowFocus(int index, std::string* error) {
  if (!ready_) {
    if (error != nullptr) {
      *error = "the lab session is not ready";
    }
    return false;
  }
  if (index < 0 || index > 1 || document_windows_[index] == nullptr) {
    if (error != nullptr) {
      *error = "invalid document window";
    }
    return false;
  }
  HWND window = document_windows_[index];
  if (::GetFocus() != window) {
    ::SetFocus(window);
    // Capture the failure value before any other API can overwrite it.
    const DWORD focus_error = ::GetLastError();
    if (::GetFocus() != window) {
      if (error != nullptr) {
        *error = "Win32 SetFocus(document) failed err=" +
                 std::to_string(focus_error);
      }
      return false;
    }
  }
  if (!IsTsfFocused(index)) {
    // The WM_SETFOCUS handler normally did this already. If the window already had Win32
    // focus, no message is sent, so issue the real TSF call here.
    OnDocumentWindowFocused(index);
  }
  return WaitForBinding(index, config_.step_timeout_ms, error);
}

bool LabSession::WaitForBinding(int index, DWORD timeout_ms, std::string* error) {
  if (!ready_) {
    if (error != nullptr) {
      *error = "the lab session is not ready";
    }
    return false;
  }
  if (index < 0 || index > 1 || documents_[index] == nullptr) {
    if (error != nullptr) {
      *error = "invalid document index";
    }
    return false;
  }
  context::ContextIdRecord record;
  const context::ContextRegistryStatus observed =
      registry_->Observe(documents_[index]->context(), &record);
  if (observed != context::ContextRegistryStatus::kOk) {
    if (error != nullptr) {
      *error = std::string("context identity lookup failed: ") +
               context::DescribeContextRegistryStatus(observed);
    }
    return false;
  }
  const std::uint64_t minimum_generation =
      record.generation == 0 ? 1 : record.generation;
  return WaitForRebind(index, minimum_generation, timeout_ms, error);
}

bool LabSession::WaitForRebind(int index, std::uint64_t minimum_generation,
                               DWORD timeout_ms, std::string* error) {
  if (!ready_ || index < 0 || index > 1 || documents_[index] == nullptr) {
    if (error != nullptr) {
      *error = "cannot wait for a binding on this document";
    }
    return false;
  }
  context::ContextIdRecord record;
  const context::ContextRegistryStatus observed =
      registry_->Observe(documents_[index]->context(), &record);
  if (observed != context::ContextRegistryStatus::kOk) {
    if (error != nullptr) {
      *error = std::string("context identity lookup failed: ") +
               context::DescribeContextRegistryStatus(observed);
    }
    return false;
  }
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    PumpOnce(0);
    const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
    if (snapshot.bound && snapshot.identity.context_id == record.context_id &&
        snapshot.identity.generation >= minimum_generation) {
      // A binding is not "ready" while the previous binding still owes a close or a
      // composition cleanup: input would be answered kRetiring. Wait on the real
      // completion facts, bounded by the same focus timeout.
      const ULONGLONG now = ::GetTickCount64();
      const DWORD remaining =
          now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
      if (!PumpRetirementsUntilIdle(remaining)) {
        if (error != nullptr) {
          *error = "the binding was admitted but the previous binding's retirement "
                   "did not reach a terminal state";
        }
        return false;
      }
      return true;
    }
    if (registry_->IsUncertain(documents_[index]->context())) {
      if (error != nullptr) {
        *error = "binding refused: this context is latched Uncertain";
      }
      return false;
    }
    if (::GetTickCount64() >= deadline) {
      const context::ContextObserverSnapshot observer = observer_->Snapshot();
      if (error != nullptr) {
        *error = "timed out waiting for document " +
                 std::string(index == 0 ? "A" : "B") +
                 " to bind: context_id=" + std::to_string(record.context_id) +
                 " generation=" + std::to_string(snapshot.identity.generation) +
                 " bound=" + (snapshot.bound ? "1" : "0") +
                 " observer_refusal=" +
                 context::DescribeContextDropReason(observer.last_refusal_reason);
      }
      return false;
    }
    PumpOnce(10);
  }
}

bool LabSession::PumpUntilEvent(HANDLE event, DWORD timeout_ms, bool* cancelled) {
  if (cancelled != nullptr) {
    *cancelled = false;
  }
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    if (Cancelled()) {
      if (cancelled != nullptr) {
        *cancelled = true;
      }
      return false;
    }
    const ULONGLONG now = ::GetTickCount64();
    if (now >= deadline) {
      PumpOnce(0);
      return false;
    }
    const DWORD slice =
        static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 20));
    if (event == nullptr) {
      PumpOnce(slice);
      continue;
    }
    const DWORD wait = ::MsgWaitForMultipleObjects(1, &event, FALSE, slice,
                                                   QS_ALLINPUT);
    if (wait == WAIT_OBJECT_0) {
      PumpOnce(0);
      return true;
    }
    if (wait == WAIT_OBJECT_0 + 1) {
      PumpOnce(0);
      continue;
    }
    if (wait == WAIT_TIMEOUT) {
      PumpOnce(0);
      continue;
    }
    PumpOnce(0);
    if (Cancelled()) {
      if (cancelled != nullptr) {
        *cancelled = true;
      }
    }
    return false;
  }
}

LabSession::DriveResult LabSession::DriveUntilTerminal(DWORD timeout_ms) {
  return DriveUntilPhase(coordinator::WorkPhase::kIdle, timeout_ms);
}

LabSession::DriveResult LabSession::DriveUntilPhase(
    coordinator::WorkPhase phase, DWORD timeout_ms) {
  DriveResult result;
  if (!ready_) {
    result.timed_out = true;
    return result;
  }
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    if (Cancelled()) {
      result.cancelled = true;
      result.final_state = coordinator_->Snapshot();
      return result;
    }
    const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
    result.final_state = snapshot;
    if (snapshot.phase == phase) {
      result.terminal = snapshot.phase == coordinator::WorkPhase::kIdle;
      return result;
    }
    if (snapshot.phase == coordinator::WorkPhase::kIdle) {
      // A work item that finished before the requested intermediate phase was observed.
      result.terminal = true;
      return result;
    }
    bool cancelled = false;
    switch (snapshot.phase) {
      case coordinator::WorkPhase::kEnginePending:
        executor_->WaitForResult(snapshot.engine_request,
                                 std::chrono::milliseconds(10));
        PumpOnce(0);
        break;
      case coordinator::WorkPhase::kEditPending:
        PumpUntilEvent(coordinator_->EditCompletionEvent(), 20, &cancelled);
        break;
      case coordinator::WorkPhase::kPermissionPending:
        PumpUntilEvent(coordinator_->PermissionCompletionEvent(), 20,
                       &cancelled);
        break;
      case coordinator::WorkPhase::kIdle:
        PumpOnce(0);
        break;
    }
    if (cancelled) {
      result.cancelled = true;
      result.final_state = coordinator_->Snapshot();
      return result;
    }
    if (::GetTickCount64() >= deadline) {
      result.timed_out = true;
      result.final_state = coordinator_->Snapshot();
      return result;
    }
  }
}

coordinator::SubmitStatus LabSession::SubmitFixedText(int index,
                                                      wchar_t character,
                                                      coordinator::WorkId* id) {
  (void)index;
  if (!ready_ || coordinator_ == nullptr) {
    return coordinator::SubmitStatus::kNotBound;
  }
  const std::uint32_t virtual_key =
      static_cast<std::uint32_t>(std::towupper(character));
  input::KeyEventSnapshot event = FixedEvent(virtual_key);
  input::KeyText text;
  text.status = input::KeyTextStatus::kText;
  text.utf16_code_units.push_back(static_cast<char16_t>(character));
  return coordinator_->SubmitKey(event, text, id);
}

coordinator::SubmitStatus LabSession::SubmitFixedKey(
    int index, std::uint32_t virtual_key, coordinator::WorkId* id) {
  (void)index;
  if (!ready_ || coordinator_ == nullptr) {
    return coordinator::SubmitStatus::kNotBound;
  }
  input::KeyEventSnapshot event = FixedEvent(virtual_key);
  input::KeyText text;
  text.status = input::KeyTextStatus::kNoText;
  return coordinator_->SubmitKey(event, text, id);
}

coordinator::SubmitStatus LabSession::SubmitCandidate(
    int index, std::size_t candidate_index, coordinator::WorkId* id) {
  (void)index;
  if (!ready_ || coordinator_ == nullptr) {
    return coordinator::SubmitStatus::kNotBound;
  }
  return coordinator_->SubmitCandidate(candidate_index, id);
}

bool LabSession::RefreshEngineState(DWORD timeout_ms, std::string* error) {
  if (!ready_ || executor_ == nullptr || clients_ == nullptr ||
      coordinator_ == nullptr) {
    if (error != nullptr) {
      *error = "the lab session is not ready";
    }
    return false;
  }
  if (coordinator_->Snapshot().phase != coordinator::WorkPhase::kIdle) {
    if (error != nullptr) {
      *error = "a coordinator work item is still pending";
    }
    return false;
  }
  const engine::ClientId client = bound_client_;
  if (!client.valid()) {
    if (error != nullptr) {
      *error = "no engine client is bound to the current context";
    }
    return false;
  }
  engine::RequestId request;
  const engine::SubmitStatus submitted = executor_->TryRefreshState(client, &request);
  if (submitted != engine::SubmitStatus::kAccepted) {
    if (error != nullptr) {
      *error = std::string("refresh submission refused: ") +
               engine::DescribeSubmitStatus(submitted);
    }
    return false;
  }
  const engine::WaitStatus waited =
      executor_->WaitForResult(request, std::chrono::milliseconds(timeout_ms));
  engine::CommandResult result;
  if (waited != engine::WaitStatus::kReady) {
    // A timeout is not a cancellation. If the worker finished just after the wait, take
    // the result now instead of leaving an occupied executor slot behind.
    if (executor_->TakeResult(request, &result) != engine::TakeStatus::kTaken) {
      if (error != nullptr) {
        *error = std::string("refresh did not complete: ") +
                 engine::DescribeWaitStatus(waited);
      }
      return false;
    }
  } else if (executor_->TakeResult(request, &result) != engine::TakeStatus::kTaken) {
    if (error != nullptr) {
      *error = "refresh result could not be taken";
    }
    return false;
  }
  if (!result.ok()) {
    if (error != nullptr) {
      *error = std::string("refresh failed: ") +
               engine::DescribeCommandStatus(result.status);
    }
    return false;
  }
  const engine::RefreshOutcome* refresh =
      std::get_if<engine::RefreshOutcome>(&result.payload);
  if (refresh == nullptr || refresh->controller_status != engine::ControllerStatus::kOk) {
    if (error != nullptr) {
      *error = std::string("refresh controller status: ") +
               (refresh == nullptr
                    ? "missing-payload"
                    : engine::DescribeControllerStatus(refresh->controller_status));
    }
    return false;
  }
  engine_state_ = refresh->state;
  engine_state_valid_ = refresh->state.valid;
  if (!engine_state_valid_) {
    engine_error_ = "the engine state read was not valid";
    if (error != nullptr) {
      *error = engine_error_;
    }
    return false;
  }
  engine_error_.clear();
  return true;
}

bool LabSession::FindCandidate(const std::string& ascii_utf8,
                               std::size_t* index) const {
  if (!engine_state_valid_ || index == nullptr) {
    return false;
  }
  const std::u16string needle(ascii_utf8.begin(), ascii_utf8.end());
  for (std::size_t candidate = 0;
       candidate < engine_state_.candidates.size(); ++candidate) {
    if (engine_state_.candidates[candidate].text_utf16_code_units == needle) {
      *index = candidate;
      return true;
    }
  }
  return false;
}

bool LabSession::SetReadOnly(int index, bool read_only) {
  if (!ready_ || index < 0 || index > 1 || documents_[index] == nullptr) {
    return false;
  }
  documents_[index]->SetReadOnly(read_only);
  return true;
}

bool LabSession::SetMutationFault(int index, edit::test::MutationFault fault) {
  if (!ready_ || index < 0 || index > 1 || documents_[index] == nullptr ||
      documents_[index]->store() == nullptr) {
    return false;
  }
  documents_[index]->store()->SetMutationFault(fault);
  return true;
}

bool LabSession::SetAsyncLocks(int index, bool enabled) {
  if (!ready_ || index < 0 || index > 1 || documents_[index] == nullptr ||
      documents_[index]->store() == nullptr) {
    return false;
  }
  documents_[index]->store()->SetAsyncLocks(enabled);
  return true;
}

void LabSession::SetHoldGrantMessages(bool hold) {
  if (host_ != nullptr) {
    host_->SetHoldGrantMessages(hold);
  }
}

std::size_t LabSession::ReleaseHeldGrantMessages() {
  return host_ != nullptr ? host_->ReleaseHeldGrantMessages() : 0;
}

bool LabSession::InsertExternalText(int index, const std::wstring& text,
                                    std::string* error) {
  if (!ready_ || index < 0 || index > 1 || documents_[index] == nullptr) {
    if (error != nullptr) {
      *error = "invalid document for an external edit";
    }
    return false;
  }
  if (external_client_id_ == 0) {
    if (error != nullptr) {
      *error = "no distinct external client id";
    }
    return false;
  }
  edit::test::TestDocument* document = documents_[index];
  if (document->selection_start_utf16() != document->selection_end_utf16()) {
    if (error != nullptr) {
      *error = "the external insert requires a collapsed selection";
    }
    return false;
  }
  const LONG caret = document->selection_start_utf16();
  oct::ExternalEditResult result;
  std::string detail;
  const bool ok = oct::RunExternalEdit(
      *host_, document, external_client_id_, oct::ExternalEditMode::kInsertText,
      caret, caret, text, &result, &detail);
  external_edit_last_hr_ =
      result.completed ? result.edit_hr : result.request_hr;
  if (result.completed) {
    ++external_edits_run_;
  }
  if (!ok) {
    if (error != nullptr) {
      *error = "external edit failed: " + detail +
               " request=" + HrText(result.request_hr) +
               " session=" + HrText(result.session_hr) +
               " edit=" + HrText(result.edit_hr);
    }
    return false;
  }
  // Let the real OnEndEdit notification reach the observer before the caller waits for a
  // rebind. The notification itself is never synthesized.
  PumpOnce(0);
  return true;
}

bool LabSession::ReadDocument(int index, LabDocumentSnapshot* snapshot) {
  if (snapshot == nullptr || index < 0 || index > 1 ||
      documents_[index] == nullptr) {
    return false;
  }
  edit::test::TestDocument* document = documents_[index];
  snapshot->text_utf16 = document->text_utf16();
  snapshot->selection_start_utf16 = document->selection_start_utf16();
  snapshot->selection_end_utf16 = document->selection_end_utf16();
  const edit::test::TestDocumentDiagnostics diagnostics = document->diagnostics();
  snapshot->read_only = diagnostics.store.read_only;
  snapshot->text_length_utf16 =
      static_cast<std::uint64_t>(diagnostics.store.text_length_utf16);
  snapshot->text_mutations = diagnostics.store.text_mutations;
  snapshot->set_text_calls = diagnostics.store.set_text_calls;
  snapshot->composition_start_calls = diagnostics.store.composition_start_calls;
  snapshot->composition_update_calls = diagnostics.store.composition_update_calls;
  snapshot->composition_end_calls = diagnostics.store.composition_end_calls;
  if (registry_ != nullptr) {
    context::ContextIdRecord record;
    if (registry_->Observe(document->context(), &record) ==
        context::ContextRegistryStatus::kOk) {
      snapshot->context_id = record.context_id;
      snapshot->generation = record.generation;
      snapshot->registry_ok = true;
    }
  }
  return true;
}

LabSnapshot LabSession::Snapshot() {
  LabSnapshot snapshot;
  snapshot.ready = ready_;
  snapshot.session_error = session_error_;
  snapshot.association_cleared[0] = association_cleared_[0];
  snapshot.association_cleared[1] = association_cleared_[1];
  snapshot.association_clear_hr[0] = association_clear_hr_[0];
  snapshot.association_clear_hr[1] = association_clear_hr_[1];
  snapshot.association_previous_released[0] = association_previous_released_[0];
  snapshot.association_previous_released[1] = association_previous_released_[1];
  snapshot.focus_calls = focus_calls_;
  snapshot.last_set_focus_hr = set_focus_hr_;
  snapshot.last_focus_target = last_focus_target_;
  snapshot.last_bind_path = last_bind_path_;
  snapshot.engine_clients_created =
      clients_ != nullptr ? clients_->created() : engine_clients_created_cache_;
  if (executor_ != nullptr) {
    snapshot.client_usage = executor_->clients();
    snapshot.capacity_usage = executor_->capacity();
  } else {
    snapshot.client_usage = client_usage_cache_;
    snapshot.capacity_usage = capacity_cache_;
  }
  snapshot.retirement = coordinator_ != nullptr
                            ? coordinator_->Snapshot().retirement
                            : retirement_cache_;
  snapshot.retire_drain_complete = retire_drain_complete_;
  snapshot.retire_drain_timed_out = retire_drain_timed_out_;
  snapshot.client_usage_after_drain = client_usage_after_drain_;
  snapshot.capacity_after_drain = capacity_after_drain_;
  snapshot.engine_state_valid = engine_state_valid_;
  snapshot.engine_state = engine_state_;
  snapshot.engine_error =
      clients_ != nullptr && !clients_->error().empty() ? clients_->error()
                                                        : engine_error_;

  for (int index = 0; index < 2; ++index) {
    ReadDocument(index, &snapshot.documents[index]);
  }

  HWND focus = ::GetFocus();
  for (int index = 0; index < 2; ++index) {
    if (focus != nullptr && focus == document_windows_[index]) {
      snapshot.win32_focus = index;
    }
  }
  for (int index = 0; index < 2; ++index) {
    if (IsTsfFocused(index)) {
      snapshot.tsf_focus = index;
      break;
    }
  }

  if (coordinator_ != nullptr) {
    const coordinator::CoordinatorSnapshot coordinator = coordinator_->Snapshot();
    snapshot.bound = coordinator.bound;
    snapshot.closed = coordinator.closed;
    snapshot.gate = coordinator.gate;
    snapshot.identity = coordinator.identity;
    snapshot.permission = coordinator.permission;
    snapshot.work_kind = coordinator.work_kind;
    snapshot.phase = coordinator.phase;
    snapshot.outcome = coordinator.outcome;
    snapshot.cause = coordinator.cause;
    snapshot.work_id = coordinator.work_id;
    snapshot.work_generation = coordinator.work_generation;
    snapshot.engine_request = coordinator.engine_request;
    snapshot.last_edit_status = coordinator.last_edit_status;
    snapshot.last_edit_reason = coordinator.last_edit_reason;
    snapshot.tracked_caret_utf16 = coordinator.tracked_caret_utf16;
    snapshot.composition_active = coordinator.composition_active;
    snapshot.composition_start_utf16 = coordinator.composition_start_utf16;
    snapshot.composition_length_utf16 = coordinator.composition_length_utf16;
    snapshot.works_accepted = coordinator.works_accepted;
    snapshot.engine_results_taken = coordinator.engine_results_taken;
    snapshot.plans_submitted = coordinator.plans_submitted;
    snapshot.edits_applied = coordinator.edits_applied;
    snapshot.edits_refused = coordinator.edits_refused;
    snapshot.edits_stale = coordinator.edits_stale;
    snapshot.permission_queries_begun = coordinator.permission_queries_begun;
    snapshot.permission_queries_completed = coordinator.permission_queries_completed;
    snapshot.permission_rechecks = coordinator.permission_rechecks;
  }
  if (observer_ != nullptr) {
    snapshot.observer = observer_->Snapshot();
  }
  if (controller_ != nullptr) {
    snapshot.binding = controller_->Snapshot();
  }
  return snapshot;
}

std::uint64_t LabSession::engine_clients_created() const {
  return clients_ != nullptr ? clients_->created() : engine_clients_created_cache_;
}

}  // namespace lab
}  // namespace tsf
}  // namespace epocanva
