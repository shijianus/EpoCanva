// EpoCanva P3.8/P3.9 -- implementation of the context-binding coordinator. The
// contract, the gate order, the work-item state machine and the lockdown rules are
// documented in coordinator.h; this file follows them line by line and adds no policy
// of its own.
#include "coordinator.h"

#include <utility>

#include "text_encoding.h"

namespace epocanva {
namespace tsf {
namespace coordinator {

namespace {

using edit::EditPlan;
using edit::EditPlanBuildResult;
using edit::EditResult;

// wchar_t and char16_t are both 16-bit on this platform (text_encoding.h static_asserts
// the wchar_t half); the value-copy between them is exact, not a re-encoding.
std::wstring Widen(const std::u16string& text) {
  static_assert(sizeof(wchar_t) == sizeof(char16_t),
                "the u16string -> wstring bridge requires 16-bit wchar_t");
  return std::wstring(text.begin(), text.end());
}

std::u16string Narrow(const std::wstring& text) {
  static_assert(sizeof(wchar_t) == sizeof(char16_t),
                "the wstring -> u16string bridge requires 16-bit wchar_t");
  return std::u16string(text.begin(), text.end());
}

edit::InputPermission MapPermission(context::PermissionState state) {
  switch (state) {
    case context::PermissionState::kAllowed:
      return edit::InputPermission::kAllowed;
    case context::PermissionState::kDenied:
      return edit::InputPermission::kDenied;
    case context::PermissionState::kUnknown:
      break;
  }
  return edit::InputPermission::kUnknown;
}

}  // namespace

const char* DescribeBindStatus(BindStatus status) {
  switch (status) {
    case BindStatus::kOk:
      return "Ok";
    case BindStatus::kAlreadyBound:
      return "AlreadyBound";
    case BindStatus::kBusy:
      return "Busy";
    case BindStatus::kRetiring:
      return "Retiring";
    case BindStatus::kClosed:
      return "Closed";
    case BindStatus::kBlockedUncertain:
      return "BlockedUncertain";
    case BindStatus::kInvalidRequest:
      return "InvalidRequest";
  }
  return "UnknownBindStatus";
}

const char* DescribeSubmitStatus(SubmitStatus status) {
  switch (status) {
    case SubmitStatus::kAccepted:
      return "Accepted";
    case SubmitStatus::kBusy:
      return "Busy";
    case SubmitStatus::kNotBound:
      return "NotBound";
    case SubmitStatus::kRetiring:
      return "Retiring";
    case SubmitStatus::kClosed:
      return "Closed";
    case SubmitStatus::kPermissionDenied:
      return "PermissionDenied";
    case SubmitStatus::kPermissionUnknown:
      return "PermissionUnknown";
    case SubmitStatus::kBlockedUncertain:
      return "BlockedUncertain";
    case SubmitStatus::kBlockedNeedsResync:
      return "BlockedNeedsResync";
    case SubmitStatus::kBlockedUnsupportedShape:
      return "BlockedUnsupportedShape";
    case SubmitStatus::kNotEngineCandidate:
      return "NotEngineCandidate";
    case SubmitStatus::kEngineNotRunning:
      return "EngineNotRunning";
    case SubmitStatus::kQueueFull:
      return "QueueFull";
    case SubmitStatus::kEngineClientUnavailable:
      return "EngineClientUnavailable";
    case SubmitStatus::kEngineSubmitFailed:
      return "EngineSubmitFailed";
    case SubmitStatus::kInvalidRequest:
      return "InvalidRequest";
  }
  return "UnknownSubmitStatus";
}

const char* DescribeWorkKind(WorkKind kind) {
  switch (kind) {
    case WorkKind::kNone:
      return "None";
    case WorkKind::kKey:
      return "Key";
    case WorkKind::kCandidate:
      return "Candidate";
  }
  return "UnknownWorkKind";
}

const char* DescribeWorkPhase(WorkPhase phase) {
  switch (phase) {
    case WorkPhase::kIdle:
      return "Idle";
    case WorkPhase::kPermissionPending:
      return "PermissionPending";
    case WorkPhase::kEnginePending:
      return "EnginePending";
    case WorkPhase::kEditPending:
      return "EditPending";
  }
  return "UnknownWorkPhase";
}

const char* DescribeWorkOutcome(WorkOutcome outcome) {
  switch (outcome) {
    case WorkOutcome::kNone:
      return "None";
    case WorkOutcome::kApplied:
      return "Applied";
    case WorkOutcome::kNoEdit:
      return "NoEdit";
    case WorkOutcome::kRejected:
      return "Rejected";
    case WorkOutcome::kNeedsResync:
      return "NeedsResync";
    case WorkOutcome::kUncertain:
      return "Uncertain";
    case WorkOutcome::kUnsupportedShape:
      return "UnsupportedShape";
    case WorkOutcome::kAbandonedStale:
      return "AbandonedStale";
  }
  return "UnknownWorkOutcome";
}

const char* DescribeCoordinatorGate(CoordinatorGate gate) {
  switch (gate) {
    case CoordinatorGate::kOpen:
      return "Open";
    case CoordinatorGate::kBlockedUncertain:
      return "BlockedUncertain";
    case CoordinatorGate::kBlockedNeedsResync:
      return "BlockedNeedsResync";
    case CoordinatorGate::kBlockedUnsupportedShape:
      return "BlockedUnsupportedShape";
  }
  return "UnknownGate";
}

const char* DescribeRejectCause(RejectCause cause) {
  switch (cause) {
    case RejectCause::kNone:
      return "None";
    case RejectCause::kPermissionDenied:
      return "PermissionDenied";
    case RejectCause::kPermissionUnknown:
      return "PermissionUnknown";
    case RejectCause::kPermissionChangedWhileQueued:
      return "PermissionChangedWhileQueued";
    case RejectCause::kEngineRefused:
      return "EngineRefused";
    case RejectCause::kEngineUnavailable:
      return "EngineUnavailable";
    case RejectCause::kEngineSubmitFailed:
      return "EngineSubmitFailed";
    case RejectCause::kEditRefused:
      return "EditRefused";
    case RejectCause::kEditFailedBeforeMutation:
      return "EditFailedBeforeMutation";
    case RejectCause::kTakeFailed:
      return "TakeFailed";
  }
  return "UnknownRejectCause";
}

const char* DescribeCleanupState(CleanupState state) {
  switch (state) {
    case CleanupState::kNone:
      return "None";
    case CleanupState::kPending:
      return "Pending";
    case CleanupState::kFinished:
      return "Finished";
    case CleanupState::kAlreadyOrdinary:
      return "AlreadyOrdinary";
    case CleanupState::kBlocked:
      return "Blocked";
  }
  return "UnknownCleanupState";
}

const char* DescribeCleanupBlockReason(CleanupBlockReason reason) {
  switch (reason) {
    case CleanupBlockReason::kNone:
      return "None";
    case CleanupBlockReason::kPermissionDenied:
      return "PermissionDenied";
    case CleanupBlockReason::kPermissionUnknown:
      return "PermissionUnknown";
    case CleanupBlockReason::kReadOnly:
      return "ReadOnly";
    case CleanupBlockReason::kContextDisconnected:
      return "ContextDisconnected";
    case CleanupBlockReason::kUncertainTextState:
      return "UncertainTextState";
    case CleanupBlockReason::kRangeMismatch:
      return "RangeMismatch";
    case CleanupBlockReason::kLockRefused:
      return "LockRefused";
    case CleanupBlockReason::kEndCompositionFailed:
      return "EndCompositionFailed";
    case CleanupBlockReason::kStale:
      return "Stale";
    case CleanupBlockReason::kPlanBuildFailed:
      return "PlanBuildFailed";
    case CleanupBlockReason::kRequestFailed:
      return "RequestFailed";
  }
  return "UnknownCleanupBlockReason";
}

ContextCoordinator::ContextCoordinator(engine::EngineExecutor& executor,
                                       TfClientId tsf_client_id,
                                       context::PermissionSource& permission_source)
    : executor_(executor),
      tsf_client_id_(tsf_client_id),
      permission_source_(permission_source) {}

ContextCoordinator::~ContextCoordinator() { Close(); }

// --- binding -----------------------------------------------------------------------------

BindStatus ContextCoordinator::Bind(ITfContext* context, std::uint64_t context_id,
                                    LONG initial_caret_utf16,
                                    engine::ClientId engine_client) {
  return BindImpl(context, context_id, /*generation=*/0, initial_caret_utf16,
                  engine_client, /*use_internal_generation=*/true);
}

BindStatus ContextCoordinator::BindObserved(ITfContext* context,
                                            std::uint64_t context_id,
                                            std::uint64_t generation,
                                            LONG initial_caret_utf16,
                                            engine::ClientId engine_client) {
  if (generation == 0) {
    return BindStatus::kInvalidRequest;
  }
  return BindImpl(context, context_id, generation, initial_caret_utf16, engine_client,
                  /*use_internal_generation=*/false);
}

bool ContextCoordinator::AttachContextRegistry(context::ContextRegistry* registry) {
  if (registry == nullptr || closed_ || bound_) {
    return false;
  }
  registry_ = registry;
  return true;
}

BindStatus ContextCoordinator::CheckBindingAdmission(
    ITfContext* context, std::uint64_t context_id, std::uint64_t generation,
    LONG initial_caret_utf16) const {
  if (context == nullptr || context_id == 0 || generation == 0 ||
      initial_caret_utf16 < 0) {
    return BindStatus::kInvalidRequest;
  }
  if (closed_) {
    return BindStatus::kClosed;
  }
  if (bound_) {
    return BindStatus::kAlreadyBound;
  }
  // The ledger is the only bounded resource binding creates. A full ledger means "do not
  // create another client yet", not "drop an old cleanup on the floor".
  if (retiring_.size() >= kRetirementCapacity) {
    return BindStatus::kRetiring;
  }
  if (phase_ != WorkPhase::kIdle) {
    return BindStatus::kBusy;
  }
  if (RegistryBlocks(context)) {
    return BindStatus::kBlockedUncertain;
  }
  if (uncertain_blocked_context_id_ != 0 &&
      uncertain_blocked_context_id_ == context_id) {
    return BindStatus::kBlockedUncertain;
  }
  return BindStatus::kOk;
}

bool ContextCoordinator::RegistryBlocks(ITfContext* context) const {
  if (registry_ == nullptr || context == nullptr) {
    return false;
  }
  // The real context pointer is the key; an integer id supplied by a caller has no
  // influence on this check.
  return registry_->IsUncertain(context);
}

void ContextCoordinator::ObserveRealCaret(LONG caret_utf16) {
  if (!bound_ || closed_ || caret_utf16 < 0) {
    return;
  }
  ++observed_caret_updates_;
  if (phase_ == WorkPhase::kIdle && !submitting_edit_) {
    view_.caret_utf16 = caret_utf16;
    ++observed_caret_applied_;
    observed_caret_pending_ = false;
    return;
  }
  observed_caret_pending_ = true;
  observed_caret_utf16_ = caret_utf16;
}

BindStatus ContextCoordinator::BindImpl(ITfContext* context,
                                        std::uint64_t context_id,
                                        std::uint64_t generation,
                                        LONG initial_caret_utf16,
                                        engine::ClientId engine_client,
                                        bool use_internal_generation) {
  // Argument validation precedes the state checks: a structurally invalid request is
  // invalid regardless of the coordinator's current state.
  if (context == nullptr || context_id == 0 || !engine_client.valid() ||
      initial_caret_utf16 < 0) {
    return BindStatus::kInvalidRequest;
  }
  if (closed_) {
    return BindStatus::kClosed;
  }
  if (bound_) {
    return BindStatus::kAlreadyBound;
  }
  if (retiring_.size() >= kRetirementCapacity) {
    return BindStatus::kRetiring;
  }
  if (phase_ != WorkPhase::kIdle) {
    // A previous work item's engine result is still outstanding; single flight means the
    // new binding cannot accept work until it has been discarded.
    return BindStatus::kBusy;
  }
  // The latch is keyed by the REAL context identity when a registry is attached, so the
  // same context cannot be re-bound under a different integer id.
  if (RegistryBlocks(context)) {
    return BindStatus::kBlockedUncertain;
  }
  if (uncertain_blocked_context_id_ != 0 &&
      uncertain_blocked_context_id_ == context_id) {
    return BindStatus::kBlockedUncertain;
  }

  session_ = std::make_unique<edit::CompositionEditSession>(tsf_client_id_);
  if (use_internal_generation) {
    generation = ++generation_counter_;
  }
  if (!session_->Bind(context, context_id, generation)) {
    session_.reset();
    return BindStatus::kInvalidRequest;
  }
  // The session's last-moment gate is the same real metadata read, performed with the
  // cookie of the granted edit session. It sees a permission change that happened after
  // the coordinator's pre-edit query (for example while the lock was deferred).
  session_->SetPermissionRecheck(
      [](ITfContext* session_context, TfEditCookie edit_cookie) {
        const context::PermissionDecision decision = context::DecidePermission(
            context::ReadPermissionFactsWithCookie(session_context, edit_cookie));
        return MapPermission(decision.state);
      });
  context->AddRef();
  bound_context_ = context;
  identity_ = edit::ContextIdentity{context_id, generation};
  engine_client_ = engine_client;
  bound_ = true;
  // A new binding starts with a clean work ledger. The NeedsResync/UnsupportedShape
  // blocks of the previous binding describe its engine view and are explicitly cleared by
  // this rebind (the uncertain block is keyed by context id and survives, above).
  outcome_ = WorkOutcome::kNone;
  cause_ = RejectCause::kNone;
  work_kind_ = WorkKind::kNone;
  observed_caret_pending_ = false;
  // Every new binding starts from the safe refusal; the first work item replaces it with
  // a verdict resolved from the real context metadata.
  CachePermission(context::PermissionState::kUnknown);
  view_ = LocalDocumentView{};
  view_.caret_utf16 = initial_caret_utf16;
  return BindStatus::kOk;
}

void ContextCoordinator::InvalidateBinding() {
  if (closed_ || !bound_) {
    return;
  }
  // The work item's terminal state is decided here, not inside any callback: a callback
  // that never runs cannot keep the coordinator pending forever.
  if (phase_ == WorkPhase::kEditPending || phase_ == WorkPhase::kPermissionPending) {
    EndWork(WorkOutcome::kAbandonedStale, RejectCause::kNone);
  } else if (phase_ == WorkPhase::kEnginePending) {
    // The engine result, when it becomes ready, is consumed once and discarded by the
    // retirement path. Its request moves with the binding below.
    work_abandoned_ = true;
    outcome_ = WorkOutcome::kAbandonedStale;
    cause_ = RejectCause::kNone;
  }
  pending_permission_ = PendingPermissionWork{};
  observed_caret_pending_ = false;
  // A permission read session that is still queued publishes nothing from now on.
  permission_source_.Abandon();
  RetireCurrentBinding();
}

// --- retirement ---------------------------------------------------------------------------

bool ContextCoordinator::RetirementPausesInput() const {
  return !retiring_.empty();
}

void ContextCoordinator::RetireCurrentBinding() {
  RetiringBinding record;
  record.identity = identity_;
  record.client = engine_client_;
  if (phase_ == WorkPhase::kEnginePending && engine_request_.valid()) {
    record.engine_request = engine_request_;
    record.engine_result_pending = true;
  }
  record.context = bound_context_;
  record.session = std::move(session_);
  bound_context_ = nullptr;
  engine_client_ = engine::ClientId{};

  // The old binding stops accepting input immediately. In this step the session is closed
  // without a TSF write; its pending plan is invalidated so a late callback cannot write.
  // (The composition-finishing step replaces this release with a verified end.)
  if (record.session != nullptr) {
    record.session->Invalidate();
    record.session->Close();
    record.session.reset();
  }
  if (record.context != nullptr) {
    record.context->Release();
    record.context = nullptr;
  }

  phase_ = WorkPhase::kIdle;
  work_kind_ = WorkKind::kNone;
  work_abandoned_ = false;
  engine_request_ = engine::RequestId{};
  bound_ = false;
  identity_ = edit::ContextIdentity{};
  view_ = LocalDocumentView{};
  // NeedsResync / UnsupportedShape describe the engine view of one binding; an explicit
  // invalidation is the documented way out of them. The uncertain block is not cleared
  // here: it belongs to the context's document, whose text state is still unknown.

  if (!record.client.valid() && !record.engine_result_pending) {
    return;
  }
  if (retiring_.size() >= kRetirementCapacity) {
    // Unreachable while binding admission honours the same capacity. If it ever happens,
    // never grow the ledger: account the reclamation as unresolved rather than pretending
    // it finished. StopAndJoin still owns the actual engine cleanup.
    ++retirement_unresolved_engine_results_;
    return;
  }
  retiring_.push_back(std::move(record));
  ++retirements_started_;
}

void ContextCoordinator::PumpRetirements() {
  for (auto record = retiring_.begin(); record != retiring_.end();) {
    if (record->engine_result_pending) {
      engine::CommandResult discarded;
      const engine::TakeStatus taken =
          executor_.TakeResult(record->engine_request, &discarded);
      if (taken == engine::TakeStatus::kTaken) {
        record->engine_result_pending = false;
        ++engine_results_taken_;
        ++retirement_engine_results_discarded_;
      } else if (taken != engine::TakeStatus::kNotReady) {
        // Already taken or unknown: there is nothing left to consume, but this is not a
        // clean retirement and must stay visible.
        record->engine_result_pending = false;
        ++retirement_unresolved_engine_results_;
      }
    }

    if (!record->close_terminal) {
      if (!record->close_requested) {
        engine::RequestId request;
        const engine::SubmitStatus submitted =
            record->client.valid()
                ? executor_.TryCloseClient(record->client, &request)
                : engine::SubmitStatus::kClientUnknown;
        switch (submitted) {
          case engine::SubmitStatus::kAccepted:
            record->close_requested = true;
            record->close_request = request;
            ++retirement_close_requests_;
            break;
          case engine::SubmitStatus::kQueueFull:
            // Retry from the next Pump. The record is the bound; nothing is dropped.
            ++retirement_close_queue_full_retries_;
            break;
          case engine::SubmitStatus::kClientUnknown:
          case engine::SubmitStatus::kClientNotOpen:
            // The executor has already closed or failed this client; no request exists.
            record->close_terminal = true;
            break;
          case engine::SubmitStatus::kNotRunning:
          case engine::SubmitStatus::kInternalFailure:
          case engine::SubmitStatus::kInvalidRequest:
          default:
            // The executor cannot accept a close. StopAndJoin remains the last-resort
            // owner; this retirement is reported unresolved, never as reclaimed.
            record->close_terminal = true;
            record->close_failed = true;
            ++retirement_unresolved_close_requests_;
            break;
        }
      } else {
        engine::CommandResult closed;
        const engine::TakeStatus taken =
            executor_.TakeResult(record->close_request, &closed);
        if (taken == engine::TakeStatus::kTaken) {
          record->close_terminal = true;
          ++retirement_close_results_;
          if (!closed.ok()) {
            record->close_failed = true;
            ++retirement_close_failures_;
          }
        } else if (taken != engine::TakeStatus::kNotReady) {
          record->close_terminal = true;
          record->close_failed = true;
          ++retirement_unresolved_close_requests_;
        }
      }
    }

    // Step 1 has no TSF cleanup to finish, so a consumed close and engine result end the
    // record. Step 2 adds the composition cleanup condition here.
    if (!record->engine_result_pending && record->close_terminal) {
      ++retirements_completed_;
      record = retiring_.erase(record);
      continue;
    }
    ++record;
  }
}

RetirementSnapshot ContextCoordinator::BuildRetirementSnapshot() const {
  RetirementSnapshot snapshot;
  snapshot.retiring_bindings = retiring_.size();
  snapshot.capacity = kRetirementCapacity;
  snapshot.started = retirements_started_;
  snapshot.completed = retirements_completed_;
  snapshot.already_ordinary = retirements_already_ordinary_;
  snapshot.blocked = retirements_blocked_;
  snapshot.close_requests = retirement_close_requests_;
  snapshot.close_results_taken = retirement_close_results_;
  snapshot.close_failures = retirement_close_failures_;
  snapshot.close_queue_full_retries = retirement_close_queue_full_retries_;
  snapshot.engine_results_discarded = retirement_engine_results_discarded_;
  snapshot.unresolved_close_requests = retirement_unresolved_close_requests_;
  snapshot.unresolved_engine_results = retirement_unresolved_engine_results_;
  snapshot.last_cleanup_state = last_cleanup_state_;
  snapshot.last_cleanup_block = last_cleanup_block_;
  for (const RetiringBinding& record : retiring_) {
    if (snapshot.pending_engine_request.value == 0 &&
        record.engine_result_pending) {
      snapshot.pending_engine_request = record.engine_request;
    }
    if (snapshot.pending_close_request.value == 0 &&
        record.close_requested && !record.close_terminal) {
      snapshot.pending_close_request = record.close_request;
    }
    if (record.cleanup == CleanupState::kPending) {
      snapshot.admission_paused = true;
      if (record.session != nullptr && record.session->HasPending()) {
        snapshot.pending_edit_event = record.session->CompletionEvent();
      }
    }
  }
  return snapshot;
}

// --- permission --------------------------------------------------------------------------

bool ContextCoordinator::StartPermissionQuery() {
  if (bound_context_ == nullptr || !bound_) {
    return false;
  }
  if (!permission_source_.Begin(bound_context_, identity_.context_id,
                                identity_.generation)) {
    return false;
  }
  ++permission_queries_begun_;
  return true;
}

bool ContextCoordinator::PermissionVerdictAllows(bool* identity_current) {
  *identity_current = false;
  const context::PermissionQueryResult verdict = permission_source_.Result();
  if (!verdict.complete) {
    return false;
  }
  // The verdict is re-validated against the binding at every use: a result resolved for
  // another context or an older generation never grants anything here.
  if (verdict.context_id != identity_.context_id ||
      verdict.generation != identity_.generation) {
    return false;
  }
  *identity_current = true;
  ++permission_queries_completed_;
  CachePermission(verdict.decision.state);
  return verdict.decision.state == context::PermissionState::kAllowed;
}

void ContextCoordinator::CachePermission(context::PermissionState state) {
  permission_ = MapPermission(state);
  if (session_ != nullptr) {
    // Defense in depth: the P3.7 session refuses its own writes under a non-Allowed
    // permission even if a pending callback is already queued.
    session_->SetInputPermission(permission_);
  }
}

input::KeyDecision ContextCoordinator::Evaluate(const input::KeyEventSnapshot& event,
                                                const input::KeyText& text) const {
  input::InputContext evaluation_context;
  evaluation_context.input_enabled = true;
  evaluation_context.engine_ready = bound_ && engine_client_.valid();
  evaluation_context.composition_active = view_.composition_active;
  switch (permission_) {
    case edit::InputPermission::kAllowed:
      evaluation_context.permission = input::InputPermission::kAllowed;
      break;
    case edit::InputPermission::kDenied:
      evaluation_context.permission = input::InputPermission::kForbidden;
      break;
    case edit::InputPermission::kUnknown:
      evaluation_context.permission = input::InputPermission::kUnknown;
      break;
  }
  return input::DecideKey(event, text, evaluation_context);
}

// --- submit ------------------------------------------------------------------------------

bool ContextCoordinator::GateBlocksSubmit(SubmitStatus* status) const {
  // The real-context latch outranks the integer-keyed member: it is keyed by the context
  // the binding actually holds, so it cannot be bypassed by a different id.
  if (RegistryBlocks(bound_context_)) {
    *status = SubmitStatus::kBlockedUncertain;
    return true;
  }
  if (uncertain_blocked_context_id_ != 0 &&
      uncertain_blocked_context_id_ == identity_.context_id) {
    *status = SubmitStatus::kBlockedUncertain;
    return true;
  }
  switch (outcome_) {
    case WorkOutcome::kNeedsResync:
      *status = SubmitStatus::kBlockedNeedsResync;
      return true;
    case WorkOutcome::kUnsupportedShape:
      *status = SubmitStatus::kBlockedUnsupportedShape;
      return true;
    default:
      return false;
  }
}

void ContextCoordinator::FinishWorkLedger(WorkKind kind) {
  work_kind_ = kind;
  outcome_ = WorkOutcome::kNone;
  cause_ = RejectCause::kNone;
  work_id_ = next_work_id_++;
  work_generation_ = identity_.generation;
  work_abandoned_ = false;
  observed_caret_pending_ = false;
  pending_plan_.reset();
  last_edit_status_ = edit::EditStatus::kRejected;
  last_edit_reason_ = edit::EditRejectReason::kNone;
  ++works_accepted_;
}

SubmitStatus ContextCoordinator::FinishSubmit(engine::RequestId request, WorkKind kind) {
  FinishWorkLedger(kind);
  phase_ = WorkPhase::kEnginePending;
  engine_request_ = request;
  return SubmitStatus::kAccepted;
}

SubmitStatus ContextCoordinator::SubmitKey(const input::KeyEventSnapshot& event,
                                           const input::KeyText& text, WorkId* id) {
  if (id == nullptr) {
    return SubmitStatus::kInvalidRequest;
  }
  id->value = 0;
  if (closed_) {
    return SubmitStatus::kClosed;
  }
  if (phase_ != WorkPhase::kIdle) {
    return SubmitStatus::kBusy;
  }
  if (!bound_) {
    return SubmitStatus::kNotBound;
  }
  if (RetirementPausesInput()) {
    // A previous binding still owes an engine result, a close, or a TSF cleanup. This is a
    // fact about the host, not about this key: nothing was processed and nothing is dropped
    // silently; the status and the snapshot name the pause.
    return SubmitStatus::kRetiring;
  }
  SubmitStatus blocked = SubmitStatus::kAccepted;
  if (GateBlocksSubmit(&blocked)) {
    return blocked;
  }
  // The real permission gate precedes the engine-candidate evaluation: a privacy
  // refusal is the more specific fact about this field (P3.3 precedence, restated).
  if (!StartPermissionQuery()) {
    return SubmitStatus::kPermissionUnknown;
  }
  if (permission_source_.Status() == context::PermissionQueryStatus::kPending) {
    // The real query went async. The work item exists from here on; nothing reaches the
    // engine until the verdict arrives.
    FinishWorkLedger(WorkKind::kKey);
    phase_ = WorkPhase::kPermissionPending;
    engine_request_ = engine::RequestId{};
    pending_permission_ = PendingPermissionWork{};
    pending_permission_.kind = WorkKind::kKey;
    pending_permission_.event = event;
    pending_permission_.text = text;
    *id = WorkId{work_id_};
    return SubmitStatus::kAccepted;
  }
  bool identity_current = false;
  if (!PermissionVerdictAllows(&identity_current)) {
    if (!identity_current) {
      return SubmitStatus::kPermissionUnknown;  // no usable verdict for this binding
    }
    return permission_ == edit::InputPermission::kDenied
               ? SubmitStatus::kPermissionDenied
               : SubmitStatus::kPermissionUnknown;
  }
  if (!Evaluate(event, text).is_engine_candidate()) {
    return SubmitStatus::kNotEngineCandidate;
  }
  engine::RequestId request;
  const engine::SubmitStatus submitted =
      executor_.TryDispatch(engine_client_, event, text, &request);
  switch (submitted) {
    case engine::SubmitStatus::kAccepted:
      FinishSubmit(request, WorkKind::kKey);
      *id = WorkId{work_id_};
      return SubmitStatus::kAccepted;
    case engine::SubmitStatus::kNotRunning:
      return SubmitStatus::kEngineNotRunning;
    case engine::SubmitStatus::kQueueFull:
      return SubmitStatus::kQueueFull;
    case engine::SubmitStatus::kClientUnknown:
    case engine::SubmitStatus::kClientNotOpen:
      return SubmitStatus::kEngineClientUnavailable;
    case engine::SubmitStatus::kInternalFailure:
      return SubmitStatus::kEngineSubmitFailed;
    case engine::SubmitStatus::kInvalidRequest:
    default:
      return SubmitStatus::kInvalidRequest;
  }
}

SubmitStatus ContextCoordinator::SubmitCandidate(std::size_t candidate_index, WorkId* id) {
  if (id == nullptr) {
    return SubmitStatus::kInvalidRequest;
  }
  id->value = 0;
  if (closed_) {
    return SubmitStatus::kClosed;
  }
  if (phase_ != WorkPhase::kIdle) {
    return SubmitStatus::kBusy;
  }
  if (!bound_) {
    return SubmitStatus::kNotBound;
  }
  if (RetirementPausesInput()) {
    return SubmitStatus::kRetiring;
  }
  SubmitStatus blocked = SubmitStatus::kAccepted;
  if (GateBlocksSubmit(&blocked)) {
    return blocked;
  }
  if (!StartPermissionQuery()) {
    return SubmitStatus::kPermissionUnknown;
  }
  if (permission_source_.Status() == context::PermissionQueryStatus::kPending) {
    FinishWorkLedger(WorkKind::kCandidate);
    phase_ = WorkPhase::kPermissionPending;
    engine_request_ = engine::RequestId{};
    pending_permission_ = PendingPermissionWork{};
    pending_permission_.kind = WorkKind::kCandidate;
    pending_permission_.candidate_index = candidate_index;
    *id = WorkId{work_id_};
    return SubmitStatus::kAccepted;
  }
  bool identity_current = false;
  if (!PermissionVerdictAllows(&identity_current)) {
    if (!identity_current) {
      return SubmitStatus::kPermissionUnknown;
    }
    return permission_ == edit::InputPermission::kDenied
               ? SubmitStatus::kPermissionDenied
               : SubmitStatus::kPermissionUnknown;
  }
  engine::RequestId request;
  const engine::SubmitStatus submitted =
      executor_.TryAcceptCandidate(engine_client_, candidate_index, &request);
  switch (submitted) {
    case engine::SubmitStatus::kAccepted:
      FinishSubmit(request, WorkKind::kCandidate);
      *id = WorkId{work_id_};
      return SubmitStatus::kAccepted;
    case engine::SubmitStatus::kNotRunning:
      return SubmitStatus::kEngineNotRunning;
    case engine::SubmitStatus::kQueueFull:
      return SubmitStatus::kQueueFull;
    case engine::SubmitStatus::kClientUnknown:
    case engine::SubmitStatus::kClientNotOpen:
      return SubmitStatus::kEngineClientUnavailable;
    case engine::SubmitStatus::kInternalFailure:
      return SubmitStatus::kEngineSubmitFailed;
    case engine::SubmitStatus::kInvalidRequest:
    default:
      return SubmitStatus::kInvalidRequest;
  }
}

// --- the permission stage ----------------------------------------------------------------

// Continues a permission-allowed, already-accepted work item into the engine. Used only
// from AdvancePermissionStage: submit-time failures here are outcomes of an existing
// work item, not submit statuses handed to the host.
void ContextCoordinator::DispatchPendingWork(const PendingPermissionWork& work) {
  engine::RequestId request;
  engine::SubmitStatus submitted = engine::SubmitStatus::kInvalidRequest;
  if (work.kind == WorkKind::kKey) {
    submitted = executor_.TryDispatch(engine_client_, work.event, work.text, &request);
  } else if (work.kind == WorkKind::kCandidate) {
    submitted = executor_.TryAcceptCandidate(engine_client_, work.candidate_index,
                                             &request);
  }
  switch (submitted) {
    case engine::SubmitStatus::kAccepted:
      FinishSubmit(request, work.kind);
      return;
    case engine::SubmitStatus::kNotRunning:
    case engine::SubmitStatus::kClientUnknown:
    case engine::SubmitStatus::kClientNotOpen:
      EndWork(WorkOutcome::kRejected, RejectCause::kEngineUnavailable);
      return;
    case engine::SubmitStatus::kQueueFull:
    case engine::SubmitStatus::kInternalFailure:
    case engine::SubmitStatus::kInvalidRequest:
    default:
      EndWork(WorkOutcome::kRejected, RejectCause::kEngineSubmitFailed);
      return;
  }
}

void ContextCoordinator::AdvancePermissionStage() {
  const context::PermissionQueryStatus status = permission_source_.Status();
  if (status == context::PermissionQueryStatus::kPending) {
    return;  // the real read session has not completed; the host bounded-waits
  }
  if (status == context::PermissionQueryStatus::kIdle) {
    // The query was terminated without a verdict; nothing was sent to the engine.
    EndWork(WorkOutcome::kRejected, RejectCause::kPermissionUnknown);
    pending_permission_ = PendingPermissionWork{};
    return;
  }
  bool identity_current = false;
  if (!PermissionVerdictAllows(&identity_current)) {
    if (!identity_current) {
      // A verdict from another binding or an older generation never grants anything.
      EndWork(WorkOutcome::kAbandonedStale, RejectCause::kNone);
    } else {
      EndWork(WorkOutcome::kRejected,
              permission_ == edit::InputPermission::kDenied
                  ? RejectCause::kPermissionDenied
                  : RejectCause::kPermissionUnknown);
    }
    pending_permission_ = PendingPermissionWork{};
    return;
  }
  PendingPermissionWork work = std::move(pending_permission_);
  pending_permission_ = PendingPermissionWork{};
  if (work.has_plan) {
    // The apply-time re-verification finished Allowed; the already-decided edit is built.
    SubmitEditPlan(work.plan);
    return;
  }
  if (work.kind == WorkKind::kKey &&
      !Evaluate(work.event, work.text).is_engine_candidate()) {
    EndWork(WorkOutcome::kNoEdit, RejectCause::kNone);
    return;
  }
  DispatchPendingWork(work);
}

// --- pump --------------------------------------------------------------------------------

void ContextCoordinator::Pump() {
  ++pumps_;
  if (closed_) {
    return;
  }
  // Retired bindings advance first: their engine results and close results are consumed
  // here, outside every TSF notification, and their cleanup edit sessions are submitted
  // here too. This is independent of the current binding's work item.
  PumpRetirements();

  if (phase_ == WorkPhase::kPermissionPending) {
    AdvancePermissionStage();
    if (phase_ == WorkPhase::kEnginePending) {
      // A freshly dispatched engine result is never ready in the same Pump; the host
      // bounded-waits on the executor's completion like before.
      return;
    }
    if (phase_ != WorkPhase::kEditPending) {
      return;
    }
    // An edit submitted by the apply-time path may have completed synchronously; fall
    // through to the existing harvest exactly as the engine-pending branch does.
  }

  if (phase_ == WorkPhase::kEnginePending) {
    engine::CommandResult result;
    const engine::TakeStatus taken = executor_.TakeResult(engine_request_, &result);
    if (taken == engine::TakeStatus::kNotReady) {
      return;  // still queued/executing; waiting belongs to the host
    }
    ++engine_results_taken_;
    if (taken != engine::TakeStatus::kTaken) {
      EndWork(WorkOutcome::kNeedsResync, RejectCause::kTakeFailed);
      return;
    }
    ResolveEngineResult(result);
    if (phase_ != WorkPhase::kEditPending) {
      return;
    }
    // The edit was submitted synchronously-complete; fall through only when it is really
    // still pending.
  }

  if (phase_ == WorkPhase::kEditPending && session_ != nullptr) {
    if (!session_->HasPending()) {
      ResolveEditResult(session_->PendingResult());
      return;
    }
    if (::WaitForSingleObject(session_->CompletionEvent(), 0) != WAIT_OBJECT_0) {
      return;  // the callback has not completed; the host pumps messages
    }
    ResolveEditResult(session_->PendingResult());
  }
}

HANDLE ContextCoordinator::EditCompletionEvent() const {
  if (phase_ == WorkPhase::kEditPending && session_ != nullptr) {
    return session_->CompletionEvent();
  }
  return nullptr;
}

HANDLE ContextCoordinator::PermissionCompletionEvent() const {
  if (phase_ == WorkPhase::kPermissionPending) {
    return permission_source_.CompletionEvent();
  }
  return nullptr;
}

// --- engine result resolution ------------------------------------------------------------

void ContextCoordinator::ResolveEngineResult(const engine::CommandResult& result) {
  if (work_abandoned_) {
    // Consumed once and discarded: an old generation's result never reaches any context.
    EndWork(WorkOutcome::kAbandonedStale, RejectCause::kNone);
    return;
  }

  if (!result.ok()) {
    switch (result.status) {
      case engine::CommandStatus::kClientUnavailable:
        EndWork(WorkOutcome::kRejected, RejectCause::kEngineUnavailable);
        return;
      case engine::CommandStatus::kControllerFailed:
      case engine::CommandStatus::kInternalFault:
      case engine::CommandStatus::kExecutorFailed:
      case engine::CommandStatus::kInvalidRequest:
      default:
        // The dispatch may already have reached the engine; the engine's state is unknown
        // and must not be guessed from a later observation. No retry, no replay.
        EnterNeedsResync("engine-command-failed", RejectCause::kNone);
        return;
    }
  }

  EngineObservation observation;
  if (result.kind == engine::CommandKind::kDispatch) {
    const auto& payload = std::get<engine::DispatchCommandOutcome>(result.payload);
    const auto& outcome = payload.outcome;
    observation.engine_call_made = outcome.engine_key_call_made;
    observation.engine_reported_handled = outcome.engine_reported_handled;
    observation.engine_refused =
        outcome.status == engine::DispatchStatus::kNotDispatched ||
        outcome.status == engine::DispatchStatus::kDeferredCompositionBoundary ||
        outcome.status == engine::DispatchStatus::kDispatchedUnhandled;
    observation.state_status = outcome.state_status;
    observation.commit_status = outcome.commit_status;
    observation.state = outcome.state;
    observation.commit = outcome.pending_commit;
  } else if (result.kind == engine::CommandKind::kAcceptCandidate) {
    const auto& payload = std::get<engine::CandidateCommandOutcome>(result.payload);
    const auto& outcome = payload.outcome;
    observation.engine_call_made = outcome.engine_selection_call_made;
    observation.engine_reported_handled = outcome.engine_reported_accepted;
    observation.engine_refused =
        outcome.status == engine::CandidateStatus::kNotReady ||
        outcome.status == engine::CandidateStatus::kOutOfRange ||
        outcome.status == engine::CandidateStatus::kRejectedByEngine;
    observation.state_status = outcome.state_status;
    observation.commit_status = outcome.commit_status;
    observation.state = outcome.state;
    observation.commit = outcome.pending_commit;
  } else {
    EnterNeedsResync("unexpected-command-kind", RejectCause::kNone);
    return;
  }

  BeginApplyAfterEngine(observation);
}

// The observation is interpreted BEFORE any further permission read. That order is what
// keeps an unreadable result of a dispatched command from being reported as NoEdit: the
// pure mapping's validity rules see the failed state/commit read first, and only an
// edit-producing plan pays for the apply-time permission query.
void ContextCoordinator::BeginApplyAfterEngine(const EngineObservation& observation) {
  const PlanDecision decision = DecidePlanFromEngine(view_, observation);
  switch (decision.kind) {
    case PlanDecisionKind::kNoEdit:
      EndWork(WorkOutcome::kNoEdit, RejectCause::kNone);
      return;
    case PlanDecisionKind::kUnsupportedShape:
      EndWork(WorkOutcome::kUnsupportedShape, RejectCause::kNone);
      return;
    case PlanDecisionKind::kInvalidObservation:
      EnterNeedsResync("invalid-engine-observation", RejectCause::kNone);
      return;
    case PlanDecisionKind::kBegin:
    case PlanDecisionKind::kUpdate:
    case PlanDecisionKind::kCommit:
    case PlanDecisionKind::kCancel:
      break;
  }

  // An edit is now unavoidable, so the permission is re-resolved from the CURRENT real
  // metadata (not read back from dispatch time). The engine already consumed the event;
  // a non-Allowed verdict now is an engine/host divergence and lands on NeedsResync,
  // never on a quiet drop.
  if (!StartPermissionQuery()) {
    EnterNeedsResync("permission-requery-refused", RejectCause::kNone);
    return;
  }
  if (permission_source_.Status() == context::PermissionQueryStatus::kPending) {
    // The apply-time re-verification went async; the decided plan is held until the
    // verdict arrives and no edit is built in the meantime.
    phase_ = WorkPhase::kPermissionPending;
    pending_permission_ = PendingPermissionWork{};
    pending_permission_.has_plan = true;
    pending_permission_.plan = decision;
    return;
  }
  bool identity_current = false;
  if (!PermissionVerdictAllows(&identity_current)) {
    EndWork(WorkOutcome::kNeedsResync, RejectCause::kPermissionChangedWhileQueued);
    return;
  }

  SubmitEditPlan(decision);
}

void ContextCoordinator::SubmitEditPlan(const PlanDecision& decision) {
  // The plan builders take UTF-8 bytes; the engine text is UTF-16 code units. The strict
  // P3.2 converter is used in both directions, so a malformed value is refused, never
  // repaired. (EditPlan itself converts straight back through the same converter.)
  const auto widened = Widen(decision.text_utf16);
  const auto utf8 =
      epocanva::windows::text::ConvertUtf16CodeUnitsToUtf8Bytes(widened);
  if (!utf8.ok()) {
    EnterNeedsResync("engine-text-conversion-refused", RejectCause::kNone);
    return;
  }

  EditPlanBuildResult built =
      [this, &decision, &utf8]() -> EditPlanBuildResult {
        switch (decision.kind) {
          case PlanDecisionKind::kBegin:
            return EditPlan::BuildBegin(identity_, utf8.utf8_bytes, decision.caret_utf16,
                                        decision.caret_utf16);
          case PlanDecisionKind::kUpdate:
            return EditPlan::BuildUpdate(identity_, utf8.utf8_bytes);
          case PlanDecisionKind::kCommit:
            return EditPlan::BuildCommit(identity_, utf8.utf8_bytes, decision.caret_utf16,
                                         decision.caret_utf16);
          case PlanDecisionKind::kCancel:
            return EditPlan::BuildCancel(identity_);
          default:
            return EditPlanBuildResult{};
        }
      }();
  if (!built.ok()) {
    EnterNeedsResync("plan-build-refused", RejectCause::kNone);
    return;
  }

  ++plans_submitted_;
  pending_plan_ = std::move(built.plan);
  submitting_edit_ = true;
  const EditResult applied = session_->ApplyAsync(pending_plan_.value());
  submitting_edit_ = false;
  if (applied.pending()) {
    phase_ = WorkPhase::kEditPending;
    return;
  }
  ResolveEditResult(applied);
}

// --- edit result resolution --------------------------------------------------------------

void ContextCoordinator::ResolveEditResult(const EditResult& result) {
  last_edit_status_ = result.status;
  last_edit_reason_ = result.reason;

  if (result.pending()) {
    // Defensive only: callers hand over final results. Pending stays pending.
    return;
  }

  if (work_abandoned_) {
    // The binding was invalidated while this edit was in flight. If the callback ran
    // before the invalidation and the edit genuinely applied, that fact is reported
    // honestly instead of being denied; every other outcome of an invalidated plan is
    // Stale/refused and stays AbandonedStale.
    if (result.status == edit::EditStatus::kApplied && pending_plan_.has_value()) {
      ApplyViewUpdate(pending_plan_.value());
      EndWork(WorkOutcome::kApplied, RejectCause::kNone);
      return;
    }
    if (result.status == edit::EditStatus::kStale) {
      ++edits_stale_;
    }
    EndWork(WorkOutcome::kAbandonedStale, RejectCause::kNone);
    return;
  }

  switch (result.status) {
    case edit::EditStatus::kApplied:
      if (pending_plan_.has_value()) {
        ApplyViewUpdate(pending_plan_.value());
      }
      ++edits_applied_;
      EndWork(WorkOutcome::kApplied, RejectCause::kNone);
      return;
    case edit::EditStatus::kStale:
      ++edits_stale_;
      EndWork(WorkOutcome::kAbandonedStale, RejectCause::kNone);
      return;
    case edit::EditStatus::kFailedPossiblyMutated:
    case edit::EditStatus::kFailedAfterMutation:
      EnterUncertainLockdown();
      return;
    case edit::EditStatus::kFailedBeforeMutation:
      // The engine already consumed the event and holds its result; an edit that failed
      // before any write leaves the engine view and the document diverged. P3.9: this is
      // NeedsResync, not a quiet return to a typeable state.
      ++edits_refused_;
      EnterNeedsResync("edit-failed-before-mutation",
                       RejectCause::kEditFailedBeforeMutation);
      return;
    case edit::EditStatus::kRejected:
    case edit::EditStatus::kPending:
    default:
      // Same divergence shape: read-only, a refused lock, a disconnected context or a
      // permission refusal inside the P3.7 callback, all after the engine ran.
      ++edits_refused_;
      EnterNeedsResync("edit-refused", RejectCause::kEditRefused);
      return;
  }
}

void ContextCoordinator::ApplyViewUpdate(const EditPlan& plan) {
  const std::u16string plan_text = Narrow(plan.text_utf16());
  const auto length = static_cast<LONG>(plan_text.size());
  switch (plan.operation()) {
    case edit::EditOperation::kBeginPreedit:
      view_.composition_active = true;
      view_.composition_start_utf16 = plan.selection_start_utf16();
      view_.composition_text_utf16 = plan_text;
      view_.caret_utf16 = view_.composition_start_utf16 + length;
      return;
    case edit::EditOperation::kUpdatePreedit:
      view_.composition_text_utf16 = plan_text;
      view_.caret_utf16 = view_.composition_start_utf16 + length;
      return;
    case edit::EditOperation::kCommit:
      if (view_.composition_active) {
        view_.caret_utf16 = view_.composition_start_utf16 + length;
      } else {
        view_.caret_utf16 = plan.selection_start_utf16() + length;
      }
      view_.composition_active = false;
      view_.composition_text_utf16.clear();
      return;
    case edit::EditOperation::kCancel:
      view_.caret_utf16 = view_.composition_start_utf16;
      view_.composition_active = false;
      view_.composition_text_utf16.clear();
      return;
  }
}

// --- terminal helpers --------------------------------------------------------------------

void ContextCoordinator::EndWork(WorkOutcome outcome, RejectCause cause) {
  outcome_ = outcome;
  cause_ = cause;
  phase_ = WorkPhase::kIdle;
  work_kind_ = WorkKind::kNone;
  work_abandoned_ = false;
  // The real caret read during our own edit wins over the plan-derived mirror, but only
  // when the work item really applied (or the document already matched). For a divergence
  // or abandonment the binding is blocked/invalid and no caret is invented.
  if (observed_caret_pending_) {
    if (outcome == WorkOutcome::kApplied || outcome == WorkOutcome::kNoEdit) {
      view_.caret_utf16 = observed_caret_utf16_;
      ++observed_caret_applied_;
    }
    observed_caret_pending_ = false;
  }
  pending_permission_ = PendingPermissionWork{};
}

void ContextCoordinator::EnterNeedsResync(const char* why, RejectCause cause) {
  (void)why;
  outcome_ = WorkOutcome::kNeedsResync;
  cause_ = cause;
  phase_ = WorkPhase::kIdle;
  work_kind_ = WorkKind::kNone;
  pending_permission_ = PendingPermissionWork{};
}

void ContextCoordinator::EnterUncertainLockdown() {
  uncertain_blocked_context_id_ = identity_.context_id;
  // Record the latch against the real context identity so a replacement coordinator
  // sharing the registry cannot clear or bypass it with another integer id.
  if (registry_ != nullptr && bound_context_ != nullptr) {
    registry_->MarkUncertain(bound_context_);
  }
  outcome_ = WorkOutcome::kUncertain;
  cause_ = RejectCause::kNone;
  phase_ = WorkPhase::kIdle;
  work_kind_ = WorkKind::kNone;
  pending_permission_ = PendingPermissionWork{};
}

void ContextCoordinator::DiscardPendingEngineResult() {
  if (phase_ != WorkPhase::kEnginePending || !engine_request_.valid()) {
    return;
  }
  engine::CommandResult discarded;
  if (executor_.TakeResult(engine_request_, &discarded) == engine::TakeStatus::kTaken) {
    ++engine_results_taken_;
  }
  // A not-ready result is left to the executor's own stop drain; nothing retries here.
  phase_ = WorkPhase::kIdle;
  work_kind_ = WorkKind::kNone;
  engine_request_ = engine::RequestId{};
}

// --- close -------------------------------------------------------------------------------

void ContextCoordinator::CloseRetirementResources(RetiringBinding* record) {
  if (record == nullptr) {
    return;
  }
  if (record->session != nullptr) {
    // Terminal release only. This never ends a composition, commits, cancels or clears
    // text; a composition that could not be finished is reported through cleanup_block.
    record->session->Close();
    record->session.reset();
  }
  if (record->context != nullptr) {
    record->context->Release();
    record->context = nullptr;
  }
}

void ContextCoordinator::Close() {
  if (closed_) {
    return;
  }
  if (bound_) {
    if (phase_ == WorkPhase::kEditPending ||
        phase_ == WorkPhase::kPermissionPending) {
      EndWork(WorkOutcome::kAbandonedStale, RejectCause::kNone);
    } else if (phase_ == WorkPhase::kEnginePending) {
      work_abandoned_ = true;
      outcome_ = WorkOutcome::kAbandonedStale;
      cause_ = RejectCause::kNone;
    }
    permission_source_.Abandon();
    RetireCurrentBinding();
  } else if (phase_ == WorkPhase::kEnginePending) {
    outcome_ = WorkOutcome::kAbandonedStale;
    DiscardPendingEngineResult();
  } else if (phase_ == WorkPhase::kEditPending ||
             phase_ == WorkPhase::kPermissionPending) {
    EndWork(WorkOutcome::kAbandonedStale, RejectCause::kNone);
  }
  // A permission read session still queued publishes nothing from now on; this returns
  // immediately and never waits for a callback that may never run.
  permission_source_.Abandon();
  pending_permission_ = PendingPermissionWork{};
  observed_caret_pending_ = false;

  // Give the executor one synchronous chance to publish each outstanding result, then
  // consume everything that is ready. Anything still outstanding is reported, not hidden:
  // StopAndJoin remains the terminal owner of the engine session.
  PumpRetirements();
  for (RetiringBinding& record : retiring_) {
    if (record.engine_result_pending) {
      engine::CommandResult discarded;
      if (executor_.TakeResult(record.engine_request, &discarded) ==
          engine::TakeStatus::kTaken) {
        ++engine_results_taken_;
        ++retirement_engine_results_discarded_;
        record.engine_result_pending = false;
      }
    }
    if (!record.close_terminal && !record.close_requested &&
        record.client.valid()) {
      engine::RequestId request;
      if (executor_.TryCloseClient(record.client, &request) ==
          engine::SubmitStatus::kAccepted) {
        record.close_requested = true;
        record.close_request = request;
        ++retirement_close_requests_;
      }
    }
    if (!record.close_terminal && record.close_requested) {
      engine::CommandResult closed;
      if (executor_.TakeResult(record.close_request, &closed) ==
          engine::TakeStatus::kTaken) {
        record.close_terminal = true;
        ++retirement_close_results_;
        if (!closed.ok()) {
          record.close_failed = true;
          ++retirement_close_failures_;
        }
      }
    }
    if (record.engine_result_pending) {
      ++retirement_unresolved_engine_results_;
    }
    if (!record.close_terminal) {
      ++retirement_unresolved_close_requests_;
    }
    CloseRetirementResources(&record);
  }
  closed_retirement_ = BuildRetirementSnapshot();
  retiring_.clear();

  session_.reset();
  if (bound_context_ != nullptr) {
    bound_context_->Release();
    bound_context_ = nullptr;
  }
  bound_ = false;
  closed_ = true;
  identity_ = edit::ContextIdentity{};
  engine_client_ = engine::ClientId{};
}

// --- snapshot ----------------------------------------------------------------------------

CoordinatorSnapshot ContextCoordinator::Snapshot() const {
  CoordinatorSnapshot snapshot;
  snapshot.bound = bound_;
  snapshot.closed = closed_;
  const bool registry_blocked = RegistryBlocks(bound_context_);
  snapshot.context_uncertain_blocked =
      registry_blocked ||
      (uncertain_blocked_context_id_ != 0 &&
       uncertain_blocked_context_id_ == identity_.context_id);
  if (closed_) {
    snapshot.gate = CoordinatorGate::kOpen;
  } else if (snapshot.context_uncertain_blocked) {
    snapshot.gate = CoordinatorGate::kBlockedUncertain;
  } else if (outcome_ == WorkOutcome::kNeedsResync) {
    snapshot.gate = CoordinatorGate::kBlockedNeedsResync;
  } else if (outcome_ == WorkOutcome::kUnsupportedShape) {
    snapshot.gate = CoordinatorGate::kBlockedUnsupportedShape;
  } else {
    snapshot.gate = CoordinatorGate::kOpen;
  }
  snapshot.identity = identity_;
  snapshot.permission = permission_;
  snapshot.work_kind = work_kind_;
  snapshot.phase = phase_;
  snapshot.outcome = outcome_;
  snapshot.cause = cause_;
  snapshot.work_id = work_id_;
  snapshot.work_generation = work_generation_;
  snapshot.engine_request = phase_ == WorkPhase::kEnginePending ? engine_request_
                                                               : engine::RequestId{};
  snapshot.last_edit_status = last_edit_status_;
  snapshot.last_edit_reason = last_edit_reason_;
  snapshot.tracked_caret_utf16 = view_.caret_utf16;
  snapshot.composition_active = view_.composition_active;
  snapshot.composition_start_utf16 = view_.composition_start_utf16;
  snapshot.composition_length_utf16 =
      static_cast<LONG>(view_.composition_text_utf16.size());
  snapshot.works_accepted = works_accepted_;
  snapshot.engine_results_taken = engine_results_taken_;
  snapshot.plans_submitted = plans_submitted_;
  snapshot.edits_applied = edits_applied_;
  snapshot.edits_refused = edits_refused_;
  snapshot.edits_stale = edits_stale_;
  snapshot.permission_queries_begun = permission_queries_begun_;
  snapshot.permission_queries_completed = permission_queries_completed_;
  snapshot.permission_rechecks = 0;
  if (session_ != nullptr) {
    snapshot.permission_rechecks = session_->Snapshot().permission_rechecks;
  }
  snapshot.pumps = pumps_;
  snapshot.observed_caret_updates = observed_caret_updates_;
  snapshot.observed_caret_applied = observed_caret_applied_;
  snapshot.owned_composition_active =
      session_ != nullptr && session_->active_composition();
  snapshot.retirement = closed_ ? closed_retirement_ : BuildRetirementSnapshot();
  return snapshot;
}

}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva
