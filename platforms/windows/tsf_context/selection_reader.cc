// EpoCanva P3.10 -- implementation of real selection snapshots. The contract (accepted
// shapes, units, cookie sources, refusal statuses) is in selection_reader.h; this file
// follows it and adds no fallback.
#include "selection_reader.h"

#include <atomic>
#include <utility>

namespace epocanva {
namespace tsf {
namespace context {

const char* DescribeSelectionReadStatus(SelectionReadStatus status) {
  switch (status) {
    case SelectionReadStatus::kOk:
      return "Ok";
    case SelectionReadStatus::kNoSelection:
      return "NoSelection";
    case SelectionReadStatus::kNonCollapsed:
      return "NonCollapsed";
    case SelectionReadStatus::kUnsupportedShape:
      return "UnsupportedShape";
    case SelectionReadStatus::kSessionUnavailable:
      return "SessionUnavailable";
    case SelectionReadStatus::kContextUnavailable:
      return "ContextUnavailable";
    case SelectionReadStatus::kClosed:
      return "Closed";
  }
  return "UnknownSelectionStatus";
}

namespace {

SelectionReadStatus StatusForGetSelectionHr(HRESULT hr) {
  if (hr == TF_E_NOSELECTION) {
    return SelectionReadStatus::kNoSelection;
  }
  if (hr == TF_E_DISCONNECTED) {
    return SelectionReadStatus::kContextUnavailable;
  }
  // TF_E_NOLOCK and every other failure: the cookie/session is not usable, so there is no
  // reliable position. Distinct from a non-collapsed selection, which was read reliably.
  return SelectionReadStatus::kSessionUnavailable;
}

}  // namespace

SelectionSnapshot ReadSelectionWithCookie(ITfContext* context,
                                          TfEditCookie edit_cookie) {
  SelectionSnapshot snapshot;
  if (context == nullptr) {
    snapshot.status = SelectionReadStatus::kContextUnavailable;
    return snapshot;
  }
  TF_SELECTION selection = {};
  ULONG fetched = 0;
  const HRESULT get_hr = context->GetSelection(edit_cookie, TF_DEFAULT_SELECTION, 1,
                                               &selection, &fetched);
  if (FAILED(get_hr)) {
    snapshot.status = StatusForGetSelectionHr(get_hr);
    return snapshot;
  }
  if (fetched != 1 || selection.range == nullptr) {
    if (selection.range != nullptr) {
      selection.range->Release();
    }
    snapshot.status = SelectionReadStatus::kNoSelection;
    return snapshot;
  }
  ITfRange* range = selection.range;  // owned by the TF_SELECTION we must release
  // Only the default selection with an identifiable active end is accepted. TF_AE_NONE is
  // documented as typical for non-default selections; guessing the active end here would
  // be an invented position.
  if (selection.style.fInterimChar ||
      (selection.style.ase != TF_AE_START && selection.style.ase != TF_AE_END)) {
    range->Release();
    snapshot.status = SelectionReadStatus::kUnsupportedShape;
    return snapshot;
  }
  ITfRangeACP* acp = nullptr;
  const HRESULT qi_hr =
      range->QueryInterface(IID_ITfRangeACP, reinterpret_cast<void**>(&acp));
  if (FAILED(qi_hr) || acp == nullptr) {
    range->Release();
    snapshot.status = SelectionReadStatus::kUnsupportedShape;
    return snapshot;
  }
  LONG anchor_utf16 = 0;
  LONG length_utf16 = 0;
  const HRESULT extent_hr = acp->GetExtent(&anchor_utf16, &length_utf16);
  acp->Release();
  range->Release();
  if (FAILED(extent_hr)) {
    snapshot.status = SelectionReadStatus::kSessionUnavailable;
    return snapshot;
  }
  if (length_utf16 != 0) {
    // Non-collapsed. A negative length is likewise not a collapsed caret; it is refused
    // rather than normalized.
    snapshot.status = SelectionReadStatus::kNonCollapsed;
    return snapshot;
  }
  if (anchor_utf16 < 0) {
    // An ACP anchor outside the document is not clamped to zero: an invented position is
    // worse than no position.
    snapshot.status = SelectionReadStatus::kUnsupportedShape;
    return snapshot;
  }
  snapshot.status = SelectionReadStatus::kOk;
  snapshot.caret_utf16 = anchor_utf16;
  return snapshot;
}

// --- the read-only edit session ------------------------------------------------------------

struct SelectionReader::ReadState {
  ReadState(TfClientId id, std::shared_ptr<SelectionReaderDiagnostics> shared_counters)
      : client_id(id), counters(std::move(shared_counters)) {
    completion_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }

  ~ReadState() {
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
  ITfContext* context = nullptr;  // owned while the read needs it
  bool active = false;            // begun and not abandoned
  bool completed = false;         // a snapshot exists
  bool abandoned = false;         // terminal; a late callback publishes nothing
  SelectionSnapshot snapshot;
  HANDLE completion_event = nullptr;
  ITfEditSession* pending_callback = nullptr;  // owned extra reference while async
  std::shared_ptr<SelectionReaderDiagnostics> counters;
};

class SelectionReadCallback final : public ITfEditSession {
 public:
  explicit SelectionReadCallback(std::shared_ptr<SelectionReader::ReadState> state)
      : state_(std::move(state)) {}
  ~SelectionReadCallback() = default;

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
      // The read was terminated while this session was queued. The shared state is
      // orphaned and no reader is waiting; publish nothing and touch no context.
      ++state_->counters->callbacks_after_abandon;
      done_ = true;
      return S_OK;
    }
    state_->snapshot = ReadSelectionWithCookie(state_->context, edit_cookie);
    Complete();
    done_ = true;
    return S_OK;
  }

  bool Done() const { return done_; }

 private:
  void Complete() {
    state_->completed = true;
    ++state_->counters->reads_completed;
    state_->counters->last_status =
        static_cast<int>(state_->snapshot.status);
    switch (state_->snapshot.status) {
      case SelectionReadStatus::kOk:
        ++state_->counters->collapsed;
        break;
      case SelectionReadStatus::kNoSelection:
        ++state_->counters->no_selection;
        break;
      case SelectionReadStatus::kNonCollapsed:
        ++state_->counters->non_collapsed;
        break;
      case SelectionReadStatus::kUnsupportedShape:
        ++state_->counters->unsupported_shape;
        break;
      case SelectionReadStatus::kSessionUnavailable:
      case SelectionReadStatus::kContextUnavailable:
      case SelectionReadStatus::kClosed:
        ++state_->counters->unavailable;
        break;
    }
    if (state_->completion_event != nullptr) {
      ::SetEvent(state_->completion_event);
    }
    if (state_->context != nullptr) {
      state_->context->Release();
      state_->context = nullptr;
    }
  }

  std::atomic<LONG> reference_count_{1};
  std::shared_ptr<SelectionReader::ReadState> state_;
  bool done_ = false;
};

SelectionReader::SelectionReader(TfClientId client_id)
    : counters_(std::make_shared<SelectionReaderDiagnostics>()),
      client_id_(client_id) {}

SelectionReader::~SelectionReader() { Abandon(); }

bool SelectionReader::Begin(ITfContext* context) {
  if (context == nullptr) {
    return false;
  }
  if (active_ != nullptr && active_->active && !active_->completed &&
      !active_->abandoned) {
    return false;  // one active read; Abandon() or Take() ends it
  }
  active_ = std::make_shared<ReadState>(client_id_, counters_);
  ReadState* state = active_.get();
  state->active = true;
  context->AddRef();
  state->context = context;
  ++counters_->reads_begun;
  if (state->completion_event != nullptr) {
    ::ResetEvent(state->completion_event);
  }

  SelectionReadCallback* callback = new SelectionReadCallback(active_);
  HRESULT session_hr = S_OK;
  HRESULT request_hr = context->RequestEditSession(
      client_id_, callback, TF_ES_SYNC | TF_ES_READ, &session_hr);
  if (SUCCEEDED(request_hr) && session_hr == TF_E_SYNCHRONOUS) {
    ++counters_->read_sessions_async;
    request_hr = context->RequestEditSession(client_id_, callback,
                                             TF_ES_ASYNC | TF_ES_READ, &session_hr);
  } else if (SUCCEEDED(request_hr)) {
    ++counters_->read_sessions_sync;
  }

  if (FAILED(request_hr)) {
    ++counters_->read_session_failures;
    state->snapshot.status = SelectionReadStatus::kSessionUnavailable;
    state->completed = true;
    ++counters_->reads_completed;
    counters_->last_status = static_cast<int>(state->snapshot.status);
    ++counters_->unavailable;
    if (state->completion_event != nullptr) {
      ::SetEvent(state->completion_event);
    }
    if (state->context != nullptr) {
      state->context->Release();
      state->context = nullptr;
    }
    callback->Release();
    return true;
  }
  if (session_hr == TF_S_ASYNC) {
    // Pending. The callback, not this return, completes the read; the host pumps messages
    // and bounded-waits on CompletionEvent().
    state->pending_callback = callback;  // takes the ownership reference
    return true;
  }
  if (FAILED(session_hr)) {
    ++counters_->read_session_failures;
    state->snapshot.status = SelectionReadStatus::kSessionUnavailable;
    state->completed = true;
    ++counters_->reads_completed;
    counters_->last_status = static_cast<int>(state->snapshot.status);
    ++counters_->unavailable;
    if (state->completion_event != nullptr) {
      ::SetEvent(state->completion_event);
    }
    if (state->context != nullptr) {
      state->context->Release();
      state->context = nullptr;
    }
    callback->Release();
    return true;
  }
  if (callback->Done()) {
    callback->Release();
    return true;  // completed inline inside the synchronous grant
  }
  // S_OK without the callback having run is neither a result nor a wait; the read is
  // unusable. No invented position follows from it.
  ++counters_->read_session_failures;
  state->snapshot.status = SelectionReadStatus::kSessionUnavailable;
  state->completed = true;
  ++counters_->reads_completed;
  counters_->last_status = static_cast<int>(state->snapshot.status);
  ++counters_->unavailable;
  if (state->completion_event != nullptr) {
    ::SetEvent(state->completion_event);
  }
  if (state->context != nullptr) {
    state->context->Release();
    state->context = nullptr;
  }
  callback->Release();
  return true;
}

bool SelectionReader::Pending() const {
  return active_ != nullptr && active_->active && !active_->completed &&
         !active_->abandoned;
}

HANDLE SelectionReader::CompletionEvent() const {
  if (Pending()) {
    return active_->completion_event;
  }
  return nullptr;
}

SelectionSnapshot SelectionReader::Take() {
  if (active_ == nullptr || !active_->completed || active_->abandoned) {
    return SelectionSnapshot{};
  }
  const SelectionSnapshot snapshot = active_->snapshot;
  ++counters_->reads_taken;
  active_.reset();
  return snapshot;
}

void SelectionReader::Abandon() {
  if (active_ == nullptr) {
    return;
  }
  if (active_->completed || active_->abandoned) {
    active_.reset();
    return;
  }
  active_->abandoned = true;
  active_->active = false;
  ++counters_->reads_abandoned;
  if (active_->pending_callback != nullptr) {
    active_->pending_callback->Release();
    active_->pending_callback = nullptr;
  }
  if (active_->context != nullptr) {
    active_->context->Release();
    active_->context = nullptr;
  }
  if (active_->completion_event != nullptr) {
    // No waiter may hang on a read that will never answer.
    ::SetEvent(active_->completion_event);
  }
  // The state is dropped, not reused: a late callback keeps it alive through its own
  // shared_ptr and can only write into this orphaned object.
  active_.reset();
}

SelectionReaderDiagnostics SelectionReader::Diagnostics() const {
  return *counters_;
}

}  // namespace context
}  // namespace tsf
}  // namespace epocanva
