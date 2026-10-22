// EpoCanva P3.1 -- the text service implementation. See text_service.h for the
// activation contract, the reactivation rule and the deliberate scope limits.
#include "text_service.h"

#include "module_state.h"

namespace epocanva {
namespace tsf {

EpoCanvaTextService::EpoCanvaTextService()
    : reference_count_(1),
      thread_manager_(nullptr),
      client_id_(0),
      activation_flags_(0),
      active_(false) {
  ModuleAddObject();
}

EpoCanvaTextService::~EpoCanvaTextService() {
  // Defensive, not decorative: a caller that releases the last reference without
  // Deactivating first must not strand a thread-manager reference. Deactivate() is the
  // one cleanup path and it is idempotent by contract.
  Deactivate();
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
  if (thread_manager_ != nullptr) {
    thread_manager_->Release();
    thread_manager_ = nullptr;
  }
  client_id_ = 0;
  activation_flags_ = 0;
  active_ = false;
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
  // Reactivation contract: tear the previous activation down first (text_service.h).
  // After this call thread_manager_ is always null, so the AddRef below can never
  // double-hold a reference however many times a host re-activates.
  Deactivate();
  thread_manager_ = thread_manager;
  thread_manager_->AddRef();
  client_id_ = client_id;      // stored verbatim; see text_service.h for why
  activation_flags_ = flags;   // recorded this phase; no flag changes behaviour yet
  active_ = true;
  return S_OK;
}

}  // namespace tsf
}  // namespace epocanva
