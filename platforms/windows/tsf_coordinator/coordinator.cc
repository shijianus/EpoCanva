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
    case BindStatus::kTargetUnverified:
      return "TargetUnverified";
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
    case SubmitStatus::kTargetChanged:
      return "TargetChanged";
    case SubmitStatus::kThreadFocusLost:
      return "ThreadFocusLost";
    case SubmitStatus::kTargetUnverified:
      return "TargetUnverified";
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

const char* DescribeSubmitStage(SubmitStatus status) {
  switch (status) {
    case SubmitStatus::kAccepted:
      return "accepted";
    case SubmitStatus::kBusy:
      return "busy";
    case SubmitStatus::kRetiring:
      return "retiring";
    case SubmitStatus::kNotBound:
    case SubmitStatus::kClosed:
      return "binding";
    case SubmitStatus::kTargetChanged:
      return "target-context";
    case SubmitStatus::kThreadFocusLost:
    case SubmitStatus::kTargetUnverified:
      return "focus";
    case SubmitStatus::kPermissionDenied:
    case SubmitStatus::kPermissionUnknown:
      return "permission";
    case SubmitStatus::kBlockedUncertain:
      return "uncertain";
    case SubmitStatus::kBlockedNeedsResync:
    case SubmitStatus::kBlockedUnsupportedShape:
      return "needs-resync";
    case SubmitStatus::kNotEngineCandidate:
      return "not-applicable";
    case SubmitStatus::kEngineNotRunning:
    case SubmitStatus::kEngineClientUnavailable:
      return "engine-not-ready";
    case SubmitStatus::kQueueFull:
      return "queue";
    case SubmitStatus::kEngineSubmitFailed:
    case SubmitStatus::kInvalidRequest:
      return "request";
  }
  return "unknown";
}

const char* DescribeWorkKind(WorkKind kind) {
  switch (kind) {
    case WorkKind::kNone:
      return "None";
    case WorkKind::kKey:
      return "Key";
    case WorkKind::kCandidate:
      return "Candidate";
    case WorkKind::kRaw:
      return "Raw";
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
    case RejectCause::kTargetChangedWhileQueued:
      return "TargetChangedWhileQueued";
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
    case CleanupState::kTerminatedExternally:
      return "TerminatedExternally";
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
    case CleanupBlockReason::kShutdown:
      return "Shutdown";
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

bool ContextCoordinator::AttachTargetRevalidator(
    context::TargetRevalidator* revalidator) {
  if (revalidator == nullptr || closed_ || bound_) {
    return false;
  }
  target_revalidator_ = revalidator;
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
  // P3.13: the product binding path does not exist without a live target authority. The
  // controller attaches the observer before any bind; controlled unit cases attach a
  // test-owned implementation. This probe runs before the host creates an engine client.
  if (target_revalidator_ == nullptr) {
    return BindStatus::kTargetUnverified;
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

// --- P3.13 target revalidation ------------------------------------------------------------

SubmitStatus ContextCoordinator::RecordSubmit(SubmitStatus status) {
  last_submit_status_ = status;
  if (status != SubmitStatus::kAccepted) {
    ++submit_refusals_;
  }
  return status;
}

void ContextCoordinator::RecordTargetRevalidation(
    const context::TargetRevalidation& result) {
  last_target_revalidation_ = result;
  ++target_revalidations_;
  if (!result.ok()) {
    ++target_refusals_;
  }
}

SubmitStatus ContextCoordinator::RevalidateForSubmit() {
  if (!bound_) {
    return SubmitStatus::kNotBound;
  }
  if (target_revalidator_ == nullptr) {
    // No live target authority: conservative refusal, never an unchecked pass.
    return SubmitStatus::kTargetUnverified;
  }
  const context::TargetRevalidation result = target_revalidator_->Revalidate(
      bound_context_, identity_.context_id, identity_.generation);
  RecordTargetRevalidation(result);
  switch (result.status) {
    case context::TargetRevalidationStatus::kOk:
      return SubmitStatus::kAccepted;
    case context::TargetRevalidationStatus::kThreadFocusLost:
      return SubmitStatus::kThreadFocusLost;
    case context::TargetRevalidationStatus::kThreadFocusUnconfirmed:
    case context::TargetRevalidationStatus::kQueryFailed:
    case context::TargetRevalidationStatus::kClosed:
      return SubmitStatus::kTargetUnverified;
    case context::TargetRevalidationStatus::kNoDocumentManager:
    case context::TargetRevalidationStatus::kNoTopContext:
    case context::TargetRevalidationStatus::kContextMismatch:
    case context::TargetRevalidationStatus::kGenerationMismatch:
      return SubmitStatus::kTargetChanged;
  }
  return SubmitStatus::kTargetUnverified;
}

bool ContextCoordinator::EnsureTargetCurrent(const char* why,
                                             bool engine_already_ran) {
  if (!bound_ || target_revalidator_ == nullptr) {
    if (engine_already_ran) {
      EnterNeedsResync(why, RejectCause::kNone);
    } else {
      EndWork(WorkOutcome::kRejected, RejectCause::kNone);
    }
    return false;
  }
  const context::TargetRevalidation result = target_revalidator_->Revalidate(
      bound_context_, identity_.context_id, identity_.generation);
  RecordTargetRevalidation(result);
  if (result.ok()) {
    return true;
  }
  if (engine_already_ran) {
    // The engine consumed the event and the target moved. The result must not be applied
    // to the old context and must not be replayed elsewhere: this is NeedsResync.
    EnterNeedsResync(why, RejectCause::kTargetChangedWhileQueued);
  } else {
    // No engine call has been made for this work item. It is a clean refusal, not a
    // divergence, and no work is replayed.
    EndWork(WorkOutcome::kRejected, RejectCause::kTargetChangedWhileQueued);
  }
  return false;
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
  // The product path (BindObserved) requires a live target authority. Bind() is the
  // controlled unit-case seam; if no authority is attached there, Submit refuses input.
  if (!use_internal_generation && target_revalidator_ == nullptr) {
    return BindStatus::kTargetUnverified;
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

  std::shared_ptr<context::TargetWriteGuard> write_guard;
  if (target_revalidator_ != nullptr) {
    write_guard = target_revalidator_->CreateWriteGuard();
    if (write_guard == nullptr) {
      return BindStatus::kTargetUnverified;
    }
  }
  session_ = std::make_unique<edit::CompositionEditSession>(tsf_client_id_);
  if (use_internal_generation) {
    generation = ++generation_counter_;
  }
  if (!session_->Bind(context, context_id, generation)) {
    session_.reset();
    return BindStatus::kInvalidRequest;
  }
  if (write_guard != nullptr) {
    session_->SetTargetRecheck(
        [write_guard](ITfContext* session_context, std::uint64_t bound_context_id,
                      std::uint64_t bound_generation) {
          return write_guard->StillCurrent(session_context, bound_context_id,
                                           bound_generation);
        });
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
  permission_refresh_ = false;
  permission_refresh_attempted_ = false;
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
  permission_refresh_ = false;
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
  record.sequence = retirements_started_ + 1;
  bound_context_ = nullptr;
  engine_client_ = engine::ClientId{};

  // The old binding stops accepting input immediately. The session stays bound (but only
  // accepts an EndOwnedComposition plan) so PumpRetirements can end a composition this
  // writer really owns through one legal edit session. A pending plan is made Stale without
  // writing; no TSF call happens in this (possibly notification) context.
  if (record.session != nullptr) {
    record.session->BeginRetirement();
  } else if (record.context != nullptr) {
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

void ContextCoordinator::CaptureCleanupEvidence(RetiringBinding& record) {
  if (record.cleanup_evidence_captured) {
    return;
  }
  edit::OwnedCompositionState owned;
  if (record.session != nullptr) {
    owned = record.session->composition_state();
  }
  record.cleanup_evidence_captured = true;
  record.cleanup_owned_at_start = owned.ever_owned;
  record.cleanup_live_at_start = owned.live;
  record.cleanup_external_at_start = owned.externally_terminated;
  record.cleanup_ended_before_at_start = owned.ended_by_writer;
  record.cleanup_termination_notifications_at_start = owned.termination_notifications;
}

void ContextCoordinator::SettleRetirementCleanup(RetiringBinding& record,
                                                 CleanupState state,
                                                 CleanupBlockReason reason) {
  if (record.cleanup_finished) {
    // A record has exactly one terminal attribution; a repeated Pump, a late result or a
    // shutdown pass must never add a second one.
    return;
  }
  if (state != CleanupState::kFinished && state != CleanupState::kAlreadyOrdinary &&
      state != CleanupState::kTerminatedExternally && state != CleanupState::kBlocked) {
    // Pending/None are not settlements; refusing them keeps the ledger balanced.
    return;
  }
  record.cleanup = state;
  record.cleanup_block = reason;
  record.cleanup_finished = true;
  ++retirements_settled_;
  switch (state) {
    case CleanupState::kFinished:
      ++retirements_finished_;
      break;
    case CleanupState::kAlreadyOrdinary:
      ++retirements_already_ordinary_;
      break;
    case CleanupState::kTerminatedExternally:
      ++retirements_externally_terminated_;
      break;
    case CleanupState::kBlocked:
      ++retirements_blocked_;
      break;
    case CleanupState::kNone:
    case CleanupState::kPending:
      // Rejected above; kept for an exhaustive switch.
      break;
  }
  last_cleanup_state_ = state;
  last_cleanup_block_ = reason;
  last_cleanup_sequence_ = record.sequence;
  last_cleanup_context_id_ = record.identity.context_id;
  last_cleanup_generation_ = record.identity.generation;
  last_cleanup_owned_at_start_ = record.cleanup_owned_at_start;
  last_cleanup_live_at_start_ = record.cleanup_live_at_start;
  last_cleanup_external_at_start_ = record.cleanup_external_at_start;
  last_cleanup_ended_before_at_start_ = record.cleanup_ended_before_at_start;
  last_cleanup_termination_notifications_at_start_ =
      record.cleanup_termination_notifications_at_start;
  last_cleanup_plan_submitted_ = record.cleanup_plan_submitted;
  last_cleanup_plan_session_id_ = record.cleanup_plan_session_id;
  last_cleanup_end_issued_ = record.cleanup_end_issued;
  last_cleanup_end_hr_ = record.cleanup_end_hr;
  last_cleanup_termination_notifications_after_call_ =
      record.cleanup_termination_notifications_after_call;
  last_cleanup_termination_notifications_at_settle_ =
      record.cleanup_termination_notifications_at_settle;
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

    if (!record->cleanup_finished) {
      if (record->cleanup == CleanupState::kPending) {
        edit::CompositionEditSession* session = record->session.get();
        if (session == nullptr) {
          // A submitted cleanup plan lives in that session; losing it is a reported
          // failure, never an ordinary success.
          SettleRetirementCleanup(
              *record,
              record->cleanup_plan_submitted ? CleanupState::kBlocked
                                             : CleanupState::kAlreadyOrdinary,
              record->cleanup_plan_submitted ? CleanupBlockReason::kRequestFailed
                                             : CleanupBlockReason::kNone);
        } else if (!session->HasPending() ||
                   ::WaitForSingleObject(session->CompletionEvent(), 0) ==
                       WAIT_OBJECT_0) {
          ResolveRetirementCleanup(*record, session->PendingResult());
        }
      } else if (record->cleanup == CleanupState::kNone) {
        edit::CompositionEditSession* session = record->session.get();
        // Capture what this record owned when the cleanup operation started, before any
        // decision or TSF call. Later attribution reads this record, not a null member.
        CaptureCleanupEvidence(*record);
        if (session == nullptr) {
          SettleRetirementCleanup(*record, CleanupState::kAlreadyOrdinary);
        } else if (session->HasUncertainOutcome()) {
          // The previous write's effect was never confirmed. Do not call TSF at all: the
          // unknown composition range must not be ended, committed, cancelled or cleared.
          SettleRetirementCleanup(*record, CleanupState::kBlocked,
                                  CleanupBlockReason::kUncertainTextState);
        } else if (!session->active_composition()) {
          // No live composition now. The text, if any, is ordinary document text; never
          // trace back and delete it. The operation evidence decides the attribution: a
          // writer that never owned a composition, or that ended its last one through its
          // own successful call, leaves AlreadyOrdinary. A composition lost to another
          // party is an external termination -- never this cleanup's success and never
          // masked as ordinary. No cleanup session is submitted, so an abandoned plan may
          // still be waiting for its lock -- it will see the closed retirement session and
          // write nothing.
          const edit::OwnedCompositionState owned = session->composition_state();
          record->cleanup_termination_notifications_at_settle =
              owned.termination_notifications;
          if (owned.ever_owned && !owned.ended_by_writer) {
            SettleRetirementCleanup(*record, CleanupState::kTerminatedExternally);
          } else {
            SettleRetirementCleanup(*record, CleanupState::kAlreadyOrdinary);
          }
        } else if (session->HasPending()) {
          // A composition must be ended, but an accepted plan from before retirement is
          // still in flight. Let it reach its own Stale terminal first (no write), then a
          // cleanup session is submitted. Until then the new binding stays Retiring.
          if (::WaitForSingleObject(session->CompletionEvent(), 0) ==
              WAIT_OBJECT_0) {
            (void)session->PendingResult();  // Stale; discarded, never applied
          }
        } else {
          edit::EditPlanBuildResult built =
              edit::EditPlan::BuildEndOwnedComposition(record->identity);
          if (!built.ok()) {
            SettleRetirementCleanup(*record, CleanupState::kBlocked,
                                    CleanupBlockReason::kPlanBuildFailed);
          } else {
            const edit::EditResult submitted =
                session->ApplyAsync(built.plan.value());
            // A precheck refusal has session_id 0: no edit session was requested. Only a
            // requested plan may claim that this cleanup's EndComposition was issued.
            record->cleanup_plan_submitted = submitted.session_id != 0;
            record->cleanup_plan_session_id = submitted.session_id;
            record->cleanup = submitted.pending() ? CleanupState::kPending
                                                  : CleanupState::kNone;
            if (!submitted.pending()) {
              ResolveRetirementCleanup(*record, submitted);
            }
          }
        }
      }
    }

    if (!record->engine_result_pending && record->close_terminal &&
        record->cleanup_finished) {
      CloseRetirementResources(&*record);
      record = retiring_.erase(record);
      continue;
    }
    ++record;
  }
}

void ContextCoordinator::ResolveRetirementCleanup(
    RetiringBinding& record, const edit::EditResult& result) {
  if (record.cleanup_finished) {
    return;
  }
  if (result.pending()) {
    record.cleanup = CleanupState::kPending;
    return;
  }
  // Record this operation's own end-call evidence before classifying its status. A status
  // is never used to infer an end call that this operation did not make.
  record.cleanup_end_issued = result.end_composition_issued;
  record.cleanup_end_hr = result.end_composition_hr;
  record.cleanup_termination_notifications_after_call =
      result.termination_notifications_after_call;
  record.cleanup_termination_notifications_at_settle =
      result.termination_notifications_after_call;
  if (result.end_composition_issued) {
    ++retirement_cleanup_end_calls_;
    if (FAILED(result.end_composition_hr)) {
      ++retirement_cleanup_end_failures_;
    }
  }

  switch (result.status) {
    case edit::EditStatus::kApplied:
      // Finished requires THIS operation's own successful EndComposition call. An Applied
      // result without that call is a contract contradiction and is never a success.
      if (!result.end_composition_issued) {
        SettleRetirementCleanup(record, CleanupState::kBlocked,
                                CleanupBlockReason::kRequestFailed);
        return;
      }
      if (FAILED(result.end_composition_hr)) {
        SettleRetirementCleanup(record, CleanupState::kBlocked,
                                CleanupBlockReason::kEndCompositionFailed);
        return;
      }
      // The composition object was closed and the displayed text remained in place. This
      // is a real finish, not a commit: no text call was issued by the cleanup plan.
      SettleRetirementCleanup(record, CleanupState::kFinished);
      return;
    case edit::EditStatus::kRejected:
      switch (result.reason) {
        case edit::EditRejectReason::kCompositionTerminated:
          if (result.end_composition_issued) {
            // An end call was issued but this result is not its success. Never call that
            // an external termination.
            SettleRetirementCleanup(
                record, CleanupState::kBlocked,
                FAILED(result.end_composition_hr)
                    ? CleanupBlockReason::kEndCompositionFailed
                    : CleanupBlockReason::kRequestFailed);
            return;
          }
          // TSF reported the composition terminated by another party; this cleanup issued
          // no end call. It is not a success of this operation.
          SettleRetirementCleanup(record, CleanupState::kTerminatedExternally);
          return;
        case edit::EditRejectReason::kNoComposition:
          if (result.end_composition_issued) {
            SettleRetirementCleanup(record, CleanupState::kBlocked,
                                    CleanupBlockReason::kRequestFailed);
            return;
          }
          // The plan was built while the writer held a live composition, so its absence at
          // callback time means another party took it. Never count that as this cleanup's
          // success; without any prior ownership it is truly ordinary.
          SettleRetirementCleanup(
              record, record.cleanup_owned_at_start
                          ? CleanupState::kTerminatedExternally
                          : CleanupState::kAlreadyOrdinary);
          return;
        case edit::EditRejectReason::kPermissionDenied:
          SettleRetirementCleanup(record, CleanupState::kBlocked,
                                  CleanupBlockReason::kPermissionDenied);
          return;
        case edit::EditRejectReason::kPermissionUnknown:
          SettleRetirementCleanup(record, CleanupState::kBlocked,
                                  CleanupBlockReason::kPermissionUnknown);
          return;
        case edit::EditRejectReason::kReadOnlyContext:
          SettleRetirementCleanup(record, CleanupState::kBlocked,
                                  CleanupBlockReason::kReadOnly);
          return;
        case edit::EditRejectReason::kContextDisconnected:
          SettleRetirementCleanup(record, CleanupState::kBlocked,
                                  CleanupBlockReason::kContextDisconnected);
          return;
        case edit::EditRejectReason::kUncertainTextState:
          SettleRetirementCleanup(record, CleanupState::kBlocked,
                                  CleanupBlockReason::kUncertainTextState);
          return;
        case edit::EditRejectReason::kCompositionRangeMismatch:
          SettleRetirementCleanup(record, CleanupState::kBlocked,
                                  CleanupBlockReason::kRangeMismatch);
          return;
        case edit::EditRejectReason::kContextLocked:
        case edit::EditRejectReason::kSyncUnavailable:
        case edit::EditRejectReason::kWriteLockRefused:
          SettleRetirementCleanup(record, CleanupState::kBlocked,
                                  CleanupBlockReason::kLockRefused);
          return;
        case edit::EditRejectReason::kEndCompositionFailed:
          SettleRetirementCleanup(record, CleanupState::kBlocked,
                                  CleanupBlockReason::kEndCompositionFailed);
          return;
        case edit::EditRejectReason::kStaleGeneration:
        case edit::EditRejectReason::kIdentityMismatch:
        case edit::EditRejectReason::kNotBound:
          SettleRetirementCleanup(record, CleanupState::kBlocked,
                                  CleanupBlockReason::kStale);
          return;
        default:
          SettleRetirementCleanup(record, CleanupState::kBlocked,
                                  CleanupBlockReason::kRequestFailed);
          return;
      }
    case edit::EditStatus::kFailedBeforeMutation:
      SettleRetirementCleanup(
          record, CleanupState::kBlocked,
          result.reason == edit::EditRejectReason::kEndCompositionFailed
              ? CleanupBlockReason::kEndCompositionFailed
              : CleanupBlockReason::kRequestFailed);
      return;
    case edit::EditStatus::kFailedPossiblyMutated:
    case edit::EditStatus::kFailedAfterMutation:
      SettleRetirementCleanup(record, CleanupState::kBlocked,
                              CleanupBlockReason::kUncertainTextState);
      return;
    case edit::EditStatus::kStale:
      SettleRetirementCleanup(record, CleanupState::kBlocked,
                              CleanupBlockReason::kStale);
      return;
    case edit::EditStatus::kPending:
      record.cleanup = CleanupState::kPending;
      return;
  }
  SettleRetirementCleanup(record, CleanupState::kBlocked,
                          CleanupBlockReason::kRequestFailed);
}

bool RetirementAccountingBalanced(const RetirementSnapshot& snapshot) {
  const std::uint64_t attributed = snapshot.finished + snapshot.already_ordinary +
                                   snapshot.externally_terminated + snapshot.blocked;
  return snapshot.settled == attributed;
}

bool RetirementCleanupEvidenceConsistent(const RetirementSnapshot& snapshot) {
  switch (snapshot.last_cleanup_state) {
    case CleanupState::kFinished:
      // This cleanup's own EndComposition was issued and succeeded, starting from a live
      // composition the writer had not already ended.
      return snapshot.last_cleanup_plan_submitted && snapshot.last_cleanup_end_issued &&
             SUCCEEDED(snapshot.last_cleanup_end_hr) &&
             snapshot.last_cleanup_owned_at_start &&
             snapshot.last_cleanup_live_at_start &&
             !snapshot.last_cleanup_ended_before_at_start;
    case CleanupState::kAlreadyOrdinary:
      // No end call by this cleanup, and either there was no owned composition or the
      // writer had already ended it normally.
      return !snapshot.last_cleanup_end_issued &&
             (!snapshot.last_cleanup_owned_at_start ||
              snapshot.last_cleanup_ended_before_at_start);
    case CleanupState::kTerminatedExternally:
      // An owned composition that this writer had not ended, taken by another party. The
      // termination is evidenced either by the state observed when cleanup started or by a
      // termination notification that arrived while cleanup was waiting. The latter also
      // covers the window in which an older accepted plan still owed its Stale terminal and
      // no cleanup plan had been submitted yet.
      return !snapshot.last_cleanup_end_issued &&
             snapshot.last_cleanup_owned_at_start &&
             !snapshot.last_cleanup_ended_before_at_start &&
             (snapshot.last_cleanup_external_at_start ||
              snapshot.last_cleanup_termination_notifications_at_settle >
                  snapshot.last_cleanup_termination_notifications_at_start);
    case CleanupState::kBlocked:
      // A named reason, and no successful end claimed as the outcome.
      return snapshot.last_cleanup_block != CleanupBlockReason::kNone &&
             !(snapshot.last_cleanup_end_issued &&
               SUCCEEDED(snapshot.last_cleanup_end_hr));
    case CleanupState::kNone:
    case CleanupState::kPending:
      return false;
  }
  return false;
}

RetirementSnapshot ContextCoordinator::BuildRetirementSnapshot() const {
  RetirementSnapshot snapshot;
  snapshot.retiring_bindings = retiring_.size();
  snapshot.capacity = kRetirementCapacity;
  snapshot.admission_paused = !retiring_.empty();
  snapshot.started = retirements_started_;
  snapshot.settled = retirements_settled_;
  snapshot.finished = retirements_finished_;
  snapshot.already_ordinary = retirements_already_ordinary_;
  snapshot.blocked = retirements_blocked_;
  snapshot.externally_terminated = retirements_externally_terminated_;
  snapshot.cleanup_end_calls = retirement_cleanup_end_calls_;
  snapshot.cleanup_end_failures = retirement_cleanup_end_failures_;
  snapshot.last_cleanup_sequence = last_cleanup_sequence_;
  snapshot.last_cleanup_context_id = last_cleanup_context_id_;
  snapshot.last_cleanup_generation = last_cleanup_generation_;
  snapshot.last_cleanup_owned_at_start = last_cleanup_owned_at_start_;
  snapshot.last_cleanup_live_at_start = last_cleanup_live_at_start_;
  snapshot.last_cleanup_external_at_start = last_cleanup_external_at_start_;
  snapshot.last_cleanup_ended_before_at_start = last_cleanup_ended_before_at_start_;
  snapshot.last_cleanup_termination_notifications_at_start =
      last_cleanup_termination_notifications_at_start_;
  snapshot.last_cleanup_plan_submitted = last_cleanup_plan_submitted_;
  snapshot.last_cleanup_plan_session_id = last_cleanup_plan_session_id_;
  snapshot.last_cleanup_end_issued = last_cleanup_end_issued_;
  snapshot.last_cleanup_end_hr = last_cleanup_end_hr_;
  snapshot.last_cleanup_termination_notifications_after_call =
      last_cleanup_termination_notifications_after_call_;
  snapshot.last_cleanup_termination_notifications_at_settle =
      last_cleanup_termination_notifications_at_settle_;
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
    if (record.session != nullptr && record.session->active_composition()) {
      ++snapshot.retiring_compositions;
    }
    if (record.cleanup == CleanupState::kPending) {
      snapshot.admission_paused = true;
    }
    if (snapshot.pending_edit_event == nullptr && record.session != nullptr &&
        record.session->HasPending()) {
      // This is either the retirement cleanup or the abandoned accepted plan it must wait
      // for. Either way it is a real completion fact the host may bounded-wait on.
      snapshot.pending_edit_event = record.session->CompletionEvent();
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

bool ContextCoordinator::RefreshPermission() {
  if (closed_ || phase_ != WorkPhase::kIdle || !bound_ || RetirementPausesInput() ||
      permission_refresh_attempted_) {
    return false;
  }
  SubmitStatus blocked = SubmitStatus::kAccepted;
  if (GateBlocksSubmit(&blocked)) {
    return false;
  }
  if (!StartPermissionQuery()) {
    return false;
  }
  permission_refresh_attempted_ = true;
  if (permission_source_.Status() == context::PermissionQueryStatus::kPending) {
    permission_refresh_ = true;
    pending_permission_ = PendingPermissionWork{};
    phase_ = WorkPhase::kPermissionPending;
    return true;
  }
  bool identity_current = false;
  (void)PermissionVerdictAllows(&identity_current);
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
  raw_terminal_noted_ = false;
  pending_plan_.reset();
  last_edit_status_ = edit::EditStatus::kRejected;
  last_edit_reason_ = edit::EditRejectReason::kNone;
  last_edit_request_hr_ = S_OK;
  last_edit_session_hr_ = S_OK;
  last_edit_operation_hr_ = S_OK;
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
    return RecordSubmit(SubmitStatus::kInvalidRequest);
  }
  id->value = 0;
  if (closed_) {
    return RecordSubmit(SubmitStatus::kClosed);
  }
  if (phase_ != WorkPhase::kIdle) {
    return RecordSubmit(SubmitStatus::kBusy);
  }
  if (!bound_) {
    return RecordSubmit(SubmitStatus::kNotBound);
  }
  if (RetirementPausesInput()) {
    // A previous binding still owes an engine result, a close, or a TSF cleanup. This is a
    // fact about the host, not about this key: nothing was processed and nothing is dropped
    // silently; the status and the snapshot name the pause.
    return RecordSubmit(SubmitStatus::kRetiring);
  }
  SubmitStatus blocked = SubmitStatus::kAccepted;
  if (GateBlocksSubmit(&blocked)) {
    return RecordSubmit(blocked);
  }
  // P3.13: revalidate the real target BEFORE the permission read and the engine. The
  // binding identity/generation alone cannot prove the thread still points at it.
  const SubmitStatus target = RevalidateForSubmit();
  if (target != SubmitStatus::kAccepted) {
    return RecordSubmit(target);
  }
  // The real permission gate precedes the engine-candidate evaluation: a privacy
  // refusal is the more specific fact about this field (P3.3 precedence, restated).
  if (!StartPermissionQuery()) {
    return RecordSubmit(SubmitStatus::kPermissionUnknown);
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
    last_submit_status_ = SubmitStatus::kAccepted;
    return SubmitStatus::kAccepted;
  }
  bool identity_current = false;
  if (!PermissionVerdictAllows(&identity_current)) {
    if (!identity_current) {
      // no usable verdict for this binding
      return RecordSubmit(SubmitStatus::kPermissionUnknown);
    }
    return RecordSubmit(permission_ == edit::InputPermission::kDenied
                            ? SubmitStatus::kPermissionDenied
                            : SubmitStatus::kPermissionUnknown);
  }
  if (!Evaluate(event, text).is_engine_candidate()) {
    return RecordSubmit(SubmitStatus::kNotEngineCandidate);
  }
  engine::RequestId request;
  const engine::SubmitStatus submitted =
      executor_.TryDispatch(engine_client_, event, text, &request);
  switch (submitted) {
    case engine::SubmitStatus::kAccepted:
      FinishSubmit(request, WorkKind::kKey);
      *id = WorkId{work_id_};
      last_submit_status_ = SubmitStatus::kAccepted;
      return SubmitStatus::kAccepted;
    case engine::SubmitStatus::kNotRunning:
      return RecordSubmit(SubmitStatus::kEngineNotRunning);
    case engine::SubmitStatus::kQueueFull:
      return RecordSubmit(SubmitStatus::kQueueFull);
    case engine::SubmitStatus::kClientUnknown:
    case engine::SubmitStatus::kClientNotOpen:
      return RecordSubmit(SubmitStatus::kEngineClientUnavailable);
    case engine::SubmitStatus::kInternalFailure:
      return RecordSubmit(SubmitStatus::kEngineSubmitFailed);
    case engine::SubmitStatus::kInvalidRequest:
    default:
      return RecordSubmit(SubmitStatus::kInvalidRequest);
  }
}

SubmitStatus ContextCoordinator::SubmitCandidate(std::size_t candidate_index, WorkId* id) {
  if (id == nullptr) {
    return RecordSubmit(SubmitStatus::kInvalidRequest);
  }
  id->value = 0;
  if (closed_) {
    return RecordSubmit(SubmitStatus::kClosed);
  }
  if (phase_ != WorkPhase::kIdle) {
    return RecordSubmit(SubmitStatus::kBusy);
  }
  if (!bound_) {
    return RecordSubmit(SubmitStatus::kNotBound);
  }
  if (RetirementPausesInput()) {
    return RecordSubmit(SubmitStatus::kRetiring);
  }
  SubmitStatus blocked = SubmitStatus::kAccepted;
  if (GateBlocksSubmit(&blocked)) {
    return RecordSubmit(blocked);
  }
  // P3.13: accepting a candidate is ordinary input; the same live target check applies.
  const SubmitStatus target = RevalidateForSubmit();
  if (target != SubmitStatus::kAccepted) {
    return RecordSubmit(target);
  }
  if (!StartPermissionQuery()) {
    return RecordSubmit(SubmitStatus::kPermissionUnknown);
  }
  if (permission_source_.Status() == context::PermissionQueryStatus::kPending) {
    FinishWorkLedger(WorkKind::kCandidate);
    phase_ = WorkPhase::kPermissionPending;
    engine_request_ = engine::RequestId{};
    pending_permission_ = PendingPermissionWork{};
    pending_permission_.kind = WorkKind::kCandidate;
    pending_permission_.candidate_index = candidate_index;
    *id = WorkId{work_id_};
    last_submit_status_ = SubmitStatus::kAccepted;
    return SubmitStatus::kAccepted;
  }
  bool identity_current = false;
  if (!PermissionVerdictAllows(&identity_current)) {
    if (!identity_current) {
      return RecordSubmit(SubmitStatus::kPermissionUnknown);
    }
    return RecordSubmit(permission_ == edit::InputPermission::kDenied
                            ? SubmitStatus::kPermissionDenied
                            : SubmitStatus::kPermissionUnknown);
  }
  engine::RequestId request;
  const engine::SubmitStatus submitted =
      executor_.TryAcceptCandidate(engine_client_, candidate_index, &request);
  switch (submitted) {
    case engine::SubmitStatus::kAccepted:
      FinishSubmit(request, WorkKind::kCandidate);
      *id = WorkId{work_id_};
      last_submit_status_ = SubmitStatus::kAccepted;
      return SubmitStatus::kAccepted;
    case engine::SubmitStatus::kNotRunning:
      return RecordSubmit(SubmitStatus::kEngineNotRunning);
    case engine::SubmitStatus::kQueueFull:
      return RecordSubmit(SubmitStatus::kQueueFull);
    case engine::SubmitStatus::kClientUnknown:
    case engine::SubmitStatus::kClientNotOpen:
      return RecordSubmit(SubmitStatus::kEngineClientUnavailable);
    case engine::SubmitStatus::kInternalFailure:
      return RecordSubmit(SubmitStatus::kEngineSubmitFailed);
    case engine::SubmitStatus::kInvalidRequest:
    default:
      return RecordSubmit(SubmitStatus::kInvalidRequest);
  }
}

SubmitStatus ContextCoordinator::SubmitRawCommit(std::u16string_view raw_utf16,
                                                 WorkId* id) {
  if (id == nullptr) {
    return RecordSubmit(SubmitStatus::kInvalidRequest);
  }
  id->value = 0;
  if (raw_utf16.empty()) {
    return RecordSubmit(SubmitStatus::kInvalidRequest);
  }
  if (closed_) {
    return RecordSubmit(SubmitStatus::kClosed);
  }
  if (phase_ != WorkPhase::kIdle) {
    return RecordSubmit(SubmitStatus::kBusy);
  }
  if (!bound_) {
    return RecordSubmit(SubmitStatus::kNotBound);
  }
  if (RetirementPausesInput()) {
    return RecordSubmit(SubmitStatus::kRetiring);
  }
  SubmitStatus blocked = SubmitStatus::kAccepted;
  if (GateBlocksSubmit(&blocked)) {
    return RecordSubmit(blocked);
  }
  const SubmitStatus target = RevalidateForSubmit();
  if (target != SubmitStatus::kAccepted) {
    return RecordSubmit(target);
  }
  if (!StartPermissionQuery()) {
    return RecordSubmit(SubmitStatus::kPermissionUnknown);
  }
  if (permission_source_.Status() == context::PermissionQueryStatus::kPending) {
    FinishWorkLedger(WorkKind::kRaw);
    phase_ = WorkPhase::kPermissionPending;
    engine_request_ = engine::RequestId{};
    pending_permission_ = PendingPermissionWork{};
    pending_permission_.kind = WorkKind::kRaw;
    pending_permission_.raw_text = std::u16string(raw_utf16);
    *id = WorkId{work_id_};
    last_submit_status_ = SubmitStatus::kAccepted;
    return SubmitStatus::kAccepted;
  }
  bool identity_current = false;
  if (!PermissionVerdictAllows(&identity_current)) {
    if (!identity_current) {
      return RecordSubmit(SubmitStatus::kPermissionUnknown);
    }
    return RecordSubmit(permission_ == edit::InputPermission::kDenied
                            ? SubmitStatus::kPermissionDenied
                            : SubmitStatus::kPermissionUnknown);
  }
  FinishWorkLedger(WorkKind::kRaw);
  SubmitRawPlan(std::u16string(raw_utf16));
  *id = WorkId{work_id_};
  last_submit_status_ = SubmitStatus::kAccepted;
  return SubmitStatus::kAccepted;
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
  if (permission_refresh_) {
    permission_refresh_ = false;
    if (status == context::PermissionQueryStatus::kIdle) {
      EndWork(WorkOutcome::kRejected, RejectCause::kPermissionUnknown);
    } else {
      bool refresh_current = false;
      (void)PermissionVerdictAllows(&refresh_current);
      EndWork(WorkOutcome::kNoEdit, RejectCause::kNone);
    }
    pending_permission_ = PendingPermissionWork{};
    return;
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
  // P3.13: the target is revalidated after the permission answer and immediately before
  // the engine call or the held edit plan. A pre-engine work item is a clean refusal; the
  // apply-time path (has_plan) has already consumed the engine event and is NeedsResync.
  if (!EnsureTargetCurrent("target-changed-after-permission",
                           pending_permission_.has_plan)) {
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
  if (work.kind == WorkKind::kRaw) {
    // The degraded run's permission read finished Allowed; no engine call exists for it.
    SubmitRawPlan(std::move(work.raw_text));
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

  // An edit is now unavoidable, so the target and the permission are re-resolved from the
  // CURRENT real state (not read back from dispatch time). The engine already consumed the
  // event; a target move or a non-Allowed verdict now is an engine/host divergence and
  // lands on NeedsResync, never on a quiet drop and never on a write to the old context.
  if (!EnsureTargetCurrent("target-changed-after-engine", true)) {
    return;
  }
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

void ContextCoordinator::SubmitRawPlan(std::u16string raw_utf16) {
  PlanDecision decision;
  decision.kind = PlanDecisionKind::kCommit;
  last_raw_units_ = raw_utf16.size();
  ++raw_commits_begun_;
  if (view_.composition_active) {
    // Preserve what the engine already displayed (the applied part of the token) and append
    // the captured suffix in one commit. The engine never saw the suffix; that divergence is
    // exactly why the host must rebuild the binding after this work applies.
    decision.text_utf16 = view_.composition_text_utf16;
    decision.text_utf16 += raw_utf16;
    decision.caret_utf16 = view_.composition_start_utf16;
  } else {
    decision.text_utf16 = std::move(raw_utf16);
    decision.caret_utf16 = view_.caret_utf16;
  }
  SubmitEditPlan(decision, false);
}

void ContextCoordinator::NoteRawTerminal(bool applied) {
  if (work_kind_ != WorkKind::kRaw || raw_terminal_noted_) {
    return;
  }
  raw_terminal_noted_ = true;
  if (applied) {
    ++raw_commits_applied_;
    raw_units_applied_ += last_raw_units_;
  } else {
    ++raw_commits_failed_;
    raw_units_refused_ += last_raw_units_;
  }
}

void ContextCoordinator::SubmitEditPlan(const PlanDecision& decision,
                                        bool engine_already_ran) {
  // The last synchronous check before a TSF request. The P3.7 session repeats the live
  // target check before requesting a lock and again inside the granted callback; this one
  // keeps an already-moved target from even reaching RequestEditSession.
  if (!EnsureTargetCurrent("target-changed-before-edit", engine_already_ran)) {
    NoteRawTerminal(false);
    return;
  }
  // The plan builders take UTF-8 bytes; the engine text is UTF-16 code units. The strict
  // P3.2 converter is used in both directions, so a malformed value is refused, never
  // repaired. (EditPlan itself converts straight back through the same converter.)
  const auto widened = Widen(decision.text_utf16);
  const auto utf8 =
      epocanva::windows::text::ConvertUtf16CodeUnitsToUtf8Bytes(widened);
  if (!utf8.ok()) {
    NoteRawTerminal(false);
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
    NoteRawTerminal(false);
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
  last_edit_request_hr_ = result.request_hr;
  last_edit_session_hr_ = result.edit_session_hr;
  last_edit_operation_hr_ = result.operation_hr;

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
      if (result.status == edit::EditStatus::kRejected &&
          result.reason == edit::EditRejectReason::kTargetChanged) {
        // The P3.7 callback's live target check refused the write after the engine ran.
        // The engine's result and the document have diverged; no retry, no replay.
        ++edits_refused_;
        EnterNeedsResync("edit-target-changed",
                         RejectCause::kTargetChangedWhileQueued);
        return;
      }
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
    case edit::EditOperation::kEndOwnedComposition:
      // Retirement cleanup leaves the text and caret exactly where they were; only the
      // composition ownership ends. This path is not used for a current-binding work item.
      view_.composition_active = false;
      view_.composition_text_utf16.clear();
      return;
  }
}

// --- terminal helpers --------------------------------------------------------------------

void ContextCoordinator::EndWork(WorkOutcome outcome, RejectCause cause) {
  NoteRawTerminal(outcome == WorkOutcome::kApplied);
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
  NoteRawTerminal(false);
  outcome_ = WorkOutcome::kNeedsResync;
  cause_ = cause;
  phase_ = WorkPhase::kIdle;
  work_kind_ = WorkKind::kNone;
  pending_permission_ = PendingPermissionWork{};
}

void ContextCoordinator::EnterUncertainLockdown() {
  NoteRawTerminal(false);
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
    if (!record.cleanup_finished) {
      // Close() is terminal and cannot dispatch the STA message loop, so a pending cleanup
      // edit session cannot be waited for here. Never report it as settled success: a
      // composition that is still live is named as blocked at shutdown.
      CaptureCleanupEvidence(record);
      if (record.session != nullptr && record.session->HasUncertainOutcome()) {
        // The unknown write's effect outranks the current pointer state: a composition that
        // was closed after an unconfirmed write is still not a normal finish.
        SettleRetirementCleanup(record, CleanupState::kBlocked,
                                CleanupBlockReason::kUncertainTextState);
      } else if (record.session == nullptr || !record.session->active_composition()) {
        const edit::OwnedCompositionState owned =
            record.session != nullptr ? record.session->composition_state()
                                      : edit::OwnedCompositionState{};
        record.cleanup_termination_notifications_at_settle =
            owned.termination_notifications;
        if (owned.ever_owned && !owned.ended_by_writer) {
          SettleRetirementCleanup(record, CleanupState::kTerminatedExternally);
        } else {
          SettleRetirementCleanup(record, CleanupState::kAlreadyOrdinary);
        }
      } else {
        SettleRetirementCleanup(record, CleanupState::kBlocked,
                                CleanupBlockReason::kShutdown);
      }
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
  snapshot.last_submit_status = last_submit_status_;
  snapshot.submit_refusals = submit_refusals_;
  snapshot.last_target_revalidation = last_target_revalidation_;
  snapshot.target_revalidations = target_revalidations_;
  snapshot.target_refusals = target_refusals_;
  snapshot.work_generation = work_generation_;
  snapshot.engine_request = phase_ == WorkPhase::kEnginePending ? engine_request_
                                                               : engine::RequestId{};
  snapshot.last_edit_status = last_edit_status_;
  snapshot.last_edit_reason = last_edit_reason_;
  snapshot.last_edit_request_hr = last_edit_request_hr_;
  snapshot.last_edit_session_hr = last_edit_session_hr_;
  snapshot.last_edit_operation_hr = last_edit_operation_hr_;
  snapshot.tracked_caret_utf16 = view_.caret_utf16;
  snapshot.composition_active = view_.composition_active;
  snapshot.composition_start_utf16 = view_.composition_start_utf16;
  snapshot.composition_length_utf16 =
      static_cast<LONG>(view_.composition_text_utf16.size());
  snapshot.works_accepted = works_accepted_;
  snapshot.raw_commits_begun = raw_commits_begun_;
  snapshot.raw_commits_applied = raw_commits_applied_;
  snapshot.raw_commits_failed = raw_commits_failed_;
  snapshot.raw_units_applied = raw_units_applied_;
  snapshot.raw_units_refused = raw_units_refused_;
  snapshot.engine_results_taken = engine_results_taken_;
  snapshot.plans_submitted = plans_submitted_;
  snapshot.edits_applied = edits_applied_;
  snapshot.edits_refused = edits_refused_;
  snapshot.edits_stale = edits_stale_;
  snapshot.permission_queries_begun = permission_queries_begun_;
  snapshot.permission_queries_completed = permission_queries_completed_;
  snapshot.permission_rechecks = 0;
  if (session_ != nullptr) {
    const edit::CompositionEditSessionDiagnostics session = session_->Snapshot();
    snapshot.permission_rechecks = session.permission_rechecks;
    snapshot.target_prechecks = session.target_prechecks;
    snapshot.target_callback_checks = session.target_callback_checks;
    snapshot.target_rejections = session.target_rejections;
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
