// EpoCanva P3.6 -- hosted TSF lifecycle verification.
//
// The program loads epocanva_tsf_hosted_test.dll by absolute path, then reaches it through
// the product COM entry points plus the private test seam. It creates REAL system
// ITfThreadMgr objects (one per STA thread), activates the real text service against them,
// and observes only lifecycle values: broker state, lease count, executor instance ids,
// Runtime initialize/finalize counts, thread ids and DllCanUnloadNow verdicts.
//
// WHAT THIS DOES NOT PROVE. No key event is sent, no sink is advised, no composition or
// host text exists, no candidate UI exists and nothing is registered. A green run proves
// the lifetime arithmetic and the unload guard, not input capability.
//
// MODES
//   single    one service: one Runtime, reactivation on the same ThreadMgr, clean stop,
//             a new executor instance after the stop
//   shared    two services: one executor, one deactivation leaves the other usable, a
//             different configuration is refused without disturbing the active service
//   multista  two STA threads with two real ThreadMgrs and two services: one shared broker,
//             objects never cross apartments
//   unload    start gate / stop gate / cleanup gate: DllCanUnloadNow is S_FALSE through the
//             whole window, Acquire during Stopping is refused, S_OK only after all objects
//   cleanup   a controlled cleanup-failure latch keeps DllCanUnloadNow S_FALSE with zero
//             live objects and zero locks
//   startup   missing DLL and missing English module fail cleanly and a later activation
//             starts a new executor
//   lease     the private lease probe: double release cannot underflow
//   self-check-fail  one deliberately false claim, exit 14, no RESULT PASS
//
// NO SLEEP-BASED RACES. Every window is held by a named event pair and every multi-thread
// phase is separated by a counting barrier.
#include <windows.h>
#include <msctf.h>
#include <objbase.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "identifiers.h"
#include "rime_runtime.h"
#include "test_seam_abi.h"

namespace {

namespace hosted = epocanva::tests::tsf_hosted;
namespace engine = epocanva::windows::engine;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitLoadFailed = 3;
constexpr int kExitExportWrong = 4;
constexpr int kExitCheckFailed = 5;
constexpr int kExitSelfCheckFailed = 14;
constexpr DWORD kWaitMs = 20000;

int g_total = 0;
int g_failed = 0;

void Check(bool condition, const std::string& what,
           const std::string& got = std::string()) {
  ++g_total;
  if (condition) {
    std::printf("[tsf-hosted] ok   %s\n", what.c_str());
  } else {
    ++g_failed;
    if (got.empty()) {
      std::printf("[tsf-hosted] FAIL %s\n", what.c_str());
    } else {
      std::printf("[tsf-hosted] FAIL %s  (got: %s)\n", what.c_str(), got.c_str());
    }
  }
}

template <typename T>
std::string Num(T value) {
  return std::to_string(static_cast<unsigned long long>(value));
}

std::string Hr(HRESULT hr) {
  char buffer[24];
  std::snprintf(buffer, sizeof(buffer), "0x%08lX",
                static_cast<unsigned long>(hr));
  return buffer;
}

void CheckHr(HRESULT hr, HRESULT want, const std::string& what) {
  Check(hr == want, what, "hr=" + Hr(hr) + " want=" + Hr(want));
}

struct Event {
  HANDLE handle = nullptr;
  Event() : handle(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  ~Event() {
    if (handle != nullptr) {
      ::CloseHandle(handle);
    }
  }
  Event(const Event&) = delete;
  Event& operator=(const Event&) = delete;
  bool valid() const { return handle != nullptr; }
  void Set() const { ::SetEvent(handle); }
  void Reset() const { ::ResetEvent(handle); }
  bool Wait() const {
    return handle != nullptr && ::WaitForSingleObject(handle, kWaitMs) == WAIT_OBJECT_0;
  }
};

class Barrier {
 public:
  explicit Barrier(int participants) : participants_(participants) {}
  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    const int generation = generation_;
    if (++arrived_ == participants_) {
      arrived_ = 0;
      ++generation_;
      cv_.notify_all();
      return;
    }
    cv_.wait(lock, [this, generation] { return generation_ != generation; });
  }

 private:
  const int participants_;
  std::mutex mutex_;
  std::condition_variable cv_;
  int arrived_ = 0;
  int generation_ = 0;
};

struct Options {
  std::string mode;
  std::filesystem::path dll;
  std::filesystem::path rime_dll;
  std::filesystem::path baseline_dll;
  std::filesystem::path fixture_dir;
  std::filesystem::path spelling_fixture_dir;
  std::filesystem::path work_root;
};

struct Env {
  std::filesystem::path shared;
  std::filesystem::path user;
  std::filesystem::path log;
  std::filesystem::path log2;
};

using DllGetClassObjectFn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);
using DllCanUnloadNowFn = HRESULT(WINAPI*)(void);

struct HostDll {
  HMODULE module = nullptr;
  DllGetClassObjectFn get_class_object = nullptr;
  DllCanUnloadNowFn can_unload = nullptr;
  hosted::SetEngineConfigFn set_config = nullptr;
  hosted::ClearEngineConfigFn clear_config = nullptr;
  hosted::GetDiagnosticsFn get_diagnostics = nullptr;
  hosted::SetGateFn set_start_gate = nullptr;
  hosted::SetGateFn set_stop_gate = nullptr;
  hosted::SetGateFn set_cleanup_gate = nullptr;
  hosted::SetForceCleanupFailureFn set_force_cleanup_failure = nullptr;
  hosted::LeaseProbeFn lease_probe = nullptr;
};

bool CopyInto(const std::filesystem::path& source,
              const std::filesystem::path& destination) {
  std::error_code ec;
  std::filesystem::create_directories(destination.parent_path(), ec);
  if (ec) {
    std::printf("[tsf-hosted] FAIL fixture: cannot create %s (%s)\n",
                destination.parent_path().string().c_str(), ec.message().c_str());
    return false;
  }
  std::filesystem::copy_file(source, destination,
                             std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    std::printf("[tsf-hosted] FAIL fixture: %s -> %s (%s)\n",
                source.string().c_str(), destination.string().c_str(),
                ec.message().c_str());
    return false;
  }
  return true;
}

bool PrepareRunRoot(const Options& options, Env* env) {
  if (options.work_root.filename() != "tsf-host-run") {
    std::printf("[tsf-hosted] FAIL run-root leaf must be tsf-host-run, got %s\n",
                options.work_root.filename().string().c_str());
    return false;
  }
  std::error_code ec;
  std::filesystem::remove_all(options.work_root, ec);
  if (ec) {
    std::printf("[tsf-hosted] FAIL run-root remove: %s\n", ec.message().c_str());
    return false;
  }
  env->shared = options.work_root / "shared";
  env->user = options.work_root / "user";
  env->log = options.work_root / "log";
  env->log2 = options.work_root / "log2";
  for (const auto& dir : {env->shared, env->user, env->log, env->log2}) {
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      std::printf("[tsf-hosted] FAIL run-root create: %s\n", ec.message().c_str());
      return false;
    }
  }
  const auto completion = options.fixture_dir;
  const auto spelling = options.spelling_fixture_dir;
  const bool staged =
      CopyInto(completion / "default.yaml", env->shared / "default.yaml") &&
      CopyInto(completion / "epocanva_test_english.schema.yaml",
               env->shared / "epocanva_test_english.schema.yaml") &&
      CopyInto(completion / "lexicon.tsv",
               env->shared / "lexicon" / "epocanva_test_lexicon.tsv") &&
      CopyInto(spelling / "epocanva_spell_english.schema.yaml",
               env->shared / "epocanva_spell_english.schema.yaml") &&
      CopyInto(spelling / "lexicon_spell.tsv",
               env->shared / "lexicon" / "epocanva_spell_lexicon.tsv");
  return staged;
}

HRESULT Configure(const HostDll& dll, const Env& env,
                  const std::filesystem::path& rime_dll,
                  const std::filesystem::path& log) {
  hosted::EngineConfigW config{};
  config.size = sizeof(hosted::EngineConfigW);
  config.rime_dll_path = rime_dll.c_str();
  config.shared_data_dir = env.shared.c_str();
  config.user_data_dir = env.user.c_str();
  config.log_dir = log.c_str();
  config.capacity = 8;
  return dll.set_config(&config);
}

hosted::HostDiagnostics Diagnostics(const HostDll& dll) {
  hosted::HostDiagnostics diagnostics{};
  diagnostics.size = sizeof(hosted::HostDiagnostics);
  dll.get_diagnostics(&diagnostics);
  return diagnostics;
}

bool CanUnload(const HostDll& dll) { return dll.can_unload() == S_OK; }

void CheckUnload(const HostDll& dll, bool want_ok, const std::string& what) {
  const HRESULT verdict = dll.can_unload();
  Check(want_ok ? verdict == S_OK : verdict == S_FALSE, what,
        "DllCanUnloadNow=" + Hr(verdict) + (want_ok ? " want=S_OK" : " want=S_FALSE"));
}

void CheckState(const hosted::HostDiagnostics& diagnostics, hosted::HostStateValue want,
                const std::string& what) {
  Check(diagnostics.state == static_cast<std::int32_t>(want), what,
        "state=" + Num(diagnostics.state) +
            " want=" + Num(static_cast<std::int32_t>(want)));
}

bool LoadHostedDll(const Options& options, HostDll* dll) {
  dll->module = ::LoadLibraryW(options.dll.c_str());
  if (dll->module == nullptr) {
    std::printf("[tsf-hosted] stage load FAILED gle=%lu path=%ls\n",
                static_cast<unsigned long>(::GetLastError()), options.dll.c_str());
    return false;
  }
  wchar_t loaded[MAX_PATH] = {};
  const DWORD loaded_len = ::GetModuleFileNameW(dll->module, loaded, MAX_PATH);
  wchar_t wanted[MAX_PATH] = {};
  const DWORD wanted_len = ::GetFullPathNameW(options.dll.c_str(), MAX_PATH, wanted, nullptr);
  const bool same = loaded_len > 0 && wanted_len > 0 && wanted_len < MAX_PATH &&
                    _wcsicmp(loaded, wanted) == 0;
  Check(same, "the loaded module is exactly the test DLL path",
        same ? "same" : "different");
  if (!same) {
    return false;
  }

  dll->get_class_object = reinterpret_cast<DllGetClassObjectFn>(
      ::GetProcAddress(dll->module, "DllGetClassObject"));
  dll->can_unload = reinterpret_cast<DllCanUnloadNowFn>(
      ::GetProcAddress(dll->module, "DllCanUnloadNow"));
  dll->set_config = reinterpret_cast<hosted::SetEngineConfigFn>(
      ::GetProcAddress(dll->module, "EpoCanvaTestSetEngineConfig"));
  dll->clear_config = reinterpret_cast<hosted::ClearEngineConfigFn>(
      ::GetProcAddress(dll->module, "EpoCanvaTestClearEngineConfig"));
  dll->get_diagnostics = reinterpret_cast<hosted::GetDiagnosticsFn>(
      ::GetProcAddress(dll->module, "EpoCanvaTestGetDiagnostics"));
  dll->set_start_gate = reinterpret_cast<hosted::SetGateFn>(
      ::GetProcAddress(dll->module, "EpoCanvaTestSetStartGate"));
  dll->set_stop_gate = reinterpret_cast<hosted::SetGateFn>(
      ::GetProcAddress(dll->module, "EpoCanvaTestSetStopGate"));
  dll->set_cleanup_gate = reinterpret_cast<hosted::SetGateFn>(
      ::GetProcAddress(dll->module, "EpoCanvaTestSetCleanupGate"));
  dll->set_force_cleanup_failure = reinterpret_cast<hosted::SetForceCleanupFailureFn>(
      ::GetProcAddress(dll->module, "EpoCanvaTestSetForceCleanupFailure"));
  dll->lease_probe = reinterpret_cast<hosted::LeaseProbeFn>(
      ::GetProcAddress(dll->module, "EpoCanvaTestLeaseDoubleRelease"));

  const bool registration_exports =
      ::GetProcAddress(dll->module, "DllRegisterServer") != nullptr &&
      ::GetProcAddress(dll->module, "DllUnregisterServer") != nullptr;
  const bool product_ok = dll->get_class_object != nullptr && dll->can_unload != nullptr &&
                          registration_exports;
  const bool seam_ok = dll->set_config != nullptr && dll->clear_config != nullptr &&
                       dll->get_diagnostics != nullptr && dll->set_start_gate != nullptr &&
                       dll->set_stop_gate != nullptr && dll->set_cleanup_gate != nullptr &&
                       dll->set_force_cleanup_failure != nullptr &&
                       dll->lease_probe != nullptr;
  Check(dll->get_class_object != nullptr && dll->can_unload != nullptr,
        "DllGetClassObject and DllCanUnloadNow are exported");
  Check(seam_ok, "the private test seam exports are present");
  Check(registration_exports,
        "DllRegisterServer and DllUnregisterServer are exported");
  return product_ok && seam_ok;
}

bool CreateService(const HostDll& dll, ITfTextInputProcessorEx** service) {
  *service = nullptr;
  IClassFactory* factory = nullptr;
  HRESULT hr = dll.get_class_object(epocanva::tsf::kTextServiceClsid,
                                    __uuidof(IClassFactory),
                                    reinterpret_cast<LPVOID*>(&factory));
  if (FAILED(hr) || factory == nullptr) {
    return false;
  }
  hr = factory->CreateInstance(nullptr, __uuidof(ITfTextInputProcessorEx),
                               reinterpret_cast<void**>(service));
  factory->Release();
  return SUCCEEDED(hr) && *service != nullptr;
}

struct ThreadContext {
  bool com_initialized = false;
  bool thread_mgr_activated = false;
  ITfThreadMgr* tm = nullptr;
  TfClientId client_id = 0;
  ITfTextInputProcessorEx* service = nullptr;

  bool Create(const HostDll& dll, std::string* why) {
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
      *why = "CoInitializeEx " + Hr(hr);
      return false;
    }
    com_initialized = true;

    hr = ::CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
                            __uuidof(ITfThreadMgr),
                            reinterpret_cast<void**>(&tm));
    if (FAILED(hr) || tm == nullptr) {
      *why = "CoCreateInstance(CLSID_TF_ThreadMgr) " + Hr(hr);
      return false;
    }
    TfClientId app_client = 0;
    hr = tm->Activate(&app_client);
    if (FAILED(hr)) {
      *why = "ITfThreadMgr::Activate " + Hr(hr);
      return false;
    }
    thread_mgr_activated = true;

    ITfClientId* client_source = nullptr;
    hr = tm->QueryInterface(__uuidof(ITfClientId),
                            reinterpret_cast<void**>(&client_source));
    if (FAILED(hr) || client_source == nullptr) {
      *why = "QueryInterface(ITfClientId) " + Hr(hr);
      return false;
    }
    hr = client_source->GetClientId(epocanva::tsf::kTextServiceClsid, &client_id);
    client_source->Release();
    if (FAILED(hr)) {
      *why = "GetClientId " + Hr(hr);
      return false;
    }

    if (!CreateService(dll, &service)) {
      *why = "CreateInstance(ITfTextInputProcessorEx) failed";
      return false;
    }
    return true;
  }

  void Destroy() {
    if (service != nullptr) {
      service->Release();
      service = nullptr;
    }
    if (tm != nullptr) {
      if (thread_mgr_activated) {
        tm->Deactivate();
        thread_mgr_activated = false;
      }
      tm->Release();
      tm = nullptr;
    }
    if (com_initialized) {
      ::CoUninitialize();
      com_initialized = false;
    }
  }
};

void RunSingle(const HostDll& dll, const Options& options, const Env& env) {
  Check(dll.set_config != nullptr, "single: the test seam is available");
  const HRESULT configured = Configure(dll, env, options.rime_dll, env.log);
  CheckHr(configured, S_OK, "single: the test engine configuration is accepted");
  CheckUnload(dll, true, "single: a freshly loaded DLL is unloadable");

  ThreadContext context;
  std::string why;
  Check(context.Create(dll, &why), "single: real ThreadMgr and service created", why);
  if (context.service == nullptr) {
    context.Destroy();
    return;
  }

  CheckHr(context.service->Activate(context.tm, context.client_id), S_OK,
          "single: activation succeeds");
  hosted::HostDiagnostics diagnostics = Diagnostics(dll);
  CheckState(diagnostics, hosted::kHostRunning, "single: the broker is Running");
  Check(diagnostics.lease_count == 1, "single: one lease", Num(diagnostics.lease_count));
  Check(diagnostics.executor_instances_created == 1,
        "single: one executor instance", Num(diagnostics.executor_instances_created));
  Check(diagnostics.start_attempts == 1,
        "single: one start attempt", Num(diagnostics.start_attempts));
  Check(diagnostics.initialize_calls == 1,
        "single: Runtime started exactly once", Num(diagnostics.initialize_calls));
  Check(diagnostics.start_thread_id == diagnostics.worker_thread_id &&
            diagnostics.worker_thread_id != 0,
        "single: Runtime::Start ran on the engine worker",
        Num(diagnostics.start_thread_id) + " vs " + Num(diagnostics.worker_thread_id));
  Check(diagnostics.worker_thread_id != ::GetCurrentThreadId(),
        "single: the engine worker is not the activating STA thread",
        Num(diagnostics.worker_thread_id));
  Check(diagnostics.module_live_objects >= 1,
        "single: the service is a live module object",
        Num(diagnostics.module_live_objects));
  Check(diagnostics.host_active == 1, "single: the host-active guard is set",
        Num(diagnostics.host_active));
  Check(diagnostics.can_unload == 0, "single: the service blocks unload",
        Num(diagnostics.can_unload));
  CheckUnload(dll, false, "single: DllCanUnloadNow is S_FALSE while active");

  // Test 7: the same ThreadMgr is passed again. AddRef happens before the old Release, and
  // the new lease before the old lease release; counts must be unchanged.
  CheckHr(context.service->ActivateEx(context.tm, context.client_id, 0), S_OK,
          "single: repeated activation with the same ThreadMgr succeeds");
  diagnostics = Diagnostics(dll);
  Check(diagnostics.start_attempts == 1,
        "single: repeated activation did not start another runtime",
        Num(diagnostics.start_attempts));
  Check(diagnostics.executor_instances_created == 1,
        "single: repeated activation reused the executor",
        Num(diagnostics.executor_instances_created));
  Check(diagnostics.lease_count == 1,
        "single: repeated activation left exactly one lease",
        Num(diagnostics.lease_count));
  Check(diagnostics.initialize_calls == 1,
        "single: Runtime was not initialized twice", Num(diagnostics.initialize_calls));
  CheckUnload(dll, false, "single: the reactivated service still blocks unload");

  CheckHr(context.service->Deactivate(), S_OK, "single: Deactivate succeeds");
  diagnostics = Diagnostics(dll);
  CheckState(diagnostics, hosted::kHostIdle, "single: the broker is Idle after Deactivate");
  Check(diagnostics.lease_count == 0, "single: the lease is gone",
        Num(diagnostics.lease_count));
  Check(diagnostics.executor_present == 0, "single: the executor was destroyed",
        Num(diagnostics.executor_present));
  Check(diagnostics.finalize_calls == 1,
        "single: Runtime finalized exactly once", Num(diagnostics.finalize_calls));
  Check(diagnostics.shutdown_thread_id == diagnostics.worker_thread_id &&
            diagnostics.shutdown_thread_id != 0,
        "single: Runtime::Shutdown ran on the same worker",
        Num(diagnostics.shutdown_thread_id) + " vs " + Num(diagnostics.worker_thread_id));
  Check(diagnostics.live_sessions_after_cleanup == 0,
        "single: no session remains", Num(diagnostics.live_sessions_after_cleanup));
  Check(diagnostics.api_balanced == 1, "single: API acquire/free counters are balanced",
        Num(diagnostics.api_balanced));
  Check(diagnostics.host_active == 0, "single: the host-active guard is clear",
        Num(diagnostics.host_active));
  CheckUnload(dll, false, "single: the service object still blocks unload");

  CheckHr(context.service->Deactivate(), S_OK,
          "single: a second Deactivate is a no-op");
  diagnostics = Diagnostics(dll);
  Check(diagnostics.lease_count == 0,
        "single: the no-op Deactivate did not change the lease count",
        Num(diagnostics.lease_count));
  Check(diagnostics.release_underflows == 0,
        "single: the no-op Deactivate did not underflow the lease",
        Num(diagnostics.release_underflows));

  context.service->Release();
  context.service = nullptr;
  CheckUnload(dll, true, "single: all COM objects released, unloadable");
  diagnostics = Diagnostics(dll);
  Check(diagnostics.module_live_objects == 0,
        "single: no live module objects remain", Num(diagnostics.module_live_objects));
  context.Destroy();

  // Test 5: a later activation after a completed stop must use a new executor instance.
  ThreadContext second;
  Check(second.Create(dll, &why), "single: a second ThreadMgr and service created", why);
  if (second.service == nullptr) {
    second.Destroy();
    return;
  }
  CheckHr(second.service->Activate(second.tm, second.client_id), S_OK,
          "single: a later activation succeeds");
  diagnostics = Diagnostics(dll);
  Check(diagnostics.executor_instances_created == 2,
        "single: the later activation created a second executor",
        Num(diagnostics.executor_instances_created));
  Check(diagnostics.current_executor_instance == 2,
        "single: the current executor id advanced",
        Num(diagnostics.current_executor_instance));
  Check(diagnostics.start_attempts == 2,
        "single: the later activation is a new start attempt",
        Num(diagnostics.start_attempts));
  Check(diagnostics.initialize_calls == 1,
        "single: the new executor initialized once", Num(diagnostics.initialize_calls));
  CheckHr(second.service->Deactivate(), S_OK, "single: the later Deactivate succeeds");
  diagnostics = Diagnostics(dll);
  Check(diagnostics.finalize_calls == 1,
        "single: the new executor finalized once", Num(diagnostics.finalize_calls));
  CheckState(diagnostics, hosted::kHostIdle, "single: the broker is Idle again");
  second.service->Release();
  second.service = nullptr;
  CheckUnload(dll, true, "single: unloadable after the second lifecycle");
  second.Destroy();
}

void RunShared(const HostDll& dll, const Options& options, const Env& env) {
  CheckHr(Configure(dll, env, options.rime_dll, env.log), S_OK,
          "shared: the test engine configuration is accepted");
  ThreadContext context;
  std::string why;
  Check(context.Create(dll, &why), "shared: ThreadMgr and first service created", why);
  if (context.service == nullptr) {
    context.Destroy();
    return;
  }
  ITfTextInputProcessorEx* second_service = nullptr;
  Check(CreateService(dll, &second_service), "shared: a second service was created");
  if (second_service == nullptr) {
    context.Destroy();
    return;
  }

  CheckHr(context.service->Activate(context.tm, context.client_id), S_OK,
          "shared: first service activation succeeds");
  CheckHr(second_service->Activate(context.tm, context.client_id), S_OK,
          "shared: second service activation succeeds");
  hosted::HostDiagnostics diagnostics = Diagnostics(dll);
  CheckState(diagnostics, hosted::kHostRunning, "shared: the broker is Running");
  Check(diagnostics.lease_count == 2, "shared: two leases are held",
        Num(diagnostics.lease_count));
  Check(diagnostics.executor_instances_created == 1,
        "shared: both services share one executor",
        Num(diagnostics.executor_instances_created));
  Check(diagnostics.start_attempts == 1,
        "shared: only one engine start occurred", Num(diagnostics.start_attempts));
  Check(diagnostics.initialize_calls == 1,
        "shared: Runtime was initialized once", Num(diagnostics.initialize_calls));

  CheckHr(context.service->Deactivate(), S_OK,
          "shared: the first service deactivates");
  diagnostics = Diagnostics(dll);
  CheckState(diagnostics, hosted::kHostRunning,
             "shared: the broker stays Running for the other service");
  Check(diagnostics.lease_count == 1, "shared: one lease remains",
        Num(diagnostics.lease_count));
  Check(diagnostics.executor_present == 1, "shared: the executor is still alive",
        Num(diagnostics.executor_present));
  Check(diagnostics.finalize_calls == 0,
        "shared: Runtime was not finalized", Num(diagnostics.finalize_calls));
  CheckHr(second_service->ActivateEx(context.tm, context.client_id, 0), S_OK,
          "shared: the remaining service is still usable");
  diagnostics = Diagnostics(dll);
  Check(diagnostics.lease_count == 1,
        "shared: reactivating the remaining service keeps one lease",
        Num(diagnostics.lease_count));
  Check(diagnostics.start_attempts == 1,
        "shared: reactivation did not restart the engine",
        Num(diagnostics.start_attempts));

  // Test 6: a different environment is refused and the active service survives.
  CheckHr(Configure(dll, env, options.rime_dll, env.log2), S_OK,
          "shared: an alternative configuration is stored");
  ITfTextInputProcessorEx* third_service = nullptr;
  Check(CreateService(dll, &third_service), "shared: a third service was created");
  if (third_service != nullptr) {
    const HRESULT mismatch = third_service->Activate(context.tm, context.client_id);
    CheckHr(mismatch, E_UNEXPECTED,
            "shared: a different configuration is refused with E_UNEXPECTED");
    diagnostics = Diagnostics(dll);
    CheckState(diagnostics, hosted::kHostRunning,
               "shared: the refusal did not disturb the running broker");
    Check(diagnostics.lease_count == 1, "shared: the existing lease is unchanged",
          Num(diagnostics.lease_count));
    Check(diagnostics.config_mismatches == 1,
          "shared: the mismatch is recorded", Num(diagnostics.config_mismatches));
    Check(diagnostics.start_attempts == 1,
          "shared: the mismatch started nothing", Num(diagnostics.start_attempts));
    CheckHr(third_service->Deactivate(), S_OK,
            "shared: the refused service deactivates as a no-op");
    third_service->Release();
  }

  CheckHr(Configure(dll, env, options.rime_dll, env.log), S_OK,
          "shared: the original configuration is restored");
  CheckHr(second_service->ActivateEx(context.tm, context.client_id, 0), S_OK,
          "shared: the original service still activates after the refusal");
  CheckHr(second_service->Deactivate(), S_OK,
          "shared: the last service deactivates");
  diagnostics = Diagnostics(dll);
  CheckState(diagnostics, hosted::kHostIdle,
             "shared: the broker is Idle after the last lease");
  Check(diagnostics.lease_count == 0, "shared: no lease remains",
        Num(diagnostics.lease_count));
  Check(diagnostics.finalize_calls == 1,
        "shared: the runtime finalized once", Num(diagnostics.finalize_calls));

  second_service->Release();
  context.service->Release();
  context.service = nullptr;
  CheckUnload(dll, true, "shared: unloadable after both service objects are gone");
  context.Destroy();
}

void RunMultiSta(const HostDll& dll, const Options& options, const Env& env) {
  CheckHr(Configure(dll, env, options.rime_dll, env.log), S_OK,
          "multista: the test engine configuration is accepted");
  CheckUnload(dll, true, "multista: the DLL is unloadable before any object");

  struct Slot {
    const HostDll* dll = nullptr;
    ThreadContext context;
    bool create_ok = false;
    HRESULT activate = E_FAIL;
    HRESULT deactivate = E_FAIL;
    std::string why;
  };
  Slot slots[2];
  slots[0].dll = &dll;
  slots[1].dll = &dll;
  Barrier barrier(3);

  auto worker = [&](int index) {
    Slot& slot = slots[index];
    slot.create_ok = slot.context.Create(*slot.dll, &slot.why);
    if (slot.create_ok && slot.context.service != nullptr) {
      slot.activate = slot.context.service->Activate(slot.context.tm,
                                                     slot.context.client_id);
    }
    barrier.Wait();  // all activated
    barrier.Wait();  // main finished checking

    if (index == 0 && slot.create_ok && slot.context.service != nullptr) {
      slot.deactivate = slot.context.service->Deactivate();
    }
    barrier.Wait();  // first deactivation done
    barrier.Wait();  // main finished checking

    if (index == 1 && slot.create_ok && slot.context.service != nullptr) {
      slot.deactivate = slot.context.service->Deactivate();
    }
    barrier.Wait();  // second deactivation done
    barrier.Wait();  // main finished checking

    slot.context.Destroy();  // releases this thread's own COM objects on this thread
  };

  std::thread first(worker, 0);
  std::thread second(worker, 1);

  barrier.Wait();
  Check(slots[0].create_ok, "multista: thread 0 created its service", slots[0].why);
  Check(slots[1].create_ok, "multista: thread 1 created its service", slots[1].why);
  CheckHr(slots[0].activate, S_OK, "multista: thread 0 activation succeeds");
  CheckHr(slots[1].activate, S_OK, "multista: thread 1 activation succeeds");
  hosted::HostDiagnostics diagnostics = Diagnostics(dll);
  CheckState(diagnostics, hosted::kHostRunning, "multista: the shared broker is Running");
  Check(diagnostics.lease_count == 2, "multista: two leases from two apartments",
        Num(diagnostics.lease_count));
  Check(diagnostics.executor_instances_created == 1,
        "multista: both apartments share one executor",
        Num(diagnostics.executor_instances_created));
  Check(diagnostics.start_attempts == 1,
        "multista: one Runtime start for both apartments",
        Num(diagnostics.start_attempts));
  Check(diagnostics.initialize_calls == 1,
        "multista: Runtime initialized once", Num(diagnostics.initialize_calls));
  Check(diagnostics.module_live_objects >= 2,
        "multista: both service objects are live",
        Num(diagnostics.module_live_objects));
  CheckUnload(dll, false, "multista: the services block unload");

  barrier.Wait();  // release both threads to the next phase
  barrier.Wait();  // thread 0 deactivated
  Check(slots[0].deactivate == S_OK, "multista: thread 0 deactivation succeeds",
        Hr(slots[0].deactivate));
  Check(slots[1].deactivate == E_FAIL,
        "multista: thread 1 has not deactivated yet", Hr(slots[1].deactivate));
  diagnostics = Diagnostics(dll);
  CheckState(diagnostics, hosted::kHostRunning,
             "multista: one apartment remains active");
  Check(diagnostics.lease_count == 1, "multista: one lease remains",
        Num(diagnostics.lease_count));
  Check(diagnostics.executor_present == 1,
        "multista: the shared executor is still alive",
        Num(diagnostics.executor_present));
  CheckUnload(dll, false, "multista: the remaining service blocks unload");

  barrier.Wait();  // release to the second deactivation
  barrier.Wait();  // thread 1 deactivated
  Check(slots[1].deactivate == S_OK, "multista: thread 1 deactivation succeeds",
        Hr(slots[1].deactivate));
  diagnostics = Diagnostics(dll);
  CheckState(diagnostics, hosted::kHostIdle,
             "multista: the shared broker is Idle");
  Check(diagnostics.lease_count == 0, "multista: no lease remains",
        Num(diagnostics.lease_count));
  Check(diagnostics.finalize_calls == 1,
        "multista: Runtime finalized exactly once", Num(diagnostics.finalize_calls));
  Check(diagnostics.start_thread_id == diagnostics.worker_thread_id &&
            diagnostics.shutdown_thread_id == diagnostics.worker_thread_id,
        "multista: Start and Shutdown ran on the one worker",
        Num(diagnostics.start_thread_id) + "/" +
            Num(diagnostics.shutdown_thread_id) + " vs " +
            Num(diagnostics.worker_thread_id));
  Check(diagnostics.host_active == 0,
        "multista: the host-active guard is clear", Num(diagnostics.host_active));

  barrier.Wait();  // let both threads destroy their own contexts
  first.join();
  second.join();
  diagnostics = Diagnostics(dll);
  Check(diagnostics.module_live_objects == 0,
        "multista: every COM object was released",
        Num(diagnostics.module_live_objects));
  CheckUnload(dll, true, "multista: unloadable after both apartments unwind");
}

void RunUnloadWindow(const HostDll& dll, const Options& options, const Env& env) {
  CheckHr(Configure(dll, env, options.rime_dll, env.log), S_OK,
          "unload: the test engine configuration is accepted");
  CheckUnload(dll, true, "unload: no object yet, unloadable");

  // The main STA owns a service used only to attempt Acquire during Stopping. The worker
  // STA owns the real activation and never shares a COM pointer with the main thread.
  ThreadContext main_context;
  std::string why;
  Check(main_context.Create(dll, &why),
        "unload: the main ThreadMgr and rejection service exist", why);
  if (main_context.service == nullptr) {
    main_context.Destroy();
    return;
  }

  Event start_entered;
  Event start_release;
  Event stop_entered;
  Event stop_release;
  Event cleanup_entered;
  Event cleanup_release;
  Event activate_done;
  Event deactivate_done;
  Event do_deactivate;
  Check(start_entered.valid() && start_release.valid() && stop_entered.valid() &&
            stop_release.valid() && cleanup_entered.valid() &&
            cleanup_release.valid() && activate_done.valid() &&
            deactivate_done.valid() && do_deactivate.valid(),
        "unload: every lifecycle event exists");
  dll.set_start_gate(start_entered.handle, start_release.handle);
  dll.set_stop_gate(stop_entered.handle, stop_release.handle);
  dll.set_cleanup_gate(cleanup_entered.handle, cleanup_release.handle);

  struct Worker {
    const HostDll* dll = nullptr;
    ThreadContext context;
    HRESULT activate = E_FAIL;
    HRESULT deactivate = E_FAIL;
    std::string why;
  } worker_state;
  worker_state.dll = &dll;

  std::thread worker([&] {
    Worker& state = worker_state;
    if (!state.context.Create(*state.dll, &state.why)) {
      activate_done.Set();
      deactivate_done.Set();
      return;
    }
    state.activate = state.context.service->Activate(state.context.tm,
                                                     state.context.client_id);
    activate_done.Set();
    (void)do_deactivate.Wait();
    state.deactivate = state.context.service->Deactivate();
    deactivate_done.Set();
    state.context.Destroy();
  });

  Check(start_entered.Wait(), "unload: activation reached the start gate");
  CheckUnload(dll, false, "unload: DllCanUnloadNow is S_FALSE during Starting");
  hosted::HostDiagnostics diagnostics = Diagnostics(dll);
  CheckState(diagnostics, hosted::kHostStarting, "unload: the broker is Starting");
  Check(diagnostics.lease_count == 0, "unload: no lease before Start succeeds",
        Num(diagnostics.lease_count));
  Check(diagnostics.executor_present == 1, "unload: the executor exists in Starting",
        Num(diagnostics.executor_present));
  Check(diagnostics.host_active == 1, "unload: the host-active guard covers Starting",
        Num(diagnostics.host_active));
  Check(diagnostics.can_unload == 0, "unload: the engine start blocks unload",
        Num(diagnostics.can_unload));
  const std::uint64_t attempts_before = diagnostics.start_attempts;

  start_release.Set();
  Check(activate_done.Wait(), "unload: activation completed");
  CheckHr(worker_state.activate, S_OK, "unload: the worker activation succeeds");
  diagnostics = Diagnostics(dll);
  CheckState(diagnostics, hosted::kHostRunning, "unload: the broker is Running");
  Check(diagnostics.lease_count == 1, "unload: one lease", Num(diagnostics.lease_count));
  Check(diagnostics.initialize_calls == 1,
        "unload: Runtime initialized once", Num(diagnostics.initialize_calls));

  do_deactivate.Set();
  Check(stop_entered.Wait(), "unload: deactivation reached the stop gate");
  CheckUnload(dll, false, "unload: DllCanUnloadNow is S_FALSE during Stopping");
  diagnostics = Diagnostics(dll);
  CheckState(diagnostics, hosted::kHostStopping, "unload: the broker is Stopping");
  Check(diagnostics.lease_count == 0, "unload: the last lease is gone",
        Num(diagnostics.lease_count));
  Check(diagnostics.executor_present == 1,
        "unload: the executor is still present in Stopping",
        Num(diagnostics.executor_present));
  Check(diagnostics.host_active == 1, "unload: the host-active guard covers Stopping",
        Num(diagnostics.host_active));

  // Test 13 at the service boundary: Acquire while the stop is held must fail, not wait.
  const HRESULT rejected = main_context.service->Activate(main_context.tm,
                                                          main_context.client_id);
  CheckHr(rejected, E_UNEXPECTED,
          "unload: activating a service during Stopping is refused");
  diagnostics = Diagnostics(dll);
  Check(diagnostics.acquire_rejections_stopping == 1,
        "unload: the refusal is counted", Num(diagnostics.acquire_rejections_stopping));
  Check(diagnostics.start_attempts == attempts_before,
        "unload: the refusal started no new runtime",
        Num(diagnostics.start_attempts));
  CheckHr(main_context.service->Deactivate(), S_OK,
          "unload: the refused service deactivates as a no-op");

  stop_release.Set();
  Check(cleanup_entered.Wait(), "unload: deactivation reached the cleanup window");
  CheckUnload(dll, false, "unload: DllCanUnloadNow is S_FALSE in cleanup");
  diagnostics = Diagnostics(dll);
  CheckState(diagnostics, hosted::kHostStopping,
             "unload: the state is still Stopping in the cleanup window");
  Check(diagnostics.executor_present == 1,
        "unload: the executor has not been destroyed yet",
        Num(diagnostics.executor_present));
  Check(diagnostics.finalize_calls == 1,
        "unload: Runtime finalized before the cleanup gate",
        Num(diagnostics.finalize_calls));
  Check(diagnostics.shutdown_thread_id == diagnostics.worker_thread_id,
        "unload: Shutdown ran on the worker", Num(diagnostics.shutdown_thread_id));
  Check(diagnostics.host_active == 1, "unload: the host-active guard covers cleanup",
        Num(diagnostics.host_active));

  cleanup_release.Set();
  Check(deactivate_done.Wait(), "unload: deactivation completed");
  CheckHr(worker_state.deactivate, S_OK, "unload: Deactivate returned S_OK");
  diagnostics = Diagnostics(dll);
  CheckState(diagnostics, hosted::kHostIdle, "unload: the broker is Idle");
  Check(diagnostics.executor_present == 0, "unload: the executor was destroyed",
        Num(diagnostics.executor_present));
  Check(diagnostics.host_active == 0, "unload: the host-active guard is clear",
        Num(diagnostics.host_active));
  Check(diagnostics.finalize_calls == 1,
        "unload: Runtime finalized exactly once", Num(diagnostics.finalize_calls));
  CheckUnload(dll, false, "unload: the service object still blocks unload");

  worker.join();
  main_context.service->Release();
  main_context.service = nullptr;
  CheckUnload(dll, true, "unload: all objects released, unloadable");
  main_context.Destroy();
  dll.set_start_gate(nullptr, nullptr);
  dll.set_stop_gate(nullptr, nullptr);
  dll.set_cleanup_gate(nullptr, nullptr);
}

void RunCleanupFailure(const HostDll& dll, const Options& options, const Env& env) {
  CheckHr(Configure(dll, env, options.rime_dll, env.log), S_OK,
          "cleanup: the test engine configuration is accepted");
  dll.set_force_cleanup_failure(1);

  ThreadContext context;
  std::string why;
  Check(context.Create(dll, &why), "cleanup: ThreadMgr and service created", why);
  if (context.service == nullptr) {
    dll.set_force_cleanup_failure(0);
    context.Destroy();
    return;
  }
  CheckHr(context.service->Activate(context.tm, context.client_id), S_OK,
          "cleanup: activation succeeds");
  CheckHr(context.service->Deactivate(), S_OK,
          "cleanup: deactivation returns S_OK");
  dll.set_force_cleanup_failure(0);

  hosted::HostDiagnostics diagnostics = Diagnostics(dll);
  CheckState(diagnostics, hosted::kHostFailed,
             "cleanup: the broker is Failed, never a false Idle");
  Check(diagnostics.cleanup_failed == 1,
        "cleanup: the cleanup failure is latched", Num(diagnostics.cleanup_failed));
  Check(diagnostics.stop_failures == 1,
        "cleanup: one stop failure is recorded", Num(diagnostics.stop_failures));
  Check(diagnostics.lease_count == 0, "cleanup: no lease remains",
        Num(diagnostics.lease_count));
  Check(diagnostics.executor_present == 0,
        "cleanup: the executor object was destroyed after the join",
        Num(diagnostics.executor_present));
  Check(diagnostics.finalize_calls == 1,
        "cleanup: Runtime finalized before the injected failure",
        Num(diagnostics.finalize_calls));
  Check(diagnostics.host_failed == 1,
        "cleanup: the module failure latch is set", Num(diagnostics.host_failed));
  CheckUnload(dll, false, "cleanup: unload is refused while the service lives");

  context.service->Release();
  context.service = nullptr;
  diagnostics = Diagnostics(dll);
  Check(diagnostics.module_live_objects == 0,
        "cleanup: no live COM object remains", Num(diagnostics.module_live_objects));
  Check(diagnostics.module_server_locks == 0,
        "cleanup: no server lock remains", Num(diagnostics.module_server_locks));
  Check(diagnostics.can_unload == 0,
        "cleanup: the failure latch alone blocks unload",
        Num(diagnostics.can_unload));
  CheckUnload(dll, false,
              "cleanup: DllCanUnloadNow stays S_FALSE with zero objects and locks");
  context.Destroy();
}

void RunStartupFailures(const HostDll& dll, const Options& options, const Env& env) {
  const std::filesystem::path missing =
      options.work_root / "missing" / "rime.dll";
  CheckHr(Configure(dll, env, missing, env.log), S_OK,
          "startup: the missing-DLL configuration is accepted");
  ThreadContext context;
  std::string why;
  Check(context.Create(dll, &why), "startup: ThreadMgr and service created", why);
  if (context.service == nullptr) {
    context.Destroy();
    return;
  }
  CheckHr(context.service->Activate(context.tm, context.client_id), E_FAIL,
          "startup: the missing DLL reports E_FAIL");
  hosted::HostDiagnostics diagnostics = Diagnostics(dll);
  CheckState(diagnostics, hosted::kHostIdle,
             "startup: the missing DLL left the broker Idle");
  Check(diagnostics.start_attempts == 1, "startup: one attempt",
        Num(diagnostics.start_attempts));
  Check(diagnostics.start_failures == 1, "startup: one failure",
        Num(diagnostics.start_failures));
  Check(diagnostics.executor_present == 0,
        "startup: the failed executor was destroyed",
        Num(diagnostics.executor_present));
  Check(diagnostics.lease_count == 0, "startup: no lease leaked",
        Num(diagnostics.lease_count));
  Check(diagnostics.runtime_start_status ==
            static_cast<std::int32_t>(engine::RuntimeStartStatus::kDllLoadFailed),
        "startup: the real kDllLoadFailed status is reported",
        Num(diagnostics.runtime_start_status));
  Check(diagnostics.host_active == 0,
        "startup: the start window is closed", Num(diagnostics.host_active));
  CheckHr(context.service->Deactivate(), S_OK,
          "startup: Deactivate after a failed activation is a no-op");
  context.service->Release();
  context.service = nullptr;
  CheckUnload(dll, true, "startup: unloadable after the missing-DLL cleanup");
  context.Destroy();

  std::error_code ec;
  if (!options.baseline_dll.empty() &&
      std::filesystem::is_regular_file(options.baseline_dll, ec) && !ec) {
    CheckHr(Configure(dll, env, options.baseline_dll, env.log), S_OK,
            "startup: the baseline configuration is accepted");
    ThreadContext baseline;
    Check(baseline.Create(dll, &why),
          "startup: ThreadMgr and service created for the baseline DLL", why);
    if (baseline.service != nullptr) {
      CheckHr(baseline.service->Activate(baseline.tm, baseline.client_id), E_FAIL,
              "startup: the missing English module reports E_FAIL");
      diagnostics = Diagnostics(dll);
      CheckState(diagnostics, hosted::kHostIdle,
                 "startup: the missing module left the broker Idle");
      Check(diagnostics.start_failures == 2, "startup: two failures are recorded",
            Num(diagnostics.start_failures));
      Check(diagnostics.runtime_start_status ==
                static_cast<std::int32_t>(
                    engine::RuntimeStartStatus::kEnglishModuleMissing),
            "startup: the real kEnglishModuleMissing status is reported",
            Num(diagnostics.runtime_start_status));
      CheckHr(baseline.service->Deactivate(), S_OK,
              "startup: the failed baseline service deactivates");
      baseline.service->Release();
      baseline.service = nullptr;
      CheckUnload(dll, true, "startup: unloadable after the missing-module cleanup");
    }
    baseline.Destroy();
  } else {
    std::printf("[tsf-hosted] UNVERIFIED startup-missing-module: no baseline rime.dll "
                "was supplied; the missing-DLL cleanup above still ran\n");
  }

  CheckHr(Configure(dll, env, options.rime_dll, env.log), S_OK,
          "startup: the good configuration is restored");
  ThreadContext good;
  Check(good.Create(dll, &why), "startup: ThreadMgr and service created for retry", why);
  if (good.service != nullptr) {
    CheckHr(good.service->Activate(good.tm, good.client_id), S_OK,
            "startup: the retry starts a new executor");
    diagnostics = Diagnostics(dll);
    Check(diagnostics.initialize_calls == 1,
          "startup: the new executor initialized once",
          Num(diagnostics.initialize_calls));
    Check(diagnostics.lease_count == 1, "startup: one lease after the retry",
          Num(diagnostics.lease_count));
    CheckHr(good.service->Deactivate(), S_OK, "startup: the retry deactivates");
    diagnostics = Diagnostics(dll);
    Check(diagnostics.finalize_calls == 1,
          "startup: the retry finalized once", Num(diagnostics.finalize_calls));
    good.service->Release();
    good.service = nullptr;
    CheckUnload(dll, true, "startup: unloadable after the successful retry");
  }
  good.Destroy();
}

void RunLeaseProbe(const HostDll& dll, const Options& options, const Env& env) {
  CheckHr(Configure(dll, env, options.rime_dll, env.log), S_OK,
          "lease: the test engine configuration is accepted");
  hosted::LeaseProbe probe{};
  probe.size = sizeof(hosted::LeaseProbe);
  dll.lease_probe(&probe);

  Check(probe.acquire_status == static_cast<std::int32_t>(hosted::kLeaseAcquired),
        "lease: a broker lease is acquired", Num(probe.acquire_status));
  Check(probe.lease_after_acquire == 1, "lease: one lease after acquire",
        Num(probe.lease_after_acquire));
  Check(probe.can_unload_after_acquire == 0,
        "lease: an engine lease blocks unload", Num(probe.can_unload_after_acquire));
  Check(probe.first_release_status == static_cast<std::int32_t>(hosted::kLeaseReleased),
        "lease: the first release succeeds", Num(probe.first_release_status));
  Check(probe.lease_after_first_release == 0,
        "lease: the count is zero after the first release",
        Num(probe.lease_after_first_release));
  Check(probe.can_unload_after_first_release == 1,
        "lease: unload is possible after the first release",
        Num(probe.can_unload_after_first_release));
  Check(probe.second_release_status ==
            static_cast<std::int32_t>(hosted::kLeaseAlreadyReleased),
        "lease: the handle refuses a second release",
        Num(probe.second_release_status));
  Check(probe.lease_after_second_release == 0,
        "lease: the count is still zero, never wrapped",
        Num(probe.lease_after_second_release));
  Check(probe.broker_duplicate_status ==
            static_cast<std::int32_t>(hosted::kLeaseAlreadyReleased),
        "lease: the broker also refuses the duplicate id",
        Num(probe.broker_duplicate_status));
  Check(probe.release_underflows_after_second == 1,
        "lease: the broker counted exactly one refused duplicate",
        Num(probe.release_underflows_after_second));
  const hosted::HostDiagnostics diagnostics = Diagnostics(dll);
  CheckState(diagnostics, hosted::kHostIdle, "lease: the broker is Idle");
  Check(diagnostics.finalize_calls == 1,
        "lease: Runtime finalized once", Num(diagnostics.finalize_calls));
  CheckUnload(dll, true, "lease: DllCanUnloadNow is S_OK after the probe");
}

int RunSelfCheckFail(const HostDll& dll) {
  std::printf("[tsf-hosted] mode self-check-fail: one deliberately false claim\n");
  const hosted::HostDiagnostics diagnostics = Diagnostics(dll);
  Check(diagnostics.state == static_cast<std::int32_t>(hosted::kHostRunning),
        "deliberately asserted that a fresh hosted DLL is Running (it is Idle)");
  std::printf("[tsf-hosted] RESULT FAIL (self-check-fail control, %d checks, %d failed)\n",
              g_total, g_failed);
  return g_failed == 0 ? kExitOk : kExitSelfCheckFailed;
}

bool ParseArgs(int argc, wchar_t** argv, Options* options) {
  bool have_mode = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    auto next_path = [&](std::filesystem::path* target) {
      if (i + 1 >= argc) {
        return false;
      }
      *target = std::filesystem::path(argv[++i]);
      return true;
    };
    if (arg == L"--mode" && i + 1 < argc) {
      const std::wstring wide = argv[++i];
      options->mode.assign(wide.size(), '\0');
      bool ascii = true;
      for (std::size_t c = 0; c < wide.size(); ++c) {
        options->mode[c] = static_cast<char>(wide[c]);
        if (wide[c] > 0x7F) {
          ascii = false;
        }
      }
      if (!ascii) {
        return false;
      }
      have_mode = true;
    } else if (arg == L"--dll") {
      if (!next_path(&options->dll)) return false;
    } else if (arg == L"--rime-dll") {
      if (!next_path(&options->rime_dll)) return false;
    } else if (arg == L"--baseline-dll") {
      if (!next_path(&options->baseline_dll)) return false;
    } else if (arg == L"--fixture-dir") {
      if (!next_path(&options->fixture_dir)) return false;
    } else if (arg == L"--spelling-fixture-dir") {
      if (!next_path(&options->spelling_fixture_dir)) return false;
    } else if (arg == L"--work-root") {
      if (!next_path(&options->work_root)) return false;
    } else {
      return false;
    }
  }
  return have_mode && !options->dll.empty();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  Options options;
  if (!ParseArgs(argc, argv, &options)) {
    std::printf("usage: tsf_hosted_tests --dll <hosted test DLL> "
                "--mode single|shared|multista|unload|cleanup|startup|lease|"
                "self-check-fail --rime-dll <path> --baseline-dll <path> "
                "--fixture-dir <dir> --spelling-fixture-dir <dir> "
                "--work-root <dir>\n");
    return kExitUsage;
  }

  if (options.mode != "self-check-fail") {
    if (options.rime_dll.empty() || options.fixture_dir.empty() ||
        options.spelling_fixture_dir.empty() || options.work_root.empty()) {
      std::printf("[tsf-hosted] missing required test paths\n");
      return kExitUsage;
    }
  }

  Env env;
  if (options.mode != "self-check-fail" && !PrepareRunRoot(options, &env)) {
    return kExitCheckFailed;
  }

  HostDll dll;
  if (!LoadHostedDll(options, &dll)) {
    if (dll.module != nullptr) {
      ::FreeLibrary(dll.module);
    }
    return kExitLoadFailed;
  }
  if (options.mode == "self-check-fail") {
    const int result = RunSelfCheckFail(dll);
    ::FreeLibrary(dll.module);
    return result;
  }

  if (options.mode == "single") {
    RunSingle(dll, options, env);
  } else if (options.mode == "shared") {
    RunShared(dll, options, env);
  } else if (options.mode == "multista") {
    RunMultiSta(dll, options, env);
  } else if (options.mode == "unload") {
    RunUnloadWindow(dll, options, env);
  } else if (options.mode == "cleanup") {
    RunCleanupFailure(dll, options, env);
  } else if (options.mode == "startup") {
    RunStartupFailures(dll, options, env);
  } else if (options.mode == "lease") {
    RunLeaseProbe(dll, options, env);
  } else {
    std::printf("[tsf-hosted] unknown mode: %s\n", options.mode.c_str());
    ::FreeLibrary(dll.module);
    return kExitUsage;
  }

  ::FreeLibrary(dll.module);
  std::printf("[tsf-hosted] RESULT %s (%d checks, %d failed)\n",
              g_failed == 0 ? "PASS" : "FAIL", g_total, g_failed);
  return g_failed == 0 ? kExitOk : kExitCheckFailed;
}
