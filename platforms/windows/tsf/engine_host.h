// EpoCanva P3.6 -- the process-wide engine host broker and its text-service leases.
//
// WHY THIS EXISTS. P3.5 built one bounded EngineExecutor per instance, but a TSF DLL loaded
// into an arbitrary host gets one Activate per text service and may get several services in
// one process (two STA threads, two documents, a restart after Deactivate). Starting one
// librime runtime per service is not possible: RimeRuntime is process-exclusive, and P3.5
// already made an executor single-use. This file owns the one process-wide lifetime:
// first lease starts one executor, later leases share it, the last release stops and joins
// it, and a later activation creates a NEW executor instance.
//
// WHAT A CALLER CAN AND CANNOT GET. A caller can acquire an EngineLease, read a value-only
// EngineHostDiagnostics snapshot, and -- W2 product wiring -- borrow the EngineExecutor
// pointer that the lease keeps alive. The pointer is valid only while this lease is held
// and must not be stored after Release()/Abandon(); it is not a second owner. There is no
// accessor returning RimeRuntime, RimeSession, InputController, a DLL handle, an ITf*
// object or a borrowed engine buffer. This header forward-declares EngineExecutor rather
// than including engine_executor.h.
//
// THREADING AND LOCKS. One broker mutex protects the state machine, configuration identity
// and lease table. Start and StopAndJoin are ALWAYS called with that mutex released. The
// executor's worker never calls back into this broker, so the lock order is simply:
//
//   service state mutex -> broker mutex -> (unlocked) executor lifecycle
//
// A text service never holds its own mutex while acquiring or releasing a lease. No COM
// object, ITfThreadMgr or ITfContext is stored here or sent to the worker.
//
// STATES. Idle -> Starting -> Running -> Stopping -> Idle, with Failed as a terminal state
// for an unrecoverable cleanup failure or an abandoned abnormal lifecycle. Starting is
// single-owner; concurrent Acquire calls with the same configuration wait for that one
// attempt. A different configuration is refused while an engine environment is active.
// Acquire during Stopping is refused, never queued, and a failed attempt is reported to
// every waiter; there is no implicit retry.
//
// CONFIGURATION. EngineHostConfig is copied at every entry point. Comparisons are exact
// (all four absolute paths, the capacity and the optional deployment request): two services
// that disagree are never silently merged into one engine environment. The product DLL now
// supplies the paths from its own module location and the per-user data root; the test seam
// remains only in the hosted test variant.
#ifndef EPOCANVA_TSF_ENGINE_HOST_H_
#define EPOCANVA_TSF_ENGINE_HOST_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace epocanva {
namespace windows {
namespace engine {
class EngineExecutor;
}
}
}

namespace epocanva {
namespace tsf {

enum class EngineHostState {
  kIdle = 0,
  kStarting,
  kRunning,
  kStopping,
  kFailed,
};

// Why Acquire returned. kStartFailed means this attempt failed but the worker was joined and
// the broker is Idle again, so a later attempt may build a new executor. kCleanupFailed means
// the failure left state that cannot be unloaded; no later Acquire is accepted.
enum class EngineLeaseStatus {
  kAcquired = 0,
  kInvalidConfig,
  kInvalidLease,     // null out-value or a lease that still owns a previous acquisition
  kStopping,         // admission is closed for the whole stop/cleanup window
  kFailed,           // terminal broker state (cleanup failure or abandoned host)
  kConfigMismatch,   // another engine environment is starting/running with different paths
  kStartFailed,      // start failed; cleanup completed and the broker is Idle
  kCleanupFailed,    // start failed and cleanup did not end safely; the broker is Failed
  kInternalFailure,  // allocation/construction failed before any worker existed
};

enum class EngineLeaseReleaseStatus {
  kReleased = 0,
  kAlreadyReleased,  // idempotent second release, or an unknown id; never decrements twice
};

// One deployment request the broker performs before the runtime is declared running. It is
// product wiring, not a test convenience: the product DLL supplies these files when settings
// changed, so the next activation compiles the schema and applies the user's patch. Every
// path is absolute and inside shared_data_dir; the runtime remains the authority on whether
// the files exist and are readable.
struct EngineHostDeployment {
  std::filesystem::path config_file;                 // e.g. <shared>/default.yaml
  std::vector<std::filesystem::path> schema_files;   // e.g. <shared>/epocanva_english.schema.yaml
};

struct EngineHostConfig {
  std::filesystem::path rime_dll_path;
  std::filesystem::path shared_data_dir;
  std::filesystem::path user_data_dir;
  std::filesystem::path log_dir;
  std::size_t capacity = 32;
  std::optional<EngineHostDeployment> deployment;

  // Absolute, non-empty paths and a bounded non-zero capacity. This is structural
  // validation only; the executor's RuntimeOptions validation remains the authority on
  // whether the directories exist.
  bool valid() const;
};

bool operator==(const EngineHostConfig& left, const EngineHostConfig& right);
bool operator!=(const EngineHostConfig& left, const EngineHostConfig& right);

// Process-wide configuration storage. A future product path will call this from an explicit
// startup step outside DllMain; this round only the test DLL variant calls it. The copy is
// taken when an activation starts, so changing the stored value cannot mutate a live lease.
bool SetEngineHostConfig(const EngineHostConfig& config);
bool GetEngineHostConfig(EngineHostConfig* config);  // false when no valid config is stored
void ClearEngineHostConfig();

// A value-only snapshot for the test seam and for review. No pointer into the executor and
// no user text is present. Integers are status enum values, not HRESULTs.
struct EngineHostDiagnostics {
  EngineHostState state = EngineHostState::kIdle;
  std::uint64_t lease_count = 0;
  bool executor_present = false;
  std::uint64_t waiting_acquires = 0;  // same-attempt callers currently waiting on the start
  std::uint64_t current_executor_instance = 0;
  std::uint64_t executor_instances_created = 0;
  std::uint64_t start_attempts = 0;
  std::uint64_t start_failures = 0;
  std::uint64_t stop_calls = 0;
  std::uint64_t stop_failures = 0;
  std::uint64_t acquire_rejections_stopping = 0;
  std::uint64_t acquire_rejections_failed = 0;
  std::uint64_t config_mismatches = 0;
  std::uint64_t release_underflows = 0;
  std::uint64_t abandoned_leases = 0;
  bool cleanup_failed = false;
  bool abandoned = false;
  int runtime_start_status = 0;
  int runtime_shutdown_status = 0;
  std::uint32_t start_thread_id = 0;     // worker thread that called Runtime::Start
  std::uint32_t shutdown_thread_id = 0;  // worker thread that called Runtime::Shutdown
  std::uint32_t worker_thread_id = 0;
  std::uint64_t initialize_calls = 0;
  std::uint64_t finalize_calls = 0;
  std::uint64_t deploy_calls = 0;
  int live_sessions_after_cleanup = 0;
  bool api_balanced = false;
  bool can_unload = false;
};

class EngineHostBroker;

// Move-only lease handle. Release is idempotent and never underflows the broker counter:
// the second call reports kAlreadyReleased and leaves the count untouched. Abandon() is the
// explicit abnormal-lifecycle path: it gives up the lease without stopping or joining, so a
// service destructor reached under an unknown loader-lock state never waits for a thread.
class EngineLease {
 public:
  EngineLease() = default;
  ~EngineLease();

  EngineLease(const EngineLease&) = delete;
  EngineLease& operator=(const EngineLease&) = delete;
  EngineLease(EngineLease&& other) noexcept;
  EngineLease& operator=(EngineLease&& other) noexcept;

  EngineLeaseReleaseStatus Release();
  void Abandon() noexcept;

  bool valid() const { return broker_ != nullptr; }
  std::uint64_t id() const { return id_; }

  // Borrowed executor kept alive by this lease. Null for an invalid/released lease or when
  // the broker is no longer Running. The caller must not submit after releasing the lease.
  ::epocanva::windows::engine::EngineExecutor* executor() const;

 private:
  friend class EngineHostBroker;
  EngineLease(EngineHostBroker* broker, std::uint64_t id) : broker_(broker), id_(id) {}

  EngineHostBroker* broker_ = nullptr;
  std::uint64_t id_ = 0;
};

class EngineHostBroker {
 public:
  static EngineHostBroker& Instance();

  EngineHostBroker(const EngineHostBroker&) = delete;
  EngineHostBroker& operator=(const EngineHostBroker&) = delete;

  ~EngineHostBroker();

  // Never called from DllMain. Blocks only while a start is in progress (to share its
  // result) or, for the last lease, while the explicit stop/join runs on the caller thread.
  EngineLeaseStatus Acquire(const EngineHostConfig& config, EngineLease* lease);

  // Release is normally called through EngineLease::Release. Exposed separately only so the
  // lease handle can stay pointer-free at the ABI boundary; callers use the lease.
  EngineLeaseReleaseStatus ReleaseLease(std::uint64_t id);

  // W2: true when this broker is Running with the same engine paths/capacity as `config`.
  // The deployment request is deliberately ignored: it is a one-shot preparation input,
  // not part of the running environment's identity. Used by the product activation to
  // decide whether a deployment can still be applied (it cannot while this engine runs).
  bool IsRunningFor(const EngineHostConfig& config) const;

  void Snapshot(EngineHostDiagnostics* diagnostics) const;

 private:
  EngineHostBroker();

  void AbandonLease(std::uint64_t id) noexcept;
  void StopOnCaller();
  ::epocanva::windows::engine::EngineExecutor* ExecutorForLease(
      std::uint64_t id) const;

  struct Impl;
  std::unique_ptr<Impl> impl_;

  friend class EngineLease;
};

// Test-only fixed lifecycle gates. They are compiled into the explicit test DLL variant and
// the broker test executable, never into a product build. Each pair is two opaque HANDLE
// values copied immediately: the DLL signals `entered` and then waits (INFINITE) for
// `release`. There is no callback, no user data and no access to an internal object.
#ifdef EPOCANVA_TSF_TEST_SEAM
void SetTestStartGate(void* entered, void* release);
void SetTestStopGate(void* entered, void* release);
void SetTestCleanupGate(void* entered, void* release);
void SetTestForceCleanupFailure(bool force);
#endif

}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TSF_ENGINE_HOST_H_
