// EpoCanva P3.6 -- private test seam of epocanva_tsf_hosted_test.dll.
//
// This translation unit is compiled with EPOCANVA_TSF_TEST_SEAM and is linked only into the
// test DLL. It converts the fixed ABI in test_seam_abi.h into EngineHostBroker calls. Every
// string is copied into an owning std::filesystem::path before returning; every result is a
// POD; no callback, user pointer or internal object crosses the boundary.
#include "test_seam_abi.h"

#include <new>
#include <utility>

#include "engine_host.h"
#include "module_state.h"

namespace {

using epocanva::tests::tsf_hosted::EngineConfigW;
using epocanva::tests::tsf_hosted::HostDiagnostics;
using epocanva::tests::tsf_hosted::LeaseProbe;
using epocanva::tsf::EngineHostBroker;
using epocanva::tsf::EngineHostConfig;
using epocanva::tsf::EngineHostDiagnostics;
using epocanva::tsf::EngineLease;
using epocanva::tsf::EngineLeaseReleaseStatus;
using epocanva::tsf::EngineLeaseStatus;

static_assert(static_cast<int>(epocanva::tsf::EngineHostState::kIdle) == 0, "abi drift");
static_assert(static_cast<int>(epocanva::tsf::EngineHostState::kStarting) == 1,
              "abi drift");
static_assert(static_cast<int>(epocanva::tsf::EngineHostState::kRunning) == 2, "abi drift");
static_assert(static_cast<int>(epocanva::tsf::EngineHostState::kStopping) == 3, "abi drift");
static_assert(static_cast<int>(epocanva::tsf::EngineHostState::kFailed) == 4, "abi drift");
static_assert(static_cast<int>(EngineLeaseStatus::kAcquired) == 0, "abi drift");
static_assert(static_cast<int>(EngineLeaseStatus::kInvalidConfig) == 1, "abi drift");
static_assert(static_cast<int>(EngineLeaseStatus::kInvalidLease) == 2, "abi drift");
static_assert(static_cast<int>(EngineLeaseStatus::kStopping) == 3, "abi drift");
static_assert(static_cast<int>(EngineLeaseStatus::kFailed) == 4, "abi drift");
static_assert(static_cast<int>(EngineLeaseStatus::kConfigMismatch) == 5, "abi drift");
static_assert(static_cast<int>(EngineLeaseStatus::kStartFailed) == 6, "abi drift");
static_assert(static_cast<int>(EngineLeaseStatus::kCleanupFailed) == 7, "abi drift");
static_assert(static_cast<int>(EngineLeaseStatus::kInternalFailure) == 8, "abi drift");
static_assert(static_cast<int>(EngineLeaseReleaseStatus::kReleased) == 0, "abi drift");
static_assert(static_cast<int>(EngineLeaseReleaseStatus::kAlreadyReleased) == 1, "abi drift");

void FillDiagnostics(HostDiagnostics* out) {
  if (out == nullptr || out->size != sizeof(HostDiagnostics)) {
    return;
  }
  EngineHostDiagnostics diagnostics;
  EngineHostBroker::Instance().Snapshot(&diagnostics);
  out->state = static_cast<std::int32_t>(diagnostics.state);
  out->lease_count = diagnostics.lease_count;
  out->waiting_acquires = diagnostics.waiting_acquires;
  out->executor_present = diagnostics.executor_present ? 1 : 0;
  out->current_executor_instance = diagnostics.current_executor_instance;
  out->executor_instances_created = diagnostics.executor_instances_created;
  out->start_attempts = diagnostics.start_attempts;
  out->start_failures = diagnostics.start_failures;
  out->stop_calls = diagnostics.stop_calls;
  out->stop_failures = diagnostics.stop_failures;
  out->acquire_rejections_stopping = diagnostics.acquire_rejections_stopping;
  out->acquire_rejections_failed = diagnostics.acquire_rejections_failed;
  out->config_mismatches = diagnostics.config_mismatches;
  out->release_underflows = diagnostics.release_underflows;
  out->abandoned_leases = diagnostics.abandoned_leases;
  out->cleanup_failed = diagnostics.cleanup_failed ? 1 : 0;
  out->abandoned = diagnostics.abandoned ? 1 : 0;
  out->runtime_start_status = diagnostics.runtime_start_status;
  out->runtime_shutdown_status = diagnostics.runtime_shutdown_status;
  out->start_thread_id = diagnostics.start_thread_id;
  out->shutdown_thread_id = diagnostics.shutdown_thread_id;
  out->worker_thread_id = diagnostics.worker_thread_id;
  out->initialize_calls = diagnostics.initialize_calls;
  out->finalize_calls = diagnostics.finalize_calls;
  out->deploy_calls = diagnostics.deploy_calls;
  out->live_sessions_after_cleanup = diagnostics.live_sessions_after_cleanup;
  out->api_balanced = diagnostics.api_balanced ? 1 : 0;
  out->can_unload = diagnostics.can_unload ? 1 : 0;
  out->module_live_objects = epocanva::tsf::ModuleLiveObjectCount();
  out->module_server_locks = epocanva::tsf::ModuleServerLockCount();
  out->host_active = epocanva::tsf::ModuleHostActiveFlag();
  out->host_failed = epocanva::tsf::ModuleHostFailedFlag();
}

}  // namespace

extern "C" HRESULT WINAPI EpoCanvaTestSetEngineConfig(const EngineConfigW* input) {
  if (input == nullptr || input->size != sizeof(EngineConfigW)) {
    return E_INVALIDARG;
  }
  try {
    EngineHostConfig config;
    if (input->rime_dll_path != nullptr) {
      config.rime_dll_path = std::filesystem::path(input->rime_dll_path);
    }
    if (input->shared_data_dir != nullptr) {
      config.shared_data_dir = std::filesystem::path(input->shared_data_dir);
    }
    if (input->user_data_dir != nullptr) {
      config.user_data_dir = std::filesystem::path(input->user_data_dir);
    }
    if (input->log_dir != nullptr) {
      config.log_dir = std::filesystem::path(input->log_dir);
    }
    config.capacity = input->capacity;
    // The path objects above own their copies; the caller may free every input buffer now.
    return epocanva::tsf::SetEngineHostConfig(config) ? S_OK : E_INVALIDARG;
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

extern "C" void WINAPI EpoCanvaTestClearEngineConfig() {
  epocanva::tsf::ClearEngineHostConfig();
}

extern "C" void WINAPI EpoCanvaTestGetDiagnostics(HostDiagnostics* out) {
  try {
    FillDiagnostics(out);
  } catch (...) {
    // The seam never throws across the DLL boundary. A null/partially filled structure is a
    // visible test failure at the caller.
  }
}

extern "C" void WINAPI EpoCanvaTestSetStartGate(HANDLE entered, HANDLE release) {
  epocanva::tsf::SetTestStartGate(entered, release);
}

extern "C" void WINAPI EpoCanvaTestSetStopGate(HANDLE entered, HANDLE release) {
  epocanva::tsf::SetTestStopGate(entered, release);
}

extern "C" void WINAPI EpoCanvaTestSetCleanupGate(HANDLE entered, HANDLE release) {
  epocanva::tsf::SetTestCleanupGate(entered, release);
}

extern "C" void WINAPI EpoCanvaTestSetForceCleanupFailure(int force) {
  epocanva::tsf::SetTestForceCleanupFailure(force != 0);
}

extern "C" void WINAPI EpoCanvaTestLeaseDoubleRelease(LeaseProbe* out) {
  if (out == nullptr || out->size != sizeof(LeaseProbe)) {
    return;
  }
  out->acquire_status = static_cast<std::int32_t>(EngineLeaseStatus::kInvalidConfig);
  out->first_release_status =
      static_cast<std::int32_t>(EngineLeaseReleaseStatus::kAlreadyReleased);
  out->second_release_status =
      static_cast<std::int32_t>(EngineLeaseReleaseStatus::kAlreadyReleased);
  out->broker_duplicate_status =
      static_cast<std::int32_t>(EngineLeaseReleaseStatus::kAlreadyReleased);
  out->lease_after_acquire = 0;
  out->lease_after_first_release = 0;
  out->lease_after_second_release = 0;
  out->release_underflows_after_second = 0;
  out->can_unload_after_acquire = 0;
  out->can_unload_after_first_release = 0;

  try {
    EngineHostConfig config;
    if (!epocanva::tsf::GetEngineHostConfig(&config)) {
      return;
    }
    EngineLease lease;
    const EngineLeaseStatus acquired = EngineHostBroker::Instance().Acquire(config, &lease);
    out->acquire_status = static_cast<std::int32_t>(acquired);
    if (acquired != EngineLeaseStatus::kAcquired) {
      return;
    }
    HostDiagnostics diagnostics{};
    diagnostics.size = sizeof(HostDiagnostics);
    FillDiagnostics(&diagnostics);
    out->lease_after_acquire = diagnostics.lease_count;
    out->can_unload_after_acquire = diagnostics.can_unload;
    const std::uint64_t id = lease.id();

    out->first_release_status = static_cast<std::int32_t>(lease.Release());
    FillDiagnostics(&diagnostics);
    out->lease_after_first_release = diagnostics.lease_count;
    out->can_unload_after_first_release = diagnostics.can_unload;

    out->second_release_status = static_cast<std::int32_t>(lease.Release());
    FillDiagnostics(&diagnostics);
    out->lease_after_second_release = diagnostics.lease_count;
    out->release_underflows_after_second = diagnostics.release_underflows;

    // The handle has already forgotten the id; exercise the broker's own duplicate guard
    // with that id. It must refuse and count the refusal without touching the count.
    if (id != 0) {
      out->broker_duplicate_status =
          static_cast<std::int32_t>(EngineHostBroker::Instance().ReleaseLease(id));
      FillDiagnostics(&diagnostics);
      out->release_underflows_after_second = diagnostics.release_underflows;
    }
  } catch (...) {
    // Leave the fixed status values as the observable failure.
  }
}
