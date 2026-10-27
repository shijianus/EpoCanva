// EpoCanva P3.1 -- the DLL boundary: DllMain and the two exported COM entry points.
//
// DLLMAIN IS MINIMAL, DELIBERATELY. It records the instance handle and nothing else
// (module_state.h). Inside DllMain this DLL never initialises COM or the engine, never
// creates a thread, never touches a file or the registry, never loads a language pack
// and never starts a logging facility -- the loader lock makes all of those both
// dangerous and forbidden by the P3.1 brief.
//
// DisableThreadLibraryCalls is NOT called. It is an optimisation for DLLs that know
// their own thread lifetime (it stops thread attach/detach notifications), and the
// brief's rule is to verify the CRT constraint before adding it rather than to add it
// reflexively. The static CRT this DLL is built with keeps its own bookkeeping across
// _DllMainCRTStartup, and nothing in this skeleton creates or destroys threads, so the
// call would save nothing today while binding us to a CRT-behaviour assumption that has
// not been verified. Revisit it only with that verification in hand
// (docs/windows/tsf-bootstrap.md records the decision).
//
// THE TWO EXPORTS (epocanva_tsf.def). DllRegisterServer/DllUnregisterServer do NOT
// exist in this DLL: the P3.1 round is explicitly unregistered, and an absent export is
// a stronger statement than a stub that would "succeed". The test program asserts both
// names are absent from the export table.
//
// EXCEPTION BOUNDARY. No C++ exception may escape a COM entry point (AGENTS.md P3.1
// contract, item 10): the exports wrap their bodies and translate anything that gets
// this far into the HRESULT a caller can act on. The objects themselves do not throw;
// the guards exist so a future change cannot silently break that.
//
// NO STL ACROSS THE BOUNDARY (item 9): every parameter is a GUID, an IID, a pointer or
// a DWORD. The DLL's internal state is plain Win32 types.
#include <new>

#include "class_factory.h"
#include "identifiers.h"
#include "module_state.h"
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
#include "product_engine_config.h"
#endif

namespace {

// The one CLSID this DLL serves. Everything else is CLASS_E_CLASSNOTAVAILABLE with a
// nulled out-parameter, so a caller cannot confuse "unknown class" with "class exists
// but construction failed".
HRESULT GetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* object) {
  if (object == nullptr) {
    return E_POINTER;
  }
  *object = nullptr;
  if (!IsEqualGUID(rclsid, epocanva::tsf::kTextServiceClsid)) {
    return CLASS_E_CLASSNOTAVAILABLE;
  }
#ifdef EPOCANVA_TSF_PRODUCT_LAYOUT
  // Explicit startup step outside DllMain. A failure leaves no engine configuration,
  // so Activate reports E_UNEXPECTED instead of starting with guessed paths.
  epocanva::product::ApplyProductEngineConfig(epocanva::tsf::ModuleInstance(),
                                              nullptr);
#endif
  auto* factory = new (std::nothrow) epocanva::tsf::EpoCanvaClassFactory();
  if (factory == nullptr) {
    return E_OUTOFMEMORY;
  }
  // The factory is born holding the construction reference; QueryInterface places the
  // caller's reference (for IID_IUnknown and IID_IClassFactory) and the construction
  // reference is dropped below. For any other riid the factory is destroyed here and
  // the module's live-object count returns to where it was.
  const HRESULT query = factory->QueryInterface(riid, object);
  factory->Release();
  return query;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)reserved;
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      epocanva::tsf::SetModuleInstance(instance);
      break;
    case DLL_PROCESS_DETACH:
      epocanva::tsf::SetModuleInstance(nullptr);
      break;
    default:
      break;
  }
  return TRUE;
}

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid,
                                            LPVOID* object) {
  try {
    return GetClassObject(rclsid, riid, object);
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

extern "C" HRESULT WINAPI DllCanUnloadNow(void) {
  // No try/catch: this function performs no allocation and calls nothing that throws.
  // It answers from the module counters alone.
  return epocanva::tsf::ModuleCanUnload() ? S_OK : S_FALSE;
}
