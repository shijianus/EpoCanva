// EpoCanva W2-B -- controlled direct-sink test seam implementation.
#include "product_test_seam.h"

#ifdef EPOCANVA_TSF_DIRECT_SINK_TEST

#include <algorithm>
#include <mutex>

#include "candidate_snapshot.h"
#include "module_lifetime.h"
#include "module_state.h"
#include "product_engine_bootstrap.h"
#include "text_service_controller.h"

namespace {

std::mutex g_controller_mutex;
epocanva::tsf::product::ProductTextController* g_controller = nullptr;
void* g_entered_event = nullptr;
void* g_release_event = nullptr;

}  // namespace

namespace epocanva {
namespace tsf {
namespace product {

void SetTestController(ProductTextController* controller) {
  std::lock_guard<std::mutex> guard(g_controller_mutex);
  g_controller = controller;
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
  if (g_controller == nullptr) {
    return 2;
  }
  const ControllerDiagnostics diagnostics = g_controller->Snapshot();
  const auto candidates = g_controller->CandidateSnapshotForTest();
  out->key_downs = diagnostics.key_downs;
  out->eaten = diagnostics.eaten;
  out->literal_key_downs = diagnostics.literal_key_downs;
  out->raw_dropped_keys = diagnostics.keys.raw_dropped_keys;
  out->raw_unrecoverable = diagnostics.raw_unrecoverable;
  out->raw_applied_units = diagnostics.keys.raw_applied_units;
  out->raw_cancelled_units = diagnostics.keys.raw_cancelled_units;
  out->raw_commits = diagnostics.raw_commits;
  out->raw_ignored_edits = diagnostics.raw_ignored_edits;
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
  out->queue_pending = diagnostics.keys.queued;
  out->queue_in_flight = diagnostics.keys.has_in_flight ? 1 : 0;
  out->coordinator_phase = static_cast<unsigned long long>(diagnostics.coordinator.phase);
  out->engine_requests = diagnostics.engine_client_requests;
  out->engine_results = diagnostics.engine_client_results;
  out->raw_active = diagnostics.raw_mode ? 1 : 0;
  out->raw_submitted = diagnostics.raw_submitted ? 1 : 0;
  out->works_accepted = diagnostics.coordinator.works_accepted;
  out->target_binds = diagnostics.target_binds;
  out->rebind_requests = diagnostics.rebind_requests;
  out->raw_entered = diagnostics.raw_entered;
  out->last_submit_status = static_cast<unsigned long long>(diagnostics.coordinator.last_submit_status);
  out->gate = static_cast<unsigned long long>(diagnostics.coordinator.gate);
  out->permission = static_cast<unsigned long long>(diagnostics.coordinator.permission);
  out->composition_length = static_cast<unsigned long long>(diagnostics.coordinator.composition_length_utf16);
  out->composition_active = diagnostics.coordinator.composition_active ? 1 : 0;
  out->raw_reason_unapplied = diagnostics.raw_reason_unapplied;
  out->raw_reason_not_candidate = diagnostics.raw_reason_not_candidate;
  out->raw_reason_high_water = diagnostics.raw_reason_high_water;
  out->raw_reason_admission = diagnostics.raw_reason_admission;
  out->last_raw_composition_start = static_cast<unsigned long long>(diagnostics.coordinator.last_raw_composition_start);
  out->last_raw_composition_units = diagnostics.coordinator.last_raw_composition_units;
  out->last_raw_input_units = diagnostics.coordinator.last_raw_input_units;
  out->last_raw_plan_units = diagnostics.coordinator.last_raw_plan_units;
  out->raw_unapplied_units = diagnostics.raw_unapplied_units;
  out->raw_exits = diagnostics.raw_exits;
  out->raw_failed_keys = diagnostics.keys.raw_failed_keys;
  out->raw_failed_units = diagnostics.keys.raw_failed_units;
  out->raw_commits = diagnostics.raw_commits;
  return 0;
}

extern "C" int EpoCanvaTestCandidateText(unsigned index, wchar_t* buffer,
                                         unsigned capacity) {
  using namespace epocanva::tsf::product;
  if (buffer == nullptr || capacity == 0) {
    return -1;
  }
  std::lock_guard<std::mutex> guard(g_controller_mutex);
  if (g_controller == nullptr) {
    return -1;
  }
  const auto snapshot = g_controller->CandidateSnapshotForTest();
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
  if (g_controller == nullptr) {
    return -1;
  }
  const auto snapshot = g_controller->CandidateSnapshotForTest();
  if (!snapshot.valid || index >= snapshot.items.size()) {
    return -1;
  }
  return static_cast<int>(snapshot.items[index].kind);
}

extern "C" int EpoCanvaTestSelectCandidate(unsigned long long snapshot_version,
                                           int page_index) {
  using namespace epocanva::tsf::product;
  std::lock_guard<std::mutex> guard(g_controller_mutex);
  if (g_controller == nullptr) {
    return 2;
  }
  const auto snapshot = g_controller->CandidateSnapshotForTest();
  CandidateUiSelection selection;
  selection.snapshot_version = snapshot_version;
  selection.context_id = snapshot.identity.context_id;
  selection.generation = snapshot.identity.generation;
  selection.page_index = page_index;
  g_controller->OnUiFinalize(selection);
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
