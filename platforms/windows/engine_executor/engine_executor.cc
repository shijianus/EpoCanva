// EpoCanva P3.5 -- EngineExecutor implementation. See engine_executor.h for the contract.
//
// LOCK DISCIPLINE. `mutex_` guards the queue, client lifecycle table, request/result store and
// the report copy. It is held only to inspect or mutate those values: it is never held while
// calling an EngineBackend method, so a real process_key/session-create call cannot block a
// submitter for the duration of engine work. The worker pops a command under the lock, unlocks,
// executes, and re-locks only to publish the terminal result. Callers waiting for a result use
// `result_cv_`; the worker never waits for a caller to take anything.
//
// REQUEST IDS AND "ALREADY TAKEN". IDs are allocated only for accepted commands, with no gaps.
// A request is in exactly one of three states: outstanding (queued or executing), completed
// and still stored, or consumed. Therefore an id <= last_submitted_ that is neither
// outstanding nor stored can only have been taken; no unbounded tombstone set is needed.
#include "engine_executor.h"

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine_backend.h"
#include "engine_executor_internal.h"

namespace epocanva {
namespace windows {
namespace engine {
namespace {

enum class ClientLifecycle {
  kPending = 0,  // the create command is accepted but has not run
  kOpen,
  kClosing,  // a close command was accepted; later commands are refused
  kClosed,
  kFailed,
};

struct OutstandingMeta {
  CommandKind kind = CommandKind::kCreateClient;
  std::uint64_t client = 0;
};

struct EngineCommand {
  std::uint64_t request = 0;
  CommandKind kind = CommandKind::kCreateClient;
  std::uint64_t client = 0;
  std::string schema_id;
  ControllerOptions controller_options;
  input::KeyEventSnapshot event;
  input::KeyText text;
  std::size_t candidate_index = 0;
};

CommandPayload DefaultPayload(const EngineCommand& command) {
  switch (command.kind) {
    case CommandKind::kCreateClient: {
      CreateClientOutcome outcome;
      outcome.client = ClientId{command.client};
      return outcome;
    }
    case CommandKind::kEvaluate:
      return EvaluateOutcome{};
    case CommandKind::kDispatch:
      return DispatchCommandOutcome{};
    case CommandKind::kAcceptCandidate:
      return CandidateCommandOutcome{};
    case CommandKind::kRefreshState:
      return RefreshOutcome{};
    case CommandKind::kCloseClient:
      return CloseClientOutcome{};
  }
  return CreateClientOutcome{};
}

CommandResult FaultResult(const EngineCommand& command, CommandStatus status,
                          std::string diagnostic) {
  CommandResult result;
  result.request = RequestId{command.request};
  result.kind = command.kind;
  result.client = ClientId{command.client};
  result.status = status;
  result.payload = DefaultPayload(command);
  result.executing_thread_id = ::GetCurrentThreadId();
  result.diagnostic = std::move(diagnostic);
  return result;
}

void AppendDiagnostic(std::string* target, const std::string& value) {
  if (value.empty()) {
    return;
  }
  if (!target->empty()) {
    *target += "; ";
  }
  *target += value;
}

}  // namespace

struct EngineExecutor::Impl {
  Impl(ExecutorOptions options, std::unique_ptr<EngineBackend> backend)
      : options_(std::move(options)),
        backend_(std::move(backend)),
        capacity_(options_.capacity) {
    // Auto-reset: one wait consumes one wake. Results are never lost because the STA
    // drains everything it can take, and a later publication sets the event again.
    result_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  }

  ~Impl() {
    if (result_event_ != nullptr) {
      ::CloseHandle(result_event_);
      result_event_ = nullptr;
    }
  }

  // --- lifecycle -----------------------------------------------------------------------
  StartStatus Start();
  StopStatus StopAndJoin();
  void JoinWorker();
  void WorkerMain();
  void FinishStop(bool runtime_started);

  ExecutorState State() const;
  std::uint32_t WorkerThreadId() const;

  // --- submission and results ------------------------------------------------------------
  SubmitStatus Submit(EngineCommand command, bool create, RequestId* request,
                      ClientId* client);
  WaitStatus Wait(RequestId request, std::chrono::milliseconds timeout);
  TakeStatus Take(RequestId request, CommandResult* result);
  CapacityUsage Capacity() const;
  ClientUsage ClientCounts() const;
  ExecutorRuntimeReport Report() const;

  // --- worker-side helpers ---------------------------------------------------------------
  CommandResult Execute(const EngineCommand& command);
  void SetClientStatus(std::uint64_t client, ClientLifecycle lifecycle);
  void NoteClientCreated();

  ExecutorOptions options_;
  std::unique_ptr<EngineBackend> backend_;

  mutable std::mutex mutex_;
  std::condition_variable work_cv_;       // worker waits for a command or stop
  std::condition_variable result_cv_;     // callers wait for a terminal result
  std::condition_variable lifecycle_cv_;  // Start/Stop wait for a state transition
  std::mutex join_mutex_;                 // exactly one join of `worker_`
  std::mutex stop_mutex_;                 // StopAndJoin is callable repeatedly
  std::thread worker_;
  std::atomic<std::uint32_t> worker_thread_id_{0};

  bool start_attempted_ = false;
  bool stop_requested_ = false;
  bool stop_reported_ = false;
  bool runtime_started_ = false;
  bool fatal_ = false;
  ExecutorState state_ = ExecutorState::kNotStarted;
  std::string last_start_diagnostic_;

  std::size_t capacity_ = 0;
  std::size_t occupied_ = 0;   // queued + executing + completed-but-unconsumed
  std::size_t executing_ = 0;  // 0 or 1 with one worker, kept explicit for diagnostics

  std::deque<EngineCommand> queue_;
  std::unordered_map<std::uint64_t, ClientLifecycle> clients_;
  std::unordered_map<std::uint64_t, OutstandingMeta> outstanding_;
  std::unordered_map<std::uint64_t, CommandResult> results_;
  HANDLE result_event_ = nullptr;

  std::uint64_t next_request_ = 1;
  std::uint64_t last_submitted_ = 0;
  std::uint64_t next_client_ = 1;
  std::uint64_t clients_created_ = 0;
  std::uint64_t close_requests_ = 0;
  std::uint64_t close_results_taken_ = 0;
  std::uint64_t close_admission_refusals_ = 0;

  ExecutorRuntimeReport report_;
};

StartStatus EngineExecutor::Impl::Start() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (start_attempted_) {
    return StartStatus::kAlreadyStarted;
  }
  start_attempted_ = true;

  if (capacity_ == 0) {
    state_ = ExecutorState::kFailed;
    last_start_diagnostic_ = "capacity must be greater than zero";
    report_.state = state_;
    report_.diagnostic = last_start_diagnostic_;
    lifecycle_cv_.notify_all();
    return StartStatus::kInvalidOptions;
  }

  state_ = ExecutorState::kStarting;
  report_.state = state_;
  lock.unlock();

  try {
    std::lock_guard<std::mutex> join_guard(join_mutex_);
    worker_ = std::thread([this] { WorkerMain(); });
  } catch (...) {
    lock.lock();
    state_ = ExecutorState::kFailed;
    last_start_diagnostic_ = "std::thread construction failed";
    report_.state = state_;
    report_.diagnostic = last_start_diagnostic_;
    lifecycle_cv_.notify_all();
    return StartStatus::kThreadStartFailed;
  }

  lock.lock();
  lifecycle_cv_.wait(lock, [this] { return state_ != ExecutorState::kStarting; });
  const ExecutorState observed = state_;
  lock.unlock();

  if (observed == ExecutorState::kRunning) {
    return StartStatus::kOk;
  }
  if (observed == ExecutorState::kStopped) {
    JoinWorker();
    return StartStatus::kStoppedDuringStart;
  }
  if (observed == ExecutorState::kStopping) {
    // The StopAndJoin caller owns the join. Joining here as well would race that thread.
    return StartStatus::kStoppedDuringStart;
  }
  JoinWorker();
  return StartStatus::kRuntimeStartFailed;
}

StopStatus EngineExecutor::Impl::StopAndJoin() {
  if (worker_thread_id_.load() != 0 &&
      worker_thread_id_.load() == ::GetCurrentThreadId()) {
    return StopStatus::kSelfJoinRefused;
  }

  std::lock_guard<std::mutex> stop_guard(stop_mutex_);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!start_attempted_) {
      return StopStatus::kNotStarted;
    }
    if (state_ == ExecutorState::kStopped) {
      if (stop_reported_) {
        return StopStatus::kAlreadyStopped;
      }
      // The worker reached Stopped without this method reporting it (for example Stop won a
      // race with Start during Starting). Join below, then report the one clean stop.
    } else if (state_ == ExecutorState::kFailed) {
      if (stop_reported_) {
        return StopStatus::kAlreadyStopped;
      }
      stop_reported_ = true;
      // The worker exits on its own after a start failure; joining below is still required.
    } else if (state_ != ExecutorState::kStopping) {
      stop_requested_ = true;
      state_ = ExecutorState::kStopping;
      report_.state = state_;
      work_cv_.notify_all();
    }
  }

  JoinWorker();

  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == ExecutorState::kStopped) {
    stop_reported_ = true;
    return StopStatus::kOk;
  }
  if (state_ == ExecutorState::kFailed) {
    stop_reported_ = true;
    return runtime_started_ ? StopStatus::kCleanupFailed : StopStatus::kExecutorFailed;
  }
  // The worker exited without publishing a terminal state: treat it as a cleanup failure
  // rather than reporting a clean stop.
  state_ = ExecutorState::kFailed;
  report_.state = state_;
  stop_reported_ = true;
  return StopStatus::kCleanupFailed;
}

void EngineExecutor::Impl::JoinWorker() {
  std::lock_guard<std::mutex> guard(join_mutex_);
  if (worker_.joinable()) {
    worker_.join();
  }
}

void EngineExecutor::Impl::WorkerMain() {
  const std::uint32_t thread_id = ::GetCurrentThreadId();
  worker_thread_id_.store(thread_id);

  BackendStartResult start;
  try {
    start = backend_->StartRuntime(options_.runtime);
  } catch (...) {
    start.status = RuntimeStartStatus::kInitializeFailed;
    start.diagnostic = "backend StartRuntime threw";
  }
  start.thread_id = thread_id;
  const bool started = start.status == RuntimeStartStatus::kOk;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    runtime_started_ = started;
    if (started && !stop_requested_) {
      state_ = ExecutorState::kRunning;
    } else {
      // Do not resurrect kRunning after StopAndJoin already set kStopping. Admission must
      // stay closed from the instant Stop won the race, even if Runtime::Start finishes
      // afterwards.
      state_ = started ? ExecutorState::kStopping : ExecutorState::kFailed;
    }
    report_.state = state_;
    report_.runtime_start_status = start.status;
    report_.start_thread_id = thread_id;
    report_.runtime_info = start.info;
    AppendDiagnostic(&report_.diagnostic, start.diagnostic);
    if (!started) {
      last_start_diagnostic_ = start.diagnostic;
    }
    lifecycle_cv_.notify_all();
  }

  if (!started) {
    FinishStop(false);
    return;
  }

  for (;;) {
    EngineCommand command;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      work_cv_.wait(lock, [this] { return !queue_.empty() || stop_requested_; });
      if (queue_.empty()) {
        if (stop_requested_) {
          break;
        }
        continue;
      }
      command = std::move(queue_.front());
      queue_.pop_front();
      ++executing_;
    }

    CommandResult result;
    try {
      result = Execute(command);
    } catch (...) {
      // Defensive: Execute handles its own exceptions. If anything still escapes, the
      // accepted request gets a terminal fault instead of disappearing.
      result = FaultResult(command, CommandStatus::kInternalFault,
                           "command threw an exception");
      if (command.kind == CommandKind::kCreateClient ||
          command.kind == CommandKind::kCloseClient) {
        SetClientStatus(command.client, ClientLifecycle::kFailed);
      }
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      --executing_;
      outstanding_.erase(command.request);
      try {
        auto inserted = results_.emplace(command.request, std::move(result));
        if (!inserted.second) {
          // Request ids are unique and results are never overwritten. Reaching this means an
          // internal invariant broke; the executor must not keep accepting work.
          fatal_ = true;
        }
      } catch (...) {
        fatal_ = true;
      }
      if (result_event_ != nullptr) {
        ::SetEvent(result_event_);
      }
      result_cv_.notify_all();
    }
  }

  FinishStop(true);
}

void EngineExecutor::Impl::FinishStop(bool runtime_started) {
  BackendCleanupReport cleanup;
  try {
    cleanup = backend_->CleanupAllClients();
  } catch (...) {
    cleanup.shutdown_status = RuntimeShutdownStatus::kFinalizeFailed;
    cleanup.shutdown_thread_id = ::GetCurrentThreadId();
    cleanup.diagnostic = "backend CleanupAllClients threw";
  }
  if (cleanup.shutdown_thread_id == 0) {
    cleanup.shutdown_thread_id = ::GetCurrentThreadId();
  }

  RuntimeInfo info;
  ApiCallCounts counts;
  try {
    info = backend_->RuntimeInfoCopy();
  } catch (...) {
    AppendDiagnostic(&cleanup.diagnostic, "RuntimeInfoCopy threw");
  }
  try {
    counts = backend_->ApiCountsCopy();
  } catch (...) {
    AppendDiagnostic(&cleanup.diagnostic, "ApiCountsCopy threw");
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& entry : clients_) {
      entry.second = ClientLifecycle::kClosed;
    }

    report_.runtime_shutdown_status = cleanup.shutdown_status;
    report_.shutdown_thread_id = cleanup.shutdown_thread_id;
    report_.runtime_info = info;
    report_.api_counts = counts;
    report_.clients_created = clients_created_;
    report_.clients_closed = cleanup.clients_closed;
    report_.client_close_failures = cleanup.close_failures;
    report_.live_sessions_after_cleanup = cleanup.live_sessions_after;
    AppendDiagnostic(&report_.diagnostic, cleanup.diagnostic);

    const bool clean = runtime_started && !fatal_ &&
                       cleanup.shutdown_status == RuntimeShutdownStatus::kOk &&
                       cleanup.live_sessions_after == 0;
    state_ = clean ? ExecutorState::kStopped : ExecutorState::kFailed;
    report_.state = state_;

    // Normal operation drained every accepted command. This is a last-resort termination for
    // an outstanding request whose result could not be published; it never overwrites a
    // result that already exists.
    for (const auto& entry : outstanding_) {
      if (results_.find(entry.first) != results_.end()) {
        continue;
      }
      EngineCommand command;
      command.request = entry.first;
      command.kind = entry.second.kind;
      command.client = entry.second.client;
      CommandResult failure = FaultResult(
          command, CommandStatus::kExecutorFailed,
          "executor stopped before this accepted request could publish a result");
      try {
        results_.emplace(entry.first, std::move(failure));
      } catch (...) {
        // Allocation failure leaves no way to publish a terminal value. The executor state
        // and the report record the failure; the process is out of memory.
      }
    }
    outstanding_.clear();

    if (result_event_ != nullptr) {
      ::SetEvent(result_event_);
    }
    lifecycle_cv_.notify_all();
    result_cv_.notify_all();
  }
}

ExecutorState EngineExecutor::Impl::State() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

std::uint32_t EngineExecutor::Impl::WorkerThreadId() const {
  return worker_thread_id_.load();
}

SubmitStatus EngineExecutor::Impl::Submit(EngineCommand command, bool create,
                                          RequestId* request, ClientId* client) {
  if (request == nullptr) {
    return SubmitStatus::kInvalidRequest;
  }
  if (create) {
    if (client == nullptr || command.schema_id.empty()) {
      return SubmitStatus::kInvalidRequest;
    }
  } else if (command.client == 0) {
    return SubmitStatus::kInvalidRequest;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ != ExecutorState::kRunning) {
    if (command.kind == CommandKind::kCloseClient) {
      ++close_admission_refusals_;
    }
    return SubmitStatus::kNotRunning;
  }

  ClientLifecycle previous_client_state = ClientLifecycle::kPending;
  if (!create) {
    const auto found = clients_.find(command.client);
    if (found == clients_.end()) {
      if (command.kind == CommandKind::kCloseClient) {
        ++close_admission_refusals_;
      }
      return SubmitStatus::kClientUnknown;
    }
    if (found->second != ClientLifecycle::kPending &&
        found->second != ClientLifecycle::kOpen) {
      if (command.kind == CommandKind::kCloseClient) {
        ++close_admission_refusals_;
      }
      return SubmitStatus::kClientNotOpen;
    }
    previous_client_state = found->second;
  }

  // A closed client is always reported as such, even when the queue is also full: the close
  // admission rule is more specific than capacity pressure.
  if (occupied_ >= capacity_) {
    if (command.kind == CommandKind::kCloseClient) {
      ++close_admission_refusals_;
    }
    return SubmitStatus::kQueueFull;
  }

  const std::uint64_t request_id = next_request_;
  const std::uint64_t client_id = create ? next_client_ : command.client;
  const CommandKind kind = command.kind;
  command.request = request_id;
  command.client = client_id;

  try {
    queue_.push_back(std::move(command));
  } catch (...) {
    return SubmitStatus::kInternalFailure;
  }

  try {
    if (create) {
      clients_.emplace(client_id, ClientLifecycle::kPending);
    } else if (kind == CommandKind::kCloseClient) {
      clients_[client_id] = ClientLifecycle::kClosing;
    }
    outstanding_.emplace(request_id, OutstandingMeta{kind, client_id});
  } catch (...) {
    // The command is still at the back of the queue and no worker can observe it while this
    // lock is held. Roll back every mutation made for it.
    if (!queue_.empty() && queue_.back().request == request_id) {
      queue_.pop_back();
    }
    if (create) {
      clients_.erase(client_id);
    } else if (kind == CommandKind::kCloseClient) {
      const auto found = clients_.find(client_id);
      if (found != clients_.end()) {
        found->second = previous_client_state;
      }
    }
    outstanding_.erase(request_id);
    if (kind == CommandKind::kCloseClient) {
      ++close_admission_refusals_;
    }
    return SubmitStatus::kInternalFailure;
  }

  ++next_request_;
  last_submitted_ = request_id;
  if (kind == CommandKind::kCloseClient) {
    ++close_requests_;
  }
  if (create) {
    ++next_client_;
    *client = ClientId{client_id};
  }
  ++occupied_;
  work_cv_.notify_one();
  *request = RequestId{request_id};
  return SubmitStatus::kAccepted;
}

WaitStatus EngineExecutor::Impl::Wait(RequestId request,
                                      std::chrono::milliseconds timeout) {
  if (!request.valid()) {
    return WaitStatus::kUnknownRequest;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  if (request.value > last_submitted_) {
    return WaitStatus::kUnknownRequest;
  }
  if (results_.find(request.value) != results_.end()) {
    return WaitStatus::kReady;
  }
  if (outstanding_.find(request.value) == outstanding_.end()) {
    return WaitStatus::kAlreadyTaken;
  }
  if (timeout.count() <= 0) {
    return WaitStatus::kTimedOut;
  }

  result_cv_.wait_for(lock, timeout, [this, &request] {
    return results_.find(request.value) != results_.end() ||
           outstanding_.find(request.value) == outstanding_.end();
  });

  if (results_.find(request.value) != results_.end()) {
    return WaitStatus::kReady;
  }
  if (outstanding_.find(request.value) == outstanding_.end()) {
    return WaitStatus::kAlreadyTaken;
  }
  return WaitStatus::kTimedOut;
}

TakeStatus EngineExecutor::Impl::Take(RequestId request, CommandResult* result) {
  if (result == nullptr) {
    return TakeStatus::kInvalidRequest;
  }
  if (!request.valid()) {
    return TakeStatus::kUnknownRequest;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (request.value > last_submitted_) {
    return TakeStatus::kUnknownRequest;
  }
  const auto found = results_.find(request.value);
  if (found == results_.end()) {
    return outstanding_.find(request.value) != outstanding_.end()
               ? TakeStatus::kNotReady
               : TakeStatus::kAlreadyTaken;
  }
  if (found->second.kind == CommandKind::kCloseClient) {
    ++close_results_taken_;
  }
  *result = std::move(found->second);
  results_.erase(found);
  if (occupied_ > 0) {
    --occupied_;
  }
  return TakeStatus::kTaken;
}

CapacityUsage EngineExecutor::Impl::Capacity() const {
  std::lock_guard<std::mutex> lock(mutex_);
  CapacityUsage usage;
  usage.capacity = capacity_;
  usage.occupied = occupied_;
  usage.queued = queue_.size();
  usage.executing = executing_;
  usage.completed_unconsumed = results_.size();
  return usage;
}

ClientUsage EngineExecutor::Impl::ClientCounts() const {
  std::lock_guard<std::mutex> lock(mutex_);
  ClientUsage usage;
  usage.created = static_cast<std::size_t>(clients_created_);
  for (const auto& entry : clients_) {
    switch (entry.second) {
      case ClientLifecycle::kPending:
        ++usage.pending;
        break;
      case ClientLifecycle::kOpen:
        ++usage.open;
        break;
      case ClientLifecycle::kClosing:
        ++usage.closing;
        break;
      case ClientLifecycle::kClosed:
        ++usage.closed;
        break;
      case ClientLifecycle::kFailed:
        ++usage.failed;
        break;
    }
  }
  usage.live = usage.pending + usage.open + usage.closing;
  usage.close_requests = close_requests_;
  usage.close_results_taken = close_results_taken_;
  usage.close_admission_refusals = close_admission_refusals_;
  return usage;
}

ExecutorRuntimeReport EngineExecutor::Impl::Report() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return report_;
}

CommandResult EngineExecutor::Impl::Execute(const EngineCommand& command) {
  CommandResult result;
  result.request = RequestId{command.request};
  result.kind = command.kind;
  result.client = ClientId{command.client};
  result.executing_thread_id = ::GetCurrentThreadId();

  try {
    switch (command.kind) {
      case CommandKind::kCreateClient: {
        const BackendCreateResult created = backend_->CreateClient(
            command.client, command.schema_id, command.controller_options);
        CreateClientOutcome outcome;
        outcome.client = ClientId{command.client};
        outcome.session_status = created.status;
        outcome.diagnostic = created.diagnostic;
        result.payload = outcome;
        result.diagnostic = created.diagnostic;
        if (created.ok()) {
          result.status = CommandStatus::kOk;
          NoteClientCreated();
          SetClientStatus(command.client, ClientLifecycle::kOpen);
        } else {
          result.status = CommandStatus::kSessionCreateFailed;
          SetClientStatus(command.client, ClientLifecycle::kFailed);
        }
        return result;
      }
      case CommandKind::kEvaluate: {
        const BackendEvaluateResult call =
            backend_->Evaluate(command.client, command.event, command.text);
        EvaluateOutcome outcome;
        outcome.evaluation = call.evaluation;
        result.payload = outcome;
        result.diagnostic = call.evaluation.diagnostic;
        if (!call.client_found) {
          result.status = CommandStatus::kClientUnavailable;
          SetClientStatus(command.client, ClientLifecycle::kFailed);
          return result;
        }
        switch (call.evaluation.status) {
          case ControllerStatus::kOk:
            result.status = CommandStatus::kOk;
            break;
          case ControllerStatus::kNoSession:
            result.status = CommandStatus::kClientUnavailable;
            SetClientStatus(command.client, ClientLifecycle::kFailed);
            break;
          case ControllerStatus::kWrongThread:
            result.status = CommandStatus::kInternalFault;
            break;
          case ControllerStatus::kStateUnavailable:
          case ControllerStatus::kEngineFaulted:
            result.status = CommandStatus::kControllerFailed;
            break;
        }
        return result;
      }
      case CommandKind::kDispatch: {
        const BackendDispatchResult call =
            backend_->Dispatch(command.client, command.event, command.text);
        DispatchCommandOutcome outcome;
        outcome.outcome = call.outcome;
        result.payload = outcome;
        result.diagnostic = call.outcome.diagnostic;
        if (!call.client_found) {
          result.status = CommandStatus::kClientUnavailable;
          SetClientStatus(command.client, ClientLifecycle::kFailed);
          return result;
        }
        result.status = call.outcome.status == DispatchStatus::kDispatchedStateFailed
                            ? CommandStatus::kControllerFailed
                            : CommandStatus::kOk;
        return result;
      }
      case CommandKind::kAcceptCandidate: {
        const BackendCandidateResult call =
            backend_->AcceptCandidate(command.client, command.candidate_index);
        CandidateCommandOutcome outcome;
        outcome.outcome = call.outcome;
        result.payload = outcome;
        result.diagnostic = call.outcome.diagnostic;
        if (!call.client_found) {
          result.status = CommandStatus::kClientUnavailable;
          SetClientStatus(command.client, ClientLifecycle::kFailed);
          return result;
        }
        switch (call.outcome.status) {
          case CandidateStatus::kAccepted:
          case CandidateStatus::kRejectedByEngine:
          case CandidateStatus::kOutOfRange:
            result.status = CommandStatus::kOk;
            break;
          case CandidateStatus::kNotReady:
            result.status = CommandStatus::kControllerFailed;
            break;
          case CandidateStatus::kWrongThread:
            result.status = CommandStatus::kInternalFault;
            break;
          case CandidateStatus::kStateFailed:
            result.status = CommandStatus::kControllerFailed;
            break;
        }
        return result;
      }
      case CommandKind::kRefreshState: {
        const BackendRefreshResult call = backend_->Refresh(command.client);
        RefreshOutcome outcome;
        outcome.controller_status = call.status;
        if (call.status == ControllerStatus::kOk) {
          outcome.state = call.state;
        }
        result.payload = outcome;
        if (!call.client_found) {
          result.status = CommandStatus::kClientUnavailable;
          SetClientStatus(command.client, ClientLifecycle::kFailed);
          return result;
        }
        switch (call.status) {
          case ControllerStatus::kOk:
            result.status = CommandStatus::kOk;
            break;
          case ControllerStatus::kNoSession:
            result.status = CommandStatus::kClientUnavailable;
            SetClientStatus(command.client, ClientLifecycle::kFailed);
            break;
          case ControllerStatus::kWrongThread:
            result.status = CommandStatus::kInternalFault;
            break;
          case ControllerStatus::kStateUnavailable:
          case ControllerStatus::kEngineFaulted:
            result.status = CommandStatus::kControllerFailed;
            break;
        }
        return result;
      }
      case CommandKind::kCloseClient: {
        const BackendCloseResult call = backend_->CloseClient(command.client);
        CloseClientOutcome outcome;
        outcome.session_status = call.status;
        result.payload = outcome;
        result.diagnostic = call.diagnostic;
        if (!call.client_found) {
          result.status = CommandStatus::kClientUnavailable;
          SetClientStatus(command.client, ClientLifecycle::kClosed);
          return result;
        }
        if (call.status == SessionCloseStatus::kOk ||
            call.status == SessionCloseStatus::kAlreadyClosed) {
          result.status = CommandStatus::kOk;
          SetClientStatus(command.client, ClientLifecycle::kClosed);
        } else {
          result.status = CommandStatus::kSessionCloseFailed;
          SetClientStatus(command.client, ClientLifecycle::kFailed);
        }
        return result;
      }
    }
  } catch (...) {
    result.status = CommandStatus::kInternalFault;
    result.payload = DefaultPayload(command);
    result.diagnostic = "command threw an exception";
    if (command.kind == CommandKind::kCreateClient ||
        command.kind == CommandKind::kCloseClient) {
      SetClientStatus(command.client, ClientLifecycle::kFailed);
    }
    return result;
  }

  result.status = CommandStatus::kInvalidRequest;
  result.payload = DefaultPayload(command);
  return result;
}

void EngineExecutor::Impl::SetClientStatus(std::uint64_t client,
                                           ClientLifecycle lifecycle) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = clients_.find(client);
  if (found != clients_.end()) {
    found->second = lifecycle;
  }
}

void EngineExecutor::Impl::NoteClientCreated() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++clients_created_;
}

EngineExecutor::EngineExecutor(ExecutorOptions options)
    : EngineExecutor(std::move(options), MakeRealEngineBackend()) {}

EngineExecutor::EngineExecutor(ExecutorOptions options,
                               std::unique_ptr<EngineBackend> backend)
    : impl_(new Impl(std::move(options), std::move(backend))) {}

EngineExecutor::~EngineExecutor() {
  if (impl_) {
    (void)impl_->StopAndJoin();
  }
}

StartStatus EngineExecutor::Start() { return impl_->Start(); }
StopStatus EngineExecutor::StopAndJoin() { return impl_->StopAndJoin(); }
ExecutorState EngineExecutor::state() const { return impl_->State(); }
std::uint32_t EngineExecutor::worker_thread_id() const {
  return impl_->WorkerThreadId();
}

SubmitStatus EngineExecutor::TryCreateClient(const std::string& schema_id,
                                             const ControllerOptions& options,
                                             RequestId* request, ClientId* client) {
  EngineCommand command;
  command.kind = CommandKind::kCreateClient;
  command.schema_id = schema_id;
  command.controller_options = options;
  return impl_->Submit(std::move(command), true, request, client);
}

SubmitStatus EngineExecutor::TryEvaluate(ClientId client,
                                         const input::KeyEventSnapshot& event,
                                         const input::KeyText& text,
                                         RequestId* request) {
  EngineCommand command;
  command.kind = CommandKind::kEvaluate;
  command.client = client.value;
  command.event = event;
  command.text = text;
  return impl_->Submit(std::move(command), false, request, nullptr);
}

SubmitStatus EngineExecutor::TryDispatch(ClientId client,
                                         const input::KeyEventSnapshot& event,
                                         const input::KeyText& text,
                                         RequestId* request) {
  EngineCommand command;
  command.kind = CommandKind::kDispatch;
  command.client = client.value;
  command.event = event;
  command.text = text;
  return impl_->Submit(std::move(command), false, request, nullptr);
}

SubmitStatus EngineExecutor::TryAcceptCandidate(ClientId client,
                                                std::size_t candidate_index,
                                                RequestId* request) {
  EngineCommand command;
  command.kind = CommandKind::kAcceptCandidate;
  command.client = client.value;
  command.candidate_index = candidate_index;
  return impl_->Submit(std::move(command), false, request, nullptr);
}

SubmitStatus EngineExecutor::TryRefreshState(ClientId client, RequestId* request) {
  EngineCommand command;
  command.kind = CommandKind::kRefreshState;
  command.client = client.value;
  return impl_->Submit(std::move(command), false, request, nullptr);
}

SubmitStatus EngineExecutor::TryCloseClient(ClientId client, RequestId* request) {
  EngineCommand command;
  command.kind = CommandKind::kCloseClient;
  command.client = client.value;
  return impl_->Submit(std::move(command), false, request, nullptr);
}

WaitStatus EngineExecutor::WaitForResult(RequestId request,
                                         std::chrono::milliseconds timeout) {
  return impl_->Wait(request, timeout);
}

TakeStatus EngineExecutor::TakeResult(RequestId request, CommandResult* result) {
  return impl_->Take(request, result);
}

HANDLE EngineExecutor::ResultReadyEvent() const { return impl_->result_event_; }

CapacityUsage EngineExecutor::capacity() const { return impl_->Capacity(); }
ClientUsage EngineExecutor::clients() const { return impl_->ClientCounts(); }
ExecutorRuntimeReport EngineExecutor::RuntimeReport() const { return impl_->Report(); }

std::unique_ptr<EngineExecutor> EngineExecutorTestFactory::Create(
    ExecutorOptions options, std::unique_ptr<EngineBackend> backend) {
  return std::unique_ptr<EngineExecutor>(
      new EngineExecutor(std::move(options), std::move(backend)));
}

const char* DescribeExecutorState(ExecutorState state) {
  switch (state) {
    case ExecutorState::kNotStarted:
      return "not-started";
    case ExecutorState::kStarting:
      return "starting";
    case ExecutorState::kRunning:
      return "running";
    case ExecutorState::kStopping:
      return "stopping";
    case ExecutorState::kStopped:
      return "stopped";
    case ExecutorState::kFailed:
      return "failed";
  }
  return "unknown-state";
}

const char* DescribeStartStatus(StartStatus status) {
  switch (status) {
    case StartStatus::kOk:
      return "ok";
    case StartStatus::kAlreadyStarted:
      return "already-started";
    case StartStatus::kInvalidOptions:
      return "invalid-options";
    case StartStatus::kThreadStartFailed:
      return "thread-start-failed";
    case StartStatus::kRuntimeStartFailed:
      return "runtime-start-failed";
    case StartStatus::kStoppedDuringStart:
      return "stopped-during-start";
  }
  return "unknown-status";
}

const char* DescribeStopStatus(StopStatus status) {
  switch (status) {
    case StopStatus::kOk:
      return "ok";
    case StopStatus::kNotStarted:
      return "not-started";
    case StopStatus::kAlreadyStopped:
      return "already-stopped";
    case StopStatus::kSelfJoinRefused:
      return "self-join-refused";
    case StopStatus::kExecutorFailed:
      return "executor-failed";
    case StopStatus::kCleanupFailed:
      return "cleanup-failed";
  }
  return "unknown-status";
}

const char* DescribeSubmitStatus(SubmitStatus status) {
  switch (status) {
    case SubmitStatus::kAccepted:
      return "accepted";
    case SubmitStatus::kNotRunning:
      return "not-running";
    case SubmitStatus::kQueueFull:
      return "queue-full";
    case SubmitStatus::kClientUnknown:
      return "client-unknown";
    case SubmitStatus::kClientNotOpen:
      return "client-not-open";
    case SubmitStatus::kInvalidRequest:
      return "invalid-request";
    case SubmitStatus::kInternalFailure:
      return "internal-failure";
  }
  return "unknown-status";
}

const char* DescribeCommandKind(CommandKind kind) {
  switch (kind) {
    case CommandKind::kCreateClient:
      return "create-client";
    case CommandKind::kEvaluate:
      return "evaluate";
    case CommandKind::kDispatch:
      return "dispatch";
    case CommandKind::kAcceptCandidate:
      return "accept-candidate";
    case CommandKind::kRefreshState:
      return "refresh-state";
    case CommandKind::kCloseClient:
      return "close-client";
  }
  return "unknown-command";
}

const char* DescribeCommandStatus(CommandStatus status) {
  switch (status) {
    case CommandStatus::kOk:
      return "ok";
    case CommandStatus::kClientUnavailable:
      return "client-unavailable";
    case CommandStatus::kSessionCreateFailed:
      return "session-create-failed";
    case CommandStatus::kSessionCloseFailed:
      return "session-close-failed";
    case CommandStatus::kControllerFailed:
      return "controller-failed";
    case CommandStatus::kInternalFault:
      return "internal-fault";
    case CommandStatus::kExecutorFailed:
      return "executor-failed";
    case CommandStatus::kInvalidRequest:
      return "invalid-request";
  }
  return "unknown-status";
}

const char* DescribeWaitStatus(WaitStatus status) {
  switch (status) {
    case WaitStatus::kReady:
      return "ready";
    case WaitStatus::kTimedOut:
      return "timed-out";
    case WaitStatus::kUnknownRequest:
      return "unknown-request";
    case WaitStatus::kAlreadyTaken:
      return "already-taken";
  }
  return "unknown-status";
}

const char* DescribeTakeStatus(TakeStatus status) {
  switch (status) {
    case TakeStatus::kTaken:
      return "taken";
    case TakeStatus::kNotReady:
      return "not-ready";
    case TakeStatus::kUnknownRequest:
      return "unknown-request";
    case TakeStatus::kAlreadyTaken:
      return "already-taken";
    case TakeStatus::kInvalidRequest:
      return "invalid-request";
  }
  return "unknown-status";
}

}  // namespace engine
}  // namespace windows
}  // namespace epocanva
