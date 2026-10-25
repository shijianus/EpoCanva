// EpoCanva P3.13 -- target revalidation for ordinary input and ordinary edits.
//
// WHAT THIS ANSWERS. "Is the context this coordinator is about to type into still the
// current TSF input target of THIS thread?" It is deliberately more than a non-null
// GetFocus check:
//
//   1. thread input focus, from ITfThreadFocusSink notifications when they arrive and from
//      the real ITfThreadMgr::IsThreadFocus query otherwise; a Win32 thread-focus window is
//      accepted only as an explicitly labelled revalidation for hosts that do not register
//      a TIP (the integration lab), never as a TSF notification;
//   2. the thread manager's current ITfDocumentMgr (GetFocus);
//   3. the canonical identity of that document manager's top context (GetTop, compared by
//      IUnknown, never by interface address);
//   4. the ContextRegistry id/generation this binding was created with;
//   5. real input permission, resolved separately by the coordinator from context metadata.
//
// GetForegroundWindow is not used here at all. It cannot stand in for TSF thread focus, and
// it is not needed: the TSF focus query plus the bound context's document manager/top
// context are the facts an input method must agree with before it writes.
//
// WHERE THE CHECK RUNS. The coordinator calls Revalidate() before accepting ordinary input
// and again before submitting an ordinary edit plan. The P3.7 session calls its
// TargetWriteGuard inside DoEditSession, after TSF granted the lock and before any write, so
// a focus change that happened while an asynchronous edit waited is also refused. The
// retirement cleanup (EndOwnedComposition) deliberately does NOT use the write guard: ending
// a composition the writer really owns is legal on a context that has already lost focus,
// and keeps the P3.12 ownership/range/permission/read-only/Uncertain checks.
//
// LIFETIME. A TargetWriteGuard holds a weak reference to the observer state through a
// std::function. It never stores a coordinator/observer raw pointer that a late callback
// could dereference after shutdown; a failed lock means "not current" and refuses the write.
// All calls run on the owning STA.
#ifndef EPOCANVA_WINDOWS_TSF_CONTEXT_TARGET_REVALIDATION_H_
#define EPOCANVA_WINDOWS_TSF_CONTEXT_TARGET_REVALIDATION_H_

#include <windows.h>
#include <msctf.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace epocanva {
namespace tsf {
namespace context {

enum class TargetRevalidationStatus : std::uint8_t {
  kOk = 0,
  kClosed,                 // the observer no longer answers
  kThreadFocusUnconfirmed, // neither a notification nor a real query confirms focus
  kThreadFocusLost,        // the last thread-focus notification was OnKillThreadFocus
  kNoDocumentManager,      // GetFocus succeeded with no document manager
  kNoTopContext,           // the focused document manager has no top context
  kContextMismatch,        // the top context is not the bound context (canonical identity)
  kGenerationMismatch,     // same context, but the registry generation moved on
  kQueryFailed,            // a required query failed; the failure is in query_hr
};

const char* DescribeTargetRevalidationStatus(TargetRevalidationStatus status);

enum class ThreadFocusEvidence : std::uint8_t {
  kNone = 0,
  kNotificationSet,    // ITfThreadFocusSink::OnSetThreadFocus was the last notification
  kNotificationKill,   // ITfThreadFocusSink::OnKillThreadFocus was the last notification
  kQuerySet,           // ITfThreadMgr::IsThreadFocus answered TRUE
  kWin32ThreadFocus,   // GetGUIThreadInfo(hwndFocus) is owned by this thread
};

const char* DescribeThreadFocusEvidence(ThreadFocusEvidence evidence);

// The raw facts. This struct is COM-free and pure, so the decision can be unit-tested and
// logged without a live TSF manager.
struct TargetEvidence {
  bool thread_focus_query_ok = false;
  bool thread_focus = false;
  bool win32_thread_focus = false;
  bool thread_focus_notification_set = false;
  bool thread_focus_notification_kill = false;
  bool has_document_manager = false;
  bool has_top_context = false;
  bool top_context_matches = false;
  bool registry_identity_matches = false;
  bool registry_generation_matches = false;
  std::uint64_t observed_context_id = 0;
  std::uint64_t observed_generation = 0;
};

struct TargetRevalidation {
  TargetRevalidationStatus status = TargetRevalidationStatus::kClosed;
  ThreadFocusEvidence thread_focus_evidence = ThreadFocusEvidence::kNone;
  // The failing query's HRESULT, or the successful query's HRESULT when the refusal is a
  // logical identity/generation mismatch. S_OK is never used to hide a failed call.
  HRESULT query_hr = S_OK;
  TargetEvidence evidence;

  bool ok() const { return status == TargetRevalidationStatus::kOk; }
};

// Pure decision over already-observed facts. Does not query anything.
TargetRevalidation EvaluateTargetEvidence(const TargetEvidence& evidence);

// Compares two contexts by canonical IUnknown, AddRef/Release-balanced. Null never matches.
bool TargetContextsEqual(ITfContext* left, ITfContext* right);

// Called inside the granted edit session for an ordinary plan. It performs the same live
// target query as Revalidate(); returning false makes the session reject the plan with
// EditRejectReason::kTargetChanged before any write. It must never throw.
class TargetWriteGuard {
 public:
  virtual ~TargetWriteGuard() {}
  virtual bool StillCurrent(ITfContext* context, std::uint64_t context_id,
                            std::uint64_t generation) const = 0;
};

// The host-side target authority. The product path (ObservedBindingController) attaches the
// ContextObserver; the plan-write guard it creates is lifetime-safe across shutdown.
class TargetRevalidator {
 public:
  virtual ~TargetRevalidator() {}
  virtual TargetRevalidation Revalidate(ITfContext* context,
                                        std::uint64_t context_id,
                                        std::uint64_t generation) = 0;
  virtual std::shared_ptr<TargetWriteGuard> CreateWriteGuard() = 0;
};

std::shared_ptr<TargetWriteGuard> MakeTargetWriteGuard(
    std::function<bool(ITfContext*, std::uint64_t, std::uint64_t)> check);

}  // namespace context
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_CONTEXT_TARGET_REVALIDATION_H_
