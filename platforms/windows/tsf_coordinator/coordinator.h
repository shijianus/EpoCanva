// EpoCanva P3.8/P3.9/P3.13 -- the context-binding coordinator.
//
// WHAT THIS OBJECT IS. A small, deliberately single-purpose coordinator that connects one
// bound TSF context to one engine client through the existing value-command executor
// (P3.5) and the existing composition edit session (P3.7). It runs ENTIRELY on the STA
// thread that created it. ITfContext pointers, edit sessions, permission queries and
// composition objects never leave that thread; the engine is reached only through
// EngineExecutor's typed commands and value results, never through a
// Runtime/Session/Controller pointer.
//
// WHAT IT SUPPORTS, AND ONLY THIS:
//   * one current binding (ITfContext + identity/generation + engine ClientId);
//   * one in-flight work item -- a single key dispatch or a single candidate acceptance;
//   * controlled input submitted explicitly by the host;
//   * permission resolved from REAL context metadata through an injected PermissionSource
//     (P3.9); the coordinator itself has no manual permission setter -- the explicit
//     Allowed/Denied knob exists only as a test-owned implementation of that interface;
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
// GATES, IN DOCUMENTED ORDER (Submit): closed -> busy -> not-bound -> retiring (a previous
// binding still owes a result/close/cleanup) -> blocked latch (uncertain / needs-resync /
// unsupported shape) -> live target (P3.13) -> permission (Denied, then Unknown, resolved
// from the real context metadata; a query that must go async parks the work in
// kPermissionPending instead of refusing) -> local engine-candidate evaluation -> executor
// admission. A Busy answer outranks a permission answer because the single-flight rule is a
// fact about the coordinator itself.
//
// TARGET REVALIDATION (P3.13). An attached TargetRevalidator (the ContextObserver in the
// product path) is asked for thread focus, current GetFocus/GetTop canonical identity, and
// the registry generation before ordinary input, after the permission answer, after an
// engine result and immediately before an edit plan. The P3.7 session repeats the live
// check before requesting a lock and inside the granted callback. A refusal before the
// engine is TargetChanged/ThreadFocusLost/TargetUnverified and creates no work; a move
// after the engine ran is NeedsResync/kTargetChangedWhileQueued. No refusal is replayed.
// EndOwnedComposition cleanup is exempt from the target check and keeps every P3.12
// ownership, range, permission, read-only and Uncertain rule.
//
// RETIREMENT IS BOUNDED AND EXPLICIT. InvalidateBinding moves the old binding into a
// fixed-capacity ledger (kRetirementCapacity). The old client is closed through the executor
// and its close result is consumed; an engine request accepted before retirement is taken
// once and discarded. A new binding is admitted only when the ledger has room, and input on
// a new binding is kRetiring until every retirement is terminal. If a cleanup cannot be
// performed (permission, read-only, range mismatch, uncertain text), it is reported as
// CleanupBlocked -- never as Completed -- and nothing is written to guess.
//
// LIFECYCLE OF ONE WORK ITEM.
//   Idle --Submit--> PermissionPending (only when the real query went async)
//        --verdict Allowed--> EnginePending --Pump takes result--> (convert, and the
//   apply-time permission re-verification, which itself may pass through
//   PermissionPending once more) --> EditPending --callback completes--> terminal.
//   Terminals: Applied (edit really succeeded), NoEdit (nothing to change), Rejected
//   (a gate refused BEFORE the engine ran: a pending query answered Denied/Unknown),
//   NeedsResync (the engine already ran but its observation is unusable, or its edit
//   could not be applied -- including a permission change, read-only, a refused lock or
//   a disconnected context; no retry, no replay), Uncertain (a TSF write could not be
//   confirmed; the coordinator locks down), UnsupportedShape (commit + preedit in one
//   observation; the coordinator locks down), AbandonedStale (invalidated or closed
//   while pending; the result is discarded).
//
// ENGINE/HOST DIVERGENCE (P3.9). Once the engine has consumed an event, every outcome
// short of a confirmed Applied or a NoEdit (document already matches) is NeedsResync or
// worse: the engine's internal state and the document may have diverged, so the binding
// is not quietly returned to a typeable state. An explicit InvalidateBinding (and, for
// the engine side, a rebuilt engine client owned by the host) is the only way on. A
// Rejected terminal therefore exists only for refusals that precede any engine call.
//
// TF_S_ASYNC IS PENDING, NOT APPLIED. An async edit session result stays EditPending until
// DoEditSession really ran; only a callback-confirmed Applied makes the work Applied. A
// permission query behaves the same way: kPermissionPending means the real read session is
// still queued, and no engine call and no edit exists for that work item yet. Close or
// invalidation do not assume any callback will ever be called: pending work is given its
// AbandonedStale terminal immediately, the query is abandoned, and cleanup is never
// written inside a callback.
//
// THE UNCERTAIN LOCKDOWN. When P3.7 reports FailedPossiblyMutated or FailedAfterMutation
// the coordinator refuses all further normal input for that context, and re-binding the
// SAME context cannot bypass the lockdown (Bind returns kBlockedUncertain). A different
// context is a different document and may be bound. Only Close() clears the block.
// NeedsResync and UnsupportedShape blocks are cleared by an explicit invalidation, because
// there the document text is known; the engine client's state is the host's to rebuild.
//
// PERMISSION, FROM REAL METADATA. Every work item resolves the permission from the bound
// context's real metadata (GetStatus / KEYBOARD_DISABLED / EMPTYCONTEXT compartments /
// the InputScope application property) through the injected PermissionSource, once before
// the engine dispatch and once more before the edit is built; a verdict is re-validated
// against the binding identity and generation at every use. Unknown and Denied never
// dispatch to the engine and never request an edit. A non-Allowed verdict AFTER the
// engine ran is NeedsResync, not a silent drop. The P3.7 session's own permission gate is
// kept in step with each verdict as defense in depth, and its callback re-reads the real
// metadata with its granted edit cookie immediately before any write (a permission change
// while a lock is deferred still refuses the write). This is context metadata, not
// content inspection: nothing here reads the document text to classify a field.
//
// PUMPING. Pump() is non-blocking: it advances a pending permission query when the real
// read session completed, takes an engine result if one is ready (TakeResult, never
// WaitForResult), converts it, submits the edit, and harvests a completed edit result if
// the completion event is already signalled. It never waits for the engine, never waits
// for a TSF write lock and never dispatches STA messages. The host owns bounded waiting
// and the message loop (P3.7 TestHost::PumpUntil / EngineExecutor::WaitForResult) and
// drives completion from real state, not from sleeps.
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
#include <vector>

#include "candidate_snapshot.h"
#include "context_registry.h"
#include "edit_plan.h"
#include "edit_result.h"
#include "edit_session.h"
#include "engine_executor.h"
#include "engine_to_plan.h"
#include "key_decision.h"
#include "key_event.h"
#include "permission_query.h"
#include "target_revalidation.h"

namespace epocanva {
namespace tsf {
namespace coordinator {

// The P3.4/P3.5 engine value types, the P3.3 input types and the P3.9 context types this
// header speaks, under short names that resolve from inside this namespace.
namespace engine = ::epocanva::windows::engine;
namespace input = ::epocanva::windows::input;
namespace context = ::epocanva::tsf::context;

// Opaque id of one accepted work item. Never reused; 0 is invalid.
struct WorkId {
  std::uint64_t value = 0;
  bool valid() const { return value != 0; }
};

enum class BindStatus : std::uint8_t {
  kOk = 0,
  kAlreadyBound,     // the current binding must be invalidated first
  kBusy,             // an engine result of a previous work item is still being discarded
  kRetiring,         // the bounded retirement ledger is full; no client may be created
  kClosed,
  kBlockedUncertain, // this context is locked down after an unconfirmed write
  kInvalidRequest,   // null context, zero context id, invalid client or negative caret
  kTargetUnverified, // P3.13: no real target authority is attached
};

enum class SubmitStatus : std::uint8_t {
  kAccepted = 0,
  kBusy,
  kNotBound,
  kRetiring,               // a retired binding's cleanup/close is not terminal yet
  kClosed,
  kPermissionDenied,
  kPermissionUnknown,
  kBlockedUncertain,
  kBlockedNeedsResync,
  kBlockedUnsupportedShape,
  // P3.13 target refusals. These are distinct from permission and from Busy/Retiring:
  //   kTargetChanged      the TSF focus/top context is no longer this binding, or the
  //                       registry generation moved on
  //   kThreadFocusLost    the thread lost TSF/Win32 input focus (or OnKillThreadFocus ran)
  //   kTargetUnverified   no live target authority answered; conservative refusal
  kTargetChanged,
  kThreadFocusLost,
  kTargetUnverified,
  kNotEngineCandidate,     // the local pure evaluation says this is not an engine key
  kCandidateStale,         // a UI selection no longer matches the engine's current page
  kEngineNotRunning,       // mapped from the executor's kNotRunning
  kQueueFull,              // mapped from the executor's kQueueFull
  kEngineClientUnavailable,// the executor no longer knows/accepts this engine client
  kEngineSubmitFailed,     // the executor could not enqueue (internal failure)
  kInvalidRequest,         // null out id
};

// kRaw is the W2-A-R degraded path: an ordered run of raw UTF-16 text is committed
// directly into the bound document (replacing the owned composition when one exists)
// without any engine call, because the engine backlog could not be serviced in time.
// It exists only for text the host already ate; it never replays a refused key.
enum class WorkKind : std::uint8_t {
  kNone = 0,
  kKey,
  kCandidate,
  kRaw,
  // Host-owned text: an explicit, versioned candidate selection that replaces the token
  // immediately before the caret. No engine command is involved.
  kSuggestionReplace,
};

enum class WorkPhase : std::uint8_t {
  kIdle = 0,
  // The real permission query for this work item went async; no engine call and no edit
  // exist for it yet. The verdict decides: dispatch (or apply) or reject.
  kPermissionPending,
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
  kRejected,         // a gate refused BEFORE any engine call (pending-query verdict)
  kNeedsResync,      // engine dispatched, but its result or edit is unusable/unapplied
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

// State of one retired binding's TSF-side cleanup. kPending means a cleanup edit session
// exists and the new binding's input admission is paused. kFinished means THIS cleanup's own
// EndComposition call was issued and succeeded. kAlreadyOrdinary means there was no own
// composition to end: none was ever started, or the writer ended it normally before
// retirement. kTerminatedExternally means an own composition was terminated by another party
// (TSF or the application) and this cleanup issued no end call. kBlocked is a reported
// failure. Only kFinished is a cleanup success.
enum class CleanupState : std::uint8_t {
  kNone = 0,
  kPending,
  kFinished,
  kAlreadyOrdinary,
  kTerminatedExternally,
  kBlocked,
};

// Why a retired binding's composition could not be finished. These name only the refusal;
// no document text or field identity is carried.
enum class CleanupBlockReason : std::uint8_t {
  kNone = 0,
  kPermissionDenied,
  kPermissionUnknown,
  kReadOnly,
  kContextDisconnected,
  kUncertainTextState,
  kRangeMismatch,
  kLockRefused,
  kEndCompositionFailed,
  kStale,
  kPlanBuildFailed,
  kRequestFailed,
  kShutdown,  // terminal Close could not wait for a legal cleanup edit session
};

const char* DescribeCleanupState(CleanupState state);
const char* DescribeCleanupBlockReason(CleanupBlockReason reason);

// Why a work item ended kRejected or kNeedsResync. The P3.7 EditStatus/EditRejectReason
// of the edit side are carried separately in the snapshot; this enumerates only the
// coordinator's own refusals.
enum class RejectCause : std::uint8_t {
  kNone = 0,
  kPermissionDenied,             // a pending permission query answered Denied
  kPermissionUnknown,            // a pending permission query answered Unknown
  kPermissionChangedWhileQueued, // allowed at dispatch time, not allowed at apply time
  kEngineRefused,                // the engine command reported a clean refusal
  kEngineUnavailable,            // the engine command reported the client gone
  kEngineSubmitFailed,           // the executor refused the enqueue at dispatch time
  kEditRefused,                  // the P3.7 session refused (read-only, lock, stale text..)
  kEditFailedBeforeMutation,     // a TSF call failed before any write was issued
  kTakeFailed,                   // the result store answered with an unexpected status
  kTargetChangedWhileQueued,     // P3.13: the real target moved while this work was queued
};

const char* DescribeBindStatus(BindStatus status);
const char* DescribeSubmitStatus(SubmitStatus status);
// The refusal category a UI must show instead of a bare failure: focus, target, permission,
// range/request, queue, engine-not-ready, uncertain, retiring, or busy.
const char* DescribeSubmitStage(SubmitStatus status);
const char* DescribeWorkKind(WorkKind kind);
const char* DescribeWorkPhase(WorkPhase phase);
const char* DescribeWorkOutcome(WorkOutcome outcome);
const char* DescribeCoordinatorGate(CoordinatorGate gate);
const char* DescribeRejectCause(RejectCause cause);

// Value-only state of the bounded retirement ledger. It carries statuses, counts and the
// outstanding request ids a host may bounded-wait on -- never text or a COM pointer.
struct RetirementSnapshot {
  std::size_t retiring_bindings = 0;
  std::size_t capacity = 0;
  // True while any retired binding is still Pending; new input is refused as kRetiring.
  bool admission_paused = false;
  std::uint32_t retiring_compositions = 0;
  std::uint64_t started = 0;
  // Cleanup attempts whose attribution is terminal. This is the number of SETTLED cleanup
  // attempts, never "all succeeded": it is exactly finished + already_ordinary +
  // externally_terminated + blocked, including every blocked attempt. A record whose
  // cleanup is settled may still be retiring until its close result is consumed.
  std::uint64_t settled = 0;
  std::uint64_t finished = 0;
  std::uint64_t already_ordinary = 0;
  std::uint64_t blocked = 0;
  // Cleanups where TSF reported the composition terminated by another party and this
  // coordinator issued no end call of its own. Counted separately from Finished.
  std::uint64_t externally_terminated = 0;
  // EndComposition calls the cleanup itself issued, and how many failed.
  std::uint64_t cleanup_end_calls = 0;
  std::uint64_t cleanup_end_failures = 0;
  // Per-operation evidence of the most recently settled cleanup, text-free: the ledger
  // sequence and context identity of the retired binding, what the record owned when
  // cleanup started, and whether that plan's own EndComposition call was issued and how it
  // answered. `plan_session_id` is 0 when no edit session was requested.
  std::uint64_t last_cleanup_sequence = 0;
  std::uint64_t last_cleanup_context_id = 0;
  std::uint64_t last_cleanup_generation = 0;
  bool last_cleanup_owned_at_start = false;          // the writer had owned a composition
  bool last_cleanup_live_at_start = false;           // that composition was still live
  bool last_cleanup_external_at_start = false;       // TSF had already reported its termination
  bool last_cleanup_ended_before_at_start = false;   // the writer had ended it successfully
  std::uint64_t last_cleanup_termination_notifications_at_start = 0;
  bool last_cleanup_plan_submitted = false;
  std::uint64_t last_cleanup_plan_session_id = 0;
  bool last_cleanup_end_issued = false;
  HRESULT last_cleanup_end_hr = S_OK;
  std::uint64_t last_cleanup_termination_notifications_after_call = 0;
  // Termination notifications observed by the settling operation (or by the final
  // composition-state check when no cleanup plan was submitted). An increase over at_start
  // is the direct notification evidence for an external termination that arrived while the
  // retirement was still waiting for an older plan's terminal.
  std::uint64_t last_cleanup_termination_notifications_at_settle = 0;
  std::uint64_t close_requests = 0;
  std::uint64_t close_results_taken = 0;
  std::uint64_t close_failures = 0;
  std::uint64_t close_queue_full_retries = 0;
  std::uint64_t engine_results_discarded = 0;
  std::uint64_t unresolved_close_requests = 0;
  std::uint64_t unresolved_engine_results = 0;
  CleanupState last_cleanup_state = CleanupState::kNone;
  CleanupBlockReason last_cleanup_block = CleanupBlockReason::kNone;
  // The first outstanding request of any retiring binding, for bounded host waiting.
  engine::RequestId pending_engine_request{};
  engine::RequestId pending_close_request{};
  // A cleanup edit session's completion fact, when one is waiting for its lock.
  HANDLE pending_edit_event = nullptr;
};

// Shared accounting contract for the retirement snapshot: every settled cleanup has exactly
// one terminal attribution. The coordinator test and the lab self-test both assert this, so
// "settled" can never be read as "all cleanups succeeded".
bool RetirementAccountingBalanced(const RetirementSnapshot& snapshot);

// Shared attribution contract for the most recently settled cleanup. It accepts an outcome
// only when the recorded operation evidence supports it:
//   Finished              -> plan submitted, this plan's EndComposition issued and succeeded,
//                            starting from a live composition the writer had not ended;
//   AlreadyOrdinary       -> no end call, and no owned composition or one already ended;
//   TerminatedExternally  -> no end call, an owned composition, and termination-notification
//                            evidence: TSF had already reported it before cleanup, or the
//                            notification count rose while cleanup waited (including while
//                            an older accepted plan still owed its Stale terminal, before
//                            any cleanup plan could be submitted);
//   Blocked               -> a named reason, and no successful end claimed.
// Tests and the lab use this instead of accepting either of two terminal states blindly.
bool RetirementCleanupEvidenceConsistent(const RetirementSnapshot& snapshot);

// Value-only state for assertions and text-free logging.
struct CoordinatorSnapshot {
  bool bound = false;
  bool closed = false;
  CoordinatorGate gate = CoordinatorGate::kOpen;
  edit::ContextIdentity identity{};
  // The last verdict the real source produced for THIS binding (Unknown until one
  // completes; Evaluate and the session gate read this cache, never a live query).
  edit::InputPermission permission = edit::InputPermission::kUnknown;
  WorkKind work_kind = WorkKind::kNone;
  WorkPhase phase = WorkPhase::kIdle;
  WorkOutcome outcome = WorkOutcome::kNone;
  RejectCause cause = RejectCause::kNone;
  std::uint64_t work_id = 0;
  // P3.13: the result of the last SubmitKey/SubmitCandidate call and the live target
  // revalidation that decided (or accompanied) it. Text-free.
  SubmitStatus last_submit_status = SubmitStatus::kAccepted;
  std::uint64_t submit_refusals = 0;
  context::TargetRevalidation last_target_revalidation{};
  std::uint64_t target_revalidations = 0;
  std::uint64_t target_refusals = 0;
  // The P3.7 session's own target checks (pre-lock and inside the granted callback).
  std::uint64_t target_prechecks = 0;
  std::uint64_t target_callback_checks = 0;
  std::uint64_t target_rejections = 0;
  // The binding generation the in-flight (or last resolved) work item belongs to.
  std::uint64_t work_generation = 0;
  // Valid while phase == kEnginePending; the host may boundedly WaitForResult on it.
  engine::RequestId engine_request{};
  // Detail of the resolved edit (default status is the P3.7 "unrejected" sentinel).
  edit::EditStatus last_edit_status = edit::EditStatus::kRejected;
  edit::EditRejectReason last_edit_reason = edit::EditRejectReason::kNone;
  HRESULT last_edit_request_hr = S_OK;
  HRESULT last_edit_session_hr = S_OK;
  HRESULT last_edit_operation_hr = S_OK;
  // The live selection the last edit operation read (ACP, UTF-16 code units), or -1.
  LONG last_edit_observed_caret_utf16 = -1;
  // The coordinator's document mirror. UTF-16 code units / code-unit offsets.
  LONG tracked_caret_utf16 = 0;
  bool composition_active = false;
  // Test diagnostics: the P3.7 session still owes an edit result, and whether its
  // completion event is already signalled.
  bool session_pending = false;
  bool edit_completion_signaled = false;
  std::uint64_t edit_pump_checks = 0;
  std::uint64_t edit_pump_ready = 0;
  LONG composition_start_utf16 = -1;
  LONG composition_length_utf16 = 0;
  // Counters.
  std::uint64_t works_accepted = 0;
  // W2-A-R raw degradation accounting. Units are UTF-16 code units of the text the host
  // captured while the engine path was backlogged; the counts are text-free.
  std::uint64_t raw_commits_begun = 0;
  std::uint64_t raw_commits_applied = 0;
  std::uint64_t raw_commits_failed = 0;
  std::uint64_t raw_units_applied = 0;
  std::uint64_t raw_units_refused = 0;
  LONG last_raw_composition_start = -1;
  std::uint64_t last_raw_composition_units = 0;
  std::uint64_t last_raw_input_units = 0;
  std::uint64_t last_raw_plan_units = 0;
  // UTF-16 units of the expected token in the last suggestion replacement. Used only to
  // size an unverifiable mutation, never to reproduce text.
  std::uint64_t last_suggestion_expected_units = 0;
  LONG last_suggestion_expected_caret_utf16 = -1;
  // W2-B candidate snapshot accounting. Text-free.
  std::uint64_t candidate_version = 0;
  std::uint64_t candidate_snapshots = 0;
  std::uint64_t candidate_items = 0;
  std::uint64_t candidate_stale_refusals = 0;
  std::uint64_t engine_results_taken = 0;
  std::uint64_t plans_submitted = 0;
  std::uint64_t edits_applied = 0;
  std::uint64_t edits_refused = 0;
  std::uint64_t edits_stale = 0;
  std::uint64_t permission_queries_begun = 0;
  std::uint64_t permission_queries_completed = 0;
  // Callback-time real-metadata rechecks the P3.7 session performed before writing.
  std::uint64_t permission_rechecks = 0;
  std::uint64_t pumps = 0;
  // P3.10: the registry's latch for the bound real context, and the real-caret
  // reconciliations accepted/applied from OnEndEdit.
  bool context_uncertain_blocked = false;
  std::uint64_t observed_caret_updates = 0;
  std::uint64_t observed_caret_applied = 0;
  // P3.12: whether the current binding's session really owns a live TSF composition
  // (writer authority, not the view mirror), and the retirement ledger.
  bool owned_composition_active = false;
  RetirementSnapshot retirement;
};

class ContextCoordinator {
 public:
  // `executor` and `permission_source` must outlive this object (the coordinator never
  // stops or joins the executor and never owns the source). All methods, including
  // Close() and the destructor, run on this STA thread.
  ContextCoordinator(engine::EngineExecutor& executor, TfClientId tsf_client_id,
                     context::PermissionSource& permission_source);
  ~ContextCoordinator();

  ContextCoordinator(const ContextCoordinator&) = delete;
  ContextCoordinator& operator=(const ContextCoordinator&) = delete;

  // --- binding ---------------------------------------------------------------------------
  // Binds one context. `initial_caret_utf16` is the host-asserted collapsed caret in
  // UTF-16 code units at bind time; the coordinator never reads the document, so its caret
  // mirror starts from this value and is then maintained from applied edits only. The
  // context is AddRef'd for the binding's duration. The permission cache starts Unknown
  // (the safe refusal) for every new binding; the first work item resolves it from real
  // metadata.
  //
  // This entry point exists for controlled unit cases. It assigns the generation itself;
  // the product integration path is BindObserved(), whose id and generation come from the
  // observer's ContextRegistry.
  BindStatus Bind(ITfContext* context, std::uint64_t context_id,
                  LONG initial_caret_utf16, engine::ClientId engine_client);

  // The real integration path (P3.10): `context_id` and `generation` are the values the
  // ContextRegistry assigned for this real context and binding attempt; `initial_caret_utf16`
  // is a selection offset (UTF-16 code units) the observer read with a real read-only
  // cookie. Rejects a zero generation. When a registry is attached, the uncertain latch is
  // checked against the REAL context pointer, so a caller-supplied integer cannot bypass it.
  BindStatus BindObserved(ITfContext* context, std::uint64_t context_id,
                          std::uint64_t generation, LONG initial_caret_utf16,
                          engine::ClientId engine_client);

  // Admission probe for the host's client source. It performs every structural and state
  // check BindObserved performs, but creates nothing. The host calls it BEFORE creating a
  // fresh engine client, so a full retirement ledger (kRetiring) can never leave an unused
  // client behind. A kOk answer is not a reservation; BindObserved must still be called on
  // the same STA thread and repeats the checks.
  BindStatus CheckBindingAdmission(ITfContext* context, std::uint64_t context_id,
                                   std::uint64_t generation,
                                   LONG initial_caret_utf16) const;

  // Attaches the registry that owns the real context identities/latch. Must be called
  // while unbound and before Close(); returns false (and changes nothing) otherwise. The
  // registry must outlive the coordinator.
  bool AttachContextRegistry(context::ContextRegistry* registry);

  // P3.13. Attaches the live target authority (the context observer in the product path).
  // Must be called while unbound and before Close(); returns false and changes nothing
  // otherwise. With no authority attached, ordinary input and ordinary BindObserved
  // admissions are refused as kTargetUnverified -- there is no "unchecked" mode for the
  // product path. A test-owned TargetRevalidator may be attached by controlled unit cases.
  bool AttachTargetRevalidator(context::TargetRevalidator* revalidator);

  // Reconciles the caret mirror with a real selection read (OnEndEdit's ecReadOnly,
  // collapsed, UTF-16 code units). Applied immediately when no work item is in flight and
  // at the work item's terminal otherwise; the plan-derived caret stays the mirror until
  // then. Negative values are ignored (no clamp). Only the current binding is updated; the
  // caller must have matched the binding identity/generation.
  void ObserveRealCaret(LONG caret_utf16);

  // Invalidates the current binding: it stops being typeable immediately, its pending work
  // item is terminal as AbandonedStale, and the binding is moved into the bounded retirement
  // ledger. Retirement closes the engine client through the executor (its result is taken
  // once) and, when the session owns a composition, finishes that composition through a
  // separate legal edit session driven by Pump(). No TSF write happens inside this call, so
  // it remains safe from OnTargetLost/OnEndEdit. Idempotent when not bound.
  void InvalidateBinding();

  bool bound() const { return bound_; }
  edit::ContextIdentity identity() const { return identity_; }

  // --- permission ------------------------------------------------------------------------
  // The last verdict the source produced for this binding. There is deliberately no
  // setter: real verdicts come from the source's metadata queries, and the explicit
  // Allowed/Denied knob exists only as a test-owned PermissionSource implementation.
  edit::InputPermission permission() const { return permission_; }

  // W2 product bootstrap: resolve the binding's permission before the first key is offered.
  // The real source's InputScope read may be synchronous or may park in kPermissionPending;
  // in the latter case Pump() advances it. This performs no engine call and no edit. It is
  // accepted only while idle, bound, not retiring and not gated; a completed refresh ends
  // as kNoEdit (the document was not touched).
  bool RefreshPermission();

  // --- evaluation (pure) -----------------------------------------------------------------
  // The local P3.3 decision over the coordinator's own context facts. Submits nothing to
  // the engine, requests no edit, queries no permission, changes no state. DecideKey's
  // own permission field is fed from the last cached verdict, so a Denied/Unknown field
  // answers pass-through here too.
  input::KeyDecision Evaluate(const input::KeyEventSnapshot& event,
                              const input::KeyText& text) const;

  // --- one input work --------------------------------------------------------------------
  SubmitStatus SubmitKey(const input::KeyEventSnapshot& event, const input::KeyText& text,
                         WorkId* id);
  SubmitStatus SubmitCandidate(std::size_t candidate_index, WorkId* id);

  // W2-B UI selection: the same engine command as SubmitCandidate, but only when the
  // request still names the CURRENT candidate snapshot (version), the CURRENT binding
  // identity/generation, and an index that exists on that page while the composition is
  // still active. An expired request is kCandidateStale and is never sent to the engine.
  SubmitStatus SubmitCandidateSelection(const CandidateSelectionRequest& request,
                                        WorkId* id);
  // Host-owned text mode: the engine only produces suggestions; the document already holds
  // the original text. A validated selection replaces the token that ends at the collapsed
  // caret with the candidate's text. The same version/identity/generation checks as above
  // apply, and the plan re-reads the range before writing: a stale menu refuses, it never
  // removes or rewrites text the user typed.
  SubmitStatus SubmitSuggestionReplacement(const CandidateSelectionRequest& request,
                                           WorkId* id);
  // When true, engine observations update the candidate snapshot but never produce document
  // edits: ordinary text belongs to the host. Product mode sets this before the first bind.
  void SetSuggestionMode(bool enabled);
  bool suggestion_mode() const { return suggestion_mode_; }
  // The current menu copy. The version increments on every engine observation.
  CandidateSnapshot candidate_snapshot() const { return candidate_snapshot_; }
  bool candidate_menu_open() const { return candidate_snapshot_.valid; }

  // W2-A-R degraded terminal: commit `raw_utf16` (UTF-16 code units) into the bound
  // document without an engine call. When the coordinator's writer owns a composition, the
  // plan replaces that composition with its current displayed text plus `raw_utf16` and
  // ends it; otherwise the text is inserted at the tracked caret. Admission runs the same
  // gates as a key (closed/busy/bound/retiring/gate/target/permission). The permission read
  // may park the work in kPermissionPending exactly like a key; no engine call is made.
  // The engine client's state is stale after an applied raw commit (it never saw the
  // bypassed keys), so the host must rebuild the binding before sending engine input again;
  // the snapshot counters under raw_* make that requirement observable.
  SubmitStatus SubmitRawCommit(std::u16string_view raw_utf16, WorkId* id);

  // --- non-blocking advance --------------------------------------------------------------
  // Advances a pending permission query whose real read session completed, takes a ready
  // engine result (once), converts it, submits the edit, and harvests an edit result
  // whose completion event is already signalled. It also advances the bounded retirement
  // ledger: consumes old engine/close results, waits for an abandoned plan's Stale terminal,
  // and submits the EndOwnedComposition cleanup outside every TSF notification. Never waits,
  // never pumps messages, never blocks on the engine or a TSF lock.
  void Pump();

  // The completion facts a pending work item is waiting on; the host bounded-waits on
  // them and dispatches STA messages. Null handles when nothing of that kind is pending.
  HANDLE EditCompletionEvent() const;
  HANDLE PermissionCompletionEvent() const;

  CoordinatorSnapshot Snapshot() const;

  // --- teardown --------------------------------------------------------------------------
  // Terminal. The current binding is moved into retirement, any ready engine/close result is
  // consumed, and every remaining retirement record is reported through the closed-retirement
  // snapshot (including a cleanup that could not be waited for, which is kBlocked/Shutdown,
  // never Completed). The session is released without commits, cancels or text clearing.
  // Idempotent. A host that wants a bounded, legal composition finish calls
  // InvalidateBinding() and pumps the retirement ledger to terminal BEFORE Close().
  void Close();

 private:
  // What a kPermissionPending work item is waiting to do once the verdict arrives.
  struct PendingPermissionWork {
    WorkKind kind = WorkKind::kNone;
    // kKey: the submitted key, held for its dispatch decision.
    input::KeyEventSnapshot event{};
    input::KeyText text{};
    // kRaw: the degraded raw run, held until its permission verdict arrives.
    std::u16string raw_text;
    // kSuggestionReplace: the expected token, the selected candidate text and the exact
    // caret ACP the menu was drawn at.
    std::u16string replace_expected;
    std::u16string replace_text;
    LONG replace_caret_utf16 = 0;
    // kCandidate: the accepted index.
    std::size_t candidate_index = 0;
    // The apply-time re-verification: the edit plan whose submission is held until the
    // verdict arrives. The plan was decided from the engine observation BEFORE the query,
    // so an unreadable observation is never masked by a later permission answer.
    bool has_plan = false;
    PlanDecision plan;
  };

  bool GateBlocksSubmit(SubmitStatus* status) const;
  // True while a retired binding has not reached a terminal retirement state.
  bool RetirementPausesInput() const;

  // One retired binding. The ledger is fixed-capacity: a new binding is refused as
  // kRetiring rather than growing it. The context reference and the retirement-only session
  // are kept until cleanup is decided, because only that session's writer can prove it still
  // owns the composition; the session releases its own context reference at Close. An engine
  // request accepted before retirement is consumed once here, and a close request is
  // submitted to the executor and its result consumed before the record is dropped. A
  // cleanup that cannot be performed becomes CleanupState::kBlocked with a named reason and
  // is never reported as Finished.
  struct RetiringBinding {
    std::uint64_t sequence = 0;  // 1-based order in which this binding entered the ledger
    edit::ContextIdentity identity{};
    ITfContext* context = nullptr;  // owned coordinator reference
    std::unique_ptr<edit::CompositionEditSession> session;
    engine::ClientId client{};
    engine::RequestId engine_request{};
    bool engine_result_pending = false;
    engine::RequestId close_request{};
    bool close_requested = false;
    bool close_terminal = false;
    bool close_failed = false;
    CleanupState cleanup = CleanupState::kNone;
    CleanupBlockReason cleanup_block = CleanupBlockReason::kNone;
    bool cleanup_finished = false;
    // Per-record operation evidence, filled when cleanup starts and when its plan answers.
    bool cleanup_evidence_captured = false;
    bool cleanup_owned_at_start = false;
    bool cleanup_live_at_start = false;
    bool cleanup_external_at_start = false;
    bool cleanup_ended_before_at_start = false;
    std::uint64_t cleanup_termination_notifications_at_start = 0;
    bool cleanup_plan_submitted = false;
    std::uint64_t cleanup_plan_session_id = 0;
    bool cleanup_end_issued = false;
    HRESULT cleanup_end_hr = S_OK;
    std::uint64_t cleanup_termination_notifications_after_call = 0;
    // Sink termination notifications observed when this cleanup settled. A value greater than
    // at_start is direct evidence that TSF terminated the composition while cleanup was
    // waiting, including while an older accepted plan still owed its Stale terminal and no
    // cleanup plan had been submitted yet.
    std::uint64_t cleanup_termination_notifications_at_settle = 0;
  };
  static constexpr std::size_t kRetirementCapacity = 2;

  void RetireCurrentBinding();
  // Advances every retirement record without writing inside a TSF notification: submits a
  // close and a composition-finish plan, consumes one engine result and one close result,
  // and drops a record only when both are consumed and cleanup is terminal.
  void PumpRetirements();
  void ResolveRetirementCleanup(RetiringBinding& record,
                                const edit::EditResult& result);
  // Captures what the record's writer owned before any cleanup decision or TSF call.
  void CaptureCleanupEvidence(RetiringBinding& record);
  // The only terminal transition for a cleanup. Idempotent: a record already terminal is
  // never counted again, so repeated Pump or a late result cannot double-settle it.
  void SettleRetirementCleanup(RetiringBinding& record, CleanupState state,
                               CleanupBlockReason reason = CleanupBlockReason::kNone);
  RetirementSnapshot BuildRetirementSnapshot() const;
  void CloseRetirementResources(RetiringBinding* record);
  // Shared implementation of Bind/BindObserved. The generation is supplied by the caller;
  // BindObserved's comes from the registry, Bind's from generation_counter_.
  BindStatus BindImpl(ITfContext* context, std::uint64_t context_id,
                      std::uint64_t generation, LONG initial_caret_utf16,
                      engine::ClientId engine_client,
                      bool use_internal_generation);
  // True when the bound/requested real context is latched Uncertain in the registry.
  bool RegistryBlocks(ITfContext* context) const;
  // Begins a real permission query for the current binding. Returns false when the
  // source refuses structurally (the caller refuses the work).
  bool StartPermissionQuery();
  // P3.13 live target checks. RevalidateForSubmit performs the full query and returns the
  // submit status to hand back (kAccepted when current). EnsureTargetCurrent is the
  // post-engine/apply-time variant: with `engine_already_ran` a refusal is NeedsResync
  // (the engine consumed the event), otherwise Rejected. Neither replays work.
  SubmitStatus RevalidateForSubmit();
  bool EnsureTargetCurrent(const char* why, bool engine_already_ran);
  // Records the last submit result for the lab/self-test without changing the state machine.
  SubmitStatus RecordSubmit(SubmitStatus status);
  void RecordTargetRevalidation(const context::TargetRevalidation& result);
  // Consumes a ready verdict: validates identity/generation, caches it, mirrors it to
  // the P3.7 session gate. Returns false unless the verdict is complete, current and
  // Allowed.
  bool PermissionVerdictAllows(bool* identity_current);
  // A kPermissionPending work item advanced by Pump once its query completed.
  void AdvancePermissionStage();
  // Continues a permission-allowed work item into the engine dispatch.
  void DispatchPendingWork(const PendingPermissionWork& work);
  // Builds and submits the raw commit plan for one degraded run. The caller owns the work
  // ledger entry (FinishWorkLedger already ran) and the permission verdict.
  void SubmitRawPlan(std::u16string raw_utf16);
  // Builds and submits the host-owned token replacement plan. The caller owns the work
  // ledger entry (FinishWorkLedger already ran) and the permission verdict.
  void SubmitSuggestionPlan(std::u16string expected_text, std::u16string replacement_text,
                            LONG expected_caret_utf16);
  // Terminal accounting for a raw work item. Never touches the document.
  void NoteRawTerminal(bool applied);
  void FinishWorkLedger(WorkKind kind);
  SubmitStatus FinishSubmit(engine::RequestId request, WorkKind kind);
  void ResolveEngineResult(const engine::CommandResult& result);
  // Copies the engine's current page into candidate_snapshot_ (or clears it) and bumps the
  // version. Text lives only in memory; diagnostics never see it.
  void UpdateCandidateSnapshot(const engine::EngineState& state);
  void InvalidateCandidateSnapshot();
  // Decides what the observation means, then (only for an edit-producing plan) resolves
  // the permission again before submitting. NoEdit/InvalidObservation/UnsupportedShape are
  // decided first so a permission answer cannot hide an unreadable engine result.
  void BeginApplyAfterEngine(const EngineObservation& observation);
  // `engine_already_ran` is false only for the raw degraded path, where no engine call
  // exists and a target move is a clean refusal rather than a divergence.
  void SubmitEditPlan(const PlanDecision& decision, bool engine_already_ran = true);
  void ResolveEditResult(const edit::EditResult& result);
  void ApplyViewUpdate(const edit::EditPlan& plan);
  void EndWork(WorkOutcome outcome, RejectCause cause);
  void EnterNeedsResync(const char* why, RejectCause cause);
  void EnterUncertainLockdown();
  void DiscardPendingEngineResult();
  void CachePermission(context::PermissionState state);

  engine::EngineExecutor& executor_;
  TfClientId tsf_client_id_;
  context::PermissionSource& permission_source_;
  // Optional P3.10 identity/latch owner; not owned. Consulted by both bind paths and by
  // the submit gate with the REAL context pointer.
  context::ContextRegistry* registry_ = nullptr;
  // Optional P3.13 target authority; not owned. The product controller attaches the
  // observer. The write guard it creates is owned by the P3.7 session's callback and does
  // not depend on this pointer's lifetime.
  context::TargetRevalidator* target_revalidator_ = nullptr;

  bool bound_ = false;
  bool closed_ = false;
  bool suggestion_mode_ = false;
  // Last engine observation said the engine itself is composing (its preedit is not in the
  // document in suggestion mode). Used only to classify Backspace/Enter/Escape as engine
  // candidates while the host owns the text.
  bool engine_composing_ = false;
  std::uint64_t generation_counter_ = 0;
  edit::ContextIdentity identity_{};
  engine::ClientId engine_client_{};
  // The context AddRef'd for the binding's duration; only the permission source and the
  // P3.7 session ever receive it, both on this STA thread.
  ITfContext* bound_context_ = nullptr;
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
  // A real caret read from OnEndEdit, waiting for the work item's terminal. Applied only
  // on Applied/NoEdit; cleared by a new work item or a binding change.
  bool observed_caret_pending_ = false;
  LONG observed_caret_utf16_ = 0;
  // True only while SubmitEditPlan is inside session_->ApplyAsync. A synchronous edit's
  // OnEndEdit arrives during that call while phase_ may still be Idle; the real caret must
  // wait for the plan-derived view update instead of being overwritten by it.
  bool submitting_edit_ = false;
  std::uint64_t observed_caret_updates_ = 0;
  std::uint64_t observed_caret_applied_ = 0;
  // The work item parked in kPermissionPending: the submitted key/candidate, or the
  // engine observation held for its apply-time re-verification.
  PendingPermissionWork pending_permission_;
  // True while the kPermissionPending phase belongs to RefreshPermission rather than a
  // work item. The pending_permission_ slot is empty in that case.
  bool permission_refresh_ = false;
  // One proactive resolution per binding. A source that cannot produce a usable verdict
  // must not be re-queried on every pump; the next binding tries again.
  bool permission_refresh_attempted_ = false;
  // The plan of the edit currently pending (EditPlan's constructors are private to its
  // factory, so the slot is an optional rather than a pre-built value).
  std::optional<edit::EditPlan> pending_plan_;
  edit::EditStatus last_edit_status_ = edit::EditStatus::kRejected;
  edit::EditRejectReason last_edit_reason_ = edit::EditRejectReason::kNone;
  HRESULT last_edit_request_hr_ = S_OK;
  HRESULT last_edit_session_hr_ = S_OK;
  HRESULT last_edit_operation_hr_ = S_OK;
  LONG last_edit_observed_caret_utf16_ = -1;

  SubmitStatus last_submit_status_ = SubmitStatus::kAccepted;
  std::uint64_t submit_refusals_ = 0;
  context::TargetRevalidation last_target_revalidation_{};
  std::uint64_t target_revalidations_ = 0;
  std::uint64_t target_refusals_ = 0;

  LocalDocumentView view_;
  CandidateSnapshot candidate_snapshot_;
  std::uint64_t candidate_version_ = 0;
  std::uint64_t candidate_snapshots_ = 0;
  std::uint64_t candidate_stale_refusals_ = 0;

  // Fixed-capacity retirement ledger. It holds only already-invalidated bindings, so it can
  // never be used to continue typing into them.
  std::vector<RetiringBinding> retiring_;
  std::uint64_t retirements_started_ = 0;
  std::uint64_t retirements_settled_ = 0;
  std::uint64_t retirements_finished_ = 0;
  std::uint64_t retirements_already_ordinary_ = 0;
  std::uint64_t retirements_blocked_ = 0;
  std::uint64_t retirements_externally_terminated_ = 0;
  std::uint64_t retirement_cleanup_end_calls_ = 0;
  std::uint64_t retirement_cleanup_end_failures_ = 0;
  std::uint64_t last_cleanup_sequence_ = 0;
  std::uint64_t last_cleanup_context_id_ = 0;
  std::uint64_t last_cleanup_generation_ = 0;
  bool last_cleanup_owned_at_start_ = false;
  bool last_cleanup_live_at_start_ = false;
  bool last_cleanup_external_at_start_ = false;
  bool last_cleanup_ended_before_at_start_ = false;
  std::uint64_t last_cleanup_termination_notifications_at_start_ = 0;
  std::uint64_t last_cleanup_termination_notifications_at_settle_ = 0;
  bool last_cleanup_plan_submitted_ = false;
  std::uint64_t last_cleanup_plan_session_id_ = 0;
  bool last_cleanup_end_issued_ = false;
  HRESULT last_cleanup_end_hr_ = S_OK;
  std::uint64_t last_cleanup_termination_notifications_after_call_ = 0;
  std::uint64_t retirement_close_requests_ = 0;
  std::uint64_t retirement_close_results_ = 0;
  std::uint64_t retirement_close_failures_ = 0;
  std::uint64_t retirement_close_queue_full_retries_ = 0;
  std::uint64_t retirement_engine_results_discarded_ = 0;
  std::uint64_t retirement_unresolved_close_requests_ = 0;
  std::uint64_t retirement_unresolved_engine_results_ = 0;
  CleanupState last_cleanup_state_ = CleanupState::kNone;
  CleanupBlockReason last_cleanup_block_ = CleanupBlockReason::kNone;
  // Request ids still outstanding when Close() ran. The coordinator is terminal, but the
  // host can bounded-wait and consume these from the executor; they are not forgotten.
  RetirementSnapshot closed_retirement_{};

  std::uint64_t works_accepted_ = 0;
  std::uint64_t raw_commits_begun_ = 0;
  std::uint64_t edit_pump_checks_ = 0;
  std::uint64_t edit_pump_ready_ = 0;
  std::uint64_t raw_commits_applied_ = 0;
  std::uint64_t raw_commits_failed_ = 0;
  std::uint64_t raw_units_applied_ = 0;
  std::uint64_t raw_units_refused_ = 0;
  std::uint64_t last_raw_units_ = 0;
  std::uint64_t last_suggestion_expected_units_ = 0;
  LONG last_suggestion_expected_caret_utf16_ = -1;
  std::uint64_t last_suggestion_replacement_units_ = 0;
  LONG last_raw_composition_start_ = -1;
  std::uint64_t last_raw_composition_units_ = 0;
  std::uint64_t last_raw_input_units_ = 0;
  std::uint64_t last_raw_plan_units_ = 0;
  bool raw_terminal_noted_ = false;
  std::uint64_t engine_results_taken_ = 0;
  std::uint64_t plans_submitted_ = 0;
  std::uint64_t edits_applied_ = 0;
  std::uint64_t edits_refused_ = 0;
  std::uint64_t edits_stale_ = 0;
  std::uint64_t permission_queries_begun_ = 0;
  std::uint64_t permission_queries_completed_ = 0;
  std::uint64_t pumps_ = 0;
};

}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_COORDINATOR_COORDINATOR_H_
