// EpoCanva P3.7 -- the TSF edit-session executor.
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
// COM pointer in a plan, no automatic retry, no rollback claim.
#ifndef EPOCANVA_WINDOWS_TSF_EDIT_EDIT_SESSION_H_
#define EPOCANVA_WINDOWS_TSF_EDIT_EDIT_SESSION_H_

#include <windows.h>
#include <msctf.h>

#include <cstdint>
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
  std::uint64_t stale = 0;
  std::uint64_t applied = 0;
  std::uint64_t rejected = 0;
  std::uint64_t failed_before_mutation = 0;
  std::uint64_t failed_after_mutation = 0;
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

  // Releases the held context/composition references. An active composition is released
  // without EndComposition (the document owner terminates it); the caller that requires a
  // clean end must apply a Cancel plan first. Idempotent.
  void Close();

  void SetInputPermission(InputPermission permission);
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
