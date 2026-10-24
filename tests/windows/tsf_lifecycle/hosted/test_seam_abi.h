// EpoCanva P3.6 -- private ABI of the hosted test DLL variant.
//
// TEST ONLY. This header is compiled into tests/windows/tsf_lifecycle/hosted/test_seam.cc
// (inside epocanva_tsf_hosted_test.dll) and into the hosted verification program. It is not
// part of the product DLL, is not installed, and adds no export to the product .def.
//
// The seam is deliberately narrow: fixed C strings copied immediately into std::filesystem
// paths, fixed POD result structures, and two opaque HANDLEs per lifecycle gate. There is no
// callback registration, no user pointer, no STL type across the boundary, no accessor for
// EngineExecutor/RimeRuntime/InputController and no way to obtain an internal object.
#ifndef EPOCANVA_TESTS_TSF_HOSTED_TEST_SEAM_ABI_H_
#define EPOCANVA_TESTS_TSF_HOSTED_TEST_SEAM_ABI_H_

#include <windows.h>

#include <cstdint>

namespace epocanva {
namespace tests {
namespace tsf_hosted {

// Mirrors epocanva::tsf::EngineHostState; test_seam.cc static_asserts the values.
enum HostStateValue : std::int32_t {
  kHostIdle = 0,
  kHostStarting = 1,
  kHostRunning = 2,
  kHostStopping = 3,
  kHostFailed = 4,
};

// Mirrors EngineLeaseStatus.
enum LeaseStatusValue : std::int32_t {
  kLeaseAcquired = 0,
  kLeaseInvalidConfig = 1,
  kLeaseInvalidLease = 2,
  kLeaseStopping = 3,
  kLeaseFailed = 4,
  kLeaseConfigMismatch = 5,
  kLeaseStartFailed = 6,
  kLeaseCleanupFailed = 7,
  kLeaseInternalFailure = 8,
};

// Mirrors EngineLeaseReleaseStatus.
enum ReleaseStatusValue : std::int32_t {
  kLeaseReleased = 0,
  kLeaseAlreadyReleased = 1,
};

struct EngineConfigW {
  std::uint32_t size;  // sizeof(EngineConfigW); checked by the DLL before reading
  const wchar_t* rime_dll_path;
  const wchar_t* shared_data_dir;
  const wchar_t* user_data_dir;
  const wchar_t* log_dir;
  std::uint32_t capacity;
};

struct HostDiagnostics {
  std::uint32_t size;
  std::int32_t state;
  std::uint64_t lease_count;
  std::uint64_t waiting_acquires;
  std::int32_t executor_present;
  std::uint64_t current_executor_instance;
  std::uint64_t executor_instances_created;
  std::uint64_t start_attempts;
  std::uint64_t start_failures;
  std::uint64_t stop_calls;
  std::uint64_t stop_failures;
  std::uint64_t acquire_rejections_stopping;
  std::uint64_t acquire_rejections_failed;
  std::uint64_t config_mismatches;
  std::uint64_t release_underflows;
  std::uint64_t abandoned_leases;
  std::int32_t cleanup_failed;
  std::int32_t abandoned;
  std::int32_t runtime_start_status;
  std::int32_t runtime_shutdown_status;
  std::uint32_t start_thread_id;
  std::uint32_t shutdown_thread_id;
  std::uint32_t worker_thread_id;
  std::uint64_t initialize_calls;
  std::uint64_t finalize_calls;
  std::uint64_t deploy_calls;
  std::int32_t live_sessions_after_cleanup;
  std::int32_t api_balanced;
  std::int32_t can_unload;
  std::int32_t module_live_objects;
  std::int32_t module_server_locks;
  std::int32_t host_active;
  std::int32_t host_failed;
};

struct LeaseProbe {
  std::uint32_t size;
  std::int32_t acquire_status;
  std::int32_t first_release_status;
  std::int32_t second_release_status;
  std::int32_t broker_duplicate_status;
  std::uint64_t lease_after_acquire;
  std::uint64_t lease_after_first_release;
  std::uint64_t lease_after_second_release;
  std::uint64_t release_underflows_after_second;
  std::int32_t can_unload_after_acquire;
  std::int32_t can_unload_after_first_release;
};

using SetEngineConfigFn = HRESULT(WINAPI*)(const EngineConfigW*);
using ClearEngineConfigFn = void(WINAPI*)();
using GetDiagnosticsFn = void(WINAPI*)(HostDiagnostics*);
using SetGateFn = void(WINAPI*)(HANDLE entered, HANDLE release);
using SetForceCleanupFailureFn = void(WINAPI*)(int force);
using LeaseProbeFn = void(WINAPI*)(LeaseProbe*);

}  // namespace tsf_hosted
}  // namespace tests
}  // namespace epocanva

#endif  // EPOCANVA_TESTS_TSF_HOSTED_TEST_SEAM_ABI_H_
