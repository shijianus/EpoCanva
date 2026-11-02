// EpoCanva P3.5 -- internal worker-side backend seam for EngineExecutor.
//
// NOT A CALLER-FACING HEADER. EngineExecutor is the only owner of an EngineBackend, and every
// method below is called on the executor's single worker thread, in queue order, with no
// lock held. The backend owns the RimeRuntime and the session/controller table. It never
// returns one of those objects: results are copies. This seam exists so the scheduling layer
// (state machine, capacity, FIFO, result store) can be verified with a controlled backend
// while the real-engine verification uses RealEngineBackend, and so neither test pretends to
// be the other.
//
// THREAD RULE. Implementations may assert (and the real one relies on) being called only from
// the worker. They must not start threads, block on caller-owned locks, or call back into
// EngineExecutor. A command-level failure is returned as a status; a thrown exception is
// caught by the executor and becomes a terminal kInternalFault result without killing the
// worker. Only a failure of StartRuntime, CleanupAllClients or the queue itself is
// executor-wide.
#ifndef EPOCANVA_WINDOWS_ENGINE_EXECUTOR_ENGINE_BACKEND_H_
#define EPOCANVA_WINDOWS_ENGINE_EXECUTOR_ENGINE_BACKEND_H_

#include <cstdint>
#include <string>

#include "engine_executor.h"
#include "engine_update.h"
#include "input_controller.h"
#include "key_event.h"
#include "rime_runtime.h"

namespace epocanva {
namespace windows {
namespace engine {

struct BackendStartResult {
  RuntimeStartStatus status = RuntimeStartStatus::kInvalidOptions;
  RuntimeInfo info;
  std::string diagnostic;
  std::uint32_t thread_id = 0;  // Win32 thread id that attempted StartRuntime
};

struct BackendCreateResult {
  SessionCreateStatus status = SessionCreateStatus::kNotStarted;
  bool client_ready = false;
  std::string diagnostic;
  bool ok() const { return status == SessionCreateStatus::kOk && client_ready; }
};

struct BackendEvaluateResult {
  bool client_found = false;
  KeyEvaluation evaluation;
};

struct BackendDispatchResult {
  bool client_found = false;
  DispatchOutcome outcome;
};

struct BackendCandidateResult {
  bool client_found = false;
  CandidateOutcome outcome;
};

struct BackendRefreshResult {
  bool client_found = false;
  ControllerStatus status = ControllerStatus::kNoSession;
  EngineState state;  // meaningful only when status == ControllerStatus::kOk
};

struct BackendCloseResult {
  bool client_found = false;
  // SessionCloseStatus has no "not attempted" value. A caller must check client_found; on a
  // found client every implementation assigns this before returning.
  SessionCloseStatus status = SessionCloseStatus::kOk;
  std::string diagnostic;
};

struct BackendCleanupReport {
  RuntimeShutdownStatus shutdown_status = RuntimeShutdownStatus::kNotStarted;
  std::uint32_t shutdown_thread_id = 0;
  std::uint64_t clients_closed = 0;
  std::uint64_t close_failures = 0;
  int live_sessions_after = 0;
  std::string diagnostic;
};

class EngineBackend {
 public:
  virtual ~EngineBackend() = default;

  // Called exactly once on the worker before any command.
  virtual BackendStartResult StartRuntime(const RuntimeOptions& options) = 0;

  // `client_id` was allocated by the executor and is never reused. On success the backend
  // must register a controller for exactly this id.
  virtual BackendCreateResult CreateClient(std::uint64_t client_id,
                                           const std::string& schema_id,
                                           const ControllerOptions& options) = 0;

  virtual BackendEvaluateResult Evaluate(std::uint64_t client_id,
                                         const input::KeyEventSnapshot& event,
                                         const input::KeyText& text) = 0;
  virtual BackendDispatchResult Dispatch(std::uint64_t client_id,
                                         const input::KeyEventSnapshot& event,
                                         const input::KeyText& text) = 0;
  virtual BackendCandidateResult AcceptCandidate(std::uint64_t client_id,
                                                 std::size_t candidate_index) = 0;
  virtual BackendRefreshResult Refresh(std::uint64_t client_id) = 0;
  // Explicitly unavailable on the legacy schema adapter/test seam.
  virtual ProfileClientOutcome CreateProfileClient(std::uint64_t client_id,
      std::uint32_t profile_id, const BackendAccess& access) {
    (void)client_id; (void)profile_id; (void)access;
    ProfileClientOutcome result;
    result.backend_status = EPOCANVA_BACKEND_UNAVAILABLE;
    return result;
  }
  virtual BackendWorkOutcome RunBackendWork(std::uint64_t client_id,
                                            const BackendWork& work) {
    (void)client_id; (void)work;
    BackendWorkOutcome result;
    result.backend_status = EPOCANVA_BACKEND_UNAVAILABLE;
    return result;
  }
  virtual BackendCloseResult CloseClient(std::uint64_t client_id) = 0;

  // Called once on the worker during Stop, after every accepted command has executed. Must
  // destroy every remaining client (including sessions) on this thread, then stop the runtime.
  virtual BackendCleanupReport CleanupAllClients() = 0;

  // Safe value copies. ApiCountsCopy is only called by the executor during Stop, when the
  // worker is the only thread touching the bridge.
  virtual RuntimeInfo RuntimeInfoCopy() const = 0;
  virtual ApiCallCounts ApiCountsCopy() const = 0;
};

}  // namespace engine
}  // namespace windows
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_ENGINE_EXECUTOR_ENGINE_BACKEND_H_
