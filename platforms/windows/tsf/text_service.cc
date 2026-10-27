// EpoCanva P3.6 -- the text service implementation. See text_service.h for the transactional
// activation contract, the abnormal-destruction rule and the deliberate scope limits.
#include "text_service.h"

#include <new>

#include "module_state.h"

#ifdef EPOCANVA_TSF_ENGINE_HOST
#include "engine_host.h"
#endif
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
#include "product_engine_config.h"
#endif
#ifdef EPOCANVA_TSF_PRODUCT_CONTROLLER
#include "text_service_controller.h"
#endif

namespace epocanva {
namespace tsf {

namespace {

#ifdef EPOCANVA_TSF_ENGINE_HOST
HRESULT LeaseFailureToHresult(EngineLeaseStatus status) {
  switch (status) {
    case EngineLeaseStatus::kAcquired:
      return S_OK;
    case EngineLeaseStatus::kStartFailed:
    case EngineLeaseStatus::kCleanupFailed:
      return E_FAIL;
    case EngineLeaseStatus::kInternalFailure:
      return E_OUTOFMEMORY;
    case EngineLeaseStatus::kInvalidConfig:
    case EngineLeaseStatus::kInvalidLease:
    case EngineLeaseStatus::kStopping:
    case EngineLeaseStatus::kFailed:
    case EngineLeaseStatus::kConfigMismatch:
      return E_UNEXPECTED;
  }
  return E_FAIL;
}
#endif

}  // namespace

EpoCanvaTextService::EpoCanvaTextService()
    : reference_count_(1),
      thread_manager_(nullptr),
      client_id_(0),
      activation_flags_(0),
      active_(false) {
  ModuleAddObject();
}

EpoCanvaTextService::~EpoCanvaTextService() {
  ITfThreadMgr* thread_manager = nullptr;
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
  void* abandoned_lease_lock = nullptr;
#endif
  {
    std::lock_guard<std::mutex> guard(state_mutex_);
    thread_manager = thread_manager_;
    thread_manager_ = nullptr;
#ifdef EPOCANVA_TSF_ENGINE_HOST
    if (engine_lease_.valid()) {
      // Reaching here means Release() destroyed an active service without Deactivate().
      // Never call StopAndJoin from this context: it may be the loader lock. Abandon makes
      // DllCanUnloadNow permanently S_FALSE instead of hiding the violation behind a wait.
      engine_lease_.Abandon();
    }
#endif
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
    abandoned_lease_lock = engine_lease_lock_;
    engine_lease_lock_ = nullptr;
#endif
    client_id_ = 0;
    activation_flags_ = 0;
    active_ = false;
  }
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
  if (abandoned_lease_lock != nullptr) {
    // The engine lease was abandoned, so the shared lock must not outlive this object.
    ::CloseHandle(static_cast<HANDLE>(abandoned_lease_lock));
  }
#endif
#ifdef EPOCANVA_TSF_PRODUCT_CONTROLLER
  {
    std::lock_guard<std::mutex> guard(controller_mutex_);
    if (controller_) {
      // Same abnormal contract: the controller owns an STA waker thread whose Stop()
      // joins. Releasing the owning pointer leaks the controller for the rest of the
      // process instead of joining a thread under an unknown loader-lock state.
      (void)controller_.release();
    }
  }
#endif
  if (thread_manager != nullptr) {
    thread_manager->Release();
  }
  ModuleReleaseObject();
}

HRESULT STDMETHODCALLTYPE EpoCanvaTextService::QueryInterface(REFIID riid,
                                                              void** object) {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (IsEqualIID(riid, __uuidof(IUnknown))) {
    // The IUnknown subobject; with this single-inheritance chain every supported
    // interface answers with the same address, so IUnknown identity is one fact here.
    *object = static_cast<ITfTextInputProcessor*>(this);
  } else if (IsEqualIID(riid, __uuidof(ITfTextInputProcessor))) {
    *object = static_cast<ITfTextInputProcessor*>(this);
  } else if (IsEqualIID(riid, __uuidof(ITfTextInputProcessorEx))) {
    *object = static_cast<ITfTextInputProcessorEx*>(this);
#ifdef EPOCANVA_TSF_PRODUCT_CONTROLLER
  } else if (IsEqualIID(riid, __uuidof(ITfKeyEventSink))) {
    *object = static_cast<ITfKeyEventSink*>(this);
#endif
  } else {
    return E_NOINTERFACE;  // *object stays null: nothing for the caller to release
  }
  AddRef();
  return S_OK;
}

ULONG STDMETHODCALLTYPE EpoCanvaTextService::AddRef() {
  return InterlockedIncrement(&reference_count_);
}

ULONG STDMETHODCALLTYPE EpoCanvaTextService::Release() {
  const LONG after = InterlockedDecrement(&reference_count_);
  if (after == 0) {
    delete this;
    return 0;
  }
  if (after < 0) {
    // Unreachable under correct pairing; see EpoCanvaClassFactory::Release for the
    // reasoning. The module counter is guarded independently in module_state.h.
    return 0;
  }
  return static_cast<ULONG>(after);
}

HRESULT STDMETHODCALLTYPE EpoCanvaTextService::Activate(ITfThreadMgr* thread_manager,
                                                        TfClientId client_id) {
  // Activate is ActivateEx with no extra flags: one internal path, two entry points.
  return ActivateInternal(thread_manager, client_id, 0);
}

HRESULT STDMETHODCALLTYPE EpoCanvaTextService::Deactivate() {
  ITfThreadMgr* thread_manager = nullptr;
  TfClientId client_id = 0;
#ifdef EPOCANVA_TSF_ENGINE_HOST
  EngineLease lease;
#endif
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
  void* lease_lock = nullptr;
#endif
#ifdef EPOCANVA_TSF_PRODUCT_CONTROLLER
  std::unique_ptr<product::ProductTextController> controller;
  bool key_sink_advised = false;
#endif
  {
    std::lock_guard<std::mutex> guard(state_mutex_);
    thread_manager = thread_manager_;
    thread_manager_ = nullptr;
    client_id = client_id_;
#ifdef EPOCANVA_TSF_ENGINE_HOST
    lease = std::move(engine_lease_);
#endif
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
    lease_lock = engine_lease_lock_;
    engine_lease_lock_ = nullptr;
#endif
    client_id_ = 0;
    activation_flags_ = 0;
    active_ = false;
  }
#ifdef EPOCANVA_TSF_PRODUCT_CONTROLLER
  {
    std::lock_guard<std::mutex> guard(controller_mutex_);
    controller = std::move(controller_);
    key_sink_advised = key_sink_advised_;
    key_sink_advised_ = false;
  }
  // Unadvise the key sink FIRST: no new callback may enter the controller while it is
  // being stopped, and the sink object is still alive for TSF's matching Release.
  if (key_sink_advised && thread_manager != nullptr && client_id != 0) {
    UnadviseKeySink(thread_manager, client_id);
  }
  if (controller) {
    controller->Stop();
    controller.reset();
  }
#endif

  // The object mutex is not held here. Releasing the last lease stops and joins the
  // executor; releasing the thread-manager reference is ordinary COM.
#ifdef EPOCANVA_TSF_ENGINE_HOST
  (void)lease.Release();
#endif
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
  if (lease_lock != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(lease_lock));
  }
#endif
  if (thread_manager != nullptr) {
    thread_manager->Release();
  }
  return S_OK;  // deactivating an inactive service is a successful no-op (documented)
}

HRESULT STDMETHODCALLTYPE EpoCanvaTextService::ActivateEx(ITfThreadMgr* thread_manager,
                                                          TfClientId client_id,
                                                          DWORD flags) {
  return ActivateInternal(thread_manager, client_id, flags);
}

HRESULT EpoCanvaTextService::ActivateInternal(ITfThreadMgr* thread_manager,
                                              TfClientId client_id, DWORD flags) {
  if (thread_manager == nullptr) {
    return E_INVALIDARG;
  }

#ifdef EPOCANVA_TSF_ENGINE_HOST
  // Acquire the engine environment BEFORE touching the current activation. No mutation of
  // this object happens on a failure, so a rejected Activate leaves the old activation
  // intact. The configuration is copied into the broker; the path storage is not shared.
  EngineLease new_lease;
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
  void* new_lease_lock = nullptr;
#endif
#ifdef EPOCANVA_TSF_PRODUCT_CONTROLLER
  std::unique_ptr<product::ProductTextController> new_controller;
  bool same_identity = false;
  bool advised_new = false;
#endif
  try {
    EngineHostConfig config;
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
    // One activation-scoped transaction: it re-reads the settings/deployment snapshot,
    // holds the cross-process start lock and installs the configuration Acquire must use.
    epocanva::product::EngineStartTransaction deployment;
      if (!deployment.Prepare(nullptr)) {
          return E_UNEXPECTED;
    }
  #endif
    if (!GetEngineHostConfig(&config)) {
      return E_UNEXPECTED;
    }
      const EngineLeaseStatus status =
        EngineHostBroker::Instance().Acquire(config, &new_lease);
      if (status != EngineLeaseStatus::kAcquired) {
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
      deployment.Abort();
#endif
      return LeaseFailureToHresult(status);
    }
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
    // The broker has finished starting and deploying. This verifies that a requested
    // deployment produced a compiled schema and records the exact snapshot; the settings
    // marker is retired only after that record. A missing artefact fails the activation.
      if (!deployment.Complete(nullptr)) {
          (void)new_lease.Release();
      return E_FAIL;
    }
    // W2-A-R: take ownership of the shared engine.lock handle for exactly as long as this
    // engine lease. Without this handoff the shared lock died with the transaction, so a
    // cross-process deployment could take the exclusive lock and delete the compiled output
    // underneath a live engine.
    new_lease_lock = deployment.TakeLeaseLock();
    if (new_lease_lock == nullptr) {
      (void)new_lease.Release();
      return E_UNEXPECTED;
    }
  #endif
#ifdef EPOCANVA_TSF_PRODUCT_CONTROLLER
    // Build the complete STA pipeline before touching the current activation. It subscribes
    // the observer (StrictTsf) but is not yet visible to the key sink.
      new_controller.reset(new product::ProductTextController());
    std::wstring start_why;
    product::ProductTextController::Options controller_options;
    controller_options.thread_manager = thread_manager;
    controller_options.client_id = client_id;
    controller_options.executor = new_lease.executor();
      if (!new_controller->Start(controller_options, &start_why)) {
          (void)new_lease.Release();
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
        if (new_lease_lock != nullptr) {
          ::CloseHandle(static_cast<HANDLE>(new_lease_lock));
          new_lease_lock = nullptr;
        }
#endif
      return HRESULT_FROM_WIN32(ERROR_NOT_READY);
    }
      {
      std::lock_guard<std::mutex> guard(state_mutex_);
      same_identity = active_ && thread_manager_ == thread_manager &&
                      client_id_ == client_id && key_sink_advised_;
    }
    if (!same_identity) {
      // Advise before the commit. A reactivation for another thread manager/client keeps
      // the old advice until this one succeeds, so a failure leaves the old service usable.
      const HRESULT advised = AdviseKeySink(thread_manager, client_id);
          if (FAILED(advised)) {
#ifdef EPOCANVA_TSF_DIRECT_SINK_TEST
        // Controlled test variant only: an unregistered TIP cannot advise a key sink, so
        // the test calls the sink directly through its QI. The product DLL never defines
        // this macro and still fails activation when the advice is refused.
        advised_new = false;
#else
        new_controller->Stop();
        (void)new_lease.Release();
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
        if (new_lease_lock != nullptr) {
          ::CloseHandle(static_cast<HANDLE>(new_lease_lock));
          new_lease_lock = nullptr;
        }
#endif
        return HRESULT_FROM_WIN32(ERROR_INVALID_FUNCTION);
#endif
      } else {
        advised_new = true;
      }
    }
#endif
  } catch (const std::bad_alloc&) {
#ifdef EPOCANVA_TSF_PRODUCT_CONTROLLER
    if (new_controller) {
      new_controller->Stop();
    }
#endif
#ifdef EPOCANVA_TSF_ENGINE_HOST
    (void)new_lease.Release();
#endif
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
    if (new_lease_lock != nullptr) {
      ::CloseHandle(static_cast<HANDLE>(new_lease_lock));
      new_lease_lock = nullptr;
    }
#endif
    return E_OUTOFMEMORY;
  } catch (...) {
#ifdef EPOCANVA_TSF_PRODUCT_CONTROLLER
    if (new_controller) {
      new_controller->Stop();
    }
#endif
#ifdef EPOCANVA_TSF_ENGINE_HOST
    (void)new_lease.Release();
#endif
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
    if (new_lease_lock != nullptr) {
      ::CloseHandle(static_cast<HANDLE>(new_lease_lock));
      new_lease_lock = nullptr;
    }
#endif
    return E_FAIL;
  }
#endif

  // Retain the new thread manager before releasing the old one. If both arguments name the
  // same object, the AddRef below happens before the old Release, so no dangling reference
  // is possible.
  thread_manager->AddRef();

  ITfThreadMgr* old_thread_manager = nullptr;
#ifdef EPOCANVA_TSF_ENGINE_HOST
  EngineLease old_lease;
#endif
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
  void* old_lease_lock = nullptr;
#endif
#ifdef EPOCANVA_TSF_PRODUCT_CONTROLLER
  std::unique_ptr<product::ProductTextController> old_controller;
  TfClientId old_client_id = 0;
  bool old_key_sink_advised = false;
#endif
  {
    std::lock_guard<std::mutex> guard(state_mutex_);
#ifdef EPOCANVA_TSF_PRODUCT_CONTROLLER
    old_client_id = client_id_;
#endif
    old_thread_manager = thread_manager_;
    thread_manager_ = thread_manager;
    client_id_ = client_id;      // stored verbatim; see text_service.h for why
    activation_flags_ = flags;   // recorded this phase; no flag changes behaviour yet
    active_ = true;
#ifdef EPOCANVA_TSF_ENGINE_HOST
    old_lease = std::move(engine_lease_);
    engine_lease_ = std::move(new_lease);
#endif
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
    old_lease_lock = engine_lease_lock_;
    engine_lease_lock_ = new_lease_lock;
    new_lease_lock = nullptr;
#endif
  }
#ifdef EPOCANVA_TSF_PRODUCT_CONTROLLER
  {
    std::lock_guard<std::mutex> guard(controller_mutex_);
    old_controller = std::move(controller_);
    controller_ = std::move(new_controller);
    old_key_sink_advised = key_sink_advised_;
#ifdef EPOCANVA_TSF_DIRECT_SINK_TEST
    key_sink_advised_ = advised_new || same_identity;
#else
    key_sink_advised_ = true;
#endif
  }
  // The old service object no longer receives callbacks through the sink: either the same
  // advice now reaches the new controller, or the old advice is removed here.
  if (!same_identity && old_key_sink_advised && old_thread_manager != nullptr &&
      old_client_id != 0) {
    UnadviseKeySink(old_thread_manager, old_client_id);
  }
  if (old_controller) {
    old_controller->Stop();
    old_controller.reset();
  }
#endif

  // Both old resources are released after the successful commit, outside the object mutex.
  // The new lease is already held, so releasing the old one cannot be the last lease of a
  // successful reactivation.
#ifdef EPOCANVA_TSF_ENGINE_HOST
  (void)old_lease.Release();
#endif
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
  if (old_lease_lock != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(old_lease_lock));
  }
#endif
  if (old_thread_manager != nullptr) {
    old_thread_manager->Release();
  }
  return S_OK;
}

#ifdef EPOCANVA_TSF_PRODUCT_CONTROLLER
HRESULT EpoCanvaTextService::AdviseKeySink(ITfThreadMgr* thread_manager,
                                           TfClientId client_id) {
  ITfKeystrokeMgr* keystroke_manager = nullptr;
  HRESULT query = thread_manager->QueryInterface(
      __uuidof(ITfKeystrokeMgr), reinterpret_cast<void**>(&keystroke_manager));
  if (FAILED(query) || keystroke_manager == nullptr) {
    return FAILED(query) ? query : E_NOINTERFACE;
  }
  const HRESULT advised = keystroke_manager->AdviseKeyEventSink(
      client_id, static_cast<ITfKeyEventSink*>(this), TRUE);
  keystroke_manager->Release();
  return advised;
}

void EpoCanvaTextService::UnadviseKeySink(ITfThreadMgr* thread_manager,
                                          TfClientId client_id) {
  ITfKeystrokeMgr* keystroke_manager = nullptr;
  if (FAILED(thread_manager->QueryInterface(
          __uuidof(ITfKeystrokeMgr), reinterpret_cast<void**>(&keystroke_manager))) ||
      keystroke_manager == nullptr) {
    return;
  }
  (void)keystroke_manager->UnadviseKeyEventSink(client_id);
  keystroke_manager->Release();
}

HRESULT STDMETHODCALLTYPE EpoCanvaTextService::OnSetFocus(BOOL foreground) {
  (void)foreground;
  // A TSF notification: no pumping and no engine or edit work from here.
  return S_OK;
}

HRESULT STDMETHODCALLTYPE EpoCanvaTextService::OnTestKeyDown(
    ITfContext* context, WPARAM wparam, LPARAM lparam, BOOL* eaten) {
  (void)context;
  if (eaten == nullptr) {
    return E_POINTER;
  }
  *eaten = FALSE;
  try {
    std::lock_guard<std::mutex> guard(controller_mutex_);
    if (controller_ && controller_->started()) {
      *eaten = controller_->PredictKey(wparam, lparam) ? TRUE : FALSE;
    }
  } catch (...) {
    *eaten = FALSE;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE EpoCanvaTextService::OnTestKeyUp(
    ITfContext* context, WPARAM wparam, LPARAM lparam, BOOL* eaten) {
  (void)context;
  (void)wparam;
  (void)lparam;
  if (eaten == nullptr) {
    return E_POINTER;
  }
  *eaten = FALSE;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE EpoCanvaTextService::OnKeyDown(ITfContext* context,
                                                         WPARAM wparam,
                                                         LPARAM lparam,
                                                         BOOL* eaten) {
  (void)context;
  if (eaten == nullptr) {
    return E_POINTER;
  }
  *eaten = FALSE;
  try {
    std::lock_guard<std::mutex> guard(controller_mutex_);
    if (controller_ && controller_->started()) {
      *eaten = controller_->HandleKeyDown(wparam, lparam, nullptr) ? TRUE : FALSE;
    }
  } catch (...) {
    *eaten = FALSE;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE EpoCanvaTextService::OnKeyUp(ITfContext* context,
                                                       WPARAM wparam,
                                                       LPARAM lparam,
                                                       BOOL* eaten) {
  (void)context;
  (void)wparam;
  (void)lparam;
  if (eaten == nullptr) {
    return E_POINTER;
  }
  // A key-up is never translated, dispatched or eaten.
  *eaten = FALSE;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE EpoCanvaTextService::OnPreservedKey(ITfContext* context,
                                                              REFGUID guid,
                                                              BOOL* eaten) {
  (void)context;
  (void)guid;
  if (eaten == nullptr) {
    return E_POINTER;
  }
  *eaten = FALSE;
  return S_OK;
}
#endif  // EPOCANVA_TSF_PRODUCT_CONTROLLER

}  // namespace tsf
}  // namespace epocanva
