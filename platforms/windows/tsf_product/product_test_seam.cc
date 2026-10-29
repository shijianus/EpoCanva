// EpoCanva W2-B -- controlled direct-sink test seam implementation.
#include "product_test_seam.h"

#ifdef EPOCANVA_TSF_DIRECT_SINK_TEST

#include <algorithm>
#include <map>
#include <mutex>

#include "candidate_snapshot.h"
#include "module_lifetime.h"
#include "module_state.h"
#include "product_engine_bootstrap.h"
#include "text_service_controller.h"

namespace {

// One controller per STA thread. Two TIPs can be active in one process on different STA
// threads; a single global pointer would answer for the wrong session. Every seam call is
// made on the STA that owns that controller, so the thread id is an exact key.
std::mutex g_controller_mutex;
std::map<DWORD, epocanva::tsf::product::ProductTextController*> g_controllers;
void* g_entered_event = nullptr;
void* g_release_event = nullptr;

epocanva::tsf::product::ProductTextController* CurrentControllerLocked() {
  const auto found = g_controllers.find(::GetCurrentThreadId());
  return found == g_controllers.end() ? nullptr : found->second;
}

}  // namespace

namespace epocanva {
namespace tsf {
namespace product {

void SetTestController(ProductTextController* controller) {
  std::lock_guard<std::mutex> guard(g_controller_mutex);
  const DWORD thread = ::GetCurrentThreadId();
  if (controller == nullptr) {
    g_controllers.erase(thread);
  } else {
    g_controllers[thread] = controller;
  }
}

}  // namespace product
}  // namespace tsf
}  // namespace epocanva

extern "C" int EpoCanvaTestQueryCounters(EpoCanvaTestCounters* out) {
  using namespace epocanva::tsf::product;
  if (out == nullptr) {
    return 1;
  }
  *out = EpoCanvaTestCounters{};
  std::lock_guard<std::mutex> guard(g_controller_mutex);
  ProductTextController* controller = CurrentControllerLocked();
  if (controller == nullptr) {
    return 2;
  }
  const ControllerDiagnostics diagnostics = controller->Snapshot();
  const auto candidates = controller->CandidateSnapshotForTest();
  out->key_downs = diagnostics.key_downs;
  out->eaten = diagnostics.eaten;
  out->literal_key_downs = diagnostics.literal_key_downs;
  out->raw_dropped_keys = 0;
  out->raw_unrecoverable = 0;
  out->raw_applied_units = 0;
  out->raw_cancelled_units = 0;
  out->raw_commits = 0;
  out->raw_ignored_edits = 0;
  out->candidate_version = candidates.version;
  out->candidate_items = candidates.items.size();
  out->candidate_stale = diagnostics.candidate_stale_selections;
  out->candidate_menu_open = candidates.valid ? 1 : 0;
  out->candidate_selected = diagnostics.candidate_selected;
  out->page_no = candidates.page_no;
  out->page_size = candidates.page_size;
  out->is_last_page = candidates.is_last_page ? 1 : 0;
  out->engine_ready = diagnostics.engine_ready ? 1 : 0;
  out->engine_state =
      static_cast<int>(ProductEngineBootstrap::Instance().state());
  out->engine_attach_failures = diagnostics.engine_attach_failures;
  out->queue_pending = diagnostics.suggestion_queue_depth;
  out->queue_in_flight = diagnostics.suggestion_in_flight ? 1 : 0;
  out->coordinator_phase = static_cast<unsigned long long>(diagnostics.coordinator.phase);
  out->engine_requests = diagnostics.engine_client_requests;
  out->engine_results = diagnostics.engine_client_results;
  out->raw_active = 0;
  out->raw_submitted = 0;
  out->works_accepted = diagnostics.coordinator.works_accepted;
  out->target_binds = diagnostics.target_binds;
  out->rebind_requests = diagnostics.rebind_requests;
  out->raw_entered = 0;
  out->last_submit_status = static_cast<unsigned long long>(diagnostics.coordinator.last_submit_status);
  out->gate = static_cast<unsigned long long>(diagnostics.coordinator.gate);
  out->permission = static_cast<unsigned long long>(diagnostics.coordinator.permission);
  out->composition_length = static_cast<unsigned long long>(
      candidates.valid ? candidates.input_utf16_code_units.size()
                       : static_cast<std::size_t>(
                             diagnostics.coordinator.composition_length_utf16));
  out->composition_active =
      (candidates.valid || diagnostics.coordinator.composition_active) ? 1 : 0;
  out->raw_reason_unapplied = 0;
  out->raw_reason_not_candidate = 0;
  out->raw_reason_high_water = 0;
  out->raw_reason_admission = 0;
  out->last_raw_composition_start = 0;
  out->last_raw_composition_units = 0;
  out->last_raw_input_units = 0;
  out->last_raw_plan_units = 0;
  out->raw_unapplied_units = 0;
  out->raw_exits = 0;
  out->raw_failed_keys = 0;
  out->raw_failed_units = 0;
  out->raw_commits = 0;
  out->host_text_keys = diagnostics.host_text_keys;
  out->suggestion_queued = diagnostics.suggestion_queued;
  out->suggestion_submitted = diagnostics.suggestion_submitted;
  out->suggestion_completed = diagnostics.suggestion_completed;
  out->suggestion_dropped = diagnostics.suggestion_dropped;
  out->suggestion_stale = diagnostics.suggestion_stale;
  out->suggestion_resets = diagnostics.suggestion_resets;
  out->suggestion_pending = diagnostics.suggestion_queue_depth;
  out->replacement_attempts = diagnostics.replacement_attempts;
  out->replacement_applied = diagnostics.replacement_applied;
  out->replacement_refused = diagnostics.replacement_refused;
  out->internal_text_losses = diagnostics.internal_text_losses;
  out->host_token_units = diagnostics.host_token_units;
  out->suggestion_active = diagnostics.suggestion_active ? 1 : 0;
  out->suggestion_in_flight = diagnostics.suggestion_in_flight ? 1 : 0;
  out->replacement_pending = diagnostics.replacement_pending ? 1 : 0;
  out->coordinator_bound = diagnostics.coordinator_bound ? 1 : 0;
  out->last_edit_status = static_cast<unsigned long long>(diagnostics.coordinator.last_edit_status);
  out->last_edit_reason = static_cast<unsigned long long>(diagnostics.coordinator.last_edit_reason);
  out->pumps = diagnostics.pumps;
  out->session_pending = diagnostics.coordinator.session_pending ? 1 : 0;
  out->edit_completion_signaled =
      diagnostics.coordinator.edit_completion_signaled ? 1 : 0;
  out->plans_submitted = diagnostics.coordinator.plans_submitted;
  out->edits_applied = diagnostics.coordinator.edits_applied;
  out->edits_refused = diagnostics.coordinator.edits_refused;
  out->edits_stale = diagnostics.coordinator.edits_stale;
  out->work_kind = static_cast<unsigned long long>(diagnostics.coordinator.work_kind);
  out->work_outcome = static_cast<unsigned long long>(diagnostics.coordinator.outcome);
  out->edit_pump_checks = diagnostics.coordinator.edit_pump_checks;
  out->edit_pump_ready = diagnostics.coordinator.edit_pump_ready;
  out->coordinator_pumps = diagnostics.coordinator.pumps;
  out->host_edits_matched = diagnostics.host_edits_matched;
  out->host_edits_unexpected = diagnostics.host_edits_unexpected;
  out->reset_boundary = diagnostics.reset_boundary;
  out->reset_owed = diagnostics.reset_owed;
  out->reset_submit_refusal = diagnostics.reset_submit_refusal;
  out->reset_replacement = diagnostics.reset_replacement;
  out->target_losses = diagnostics.target_losses;
  out->observer_awaiting_bind = diagnostics.observer.awaiting_bind ? 1 : 0;
  out->observer_published = diagnostics.observer.has_published_target ? 1 : 0;
  out->observer_last_drop = static_cast<unsigned long long>(diagnostics.observer.last_target_lost_reason);
  out->rebind_pending = diagnostics.binding.rebind_pending ? 1 : 0;
  out->last_bind_status = static_cast<unsigned long long>(diagnostics.binding.last_bind_status);
  out->binds_refused = diagnostics.binding.binds_refused;
  out->binds_paused_retiring = diagnostics.binding.binds_paused_retiring;
  out->observer_selection_refusals = diagnostics.observer.selection_refusals;
  out->observer_selection_reads = diagnostics.observer.selection_reads_completed;
  out->observer_selection_pending = diagnostics.observer.selection_read_pending ? 1 : 0;
  out->observer_has_context =
      diagnostics.observer.has_top_context ? 1 : 0;
  out->observer_subscribed = diagnostics.observer.subscribed ? 1 : 0;
  out->retiring_bindings = diagnostics.coordinator.retirement.retiring_bindings;
  out->admission_paused = diagnostics.coordinator.retirement.admission_paused ? 1 : 0;
  out->retiring_compositions = diagnostics.coordinator.retirement.retiring_compositions;
  out->retired_settled = diagnostics.coordinator.retirement.settled;
  out->retired_already_ordinary =
      diagnostics.coordinator.retirement.already_ordinary;
  out->retired_blocked = diagnostics.coordinator.retirement.blocked;
  out->close_requests = diagnostics.coordinator.retirement.close_requests;
  out->close_results = diagnostics.coordinator.retirement.close_results_taken;
  out->close_failures = diagnostics.coordinator.retirement.close_failures;
  out->unresolved_close =
      diagnostics.coordinator.retirement.unresolved_close_requests;
  out->unresolved_engine =
      diagnostics.coordinator.retirement.unresolved_engine_results;
  out->pending_edit_event =
      diagnostics.coordinator.retirement.pending_edit_event != nullptr ? 1 : 0;
  return 0;
}

extern "C" int EpoCanvaTestCandidateText(unsigned index, wchar_t* buffer,
                                         unsigned capacity) {
  using namespace epocanva::tsf::product;
  if (buffer == nullptr || capacity == 0) {
    return -1;
  }
  std::lock_guard<std::mutex> guard(g_controller_mutex);
  ProductTextController* controller = CurrentControllerLocked();
  if (controller == nullptr) {
    return -1;
  }
  const auto snapshot = controller->CandidateSnapshotForTest();
  if (!snapshot.valid || index >= snapshot.items.size()) {
    return -1;
  }
  const std::u16string& text = snapshot.items[index].text_utf16_code_units;
  const unsigned count = static_cast<unsigned>(text.size());
  const unsigned copied = count < capacity - 1 ? count : capacity - 1;
  for (unsigned i = 0; i < copied; ++i) {
    buffer[i] = static_cast<wchar_t>(text[i]);
  }
  buffer[copied] = L'\0';
  return static_cast<int>(copied);
}

extern "C" int EpoCanvaTestCandidateKind(unsigned index) {
  using namespace epocanva::tsf::product;
  std::lock_guard<std::mutex> guard(g_controller_mutex);
  ProductTextController* controller = CurrentControllerLocked();
  if (controller == nullptr) {
    return -1;
  }
  const auto snapshot = controller->CandidateSnapshotForTest();
  if (!snapshot.valid || index >= snapshot.items.size()) {
    return -1;
  }
  return static_cast<int>(snapshot.items[index].kind);
}

extern "C" int EpoCanvaTestSelectCandidate(unsigned long long snapshot_version,
                                           int page_index) {
  using namespace epocanva::tsf::product;
  std::lock_guard<std::mutex> guard(g_controller_mutex);
  ProductTextController* controller = CurrentControllerLocked();
  if (controller == nullptr) {
    return 2;
  }
  const auto snapshot = controller->CandidateSnapshotForTest();
  CandidateUiSelection selection;
  selection.snapshot_version = snapshot_version;
  selection.context_id = snapshot.identity.context_id;
  selection.generation = snapshot.identity.generation;
  selection.page_index = page_index;
  controller->OnUiFinalize(selection);
  return 0;
}

extern "C" int EpoCanvaTestBeginBlockingStart(void* entered_event,
                                              void* release_event) {
  using namespace epocanva::tsf::product;
  g_entered_event = entered_event;
  g_release_event = release_event;
  ProductEngineBootstrap::Instance().SetStartFunctionForTest([]() {
    if (g_entered_event != nullptr) {
      ::SetEvent(static_cast<HANDLE>(g_entered_event));
    }
    if (g_release_event != nullptr) {
      ::WaitForSingleObject(static_cast<HANDLE>(g_release_event), INFINITE);
    }
    ProductEngineBootstrap::StartOutcome outcome;
    outcome.ok = false;
    outcome.why = L"controlled blocking start released";
    return outcome;
  });
  return 0;
}

extern "C" int EpoCanvaTestResetBootstrap() {
  using namespace epocanva::product;
  if (ProductEngineBootstrap::Instance().state() ==
      EngineBootstrapState::kStarting) {
    return 1;
  }
  ProductEngineBootstrap::Instance().ResetForTest();
  return 0;
}

extern "C" int EpoCanvaTestBootstrapState() {
  return static_cast<int>(
      epocanva::tsf::product::ProductEngineBootstrap::Instance().state());
}

extern "C" int EpoCanvaTestEngineHostState() {
  epocanva::tsf::EngineHostDiagnostics diagnostics;
  epocanva::tsf::EngineHostBroker::Instance().Snapshot(&diagnostics);
  return static_cast<int>(diagnostics.state);
}

extern "C" int EpoCanvaTestModuleState(long long* live_objects, long long* server_locks,
                                       long long* host_active, long long* host_failed,
                                       long long* guarded_objects) {
  if (live_objects != nullptr) {
    *live_objects = epocanva::tsf::ModuleLiveObjectCount();
  }
  if (server_locks != nullptr) {
    *server_locks = epocanva::tsf::ModuleServerLockCount();
  }
  if (host_active != nullptr) {
    *host_active = epocanva::tsf::ModuleHostActiveFlag();
  }
  if (host_failed != nullptr) {
    *host_failed = epocanva::tsf::ModuleHostFailedFlag();
  }
  if (guarded_objects != nullptr) {
    *guarded_objects = epocanva::tsf::edit::LiveGuardedObjects();
  }
  return 0;
}

extern "C" int EpoCanvaTestGuardedObjects(long long* out, int capacity) {
  if (out == nullptr || capacity <= 0) {
    return 0;
  }
  using epocanva::tsf::edit::LiveGuardedObjects;
  using epocanva::tsf::edit::ModuleObjectKind;
  const int count =
      std::min<int>(capacity, static_cast<int>(ModuleObjectKind::kKindCount));
  const ModuleObjectKind kinds[] = {
      ModuleObjectKind::kEditSession,      ModuleObjectKind::kSelectionRead,
      ModuleObjectKind::kCompositionSink,  ModuleObjectKind::kRectRead,
      ModuleObjectKind::kCandidateElement, ModuleObjectKind::kCandidateSink,
  };
  for (int index = 0; index < count; ++index) {
    out[index] = LiveGuardedObjects(kinds[index]);
  }
  return count;
}

#endif  // EPOCANVA_TSF_DIRECT_SINK_TEST
