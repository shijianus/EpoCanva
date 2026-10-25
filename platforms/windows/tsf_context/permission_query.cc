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
  ++counters->compartment_reads;
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

// --- shared query state -------------------------------------------------------------------

struct QueryState {
  explicit QueryState(TfClientId id) : client_id(id) {
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
  bool active = false;      // a query was begun and not abandoned
  bool completed = false;   // a verdict exists
  bool abandoned = false;   // terminal; a late callback publishes nothing
  PermissionFacts facts;
  PermissionDecision decision;
  HANDLE completion_event = nullptr;
  ITfEditSession* pending_callback = nullptr;  // owned extra reference while async
  PermissionQueryDiagnostics counters;
};

void CompleteQuery(QueryState* state) {
  state->decision = DecidePermission(state->facts);
  state->completed = true;
  ++state->counters.queries_completed;
  if (state->completion_event != nullptr) {
    ::SetEvent(state->completion_event);
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
    ++state_->counters.scope_property_reads;
    ReadSelectionScope(edit_cookie);
    CompleteQuery(state_.get());
    done_ = true;
    return S_OK;
  }

  bool Done() const { return done_; }

 private:
  // Reads the InputScope property over the current selection. The selection is where
  // typing would land, and it is the range the ITfInputScope documentation's own
  // example passes to GetValue.
  void ReadSelectionScope(TfEditCookie edit_cookie) {
    ScopeFact& fact = state_->facts.input_scope;
    TF_SELECTION selection = {};
    ULONG fetched = 0;
    const HRESULT selection_hr =
        state_->context->GetSelection(edit_cookie, TF_DEFAULT_SELECTION, 1, &selection,
                                      &fetched);
    if (FAILED(selection_hr) || fetched != 1 || selection.range == nullptr) {
      state_->counters.last_scope_stage = 1;
      state_->counters.last_scope_hr = selection_hr;
      fact.presence = FactPresence::kQueryFailed;
      return;
    }
    ITfRange* range = selection.range;  // owned by the TF_SELECTION we must release
    VARIANT value;
    ::VariantInit(&value);
    const HRESULT value_hr = property_->GetValue(edit_cookie, range, &value);
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
      fact.presence = FactPresence::kAbsent;
      return;
    }
    if (FAILED(value_hr)) {
      state_->counters.last_scope_stage = 2;
      state_->counters.last_scope_hr = value_hr;
      fact.presence = FactPresence::kQueryFailed;
      return;
    }
    if (value.vt == VT_EMPTY || value.vt == VT_NULL) {
      fact.presence = FactPresence::kAbsent;
      return;
    }
    if (value.vt != VT_UNKNOWN || value.punkVal == nullptr) {
      ::VariantClear(&value);
      fact.presence = FactPresence::kWrongType;
      return;
    }
    ITfInputScope* scope = nullptr;
    const HRESULT qi_hr =
        value.punkVal->QueryInterface(IID_ITfInputScope,
                                      reinterpret_cast<void**>(&scope));
    if (FAILED(qi_hr) || scope == nullptr) {
      state_->counters.last_scope_stage = 3;
      state_->counters.last_scope_hr = qi_hr;
      ::VariantClear(&value);
      fact.presence = FactPresence::kWrongType;
      return;
    }
    InputScope* delivered = nullptr;
    UINT count = 0;
    const HRESULT scopes_hr = scope->GetInputScopes(&delivered, &count);
    if (FAILED(scopes_hr)) {
      state_->counters.last_scope_stage = 4;
      state_->counters.last_scope_hr = scopes_hr;
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
  }

  std::atomic<LONG> reference_count_{1};
  std::shared_ptr<QueryState> state_;
  ITfReadOnlyProperty* property_ = nullptr;
  bool done_ = false;
};

// --- the real source ----------------------------------------------------------------------

class RealPermissionSource final : public PermissionSource {
 public:
  explicit RealPermissionSource(TfClientId client_id)
      : state_(std::make_shared<QueryState>(client_id)) {}

  ~RealPermissionSource() override { Abandon(); }

  bool Begin(ITfContext* context, std::uint64_t context_id,
             std::uint64_t generation) override {
    if (context == nullptr || context_id == 0 || generation == 0) {
      return false;
    }
    if (state_->active && !state_->completed) {
      return false;  // one query at a time; Abandon() ends the previous one
    }
    // Reset for a fresh query. The completion event from a previous query may still be
    // signalled; every waiter of THIS source re-reads Status() after Begin.
    state_->context_id = context_id;
    state_->generation = generation;
    state_->completed = false;
    state_->abandoned = false;
    state_->active = true;
    state_->decision = PermissionDecision{};
    state_->facts = PermissionFacts{};
    if (state_->completion_event != nullptr) {
      ::ResetEvent(state_->completion_event);
    }
    if (state_->pending_callback != nullptr) {
      state_->pending_callback->Release();
      state_->pending_callback = nullptr;
    }
    context->AddRef();
    state_->context = context;
    ++state_->counters.queries_begun;

    ReadStatusAndCompartments();

    // The InputScope application property is the only axis that needs an edit cookie.
    // GetAppProperty's S_FALSE and E_NOTIMPL are the documented "the context owner does
    // not support / does not implement" answers -- an officially absent property.
    ITfReadOnlyProperty* property = nullptr;
    const HRESULT property_hr =
        state_->context->GetAppProperty(GUID_PROP_INPUTSCOPE, &property);
    if (property_hr == S_FALSE || property_hr == E_NOTIMPL) {
      state_->facts.input_scope.presence = FactPresence::kAbsent;
      CompleteQuery(state_.get());
      return true;
    }
    if (FAILED(property_hr) || property == nullptr) {
      state_->facts.input_scope.presence = FactPresence::kQueryFailed;
      CompleteQuery(state_.get());
      return true;
    }

    RequestScopeRead(property);
    return true;
  }

  PermissionQueryStatus Status() const override {
    if (!state_->active || state_->abandoned) {
      return PermissionQueryStatus::kIdle;
    }
    return state_->completed ? PermissionQueryStatus::kReady
                             : PermissionQueryStatus::kPending;
  }

  PermissionQueryResult Result() const override {
    PermissionQueryResult result;
    result.context_id = state_->context_id;
    result.generation = state_->generation;
    if (state_->active && !state_->abandoned && state_->completed) {
      result.complete = true;
      result.decision = state_->decision;
    }
    return result;
  }

  HANDLE CompletionEvent() const override {
    if (state_->active && !state_->abandoned && !state_->completed) {
      return state_->completion_event;
    }
    return nullptr;
  }

  void Abandon() override {
    if (state_->abandoned || !state_->active) {
      return;
    }
    state_->abandoned = true;
    state_->active = false;
    ++state_->counters.queries_abandoned;
    if (state_->pending_callback != nullptr) {
      state_->pending_callback->Release();
      state_->pending_callback = nullptr;
    }
    if (state_->context != nullptr) {
      state_->context->Release();
      state_->context = nullptr;
    }
    if (state_->completion_event != nullptr) {
      // No waiter of an abandoned query may hang; a verdict will never arrive.
      ::SetEvent(state_->completion_event);
    }
  }

  PermissionQueryDiagnostics Diagnostics() const override { return state_->counters; }

 private:
  void ReadStatusAndCompartments() {
    ++state_->counters.status_reads;
    TF_STATUS status = {};
    if (SUCCEEDED(state_->context->GetStatus(&status))) {
      state_->facts.status.presence = FactPresence::kValue;
      state_->facts.status.read_only = (status.dwDynamicFlags & TF_SD_READONLY) != 0;
    } else {
      state_->facts.status.presence = FactPresence::kQueryFailed;
    }
    state_->facts.keyboard_disabled =
        ReadCompartment(state_->context, GUID_COMPARTMENT_KEYBOARD_DISABLED,
                        &state_->counters);
    state_->facts.empty_context =
        ReadCompartment(state_->context, GUID_COMPARTMENT_EMPTYCONTEXT,
                        &state_->counters);
  }

  // Requests the read session for the property read. A synchronous read grant is the
  // common case; when the document cannot grant it now, the documented TF_ES_ASYNC
  // fallback defers to the host's message loop and the completion event.
  void RequestScopeRead(ITfReadOnlyProperty* property) {
    ScopeReadCallback* callback = new ScopeReadCallback(state_, property);
    property->Release();  // the callback holds its own reference now
    property = nullptr;

    HRESULT session_hr = S_OK;
    HRESULT request_hr = state_->context->RequestEditSession(
        state_->client_id, callback, TF_ES_SYNC | TF_ES_READ, &session_hr);
    if (SUCCEEDED(request_hr) && session_hr == TF_E_SYNCHRONOUS) {
      ++state_->counters.read_sessions_async;
      request_hr = state_->context->RequestEditSession(
          state_->client_id, callback, TF_ES_ASYNC | TF_ES_READ, &session_hr);
    } else {
      ++state_->counters.read_sessions_sync;
    }

    if (FAILED(request_hr)) {
      ++state_->counters.read_session_failures;
      state_->facts.input_scope.presence = FactPresence::kQueryFailed;
      callback->Release();
      CompleteQuery(state_.get());
      return;
    }
    if (session_hr == TF_S_ASYNC) {
      // Pending. The callback, not this return, completes the query; the host pumps
      // messages and bounded-waits on CompletionEvent().
      state_->pending_callback = callback;  // takes the ownership reference
      return;
    }
    if (FAILED(session_hr)) {
      ++state_->counters.read_session_failures;
      state_->facts.input_scope.presence = FactPresence::kQueryFailed;
      callback->Release();
      CompleteQuery(state_.get());
      return;
    }
    if (callback->Done()) {
      callback->Release();
      return;  // completed inline inside the synchronous grant
    }
    // S_OK without the callback having run is not a success and not a wait; the read is
    // unusable, exactly as in the P3.7 session.
    ++state_->counters.read_session_failures;
    state_->facts.input_scope.presence = FactPresence::kQueryFailed;
    callback->Release();
    CompleteQuery(state_.get());
  }

  std::shared_ptr<QueryState> state_;
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

std::unique_ptr<PermissionSource> CreateRealPermissionSource(TfClientId client_id) {
  return std::unique_ptr<PermissionSource>(new RealPermissionSource(client_id));
}

}  // namespace context
}  // namespace tsf
}  // namespace epocanva
