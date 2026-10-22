// EpoCanva P3.1 -- the IClassFactory for the EpoCanva text service.
//
// WHAT THIS OBJECT IS. The one thing DllGetClassObject hands out: a stateless factory
// that creates EpoCanvaTextService instances (text_service.h). It supports exactly
// IID_IUnknown and IID_IClassFactory; every other IID is refused with E_NOINTERFACE and
// a nulled out-parameter, per the P3.1 contract that a failed QueryInterface leaves
// nothing behind for the caller to release.
//
// AGGREGATION. Refused outright (CLASS_E_NOAGGREGATION). A non-null pUnkOuter means the
// caller wants this object to delegate its IUnknown to a controlling outer object; the
// skeleton implements no delegation path, and pretending otherwise would produce an
// object whose reference arithmetic lies. Creating non-aggregated only is a complete,
// honest contract for this phase, and the test program pins the refusal.
//
// LOCKSERVER. Counts into the module's server-lock tally (module_state.h), which is
// what keeps DllCanUnloadNow at S_FALSE while a host holds the server open without
// holding any object. LockServer(FALSE) with no outstanding lock is refused: the count
// must not underflow (see module_state.h), and the refusal is reported as E_UNEXPECTED
// so a caller with broken bookkeeping sees it rather than silently unlocking someone
// else's lock.
//
// LIFETIME. Constructed with a reference count of 1 owned by the caller of
// DllGetClassObject (exported in dll_main.cc). The constructor/destructor pair moves the
// module's live-object counter, so a factory alive for even an instant holds the module
// down -- that is the behaviour point 5 of the test program turns into evidence.
#ifndef EPOCANVA_TSF_CLASS_FACTORY_H_
#define EPOCANVA_TSF_CLASS_FACTORY_H_

#include <unknwn.h>

namespace epocanva {
namespace tsf {

class EpoCanvaClassFactory : public IClassFactory {
 public:
  EpoCanvaClassFactory();

  EpoCanvaClassFactory(const EpoCanvaClassFactory&) = delete;
  EpoCanvaClassFactory& operator=(const EpoCanvaClassFactory&) = delete;

  // IUnknown
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  // IClassFactory
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid,
                                           void** object) override;
  HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override;

 private:
  ~EpoCanvaClassFactory();  // Release()-only lifecycle

  volatile LONG reference_count_;
};

}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TSF_CLASS_FACTORY_H_
