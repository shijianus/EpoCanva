// EpoCanva W2 -- product STA controller implementation. See the header for the contract.
#include "text_service_controller.h"

#include <atomic>
#include <utility>

#include "key_text_translation.h"
#include "keyboard_state.h"

namespace epocanva {
namespace tsf {
namespace product {
namespace {

namespace engine = ::epocanva::windows::engine;
namespace input = ::epocanva::windows::input;
namespace ctx = ::epocanva::tsf::context;
namespace edit = ::epocanva::tsf::edit;

constexpr char kProductSchema[] = "epocanva_english";
constexpr std::uint32_t kVirtualKeyProcessKey = 0xE5;

std::atomic<std::uint64_t> g_next_generation{1};

input::KeyEventSnapshot EventFromTsf(WPARAM wparam, LPARAM lparam) {
  input::KeyEventSnapshot event;
  event.virtual_key = static_cast<std::uint32_t>(wparam);
  event.scan_code =
      (static_cast<std::uint32_t>(lparam) >> 16) & 0xFFu;
  event.is_extended =
      (static_cast<std::uint32_t>(lparam) & (1u << 24)) != 0;
  event.transition = input::KeyTransition::kKeyDown;
  event.repeat_count = static_cast<std::uint32_t>(lparam) & 0xFFFFu;
  if (event.repeat_count == 0) {
    event.repeat_count = 1;
  }
  event.previously_down =
      (static_cast<std::uint32_t>(lparam) & (1u << 30)) != 0;
  return event;
}

KeyFailure FailureForReject(coordinator::RejectCause cause) {
  switch (cause) {
    case coordinator::RejectCause::kPermissionDenied:
    case coordinator::RejectCause::kPermissionUnknown:
    case coordinator::RejectCause::kPermissionChangedWhileQueued:
      return KeyFailure::kPermissionRefused;
    case coordinator::RejectCause::kEngineRefused:
    case coordinator::RejectCause::kEngineUnavailable:
    case coordinator::RejectCause::kEngineSubmitFailed:
    case coordinator::RejectCause::kEditRefused:
    case coordinator::RejectCause::kEditFailedBeforeMutation:
    case coordinator::RejectCause::kTakeFailed:
      return KeyFailure::kEngineUnavailable;
    case coordinator::RejectCause::kTargetChangedWhileQueued:
      return KeyFailure::kTargetChanged;
    case coordinator::RejectCause::kNone:
      break;
  }
  return KeyFailure::kInternal;
}

}  // namespace

class ProductTextController::ClientProvider final
    : public coordinator::ObservedEngineClients {
 public:
  explicit ClientProvider(engine::EngineExecutor& executor) : executor_(executor) {}

  engine::ClientId NextClient() override {
    engine::RequestId request;
    engine::ClientId client;
    const engine::SubmitStatus submitted = executor_.TryCreateClient(
        kProductSchema,
        engine::ControllerOptions{true, input::InputPermission::kAllowed}, &request,
        &client);
    ++requests_;
    if (submitted != engine::SubmitStatus::kAccepted) {
      ++refusals_;
      return engine::ClientId{};
    }
    pending_.push_back(request);
    last_client_ = client;
    return client;
  }

  struct Harvest {
    bool failed = false;
    engine::ClientId failed_client;
  };

  Harvest TakeReady() {
    Harvest harvest;
    std::vector<engine::RequestId> remaining;
    for (const engine::RequestId& request : pending_) {
      engine::CommandResult result;
      const engine::TakeStatus taken = executor_.TakeResult(request, &result);
      if (taken == engine::TakeStatus::kTaken) {
        ++results_;
        if (!result.ok()) {
          ++failures_;
          harvest.failed = true;
          harvest.failed_client = result.client;
        }
      } else if (taken == engine::TakeStatus::kNotReady) {
        remaining.push_back(request);
      }
    }
    pending_.swap(remaining);
    return harvest;
  }

  std::uint64_t requests() const { return requests_; }
  std::uint64_t results() const { return results_; }
  std::uint64_t failures() const { return failures_; }
  std::size_t pending() const { return pending_.size(); }
  engine::ClientId last_client() const { return last_client_; }

 private:
  engine::EngineExecutor& executor_;
  std::vector<engine::RequestId> pending_;
  engine::ClientId last_client_{};
  std::uint64_t requests_ = 0;
  std::uint64_t results_ = 0;
  std::uint64_t failures_ = 0;
  std::uint64_t refusals_ = 0;
};

ProductTextController::ProductTextController()
    : generation_(g_next_generation.fetch_add(1)) {}

ProductTextController::~ProductTextController() {
  // The normal path is Stop(). Reaching the destructor with started_ true means the host
  // violated Deactivate-before-Release; do not join threads or call COM here.
}

bool ProductTextController::Start(const Options& options, std::wstring* why) {
  if (started_ || stopping_) {
    if (why != nullptr) {
      *why = L"the product controller is already started";
    }
    return false;
  }
  if (options.thread_manager == nullptr || options.client_id == 0 ||
      options.executor == nullptr) {
    if (why != nullptr) {
      *why = L"the product controller needs a thread manager, client id and executor";
    }
    return false;
  }
  thread_manager_ = options.thread_manager;
  client_id_ = options.client_id;
  executor_ = options.executor;

  registry_.reset(new ctx::ContextRegistry(8));
  permission_ = ctx::CreateRealPermissionSource(client_id_);
  coordinator_.reset(
      new coordinator::ContextCoordinator(*executor_, client_id_, *permission_));
  ctx::ThreadFocusPolicy focus_policy = ctx::ThreadFocusPolicy::kStrictTsf;
#ifdef EPOCANVA_TSF_DIRECT_SINK_TEST
  // Controlled direct-sink tests run in a headless STA with no OS foreground focus. The
  // product DLL (which never defines this macro) keeps the StrictTsf policy.
  focus_policy = ctx::ThreadFocusPolicy::kLabWin32Fallback;
#endif
  observer_.reset(
      new ctx::ContextObserver(*registry_, client_id_, focus_policy));
  clients_.reset(new ClientProvider(*executor_));
  binding_.reset(new coordinator::ObservedBindingController(
      *observer_, *coordinator_, *registry_, *clients_));

  std::string subscribe_error;
  if (!observer_->Subscribe(thread_manager_, this, &subscribe_error)) {
    if (why != nullptr) {
      *why = std::wstring(subscribe_error.begin(), subscribe_error.end());
    }
    ResetObjects();
    return false;
  }
  subscribed_ = true;
  if (!waker_.Start(this, executor_->ResultReadyEvent(), generation_, why)) {
    ResetObjects();
    return false;
  }
  started_ = true;
  PumpNow();
  return true;
}

void ProductTextController::ResetObjects() {
  binding_.reset();
  if (observer_) {
    observer_->Shutdown();
  }
  if (coordinator_) {
    coordinator_->Close();
  }
  coordinator_.reset();
  observer_.reset();
  permission_.reset();
  clients_.reset();
  registry_.reset();
  subscribed_ = false;
}

void ProductTextController::Stop() {
  if (!started_ && !binding_) {
    return;
  }
  stopping_ = true;
  // Unregister the wake target before any object it can reach is destroyed.
  waker_.Stop();
  queue_.FailQueued(KeyFailure::kShutdown);
  if (queue_.HasInFlight()) {
    queue_.MarkInFlightFailed(KeyFailure::kShutdown);
  }
  ResetObjects();
  queue_.ReleaseFailedInFlight();
  started_ = false;
  stopping_ = false;
}

bool ProductTextController::BuildKey(WPARAM wparam, LPARAM lparam, bool key_down,
                                     BuiltKey* key) const {
  if (!key_down || key == nullptr) {
    return false;
  }
  if (static_cast<std::uint32_t>(wparam) == kVirtualKeyProcessKey) {
    return false;
  }
  const input::KeyboardStateCaptureResult captured =
      input::CaptureThreadKeyboardState();
  if (!captured.ok()) {
    return false;
  }
  key->event = EventFromTsf(wparam, lparam);
  key->event.modifiers = captured.snapshot.modifiers();
  const input::KeyTranslationResult translation =
      input::TranslateKeyEvent(key->event, captured.snapshot);
  key->text = translation.text;
  key->decision = coordinator_->Evaluate(key->event, key->text);
  const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
  key->composition_active =
      snapshot.composition_active || snapshot.owned_composition_active;
  return true;
}

bool ProductTextController::AdmissionAvailable(bool composition_active) const {
  return queue_.CanAccept(composition_active);
}

bool ProductTextController::PredictKey(WPARAM wparam, LPARAM lparam) {
  if (!started_ || stopping_) {
    return false;
  }
  ++predictions_;
  BuiltKey key;
  if (!BuildKey(wparam, lparam, true, &key)) {
    return false;
  }
  const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
  if (!snapshot.bound || snapshot.closed ||
      snapshot.gate != coordinator::CoordinatorGate::kOpen) {
    return false;
  }
  if (queue_.HasInFlight() && queue_.in_flight_failed()) {
    return false;
  }
  if (!key.decision.is_engine_candidate()) {
    return false;
  }
  if (!AdmissionAvailable(key.composition_active)) {
    // Full without a composition is a safe pass-through. Full inside a composition has no
    // safe pass-through; the key is eaten and recorded as an explicit overflow failure.
    return key.composition_active;
  }
  ++predicted_eaten_;
  return true;
}

bool ProductTextController::HandleKeyDown(WPARAM wparam, LPARAM lparam,
                                          std::wstring* why) {
  (void)why;
  if (!started_ || stopping_) {
    return false;
  }
  ++key_downs_;
  BuiltKey key;
  if (!BuildKey(wparam, lparam, true, &key)) {
    ++not_eaten_;
    return false;
  }
  const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
  if (!snapshot.bound || snapshot.closed ||
      snapshot.gate != coordinator::CoordinatorGate::kOpen ||
      (queue_.HasInFlight() && queue_.in_flight_failed())) {
    ++not_eaten_;
    return false;
  }
  if (!key.decision.is_engine_candidate()) {
    // A shortcut, a key-up-shaped event, or unsupported text is never admitted. Prediction
    // and execution must agree even when the coordinator is busy or retiring.
    ++not_eaten_;
    return false;
  }
  if (!AdmissionAvailable(key.composition_active)) {
    if (!key.composition_active) {
      ++not_eaten_;
      return false;
    }
    // See PredictKey: bounded exhaustion inside a composition is an explicit failure.
    queue_.RecordOverflowEaten();
    ++overflow_eaten_;
    ++eaten_;
    waker_.SetPacing(true);
    return true;
  }

  const bool was_empty = queue_.empty() && !queue_.HasInFlight();
  const std::uint64_t id = queue_.Push(key.event, key.text, key.decision,
                                       snapshot.identity.context_id,
                                       snapshot.identity.generation);
  const SubmitAttempt attempt =
      was_empty ? SubmitHead(true) : SubmitAttempt::kWaited;
  if (attempt == SubmitAttempt::kNotEatenHostKeeps) {
    ++not_eaten_;
    return false;
  }
  if (attempt == SubmitAttempt::kFailedEaten) {
    ++eaten_;
    waker_.SetPacing(true);
    return true;
  }
  queue_.ConfirmEaten(id);
  ++eaten_;
  waker_.SetPacing(true);
  return true;
}

ProductTextController::SubmitAttempt ProductTextController::SubmitHead(bool immediate) {
  AdmissionKey* head = queue_.head();
  if (head == nullptr) {
    return SubmitAttempt::kNoHead;
  }
  const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
  const bool composing =
      snapshot.composition_active || snapshot.owned_composition_active;
  auto keep = [this, immediate, composing]() {
    if (immediate && !composing) {
      queue_.DropHeadNotEaten();
      return SubmitAttempt::kNotEatenHostKeeps;
    }
    return SubmitAttempt::kWaited;
  };

  if (snapshot.closed) {
    queue_.FailHead(KeyFailure::kShutdown, true);
    queue_.FailQueued(KeyFailure::kShutdown);
    return SubmitAttempt::kFailedEaten;
  }
  if (!snapshot.bound || snapshot.gate != coordinator::CoordinatorGate::kOpen) {
    if (immediate && !composing) {
      queue_.DropHeadNotEaten();
      queue_.FailQueued(KeyFailure::kBlockedGate);
      RequestRebindSoon();
      return SubmitAttempt::kNotEatenHostKeeps;
    }
    queue_.FailHead(KeyFailure::kBlockedGate, true);
    queue_.FailQueued(KeyFailure::kBlockedGate);
    coordinator_->InvalidateBinding();
    RequestRebindSoon();
    return SubmitAttempt::kFailedEaten;
  }
  if (head->context_id != snapshot.identity.context_id ||
      head->generation != snapshot.identity.generation) {
    if (immediate && !composing) {
      queue_.DropHeadNotEaten();
      queue_.FailQueued(KeyFailure::kBindingRetired);
      RequestRebindSoon();
      return SubmitAttempt::kNotEatenHostKeeps;
    }
    queue_.FailHead(KeyFailure::kBindingRetired, true);
    queue_.FailQueued(KeyFailure::kBindingRetired);
    coordinator_->InvalidateBinding();
    RequestRebindSoon();
    return SubmitAttempt::kFailedEaten;
  }
  if (snapshot.phase != coordinator::WorkPhase::kIdle ||
      snapshot.retirement.retiring_bindings != 0 ||
      snapshot.retirement.admission_paused) {
    return SubmitAttempt::kWaited;
  }

  coordinator::WorkId work;
  const coordinator::SubmitStatus submitted =
      coordinator_->SubmitKey(head->event, head->text, &work);
  switch (submitted) {
    case coordinator::SubmitStatus::kAccepted:
      if (!queue_.MoveHeadToInFlight(work)) {
        return SubmitAttempt::kWaited;
      }
      return SubmitAttempt::kSubmitted;
    case coordinator::SubmitStatus::kBusy:
    case coordinator::SubmitStatus::kRetiring:
    case coordinator::SubmitStatus::kQueueFull:
      return SubmitAttempt::kWaited;
    case coordinator::SubmitStatus::kNotEngineCandidate:
      if (immediate && !composing) {
        return keep();
      }
      queue_.FailHead(KeyFailure::kPredictionMismatch, true);
      return SubmitAttempt::kFailedEaten;
    case coordinator::SubmitStatus::kPermissionDenied:
    case coordinator::SubmitStatus::kPermissionUnknown:
      if (immediate && !composing) {
        return keep();
      }
      queue_.FailHead(KeyFailure::kPermissionRefused, true);
      if (composing) {
        coordinator_->InvalidateBinding();
        RequestRebindSoon();
      }
      return SubmitAttempt::kFailedEaten;
    case coordinator::SubmitStatus::kThreadFocusLost:
    case coordinator::SubmitStatus::kTargetChanged:
    case coordinator::SubmitStatus::kTargetUnverified:
    case coordinator::SubmitStatus::kNotBound:
      if (immediate && !composing) {
        queue_.DropHeadNotEaten();
        queue_.FailQueued(KeyFailure::kTargetChanged);
        RequestRebindSoon();
        return SubmitAttempt::kNotEatenHostKeeps;
      }
      queue_.FailHead(KeyFailure::kTargetChanged, true);
      queue_.FailQueued(KeyFailure::kTargetChanged);
      coordinator_->InvalidateBinding();
      RequestRebindSoon();
      return SubmitAttempt::kFailedEaten;
    case coordinator::SubmitStatus::kBlockedUncertain:
    case coordinator::SubmitStatus::kBlockedNeedsResync:
    case coordinator::SubmitStatus::kBlockedUnsupportedShape:
      if (immediate && !composing) {
        queue_.DropHeadNotEaten();
        queue_.FailQueued(KeyFailure::kBlockedGate);
        RequestRebindSoon();
        return SubmitAttempt::kNotEatenHostKeeps;
      }
      queue_.FailHead(KeyFailure::kBlockedGate, true);
      queue_.FailQueued(KeyFailure::kBlockedGate);
      coordinator_->InvalidateBinding();
      RequestRebindSoon();
      return SubmitAttempt::kFailedEaten;
    case coordinator::SubmitStatus::kEngineNotRunning:
    case coordinator::SubmitStatus::kEngineClientUnavailable:
    case coordinator::SubmitStatus::kEngineSubmitFailed:
      if (immediate && !composing) {
        queue_.DropHeadNotEaten();
        queue_.FailQueued(KeyFailure::kEngineUnavailable);
        RequestRebindSoon();
        return SubmitAttempt::kNotEatenHostKeeps;
      }
      queue_.FailHead(KeyFailure::kEngineUnavailable, true);
      queue_.FailQueued(KeyFailure::kEngineUnavailable);
      coordinator_->InvalidateBinding();
      RequestRebindSoon();
      return SubmitAttempt::kFailedEaten;
    case coordinator::SubmitStatus::kClosed:
      queue_.FailHead(KeyFailure::kShutdown, true);
      queue_.FailQueued(KeyFailure::kShutdown);
      return SubmitAttempt::kFailedEaten;
    case coordinator::SubmitStatus::kInvalidRequest:
      queue_.FailHead(KeyFailure::kInternal, true);
      return SubmitAttempt::kFailedEaten;
  }
  return SubmitAttempt::kWaited;
}

void ProductTextController::FailQueuedAndMaybeInvalidate(KeyFailure failure,
                                                         bool invalidate) {
  queue_.FailQueued(failure);
  if (invalidate && coordinator_->bound()) {
    coordinator_->InvalidateBinding();
    RequestRebindSoon();
  }
}

void ProductTextController::RequestRebindSoon() { rebind_soon_ = true; }

void ProductTextController::ResolveInFlight() {
  if (!queue_.HasInFlight()) {
    return;
  }
  const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
  if (queue_.in_flight_failed()) {
    if (snapshot.phase == coordinator::WorkPhase::kIdle) {
      queue_.ReleaseFailedInFlight();
    }
    return;
  }
  AdmissionKey* key = queue_.in_flight();
  if (key == nullptr || snapshot.work_id != key->work.value) {
    return;
  }
  if (snapshot.phase == coordinator::WorkPhase::kEditPending ||
      (snapshot.phase == coordinator::WorkPhase::kIdle &&
       (snapshot.outcome == coordinator::WorkOutcome::kApplied ||
        snapshot.outcome == coordinator::WorkOutcome::kNoEdit))) {
    queue_.MarkInFlightEngineCompleted();
  }
  if (snapshot.phase != coordinator::WorkPhase::kIdle) {
    return;
  }
  switch (snapshot.outcome) {
    case coordinator::WorkOutcome::kApplied:
    case coordinator::WorkOutcome::kNoEdit:
      queue_.CompleteInFlight(KeyState::kEditCompleted, KeyFailure::kNone);
      break;
    case coordinator::WorkOutcome::kRejected:
      queue_.CompleteInFlight(KeyState::kFailed,
                              FailureForReject(snapshot.cause));
      break;
    case coordinator::WorkOutcome::kNeedsResync:
      queue_.CompleteInFlight(KeyState::kFailed,
                              snapshot.cause ==
                                      coordinator::RejectCause::kTargetChangedWhileQueued
                                  ? KeyFailure::kTargetChanged
                                  : KeyFailure::kEngineUnavailable);
      break;
    case coordinator::WorkOutcome::kUncertain:
    case coordinator::WorkOutcome::kUnsupportedShape:
      queue_.CompleteInFlight(KeyState::kFailed, KeyFailure::kBlockedGate);
      break;
    case coordinator::WorkOutcome::kAbandonedStale:
      queue_.CompleteInFlight(KeyState::kFailed, KeyFailure::kFocusChanged);
      break;
    case coordinator::WorkOutcome::kNone:
      return;
  }
  if (snapshot.gate != coordinator::CoordinatorGate::kOpen) {
    queue_.FailQueued(KeyFailure::kBlockedGate);
  }
}

void ProductTextController::PumpNow() {
  if (!started_ || stopping_) {
    return;
  }
  ++pumps_;
  if (rebind_soon_) {
    rebind_soon_ = false;
    if (observer_) {
      observer_->RequestRebind();
      ++rebind_requests_;
    }
  }
  coordinator_->Pump();
  const ClientProvider::Harvest harvest = clients_->TakeReady();
  if (harvest.failed && harvest.failed_client == clients_->last_client()) {
    FailQueuedAndMaybeInvalidate(KeyFailure::kEngineUnavailable, true);
    if (queue_.HasInFlight()) {
      queue_.MarkInFlightFailed(KeyFailure::kEngineUnavailable);
    }
  }
  binding_->Pump();
  observer_->Pump();
  if (coordinator_->bound() &&
      coordinator_->permission() == edit::InputPermission::kUnknown &&
      !queue_.HasInFlight() && queue_.empty()) {
    coordinator_->RefreshPermission();
  }
  ResolveInFlight();
  SubmitHead(false);
  UpdatePacing();
}

void ProductTextController::UpdatePacing() {
  bool pace = false;
  if (started_ && !stopping_) {
    const coordinator::CoordinatorSnapshot coordinator = coordinator_->Snapshot();
    const ctx::ContextObserverSnapshot observer = observer_->Snapshot();
    if (queue_.HasInFlight() || !queue_.empty() ||
        coordinator.phase != coordinator::WorkPhase::kIdle ||
        coordinator.retirement.retiring_bindings != 0 ||
        coordinator.retirement.admission_paused || clients_->pending() != 0 ||
        (!coordinator.bound && observer.awaiting_bind) ||
        observer.selection_read_pending) {
      pace = true;
    }
  }
  waker_.SetPacing(pace);
}

void ProductTextController::OnStaWake(std::uint64_t generation) {
  if (!started_ || stopping_ || generation != generation_) {
    ++stale_wakes_;
    return;
  }
  ++wakes_;
  PumpNow();
}

void ProductTextController::OnTargetLost(ctx::ContextDropReason reason) {
  ++target_losses_;
  queue_.FailQueued(KeyFailure::kFocusChanged);
  if (queue_.HasInFlight() && !queue_.in_flight_failed()) {
    queue_.MarkInFlightFailed(KeyFailure::kFocusChanged);
  }
  binding_->OnTargetLost(reason);
  waker_.SetPacing(true);
}

void ProductTextController::OnTargetBound(ITfContext* context,
                                          const ctx::ObservedTarget& target) {
  ++target_binds_;
  binding_->OnTargetBound(context, target);
  waker_.SetPacing(true);
}

void ProductTextController::OnOwnEditCompleted(
    ITfContext* context, const ctx::ObservedTarget& target,
    const ctx::SelectionSnapshot& selection) {
  binding_->OnOwnEditCompleted(context, target, selection);
}

ControllerDiagnostics ProductTextController::Snapshot() const {
  ControllerDiagnostics diagnostics;
  diagnostics.started = started_;
  diagnostics.stopping = stopping_;
  diagnostics.subscribed = subscribed_;
  diagnostics.observer_closed = observer_ ? observer_->closed() : true;
  diagnostics.generation = generation_;
  diagnostics.pumps = pumps_;
  diagnostics.wakes = wakes_;
  diagnostics.stale_wakes = stale_wakes_;
  diagnostics.predictions = predictions_;
  diagnostics.predicted_eaten = predicted_eaten_;
  diagnostics.key_downs = key_downs_;
  diagnostics.eaten = eaten_;
  diagnostics.not_eaten = not_eaten_;
  diagnostics.overflow_eaten = overflow_eaten_;
  diagnostics.target_losses = target_losses_;
  diagnostics.target_binds = target_binds_;
  diagnostics.rebind_requests = rebind_requests_;
  if (clients_) {
    diagnostics.engine_client_requests = clients_->requests();
    diagnostics.engine_client_results = clients_->results();
    diagnostics.engine_client_failures = clients_->failures();
  }
  if (coordinator_) {
    diagnostics.coordinator_bound = coordinator_->bound();
    diagnostics.coordinator = coordinator_->Snapshot();
  }
  if (observer_) {
    diagnostics.observer = observer_->Snapshot();
  }
  diagnostics.keys = queue_.Snapshot();
  diagnostics.pacing = waker_.Snapshot().paced;
  return diagnostics;
}

}  // namespace product
}  // namespace tsf
}  // namespace epocanva
