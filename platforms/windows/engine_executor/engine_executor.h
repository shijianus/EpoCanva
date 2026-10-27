// EpoCanva P3.5 -- a bounded, single-threaded executor for the P3.4 engine bridge.
//
// WHY THIS EXISTS. Every P3.4 engine call is owner-thread affine: RimeRuntime records the
// thread that called Start, and Runtime/Session/Controller refuse to run anywhere else
// (rime_runtime.h:33-35, rime_session.h:13-16, input_controller.h:39-43). RimeRuntime is also
// process-exclusive. A host will call from several threads, so those calls must be converted
// into value commands and executed in order on one dedicated worker thread that owns the one
// runtime and every session/controller. This header is the whole caller-facing surface.
//
// WHAT A CALLER CANNOT GET. There is no accessor returning RimeRuntime, RimeSession,
// InputController, a DLL handle, an ITf* object or a borrowed engine buffer. Commands are
// copied into executor-owned storage, results are copies, and both cross the thread boundary
// as values. The backend that does the real work is an internal implementation detail
// (engine_backend.h); it is not part of this contract.
//
// COMMANDS. Every command is typed, not an arbitrary std::function:
//   kCreateClient     create the engine session and controller for one client
//   kEvaluate         run the pure P3.3 decision only; no process_key, no state read
//   kDispatch         re-evaluate against the cached state and dispatch at most one key
//   kAcceptCandidate  one explicit select_candidate_on_current_page call
//   kRefreshState     read-only get_status/get_context/get_input refresh
//   kCloseClient      destroy the controller and close the engine session
// There is no generic task system, thread pool, callback registry or cancellation token.
//
// ORDER AND CAPACITY. One global FIFO feeds the single worker, so commands accepted from one
// caller for one client cannot be reordered. A request occupies one capacity slot from
// acceptance until its result is taken; queued, executing and completed-but-unconsumed
// requests all count. When full, TrySubmit returns kQueueFull without blocking, dropping an
// older request or overwriting an older result. Taking a result releases its slot. There is no
// result eviction. This round has no cancellation: an accepted command always terminates, and
// a wait timeout is not a cancellation and does not retry anything.
//
// LIFECYCLE. NotStarted -> Starting -> Running -> Stopping -> Stopped (or Failed). One
// instance never restarts; a start failure is terminal for that object. Start/Stop must happen
// outside DllMain. StopAndJoin stops admission atomically, drains every already-accepted
// command, destroys all clients on the worker, closes the runtime, exits the worker and joins.
// It does not wait for callers to take results; results remain readable while the executor
// object lives. No thread is detached and no thread is terminated.
//
// CLIENT TOKENS. ClientId values are allocated once, before the create command runs, and are
// never reused. A command for an unknown or never-created client is rejected. As soon as a
// close is accepted, later commands for that client are rejected; older accepted commands
// still run first because the queue is FIFO. Closing a client does not erase results that its
// earlier commands already produced.
//
// UNITS. RequestId/ClientId are opaque 64-bit identifiers. `executing_thread_id` is a Win32
// thread id (a DWORD), not a position or length. Engine text in results follows
// engine_update.h: UTF-16 code units for host-facing text, UTF-8 bytes for lengths.
//
// WHAT THIS ROUND DOES NOT DO. No TSF sink, no registration, no host text edit, no candidate
// UI, no focus handling, no key-eating policy. A pending commit in a result is text the engine
// queued for a host; it is not evidence that any application accepted it.
#ifndef EPOCANVA_WINDOWS_ENGINE_EXECUTOR_ENGINE_EXECUTOR_H_
#define EPOCANVA_WINDOWS_ENGINE_EXECUTOR_ENGINE_EXECUTOR_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>

#include <windows.h>

#include "engine_update.h"
#include "input_controller.h"
#include "key_event.h"
#include "rime_runtime.h"

namespace epocanva {
namespace windows {
namespace engine {

struct RequestId {
  std::uint64_t value = 0;
  bool valid() const { return value != 0; }
};
inline bool operator==(const RequestId& left, const RequestId& right) {
  return left.value == right.value;
}
inline bool operator!=(const RequestId& left, const RequestId& right) {
  return !(left == right);
}

struct ClientId {
  std::uint64_t value = 0;
  bool valid() const { return value != 0; }
};
inline bool operator==(const ClientId& left, const ClientId& right) {
  return left.value == right.value;
}
inline bool operator!=(const ClientId& left, const ClientId& right) {
  return !(left == right);
}

enum class ExecutorState {
  kNotStarted = 0,
  kStarting,
  kRunning,
  kStopping,
  kStopped,
  kFailed,
};

struct ExecutorOptions {
  RuntimeOptions runtime;
  // Total accepted requests that may be queued, executing, or completed-but-unconsumed.
  std::size_t capacity = 32;
};

// Why Start returned. kAlreadyStarted covers any second Start, including after failure: this
// object is single-use.
enum class StartStatus {
  kOk = 0,
  kAlreadyStarted,
  kInvalidOptions,     // capacity == 0 or a structurally invalid option value
  kThreadStartFailed,  // std::thread construction threw
  kRuntimeStartFailed,  // the worker could not start the real engine runtime
  kStoppedDuringStart,  // StopAndJoin won the race with Start
};

// Why StopAndJoin returned. Repeating a clean Stop reports kAlreadyStopped.
enum class StopStatus {
  kOk = 0,
  kNotStarted,       // Start was never called
  kAlreadyStopped,   // this object already reached a terminal state and reported it
  kSelfJoinRefused,  // called on the worker thread; no join was attempted
  kExecutorFailed,   // the executor was already Failed (for example a runtime start failure)
  kCleanupFailed,    // this call stopped the worker but runtime/client cleanup failed
};

enum class SubmitStatus {
  kAccepted = 0,
  kNotRunning,     // Starting, Stopping, Stopped or Failed
  kQueueFull,      // every capacity slot is queued, executing or unconsumed
  kClientUnknown,  // no such ClientId was ever issued
  kClientNotOpen,  // the client is closing, closed or failed
  kInvalidRequest, // null output pointer, empty schema id, zero client id, ...
  kInternalFailure,// allocation failed while enqueuing; nothing was accepted
};

enum class CommandKind {
  kCreateClient = 0,
  kEvaluate,
  kDispatch,
  kAcceptCandidate,
  kRefreshState,
  kCloseClient,
};

// Terminal status of one accepted command. This is deliberately separate from the P3.4
// DispatchStatus/CandidateStatus: kOk means "the command executed and the payload is
// meaningful", not "the key was handled" and not "a document changed".
enum class CommandStatus {
  kOk = 0,
  kClientUnavailable,   // the backend no longer has this client (for example a failed create)
  kSessionCreateFailed,
  kSessionCloseFailed,
  kControllerFailed,    // the controller reported a state/engine fault
  kInternalFault,       // an exception escaped this command; the executor stayed usable
  kExecutorFailed,      // the whole executor failed before/while this request could run
  kInvalidRequest,
};

struct CreateClientOutcome {
  ClientId client;
  SessionCreateStatus session_status = SessionCreateStatus::kNotStarted;
  std::string diagnostic;  // statuses and counts only; never input text
};

struct EvaluateOutcome {
  KeyEvaluation evaluation;
};

struct DispatchCommandOutcome {
  DispatchOutcome outcome;
};

struct CandidateCommandOutcome {
  CandidateOutcome outcome;
};

struct RefreshOutcome {
  ControllerStatus controller_status = ControllerStatus::kNoSession;
  EngineState state;  // meaningful only when controller_status == kOk
};

struct CloseClientOutcome {
  // Meaningful only when the enclosing CommandResult status is kOk or
  // kSessionCloseFailed. SessionCloseStatus has no "not attempted" value; a fault result
  // therefore leaves this at kOk and the command status is the authority.
  SessionCloseStatus session_status = SessionCloseStatus::kOk;
};

using CommandPayload =
    std::variant<CreateClientOutcome, EvaluateOutcome, DispatchCommandOutcome,
                 CandidateCommandOutcome, RefreshOutcome, CloseClientOutcome>;

struct CommandResult {
  RequestId request;
  CommandKind kind = CommandKind::kCreateClient;
  ClientId client;
  CommandStatus status = CommandStatus::kInvalidRequest;
  CommandPayload payload;  // the alternative matches `kind`
  // Win32 thread id of the worker that executed this command. 0 if it was never executed.
  std::uint32_t executing_thread_id = 0;
  std::string diagnostic;  // text-free: status names, counts, stage names

  bool ok() const { return status == CommandStatus::kOk; }
};

enum class WaitStatus {
  kReady = 0,
  kTimedOut,       // the request is still queued/executing; nothing was cancelled
  kUnknownRequest, // never accepted by this executor
  kAlreadyTaken,   // the result was already consumed
};

enum class TakeStatus {
  kTaken = 0,
  kNotReady,
  kUnknownRequest,
  kAlreadyTaken,
  kInvalidRequest,  // null output pointer
};

// Live capacity accounting. The three components sum to `occupied`.
struct CapacityUsage {
  std::size_t capacity = 0;
  std::size_t occupied = 0;
  std::size_t queued = 0;
  std::size_t executing = 0;
  std::size_t completed_unconsumed = 0;
};

// Client lifecycle accounting. `live` counts clients that still own a real engine session:
// accepted-but-not-created, open, and closing. `close_requests` counts accepted close
// commands; `close_results_taken` counts the close results a caller really consumed. The two
// differ while a close is queued, executing, or completed-but-unconsumed, which is the
// evidence a host needs to prove it reclaimed every retired client rather than waiting for
// StopAndJoin. These are counts only; no client identity or engine state is exposed.
struct ClientUsage {
  std::size_t created = 0;
  std::size_t pending = 0;
  std::size_t open = 0;
  std::size_t closing = 0;
  std::size_t closed = 0;
  std::size_t failed = 0;
  std::size_t live = 0;
  std::uint64_t close_requests = 0;
  std::uint64_t close_results_taken = 0;
  std::uint64_t close_admission_refusals = 0;
};

// A value copy of what the worker saw. `api_counts` is captured on the worker during Stop,
// because the bridge's counters are only safe to read from its owner thread. No borrowed
// pointer is retained.
struct ExecutorRuntimeReport {
  ExecutorState state = ExecutorState::kNotStarted;
  RuntimeStartStatus runtime_start_status = RuntimeStartStatus::kInvalidOptions;
  RuntimeShutdownStatus runtime_shutdown_status = RuntimeShutdownStatus::kNotStarted;
  std::uint32_t start_thread_id = 0;     // Win32 thread id that called Runtime::Start
  std::uint32_t shutdown_thread_id = 0;  // Win32 thread id that called Runtime::Shutdown
  RuntimeInfo runtime_info;
  ApiCallCounts api_counts;
  std::uint64_t clients_created = 0;
  std::uint64_t clients_closed = 0;
  std::uint64_t client_close_failures = 0;
  int live_sessions_after_cleanup = 0;
  std::string diagnostic;  // text-free
};

class EngineBackend;  // internal; defined in engine_backend.h
class EngineExecutorTestFactory;

class EngineExecutor {
 public:
  explicit EngineExecutor(ExecutorOptions options);
  ~EngineExecutor();

  EngineExecutor(const EngineExecutor&) = delete;
  EngineExecutor& operator=(const EngineExecutor&) = delete;
  EngineExecutor(EngineExecutor&&) = delete;
  EngineExecutor& operator=(EngineExecutor&&) = delete;

  // Explicit lifecycle. Never called from DllMain. One Start attempt per object.
  StartStatus Start();
  StopStatus StopAndJoin();

  ExecutorState state() const;
  // The Win32 thread id of the worker once it has started; stable after Stop for diagnostics.
  std::uint32_t worker_thread_id() const;

  // Submissions never block on the engine and never wait for capacity.
  SubmitStatus TryCreateClient(const std::string& schema_id,
                               const ControllerOptions& options,
                               RequestId* request, ClientId* client);
  SubmitStatus TryEvaluate(ClientId client, const input::KeyEventSnapshot& event,
                           const input::KeyText& text, RequestId* request);
  SubmitStatus TryDispatch(ClientId client, const input::KeyEventSnapshot& event,
                           const input::KeyText& text, RequestId* request);
  SubmitStatus TryAcceptCandidate(ClientId client, std::size_t candidate_index,
                                  RequestId* request);
  SubmitStatus TryRefreshState(ClientId client, RequestId* request);
  SubmitStatus TryCloseClient(ClientId client, RequestId* request);

  // Waiting is read-only: a timeout does not cancel, retry, or mark anything consumed.
  WaitStatus WaitForResult(RequestId request, std::chrono::milliseconds timeout);
  // Consumes the result exactly once. A second take reports kAlreadyTaken.
  TakeStatus TakeResult(RequestId request, CommandResult* result);

  // W2: one auto-reset Win32 event signalled whenever a terminal result is published.
  // It is a wake hint, not a result: waiting on it does not consume or reorder anything,
  // and the caller still takes each result by id. Null only if CreateEvent failed.
  HANDLE ResultReadyEvent() const;

  CapacityUsage capacity() const;
  // Client lifecycle counts. Reading them never waits and never consumes anything.
  ClientUsage clients() const;
  ExecutorRuntimeReport RuntimeReport() const;

 private:
  EngineExecutor(ExecutorOptions options, std::unique_ptr<EngineBackend> backend);
  friend class EngineExecutorTestFactory;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

const char* DescribeExecutorState(ExecutorState state);
const char* DescribeStartStatus(StartStatus status);
const char* DescribeStopStatus(StopStatus status);
const char* DescribeSubmitStatus(SubmitStatus status);
const char* DescribeCommandKind(CommandKind kind);
const char* DescribeCommandStatus(CommandStatus status);
const char* DescribeWaitStatus(WaitStatus status);
const char* DescribeTakeStatus(TakeStatus status);

}  // namespace engine
}  // namespace windows
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_ENGINE_EXECUTOR_ENGINE_EXECUTOR_H_
