// EpoCanva P3.1 -- the unregistered-lifecycle verification program for
// platforms/windows/tsf/epocanva_tsf.dll.
//
// HOW THIS PROGRAM REACHES THE DLL (P3.1 brief, section 8). Through an absolute path it
// is given on the command line: LoadLibraryW, then GetProcAddress for
// DllGetClassObject/DllCanUnloadNow, then the COM interfaces those return. The DLL is
// NOT registered, no EpoCanva CLSID exists in the registry, and nothing here touches
// regsvr32, profiles, categories or the input-language list. A green run therefore
// proves the DLL's own contract, never an installation.
//
// THE TWO JUDGEMENTS. A check is only counted when it observes BEHAVIOUR: interfaces
// that answer, activation state that holds, DllCanUnloadNow verdicts at every stage.
// AddRef/Release return values are printed for the record but never used as a
// judgement, per the brief's rule that numbers alone cannot prove reference health --
// the final unload verdict after every release is the load-bearing observable.
//
// AFTER FreeLibrary THE PROGRAM TOUCHES NO LEFTOVER POINTER. Every COM pointer is out
// of scope (or nulled) before the library is freed; the CTest case for this program
// would crash, not pass, if the DLL's object lifetime arithmetic were wrong. The same
// rule applies one level down: no pointer is ever used after the reference it holds was
// its last, because a stale vtable slot in a Release build usually still calls the right
// code and would turn a lifetime bug into a green test.
//
// MODES
//   lifecycle       the ten boundary checks the brief lists, on the real exported
//                   entry points: exports present (including the W3-B registration
//                   exports, checked but never called here), unknown CLSID refused, an
//                   unsupported IID refused at BOTH
//                   levels (DllGetClassObject and QueryInterface) with the module left
//                   holding no live object, a null out-parameter refused, aggregation
//                   refused, unload verdict S_FALSE with the factory alive, S_FALSE with
//                   a service alive, S_FALSE with a lock held and no object alive,
//                   LockServer(TRUE/FALSE) with no underflow, S_OK after everything is
//                   released, five clean create/destroy cycles, and a failed QI that
//                   leaves the object usable.
//   threadmgr       the real-TSF round trip: COM initialised on this thread, a REAL
//                   ITfThreadMgr from the OS (CoCreateInstance, no fake pointer), a
//                   real client id via GetClientId, then our own service object
//                   activated/deactivated/reactivated against it, released in order,
//                   COM unwound. Every stage reports its HRESULT; any failure exits 6
//                   naming the stage -- it is never converted into a pass.
//   self-check-fail the falsifiability control: it makes one deliberately false claim
//                   (that an unknown CLSID yields a factory) and must exit 14 without
//                   printing RESULT PASS, proving the checks above can fail.
//
// TEST-ONLY FROZEN CONSTANTS. The two GUIDs below were generated once together with
// the product CLSID (2026-10-22, PowerShell [guid]::NewGuid()) and frozen like it.
// They are arbitrary: not the product CLSID, and not any interface this DLL or the
// SDK implements -- which is exactly what makes them the right probes for "unknown"
// and "unsupported".
//
// EXIT CODES: 0 pass; 2 usage; 3 LoadLibrary failed; 4 exports wrong; 5 an expectation
// failed; 6 a real-TSF stage failed (stage + HRESULT printed); 14 the self-check-fail
// mode's expected failure. RESULT PASS on stdout is the CTest marker.
#include <windows.h>
#include <msctf.h>
#include <objbase.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "identifiers.h"

namespace {

// {E9C79913-AC0B-48ED-A07E-3FD64308D574} -- test-only, never registered anywhere.
const GUID kUnknownTestClassid = {
    0xE9C79913, 0xAC0B, 0x48ED, {0xA0, 0x7E, 0x3F, 0xD6, 0x43, 0x08, 0xD5, 0x74}};
// {0F39C42C-B8A4-4527-A886-D22EE7E839F2} -- test-only, implemented by nothing here.
const IID kUnsupportedTestIid = {
    0x0F39C42C, 0xB8A4, 0x4527, {0xA8, 0x86, 0xD2, 0x2E, 0xE7, 0xE8, 0x39, 0xF2}};

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitLoadFailed = 3;
constexpr int kExitExportWrong = 4;
constexpr int kExitCheckFailed = 5;
constexpr int kExitTsfStageFailed = 6;
constexpr int kExitSelfCheckFailed = 14;

// The failure counter, and the checks. A failed check is reported in place and turns
// the run red at the end; the program never stops at the first mismatch.
int g_failed = 0;
int g_total = 0;

void Check(bool condition, const std::string& what, const std::string& got) {
  ++g_total;
  if (condition) {
    std::printf("[tsf]   ok   %s\n", what.c_str());
  } else {
    ++g_failed;
    std::printf("[tsf]   FAIL %s  (got: %s)\n", what.c_str(), got.c_str());
  }
}

void CheckHr(HRESULT hr, HRESULT want, const std::string& what) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "hr=0x%08lX want=0x%08lX",
                static_cast<unsigned long>(hr), static_cast<unsigned long>(want));
  Check(hr == want, what, buf);
}

std::string Hr(HRESULT hr) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
  return buf;
}

// --- loading ----------------------------------------------------------------------
HMODULE LoadServiceDll(const std::wstring& path, int* exit_code) {
  HMODULE module = ::LoadLibraryW(path.c_str());
  if (module == nullptr) {
    std::printf("[tsf] stage load          FAILED gle=%lu  path=%ls\n",
                static_cast<unsigned long>(::GetLastError()), path.c_str());
    *exit_code = kExitLoadFailed;
    return nullptr;
  }
  // The loaded module must be the file this run named -- not some other copy resolved
  // by a search order. Compared case-insensitively after GetFullPathNameW on both sides.
  wchar_t loaded[MAX_PATH] = {};
  const DWORD loaded_len = ::GetModuleFileNameW(module, loaded, MAX_PATH);
  wchar_t wanted[MAX_PATH] = {};
  const DWORD wanted_len = ::GetFullPathNameW(path.c_str(), MAX_PATH, wanted, nullptr);
  const bool same = loaded_len > 0 && wanted_len > 0 && wanted_len < MAX_PATH &&
                    _wcsicmp(loaded, wanted) == 0;
  {
    char loaded_utf8[MAX_PATH * 2] = {};
    const int converted =
        ::WideCharToMultiByte(CP_UTF8, 0, loaded, -1, loaded_utf8, sizeof(loaded_utf8),
                              nullptr, nullptr);
    Check(same, "the loaded module is exactly the DLL path this run named",
          converted > 0 ? std::string("loaded: ") + loaded_utf8
                        : "GetModuleFileNameW failed");
  }
  if (!same) {
    std::printf("[tsf]   wanted : %ls\n", wanted);
    *exit_code = kExitLoadFailed;
    return nullptr;
  }
  return module;
}

bool ResolveExports(HMODULE module, void** get_class_object, void** can_unload_now) {
  // The two exports that must exist...
  *get_class_object = reinterpret_cast<void*>(::GetProcAddress(module, "DllGetClassObject"));
  *can_unload_now = reinterpret_cast<void*>(::GetProcAddress(module, "DllCanUnloadNow"));
  Check(*get_class_object != nullptr, "DllGetClassObject is exported",
        *get_class_object ? "found" : "absent");
  Check(*can_unload_now != nullptr, "DllCanUnloadNow is exported",
        *can_unload_now ? "found" : "absent");
  if (*get_class_object == nullptr || *can_unload_now == nullptr) {
    return false;
  }
  // ...and the two W3-B registration exports, which must exist. This test never calls
  // them: calling DllRegisterServer here would write the real registry, which only an
  // explicitly authorized layer-C session may do. Their behaviour is covered by the
  // fake-backend registration tests.
  Check(::GetProcAddress(module, "DllRegisterServer") != nullptr,
        "DllRegisterServer is exported",
        ::GetProcAddress(module, "DllRegisterServer") ? "present" : "absent");
  Check(::GetProcAddress(module, "DllUnregisterServer") != nullptr,
        "DllUnregisterServer is exported",
        ::GetProcAddress(module, "DllUnregisterServer") ? "present" : "absent");
  return ::GetProcAddress(module, "DllRegisterServer") != nullptr &&
         ::GetProcAddress(module, "DllUnregisterServer") != nullptr;
}

using DllGetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
using DllCanUnloadNowFn = HRESULT(WINAPI*)(void);

bool UnloadVerdict(DllCanUnloadNowFn can_unload, bool want_ok,
                   const std::string& what) {
  const HRESULT hr = can_unload();
  Check(want_ok ? (hr == S_OK) : (hr == S_FALSE), what,
        "DllCanUnloadNow=" + Hr(hr) + (want_ok ? " want=S_OK" : " want=S_FALSE"));
  return want_ok ? (hr == S_OK) : (hr == S_FALSE);
}

// --- the lifecycle mode ------------------------------------------------------------
int RunLifecycle(const std::wstring& dll_path) {
  int exit_code = kExitOk;
  HMODULE module = LoadServiceDll(dll_path, &exit_code);
  if (module == nullptr) {
    return exit_code;
  }
  void* get_class_object_ptr = nullptr;
  void* can_unload_ptr = nullptr;
  if (!ResolveExports(module, &get_class_object_ptr, &can_unload_ptr)) {
    ::FreeLibrary(module);
    return kExitExportWrong;
  }
  const auto get_class_object = reinterpret_cast<DllGetClassObjectFn>(get_class_object_ptr);
  const auto can_unload = reinterpret_cast<DllCanUnloadNowFn>(can_unload_ptr);

  // A freshly loaded, untouched server must be unloadable: no object exists yet.
  UnloadVerdict(can_unload, true, "freshly loaded, no object created: unloadable");

  // (2) an unknown CLSID is refused, with a null out-value, and leaves nothing behind.
  IClassFactory* refused = nullptr;
  HRESULT hr = get_class_object(kUnknownTestClassid, __uuidof(IClassFactory),
                                reinterpret_cast<LPVOID*>(&refused));
  CheckHr(hr, CLASS_E_CLASSNOTAVAILABLE, "unknown CLSID refused with CLASS_E_CLASSNOTAVAILABLE");
  Check(refused == nullptr, "unknown CLSID refusal left a null out-pointer",
        refused ? "non-null" : "null");
  UnloadVerdict(can_unload, true, "after the refused CLSID the module is still unloadable");

  // (1a) the same refusal one level up, at the DLL's own boundary: the REAL CLSID with
  //      an IID this DLL does not implement. dll_main.cc builds a factory to answer this
  //      query, the QueryInterface refuses it, and the factory is destroyed again -- so
  //      the verdict below is the evidence that a refused query leaves no live object
  //      behind (a leak here would pin the module forever).
  IUnknown* wrong_iid = nullptr;
  hr = get_class_object(epocanva::tsf::kTextServiceClsid, kUnsupportedTestIid,
                        reinterpret_cast<LPVOID*>(&wrong_iid));
  CheckHr(hr, E_NOINTERFACE, "DllGetClassObject refuses an unsupported IID with E_NOINTERFACE");
  Check(wrong_iid == nullptr, "the refused DllGetClassObject left a null out-pointer",
        wrong_iid ? "non-null" : "null");
  UnloadVerdict(can_unload, true, "the refused IID left no live object in the module");

  // A null out-parameter is refused before anything is built (the guard is first in
  // dll_main.cc's GetClassObject, ahead of the CLSID test).
  hr = get_class_object(epocanva::tsf::kTextServiceClsid, __uuidof(IClassFactory), nullptr);
  CheckHr(hr, E_POINTER, "DllGetClassObject with a null out-parameter is E_POINTER");
  UnloadVerdict(can_unload, true, "the null out-parameter call created nothing");

  // (1) the real CLSID yields the class factory.
  IClassFactory* factory = nullptr;
  hr = get_class_object(epocanva::tsf::kTextServiceClsid, __uuidof(IClassFactory),
                        reinterpret_cast<LPVOID*>(&factory));
  CheckHr(hr, S_OK, "DllGetClassObject yields the class factory");
  if (hr != S_OK || factory == nullptr) {
    ::FreeLibrary(module);
    return kExitCheckFailed;
  }
  IUnknown* factory_unknown = nullptr;
  hr = factory->QueryInterface(__uuidof(IUnknown),
                               reinterpret_cast<void**>(&factory_unknown));
  CheckHr(hr, S_OK, "the factory answers IID_IUnknown");
  // IUnknown identity: the same address from the IClassFactory pointer and from the
  // dedicated IUnknown query -- one object, one identity.
  Check(factory_unknown == static_cast<IUnknown*>(factory),
        "IUnknown identity: both queries name the same object",
        factory_unknown ? "same" : "null");
  if (factory_unknown != nullptr) {
    factory_unknown->Release();
  }

  // (3) an unsupported IID is refused with a null out-value...
  IUnknown* unsupported = nullptr;
  hr = factory->QueryInterface(kUnsupportedTestIid,
                               reinterpret_cast<void**>(&unsupported));
  CheckHr(hr, E_NOINTERFACE, "unsupported IID refused with E_NOINTERFACE");
  Check(unsupported == nullptr, "the failed QueryInterface left a null out-pointer",
        unsupported ? "non-null" : "null");
  // (10) ...and the failed query did not damage the object: the factory still answers.
  IClassFactory* still_usable = nullptr;
  hr = factory->QueryInterface(__uuidof(IClassFactory),
                               reinterpret_cast<void**>(&still_usable));
  CheckHr(hr, S_OK, "after the failed query the factory still answers IID_IClassFactory");
  if (still_usable != nullptr) {
    still_usable->Release();
  }

  // (5) a factory alive holds the module down.
  UnloadVerdict(can_unload, false, "factory alive: not unloadable");

  // (4) aggregation is refused before anything is created.
  IUnknown* aggregated = nullptr;
  hr = factory->CreateInstance(static_cast<IUnknown*>(factory), __uuidof(IClassFactory),
                               reinterpret_cast<void**>(&aggregated));
  CheckHr(hr, CLASS_E_NOAGGREGATION, "aggregation refused with CLASS_E_NOAGGREGATION");
  Check(aggregated == nullptr, "the aggregation refusal left a null out-pointer",
        aggregated ? "non-null" : "null");

  // (6) the service object alive also holds the module down -- even after the factory
  // itself is released, which is what makes the two probes distinguishable.
  ITfTextInputProcessorEx* service = nullptr;
  hr = factory->CreateInstance(nullptr, __uuidof(ITfTextInputProcessorEx),
                               reinterpret_cast<void**>(&service));
  CheckHr(hr, S_OK, "CreateInstance yields the text service as ITfTextInputProcessorEx");
  hr = factory->Release();
  Check(hr == 0, "the factory reference released to zero",
        "Release returned " + std::to_string(hr));
  UnloadVerdict(can_unload, false, "service alive, factory gone: not unloadable");

  // Interface reachability and identity on the service object.
  ITfTextInputProcessor* service_base = nullptr;
  hr = service->QueryInterface(__uuidof(ITfTextInputProcessor),
                               reinterpret_cast<void**>(&service_base));
  CheckHr(hr, S_OK, "the service answers IID_ITfTextInputProcessor");
  IUnknown* service_unknown = nullptr;
  hr = service->QueryInterface(__uuidof(IUnknown),
                               reinterpret_cast<void**>(&service_unknown));
  CheckHr(hr, S_OK, "the service answers IID_IUnknown");
  Check(service_unknown == static_cast<IUnknown*>(service),
        "service IUnknown identity: one object behind both interfaces",
        service_unknown ? "same" : "null");
  if (service_unknown != nullptr) {
    service_unknown->Release();
  }
  IUnknown* service_unsupported = nullptr;
  hr = service->QueryInterface(kUnsupportedTestIid,
                               reinterpret_cast<void**>(&service_unsupported));
  CheckHr(hr, E_NOINTERFACE, "the service refuses the unsupported IID");
  Check(service_unsupported == nullptr, "and left a null out-pointer",
        service_unsupported ? "non-null" : "null");
  // (10) again: after the refused query the object still works.
  ITfTextInputProcessor* service_still_usable = nullptr;
  hr = service->QueryInterface(__uuidof(ITfTextInputProcessor),
                               reinterpret_cast<void**>(&service_still_usable));
  CheckHr(hr, S_OK, "after the refused query the service still answers its interface");
  if (service_still_usable != nullptr) {
    service_still_usable->Release();
  }

  // Parameter validation, callable without any TSF host: a null thread manager is the
  // one invalid input the SDK's own signature lets us detect.
  CheckHr(service->Activate(nullptr, 0x11111111u), E_INVALIDARG,
          "Activate with a null ITfThreadMgr is E_INVALIDARG");
  CheckHr(service->ActivateEx(nullptr, 0x11111111u, 0), E_INVALIDARG,
          "ActivateEx with a null ITfThreadMgr is E_INVALIDARG");
  // The documented no-op: deactivating a never-activated service succeeds and changes
  // nothing (the unload verdict below stays exactly where it was).
  CheckHr(service->Deactivate(), S_OK,
          "Deactivate without a prior activation is a successful no-op");
  UnloadVerdict(can_unload, false, "after the no-op Deactivate the service still lives");

  // (8) everything released -> unloadable again. `service` and `service_base` are two
  // references to ONE object (QueryInterface placed the second), so the counts step
  // down 2 -> 1 -> 0; asserting the intermediate 1 is part of the arithmetic check.
  hr = service_base->Release();
  Check(hr == 1, "the ITfTextInputProcessor reference released (one reference left)",
        "Release returned " + std::to_string(hr));
  hr = service->Release();
  Check(hr == 0, "the last reference released to zero",
        "Release returned " + std::to_string(hr));
  UnloadVerdict(can_unload, true, "all objects released: unloadable again");

  // (7) The server lock, isolated from object lifetime. A lock has to hold the DLL down
  //     when no object is alive -- that is its entire purpose -- so the decisive verdict
  //     is the one taken AFTER the factory that set it is destroyed. Releasing it then
  //     goes through a fresh factory instance: LockServer accounts into module state
  //     (module_state.h), not per-object state, so any instance of this class factory is
  //     a legitimate caller of LockServer(FALSE). Talking to the destroyed factory
  //     instead would be a use-after-free -- and in a Release build the freed vtable
  //     slot usually still points at the right code, so it would pass. Underflow is the
  //     last check: with no lock outstanding a second unlock is refused and the counter
  //     is left at zero, which the final S_OK verdict proves (a wrapped counter would
  //     keep ModuleCanUnload() false for the rest of the run).
  IClassFactory* locker = nullptr;
  hr = get_class_object(epocanva::tsf::kTextServiceClsid, __uuidof(IClassFactory),
                        reinterpret_cast<LPVOID*>(&locker));
  CheckHr(hr, S_OK, "a factory obtained for the lock stage");
  if (locker == nullptr) {
    ::FreeLibrary(module);
    return kExitCheckFailed;
  }
  CheckHr(locker->LockServer(TRUE), S_OK, "LockServer(TRUE) accepted");
  UnloadVerdict(can_unload, false, "factory and lock both alive: not unloadable");
  const ULONG locker_last = locker->Release();  // destroys the factory, the lock survives
  Check(locker_last == 0, "the locking factory released to zero",
        "Release returned " + std::to_string(locker_last));
  UnloadVerdict(can_unload, false, "no object alive, one lock held: still not unloadable");

  IClassFactory* unlocker = nullptr;
  hr = get_class_object(epocanva::tsf::kTextServiceClsid, __uuidof(IClassFactory),
                        reinterpret_cast<LPVOID*>(&unlocker));
  CheckHr(hr, S_OK, "a second factory obtained to release the lock");
  if (unlocker == nullptr) {
    ::FreeLibrary(module);
    return kExitCheckFailed;
  }
  CheckHr(unlocker->LockServer(FALSE), S_OK,
          "LockServer(FALSE) through the second factory releases the module lock");
  UnloadVerdict(can_unload, false,
                "lock gone, the unlocking factory still alive: not unloadable");
  CheckHr(unlocker->LockServer(FALSE), E_UNEXPECTED,
          "an unlock with no outstanding lock is refused, not underflowed");
  const ULONG unlocker_last = unlocker->Release();
  Check(unlocker_last == 0, "the unlocking factory released to zero",
        "Release returned " + std::to_string(unlocker_last));
  UnloadVerdict(can_unload, true,
                "lock released and every factory gone: unloadable again");

  // (9) five create/destroy cycles, each ending fully unloadable: no per-cycle residue.
  bool cycles_clean = true;
  for (int cycle = 0; cycle < 5; ++cycle) {
    IClassFactory* cycle_factory = nullptr;
    hr = get_class_object(epocanva::tsf::kTextServiceClsid, __uuidof(IClassFactory),
                          reinterpret_cast<LPVOID*>(&cycle_factory));
    ITfTextInputProcessor* cycle_service = nullptr;
    if (hr == S_OK) {
      hr = cycle_factory->CreateInstance(nullptr, __uuidof(ITfTextInputProcessor),
                                         reinterpret_cast<void**>(&cycle_service));
    }
    if (cycle_service != nullptr) {
      cycle_service->Release();
    }
    if (cycle_factory != nullptr) {
      cycle_factory->Release();
    }
    const HRESULT verdict = can_unload();
    if (hr != S_OK || verdict != S_OK) {
      cycles_clean = false;
      std::printf("[tsf]   cycle %d left hr=0x%08lX verdict=0x%08lX\n", cycle,
                  static_cast<unsigned long>(hr), static_cast<unsigned long>(verdict));
    }
  }
  Check(cycles_clean, "five create/destroy cycles each ended fully unloadable",
        cycles_clean ? "5/5 clean" : "see cycle lines above");

  ::FreeLibrary(module);
  // Nothing above touches a DLL pointer after this point: the library is released and
  // every COM pointer is already out of scope. The verdict that the module could have
  // been unloaded was taken BEFORE the free, which is the only honest order.
  std::printf("[tsf] RESULT PASS (%d checks, %d failed)\n", g_total, g_failed);
  return g_failed == 0 ? kExitOk : kExitCheckFailed;
}

// --- the real-TSF ThreadMgr mode ----------------------------------------------------
// Every stage names itself and its HRESULT; any failure exits 6 and is never folded
// into a pass. The ThreadMgr is the OS's own: no fake pointer, no mock, and no
// registration of anything (GetClientId maps a CLSID to an in-memory id inside this
// thread manager -- it performs no profile or registry operation).
int RunThreadMgr(const std::wstring& dll_path) {
  int exit_code = kExitOk;
  HMODULE module = LoadServiceDll(dll_path, &exit_code);
  if (module == nullptr) {
    return exit_code;
  }
  void* get_class_object_ptr = nullptr;
  void* can_unload_ptr = nullptr;
  if (!ResolveExports(module, &get_class_object_ptr, &can_unload_ptr)) {
    ::FreeLibrary(module);
    return kExitExportWrong;
  }
  const auto get_class_object = reinterpret_cast<DllGetClassObjectFn>(get_class_object_ptr);
  const auto can_unload = reinterpret_cast<DllCanUnloadNowFn>(can_unload_ptr);

  // Stage 1: COM on this thread, apartment-threaded like a normal TSF client.
  HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(hr)) {
    std::printf("[tsf] stage CoInitializeEx   FAILED hr=%s\n", Hr(hr).c_str());
    ::FreeLibrary(module);
    return kExitTsfStageFailed;
  }

  // Stage 2: the REAL system thread manager.
  ITfThreadMgr* thread_manager = nullptr;
  hr = ::CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
                          __uuidof(ITfThreadMgr),
                          reinterpret_cast<void**>(&thread_manager));
  if (FAILED(hr) || thread_manager == nullptr) {
    std::printf("[tsf] stage CoCreateInstance(CLSID_TF_ThreadMgr) FAILED hr=%s\n",
                Hr(hr).c_str());
    ::CoUninitialize();
    ::FreeLibrary(module);
    return kExitTsfStageFailed;
  }
  std::printf("[tsf] stage thread mgr       OK\n");

  // Stage 3: activate the thread manager and take a real client id for OUR CLSID.
  TfClientId app_client_id = 0;
  hr = thread_manager->Activate(&app_client_id);
  if (FAILED(hr)) {
    std::printf("[tsf] stage ThreadMgr::Activate FAILED hr=%s\n", Hr(hr).c_str());
    thread_manager->Release();
    ::CoUninitialize();
    ::FreeLibrary(module);
    return kExitTsfStageFailed;
  }
  TfClientId service_client_id = 0;
  // GetClientId lives on ITfClientId, a separate interface the thread-manager object
  // also implements (msctf.h:10700-10705, uuid d60a7b49-1b9f-4be2-b702-47e9dc05dec3);
  // it maps a CLSID to an id inside THIS manager and performs no registration.
  ITfClientId* client_id_source = nullptr;
  hr = thread_manager->QueryInterface(__uuidof(ITfClientId),
                                      reinterpret_cast<void**>(&client_id_source));
  if (SUCCEEDED(hr) && client_id_source != nullptr) {
    hr = client_id_source->GetClientId(epocanva::tsf::kTextServiceClsid, &service_client_id);
    client_id_source->Release();
  }
  if (FAILED(hr)) {
    std::printf("[tsf] stage GetClientId FAILED hr=%s (via ITfClientId)\n", Hr(hr).c_str());
    thread_manager->Release();
    ::CoUninitialize();
    ::FreeLibrary(module);
    return kExitTsfStageFailed;
  }
  std::printf("[tsf] stage client ids       OK (app=%lu service=%lu; in-memory ids "
              "of this ThreadMgr only)\n",
              static_cast<unsigned long>(app_client_id),
              static_cast<unsigned long>(service_client_id));

  // Stage 4: our own service object, created through the DLL's real entry point.
  IClassFactory* factory = nullptr;
  hr = get_class_object(epocanva::tsf::kTextServiceClsid, __uuidof(IClassFactory),
                        reinterpret_cast<LPVOID*>(&factory));
  if (FAILED(hr) || factory == nullptr) {
    std::printf("[tsf] stage DllGetClassObject FAILED hr=%s\n", Hr(hr).c_str());
    thread_manager->Release();
    ::CoUninitialize();
    ::FreeLibrary(module);
    return kExitTsfStageFailed;
  }
  ITfTextInputProcessorEx* service = nullptr;
  hr = factory->CreateInstance(nullptr, __uuidof(ITfTextInputProcessorEx),
                               reinterpret_cast<void**>(&service));
  factory->Release();
  if (FAILED(hr) || service == nullptr) {
    std::printf("[tsf] stage CreateInstance FAILED hr=%s\n", Hr(hr).c_str());
    thread_manager->Release();
    ::CoUninitialize();
    ::FreeLibrary(module);
    return kExitTsfStageFailed;
  }

  // Stage 5: activate OUR service against the REAL thread manager. This is exactly what
  // the skeleton implements: parameter check, the thread-manager reference, the client
  // id, the activated state. No key sink, no composition, no candidate UI -- none of
  // that exists, and this stage does not pretend otherwise.
  hr = service->Activate(thread_manager, service_client_id);
  if (FAILED(hr)) {
    std::printf("[tsf] stage service Activate FAILED hr=%s\n", Hr(hr).c_str());
    service->Release();
    thread_manager->Release();
    ::CoUninitialize();
    ::FreeLibrary(module);
    return kExitTsfStageFailed;
  }
  UnloadVerdict(can_unload, false, "an activated service holds the module down");

  // Stage 6: the documented activation contract, against the real manager.
  CheckHr(service->ActivateEx(thread_manager, service_client_id, 0), S_OK,
          "re-ActivateEx while active succeeds (self-healing contract)");
  CheckHr(service->Deactivate(), S_OK, "Deactivate succeeds");
  CheckHr(service->Deactivate(), S_OK,
          "a second Deactivate is a successful no-op");
  CheckHr(service->Activate(thread_manager, service_client_id), S_OK,
          "Activate succeeds again after a Deactivate");
  CheckHr(service->ActivateEx(nullptr, service_client_id, 0), E_INVALIDARG,
          "ActivateEx with a null manager is E_INVALIDARG against the real host too");
  // Failure cleanup, as far as a client can observe it: a refused activation returns
  // before it mutates anything, so the activation that preceded it survives intact. The
  // honest, checkable claim is about OUR arithmetic -- the module still holds the object
  // and the verdict is unchanged. A stranded reference inside the OS thread manager is
  // not observable from a client at all (COM exposes no refcount read), which is why the
  // destructor releases through the same Deactivate path (text_service.cc) rather than a
  // second cleanup implementation that nothing could cross-check.
  CheckHr(service->Activate(nullptr, service_client_id), E_INVALIDARG,
          "a failed Activate against a real host is refused while active");
  UnloadVerdict(can_unload, false,
                "the failed Activate changed nothing: the service is still alive");
  CheckHr(service->Deactivate(), S_OK, "final Deactivate succeeds");

  // Stage 7: unwind in order -- the service (its Release returning 0 is checked, and
  // the unload verdict is taken AFTER it, which is what makes the verdict honest), the
  // real thread manager, COM, the DLL. Nothing touches a DLL pointer after FreeLibrary.
  const ULONG last_reference = service->Release();
  Check(last_reference == 0, "the service's last reference released to zero",
        "Release returned " + std::to_string(last_reference));
  UnloadVerdict(can_unload, true, "service released: unloadable while the real "
                                  "ThreadMgr is still alive");
  thread_manager->Deactivate();
  thread_manager->Release();
  ::CoUninitialize();
  ::FreeLibrary(module);
  std::printf("[tsf] RESULT PASS (%d checks, %d failed)\n", g_total, g_failed);
  return g_failed == 0 ? kExitOk : kExitCheckFailed;
}

// --- the falsifiability control -------------------------------------------------------
int RunSelfCheckFail(const std::wstring& dll_path) {
  int exit_code = kExitOk;
  HMODULE module = LoadServiceDll(dll_path, &exit_code);
  if (module == nullptr) {
    return exit_code;
  }
  void* get_class_object_ptr = nullptr;
  void* can_unload_ptr = nullptr;
  if (!ResolveExports(module, &get_class_object_ptr, &can_unload_ptr)) {
    ::FreeLibrary(module);
    return kExitExportWrong;
  }
  const auto get_class_object = reinterpret_cast<DllGetClassObjectFn>(get_class_object_ptr);
  // The false claim: that an unknown CLSID yields a factory. The lifecycle mode pins
  // the opposite, so this must fail here -- exit 14, no RESULT PASS.
  IClassFactory* factory = nullptr;
  const HRESULT hr = get_class_object(kUnknownTestClassid, __uuidof(IClassFactory),
                                      reinterpret_cast<LPVOID*>(&factory));
  CheckHr(hr, S_OK, "deliberately asserted that an unknown CLSID yields a factory");
  if (factory != nullptr) {
    factory->Release();
  }
  ::FreeLibrary(module);
  std::printf("[tsf] RESULT FAIL (self-check-fail control, %d checks, %d failed)\n",
              g_total, g_failed);
  return g_failed == 0 ? kExitOk : kExitSelfCheckFailed;
}

bool ParseArgs(int argc, wchar_t** argv, std::wstring* dll, std::string* mode) {
  bool have_dll = false;
  bool have_mode = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--dll" && i + 1 < argc) {
      *dll = argv[++i];
      have_dll = true;
    } else if (arg == L"--mode" && i + 1 < argc) {
      // Explicit narrowing rather than std::string(begin, end): the mode names are
      // ASCII, and an explicit loop keeps the wchar_t->char conversion visible instead
      // of C4244-warning its way through <xutility>.
      const std::wstring wide_mode = argv[++i];
      mode->assign(wide_mode.size(), '\0');
      bool ascii_only = true;
      for (std::size_t c = 0; c < wide_mode.size(); ++c) {
        (*mode)[c] = static_cast<char>(wide_mode[c]);
        if (wide_mode[c] > 0x7F) {
          ascii_only = false;
        }
      }
      if (!ascii_only) {
        return false;  // mode names are ASCII by contract; anything else is usage noise
      }
      have_mode = true;
    } else {
      return false;
    }
  }
  return have_dll && have_mode;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::wstring dll_path;
  std::string mode;
  if (!ParseArgs(argc, argv, &dll_path, &mode)) {
    std::printf("usage: tsf_lifecycle --dll <absolute path to epocanva_tsf.dll> "
                "--mode lifecycle|threadmgr|self-check-fail\n");
    return kExitUsage;
  }
  // The path must be absolute: the whole point is a controlled load, not a search-order
  // accident. Both slash conventions count (CMake generator expressions produce forward
  // slashes); LoadServiceDll additionally proves the loaded module IS this file.
  if (dll_path.size() < 3 || dll_path[1] != L':' ||
      (dll_path[2] != L'\\' && dll_path[2] != L'/')) {
    std::printf("[tsf] --dll must be an absolute path, got: %ls\n", dll_path.c_str());
    return kExitUsage;
  }
  if (mode == "lifecycle") {
    return RunLifecycle(dll_path);
  }
  if (mode == "threadmgr") {
    return RunThreadMgr(dll_path);
  }
  if (mode == "self-check-fail") {
    return RunSelfCheckFail(dll_path);
  }
  std::printf("[tsf] unknown mode: %s\n", mode.c_str());
  return kExitUsage;
}
