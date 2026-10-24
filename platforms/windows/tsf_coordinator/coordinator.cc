// EpoCanva P3.8 -- implementation of the context-binding coordinator. The contract, the
// gate order, the work-item state machine and the lockdown rules are documented in
// coordinator.h; this file follows them line by line and adds no policy of its own.
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

}  // namespace

const char* DescribeBindStatus(BindStatus status) {
  switch (status) {
    case BindStatus::kOk:
      return "Ok";
    case BindStatus::kAlreadyBound:
      return "AlreadyBound";
    case BindStatus::kBusy:
      return "Busy";
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
    case RejectCause::kPermissionChangedWhileQueued:
      return "PermissionChangedWhileQueued";
    case RejectCause::kEngineRefused:
      return "EngineRefused";
    case RejectCause::kEngineUnavailable:
      return "EngineUnavailable";
    case RejectCause::kEditRefused:
      return "EditRefused";
    case RejectCause::kEditFailedBeforeMutation:
      return "EditFailedBeforeMutation";
    case RejectCause::kTakeFailed:
      return "TakeFailed";
  }
  return "UnknownRejectCause";
}

ContextCoordinator::ContextCoordinator(engine::EngineExecutor& executor,
                                       TfClientId tsf_client_id)
    : executor_(executor), tsf_client_id_(tsf_client_id) {}

ContextCoordinator::~ContextCoordinator() { Close(); }

// --- binding -----------------------------------------------------------------------------

BindStatus ContextCoordinator::Bind(ITfContext* context, std::uint64_t context_id,
                                    LONG initial_caret_utf16,
                                    engine::ClientId engine_client) {
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
  if (phase_ != WorkPhase::kIdle) {
    // A previous work item's engine result is still outstanding; single flight means the
    // new binding cannot accept work until it has been discarded.
    return BindStatus::kBusy;
  }
  if (uncertain_blocked_context_id_ != 0 &&
      uncertain_blocked_context_id_ == context_id) {
    return BindStatus::kBlockedUncertain;
  }

  session_ = std::make_unique<edit::CompositionEditSession>(tsf_client_id_);
  const std::uint64_t generation = ++generation_counter_;
  if (!session_->Bind(context, context_id, generation)) {
    session_.reset();
    return BindStatus::kInvalidRequest;
  }
  identity_ = edit::ContextIdentity{context_id, generation};
  engine_client_ = engine_client;
  bound_ = true;
  // A new binding starts with a clean work ledger. The NeedsResync/UnsupportedShape
  // blocks of the previous binding describe its engine view and are explicitly cleared by
  // this rebind (the uncertain block is keyed by context id and survives, above).
  outcome_ = WorkOutcome::kNone;
  cause_ = RejectCause::kNone;
  work_kind_ = WorkKind::kNone;
  // Every new binding starts from the safe refusal; the host grants Allowed explicitly.
  permission_ = edit::InputPermission::kUnknown;
  session_->SetInputPermission(permission_);
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
  if (phase_ == WorkPhase::kEditPending) {
    EndWork(WorkOutcome::kAbandonedStale, RejectCause::kNone);
  } else if (phase_ == WorkPhase::kEnginePending) {
    // The engine result, when it becomes ready, is consumed once and discarded.
    work_abandoned_ = true;
    outcome_ = WorkOutcome::kAbandonedStale;
    cause_ = RejectCause::kNone;
  }
  if (session_ != nullptr) {
    session_->Invalidate();
    session_->Close();
    session_.reset();
  }
  bound_ = false;
  identity_ = edit::ContextIdentity{};
  engine_client_ = engine::ClientId{};
  view_ = LocalDocumentView{};
  // NeedsResync / UnsupportedShape describe the engine view of one binding; an explicit
  // invalidation is the documented way out of them. The uncertain block is not cleared
  // here: it belongs to the context's document, whose text state is still unknown.
}

void ContextCoordinator::SetPermission(edit::InputPermission permission) {
  permission_ = permission;
  if (session_ != nullptr) {
    session_->SetInputPermission(permission);
  }
}

input::KeyDecision ContextCoordinator::Evaluate(const input::KeyEventSnapshot& event,
                                                const input::KeyText& text) const {
  input::InputContext context;
  context.input_enabled = true;
  context.engine_ready = bound_ && engine_client_.valid();
  context.composition_active = view_.composition_active;
  switch (permission_) {
    case edit::InputPermission::kAllowed:
      context.permission = input::InputPermission::kAllowed;
      break;
    case edit::InputPermission::kDenied:
      context.permission = input::InputPermission::kForbidden;
      break;
    case edit::InputPermission::kUnknown:
      context.permission = input::InputPermission::kUnknown;
      break;
  }
  return input::DecideKey(event, text, context);
}

// --- submit ------------------------------------------------------------------------------

bool ContextCoordinator::GateBlocksSubmit(SubmitStatus* status) const {
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

SubmitStatus ContextCoordinator::FinishSubmit(engine::RequestId request, WorkKind kind) {
  work_kind_ = kind;
  phase_ = WorkPhase::kEnginePending;
  outcome_ = WorkOutcome::kNone;
  cause_ = RejectCause::kNone;
  work_id_ = next_work_id_++;
  work_generation_ = identity_.generation;
  engine_request_ = request;
  work_abandoned_ = false;
  pending_plan_.reset();
  last_edit_status_ = edit::EditStatus::kRejected;
  last_edit_reason_ = edit::EditRejectReason::kNone;
  ++works_accepted_;
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
  SubmitStatus blocked = SubmitStatus::kAccepted;
  if (GateBlocksSubmit(&blocked)) {
    return blocked;
  }
  // The explicit permission gate precedes the engine-candidate evaluation: a privacy
  // refusal is the more specific fact about this field (P3.3 precedence, restated).
  if (permission_ == edit::InputPermission::kDenied) {
    return SubmitStatus::kPermissionDenied;
  }
  if (permission_ == edit::InputPermission::kUnknown) {
    return SubmitStatus::kPermissionUnknown;
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
  SubmitStatus blocked = SubmitStatus::kAccepted;
  if (GateBlocksSubmit(&blocked)) {
    return blocked;
  }
  if (permission_ == edit::InputPermission::kDenied) {
    return SubmitStatus::kPermissionDenied;
  }
  if (permission_ == edit::InputPermission::kUnknown) {
    return SubmitStatus::kPermissionUnknown;
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

// --- pump --------------------------------------------------------------------------------

void ContextCoordinator::Pump() {
  ++pumps_;
  if (closed_) {
    return;
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
        EnterNeedsResync("engine-command-failed");
        return;
    }
  }

  // The permission is re-verified here, not only at dispatch time: the work item is
  // rejected if the permission changed while it was queued, even though the engine already
  // consumed the event. No edit is requested for it.
  if (permission_ != edit::InputPermission::kAllowed) {
    EndWork(WorkOutcome::kRejected, RejectCause::kPermissionChangedWhileQueued);
    return;
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
    EnterNeedsResync("unexpected-command-kind");
    return;
  }

  ConvertAndSubmitEdit(observation);
}

void ContextCoordinator::ConvertAndSubmitEdit(const EngineObservation& observation) {
  const PlanDecision decision = DecidePlanFromEngine(view_, observation);
  switch (decision.kind) {
    case PlanDecisionKind::kNoEdit:
      EndWork(WorkOutcome::kNoEdit, RejectCause::kNone);
      return;
    case PlanDecisionKind::kUnsupportedShape:
      EndWork(WorkOutcome::kUnsupportedShape, RejectCause::kNone);
      return;
    case PlanDecisionKind::kInvalidObservation:
      EnterNeedsResync("invalid-engine-observation");
      return;
    case PlanDecisionKind::kBegin:
    case PlanDecisionKind::kUpdate:
    case PlanDecisionKind::kCommit:
    case PlanDecisionKind::kCancel:
      SubmitEditPlan(decision);
      return;
  }
}

void ContextCoordinator::SubmitEditPlan(const PlanDecision& decision) {
  // The plan builders take UTF-8 bytes; the engine text is UTF-16 code units. The strict
  // P3.2 converter is used in both directions, so a malformed value is refused, never
  // repaired. (EditPlan itself converts straight back through the same converter.)
  const auto widened = Widen(decision.text_utf16);
  const auto utf8 =
      epocanva::windows::text::ConvertUtf16CodeUnitsToUtf8Bytes(widened);
  if (!utf8.ok()) {
    EnterNeedsResync("engine-text-conversion-refused");
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
    EnterNeedsResync("plan-build-refused");
    return;
  }

  ++plans_submitted_;
  pending_plan_ = std::move(built.plan);
  const EditResult applied = session_->ApplyAsync(pending_plan_.value());
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
      ++edits_refused_;
      EndWork(WorkOutcome::kRejected, RejectCause::kEditFailedBeforeMutation);
      return;
    case edit::EditStatus::kRejected:
    case edit::EditStatus::kPending:
    default:
      ++edits_refused_;
      EndWork(WorkOutcome::kRejected, RejectCause::kEditRefused);
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
}

void ContextCoordinator::EnterNeedsResync(const char* why) {
  (void)why;
  outcome_ = WorkOutcome::kNeedsResync;
  cause_ = RejectCause::kNone;
  phase_ = WorkPhase::kIdle;
  work_kind_ = WorkKind::kNone;
}

void ContextCoordinator::EnterUncertainLockdown() {
  uncertain_blocked_context_id_ = identity_.context_id;
  outcome_ = WorkOutcome::kUncertain;
  cause_ = RejectCause::kNone;
  phase_ = WorkPhase::kIdle;
  work_kind_ = WorkKind::kNone;
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

void ContextCoordinator::Close() {
  if (closed_) {
    return;
  }
  if (phase_ == WorkPhase::kEnginePending) {
    outcome_ = WorkOutcome::kAbandonedStale;
    DiscardPendingEngineResult();
  } else if (phase_ == WorkPhase::kEditPending) {
    EndWork(WorkOutcome::kAbandonedStale, RejectCause::kNone);
  }
  if (session_ != nullptr) {
    session_->Invalidate();
    session_->Close();
    session_.reset();
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
  if (closed_) {
    snapshot.gate = CoordinatorGate::kOpen;
  } else if (uncertain_blocked_context_id_ != 0 &&
             uncertain_blocked_context_id_ == identity_.context_id) {
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
  snapshot.pumps = pumps_;
  return snapshot;
}

}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva
