// EpoCanva P3.1 -- the class factory implementation. See class_factory.h for the
// supported-IID set, the aggregation refusal and the LockServer contract.
#include "class_factory.h"

#include <new>

#include "module_state.h"
#include "text_service.h"

namespace epocanva {
namespace tsf {

EpoCanvaClassFactory::EpoCanvaClassFactory() : reference_count_(1) {
  ModuleAddObject();
}

EpoCanvaClassFactory::~EpoCanvaClassFactory() {
  ModuleReleaseObject();
}

HRESULT STDMETHODCALLTYPE EpoCanvaClassFactory::QueryInterface(REFIID riid,
                                                               void** object) {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (IsEqualIID(riid, __uuidof(IUnknown))) {
    *object = static_cast<IClassFactory*>(this);  // the IUnknown subobject: same address
  } else if (IsEqualIID(riid, __uuidof(IClassFactory))) {
    *object = static_cast<IClassFactory*>(this);
  } else {
    return E_NOINTERFACE;  // *object stays null: nothing for the caller to release
  }
  AddRef();
  return S_OK;
}

ULONG STDMETHODCALLTYPE EpoCanvaClassFactory::AddRef() {
  return InterlockedIncrement(&reference_count_);
}

ULONG STDMETHODCALLTYPE EpoCanvaClassFactory::Release() {
  const LONG after = InterlockedDecrement(&reference_count_);
  if (after == 0) {
    delete this;
    return 0;
  }
  if (after < 0) {
    // InterlockedDecrement on a correctly paired object cannot get here; reaching it
    // means a caller over-released. The module counter is separately guarded
    // (module_state.h), so this branch only reports the local arithmetic going wrong
    // rather than crashing the host.
    return 0;
  }
  return static_cast<ULONG>(after);
}

HRESULT STDMETHODCALLTYPE EpoCanvaClassFactory::CreateInstance(IUnknown* outer,
                                                               REFIID riid,
                                                               void** object) {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (outer != nullptr) {
    // Aggregation needs an IUnknown-delegation path this skeleton does not have; see
    // class_factory.h for why refusing beats faking it.
    return CLASS_E_NOAGGREGATION;
  }
  EpoCanvaTextService* service = new (std::nothrow) EpoCanvaTextService();
  if (service == nullptr) {
    return E_OUTOFMEMORY;
  }
  // The service is born holding the construction reference; QueryInterface places the
  // caller's own reference in *object, then the construction reference is dropped. On
  // an unsupported riid this destroys the service and puts the module counters back.
  const HRESULT query = service->QueryInterface(riid, object);
  service->Release();
  return query;
}

HRESULT STDMETHODCALLTYPE EpoCanvaClassFactory::LockServer(BOOL lock) {
  if (lock) {
    ModuleAddLock();
    return S_OK;
  }
  return ModuleReleaseLock() ? S_OK : E_UNEXPECTED;
}

}  // namespace tsf
}  // namespace epocanva
