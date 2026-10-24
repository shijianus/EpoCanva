// EpoCanva P3.6 -- broker/lease verification without TSF or COM.
//
// This program links the production EngineHostBroker (platforms/windows/tsf/engine_host.cc)
// and the real English rime.dll through EngineExecutor. It exercises the lifetime contract
// the TSF front end will consume in the second commit:
//
//   one start for the first lease, shared leases, last-release stop+join, a new executor
//   after a clean stop, configuration mismatch, Acquire rejection during Stopping, clean
//   startup failure with retry, missing-module failure with retry, lease double-release,
//   one starter with a waiting Acquire, and the unresolved-cleanup latch.
//
// DllCanUnloadNow is not called here: this executable has no DLL. It calls the same
// ModuleCanUnload() predicate the DLL export uses, so the host-active/failed flags are
// verified before the TSF module is built.
//
// NOT AN ENGINE CAPABILITY TEST. There are no clients, no keys and no host text. Runtime
// initialize/finalize counts and thread ids are lifecycle witnesses only.
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "engine_host.h"
#include "module_state.h"
#include "rime_runtime.h"

namespace {

namespace engine = epocanva::windows::engine;
using epocanva::tsf::EngineHostBroker;
using epocanva::tsf::EngineHostConfig;
using epocanva::tsf::EngineHostDiagnostics;
using epocanva::tsf::EngineHostState;
using epocanva::tsf::EngineLease;
using epocanva::tsf::EngineLeaseReleaseStatus;
using epocanva::tsf::EngineLeaseStatus;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitCheckFailed = 5;
constexpr int kExitSelfCheckFailed = 14;
constexpr DWORD kWaitMs = 20000;

int g_total = 0;
int g_failed = 0;

void Check(bool condition, const std::string& what,
           const std::string& got = std::string()) {
  ++g_total;
  if (condition) {
    std::printf("[tsf-host] ok   %s\n", what.c_str());
  } else {
    ++g_failed;
    if (got.empty()) {
      std::printf("[tsf-host] FAIL %s\n", what.c_str());
    } else {
      std::printf("[tsf-host] FAIL %s  (got: %s)\n", what.c_str(), got.c_str());
    }
  }
}

template <typename T>
std::string Num(T value) {
  return std::to_string(static_cast<unsigned long long>(value));
}

std::string Bool(bool value) { return value ? "true" : "false"; }

EngineHostDiagnostics Snapshot() {
  EngineHostDiagnostics diagnostics;
  EngineHostBroker::Instance().Snapshot(&diagnostics);
  return diagnostics;
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
  std::filesystem::path rime_dll;
  std::filesystem::path baseline_dll;
  std::filesystem::path fixture_dir;
  std::filesystem::path spelling_fixture_dir;
  std::filesystem::path work_root;
};

bool CopyInto(const std::filesystem::path& source,
              const std::filesystem::path& destination) {
  std::error_code ec;
  std::filesystem::create_directories(destination.parent_path(), ec);
  if (ec) {
    std::printf("[tsf-host] FAIL fixture: cannot create %s (%s)\n",
                destination.parent_path().string().c_str(), ec.message().c_str());
    return false;
  }
  std::filesystem::copy_file(source, destination,
                             std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    std::printf("[tsf-host] FAIL fixture: %s -> %s (%s)\n",
                source.string().c_str(), destination.string().c_str(),
                ec.message().c_str());
    return false;
  }
  return true;
}

bool PrepareRunRoot(const Options& options, std::filesystem::path* shared,
                    std::filesystem::path* user, std::filesystem::path* log) {
  if (options.work_root.filename() != "tsf-host-run") {
    std::printf("[tsf-host] FAIL run-root leaf must be tsf-host-run, got %s\n",
                options.work_root.filename().string().c_str());
    return false;
  }
  std::error_code ec;
  std::filesystem::remove_all(options.work_root, ec);
  if (ec) {
    std::printf("[tsf-host] FAIL run-root remove: %s\n", ec.message().c_str());
    return false;
  }
  *shared = options.work_root / "shared";
  *user = options.work_root / "user";
  *log = options.work_root / "log";
  for (const auto& dir : {*shared, *user, *log}) {
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      std::printf("[tsf-host] FAIL run-root create: %s\n", ec.message().c_str());
      return false;
    }
  }
  const auto completion = options.fixture_dir;
  const auto spelling = options.spelling_fixture_dir;
  const bool staged =
      CopyInto(completion / "default.yaml", *shared / "default.yaml") &&
      CopyInto(completion / "epocanva_test_english.schema.yaml",
               *shared / "epocanva_test_english.schema.yaml") &&
      CopyInto(completion / "lexicon.tsv",
               *shared / "lexicon" / "epocanva_test_lexicon.tsv") &&
      CopyInto(spelling / "epocanva_spell_english.schema.yaml",
               *shared / "epocanva_spell_english.schema.yaml") &&
      CopyInto(spelling / "lexicon_spell.tsv",
               *shared / "lexicon" / "epocanva_spell_lexicon.tsv");
  if (!staged) {
    return false;
  }
  std::filesystem::create_directories(options.work_root / "log2", ec);
  return !ec;
}

EngineHostConfig MakeConfig(const Options& options,
                            const std::filesystem::path& shared,
                            const std::filesystem::path& user,
                            const std::filesystem::path& log,
                            std::size_t capacity = 8) {
  EngineHostConfig config;
  config.rime_dll_path = options.rime_dll;
  config.shared_data_dir = shared;
  config.user_data_dir = user;
  config.log_dir = log;
  config.capacity = capacity;
  return config;
}

bool WaitForDiagnostic(bool (*predicate)(const EngineHostDiagnostics&),
                       std::string* observed) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    const EngineHostDiagnostics diagnostics = Snapshot();
    if (predicate(diagnostics)) {
      return true;
    }
    *observed = "state=" + Num(static_cast<int>(diagnostics.state)) +
                " waiters=" + Num(diagnostics.waiting_acquires);
    std::this_thread::yield();
  }
  return false;
}

bool OneWaiter(const EngineHostDiagnostics& diagnostics) {
  return diagnostics.waiting_acquires == 1;
}

void CheckState(const EngineHostDiagnostics& diagnostics, EngineHostState want,
                const std::string& what) {
  Check(diagnostics.state == want, what,
        "state=" + Num(static_cast<int>(diagnostics.state)) +
            " want=" + Num(static_cast<int>(want)));
}

void RunSingleLifecycle(const Options& options, const std::filesystem::path& shared,
                        const std::filesystem::path& user,
                        const std::filesystem::path& log) {
  const EngineHostConfig config = MakeConfig(options, shared, user, log);
  Check(epocanva::tsf::SetEngineHostConfig(config),
        "single: the test configuration is accepted");

  EngineLease lease;
  const EngineLeaseStatus acquired =
      EngineHostBroker::Instance().Acquire(config, &lease);
  Check(acquired == EngineLeaseStatus::kAcquired, "single: first lease acquired",
        "status=" + Num(static_cast<int>(acquired)));
  Check(lease.valid(), "single: the returned lease is valid", Bool(lease.valid()));

  EngineHostDiagnostics diagnostics = Snapshot();
  CheckState(diagnostics, EngineHostState::kRunning, "single: broker is running");
  Check(diagnostics.lease_count == 1, "single: exactly one lease",
        Num(diagnostics.lease_count));
  Check(diagnostics.executor_instances_created == 1,
        "single: one executor instance was created",
        Num(diagnostics.executor_instances_created));
  Check(diagnostics.start_attempts == 1, "single: one start attempt",
        Num(diagnostics.start_attempts));
  Check(diagnostics.initialize_calls == 1,
        "single: Runtime::Start initialized the engine exactly once",
        Num(diagnostics.initialize_calls));
  Check(diagnostics.finalize_calls == 0,
        "single: Runtime::Shutdown has not run yet",
        Num(diagnostics.finalize_calls));
  Check(diagnostics.start_thread_id == diagnostics.worker_thread_id,
        "single: Runtime::Start ran on the executor worker",
        Num(diagnostics.start_thread_id) + " vs " + Num(diagnostics.worker_thread_id));
  Check(diagnostics.worker_thread_id != ::GetCurrentThreadId() &&
            diagnostics.worker_thread_id != 0,
        "single: the engine worker is a different, live thread",
        Num(diagnostics.worker_thread_id));
  Check(diagnostics.live_sessions_after_cleanup == 0,
        "single: no session was created (lifecycle-only phase)",
        Num(diagnostics.live_sessions_after_cleanup));
  Check(!epocanva::tsf::ModuleCanUnload(),
        "single: an active lease blocks ModuleCanUnload", Bool(false));

  Check(lease.Release() == EngineLeaseReleaseStatus::kReleased,
        "single: the last lease release succeeds");
  diagnostics = Snapshot();
  CheckState(diagnostics, EngineHostState::kIdle, "single: broker returns to idle");
  Check(diagnostics.lease_count == 0, "single: no lease remains",
        Num(diagnostics.lease_count));
  Check(!diagnostics.executor_present,
        "single: the executor object was destroyed", Bool(diagnostics.executor_present));
  Check(diagnostics.finalize_calls == 1,
        "single: Runtime::Shutdown finalized exactly once",
        Num(diagnostics.finalize_calls));
  Check(diagnostics.shutdown_thread_id == diagnostics.worker_thread_id &&
            diagnostics.shutdown_thread_id != 0,
        "single: Runtime::Shutdown ran on the same worker",
        Num(diagnostics.shutdown_thread_id) + " vs " + Num(diagnostics.worker_thread_id));
  Check(diagnostics.api_balanced,
        "single: API acquire/free counters are balanced", Bool(diagnostics.api_balanced));
  Check(diagnostics.stop_calls == 1 && diagnostics.stop_failures == 0,
        "single: one clean stop",
        "calls=" + Num(diagnostics.stop_calls) +
            " failures=" + Num(diagnostics.stop_failures));
  Check(epocanva::tsf::ModuleCanUnload(),
        "single: after the clean stop ModuleCanUnload is true", Bool(true));

  // Test 5: after a completed stop the next acquisition must build a new executor, never
  // call Start on the stopped one.
  EngineLease second;
  const EngineLeaseStatus acquired_again =
      EngineHostBroker::Instance().Acquire(config, &second);
  Check(acquired_again == EngineLeaseStatus::kAcquired,
        "single: a later activation acquires a new lease",
        "status=" + Num(static_cast<int>(acquired_again)));
  diagnostics = Snapshot();
  Check(diagnostics.executor_instances_created == 2,
        "single: the later activation created a second executor instance",
        Num(diagnostics.executor_instances_created));
  Check(diagnostics.current_executor_instance == 2,
        "single: the current instance id advanced",
        Num(diagnostics.current_executor_instance));
  Check(diagnostics.start_attempts == 2,
        "single: the second start is a new attempt",
        Num(diagnostics.start_attempts));
  Check(diagnostics.initialize_calls == 1,
        "single: the new executor initialized once",
        Num(diagnostics.initialize_calls));
  Check(diagnostics.finalize_calls == 0,
        "single: the new executor is running, not stopped",
        Num(diagnostics.finalize_calls));
  Check(second.Release() == EngineLeaseReleaseStatus::kReleased,
        "single: the second lease release succeeds");
  diagnostics = Snapshot();
  Check(diagnostics.finalize_calls == 1,
        "single: the second executor finalized exactly once",
        Num(diagnostics.finalize_calls));
  Check(epocanva::tsf::ModuleCanUnload(),
        "single: ModuleCanUnload is true after the second clean stop", Bool(true));
}

void RunSharedAndMismatch(const Options& options,
                          const std::filesystem::path& shared,
                          const std::filesystem::path& user,
                          const std::filesystem::path& log) {
  const EngineHostConfig config = MakeConfig(options, shared, user, log);
  Check(epocanva::tsf::SetEngineHostConfig(config),
        "shared: the test configuration is accepted");
  const EngineHostDiagnostics before = Snapshot();

  EngineLease first;
  EngineLease second;
  Check(EngineHostBroker::Instance().Acquire(config, &first) ==
            EngineLeaseStatus::kAcquired,
        "shared: first service acquires a lease");
  Check(EngineHostBroker::Instance().Acquire(config, &second) ==
            EngineLeaseStatus::kAcquired,
        "shared: second service acquires a lease");

  EngineHostDiagnostics diagnostics = Snapshot();
  Check(diagnostics.lease_count == 2, "shared: two leases share the broker",
        Num(diagnostics.lease_count));
  Check(diagnostics.executor_instances_created ==
            before.executor_instances_created + 1,
        "shared: two leases created only one executor",
        Num(diagnostics.executor_instances_created));
  Check(diagnostics.start_attempts == before.start_attempts + 1,
        "shared: only the first lease started the engine",
        Num(diagnostics.start_attempts));
  Check(diagnostics.initialize_calls == 1,
        "shared: Runtime::Start ran once for both services",
        Num(diagnostics.initialize_calls));

  Check(first.Release() == EngineLeaseReleaseStatus::kReleased,
        "shared: the first lease releases");
  diagnostics = Snapshot();
  CheckState(diagnostics, EngineHostState::kRunning,
             "shared: the broker remains running after one deactivation");
  Check(diagnostics.lease_count == 1,
        "shared: the other service still holds its lease",
        Num(diagnostics.lease_count));
  Check(diagnostics.executor_present,
        "shared: the executor remains alive", Bool(diagnostics.executor_present));
  Check(diagnostics.finalize_calls == 0,
        "shared: the runtime was not finalized while a lease remains",
        Num(diagnostics.finalize_calls));
  Check(!epocanva::tsf::ModuleCanUnload(),
        "shared: the remaining lease blocks unload", Bool(false));

  // Test 6: a different environment is refused while the first one is in use. The
  // existing lease must remain untouched and no Runtime may be initialized.
  EngineHostConfig other = config;
  other.log_dir = options.work_root / "log2";
  EngineLease mismatched;
  const EngineLeaseStatus mismatch =
      EngineHostBroker::Instance().Acquire(other, &mismatched);
  Check(mismatch == EngineLeaseStatus::kConfigMismatch,
        "shared: a different configuration is refused",
        "status=" + Num(static_cast<int>(mismatch)));
  Check(!mismatched.valid(), "shared: the refused lease is not valid",
        Bool(mismatched.valid()));
  diagnostics = Snapshot();
  CheckState(diagnostics, EngineHostState::kRunning,
             "shared: the mismatch did not disturb the running broker");
  Check(diagnostics.lease_count == 1, "shared: the existing lease is unchanged",
        Num(diagnostics.lease_count));
  Check(diagnostics.config_mismatches == before.config_mismatches + 1,
        "shared: the mismatch is recorded", Num(diagnostics.config_mismatches));
  Check(diagnostics.start_attempts == before.start_attempts + 1,
        "shared: the mismatch started nothing", Num(diagnostics.start_attempts));
  Check(diagnostics.initialize_calls == 1,
        "shared: the mismatch did not initialize a second runtime",
        Num(diagnostics.initialize_calls));

  Check(second.Release() == EngineLeaseReleaseStatus::kReleased,
        "shared: the last lease releases");
  diagnostics = Snapshot();
  CheckState(diagnostics, EngineHostState::kIdle,
             "shared: the broker is idle after the last lease");
  Check(diagnostics.finalize_calls == 1,
        "shared: the runtime finalized exactly once", Num(diagnostics.finalize_calls));
  Check(epocanva::tsf::ModuleCanUnload(),
        "shared: ModuleCanUnload is true after all leases are gone", Bool(true));
}

void RunStopRejection(const Options& options,
                      const std::filesystem::path& shared,
                      const std::filesystem::path& user,
                      const std::filesystem::path& log) {
  const EngineHostConfig config = MakeConfig(options, shared, user, log);
  Check(epocanva::tsf::SetEngineHostConfig(config),
        "stop-race: the test configuration is accepted");
  const EngineHostDiagnostics before = Snapshot();

  EngineLease lease;
  Check(EngineHostBroker::Instance().Acquire(config, &lease) ==
            EngineLeaseStatus::kAcquired,
        "stop-race: the lease is acquired");

  Event entered;
  Event release;
  Check(entered.valid() && release.valid(), "stop-race: stop gate events exist");
  epocanva::tsf::SetTestStopGate(entered.handle, release.handle);

  EngineLeaseReleaseStatus release_status = EngineLeaseReleaseStatus::kAlreadyReleased;
  std::thread stopper([&] { release_status = lease.Release(); });

  Check(entered.Wait(), "stop-race: Deactivate reached the stop gate");
  EngineHostDiagnostics diagnostics = Snapshot();
  CheckState(diagnostics, EngineHostState::kStopping,
             "stop-race: the broker is Stopping");
  Check(diagnostics.lease_count == 0,
        "stop-race: the last lease was removed before admission closed",
        Num(diagnostics.lease_count));
  Check(diagnostics.executor_present,
        "stop-race: the executor is still present during Stopping",
        Bool(diagnostics.executor_present));
  Check(diagnostics.stop_calls == before.stop_calls + 1,
        "stop-race: exactly one stop was initiated", Num(diagnostics.stop_calls));
  Check(!epocanva::tsf::ModuleCanUnload(),
        "stop-race: the stop window blocks unload", Bool(false));

  EngineLease rejected;
  const EngineLeaseStatus rejected_status =
      EngineHostBroker::Instance().Acquire(config, &rejected);
  Check(rejected_status == EngineLeaseStatus::kStopping,
        "stop-race: Acquire during Stopping is explicitly refused",
        "status=" + Num(static_cast<int>(rejected_status)));
  Check(!rejected.valid(), "stop-race: the refused lease is invalid",
        Bool(rejected.valid()));
  diagnostics = Snapshot();
  Check(diagnostics.acquire_rejections_stopping ==
            before.acquire_rejections_stopping + 1,
        "stop-race: the rejection is counted",
        Num(diagnostics.acquire_rejections_stopping));
  Check(diagnostics.start_attempts == before.start_attempts + 1,
        "stop-race: the refused Acquire did not start another executor",
        Num(diagnostics.start_attempts));
  Check(diagnostics.executor_instances_created ==
            before.executor_instances_created + 1,
        "stop-race: the refused Acquire created no instance",
        Num(diagnostics.executor_instances_created));

  release.Set();
  stopper.join();
  epocanva::tsf::SetTestStopGate(nullptr, nullptr);
  Check(release_status == EngineLeaseReleaseStatus::kReleased,
        "stop-race: the last release completed after the gate opened");

  diagnostics = Snapshot();
  CheckState(diagnostics, EngineHostState::kIdle,
             "stop-race: the broker returned to idle");
  Check(diagnostics.finalize_calls == 1,
        "stop-race: the runtime finalized exactly once",
        Num(diagnostics.finalize_calls));
  Check(epocanva::tsf::ModuleCanUnload(),
        "stop-race: ModuleCanUnload is true after the stop", Bool(true));
}

void CheckStartupFailure(const char* label, const EngineHostConfig& failing,
                         const EngineHostConfig& good,
                         engine::RuntimeStartStatus want_start_status,
                         std::uint64_t expected_instances_delta,
                         std::uint64_t expected_attempts_delta,
                         std::uint64_t expected_failures_delta) {
  const EngineHostDiagnostics before = Snapshot();
  Check(epocanva::tsf::SetEngineHostConfig(failing),
        std::string(label) + ": the failing configuration is accepted");
  EngineLease lease;
  const EngineLeaseStatus status =
      EngineHostBroker::Instance().Acquire(failing, &lease);
  Check(status == EngineLeaseStatus::kStartFailed,
        std::string(label) + ": Acquire reports a clean start failure",
        "status=" + Num(static_cast<int>(status)));
  Check(!lease.valid(), std::string(label) + ": no lease is handed out",
        Bool(lease.valid()));

  EngineHostDiagnostics diagnostics = Snapshot();
  CheckState(diagnostics, EngineHostState::kIdle,
             std::string(label) + ": the broker is Idle after cleanup");
  Check(diagnostics.lease_count == 0,
        std::string(label) + ": no lease remains", Num(diagnostics.lease_count));
  Check(!diagnostics.executor_present,
        std::string(label) + ": the failed executor was destroyed",
        Bool(diagnostics.executor_present));
  Check(diagnostics.start_attempts == before.start_attempts + expected_attempts_delta,
        std::string(label) + ": the attempt count",
        Num(diagnostics.start_attempts));
  Check(diagnostics.start_failures == before.start_failures + expected_failures_delta,
        std::string(label) + ": the failure count",
        Num(diagnostics.start_failures));
  Check(diagnostics.executor_instances_created ==
            before.executor_instances_created + expected_instances_delta,
        std::string(label) + ": one new executor instance was created for the attempt",
        Num(diagnostics.executor_instances_created));
  Check(diagnostics.runtime_start_status == static_cast<int>(want_start_status),
        std::string(label) + ": the real RuntimeStartStatus is reported",
        "status=" + Num(diagnostics.runtime_start_status) + " want=" +
            Num(static_cast<int>(want_start_status)));
  Check(epocanva::tsf::ModuleCanUnload(),
        std::string(label) + ": ModuleCanUnload is true after clean cleanup",
        Bool(epocanva::tsf::ModuleCanUnload()));

  Check(epocanva::tsf::SetEngineHostConfig(good),
        std::string(label) + ": the good configuration is restored");
  const EngineHostDiagnostics before_retry = Snapshot();
  EngineLease retry;
  const EngineLeaseStatus retry_status =
      EngineHostBroker::Instance().Acquire(good, &retry);
  Check(retry_status == EngineLeaseStatus::kAcquired,
        std::string(label) + ": a later activation starts a new executor",
        "status=" + Num(static_cast<int>(retry_status)));
  diagnostics = Snapshot();
  Check(diagnostics.start_attempts == before_retry.start_attempts + 1,
        std::string(label) + ": the retry is one new attempt",
        Num(diagnostics.start_attempts));
  Check(diagnostics.executor_instances_created ==
            before_retry.executor_instances_created + 1,
        std::string(label) + ": the retry created one new executor",
        Num(diagnostics.executor_instances_created));
  Check(diagnostics.initialize_calls == 1,
        std::string(label) + ": the new executor initialized once",
        Num(diagnostics.initialize_calls));
  Check(retry.Release() == EngineLeaseReleaseStatus::kReleased,
        std::string(label) + ": the retry lease releases");
  diagnostics = Snapshot();
  Check(diagnostics.finalize_calls == 1,
        std::string(label) + ": the retry runtime finalized once",
        Num(diagnostics.finalize_calls));
}

void RunStartupFailures(const Options& options,
                        const std::filesystem::path& shared,
                        const std::filesystem::path& user,
                        const std::filesystem::path& log) {
  const EngineHostConfig good = MakeConfig(options, shared, user, log);
  EngineHostConfig missing = good;
  missing.rime_dll_path = options.work_root / "missing" / "rime.dll";
  CheckStartupFailure("startup-missing-dll", missing, good,
                      engine::RuntimeStartStatus::kDllLoadFailed, 1, 1, 1);
  std::error_code ec;
  if (!options.baseline_dll.empty() &&
      std::filesystem::is_regular_file(options.baseline_dll, ec) && !ec) {
    EngineHostConfig baseline = good;
    baseline.rime_dll_path = options.baseline_dll;
    CheckStartupFailure("startup-missing-module", baseline, good,
                        engine::RuntimeStartStatus::kEnglishModuleMissing, 1, 1, 1);
  } else {
    std::printf("[tsf-host] UNVERIFIED startup-missing-module: no baseline rime.dll "
                "was supplied; the missing-DLL startup cleanup above still ran\n");
  }
}

void RunLeaseUnderflow(const Options& options,
                       const std::filesystem::path& shared,
                       const std::filesystem::path& user,
                       const std::filesystem::path& log) {
  const EngineHostConfig config = MakeConfig(options, shared, user, log);
  Check(epocanva::tsf::SetEngineHostConfig(config),
        "lease-underflow: the test configuration is accepted");
  const EngineHostDiagnostics before = Snapshot();

  EngineLease lease;
  Check(EngineHostBroker::Instance().Acquire(config, &lease) ==
            EngineLeaseStatus::kAcquired,
        "lease-underflow: the lease is acquired");
  const std::uint64_t lease_id = lease.id();
  Check(lease.Release() == EngineLeaseReleaseStatus::kReleased,
        "lease-underflow: the first release succeeds");
  Check(lease.Release() == EngineLeaseReleaseStatus::kAlreadyReleased,
        "lease-underflow: the handle's second release is refused explicitly");
  Check(!lease.valid(), "lease-underflow: the lease is invalid after release",
        Bool(lease.valid()));

  EngineHostDiagnostics diagnostics = Snapshot();
  Check(diagnostics.release_underflows == before.release_underflows,
        "lease-underflow: a repeated handle release never reaches the broker",
        Num(diagnostics.release_underflows));

  // Exercise the broker's own duplicate-release guard with the same id. This is the layer
  // that owns the lease set; it must refuse without decrementing anything.
  Check(EngineHostBroker::Instance().ReleaseLease(lease_id) ==
            EngineLeaseReleaseStatus::kAlreadyReleased,
        "lease-underflow: the broker refuses a duplicate id");
  diagnostics = Snapshot();
  Check(diagnostics.lease_count == 0,
        "lease-underflow: the active lease count is zero, not wrapped",
        Num(diagnostics.lease_count));
  Check(diagnostics.release_underflows == before.release_underflows + 1,
        "lease-underflow: the broker refusal is counted once",
        Num(diagnostics.release_underflows));
  CheckState(diagnostics, EngineHostState::kIdle,
             "lease-underflow: the broker is idle");
  Check(diagnostics.finalize_calls == 1,
        "lease-underflow: the runtime finalized exactly once",
        Num(diagnostics.finalize_calls));
  Check(epocanva::tsf::ModuleCanUnload(),
        "lease-underflow: ModuleCanUnload is true", Bool(true));
}

void RunStartGateSingleStarter(const Options& options,
                               const std::filesystem::path& shared,
                               const std::filesystem::path& user,
                               const std::filesystem::path& log) {
  const EngineHostConfig config = MakeConfig(options, shared, user, log);
  Check(epocanva::tsf::SetEngineHostConfig(config),
        "start-gate: the test configuration is accepted");
  const EngineHostDiagnostics before = Snapshot();
  Event entered;
  Event release;
  Event keep_alive;
  Check(entered.valid() && release.valid() && keep_alive.valid(),
        "start-gate: all gate events exist");
  epocanva::tsf::SetTestStartGate(entered.handle, release.handle);

  Barrier barrier(3);
  std::mutex result_mutex;
  std::condition_variable result_cv;
  int done = 0;
  EngineLeaseStatus statuses[2] = {EngineLeaseStatus::kInvalidLease,
                                   EngineLeaseStatus::kInvalidLease};
  EngineLease leases[2];

  auto worker = [&](int index) {
    barrier.Wait();
    const EngineLeaseStatus status =
        EngineHostBroker::Instance().Acquire(config, &leases[index]);
    {
      std::lock_guard<std::mutex> guard(result_mutex);
      statuses[index] = status;
      ++done;
      result_cv.notify_all();
    }
    if (status == EngineLeaseStatus::kAcquired) {
      (void)keep_alive.Wait();
      (void)leases[index].Release();
    }
  };

  std::thread first(worker, 0);
  std::thread second(worker, 1);
  barrier.Wait();
  Check(entered.Wait(), "start-gate: the single starter reached the start gate");
  EngineHostDiagnostics diagnostics = Snapshot();
  CheckState(diagnostics, EngineHostState::kStarting,
             "start-gate: the broker is Starting");
  Check(diagnostics.start_attempts == before.start_attempts + 1,
        "start-gate: exactly one start attempt exists",
        Num(diagnostics.start_attempts));
  Check(diagnostics.executor_instances_created ==
            before.executor_instances_created + 1,
        "start-gate: exactly one executor instance exists",
        Num(diagnostics.executor_instances_created));
  Check(diagnostics.lease_count == 0,
        "start-gate: no lease is granted before Start succeeds",
        Num(diagnostics.lease_count));
  {
    std::lock_guard<std::mutex> guard(result_mutex);
    Check(done == 0, "start-gate: both Acquire calls are still in flight",
          Num(static_cast<std::uint64_t>(done)));
  }
  std::string observed;
  Check(WaitForDiagnostic(OneWaiter, &observed),
        "start-gate: the second Acquire waits for the one start", observed);
  Check(!epocanva::tsf::ModuleCanUnload(),
        "start-gate: the Starting window blocks unload", Bool(false));

  release.Set();
  {
    std::unique_lock<std::mutex> lock(result_mutex);
    result_cv.wait(lock, [&done] { return done == 2; });
  }
  Check(statuses[0] == EngineLeaseStatus::kAcquired &&
            statuses[1] == EngineLeaseStatus::kAcquired,
        "start-gate: both waiters received a lease after the one start");
  diagnostics = Snapshot();
  Check(diagnostics.lease_count == 2,
        "start-gate: both leases share the executor", Num(diagnostics.lease_count));
  Check(diagnostics.start_attempts == before.start_attempts + 1,
        "start-gate: waiting Acquire did not start a second runtime",
        Num(diagnostics.start_attempts));
  Check(diagnostics.initialize_calls == 1,
        "start-gate: Runtime::Start ran once",
        Num(diagnostics.initialize_calls));
  Check(diagnostics.worker_thread_id != 0,
        "start-gate: one worker exists", Num(diagnostics.worker_thread_id));

  keep_alive.Set();
  first.join();
  second.join();
  diagnostics = Snapshot();
  CheckState(diagnostics, EngineHostState::kIdle,
             "start-gate: the broker is idle after both leases release");
  Check(diagnostics.finalize_calls == 1,
        "start-gate: the runtime finalized once", Num(diagnostics.finalize_calls));
  epocanva::tsf::SetTestStartGate(nullptr, nullptr);
}

void RunStartFailureWaiters(const Options& options,
                            const std::filesystem::path& shared,
                            const std::filesystem::path& user,
                            const std::filesystem::path& log) {
  EngineHostConfig failing = MakeConfig(options, shared, user, log);
  failing.rime_dll_path = options.work_root / "missing" / "rime.dll";
  Check(epocanva::tsf::SetEngineHostConfig(failing),
        "failure-waiters: the failing configuration is accepted");
  const EngineHostDiagnostics before = Snapshot();
  Event entered;
  Event release;
  Check(entered.valid() && release.valid(), "failure-waiters: gate events exist");
  epocanva::tsf::SetTestStartGate(entered.handle, release.handle);

  Barrier barrier(3);
  std::mutex result_mutex;
  std::condition_variable result_cv;
  int done = 0;
  EngineLeaseStatus statuses[2] = {EngineLeaseStatus::kInvalidLease,
                                   EngineLeaseStatus::kInvalidLease};

  auto worker = [&](int index) {
    barrier.Wait();
    EngineLease lease;
    const EngineLeaseStatus real_status =
        EngineHostBroker::Instance().Acquire(failing, &lease);
    {
      std::lock_guard<std::mutex> guard(result_mutex);
      statuses[index] = real_status;
      ++done;
      result_cv.notify_all();
    }
    if (lease.valid()) {
      (void)lease.Release();
    }
  };

  std::thread first(worker, 0);
  std::thread second(worker, 1);
  barrier.Wait();
  Check(entered.Wait(), "failure-waiters: the starter reached the start gate");
  std::string observed;
  Check(WaitForDiagnostic(OneWaiter, &observed),
        "failure-waiters: the second Acquire waits on the same attempt", observed);
  EngineHostDiagnostics diagnostics = Snapshot();
  Check(diagnostics.start_attempts == before.start_attempts + 1,
        "failure-waiters: one attempt for both callers",
        Num(diagnostics.start_attempts));
  Check(diagnostics.lease_count == 0,
        "failure-waiters: no lease exists during Starting",
        Num(diagnostics.lease_count));
  Check(!epocanva::tsf::ModuleCanUnload(),
        "failure-waiters: the failing start window blocks unload", Bool(false));

  release.Set();
  {
    std::unique_lock<std::mutex> lock(result_mutex);
    result_cv.wait(lock, [&done] { return done == 2; });
  }
  Check(statuses[0] == EngineLeaseStatus::kStartFailed &&
            statuses[1] == EngineLeaseStatus::kStartFailed,
        "failure-waiters: every waiter received the explicit start failure");
  diagnostics = Snapshot();
  CheckState(diagnostics, EngineHostState::kIdle,
             "failure-waiters: the broker is Idle after cleanup");
  Check(diagnostics.start_attempts == before.start_attempts + 1,
        "failure-waiters: no waiter retried the failed attempt",
        Num(diagnostics.start_attempts));
  Check(diagnostics.start_failures == before.start_failures + 1,
        "failure-waiters: one failure is recorded",
        Num(diagnostics.start_failures));
  Check(diagnostics.executor_instances_created ==
            before.executor_instances_created + 1,
        "failure-waiters: one failed instance was created",
        Num(diagnostics.executor_instances_created));
  Check(diagnostics.lease_count == 0,
        "failure-waiters: no lease leaked", Num(diagnostics.lease_count));
  Check(epocanva::tsf::ModuleCanUnload(),
        "failure-waiters: ModuleCanUnload is true after clean cleanup", Bool(true));

  first.join();
  second.join();
  epocanva::tsf::SetTestStartGate(nullptr, nullptr);
}

void RunCleanupFailure(const Options& options,
                       const std::filesystem::path& shared,
                       const std::filesystem::path& user,
                       const std::filesystem::path& log) {
  const EngineHostConfig config = MakeConfig(options, shared, user, log);
  Check(epocanva::tsf::SetEngineHostConfig(config),
        "cleanup-failure: the test configuration is accepted");
  const EngineHostDiagnostics before = Snapshot();
  epocanva::tsf::SetTestForceCleanupFailure(true);

  EngineLease lease;
  Check(EngineHostBroker::Instance().Acquire(config, &lease) ==
            EngineLeaseStatus::kAcquired,
        "cleanup-failure: the lease is acquired");
  Check(lease.Release() == EngineLeaseReleaseStatus::kReleased,
        "cleanup-failure: the release completes");
  epocanva::tsf::SetTestForceCleanupFailure(false);

  const EngineHostDiagnostics diagnostics = Snapshot();
  CheckState(diagnostics, EngineHostState::kFailed,
             "cleanup-failure: the broker is Failed, not Idle");
  Check(diagnostics.cleanup_failed,
        "cleanup-failure: the cleanup failure is latched",
        Bool(diagnostics.cleanup_failed));
  Check(diagnostics.stop_failures == before.stop_failures + 1,
        "cleanup-failure: one stop failure is recorded",
        Num(diagnostics.stop_failures));
  Check(diagnostics.lease_count == 0,
        "cleanup-failure: no lease remains", Num(diagnostics.lease_count));
  Check(diagnostics.finalize_calls == 1,
        "cleanup-failure: the engine still finalized (fault was injected after the join)",
        Num(diagnostics.finalize_calls));
  Check(!epocanva::tsf::ModuleCanUnload(),
        "cleanup-failure: an unresolved cleanup never allows unload", Bool(false));

  EngineLease rejected;
  const EngineLeaseStatus rejected_status =
      EngineHostBroker::Instance().Acquire(config, &rejected);
  Check(rejected_status == EngineLeaseStatus::kFailed,
        "cleanup-failure: a later Acquire is refused in the Failed state",
        "status=" + Num(static_cast<int>(rejected_status)));
}

int RunSelfCheckFail() {
  std::printf("[tsf-host] mode self-check-fail: one deliberately false claim\n");
  const EngineHostDiagnostics diagnostics = Snapshot();
  Check(diagnostics.state == EngineHostState::kRunning,
        "deliberately asserted that a fresh broker is Running (it is Idle)");
  std::printf("[tsf-host] RESULT FAIL (self-check-fail control, %d checks, %d failed)\n",
              g_total, g_failed);
  return g_failed == 0 ? kExitOk : kExitSelfCheckFailed;
}

bool ParseArgs(int argc, wchar_t** argv, Options* options) {
  bool have_mode = false;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    auto next = [&](std::filesystem::path* target) {
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
    } else if (arg == L"--rime-dll") {
      if (!next(&options->rime_dll)) return false;
    } else if (arg == L"--baseline-dll") {
      if (!next(&options->baseline_dll)) return false;
    } else if (arg == L"--fixture-dir") {
      if (!next(&options->fixture_dir)) return false;
    } else if (arg == L"--spelling-fixture-dir") {
      if (!next(&options->spelling_fixture_dir)) return false;
    } else if (arg == L"--work-root") {
      if (!next(&options->work_root)) return false;
    } else {
      return false;
    }
  }
  return have_mode;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  Options options;
  if (!ParseArgs(argc, argv, &options)) {
    std::printf("usage: tsf_engine_host_tests --mode broker|self-check-fail "
                "--rime-dll <path> --baseline-dll <path> --fixture-dir <dir> "
                "--spelling-fixture-dir <dir> --work-root <dir>\n");
    return kExitUsage;
  }
  if (options.mode == "self-check-fail") {
    return RunSelfCheckFail();
  }
  if (options.mode != "broker") {
    std::printf("[tsf-host] unknown mode: %s\n", options.mode.c_str());
    return kExitUsage;
  }
  if (options.rime_dll.empty() || options.fixture_dir.empty() ||
      options.spelling_fixture_dir.empty() || options.work_root.empty()) {
    std::printf("[tsf-host] broker mode requires --rime-dll, --fixture-dir, "
                "--spelling-fixture-dir and --work-root\n");
    return kExitUsage;
  }

  std::filesystem::path shared;
  std::filesystem::path user;
  std::filesystem::path log;
  if (!PrepareRunRoot(options, &shared, &user, &log)) {
    return kExitCheckFailed;
  }
  std::printf("[tsf-host] run-root %s\n", options.work_root.string().c_str());

  RunSingleLifecycle(options, shared, user, log);
  RunSharedAndMismatch(options, shared, user, log);
  RunStopRejection(options, shared, user, log);
  RunStartupFailures(options, shared, user, log);
  RunLeaseUnderflow(options, shared, user, log);
  RunStartGateSingleStarter(options, shared, user, log);
  RunStartFailureWaiters(options, shared, user, log);
  RunCleanupFailure(options, shared, user, log);  // terminal: latches Failed

  std::printf("[tsf-host] RESULT %s (%d checks, %d failed)\n",
              g_failed == 0 ? "PASS" : "FAIL", g_total, g_failed);
  return g_failed == 0 ? kExitOk : kExitCheckFailed;
}
