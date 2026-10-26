// EpoCanva P3.7/P3.13 -- the TSF edit-session executor.
//
// WHAT THIS DOES. Binds one real ITfContext to an opaque identity/generation, accepts
// immutable EditPlans, and applies each plan through the document's real edit-session
// protocol:
//
//   RequestEditSession(client_id, callback, TF_ES_SYNC|TF_ES_READWRITE, &phrSession)
//      -> msctf.dll requests TS_LF_READWRITE from the ITextStoreACP
//      -> the store grants the lock through ITextStoreACPSink::OnLockGranted
//      -> msctf.dll calls callback->DoEditSession(ec) inside that grant
//      -> CompositionWriter performs the TSF range/composition calls with ec
//
// All three results are kept separately: RequestEditSession's HRESULT, its phrSession, and
// DoEditSession's own HRESULT. TF_S_ASYNC in phrSession means only that the session is
// waiting; it never means the text was applied. An asynchronous result stays kPending until
// the callback actually runs; the caller pumps its STA message loop and waits on
// CompletionEvent() -- no sleep and no polling here.
//
// TARGET REVALIDATION (P3.13). An optional TargetRecheck is called before the lock is
// requested and again inside DoEditSession, after the identity checks and before the
// permission recheck or any writer call. A false answer is kRejected/kTargetChanged and
// issues no TSF write. EndOwnedComposition is exempt so a writer can finish its own
// composition on an unfocused context; every P3.12 cleanup rule still applies.
//
// IDENTITY AND GENERATION. A plan is applied only when its ContextIdentity equals the bound
// identity. Invalidate() bumps the generation and unbinds; a plan built before that is
// kStale. The callback rechecks closed/bound/identity/generation/permission and the exact
// ITfContext pointer under the state mutex before making any TSF call, so an old plan cannot
// write to a newly bound context.
//
// PERMISSION. The bound context carries an InputPermission. The executor writes only when it
// is kAllowed; kDenied and kUnknown are refused before RequestEditSession, so no lock is even
// requested for them. This is a host-supplied gate, not real password-field detection; see
// docs/windows/tsf-edit-contract.md.
//
// THREADING. Every method, including Close(), must be called on the thread that owns the bound
// STA. The class is not a cross-thread API; the mutex only protects asynchronous completion
// bookkeeping and makes PendingResult()/Snapshot() safe to read after the callback ran.
//
// WHAT IS NOT HERE. No keyboard sink, no registration, no candidate UI, no worker thread, no
// COM pointer in a plan, no automatic retry, no rollback claim, and no continued editing
// after a write whose effect could not be confirmed (see kUncertainTextState).
#ifndef EPOCANVA_WINDOWS_TSF_EDIT_EDIT_SESSION_H_
#define EPOCANVA_WINDOWS_TSF_EDIT_EDIT_SESSION_H_

#include <windows.h>
#include <msctf.h>

#include <cstdint>
#include <functional>
#include <memory>

#include "edit_plan.h"
#include "edit_result.h"

namespace epocanva {
namespace tsf {
namespace edit {

enum class InputPermission : std::uint8_t {
  kAllowed = 0,
  kDenied,
  kUnknown,
};

const char* DescribeInputPermission(InputPermission permission);

// Lifetime evidence of the composition this session's writer owned. `live` is the current
// hold; the other flags survive the composition reference being released. `ended_by_writer`
// means the last own composition was closed by this writer's successful EndComposition;
// `externally_terminated` means TSF reported it terminated by another party.
struct OwnedCompositionState {
  bool live = false;
  bool ever_owned = false;
  bool externally_terminated = false;
  bool ended_by_writer = false;
  std::uint64_t termination_notifications = 0;
};

struct CompositionEditSessionDiagnostics {
  bool bound = false;
  bool closed = false;
  bool pending = false;
  bool active_composition = false;
  std::uint64_t context_id = 0;
  std::uint64_t generation = 0;
  InputPermission permission = InputPermission::kUnknown;
  LONG composition_start_utf16 = -1;
  LONG composition_length_utf16 = -1;
  std::uint64_t apply_calls = 0;
  std::uint64_t sync_requests = 0;
  std::uint64_t async_requests = 0;
  std::uint64_t request_failures = 0;
  std::uint64_t callbacks = 0;
  std::uint64_t locks_observed = 0;
  std::uint64_t permission_rechecks = 0;
  std::uint64_t target_prechecks = 0;
  std::uint64_t target_callback_checks = 0;
  std::uint64_t target_rejections = 0;
  std::uint64_t stale = 0;
  std::uint64_t applied = 0;
  std::uint64_t rejected = 0;
  std::uint64_t failed_before_mutation = 0;
  std::uint64_t failed_possibly_mutated = 0;
  std::uint64_t failed_after_mutation = 0;
  // Composition lifetime evidence (see OwnedCompositionState).
  bool composition_live = false;
  bool composition_ever_owned = false;
  bool composition_externally_terminated = false;
  bool composition_ended_by_writer = false;
  std::uint64_t composition_end_calls = 0;
  std::uint64_t composition_end_successes = 0;
  std::uint64_t composition_end_failures = 0;
  std::uint64_t composition_termination_notifications = 0;
};

class EditSessionCallback;

class CompositionEditSession {
 public:
  explicit CompositionEditSession(TfClientId client_id);
  ~CompositionEditSession();

  CompositionEditSession(const CompositionEditSession&) = delete;
  CompositionEditSession& operator=(const CompositionEditSession&) = delete;

  // AddRefs the context. Fails when the session is closed, a request is pending, or a
  // composition is still active. The initial permission is kUnknown (safe refusal).
  bool Bind(ITfContext* context, std::uint64_t context_id, std::uint64_t generation);

  // Marks the binding invalid and increments the generation. The held context reference is
  // kept until Close()/destruction so a pending callback can compare safely; old plans become
  // kStale and the callback makes no TSF call.
  void Invalidate();

  // Retirement entry. A pending accepted plan is made Stale without writing; from then on
  // only an EndOwnedComposition plan for the SAME identity is accepted. The session stays
  // bound so the writer's own composition can be ended through one legal edit session.
  // Idempotent; has no effect after Close().
  void BeginRetirement();
  // True when this session's writer had an unconfirmed write. No cleanup plan may run.
  bool HasUncertainOutcome() const;

  // Releases the held context/composition references. An active composition is released
  // without EndComposition (the document owner terminates it); the caller that requires a
  // clean end must apply a Cancel plan first. Idempotent.
  void Close();

  void SetInputPermission(InputPermission permission);
  // The last-moment, callback-time permission source. When set, it is called inside
  // DoEditSession with the session's real context and the granted edit cookie, after the
  // identity/staleness checks and before any TSF write. The value it returns is
  // authoritative for that callback; a throw or kUnknown refuses the write. The product
  // coordinator installs a real-metadata reader here (P3.9); P3.7-only users leave it
  // unset and keep the SetInputPermission behaviour.
  using PermissionRecheck = std::function<InputPermission(ITfContext*, TfEditCookie)>;
  void SetPermissionRecheck(PermissionRecheck recheck);
  // P3.13 target authority. Called before requesting the lock and again inside
  // DoEditSession with the granted cookie, before any write; false makes the plan
  // kRejected/kTargetChanged. It is deliberately NOT called for EndOwnedComposition, so a
  // retirement cleanup can still finish a composition the writer owns on an unfocused
  // context. The coordinator installs a guard whose lifetime is independent of itself.
  using TargetRecheck =
      std::function<bool(ITfContext*, std::uint64_t context_id, std::uint64_t generation)>;
  void SetTargetRecheck(TargetRecheck recheck);
  InputPermission input_permission() const;
  bool bound() const;
  ContextIdentity identity() const;

  // Synchronous request: returns a final result (normally Applied/Rejected/Failed.../Stale).
  // A Pending result is possible only if TSF queued a sync request; the caller must then pump
  // and wait on CompletionEvent(), exactly as for ApplyAsync.
  EditResult ApplySync(const EditPlan& plan);

  // Asynchronous request. Returns kPending when TSF queued it; otherwise a final result. The
  // completion event is reset before submission and set by the callback.
  EditResult ApplyAsync(const EditPlan& plan);

  EditResult PendingResult() const;
  bool HasPending() const;
  HANDLE CompletionEvent() const;

  CompositionEditSessionDiagnostics Snapshot() const;
  bool active_composition() const;
  OwnedCompositionState composition_state() const;

 private:
  friend class EditSessionCallback;
  struct State;

  EditResult Submit(const EditPlan& plan, bool asynchronous);
  bool Precheck(const EditPlan& plan, ITfContext** context, EditResult* rejection);

  std::shared_ptr<State> state_;
};

}  // namespace edit
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_EDIT_EDIT_SESSION_H_
