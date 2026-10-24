// EpoCanva P3.8 -- the context-binding coordinator.
//
// WHAT THIS OBJECT IS. A small, deliberately single-purpose coordinator that connects one
// bound TSF context to one engine client through the existing value-command executor
// (P3.5) and the existing composition edit session (P3.7). It runs ENTIRELY on the STA
// thread that created it. ITfContext pointers, edit sessions and composition objects never
// leave that thread; the engine is reached only through EngineExecutor's typed commands
// and value results, never through a Runtime/Session/Controller pointer.
//
// WHAT IT SUPPORTS, AND ONLY THIS (brief section 2):
//   * one current binding (ITfContext + identity/generation + engine ClientId + permission);
//   * one in-flight work item -- a single key dispatch or a single candidate acceptance;
//   * controlled input submitted explicitly by the host;
//   * explicit permission (Allowed/Denied/Unknown, host-supplied);
//   * explicit binding invalidation with generation bump;
//   * real engine results landing in the real P3.7 text store through real edit sessions.
// There is no multi-request pipeline, no message bus, no key sink, no candidate window, no
// registration, and no write into an external application.
//
// SINGLE FLIGHT. Submit while a work item is unresolved returns kBusy; nothing is queued
// behind the coordinator's back. A work item is resolved exactly once from the engine
// result (taken once) and at most one edit plan is submitted for it. Accepting a work
// means neither that input reached the engine nor that text was applied, and promises
// nothing about a future pfEaten.
//
// GATES, IN DOCUMENTED ORDER (Submit): closed -> busy -> not-bound -> blocked latch
// (uncertain / needs-resync / unsupported shape) -> permission (Denied, then Unknown) ->
// local engine-candidate evaluation -> executor admission. A Busy answer outranks a
// permission answer because the single-flight rule is a fact about the coordinator itself.
//
// LIFECYCLE OF ONE WORK ITEM.
//   Idle --Submit--> EnginePending --Pump takes result--> (convert) --> EditPending
//        --callback completes--> terminal. Terminals: Applied (edit really succeeded),
//   NoEdit (nothing to change), Rejected (a gate refused; no edit or a clean refusal),
//   NeedsResync (engine dispatched but its observation is unusable; no retry, no replay),
//   Uncertain (a TSF write could not be confirmed; the coordinator locks down),
//   UnsupportedShape (commit + preedit in one observation; the coordinator locks down),
//   AbandonedStale (invalidated or closed while pending; the result is discarded).
//
// TF_S_ASYNC IS PENDING, NOT APPLIED. An async edit session result stays EditPending until
// DoEditSession really ran; only a callback-confirmed Applied makes the work Applied.
// Close/invalidation do not assume DoEditSession will ever be called: a pending work item
// is given its AbandonedStale terminal immediately, cleanup is not written inside the
// callback, and a never-run callback cannot make the coordinator wait forever.
//
// THE UNCERTAIN LOCKDOWN. When P3.7 reports FailedPossiblyMutated or FailedAfterMutation
// the coordinator refuses all further normal input for that context, and re-binding the
// SAME context cannot bypass the lockdown (Bind returns kBlockedUncertain). A different
// context is a different document and may be bound. Only Close() clears the block.
// NeedsResync and UnsupportedShape blocks are cleared by an explicit invalidation, because
// there the document text is known; the engine client's state is the host's to rebuild.
//
// PERMISSION. Unknown and Denied never dispatch to the engine and never request an edit,
// at Submit and again when an engine result is about to be converted. A permission change
// while a work item is queued rejects that work item; a change while an edit is pending is
// caught again inside the P3.7 callback. This is an explicit host gate, NOT password-field
// detection, and nothing here reads the document to classify a field.
//
// PUMPING. Pump() is non-blocking: it takes an engine result if one is ready (TakeResult,
// never WaitForResult), converts it, submits the edit, and harvests a completed edit
// result if the completion event is already signalled. It never waits for the engine, never
// waits for a TSF write lock and never dispatches STA messages. The host owns bounded
// waiting and the message loop (P3.7 TestHost::PumpUntil / EngineExecutor::WaitForResult)
// and drives completion from real state, not from sleeps.
//
// PRIVACY. No network I/O exists anywhere in this path. Diagnostics and snapshots carry
// statuses, generations and counters only -- never key text, preedit, commit text or field
// identifiers. The engine client is created and owned by the host process, not here.
#ifndef EPOCANVA_WINDOWS_TSF_COORDINATOR_COORDINATOR_H_
#define EPOCANVA_WINDOWS_TSF_COORDINATOR_COORDINATOR_H_

#include <windows.h>
#include <msctf.h>

#include <cstdint>
#include <memory>
#include <optional>

#include "edit_plan.h"
#include "edit_result.h"
#include "edit_session.h"
#include "engine_executor.h"
#include "engine_to_plan.h"
#include "key_decision.h"
#include "key_event.h"

namespace epocanva {
namespace tsf {
namespace coordinator {

// The P3.4/P3.5 engine value types and the P3.3 input types this header speaks, under
// short names that resolve from inside this namespace.
namespace engine = ::epocanva::windows::engine;
namespace input = ::epocanva::windows::input;

// Opaque id of one accepted work item. Never reused; 0 is invalid.
struct WorkId {
  std::uint64_t value = 0;
  bool valid() const { return value != 0; }
};

enum class BindStatus : std::uint8_t {
  kOk = 0,
  kAlreadyBound,     // the current binding must be invalidated first
  kBusy,             // an engine result of a previous work item is still being discarded
  kClosed,
  kBlockedUncertain, // this context is locked down after an unconfirmed write
  kInvalidRequest,   // null context, zero context id, invalid client or negative caret
};

enum class SubmitStatus : std::uint8_t {
  kAccepted = 0,
  kBusy,
  kNotBound,
  kClosed,
  kPermissionDenied,
  kPermissionUnknown,
  kBlockedUncertain,
  kBlockedNeedsResync,
  kBlockedUnsupportedShape,
  kNotEngineCandidate,     // the local pure evaluation says this is not an engine key
  kEngineNotRunning,       // mapped from the executor's kNotRunning
  kQueueFull,              // mapped from the executor's kQueueFull
  kEngineClientUnavailable,// the executor no longer knows/accepts this engine client
  kEngineSubmitFailed,     // the executor could not enqueue (internal failure)
  kInvalidRequest,         // null out id
};

enum class WorkKind : std::uint8_t { kNone = 0, kKey, kCandidate };

enum class WorkPhase : std::uint8_t {
  kIdle = 0,
  // A dispatch/candidate command is accepted by the executor and its result has not been
  // taken. Still EnginePending when the result exists but was not consumed yet.
  kEnginePending,
  // An edit plan was submitted through the P3.7 session and its callback has not been
  // observed complete. TF_S_ASYNC is exactly this state, never Applied.
  kEditPending,
};

enum class WorkOutcome : std::uint8_t {
  kNone = 0,
  kApplied,          // the edit ran and every step, including the write, succeeded
  kNoEdit,           // the engine consumed the event; the document already matches
  kRejected,         // a gate or the edit refused; see `cause` / last-edit fields
  kNeedsResync,      // engine dispatched, observation unusable; no retry, no replay
  kUncertain,        // a TSF write could not be confirmed; this context locks down
  kUnsupportedShape, // commit and preedit in one observation; this context locks down
  kAbandonedStale,   // invalidated/closed while pending; the old result is never applied
};

enum class CoordinatorGate : std::uint8_t {
  kOpen = 0,
  kBlockedUncertain,
  kBlockedNeedsResync,
  kBlockedUnsupportedShape,
};

// Why a work item ended kRejected. The P3.7 EditStatus/EditRejectReason of the edit side
// are carried separately in the snapshot; this enumerates only the coordinator's own
// refusals.
enum class RejectCause : std::uint8_t {
  kNone = 0,
  kPermissionChangedWhileQueued, // allowed at dispatch time, not allowed at apply time
  kEngineRefused,                // the engine command reported a clean refusal
  kEngineUnavailable,            // the engine command reported the client gone
  kEditRefused,                  // the P3.7 session refused (read-only, lock, stale text..)
  kEditFailedBeforeMutation,     // a TSF call failed before any write was issued
  kTakeFailed,                   // the result store answered with an unexpected status
};

const char* DescribeBindStatus(BindStatus status);
const char* DescribeSubmitStatus(SubmitStatus status);
const char* DescribeWorkKind(WorkKind kind);
const char* DescribeWorkPhase(WorkPhase phase);
const char* DescribeWorkOutcome(WorkOutcome outcome);
const char* DescribeCoordinatorGate(CoordinatorGate gate);
const char* DescribeRejectCause(RejectCause cause);

// Value-only state for assertions and text-free logging.
struct CoordinatorSnapshot {
  bool bound = false;
  bool closed = false;
  CoordinatorGate gate = CoordinatorGate::kOpen;
  edit::ContextIdentity identity{};
  edit::InputPermission permission = edit::InputPermission::kUnknown;
  WorkKind work_kind = WorkKind::kNone;
  WorkPhase phase = WorkPhase::kIdle;
  WorkOutcome outcome = WorkOutcome::kNone;
  RejectCause cause = RejectCause::kNone;
  std::uint64_t work_id = 0;
  // The binding generation the in-flight (or last resolved) work item belongs to.
  std::uint64_t work_generation = 0;
  // Valid while phase == kEnginePending; the host may boundedly WaitForResult on it.
  engine::RequestId engine_request{};
  // Detail of the resolved edit (default status is the P3.7 "unrejected" sentinel).
  edit::EditStatus last_edit_status = edit::EditStatus::kRejected;
  edit::EditRejectReason last_edit_reason = edit::EditRejectReason::kNone;
  // The coordinator's document mirror. UTF-16 code units / code-unit offsets.
  LONG tracked_caret_utf16 = 0;
  bool composition_active = false;
  LONG composition_start_utf16 = -1;
  LONG composition_length_utf16 = 0;
  // Counters.
  std::uint64_t works_accepted = 0;
  std::uint64_t engine_results_taken = 0;
  std::uint64_t plans_submitted = 0;
  std::uint64_t edits_applied = 0;
  std::uint64_t edits_refused = 0;
  std::uint64_t edits_stale = 0;
  std::uint64_t pumps = 0;
};

class ContextCoordinator {
 public:
  // `executor` must outlive this object (the coordinator never stops or joins it). All
  // methods, including Close() and the destructor, run on this STA thread.
  ContextCoordinator(engine::EngineExecutor& executor, TfClientId tsf_client_id);
  ~ContextCoordinator();

  ContextCoordinator(const ContextCoordinator&) = delete;
  ContextCoordinator& operator=(const ContextCoordinator&) = delete;

  // --- binding ---------------------------------------------------------------------------
  // Binds one context. `initial_caret_utf16` is the host-asserted collapsed caret in
  // UTF-16 code units at bind time; the coordinator never reads the document, so its caret
  // mirror starts from this value and is then maintained from applied edits only.
  // Permission starts Unknown (the safe refusal) for every new binding.
  BindStatus Bind(ITfContext* context, std::uint64_t context_id,
                  LONG initial_caret_utf16, engine::ClientId engine_client);

  // Invalidates the current binding: the P3.7 session is invalidated (its generation
  // bumps, pending plans of the old binding become Stale) and released. A pending work
  // item is terminal immediately as AbandonedStale; an engine result that arrives later is
  // consumed once and discarded, never applied to any context. Clears the
  // NeedsResync/UnsupportedShape blocks (the document text is known); the Uncertain block
  // stays with its context id. Idempotent when not bound.
  void InvalidateBinding();

  bool bound() const { return bound_; }
  edit::ContextIdentity identity() const { return identity_; }

  // --- permission ------------------------------------------------------------------------
  // The explicit host gate. Applied to the current P3.7 session as well, so an edit that
  // is already pending is refused inside its callback if the permission changed.
  void SetPermission(edit::InputPermission permission);
  edit::InputPermission permission() const { return permission_; }

  // --- evaluation (pure) -----------------------------------------------------------------
  // The local P3.3 decision over the coordinator's own context facts. Submits nothing to
  // the engine, requests no edit, changes no state. DecideKey's own permission field is
  // fed from the coordinator's permission, so a Denied/Unknown field answers
  // pass-through here too.
  input::KeyDecision Evaluate(const input::KeyEventSnapshot& event,
                              const input::KeyText& text) const;

  // --- one input work --------------------------------------------------------------------
  SubmitStatus SubmitKey(const input::KeyEventSnapshot& event, const input::KeyText& text,
                         WorkId* id);
  SubmitStatus SubmitCandidate(std::size_t candidate_index, WorkId* id);

  // --- non-blocking advance --------------------------------------------------------------
  // Takes a ready engine result (once), converts it, submits the edit, and harvests an
  // edit result whose completion event is already signalled. Never waits, never pumps
  // messages, never blocks on the engine or a TSF lock.
  void Pump();

  // The P3.7 session's completion event while a work item is in EditPending; the host
  // bounded-waits on it and dispatches STA messages. Invalid handle when not pending.
  HANDLE EditCompletionEvent() const;

  CoordinatorSnapshot Snapshot() const;

  // --- teardown --------------------------------------------------------------------------
  // Terminal. Pending work becomes AbandonedStale, a ready-but-untaken engine result is
  // consumed and discarded (an unready one is left for the executor's own stop drain --
  // documented, not hidden), the session is invalidated, closed and destroyed. Never
  // issues a text operation: cleanup never commits or cancels unknown text. Idempotent.
  void Close();

 private:
  bool GateBlocksSubmit(SubmitStatus* status) const;
  SubmitStatus FinishSubmit(engine::RequestId request, WorkKind kind);
  void ResolveEngineResult(const engine::CommandResult& result);
  void ConvertAndSubmitEdit(const EngineObservation& observation);
  void SubmitEditPlan(const PlanDecision& decision);
  void ResolveEditResult(const edit::EditResult& result);
  void ApplyViewUpdate(const edit::EditPlan& plan);
  void EndWork(WorkOutcome outcome, RejectCause cause);
  void EnterNeedsResync(const char* why);
  void EnterUncertainLockdown();
  void DiscardPendingEngineResult();

  engine::EngineExecutor& executor_;
  TfClientId tsf_client_id_;

  bool bound_ = false;
  bool closed_ = false;
  std::uint64_t generation_counter_ = 0;
  edit::ContextIdentity identity_{};
  engine::ClientId engine_client_{};
  edit::InputPermission permission_ = edit::InputPermission::kUnknown;
  std::unique_ptr<edit::CompositionEditSession> session_;
  // 0 = no context is blocked. Set only by the uncertain lockdown; survives
  // InvalidateBinding and is cleared only by Close().
  std::uint64_t uncertain_blocked_context_id_ = 0;

  WorkKind work_kind_ = WorkKind::kNone;
  WorkPhase phase_ = WorkPhase::kIdle;
  WorkOutcome outcome_ = WorkOutcome::kNone;
  RejectCause cause_ = RejectCause::kNone;
  std::uint64_t work_id_ = 0;
  std::uint64_t next_work_id_ = 1;
  std::uint64_t work_generation_ = 0;
  engine::RequestId engine_request_{};
  bool work_abandoned_ = false;
  // The plan of the edit currently pending (EditPlan's constructors are private to its
  // factory, so the slot is an optional rather than a pre-built value).
  std::optional<edit::EditPlan> pending_plan_;
  edit::EditStatus last_edit_status_ = edit::EditStatus::kRejected;
  edit::EditRejectReason last_edit_reason_ = edit::EditRejectReason::kNone;

  LocalDocumentView view_;

  std::uint64_t works_accepted_ = 0;
  std::uint64_t engine_results_taken_ = 0;
  std::uint64_t plans_submitted_ = 0;
  std::uint64_t edits_applied_ = 0;
  std::uint64_t edits_refused_ = 0;
  std::uint64_t edits_stale_ = 0;
  std::uint64_t pumps_ = 0;
};

}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_COORDINATOR_COORDINATOR_H_
