// EpoCanva P3.7 -- implementation of the TSF edit-session executor.
#include "edit_session.h"

#include <atomic>
#include <mutex>
#include <utility>

#include "composition_writer.h"

namespace epocanva {
namespace tsf {
namespace edit {
namespace {

EditResult MakeBaseResult(const EditPlan& plan, std::uint64_t session_id) {
  EditResult result;
  result.context = plan.context();
  result.plan_sequence = plan.sequence();
  result.session_id = static_cast<std::uint32_t>(
      session_id > 0xFFFFFFFFu ? 0xFFFFFFFFu : session_id);
  return result;
}

void RecordStatusLocked(CompositionEditSessionDiagnostics* counters, EditStatus status) {
  switch (status) {
    case EditStatus::kApplied:
      ++counters->applied;
      break;
    case EditStatus::kRejected:
      ++counters->rejected;
      break;
    case EditStatus::kFailedBeforeMutation:
      ++counters->failed_before_mutation;
      break;
    case EditStatus::kFailedPossiblyMutated:
      ++counters->failed_possibly_mutated;
      break;
    case EditStatus::kFailedAfterMutation:
      ++counters->failed_after_mutation;
      break;
    case EditStatus::kStale:
      ++counters->stale;
      break;
    case EditStatus::kPending:
      break;
  }
}

bool IsGenerationMismatch(const ContextIdentity& plan, const ContextIdentity& bound) {
  return plan.context_id == bound.context_id &&
         plan.generation != bound.generation;
}

void MapRequestFailure(HRESULT request_hr, HRESULT phr_session_hr, EditResult* result) {
  result->request_hr = request_hr;
  result->phr_session_hr = phr_session_hr;
  const HRESULT effective = FAILED(request_hr) ? request_hr : phr_session_hr;
  result->operation_hr = effective;
  if (effective == TF_E_DISCONNECTED) {
    result->status = EditStatus::kRejected;
    result->reason = EditRejectReason::kContextDisconnected;
    return;
  }
  if (effective == TF_E_LOCKED) {
    result->status = EditStatus::kRejected;
    result->reason = EditRejectReason::kContextLocked;
    return;
  }
  if (effective == TF_E_SYNCHRONOUS) {
    result->status = EditStatus::kRejected;
    result->reason = EditRejectReason::kSyncUnavailable;
    return;
  }
  if (effective == TS_E_READONLY) {
    result->status = EditStatus::kRejected;
    result->reason = EditRejectReason::kWriteLockRefused;
    return;
  }
  if (FAILED(effective)) {
    result->status = EditStatus::kFailedBeforeMutation;
    result->reason = EditRejectReason::kRequestFailed;
    return;
  }
  // S_OK without a callback is not a success.
  result->status = EditStatus::kFailedBeforeMutation;
  result->reason = EditRejectReason::kRequestFailed;
  result->operation_hr = E_UNEXPECTED;
}

}  // namespace

struct CompositionEditSession::State {
  explicit State(TfClientId id) : client_id(id) {
    completion_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }

  ~State() {
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
    writer.ReleaseHeldObjects();
  }

  std::mutex mutex;
  TfClientId client_id = 0;
  ITfContext* context = nullptr;  // owned; kept across Invalidate for pending safety
  ContextIdentity identity{};
  bool bound = false;
  bool closed = false;
  bool retiring = false;
  // Set when Retirement begins while a plan was already accepted. That plan must reach a
  // terminal result (Stale) without writing, and only then may cleanup use a new session.
  bool abandon_pending = false;
  InputPermission permission = InputPermission::kUnknown;
  CompositionEditSession::PermissionRecheck permission_recheck;
  CompositionEditSession::TargetRecheck target_recheck;
  CompositionWriter writer;

  bool has_pending = false;
  std::uint64_t pending_session_id = 0;
  EditResult pending_result;
  HANDLE completion_event = nullptr;
  ITfEditSession* pending_callback = nullptr;  // owned extra reference
  std::uint64_t next_session_id = 0;
  CompositionEditSessionDiagnostics counters;
};

class EditSessionCallback final : public ITfEditSession {
 public:
  EditSessionCallback(std::shared_ptr<CompositionEditSession::State> state,
                      EditPlan plan, ITfContext* context, std::uint64_t session_id)
      : state_(std::move(state)),
        plan_(std::move(plan)),
        context_(context),
        session_id_(session_id) {
    context_->AddRef();
  }

  ~EditSessionCallback() {
    if (context_ != nullptr) {
      context_->Release();
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
    EditResult result = MakeBaseResult(plan_, session_id_);
    bool stale = false;
    EditRejectReason stale_reason = EditRejectReason::kNotBound;
    InputPermission permission = InputPermission::kUnknown;
    CompositionEditSession::PermissionRecheck recheck;
    CompositionEditSession::TargetRecheck target_recheck;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      ++state_->counters.callbacks;
      if (state_->closed) {
        stale = true;
        stale_reason = EditRejectReason::kNotBound;
      } else if (!state_->bound || state_->context != context_) {
        stale = true;
        stale_reason = EditRejectReason::kNotBound;
      } else if (plan_.context().context_id != state_->identity.context_id) {
        stale = true;
        stale_reason = EditRejectReason::kIdentityMismatch;
      } else if (plan_.context().generation != state_->identity.generation) {
        stale = true;
        stale_reason = EditRejectReason::kStaleGeneration;
      } else if (state_->abandon_pending &&
                 session_id_ == state_->pending_session_id) {
        // This accepted plan was abandoned by BeginRetirement. It reaches a normal Stale
        // terminal (through the same publish path) and performs no TSF call.
        stale = true;
        stale_reason = EditRejectReason::kNotBound;
      } else if (state_->pending_session_id != session_id_) {
        stale = true;
        stale_reason = EditRejectReason::kStaleGeneration;
      } else if (state_->retiring &&
                 plan_.operation() != EditOperation::kEndOwnedComposition) {
        // Only the retirement cleanup may run once the binding has been retired.
        stale = true;
        stale_reason = EditRejectReason::kNotBound;
      } else {
        permission = state_->permission;
        recheck = state_->permission_recheck;
        target_recheck = state_->target_recheck;
      }
    }

    // P3.13: the target authority runs BEFORE the permission read and before any TSF write.
    // It re-reads the real thread focus/GetFocus/GetTop/generation inside the granted lock,
    // so a focus change during an asynchronous wait cannot be papered over by a permission
    // answer or by a plan that was valid at submission time. Retirement cleanup is exempt.
    bool target_checked = false;
    bool target_current = true;
    if (!stale && plan_.operation() != EditOperation::kEndOwnedComposition &&
        target_recheck) {
      target_checked = true;
      try {
        target_current =
            target_recheck(context_, plan_.context().context_id,
                           plan_.context().generation);
      } catch (...) {
        target_current = false;
      }
    }

    // The callback-time permission recheck runs outside the state mutex and only for an
    // otherwise admissible plan. It is authoritative: the cached permission can be older
    // than the field's current metadata (the lock may have been deferred after the plan was
    // built). A stale target short-circuits it: no permission read is made for a plan that
    // may not write.
    bool recheck_ran = false;
    if (!stale && target_current && permission == InputPermission::kAllowed && recheck) {
      recheck_ran = true;
      try {
        permission = recheck(context_, edit_cookie);
      } catch (...) {
        permission = InputPermission::kUnknown;
      }
    }

    HRESULT session_hr = S_OK;
    if (stale) {
      result.status = EditStatus::kStale;
      result.reason = stale_reason;
    } else if (!target_current) {
      result.status = EditStatus::kRejected;
      result.reason = EditRejectReason::kTargetChanged;
    } else if (permission != InputPermission::kAllowed) {
      result.status = EditStatus::kRejected;
      result.reason = permission == InputPermission::kDenied
                          ? EditRejectReason::kPermissionDenied
                          : EditRejectReason::kPermissionUnknown;
    } else {
      // Re-check read-only state at the last possible moment: the host can change it between
      // submission and the granted lock.
      TF_STATUS status = {};
      const HRESULT status_hr = context_->GetStatus(&status);
      if (FAILED(status_hr)) {
        result.status = EditStatus::kRejected;
        result.reason = status_hr == TF_E_DISCONNECTED
                            ? EditRejectReason::kContextDisconnected
                            : EditRejectReason::kContextStatusFailed;
        result.operation_hr = status_hr;
      } else if ((status.dwDynamicFlags & TF_SD_READONLY) != 0) {
        result.status = EditStatus::kRejected;
        result.reason = EditRejectReason::kReadOnlyContext;
      } else {
        const CompositionWriteOutcome outcome = state_->writer.Apply(plan_, context_,
                                                                     edit_cookie);
        result.status = outcome.status;
        result.reason = outcome.reason;
        result.operation_hr = outcome.operation_hr;
        result.end_composition_hr = outcome.end_composition_hr;
        result.cleanup_end_composition = outcome.cleanup_end_composition;
        result.composition_start_utf16 = outcome.composition_start_utf16;
        result.composition_length_utf16 = outcome.composition_length_utf16;
        result.text_may_have_changed = outcome.text_may_have_changed;
        result.mutation_verified = outcome.mutation_verified;
        result.composition_active = outcome.composition_active;
        result.lock_observed = true;
        if (outcome.status == EditStatus::kFailedBeforeMutation ||
            outcome.status == EditStatus::kFailedAfterMutation) {
          session_hr = FAILED(outcome.end_composition_hr)
                           ? outcome.end_composition_hr
                           : outcome.operation_hr;
          if (!FAILED(session_hr)) {
            session_hr = E_FAIL;
          }
        }
      }
    }
    result.edit_session_hr = session_hr;

    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      if (result.lock_observed) {
        ++state_->counters.locks_observed;
      }
      if (recheck_ran) {
        ++state_->counters.permission_rechecks;
      }
      if (target_checked) {
        ++state_->counters.target_callback_checks;
        if (!target_current) {
          ++state_->counters.target_rejections;
        }
      }
      result_ = result;
      done_ = true;
      if (state_->has_pending && state_->pending_session_id == session_id_) {
        if (state_->abandon_pending) {
          state_->abandon_pending = false;
        }
        // Preserve the submission's request_hr / TF_S_ASYNC phrase in the final result: the
        // callback's own result carries the edit outcome, not the waiting phrase.
        result.request_hr = state_->pending_result.request_hr;
        result.phr_session_hr = state_->pending_result.phr_session_hr;
        result.session_id = state_->pending_result.session_id;
        state_->pending_result = result;
        state_->has_pending = false;
        state_->pending_session_id = 0;
        RecordStatusLocked(&state_->counters, result.status);
        if (state_->completion_event != nullptr) {
          ::SetEvent(state_->completion_event);
        }
      }
    }
    return session_hr;
  }

  EditResult Result() const { return result_; }
  bool Done() const { return done_; }
  HRESULT DoEditSessionHr() const { return result_.edit_session_hr; }

 private:
  std::atomic<LONG> reference_count_{1};
  std::shared_ptr<CompositionEditSession::State> state_;
  EditPlan plan_;
  ITfContext* context_ = nullptr;
  std::uint64_t session_id_ = 0;
  EditResult result_;
  bool done_ = false;
};

const char* DescribeInputPermission(InputPermission permission) {
  switch (permission) {
    case InputPermission::kAllowed:
      return "Allowed";
    case InputPermission::kDenied:
      return "Denied";
    case InputPermission::kUnknown:
      return "Unknown";
  }
  return "UnknownPermission";
}

CompositionEditSession::CompositionEditSession(TfClientId client_id)
    : state_(std::make_shared<State>(client_id)) {}

CompositionEditSession::~CompositionEditSession() {
  Close();
  state_.reset();
}

bool CompositionEditSession::Bind(ITfContext* context, std::uint64_t context_id,
                                  std::uint64_t generation) {
  if (context == nullptr || context_id == 0 || generation == 0) {
    return false;
  }
  const std::shared_ptr<State> state = state_;
  context->AddRef();
  ITfContext* previous = nullptr;
  bool accepted = false;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    // A session whose last write outcome could not be confirmed stays terminal: re-binding it
    // (even to a different context) would silently continue editing around unknown text. The
    // recovery path is a new CompositionEditSession.
    if (!state->closed && !state->has_pending && !state->writer.active() &&
        !state->writer.HasUncertainOutcome()) {
      previous = state->context;
      state->context = context;
      state->identity.context_id = context_id;
      state->identity.generation = generation;
      state->bound = true;
      state->permission = InputPermission::kUnknown;
      state->permission_recheck = CompositionEditSession::PermissionRecheck{};
      state->target_recheck = CompositionEditSession::TargetRecheck{};
      accepted = true;
    }
  }
  if (!accepted) {
    context->Release();
    return false;
  }
  if (previous != nullptr) {
    previous->Release();
  }
  return true;
}

void CompositionEditSession::Invalidate() {
  const std::shared_ptr<State> state = state_;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->closed) {
    return;
  }
  state->bound = false;
  if (state->identity.generation == 0xFFFFFFFFFFFFFFFFull) {
    state->identity.generation = 1;
  } else {
    ++state->identity.generation;
  }
}

void CompositionEditSession::BeginRetirement() {
  const std::shared_ptr<State> state = state_;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->closed) {
    return;
  }
  state->retiring = true;
  if (state->has_pending) {
    // The accepted plan is not cancelled here: it must still reach one terminal result.
    // The callback sees abandon_pending and returns Stale without any TSF call; cleanup
    // waits for that terminal before submitting its own EndOwnedComposition session.
    state->abandon_pending = true;
  }
}

bool CompositionEditSession::HasUncertainOutcome() const {
  const std::shared_ptr<State> state = state_;
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->writer.HasUncertainOutcome();
}

void CompositionEditSession::Close() {
  const std::shared_ptr<State> state = state_;
  ITfContext* context = nullptr;
  ITfEditSession* callback = nullptr;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed) {
      return;
    }
    state->closed = true;
    state->bound = false;
    context = state->context;
    state->context = nullptr;
    callback = state->pending_callback;
    state->pending_callback = nullptr;
  }
  state->writer.ReleaseHeldObjects();
  if (context != nullptr) {
    context->Release();
  }
  if (callback != nullptr) {
    callback->Release();
  }
}

void CompositionEditSession::SetInputPermission(InputPermission permission) {
  const std::shared_ptr<State> state = state_;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (!state->closed) {
    state->permission = permission;
  }
}

void CompositionEditSession::SetPermissionRecheck(PermissionRecheck recheck) {
  const std::shared_ptr<State> state = state_;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (!state->closed) {
    state->permission_recheck = std::move(recheck);
  }
}

void CompositionEditSession::SetTargetRecheck(TargetRecheck recheck) {
  const std::shared_ptr<State> state = state_;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (!state->closed) {
    state->target_recheck = std::move(recheck);
  }
}

InputPermission CompositionEditSession::input_permission() const {
  const std::shared_ptr<State> state = state_;
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->permission;
}

bool CompositionEditSession::bound() const {
  const std::shared_ptr<State> state = state_;
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->bound;
}

ContextIdentity CompositionEditSession::identity() const {
  const std::shared_ptr<State> state = state_;
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->identity;
}

bool CompositionEditSession::Precheck(const EditPlan& plan, ITfContext** context,
                                      EditResult* rejection) {
  const std::shared_ptr<State> state = state_;
  *context = nullptr;
  *rejection = MakeBaseResult(plan, 0);
  CompositionEditSession::TargetRecheck target_recheck;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    ++state->counters.apply_calls;
    if (state->closed) {
      rejection->status = EditStatus::kStale;
      rejection->reason = EditRejectReason::kNotBound;
      RecordStatusLocked(&state->counters, rejection->status);
      return false;
    }
    if (!state->bound || state->context == nullptr) {
      if (plan.context() != state->identity) {
        rejection->status = EditStatus::kStale;
        rejection->reason = IsGenerationMismatch(plan.context(), state->identity)
                                ? EditRejectReason::kStaleGeneration
                                : EditRejectReason::kIdentityMismatch;
      } else {
        rejection->status = EditStatus::kRejected;
        rejection->reason = EditRejectReason::kNotBound;
      }
      RecordStatusLocked(&state->counters, rejection->status);
      return false;
    }
    if (plan.context().context_id != state->identity.context_id) {
      rejection->status = EditStatus::kStale;
      rejection->reason = EditRejectReason::kIdentityMismatch;
      RecordStatusLocked(&state->counters, rejection->status);
      return false;
    }
    if (plan.context().generation != state->identity.generation) {
      rejection->status = EditStatus::kStale;
      rejection->reason = EditRejectReason::kStaleGeneration;
      RecordStatusLocked(&state->counters, rejection->status);
      return false;
    }
    if (state->retiring &&
        plan.operation() != EditOperation::kEndOwnedComposition) {
      // A retired binding accepts no normal plan, even before any TSF call.
      rejection->status = EditStatus::kStale;
      rejection->reason = EditRejectReason::kNotBound;
      RecordStatusLocked(&state->counters, rejection->status);
      return false;
    }
    if (state->writer.HasUncertainOutcome()) {
      // A previous write's effect on the backing text was never confirmed. No lock is
      // requested and no plan (Update/Commit/Cancel/Begin alike) runs on this session again;
      // the caller must Close() it and start a new one.
      rejection->status = EditStatus::kRejected;
      rejection->reason = EditRejectReason::kUncertainTextState;
      RecordStatusLocked(&state->counters, rejection->status);
      return false;
    }
    if (state->has_pending) {
      rejection->status = EditStatus::kRejected;
      rejection->reason = EditRejectReason::kBusy;
      RecordStatusLocked(&state->counters, rejection->status);
      return false;
    }
    if (state->permission != InputPermission::kAllowed) {
      rejection->status = EditStatus::kRejected;
      rejection->reason = state->permission == InputPermission::kDenied
                              ? EditRejectReason::kPermissionDenied
                              : EditRejectReason::kPermissionUnknown;
      RecordStatusLocked(&state->counters, rejection->status);
      return false;
    }
    state->context->AddRef();
    *context = state->context;
    target_recheck = state->target_recheck;
  }

  // P3.13: reject a plan whose target is already gone before even requesting a lock. The
  // callback repeats this check under the granted lock; this precheck simply avoids a
  // pointless TSF request. Retirement cleanup is exempt (it may run on an unfocused
  // context the writer still owns).
  if (plan.operation() != EditOperation::kEndOwnedComposition && target_recheck) {
    bool current = true;
    try {
      current = target_recheck(*context, plan.context().context_id,
                               plan.context().generation);
    } catch (...) {
      current = false;
    }
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      ++state->counters.target_prechecks;
      if (!current) {
        ++state->counters.target_rejections;
      }
    }
    if (!current) {
      (*context)->Release();
      *context = nullptr;
      rejection->status = EditStatus::kRejected;
      rejection->reason = EditRejectReason::kTargetChanged;
      std::lock_guard<std::mutex> lock(state->mutex);
      RecordStatusLocked(&state->counters, rejection->status);
      return false;
    }
  }

  TF_STATUS status = {};
  const HRESULT status_hr = (*context)->GetStatus(&status);
  if (FAILED(status_hr)) {
    (*context)->Release();
    *context = nullptr;
    rejection->status = EditStatus::kRejected;
    rejection->reason = status_hr == TF_E_DISCONNECTED
                            ? EditRejectReason::kContextDisconnected
                            : EditRejectReason::kContextStatusFailed;
    rejection->operation_hr = status_hr;
    std::lock_guard<std::mutex> lock(state->mutex);
    RecordStatusLocked(&state->counters, rejection->status);
    return false;
  }
  if ((status.dwDynamicFlags & TF_SD_READONLY) != 0) {
    (*context)->Release();
    *context = nullptr;
    rejection->status = EditStatus::kRejected;
    rejection->reason = EditRejectReason::kReadOnlyContext;
    std::lock_guard<std::mutex> lock(state->mutex);
    RecordStatusLocked(&state->counters, rejection->status);
    return false;
  }
  return true;
}

EditResult CompositionEditSession::Submit(const EditPlan& plan, bool asynchronous) {
  const std::shared_ptr<State> state = state_;
  ITfContext* context = nullptr;
  EditResult rejection;
  if (!Precheck(plan, &context, &rejection)) {
    return rejection;
  }
  if (state->completion_event == nullptr) {
    context->Release();
    rejection = MakeBaseResult(plan, 0);
    rejection.status = EditStatus::kRejected;
    rejection.reason = EditRejectReason::kRequestFailed;
    rejection.operation_hr = E_UNEXPECTED;
    return rejection;
  }

  std::uint64_t session_id = 0;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    session_id = ++state->next_session_id;
    state->has_pending = true;
    state->pending_session_id = session_id;
    state->pending_result = MakeBaseResult(plan, session_id);
    state->pending_result.status = EditStatus::kPending;
    if (asynchronous) {
      ++state->counters.async_requests;
    } else {
      ++state->counters.sync_requests;
    }
    ::ResetEvent(state->completion_event);
  }

  EditSessionCallback* callback =
      new EditSessionCallback(state, plan, context, session_id);
  HRESULT phr_session = S_OK;
  const DWORD flags = TF_ES_READWRITE | (asynchronous ? TF_ES_ASYNC : TF_ES_SYNC);
  const HRESULT request_hr = context->RequestEditSession(
      state->client_id, callback, flags, &phr_session);
  context->Release();
  context = nullptr;

  if (FAILED(request_hr)) {
    EditResult failure = MakeBaseResult(plan, session_id);
    MapRequestFailure(request_hr, phr_session, &failure);
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      ++state->counters.request_failures;
      state->has_pending = false;
      state->pending_session_id = 0;
      state->pending_result = failure;
      RecordStatusLocked(&state->counters, failure.status);
    }
    callback->Release();
    return failure;
  }

  if (phr_session == TF_S_ASYNC) {
    // Waiting. The callback, not this return, establishes whether text was applied.
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->pending_callback = callback;
      callback->AddRef();
      state->pending_result = MakeBaseResult(plan, session_id);
      state->pending_result.status = EditStatus::kPending;
      state->pending_result.request_hr = request_hr;
      state->pending_result.phr_session_hr = phr_session;
    }
    EditResult pending = MakeBaseResult(plan, session_id);
    pending.status = EditStatus::kPending;
    pending.request_hr = request_hr;
    pending.phr_session_hr = phr_session;
    return pending;
  }

  const bool callback_ran = callback->Done();
  EditResult final;
  if (callback_ran) {
    final = callback->Result();
    final.request_hr = request_hr;
    final.phr_session_hr = phr_session;
    final.edit_session_hr = callback->DoEditSessionHr();
  } else {
    final = MakeBaseResult(plan, session_id);
    MapRequestFailure(request_hr, phr_session, &final);
  }
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->has_pending = false;
    state->pending_session_id = 0;
    state->pending_result = final;
    if (!callback_ran) {
      // The callback publishes (and counts) its own result. Only a result with no callback
      // has to be counted here; counting twice would inflate the status evidence.
      RecordStatusLocked(&state->counters, final.status);
    }
  }
  callback->Release();
  return final;
}

EditResult CompositionEditSession::ApplySync(const EditPlan& plan) {
  return Submit(plan, /*asynchronous=*/false);
}

EditResult CompositionEditSession::ApplyAsync(const EditPlan& plan) {
  return Submit(plan, /*asynchronous=*/true);
}

EditResult CompositionEditSession::PendingResult() const {
  const std::shared_ptr<State> state = state_;
  EditResult result;
  ITfEditSession* callback = nullptr;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    result = state->pending_result;
    if (!state->has_pending && state->pending_callback != nullptr) {
      callback = state->pending_callback;
      state->pending_callback = nullptr;
    }
  }
  if (callback != nullptr) {
    callback->Release();
  }
  return result;
}

bool CompositionEditSession::HasPending() const {
  const std::shared_ptr<State> state = state_;
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->has_pending;
}

HANDLE CompositionEditSession::CompletionEvent() const {
  return state_->completion_event;
}

CompositionEditSessionDiagnostics CompositionEditSession::Snapshot() const {
  const std::shared_ptr<State> state = state_;
  CompositionEditSessionDiagnostics snapshot;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    snapshot = state->counters;
    snapshot.bound = state->bound;
    snapshot.closed = state->closed;
    snapshot.pending = state->has_pending;
    snapshot.context_id = state->identity.context_id;
    snapshot.generation = state->identity.generation;
    snapshot.permission = state->permission;
  }
  snapshot.active_composition = state->writer.active();
  snapshot.composition_start_utf16 = state->writer.composition_start_utf16();
  snapshot.composition_length_utf16 = state->writer.composition_length_utf16();
  return snapshot;
}

bool CompositionEditSession::active_composition() const {
  return state_->writer.active();
}

}  // namespace edit
}  // namespace tsf
}  // namespace epocanva
