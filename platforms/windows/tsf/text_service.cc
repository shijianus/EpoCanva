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
    client_id_ = 0;
    activation_flags_ = 0;
    active_ = false;
  }
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
#ifdef EPOCANVA_TSF_ENGINE_HOST
  EngineLease lease;
#endif
  {
    std::lock_guard<std::mutex> guard(state_mutex_);
    thread_manager = thread_manager_;
    thread_manager_ = nullptr;
#ifdef EPOCANVA_TSF_ENGINE_HOST
    lease = std::move(engine_lease_);
#endif
    client_id_ = 0;
    activation_flags_ = 0;
    active_ = false;
  }

  // The object mutex is not held here. Releasing the last lease stops and joins the
  // executor; releasing the thread-manager reference is ordinary COM.
#ifdef EPOCANVA_TSF_ENGINE_HOST
  (void)lease.Release();
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
#endif
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
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
  {
    std::lock_guard<std::mutex> guard(state_mutex_);
    old_thread_manager = thread_manager_;
    thread_manager_ = thread_manager;
    client_id_ = client_id;      // stored verbatim; see text_service.h for why
    activation_flags_ = flags;   // recorded this phase; no flag changes behaviour yet
    active_ = true;
#ifdef EPOCANVA_TSF_ENGINE_HOST
    old_lease = std::move(engine_lease_);
    engine_lease_ = std::move(new_lease);
#endif
  }

  // Both old resources are released after the successful commit, outside the object mutex.
  // The new lease is already held, so releasing the old one cannot be the last lease of a
  // successful reactivation.
#ifdef EPOCANVA_TSF_ENGINE_HOST
  (void)old_lease.Release();
#endif
  if (old_thread_manager != nullptr) {
    old_thread_manager->Release();
  }
  return S_OK;
}

}  // namespace tsf
}  // namespace epocanva
