// EpoCanva P3.9 -- real TSF permission queries and the coordinator's source seam.
//
// WHAT THE REAL SOURCE READS, AND WHERE EACH ANSWER COMES FROM. All reads happen on the
// STA thread that owns the context, through the real interfaces of the bound
// ITfContext; nothing here talks to the engine executor or crosses a thread:
//
//   ITfContext::GetStatus                       no cookie needed (msctf.h)
//   ITfContext -> QI IID_ITfCompartmentMgr
//     -> GetCompartment(GUID_COMPARTMENT_KEYBOARD_DISABLED) -> GetValue   no cookie
//     -> GetCompartment(GUID_COMPARTMENT_EMPTYCONTEXT)     -> GetValue   no cookie
//   ITfContext::GetAppProperty(GUID_PROP_INPUTSCOPE)
//     -> ITfReadOnlyProperty::GetValue(ec, selection range, &var)        NEEDS a real
//        read-only edit cookie: requested as RequestEditSession(TF_ES_READ), sync
//        first and TF_ES_ASYNC as the documented fallback when the synchronous grant is
//        unavailable. No cookie is invented and the CreateContext cookie is never used.
//        A VT_UNKNOWN value is QI'd for IID_ITfInputScope and its GetInputScopes array
//        is copied out and freed with CoTaskMemFree, per that method's contract.
//
// The read path for InputScope is the one the ITfInputScope documentation prescribes for
// a text service: GetAppProperty(GUID_PROP_INPUTSCOPE) + GetValue over a range inside a
// real edit session. The application side of that contract is the context owner's
// attribute support (ITextStoreACP::RequestSupportedAttrs / RetrieveRequestedAttrs),
// which is how the P3.7 test host publishes scopes in the integration tests. Two
// no-metadata answers map to "absent": the officially documented S_FALSE (no coverage,
// VT_EMPTY) and the E_FAIL this machine's real msctf.dll returns for exactly that state
// (empirically verified; see the .cc for the residual-risk reasoning). Every other
// failure HRESULT is a query failure and yields Unknown.
//
// IDENTITY AND LIFETIME. A query is begun against one (context, context_id, generation)
// triple and its result carries that triple; the coordinator re-validates it against
// its binding before using a verdict. The source AddRefs the context for the duration of
// the query and releases it when the query completes, is abandoned, or is destroyed; the
// property and callback objects it holds are released per their contracts. A callback
// that never runs cannot make anything wait: Abandon() returns immediately and a late
// callback publishes nothing.
//
// WHAT IS DELIBERATELY NOT HERE. No network I/O, no document text read (the InputScope
// property is metadata about the field, not field content), no clipboard, no UIA, no
// window scanning, no key sink, no registration.
#ifndef EPOCANVA_WINDOWS_TSF_CONTEXT_PERMISSION_QUERY_H_
#define EPOCANVA_WINDOWS_TSF_CONTEXT_PERMISSION_QUERY_H_

#include <windows.h>
#include <msctf.h>
#include <inputscope.h>

#include <cstdint>
#include <memory>

#include "input_permission.h"

namespace epocanva {
namespace tsf {
namespace context {

// Reads the same four axes again, using an edit cookie the caller already holds. This is
// the last-moment check an edit session performs inside its own granted lock: it requests
// no new edit session, invents no cookie, and reads only the field's metadata (status,
// two compartments, InputScope) -- never document text. A failed read is represented in
// the returned facts (and therefore classifies as Unknown), never as Allowed.
PermissionFacts ReadPermissionFactsWithCookie(ITfContext* context,
                                              TfEditCookie edit_cookie);

// One resolved query, stamped with the binding identity it was begun against. Callers
// that hold a result across time must compare identity and generation again at use
// time; a mismatch invalidates the verdict.
struct PermissionQueryResult {
  bool complete = false;
  PermissionDecision decision;
  std::uint64_t context_id = 0;
  std::uint64_t generation = 0;
};

enum class PermissionQueryStatus : std::uint8_t {
  kIdle = 0,   // no query is active (never begun, or abandoned)
  kPending,    // begun; the read edit session has not completed yet
  kReady,      // a verdict exists; read it with Result()
};

const char* DescribePermissionQueryStatus(PermissionQueryStatus status);

// Value-only counters for assertions and text-free logging.
struct PermissionQueryDiagnostics {
  std::uint64_t queries_begun = 0;
  std::uint64_t queries_completed = 0;   // reached a verdict (Allowed/Denied/Unknown)
  std::uint64_t queries_abandoned = 0;   // terminated before a verdict existed
  std::uint64_t status_reads = 0;
  std::uint64_t compartment_reads = 0;
  std::uint64_t scope_property_reads = 0;  // GetValue calls inside the read session
  std::uint64_t read_sessions_sync = 0;
  std::uint64_t read_sessions_async = 0;   // sync grant unavailable, async fallback
  std::uint64_t read_session_failures = 0;
  // The last scope-axis failure point, for text-free evidence: 0 none, 1 GetSelection,
  // 2 property GetValue, 3 payload QI, 4 GetInputScopes, 5 session request, and the
  // HRESULT of that call (or S_OK).
  int last_scope_stage = 0;
  HRESULT last_scope_hr = S_OK;
};

// The seam the coordinator (and only the coordinator) drives. The production
// implementation reads the real TSF metadata; test programs implement the same
// interface explicitly as their unit seam -- the product coordinator has no manual
// permission setter.
class PermissionSource {
 public:
  virtual ~PermissionSource() {}
  // Starts one query. Structurally invalid requests (null context, zero id/generation,
  // or a previous query still pending) return false and change nothing.
  virtual bool Begin(ITfContext* context, std::uint64_t context_id,
                     std::uint64_t generation) = 0;
  virtual PermissionQueryStatus Status() const = 0;
  // Meaningful only when Status() == kReady.
  virtual PermissionQueryResult Result() const = 0;
  // Signalled when a pending query completes; also set by Abandon() so a bounded waiter
  // cannot hang. Null handle when nothing is pending.
  virtual HANDLE CompletionEvent() const = 0;
  // Terminal for the current query. Never waits; a callback that runs later publishes
  // nothing. The held context reference is released here.
  virtual void Abandon() = 0;
  virtual PermissionQueryDiagnostics Diagnostics() const = 0;
};

// The production source: real GetStatus/compartment/InputScope reads as documented in
// the file comment. `client_id` is the TIP client id the read edit session is requested
// with; the returned object must be used and destroyed on the context's STA thread.
std::unique_ptr<PermissionSource> CreateRealPermissionSource(TfClientId client_id);

}  // namespace context
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_CONTEXT_PERMISSION_QUERY_H_
