// EpoCanva P3.9 -- implementation of the real TSF permission source.
//
// THREADING. Everything here runs on the STA thread that owns the bound context:
// Begin(), Status(), Result(), Abandon() and the edit-session callback alike. The
// callback can only run while that thread dispatches messages, so no lock is needed
// around the shared state; the completion event exists for the HOST's bounded waits,
// exactly like the P3.7 edit session's event.
#include "permission_query.h"

#include <objbase.h>

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

namespace epocanva {
namespace tsf {
namespace context {

namespace {

// --- compartment read --------------------------------------------------------------------

// Reads one context compartment. The outcome keeps the four documented shapes apart:
//   S_OK + VT_I4             -> kValue (any nonzero LONG later means "condition holds")
//   S_FALSE (VT_EMPTY)       -> kAbsent -- the official "compartment has no value"
//   S_OK + any other type    -> kWrongType -- uninterpretable, not "false"
//   anything FAILED          -> kQueryFailed (including E_UNEXPECTED from a cleared
//                               compartment: a mid-transition read stays Unknown)
CompartmentFact ReadCompartment(ITfContext* context, REFGUID compartment_guid,
                                PermissionQueryDiagnostics* counters) {
  CompartmentFact fact;
  if (counters != nullptr) {
    ++counters->compartment_reads;
  }
  ITfCompartmentMgr* manager = nullptr;
  if (FAILED(context->QueryInterface(IID_ITfCompartmentMgr,
                                     reinterpret_cast<void**>(&manager))) ||
      manager == nullptr) {
    fact.presence = FactPresence::kQueryFailed;
    return fact;
  }
  ITfCompartment* compartment = nullptr;
  const HRESULT get_hr = manager->GetCompartment(compartment_guid, &compartment);
  manager->Release();
  if (FAILED(get_hr) || compartment == nullptr) {
    fact.presence = FactPresence::kQueryFailed;
    return fact;
  }
  VARIANT value;
  ::VariantInit(&value);
  const HRESULT value_hr = compartment->GetValue(&value);
  compartment->Release();
  if (value_hr == S_FALSE) {
    // Official no-value answer; VariantInit already matches VT_EMPTY.
    fact.presence = FactPresence::kAbsent;
    return fact;
  }
  if (FAILED(value_hr)) {
    ::VariantClear(&value);  // defensive: a failed call may have left a payload
    fact.presence = FactPresence::kQueryFailed;
    return fact;
  }
  if (value.vt == VT_EMPTY || value.vt == VT_NULL) {
    // A successful read that carries no payload; the same meaning as S_FALSE.
    fact.presence = FactPresence::kAbsent;
    return fact;
  }
  if (value.vt != VT_I4) {
    ::VariantClear(&value);  // documented type discipline, defensive only
    fact.presence = FactPresence::kWrongType;
    return fact;
  }
  fact.presence = FactPresence::kValue;
  fact.value = value.lVal;
  return fact;
}

void ReadStatusAndCompartments(ITfContext* context, PermissionFacts* facts,
                               PermissionQueryDiagnostics* counters) {
  if (counters != nullptr) {
    ++counters->status_reads;
  }
  TF_STATUS status = {};
  if (SUCCEEDED(context->GetStatus(&status))) {
    facts->status.presence = FactPresence::kValue;
    facts->status.read_only = (status.dwDynamicFlags & TF_SD_READONLY) != 0;
  } else {
    facts->status.presence = FactPresence::kQueryFailed;
  }
  facts->keyboard_disabled =
      ReadCompartment(context, GUID_COMPARTMENT_KEYBOARD_DISABLED, counters);
  facts->empty_context =
      ReadCompartment(context, GUID_COMPARTMENT_EMPTYCONTEXT, counters);
}

// Reads the GUID_PROP_INPUTSCOPE property over the current selection, inside an edit
// session the caller already holds. The selection is where typing would land, and it is
// the range the ITfInputScope documentation's own example passes to GetValue. Resource
// rules: the TF_SELECTION range is released here, the ITfInputScope payload reference is
// released through VariantClear, and the CoTaskMem array is freed with CoTaskMemFree.
ScopeFact ReadScopeOverSelection(ITfContext* context, TfEditCookie edit_cookie,
                                 ITfReadOnlyProperty* property,
                                 PermissionQueryDiagnostics* counters) {
  ScopeFact fact;
  TF_SELECTION selection = {};
  ULONG fetched = 0;
  const HRESULT selection_hr = context->GetSelection(edit_cookie, TF_DEFAULT_SELECTION, 1,
                                                     &selection, &fetched);
  if (FAILED(selection_hr) || fetched != 1 || selection.range == nullptr) {
    if (counters != nullptr) {
      counters->last_scope_stage = 1;
      counters->last_scope_hr = selection_hr;
    }
    fact.presence = FactPresence::kQueryFailed;
    return fact;
  }
  ITfRange* range = selection.range;  // owned by the TF_SELECTION we must release
  VARIANT value;
  ::VariantInit(&value);
  const HRESULT value_hr = property->GetValue(edit_cookie, range, &value);
  range->Release();
  if (value_hr == S_FALSE || value_hr == E_FAIL) {
    // No published InputScope metadata over this range. S_FALSE is the officially
    // documented no-coverage answer (VT_EMPTY). E_FAIL is the answer this machine's
    // real msctf.dll returns for exactly the same state -- an attribute-free context
    // owner (verified empirically: a store that publishes scopes answers S_OK with
    // VT_UNKNOWN; a store that publishes nothing answers E_FAIL). Treating it as
    // "absent" rather than "query failed" is a documented residual-risk decision: a
    // context owner whose attribute machinery genuinely breaks internally is
    // indistinguishable from one that publishes nothing, and absence is permissive.
    // Every other failure HRESULT stays a query failure (Unknown).
    ::VariantClear(&value);  // defensive; normally VT_EMPTY
    fact.presence = FactPresence::kAbsent;
    return fact;
  }
  if (FAILED(value_hr)) {
    if (counters != nullptr) {
      counters->last_scope_stage = 2;
      counters->last_scope_hr = value_hr;
    }
    ::VariantClear(&value);  // defensive
    fact.presence = FactPresence::kQueryFailed;
    return fact;
  }
  if (value.vt == VT_EMPTY || value.vt == VT_NULL) {
    fact.presence = FactPresence::kAbsent;
    return fact;
  }
  if (value.vt != VT_UNKNOWN || value.punkVal == nullptr) {
    ::VariantClear(&value);
    fact.presence = FactPresence::kWrongType;
    return fact;
  }
  ITfInputScope* scope = nullptr;
  const HRESULT qi_hr =
      value.punkVal->QueryInterface(IID_ITfInputScope, reinterpret_cast<void**>(&scope));
  if (FAILED(qi_hr) || scope == nullptr) {
    if (counters != nullptr) {
      counters->last_scope_stage = 3;
      counters->last_scope_hr = qi_hr;
    }
    ::VariantClear(&value);
    fact.presence = FactPresence::kWrongType;
    return fact;
  }
  InputScope* delivered = nullptr;
  UINT count = 0;
  const HRESULT scopes_hr = scope->GetInputScopes(&delivered, &count);
  if (FAILED(scopes_hr)) {
    if (counters != nullptr) {
      counters->last_scope_stage = 4;
      counters->last_scope_hr = scopes_hr;
    }
    fact.presence = FactPresence::kQueryFailed;
  } else {
    fact.presence = FactPresence::kValue;
    if (delivered != nullptr) {
      for (UINT index = 0; index < count; ++index) {
        fact.scopes.push_back(static_cast<long>(delivered[index]));
      }
      ::CoTaskMemFree(delivered);  // the method's documented ownership rule
    } else if (count != 0) {
      // A null array with a nonzero count cannot be read; not guessed from.
      fact.presence = FactPresence::kQueryFailed;
      fact.scopes.clear();
    }
  }
  scope->Release();
  ::VariantClear(&value);  // releases the punkVal reference
  return fact;
}

// --- shared query state -------------------------------------------------------------------
//
// PER-QUERY STATE, SHARED COUNTERS. Every Begin() gets its own QueryState, so a
// callback still in flight for an abandoned query can only ever write its own orphaned
// state; it can never complete or corrupt the query that replaced it. The diagnostics
// counters are shared by pointer, because they describe the source, not one query.

struct QueryState {
  QueryState(TfClientId id, std::shared_ptr<PermissionQueryDiagnostics> shared_counters)
      : client_id(id), counters(std::move(shared_counters)) {
    completion_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }

  ~QueryState() {
    if (pending_callback != nullptr) {
      pending_callback->Release();
      pending_callback = nullptr;
    }
    if (context != nullptr) {
      context->Release();
      context = nullptr;
    }
    if (completion_event != nullptr) {
      ::CloseHandle(completion_event);
      completion_event = nullptr;
    }
  }

  TfClientId client_id = 0;
  ITfContext* context = nullptr;  // owned for the query's duration
  std::uint64_t context_id = 0;
  std::uint64_t generation = 0;
  bool active = false;      // this query was begun and not abandoned
  bool completed = false;   // a verdict exists
  bool abandoned = false;   // terminal; a late callback publishes nothing
  PermissionFacts facts;
  PermissionDecision decision;
  HANDLE completion_event = nullptr;
  ITfEditSession* pending_callback = nullptr;  // owned extra reference while async
  std::shared_ptr<PermissionQueryDiagnostics> counters;
};

void CompleteQuery(QueryState* state) {
  state->decision = DecidePermission(state->facts);
  state->completed = true;
  ++state->counters->queries_completed;
  if (state->completion_event != nullptr) {
    ::SetEvent(state->completion_event);
  }
  // The context reference is held only while the query needs it. A completed query keeps
  // values, not a COM pointer, so a source that is never Begin()'d again cannot pin the
  // document (or a scope object it handed out) alive.
  if (state->context != nullptr) {
    state->context->Release();
    state->context = nullptr;
  }
}

// --- the read-only edit session that reads the InputScope property ------------------------

class ScopeReadCallback final : public ITfEditSession {
 public:
  ScopeReadCallback(std::shared_ptr<QueryState> state, ITfReadOnlyProperty* property)
      : state_(std::move(state)), property_(property) {
    property_->AddRef();
  }

  ~ScopeReadCallback() {
    if (property_ != nullptr) {
      property_->Release();
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) {
      return E_INVALIDARG;
    }
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfEditSession) {
      *object = static_cast<ITfEditSession*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(reference_count_.fetch_add(1) + 1);
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const LONG remaining = reference_count_.fetch_sub(1) - 1;
    if (remaining == 0) {
      delete this;
    }
    return static_cast<ULONG>(remaining);
  }

  HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie edit_cookie) override {
    if (state_->abandoned) {
      // The query was terminated while this session was queued; publish nothing.
      done_ = true;
      return S_OK;
    }
    ++state_->counters->scope_property_reads;
    state_->facts.input_scope = ReadScopeOverSelection(
        state_->context, edit_cookie, property_, state_->counters.get());
    CompleteQuery(state_.get());
    done_ = true;
    return S_OK;
  }

  bool Done() const { return done_; }

 private:
  std::atomic<LONG> reference_count_{1};
  std::shared_ptr<QueryState> state_;
  ITfReadOnlyProperty* property_ = nullptr;
  bool done_ = false;
};

// --- the real source ----------------------------------------------------------------------
//
// The source owns at most one ACTIVE query; each Begin() allocates a fresh QueryState.
// A previous query's callback that is still queued can therefore only touch its own
// orphaned state, never the replacement.

class RealPermissionSource final : public PermissionSource {
 public:
  explicit RealPermissionSource(TfClientId client_id)
      : client_id_(client_id),
        counters_(std::make_shared<PermissionQueryDiagnostics>()) {}

  ~RealPermissionSource() override { Abandon(); }

  bool Begin(ITfContext* context, std::uint64_t context_id,
             std::uint64_t generation) override {
    if (context == nullptr || context_id == 0 || generation == 0) {
      return false;
    }
    if (active_ != nullptr && !active_->completed) {
      return false;  // one active query at a time; Abandon() ends it
    }
    active_ = std::make_shared<QueryState>(client_id_, counters_);
    QueryState* state = active_.get();
    state->context_id = context_id;
    state->generation = generation;
    state->active = true;
    context->AddRef();
    state->context = context;
    ++counters_->queries_begun;

    ReadStatusAndCompartments(state->context, &state->facts, state->counters.get());

    // The InputScope application property is the only axis that needs an edit cookie.
    // GetAppProperty's S_FALSE and E_NOTIMPL are the documented "the context owner does
    // not support / does not implement" answers -- an officially absent property.
    ITfReadOnlyProperty* property = nullptr;
    const HRESULT property_hr =
        state->context->GetAppProperty(GUID_PROP_INPUTSCOPE, &property);
    if (property_hr == S_FALSE || property_hr == E_NOTIMPL) {
      state->facts.input_scope.presence = FactPresence::kAbsent;
      CompleteQuery(state);
      return true;
    }
    if (FAILED(property_hr) || property == nullptr) {
      state->facts.input_scope.presence = FactPresence::kQueryFailed;
      CompleteQuery(state);
      return true;
    }

    RequestScopeRead(active_, property);
    return true;
  }

  PermissionQueryStatus Status() const override {
    if (active_ == nullptr || !active_->active || active_->abandoned) {
      return PermissionQueryStatus::kIdle;
    }
    return active_->completed ? PermissionQueryStatus::kReady
                              : PermissionQueryStatus::kPending;
  }

  PermissionQueryResult Result() const override {
    PermissionQueryResult result;
    if (active_ == nullptr) {
      return result;
    }
    result.context_id = active_->context_id;
    result.generation = active_->generation;
    if (active_->active && !active_->abandoned && active_->completed) {
      result.complete = true;
      result.decision = active_->decision;
    }
    return result;
  }

  HANDLE CompletionEvent() const override {
    if (active_ != nullptr && active_->active && !active_->abandoned &&
        !active_->completed) {
      return active_->completion_event;
    }
    return nullptr;
  }

  void Abandon() override {
    if (active_ == nullptr || !active_->active || active_->abandoned) {
      return;
    }
    if (active_->completed) {
      // A verdict already exists. Terminating the source must not retroactively count it
      // as abandoned (diagnostics distinguish "never answered" from "answered"); dropping
      // the state only releases the event and any remaining references.
      active_->active = false;
      active_.reset();
      return;
    }
    active_->abandoned = true;
    active_->active = false;
    ++counters_->queries_abandoned;
    if (active_->pending_callback != nullptr) {
      active_->pending_callback->Release();
      active_->pending_callback = nullptr;
    }
    if (active_->context != nullptr) {
      active_->context->Release();
      active_->context = nullptr;
    }
    if (active_->completion_event != nullptr) {
      // No waiter of an abandoned query may hang; a verdict will never arrive.
      ::SetEvent(active_->completion_event);
    }
    // The state itself is dropped, not reused: a late callback keeps it alive through
    // its own shared_ptr and can only write into this orphaned object.
    active_.reset();
  }

  PermissionQueryDiagnostics Diagnostics() const override { return *counters_; }

 private:
  // Requests the read session for the property read. A synchronous read grant is the
  // common case; when the document cannot grant it now, the documented TF_ES_ASYNC
  // fallback defers to the host's message loop and the completion event.
  static void RequestScopeRead(std::shared_ptr<QueryState> state,
                               ITfReadOnlyProperty* property) {
    ScopeReadCallback* callback = new ScopeReadCallback(state, property);
    property->Release();  // the callback holds its own reference now
    property = nullptr;

    HRESULT session_hr = S_OK;
    HRESULT request_hr = state->context->RequestEditSession(
        state->client_id, callback, TF_ES_SYNC | TF_ES_READ, &session_hr);
    if (SUCCEEDED(request_hr) && session_hr == TF_E_SYNCHRONOUS) {
      ++state->counters->read_sessions_async;
      request_hr = state->context->RequestEditSession(
          state->client_id, callback, TF_ES_ASYNC | TF_ES_READ, &session_hr);
    } else {
      ++state->counters->read_sessions_sync;
    }

    if (FAILED(request_hr)) {
      ++state->counters->read_session_failures;
      state->facts.input_scope.presence = FactPresence::kQueryFailed;
      callback->Release();
      CompleteQuery(state.get());
      return;
    }
    if (session_hr == TF_S_ASYNC) {
      // Pending. The callback, not this return, completes the query; the host pumps
      // messages and bounded-waits on CompletionEvent().
      state->pending_callback = callback;  // takes the ownership reference
      return;
    }
    if (FAILED(session_hr)) {
      ++state->counters->read_session_failures;
      state->facts.input_scope.presence = FactPresence::kQueryFailed;
      callback->Release();
      CompleteQuery(state.get());
      return;
    }
    if (callback->Done()) {
      callback->Release();
      return;  // completed inline inside the synchronous grant
    }
    // S_OK without the callback having run is not a success and not a wait; the read is
    // unusable, exactly as in the P3.7 session.
    ++state->counters->read_session_failures;
    state->facts.input_scope.presence = FactPresence::kQueryFailed;
    callback->Release();
    CompleteQuery(state.get());
  }

  TfClientId client_id_ = 0;
  std::shared_ptr<PermissionQueryDiagnostics> counters_;
  std::shared_ptr<QueryState> active_;
};

}  // namespace

const char* DescribePermissionQueryStatus(PermissionQueryStatus status) {
  switch (status) {
    case PermissionQueryStatus::kIdle:
      return "Idle";
    case PermissionQueryStatus::kPending:
      return "Pending";
    case PermissionQueryStatus::kReady:
      return "Ready";
  }
  return "UnknownQueryStatus";
}

PermissionFacts ReadPermissionFactsWithCookie(ITfContext* context,
                                              TfEditCookie edit_cookie) {
  PermissionFacts facts;
  if (context == nullptr) {
    facts.status.presence = FactPresence::kQueryFailed;
    facts.keyboard_disabled.presence = FactPresence::kQueryFailed;
    facts.empty_context.presence = FactPresence::kQueryFailed;
    facts.input_scope.presence = FactPresence::kQueryFailed;
    return facts;
  }
  ReadStatusAndCompartments(context, &facts, nullptr);

  ITfReadOnlyProperty* property = nullptr;
  const HRESULT property_hr = context->GetAppProperty(GUID_PROP_INPUTSCOPE, &property);
  if (property_hr == S_FALSE || property_hr == E_NOTIMPL) {
    facts.input_scope.presence = FactPresence::kAbsent;
    return facts;
  }
  if (FAILED(property_hr) || property == nullptr) {
    facts.input_scope.presence = FactPresence::kQueryFailed;
    return facts;
  }
  facts.input_scope =
      ReadScopeOverSelection(context, edit_cookie, property, nullptr);
  property->Release();
  return facts;
}

std::unique_ptr<PermissionSource> CreateRealPermissionSource(TfClientId client_id) {
  return std::unique_ptr<PermissionSource>(new RealPermissionSource(client_id));
}

}  // namespace context
}  // namespace tsf
}  // namespace epocanva
