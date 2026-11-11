// EpoCanva P3.6 -- engine host broker implementation. See engine_host.h for the contract.
//
// LOCK DISCIPLINE (the whole file depends on it):
//   * impl_->mutex protects the state enum, the configuration snapshot, the lease id set and
//     every counter below.
//   * EngineExecutor::Start and ::StopAndJoin are called with that mutex RELEASED.
//   * Destroying the executor (its destructor repeats a terminal StopAndJoin) happens with
//     the mutex released too; the host remains "active" for DllCanUnloadNow until it ends.
//   * The executor's worker never enters this file, so there is no callback path from the
//     worker back into broker or service code.
#include "engine_host.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <utility>

#include "engine_executor.h"
#include "module_state.h"

namespace epocanva {
namespace tsf {

namespace engine = ::epocanva::windows::engine;

namespace {

constexpr std::size_t kMaxCapacity = 4096;

// --- process-wide configuration ---------------------------------------------------------
std::mutex g_config_mutex;
EngineHostConfig g_config;
bool g_config_set = false;

#ifdef EPOCANVA_TSF_TEST_SEAM
std::mutex g_gate_mutex;
void* g_start_entered = nullptr;
void* g_start_release = nullptr;
void* g_stop_entered = nullptr;
void* g_stop_release = nullptr;
void* g_cleanup_entered = nullptr;
void* g_cleanup_release = nullptr;
std::atomic<bool> g_force_cleanup_failure{false};
#endif

#ifdef EPOCANVA_TSF_TEST_SEAM
void WaitGate(void* entered, void* release) {
  if (entered != nullptr) {
    ::SetEvent(static_cast<HANDLE>(entered));
  }
  if (release != nullptr) {
    // The test owns the handles and signals release before closing them. A failed wait is
    // not turned into a hidden failure here; the test observes its own timeout.
    (void)::WaitForSingleObject(static_cast<HANDLE>(release), INFINITE);
  }
}

void RunStartGate() {
  void* entered = nullptr;
  void* release = nullptr;
  {
    std::lock_guard<std::mutex> guard(g_gate_mutex);
    entered = g_start_entered;
    release = g_start_release;
  }
  WaitGate(entered, release);
}

void RunStopGate() {
  void* entered = nullptr;
  void* release = nullptr;
  {
    std::lock_guard<std::mutex> guard(g_gate_mutex);
    entered = g_stop_entered;
    release = g_stop_release;
  }
  WaitGate(entered, release);
}

void RunCleanupGate() {
  void* entered = nullptr;
  void* release = nullptr;
  {
    std::lock_guard<std::mutex> guard(g_gate_mutex);
    entered = g_cleanup_entered;
    release = g_cleanup_release;
  }
  WaitGate(entered, release);
}
#else
void RunStartGate() {}
void RunStopGate() {}
void RunCleanupGate() {}
#endif

bool CleanStartFailure(engine::StopStatus stop,
                       const engine::ExecutorRuntimeReport& report) {
  // A failed Start is recovered only if the executor joined its worker and the runtime did
  // not report a finalize/unload failure. kExecutorFailed is the normal result of a Start
  // that failed before the runtime became usable; P3.5 proves a later executor can start
  // after that path, and this broker's own startup-failure test re-checks it.
  if (stop != engine::StopStatus::kOk &&
      stop != engine::StopStatus::kExecutorFailed &&
      stop != engine::StopStatus::kAlreadyStopped) {
    return false;
  }
  if (report.live_sessions_after_cleanup != 0) {
    return false;
  }
  switch (report.runtime_shutdown_status) {
    case engine::RuntimeShutdownStatus::kNotStarted:
    case engine::RuntimeShutdownStatus::kOk:
    case engine::RuntimeShutdownStatus::kAlreadyShutdown:
      return true;
    default:
      return false;
  }
}

bool CleanStop(engine::StopStatus stop,
               const engine::ExecutorRuntimeReport& report) {
  return stop == engine::StopStatus::kOk &&
         report.state == engine::ExecutorState::kStopped &&
         report.runtime_shutdown_status == engine::RuntimeShutdownStatus::kOk &&
         report.live_sessions_after_cleanup == 0;
}

}  // namespace

namespace {

bool DeploymentEqual(const std::optional<EngineHostDeployment>& left,
                     const std::optional<EngineHostDeployment>& right) {
  if (left.has_value() != right.has_value()) {
    return false;
  }
  if (!left.has_value()) {
    return true;
  }
  return left->config_file == right->config_file &&
         left->schema_files == right->schema_files;
}

bool DeploymentValid(const EngineHostDeployment& deployment) {
  if (!deployment.config_file.is_absolute() ||
      !deployment.config_file.has_filename() || deployment.schema_files.empty()) {
    return false;
  }
  for (const std::filesystem::path& schema : deployment.schema_files) {
    if (!schema.is_absolute()) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool EngineHostConfig::valid() const {
  if (!(rime_dll_path.is_absolute() && !rime_dll_path.empty() &&
        shared_data_dir.is_absolute() && !shared_data_dir.empty() &&
        user_data_dir.is_absolute() && !user_data_dir.empty() &&
        log_dir.is_absolute() && !log_dir.empty() && capacity > 0 &&
        capacity <= kMaxCapacity)) {
    return false;
  }
  return !deployment.has_value() || DeploymentValid(*deployment);
}

bool operator==(const EngineHostConfig& left, const EngineHostConfig& right) {
  return left.rime_dll_path == right.rime_dll_path &&
         left.shared_data_dir == right.shared_data_dir &&
         left.user_data_dir == right.user_data_dir &&
         left.log_dir == right.log_dir && left.capacity == right.capacity &&
         left.backend == right.backend &&
         DeploymentEqual(left.deployment, right.deployment);
}

bool operator!=(const EngineHostConfig& left, const EngineHostConfig& right) {
  return !(left == right);
}

bool SetEngineHostConfig(const EngineHostConfig& config) {
  if (!config.valid()) {
    return false;
  }
  std::lock_guard<std::mutex> guard(g_config_mutex);
  g_config = config;  // copies every path; the caller's storage may disappear immediately
  g_config_set = true;
  return true;
}

bool GetEngineHostConfig(EngineHostConfig* config) {
  if (config == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> guard(g_config_mutex);
  if (!g_config_set) {
    return false;
  }
  *config = g_config;
  return true;
}

void ClearEngineHostConfig() {
  std::lock_guard<std::mutex> guard(g_config_mutex);
  g_config = EngineHostConfig{};
  g_config_set = false;
}

struct EngineHostBroker::Impl {
  mutable std::mutex mutex;
  std::condition_variable cv;

  EngineHostState state = EngineHostState::kIdle;
  std::unique_ptr<engine::EngineExecutor> executor;
  EngineHostConfig config;

  std::unordered_set<std::uint64_t> leases;
  std::uint64_t next_lease_id = 1;
  std::uint64_t next_executor_id = 1;
  std::uint64_t current_executor_id = 0;
  std::uint64_t executor_instances_created = 0;
  std::uint64_t generation = 0;

  std::uint64_t start_attempts = 0;
  std::uint64_t start_failures = 0;
  std::uint64_t stop_calls = 0;
  std::uint64_t stop_failures = 0;
  std::uint64_t acquire_rejections_stopping = 0;
  std::uint64_t acquire_rejections_failed = 0;
  std::uint64_t config_mismatches = 0;
  std::uint64_t release_underflows = 0;
  std::uint64_t abandoned_leases = 0;
  std::uint64_t waiting_acquires = 0;

  bool cleanup_failed = false;
  bool abandoned = false;

  engine::ExecutorRuntimeReport last_report;
  engine::StopStatus last_stop_status = engine::StopStatus::kNotStarted;
  std::uint32_t worker_thread_id = 0;
};

EngineHostBroker::EngineHostBroker() : impl_(new Impl) {}

EngineHostBroker::~EngineHostBroker() {
  // NORMAL UNLOAD: the last lease stopped and destroyed the executor, so this is null and
  // nothing below runs. DllCanUnloadNow cannot return S_OK while an executor is present.
  //
  // ABNORMAL TEARDOWN: the broker is being destroyed by CRT shutdown (possibly on
  // DLL_PROCESS_DETACH under the loader lock) with a worker still alive. Joining here would
  // deadlock the process. The executor object is deliberately abandoned; the worker is
  // terminated by process exit, and the DLL was never declared unloadable. This is the one
  // documented exception to "no controlled leak", and it is reachable only after a host
  // violated the Deactivate-before-Release contract.
  if (impl_ && impl_->executor) {
    (void)impl_->executor.release();
  }
}

EngineHostBroker& EngineHostBroker::Instance() {
  static EngineHostBroker broker;
  return broker;
}

EngineLeaseStatus EngineHostBroker::Acquire(const EngineHostConfig& config,
                                            EngineLease* lease) {
  if (lease == nullptr || lease->valid()) {
    return EngineLeaseStatus::kInvalidLease;
  }
  if (!config.valid()) {
    return EngineLeaseStatus::kInvalidConfig;
  }

  Impl& s = *impl_;
  std::unique_lock<std::mutex> lock(s.mutex);

  for (;;) {
    switch (s.state) {
      case EngineHostState::kIdle: {
        // This thread owns the one start attempt. Record the environment before unlocking:
        // every later Acquire compares against exactly this snapshot.
        s.config = config;
        s.state = EngineHostState::kStarting;
        ++s.generation;
        const std::uint64_t attempt = s.generation;
        ++s.start_attempts;
        s.current_executor_id = s.next_executor_id++;
        ++s.executor_instances_created;

        try {
          engine::ExecutorOptions options;
          options.runtime.rime_dll_path = config.rime_dll_path;
          options.runtime.shared_data_dir = config.shared_data_dir;
          options.runtime.user_data_dir = config.user_data_dir;
          options.runtime.log_dir = config.log_dir;
          if (config.deployment.has_value()) {
            engine::DeploymentOptions deployment;
            deployment.config_file_name =
                config.deployment->config_file.filename().u8string();
            deployment.schema_files = config.deployment->schema_files;
            options.runtime.deployment = std::move(deployment);
          }
          options.capacity = config.capacity;
          options.backend = config.backend;
          s.executor.reset(new engine::EngineExecutor(std::move(options)));
        } catch (...) {
          // No worker exists; the attempt is a clean failure. There cannot be a waiter yet
          // because this thread still holds the mutex in Starting.
          s.state = EngineHostState::kIdle;
          s.config = EngineHostConfig{};
          ++s.start_failures;
          ModuleSetHostActive(false);
          s.cv.notify_all();
          return EngineLeaseStatus::kInternalFailure;
        }

        // This flag covers Starting, Running, Stopping and the executor destruction window.
        ModuleSetHostActive(true);
        lock.unlock();
        RunStartGate();

        engine::StartStatus start = engine::StartStatus::kThreadStartFailed;
        try {
          start = s.executor->Start();
        } catch (...) {
          start = engine::StartStatus::kThreadStartFailed;
        }

        if (start == engine::StartStatus::kOk) {
          engine::ExecutorRuntimeReport report;
          try {
            report = s.executor->RuntimeReport();
          } catch (...) {
            report = engine::ExecutorRuntimeReport{};
          }

          lock.lock();
          if (s.state != EngineHostState::kStarting || s.generation != attempt) {
            // No stop can be initiated without a lease, so this is an internal invariant
            // break. Fall through to the failure cleanup rather than claiming a lease.
            lock.unlock();
          } else {
            // W2: deployment is a one-shot preparation input. The compiled output now
            // exists, so the running environment's identity no longer includes it and a
            // later lease with the same paths/capacity must match.
            s.config.deployment.reset();
            s.state = EngineHostState::kRunning;
            s.worker_thread_id = s.executor->worker_thread_id();
            s.last_report = report;
            const std::uint64_t id = s.next_lease_id++;
            try {
              s.leases.insert(id);
            } catch (...) {
              // The executor is running but no lease can be represented. Pin the broker in
              // Failed and let the exception reach the COM boundary, which reports OOM.
              s.state = EngineHostState::kFailed;
              s.abandoned = true;
              ModuleSetHostFailed(true);
              s.cv.notify_all();
              throw;
            }
            *lease = EngineLease(this, id);
            s.cv.notify_all();
            return EngineLeaseStatus::kAcquired;
          }
        }

        // Start failed (or the success path hit the impossible state change above). Stop and
        // join the worker outside the broker lock, then decide whether the failure is clean.
        engine::StopStatus stop = engine::StopStatus::kCleanupFailed;
        try {
          stop = s.executor ? s.executor->StopAndJoin()
                            : engine::StopStatus::kNotStarted;
        } catch (...) {
          stop = engine::StopStatus::kCleanupFailed;
        }
        engine::ExecutorRuntimeReport report;
        try {
          report = s.executor ? s.executor->RuntimeReport()
                              : engine::ExecutorRuntimeReport{};
        } catch (...) {
          report = engine::ExecutorRuntimeReport{};
        }
        const bool fatal = !CleanStartFailure(stop, report);
        std::uint32_t worker_id = report.start_thread_id;

        std::unique_ptr<engine::EngineExecutor> doomed;
        {
          std::lock_guard<std::mutex> guard(s.mutex);
          s.last_report = report;
          s.last_stop_status = stop;
          if (worker_id == 0) {
            worker_id = s.worker_thread_id;
          }
          s.worker_thread_id = worker_id;
          ++s.start_failures;
          doomed = std::move(s.executor);
        }
        // The host-active flag is still set: DllCanUnloadNow must stay S_FALSE until this
        // destruction (a terminal StopAndJoin inside the executor destructor) is over.
        doomed.reset();

        {
          std::lock_guard<std::mutex> guard(s.mutex);
          if (fatal) {
            s.state = EngineHostState::kFailed;
            s.cleanup_failed = true;
            ModuleSetHostFailed(true);
          } else {
            s.state = EngineHostState::kIdle;
            s.config = EngineHostConfig{};
            ModuleSetHostActive(false);
          }
          s.cv.notify_all();
        }
        return fatal ? EngineLeaseStatus::kCleanupFailed
                     : EngineLeaseStatus::kStartFailed;
      }

      case EngineHostState::kStarting: {
        if (config != s.config) {
          ++s.config_mismatches;
          return EngineLeaseStatus::kConfigMismatch;
        }
        const std::uint64_t attempt = s.generation;
        ++s.waiting_acquires;
        s.cv.wait(lock, [&s, attempt] {
          return s.state != EngineHostState::kStarting || s.generation != attempt;
        });
        --s.waiting_acquires;
        if (s.state == EngineHostState::kRunning && s.generation == attempt) {
          if (config != s.config) {
            ++s.config_mismatches;
            return EngineLeaseStatus::kConfigMismatch;
          }
          const std::uint64_t id = s.next_lease_id++;
          try {
            s.leases.insert(id);
          } catch (...) {
            s.state = EngineHostState::kFailed;
            s.abandoned = true;
            ModuleSetHostFailed(true);
            s.cv.notify_all();
            throw;
          }
          *lease = EngineLease(this, id);
          return EngineLeaseStatus::kAcquired;
        }
        if (s.state == EngineHostState::kFailed) {
          ++s.acquire_rejections_failed;
          return s.cleanup_failed ? EngineLeaseStatus::kCleanupFailed
                                  : EngineLeaseStatus::kFailed;
        }
        if (s.state == EngineHostState::kStopping) {
          ++s.acquire_rejections_stopping;
          return EngineLeaseStatus::kStopping;
        }
        // Idle after a clean failed attempt, or a newer attempt already replaced this one.
        // Either way this waiter's attempt is over; no implicit retry.
        return EngineLeaseStatus::kStartFailed;
      }

      case EngineHostState::kRunning: {
        if (config != s.config) {
          ++s.config_mismatches;
          return EngineLeaseStatus::kConfigMismatch;
        }
        const std::uint64_t id = s.next_lease_id++;
        try {
          s.leases.insert(id);
        } catch (...) {
          s.state = EngineHostState::kFailed;
          s.abandoned = true;
          ModuleSetHostFailed(true);
          s.cv.notify_all();
          throw;
        }
        *lease = EngineLease(this, id);
        return EngineLeaseStatus::kAcquired;
      }

      case EngineHostState::kStopping:
        ++s.acquire_rejections_stopping;
        return EngineLeaseStatus::kStopping;

      case EngineHostState::kFailed:
        ++s.acquire_rejections_failed;
        return EngineLeaseStatus::kFailed;
    }
  }
}

EngineLeaseReleaseStatus EngineHostBroker::ReleaseLease(std::uint64_t id) {
  Impl& s = *impl_;
  bool stop = false;
  {
    std::lock_guard<std::mutex> guard(s.mutex);
    if (id == 0 || s.leases.erase(id) == 0) {
      // A second release, an unknown id or a hand-built zero lease. The active count is a
      // set, so it cannot go negative; the refusal is counted for diagnostics.
      ++s.release_underflows;
      return EngineLeaseReleaseStatus::kAlreadyReleased;
    }
    if (s.leases.empty() && s.state == EngineHostState::kRunning) {
      s.state = EngineHostState::kStopping;
      ++s.stop_calls;
      stop = true;
    }
  }
  if (stop) {
    StopOnCaller();
  }
  return EngineLeaseReleaseStatus::kReleased;
}

void EngineHostBroker::AbandonLease(std::uint64_t id) noexcept {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> guard(s.mutex);
  if (id == 0 || s.leases.erase(id) == 0) {
    ++s.release_underflows;
    return;
  }
  ++s.abandoned_leases;
  s.abandoned = true;
  s.state = EngineHostState::kFailed;
  // Deliberately no StopAndJoin: this path is reached from a service destructor whose
  // calling context may be the loader lock. The worker stays alive and DllCanUnloadNow stays
  // S_FALSE for the rest of the process lifetime.
  ModuleSetHostFailed(true);
  s.cv.notify_all();
}

void EngineHostBroker::StopOnCaller() {
  Impl& s = *impl_;

  RunStopGate();

  engine::StopStatus stop = engine::StopStatus::kCleanupFailed;
  engine::ExecutorRuntimeReport report;
  try {
    if (s.executor) {
      stop = s.executor->StopAndJoin();
      report = s.executor->RuntimeReport();
    }
  } catch (...) {
    stop = engine::StopStatus::kCleanupFailed;
    report = engine::ExecutorRuntimeReport{};
  }

  bool fatal = !CleanStop(stop, report);
#ifdef EPOCANVA_TSF_TEST_SEAM
  if (g_force_cleanup_failure.load()) {
    fatal = true;
  }
#endif

  // The stop/join is over, but the executor object (and the host-active flag) still cover
  // the cleanup window. Publish the final runtime report before the gate so a snapshot in
  // that window sees the completed Shutdown while the executor is still owned.
  {
    std::lock_guard<std::mutex> guard(s.mutex);
    s.last_report = report;
    s.last_stop_status = stop;
    if (report.shutdown_thread_id != 0) {
      s.worker_thread_id = report.shutdown_thread_id;
    }
    if (fatal) {
      ++s.stop_failures;
    }
  }
  if (!fatal) {
    RunCleanupGate();
  }

  std::unique_ptr<engine::EngineExecutor> doomed;
  {
    std::lock_guard<std::mutex> guard(s.mutex);
    doomed = std::move(s.executor);
  }
  doomed.reset();

  {
    std::lock_guard<std::mutex> guard(s.mutex);
    if (fatal) {
      // Never pretend Idle and never let DllCanUnloadNow succeed after an unresolved
      // cleanup. The failed flag is process-lifetime.
      s.state = EngineHostState::kFailed;
      s.cleanup_failed = true;
      ModuleSetHostFailed(true);
    } else {
      s.state = EngineHostState::kIdle;
      s.config = EngineHostConfig{};
      ModuleSetHostActive(false);
    }
    s.cv.notify_all();
  }
}

bool EngineHostBroker::IsRunningFor(const EngineHostConfig& config) const {
  if (!config.valid()) {
    return false;
  }
  const Impl& s = *impl_;
  std::lock_guard<std::mutex> guard(s.mutex);
  if (s.state != EngineHostState::kRunning) {
    return false;
  }
  EngineHostConfig requested = config;
  EngineHostConfig running = s.config;
  requested.deployment.reset();
  running.deployment.reset();
  return requested == running;
}

void EngineHostBroker::Snapshot(EngineHostDiagnostics* diagnostics) const {
  if (diagnostics == nullptr) {
    return;
  }
  const Impl& s = *impl_;
  std::lock_guard<std::mutex> guard(s.mutex);
  diagnostics->state = s.state;
  diagnostics->lease_count = s.leases.size();
  diagnostics->executor_present = s.executor != nullptr;
  diagnostics->waiting_acquires = s.waiting_acquires;
  diagnostics->current_executor_instance = s.current_executor_id;
  diagnostics->executor_instances_created = s.executor_instances_created;
  diagnostics->start_attempts = s.start_attempts;
  diagnostics->start_failures = s.start_failures;
  diagnostics->stop_calls = s.stop_calls;
  diagnostics->stop_failures = s.stop_failures;
  diagnostics->acquire_rejections_stopping = s.acquire_rejections_stopping;
  diagnostics->acquire_rejections_failed = s.acquire_rejections_failed;
  diagnostics->config_mismatches = s.config_mismatches;
  diagnostics->release_underflows = s.release_underflows;
  diagnostics->abandoned_leases = s.abandoned_leases;
  diagnostics->cleanup_failed = s.cleanup_failed;
  diagnostics->abandoned = s.abandoned;
  diagnostics->runtime_start_status =
      static_cast<int>(s.last_report.runtime_start_status);
  diagnostics->runtime_shutdown_status =
      static_cast<int>(s.last_report.runtime_shutdown_status);
  diagnostics->start_diagnostic = s.last_report.diagnostic;
  diagnostics->start_thread_id = s.last_report.start_thread_id;
  diagnostics->shutdown_thread_id = s.last_report.shutdown_thread_id;
  diagnostics->worker_thread_id = s.worker_thread_id;
  diagnostics->initialize_calls = s.last_report.runtime_info.initialize_calls;
  diagnostics->finalize_calls = s.last_report.runtime_info.finalize_calls;
  diagnostics->deploy_calls = s.last_report.runtime_info.deploy_calls;
  diagnostics->live_sessions_after_cleanup =
      s.last_report.live_sessions_after_cleanup;
  diagnostics->api_balanced = s.last_report.api_counts.balanced();
  diagnostics->can_unload = ModuleCanUnload();
}

EngineLease::~EngineLease() {
  (void)Release();
}

EngineLease::EngineLease(EngineLease&& other) noexcept
    : broker_(other.broker_), id_(other.id_) {
  other.broker_ = nullptr;
  other.id_ = 0;
}

EngineLease& EngineLease::operator=(EngineLease&& other) noexcept {
  if (this != &other) {
    (void)Release();
    broker_ = other.broker_;
    id_ = other.id_;
    other.broker_ = nullptr;
    other.id_ = 0;
  }
  return *this;
}

EngineLeaseReleaseStatus EngineLease::Release() {
  if (broker_ == nullptr) {
    return EngineLeaseReleaseStatus::kAlreadyReleased;
  }
  EngineHostBroker* broker = broker_;
  const std::uint64_t id = id_;
  broker_ = nullptr;
  id_ = 0;
  return broker->ReleaseLease(id);
}

void EngineLease::Abandon() noexcept {
  if (broker_ == nullptr) {
    return;
  }
  EngineHostBroker* broker = broker_;
  const std::uint64_t id = id_;
  broker_ = nullptr;
  id_ = 0;
  broker->AbandonLease(id);
}

engine::EngineExecutor* EngineLease::executor() const {
  if (broker_ == nullptr) {
    return nullptr;
  }
  return broker_->ExecutorForLease(id_);
}

engine::EngineExecutor* EngineHostBroker::ExecutorForLease(std::uint64_t id) const {
  Impl& s = *impl_;
  std::lock_guard<std::mutex> guard(s.mutex);
  if (id == 0 || s.leases.count(id) == 0 || s.state != EngineHostState::kRunning) {
    return nullptr;
  }
  return s.executor.get();
}

#ifdef EPOCANVA_TSF_TEST_SEAM
void SetTestStartGate(void* entered, void* release) {
  std::lock_guard<std::mutex> guard(g_gate_mutex);
  g_start_entered = entered;
  g_start_release = release;
}

void SetTestStopGate(void* entered, void* release) {
  std::lock_guard<std::mutex> guard(g_gate_mutex);
  g_stop_entered = entered;
  g_stop_release = release;
}

void SetTestCleanupGate(void* entered, void* release) {
  std::lock_guard<std::mutex> guard(g_gate_mutex);
  g_cleanup_entered = entered;
  g_cleanup_release = release;
}

void SetTestForceCleanupFailure(bool force) {
  g_force_cleanup_failure.store(force);
}
#endif

}  // namespace tsf
}  // namespace epocanva
