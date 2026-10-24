// EpoCanva P3.5 -- scheduling-layer verification with a controlled backend.
//
// These cases judge the executor itself: state machine, admission, FIFO, client tokens, close
// rule, capacity accounting, timeout behavior, result consumption and stop draining. They do
// NOT load rime.dll and do NOT claim any English input result. The real-engine suite is a
// separate mode and a separate CTest case.
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "controlled_backend.h"
#include "engine_executor_internal.h"
#include "test_support.h"

namespace epocanva {
namespace tests {
namespace engine_executor {

using epocanva::windows::engine::ClientId;
using epocanva::windows::engine::CommandResult;
using epocanva::windows::engine::CommandStatus;
using epocanva::windows::engine::ControllerOptions;
using epocanva::windows::engine::CreateClientOutcome;
using epocanva::windows::engine::DescribeCommandStatus;
using epocanva::windows::engine::DescribeExecutorState;
using epocanva::windows::engine::DescribeStartStatus;
using epocanva::windows::engine::DescribeStopStatus;
using epocanva::windows::engine::DescribeSubmitStatus;
using epocanva::windows::engine::DescribeWaitStatus;
using epocanva::windows::engine::DispatchCommandOutcome;
using epocanva::windows::engine::DispatchStatus;
using epocanva::windows::engine::EngineExecutor;
using epocanva::windows::engine::EngineExecutorTestFactory;
using epocanva::windows::engine::EvaluateOutcome;
using epocanva::windows::engine::ExecutorOptions;
using epocanva::windows::engine::ExecutorRuntimeReport;
using epocanva::windows::engine::ExecutorState;
using epocanva::windows::engine::RefreshOutcome;
using epocanva::windows::engine::RequestId;
using epocanva::windows::engine::RuntimeShutdownStatus;
using epocanva::windows::engine::RuntimeStartStatus;
using epocanva::windows::engine::SessionCreateStatus;
using epocanva::windows::engine::StartStatus;
using epocanva::windows::engine::StopStatus;
using epocanva::windows::engine::SubmitStatus;
using epocanva::windows::engine::TakeStatus;
using epocanva::windows::engine::WaitStatus;
using epocanva::windows::input::InputPermission;
using epocanva::windows::input::KeyDecision;
using epocanva::windows::input::KeyEventSnapshot;
using epocanva::windows::input::KeyText;
using epocanva::windows::input::KeyTextStatus;

namespace {

ExecutorOptions OptionsWithCapacity(std::size_t capacity) {
  ExecutorOptions options;
  options.capacity = capacity;
  return options;
}

std::unique_ptr<EngineExecutor> MakeExecutor(ControlledBackend::Configuration config,
                                             std::size_t capacity,
                                             ControlledBackend** backend_out) {
  auto backend = std::unique_ptr<ControlledBackend>(new ControlledBackend(config));
  *backend_out = backend.get();
  return EngineExecutorTestFactory::Create(OptionsWithCapacity(capacity),
                                          std::move(backend));
}

struct Letter {
  KeyEventSnapshot event;
  KeyText text;
};

Letter LetterKey(char character) {
  Letter letter;
  letter.event = MakeKeyEvent(static_cast<std::uint32_t>(character), false, false,
                              false, false);
  letter.text.status = KeyTextStatus::kText;
  letter.text.utf16_code_units = std::u16string(1, static_cast<char16_t>(character));
  return letter;
}

ControllerOptions Allowed() {
  return ControllerOptions{true, InputPermission::kAllowed};
}

const CreateClientOutcome* CreatePayload(const CommandResult& result) {
  return std::get_if<CreateClientOutcome>(&result.payload);
}

const DispatchCommandOutcome* DispatchPayload(const CommandResult& result) {
  return std::get_if<DispatchCommandOutcome>(&result.payload);
}

const RefreshOutcome* RefreshPayload(const CommandResult& result) {
  return std::get_if<RefreshOutcome>(&result.payload);
}

std::string DispatchRaw(const CommandResult& result) {
  const DispatchCommandOutcome* payload = DispatchPayload(result);
  return payload == nullptr ? std::string("missing-payload")
                            : AsciiOf(payload->outcome.state.raw_input.utf16_code_units);
}

std::string RefreshRaw(const CommandResult& result) {
  const RefreshOutcome* payload = RefreshPayload(result);
  return payload == nullptr ? std::string("missing-payload")
                            : AsciiOf(payload->state.raw_input.utf16_code_units);
}

bool WaitForCapacity(EngineExecutor& executor, std::size_t executing,
                     std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (executor.capacity().executing == executing) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

bool WaitForState(EngineExecutor& executor, ExecutorState state,
                  std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (executor.state() == state) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

class TestBarrier {
 public:
  explicit TestBarrier(int participants) : participants_(participants) {}

  void Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++arrived_;
    if (arrived_ == participants_) {
      ++generation_;
      condition_.notify_all();
      return;
    }
    const unsigned generation = generation_;
    condition_.wait(lock, [this, generation] { return generation_ != generation; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  int participants_ = 0;
  int arrived_ = 0;
  unsigned generation_ = 0;
};

struct ThreadRun {
  std::array<RequestId, 3> requests{};
  std::array<SubmitStatus, 3> submits{};
  std::array<WaitStatus, 3> waits{};
  std::array<TakeStatus, 3> takes{};
  std::array<CommandResult, 3> results{};
};

void RunLifecycleAndResultCases() {
  ControlledBackend* backend = nullptr;
  auto executor = MakeExecutor({}, 8, &backend);
  {
    const ExecutorState initial = executor->state();
    Check(initial == ExecutorState::kNotStarted,
          "lifecycle: an unstarted executor is not-started",
          DescribeExecutorState(initial));
    const StopStatus early_stop = executor->StopAndJoin();
    Check(early_stop == StopStatus::kNotStarted,
          "lifecycle: Stop before Start reports not-started",
          DescribeStopStatus(early_stop));
    RequestId request;
    ClientId client;
    const SubmitStatus before_start =
        executor->TryCreateClient("controlled", Allowed(), &request, &client);
    Check(before_start == SubmitStatus::kNotRunning,
          "lifecycle: submission before Start is refused",
          DescribeSubmitStatus(before_start));
  }

  const StartStatus start = executor->Start();
  Check(start == StartStatus::kOk, "lifecycle: Start succeeds",
        DescribeStartStatus(start));
  {
    const ExecutorState running = executor->state();
    Check(running == ExecutorState::kRunning, "lifecycle: state is running",
          DescribeExecutorState(running));
  }
  Check(executor->worker_thread_id() != 0, "lifecycle: worker thread id is set");
  Check(executor->worker_thread_id() != ::GetCurrentThreadId(),
        "lifecycle: the worker is not the caller thread");
  {
    const StartStatus second_start = executor->Start();
    Check(second_start == StartStatus::kAlreadyStarted,
          "lifecycle: a second Start is refused", DescribeStartStatus(second_start));
  }

  // --- create, three evaluations, one dispatch -----------------------------------------
  ClientId client;
  RequestId create_request =
      SubmitCreateOrFail(*executor, "controlled", Allowed(), &client, "lifecycle create");
  CommandResult created = TakeOrFail(*executor, create_request, 5000, "lifecycle create");
  Check(created.status == CommandStatus::kOk, "lifecycle: create result is ok",
        DescribeCommandStatus(created.status));
  const CreateClientOutcome* create_payload = CreatePayload(created);
  Check(create_payload != nullptr && create_payload->client == client,
        "lifecycle: create result carries the issued client id");
  if (create_payload != nullptr) {
    Check(create_payload->session_status == SessionCreateStatus::kOk,
          "lifecycle: controller reports a created session");
  }
  Check(created.executing_thread_id == executor->worker_thread_id(),
        "lifecycle: create executed on the worker");
  Check(executor->capacity().occupied == 0,
        "lifecycle: taking the create result released its slot",
        CountText(executor->capacity().occupied));

  const Letter h = LetterKey('h');
  RequestId evaluate_requests[3] = {
      SubmitEvaluateOrFail(*executor, client, h.event, h.text, "evaluate 1"),
      SubmitEvaluateOrFail(*executor, client, h.event, h.text, "evaluate 2"),
      SubmitEvaluateOrFail(*executor, client, h.event, h.text, "evaluate 3")};
  CommandResult eval_one =
      TakeOrFail(*executor, evaluate_requests[0], 5000, "evaluate 1");
  const EvaluateOutcome* eval_one_payload =
      std::get_if<EvaluateOutcome>(&eval_one.payload);
  Check(eval_one_payload != nullptr, "evaluate 1: payload alternative matches");
  KeyDecision first_decision;
  if (eval_one_payload != nullptr) {
    first_decision = eval_one_payload->evaluation.decision;
  }
  for (int i = 1; i < 3; ++i) {
    CommandResult result =
        TakeOrFail(*executor, evaluate_requests[i], 5000, "evaluate repeat");
    Check(result.status == CommandStatus::kOk, "evaluate repeat: result is ok",
          DescribeCommandStatus(result.status));
    const EvaluateOutcome* payload = std::get_if<EvaluateOutcome>(&result.payload);
    Check(payload != nullptr, "evaluate repeat: payload alternative matches");
    if (payload != nullptr && eval_one_payload != nullptr) {
      Check(payload->evaluation.decision == first_decision,
            "evaluate repeat: repeated evaluation returns the same decision");
    }
  }
  Check(backend->dispatch_calls() == 0,
        "evaluate: repeated evaluations made no dispatch",
        CountText(backend->dispatch_calls()));

  RequestId refresh_request =
      SubmitRefreshOrFail(*executor, client, "refresh before key");
  CommandResult refreshed =
      TakeOrFail(*executor, refresh_request, 5000, "refresh before key");
  Check(refreshed.status == CommandStatus::kOk, "evaluate: refresh is ok",
        DescribeCommandStatus(refreshed.status));
  Check(RefreshRaw(refreshed).empty(), "evaluate: three evaluations left raw input empty",
        RefreshRaw(refreshed));

  RequestId dispatch_request =
      SubmitDispatchOrFail(*executor, client, h.event, h.text, "dispatch h");
  CommandResult dispatched = TakeOrFail(*executor, dispatch_request, 5000, "dispatch h");
  Check(dispatched.status == CommandStatus::kOk, "dispatch h: result is ok",
        DescribeCommandStatus(dispatched.status));
  const DispatchCommandOutcome* dispatch_payload = DispatchPayload(dispatched);
  Check(dispatch_payload != nullptr, "dispatch h: payload alternative matches");
  if (dispatch_payload != nullptr) {
    Check(dispatch_payload->outcome.status == DispatchStatus::kDispatchedHandled,
          "dispatch h: exactly one handled dispatch");
    Check(DispatchRaw(dispatched) == "h", "dispatch h: controlled raw input is h",
          DispatchRaw(dispatched));
  }
  Check(backend->dispatch_calls() == 1,
        "dispatch: three evaluations plus one dispatch made exactly one dispatch",
        CountText(backend->dispatch_calls()));
  Check(executor->capacity().occupied == 0,
        "dispatch: all results were taken, so capacity is free",
        CountText(executor->capacity().occupied));

  // --- close admission and result preservation -----------------------------------------
  RequestId close_request;
  {
    const SubmitStatus submitted = executor->TryCloseClient(client, &close_request);
    Check(submitted == SubmitStatus::kAccepted, "close: close is accepted",
          DescribeSubmitStatus(submitted));
  }
  RequestId rejected_request;
  {
    const SubmitStatus submitted =
        executor->TryDispatch(client, h.event, h.text, &rejected_request);
    Check(submitted == SubmitStatus::kClientNotOpen,
          "close: a dispatch after an accepted close is refused",
          DescribeSubmitStatus(submitted));
  }
  CommandResult closed = TakeOrFail(*executor, close_request, 5000, "close");
  Check(closed.status == CommandStatus::kOk, "close: result is ok",
        DescribeCommandStatus(closed.status));
  {
    const SubmitStatus submitted =
        executor->TryRefreshState(client, &rejected_request);
    Check(submitted == SubmitStatus::kClientNotOpen,
          "close: refresh after close is refused", DescribeSubmitStatus(submitted));
  }
  {
    const SubmitStatus submitted = executor->TryCloseClient(client, &rejected_request);
    Check(submitted == SubmitStatus::kClientNotOpen,
          "close: closing twice is refused", DescribeSubmitStatus(submitted));
  }

  // --- results survive Stop ------------------------------------------------------------
  ClientId second_client;
  RequestId second_create = SubmitCreateOrFail(*executor, "controlled", Allowed(),
                                               &second_client, "post-stop create");
  (void)TakeOrFail(*executor, second_create, 5000, "post-stop create");
  const Letter a = LetterKey('a');
  const Letter p = LetterKey('p');
  RequestId left_requests[3] = {
      SubmitDispatchOrFail(*executor, second_client, a.event, a.text, "left 1"),
      SubmitDispatchOrFail(*executor, second_client, p.event, p.text, "left 2"),
      SubmitDispatchOrFail(*executor, second_client, p.event, p.text, "left 3")};
  {
    const StopStatus stopped = executor->StopAndJoin();
    Check(stopped == StopStatus::kOk, "stop: first Stop reports ok",
          DescribeStopStatus(stopped));
  }
  {
    const ExecutorState stopped_state = executor->state();
    Check(stopped_state == ExecutorState::kStopped, "stop: state is stopped",
          DescribeExecutorState(stopped_state));
  }
  {
    const StopStatus repeated = executor->StopAndJoin();
    Check(repeated == StopStatus::kAlreadyStopped,
          "stop: repeated Stop reports already-stopped", DescribeStopStatus(repeated));
  }
  {
    const StartStatus restart = executor->Start();
    Check(restart == StartStatus::kAlreadyStarted,
          "stop: a stopped executor does not restart", DescribeStartStatus(restart));
  }
  {
    const SubmitStatus submitted =
        executor->TryRefreshState(second_client, &rejected_request);
    Check(submitted == SubmitStatus::kNotRunning,
          "stop: submission after Stop is refused", DescribeSubmitStatus(submitted));
  }
  const std::string expected_left[3] = {"a", "ap", "app"};
  for (int i = 0; i < 3; ++i) {
    CommandResult left =
        TakeOrFail(*executor, left_requests[i], 5000, "left result after stop");
    Check(left.status == CommandStatus::kOk,
          "stop: an accepted result survives Stop and is still takeable",
          DescribeCommandStatus(left.status));
    Check(DispatchRaw(left) == expected_left[i],
          "stop: the surviving result preserves its own state", DispatchRaw(left));
  }
  Check(executor->capacity().occupied == 0,
        "stop: taking post-stop results released their slots",
        CountText(executor->capacity().occupied));

  const ExecutorRuntimeReport report = executor->RuntimeReport();
  Check(report.start_thread_id == executor->worker_thread_id(),
        "report: runtime Start happened on the worker");
  Check(report.shutdown_thread_id == executor->worker_thread_id(),
        "report: runtime Shutdown happened on the worker");
  Check(report.runtime_start_status == RuntimeStartStatus::kOk,
        "report: runtime start status is ok");
  Check(report.runtime_shutdown_status == RuntimeShutdownStatus::kOk,
        "report: runtime shutdown status is ok");
  Check(report.live_sessions_after_cleanup == 0,
        "report: no live sessions remain after cleanup",
        CountText(static_cast<std::uint64_t>(report.live_sessions_after_cleanup)));
  Check(report.api_counts.balanced(), "report: controlled API counters are balanced",
        DescribeCounts(report.api_counts));
  Check(report.clients_created == 2, "report: two clients were created",
        CountText(report.clients_created));
  Check(report.clients_closed == 1,
        "report: the remaining client was closed during cleanup",
        CountText(report.clients_closed));
}

void RunFifoAndIsolationCase() {
  ControlledBackend* backend = nullptr;
  auto executor = MakeExecutor({}, 32, &backend);
  {
    const StartStatus started = executor->Start();
    Check(started == StartStatus::kOk, "fifo: executor starts",
          DescribeStartStatus(started));
  }
  ClientId client_a;
  ClientId client_b;
  (void)TakeOrFail(*executor,
                   SubmitCreateOrFail(*executor, "controlled", Allowed(), &client_a,
                                      "fifo create A"),
                   5000, "fifo create A");
  (void)TakeOrFail(*executor,
                   SubmitCreateOrFail(*executor, "controlled", Allowed(), &client_b,
                                      "fifo create B"),
                   5000, "fifo create B");

  const Letter h = LetterKey('h');
  const Letter e = LetterKey('e');
  const Letter l = LetterKey('l');
  const Letter a = LetterKey('a');
  const Letter p = LetterKey('p');

  ThreadRun run_a;
  ThreadRun run_b;
  TestBarrier barrier(2);
  auto submit_sequence = [](EngineExecutor& target, ClientId client, TestBarrier* gate,
                            ThreadRun* run, const Letter* first, const Letter* second,
                            const Letter* third) {
    gate->Wait();
    const Letter* letters[3] = {first, second, third};
    for (int i = 0; i < 3; ++i) {
      run->submits[i] = target.TryDispatch(client, letters[i]->event, letters[i]->text,
                                           &run->requests[i]);
    }
    for (int i = 0; i < 3; ++i) {
      run->waits[i] = target.WaitForResult(run->requests[i], std::chrono::seconds(5));
      if (run->waits[i] == WaitStatus::kReady) {
        run->takes[i] = target.TakeResult(run->requests[i], &run->results[i]);
      }
    }
  };

  std::thread thread_a(submit_sequence, std::ref(*executor), client_a, &barrier, &run_a,
                       &h, &e, &l);
  std::thread thread_b(submit_sequence, std::ref(*executor), client_b, &barrier, &run_b,
                       &a, &p, &p);
  thread_a.join();
  thread_b.join();

  const std::string expected_a[3] = {"h", "he", "hel"};
  const std::string expected_b[3] = {"a", "ap", "app"};
  for (int i = 0; i < 3; ++i) {
    Check(run_a.submits[i] == SubmitStatus::kAccepted,
          "fifo A: every dispatch was accepted", DescribeSubmitStatus(run_a.submits[i]));
    Check(run_a.waits[i] == WaitStatus::kReady, "fifo A: result became ready",
          DescribeWaitStatus(run_a.waits[i]));
    Check(run_a.takes[i] == TakeStatus::kTaken, "fifo A: result was taken",
          "take-status");
    Check(run_a.results[i].status == CommandStatus::kOk, "fifo A: command is ok",
          DescribeCommandStatus(run_a.results[i].status));
    Check(DispatchRaw(run_a.results[i]) == expected_a[i],
          "fifo A: same-client sequence is stable", DispatchRaw(run_a.results[i]));

    Check(run_b.submits[i] == SubmitStatus::kAccepted,
          "fifo B: every dispatch was accepted", DescribeSubmitStatus(run_b.submits[i]));
    Check(run_b.waits[i] == WaitStatus::kReady, "fifo B: result became ready",
          DescribeWaitStatus(run_b.waits[i]));
    Check(run_b.takes[i] == TakeStatus::kTaken, "fifo B: result was taken",
          "take-status");
    Check(run_b.results[i].status == CommandStatus::kOk, "fifo B: command is ok",
          DescribeCommandStatus(run_b.results[i].status));
    Check(DispatchRaw(run_b.results[i]) == expected_b[i],
          "fifo B: same-client sequence is stable", DispatchRaw(run_b.results[i]));
  }

  CommandResult refresh_a =
      TakeOrFail(*executor, SubmitRefreshOrFail(*executor, client_a, "fifo refresh A"),
                 5000, "fifo refresh A");
  CommandResult refresh_b =
      TakeOrFail(*executor, SubmitRefreshOrFail(*executor, client_b, "fifo refresh B"),
                 5000, "fifo refresh B");
  Check(RefreshRaw(refresh_a) == "hel", "fifo: client A holds hel",
        RefreshRaw(refresh_a));
  Check(RefreshRaw(refresh_b) == "app", "fifo: client B holds app",
        RefreshRaw(refresh_b));
  Check(backend->client_count() == 2, "fifo: two independent clients exist",
        CountText(static_cast<std::uint64_t>(backend->client_count())));
  {
    const StopStatus stopped = executor->StopAndJoin();
    Check(stopped == StopStatus::kOk, "fifo: executor stops", DescribeStopStatus(stopped));
  }
}

void RunCapacityCase() {
  ControlledBackend* backend = nullptr;
  auto executor = MakeExecutor({}, 2, &backend);
  {
    const StartStatus started = executor->Start();
    Check(started == StartStatus::kOk, "capacity: executor starts",
          DescribeStartStatus(started));
  }
  ClientId client;
  (void)TakeOrFail(*executor,
                   SubmitCreateOrFail(*executor, "controlled", Allowed(), &client,
                                      "capacity create"),
                   5000, "capacity create");
  Check(executor->capacity().occupied == 0, "capacity: create slot was released");

  backend->HoldDispatches();
  const Letter h = LetterKey('h');
  RequestId first;
  RequestId second;
  RequestId third;
  {
    const SubmitStatus submitted = executor->TryDispatch(client, h.event, h.text, &first);
    Check(submitted == SubmitStatus::kAccepted, "capacity: first dispatch accepted",
          DescribeSubmitStatus(submitted));
  }
  {
    const SubmitStatus submitted = executor->TryDispatch(client, h.event, h.text, &second);
    Check(submitted == SubmitStatus::kAccepted, "capacity: second dispatch accepted",
          DescribeSubmitStatus(submitted));
  }
  Check(WaitForCapacity(*executor, 1, std::chrono::seconds(5)),
        "capacity: one command is executing while the other is queued");
  Check(executor->capacity().occupied == 2, "capacity: two slots are occupied",
        CountText(executor->capacity().occupied));
  {
    const SubmitStatus submitted = executor->TryDispatch(client, h.event, h.text, &third);
    Check(submitted == SubmitStatus::kQueueFull,
          "capacity: a third dispatch is refused with queue-full",
          DescribeSubmitStatus(submitted));
  }

  backend->ReleaseDispatches();
  (void)TakeOrFail(*executor, first, 5000, "capacity first");
  Check(executor->capacity().occupied == 1,
        "capacity: taking one result frees exactly one slot",
        CountText(executor->capacity().occupied));
  const Letter e = LetterKey('e');
  RequestId fourth;
  {
    const SubmitStatus submitted = executor->TryDispatch(client, e.event, e.text, &fourth);
    Check(submitted == SubmitStatus::kAccepted,
          "capacity: a new dispatch is accepted after one take",
          DescribeSubmitStatus(submitted));
  }
  CommandResult second_result = TakeOrFail(*executor, second, 5000, "capacity second");
  CommandResult fourth_result = TakeOrFail(*executor, fourth, 5000, "capacity fourth");
  Check(DispatchRaw(second_result) == "hh",
        "capacity: the queued request was not dropped or overwritten",
        DispatchRaw(second_result));
  Check(DispatchRaw(fourth_result) == "hhe",
        "capacity: the later request executed after the earlier ones",
        DispatchRaw(fourth_result));
  Check(executor->capacity().occupied == 0, "capacity: all slots free after takes");
  {
    const StopStatus stopped = executor->StopAndJoin();
    Check(stopped == StopStatus::kOk, "capacity: executor stops",
          DescribeStopStatus(stopped));
  }
}

void RunCompletedButUnconsumedCase() {
  ControlledBackend* backend = nullptr;
  auto executor = MakeExecutor({}, 2, &backend);
  {
    const StartStatus started = executor->Start();
    Check(started == StartStatus::kOk, "capacity2: executor starts",
          DescribeStartStatus(started));
  }
  ClientId client;
  (void)TakeOrFail(*executor,
                   SubmitCreateOrFail(*executor, "controlled", Allowed(), &client,
                                      "capacity2 create"),
                   5000, "capacity2 create");
  const Letter h = LetterKey('h');
  RequestId first;
  RequestId second;
  RequestId third;
  {
    const SubmitStatus submitted = executor->TryDispatch(client, h.event, h.text, &first);
    Check(submitted == SubmitStatus::kAccepted, "capacity2: first accepted",
          DescribeSubmitStatus(submitted));
  }
  {
    const SubmitStatus submitted = executor->TryDispatch(client, h.event, h.text, &second);
    Check(submitted == SubmitStatus::kAccepted, "capacity2: second accepted",
          DescribeSubmitStatus(submitted));
  }
  {
    const WaitStatus waited = executor->WaitForResult(first, std::chrono::seconds(5));
    Check(waited == WaitStatus::kReady,
          "capacity2: first result is ready and unconsumed", DescribeWaitStatus(waited));
  }
  {
    const WaitStatus waited = executor->WaitForResult(second, std::chrono::seconds(5));
    Check(waited == WaitStatus::kReady,
          "capacity2: second result is ready and unconsumed", DescribeWaitStatus(waited));
  }
  Check(executor->capacity().completed_unconsumed == 2,
        "capacity2: both completed results occupy capacity",
        CountText(static_cast<std::uint64_t>(
            executor->capacity().completed_unconsumed)));
  {
    const SubmitStatus submitted = executor->TryDispatch(client, h.event, h.text, &third);
    Check(submitted == SubmitStatus::kQueueFull,
          "capacity2: completed-but-unconsumed results still consume capacity",
          DescribeSubmitStatus(submitted));
  }
  CommandResult first_result = TakeOrFail(*executor, first, 5000, "capacity2 first");
  {
    const SubmitStatus submitted = executor->TryDispatch(client, h.event, h.text, &third);
    Check(submitted == SubmitStatus::kAccepted,
          "capacity2: taking one result restores one unit of capacity",
          DescribeSubmitStatus(submitted));
  }
  Check(DispatchRaw(first_result) == "h", "capacity2: the old result was not overwritten",
        DispatchRaw(first_result));
  (void)TakeOrFail(*executor, second, 5000, "capacity2 second");
  (void)TakeOrFail(*executor, third, 5000, "capacity2 third");
  {
    const StopStatus stopped = executor->StopAndJoin();
    Check(stopped == StopStatus::kOk, "capacity2: executor stops",
          DescribeStopStatus(stopped));
  }
}

void RunTimeoutCase() {
  ControlledBackend* backend = nullptr;
  auto executor = MakeExecutor({}, 4, &backend);
  {
    const StartStatus started = executor->Start();
    Check(started == StartStatus::kOk, "timeout: executor starts",
          DescribeStartStatus(started));
  }
  ClientId client;
  (void)TakeOrFail(*executor,
                   SubmitCreateOrFail(*executor, "controlled", Allowed(), &client,
                                      "timeout create"),
                   5000, "timeout create");

  backend->HoldDispatches();
  const Letter h = LetterKey('h');
  RequestId request;
  {
    const SubmitStatus submitted = executor->TryDispatch(client, h.event, h.text, &request);
    Check(submitted == SubmitStatus::kAccepted, "timeout: dispatch accepted",
          DescribeSubmitStatus(submitted));
  }
  {
    const WaitStatus waited =
        executor->WaitForResult(request, std::chrono::milliseconds(30));
    Check(waited == WaitStatus::kTimedOut,
          "timeout: a waiting caller observes a timeout", DescribeWaitStatus(waited));
  }
  Check(backend->dispatch_calls() == 0,
        "timeout: the timed-out request was neither cancelled nor replayed",
        CountText(backend->dispatch_calls()));
  Check(executor->capacity().occupied == 1,
        "timeout: the request still owns its capacity slot");
  backend->ReleaseDispatches();
  CommandResult result = TakeOrFail(*executor, request, 5000, "timeout result");
  Check(result.status == CommandStatus::kOk, "timeout: the request completed normally",
        DescribeCommandStatus(result.status));
  Check(backend->dispatch_calls() == 1,
        "timeout: the request executed exactly once after the timeout",
        CountText(backend->dispatch_calls()));
  CommandResult again;
  {
    const TakeStatus taken = executor->TakeResult(request, &again);
    Check(taken == TakeStatus::kAlreadyTaken,
          "timeout: a second take reports already-taken");
  }
  {
    const WaitStatus waited =
        executor->WaitForResult(request, std::chrono::milliseconds(0));
    Check(waited == WaitStatus::kAlreadyTaken,
          "timeout: waiting after a take reports already-taken",
          DescribeWaitStatus(waited));
  }
  {
    const WaitStatus waited =
        executor->WaitForResult(RequestId{999999}, std::chrono::milliseconds(0));
    Check(waited == WaitStatus::kUnknownRequest,
          "timeout: an unknown request id is named, not treated as a timeout",
          DescribeWaitStatus(waited));
  }
  {
    const StopStatus stopped = executor->StopAndJoin();
    Check(stopped == StopStatus::kOk, "timeout: executor stops",
          DescribeStopStatus(stopped));
  }
}

void RunClosePreservesResultsCase() {
  ControlledBackend* backend = nullptr;
  auto executor = MakeExecutor({}, 8, &backend);
  {
    const StartStatus started = executor->Start();
    Check(started == StartStatus::kOk, "close2: executor starts",
          DescribeStartStatus(started));
  }
  ClientId client;
  (void)TakeOrFail(*executor,
                   SubmitCreateOrFail(*executor, "controlled", Allowed(), &client,
                                      "close2 create"),
                   5000, "close2 create");
  const Letter h = LetterKey('h');
  const Letter e = LetterKey('e');
  const Letter l = LetterKey('l');
  RequestId first;
  RequestId second;
  {
    const SubmitStatus submitted = executor->TryDispatch(client, h.event, h.text, &first);
    Check(submitted == SubmitStatus::kAccepted, "close2: first dispatch accepted",
          DescribeSubmitStatus(submitted));
  }
  {
    const SubmitStatus submitted = executor->TryDispatch(client, e.event, e.text, &second);
    Check(submitted == SubmitStatus::kAccepted, "close2: second dispatch accepted",
          DescribeSubmitStatus(submitted));
  }
  RequestId close_request;
  {
    const SubmitStatus submitted = executor->TryCloseClient(client, &close_request);
    Check(submitted == SubmitStatus::kAccepted,
          "close2: close accepted after two dispatches", DescribeSubmitStatus(submitted));
  }
  RequestId rejected;
  {
    const SubmitStatus submitted = executor->TryDispatch(client, l.event, l.text, &rejected);
    Check(submitted == SubmitStatus::kClientNotOpen,
          "close2: a later dispatch is refused while the close is pending",
          DescribeSubmitStatus(submitted));
  }
  CommandResult closed = TakeOrFail(*executor, close_request, 5000, "close2 close");
  Check(closed.status == CommandStatus::kOk, "close2: close result is ok",
        DescribeCommandStatus(closed.status));
  CommandResult first_result = TakeOrFail(*executor, first, 5000, "close2 first");
  CommandResult second_result = TakeOrFail(*executor, second, 5000, "close2 second");
  Check(first_result.status == CommandStatus::kOk &&
            second_result.status == CommandStatus::kOk,
        "close2: earlier accepted commands have terminal results");
  Check(DispatchRaw(first_result) == "h", "close2: first result is intact",
        DispatchRaw(first_result));
  Check(DispatchRaw(second_result) == "he", "close2: second result is intact",
        DispatchRaw(second_result));
  Check(backend->client_count() == 0, "close2: the closed client is gone",
        CountText(static_cast<std::uint64_t>(backend->client_count())));
  {
    const StopStatus stopped = executor->StopAndJoin();
    Check(stopped == StopStatus::kOk, "close2: executor stops",
          DescribeStopStatus(stopped));
  }
}

void RunStopDrainCase() {
  ControlledBackend* backend = nullptr;
  auto executor = MakeExecutor({}, 8, &backend);
  {
    const StartStatus started = executor->Start();
    Check(started == StartStatus::kOk, "stop-drain: executor starts",
          DescribeStartStatus(started));
  }
  ClientId client;
  (void)TakeOrFail(*executor,
                   SubmitCreateOrFail(*executor, "controlled", Allowed(), &client,
                                      "stop-drain create"),
                   5000, "stop-drain create");

  backend->HoldDispatches();
  const Letter h = LetterKey('h');
  RequestId first;
  RequestId second;
  {
    const SubmitStatus submitted = executor->TryDispatch(client, h.event, h.text, &first);
    Check(submitted == SubmitStatus::kAccepted, "stop-drain: first dispatch accepted",
          DescribeSubmitStatus(submitted));
  }
  {
    const SubmitStatus submitted = executor->TryDispatch(client, h.event, h.text, &second);
    Check(submitted == SubmitStatus::kAccepted, "stop-drain: second dispatch accepted",
          DescribeSubmitStatus(submitted));
  }
  Check(WaitForCapacity(*executor, 1, std::chrono::seconds(5)),
        "stop-drain: one accepted command is executing");

  StopStatus stop_status = StopStatus::kOk;
  std::thread stopper([&] { stop_status = executor->StopAndJoin(); });
  Check(WaitForState(*executor, ExecutorState::kStopping, std::chrono::seconds(5)),
        "stop-drain: the executor enters stopping while a command executes");
  RequestId rejected;
  {
    const SubmitStatus submitted = executor->TryRefreshState(client, &rejected);
    Check(submitted == SubmitStatus::kNotRunning,
          "stop-drain: a submission racing Stop is explicitly refused",
          DescribeSubmitStatus(submitted));
  }
  backend->ReleaseDispatches();
  stopper.join();
  {
    const StopStatus observed = stop_status;
    Check(observed == StopStatus::kOk, "stop-drain: StopAndJoin returns ok",
          DescribeStopStatus(observed));
  }

  CommandResult first_result = TakeOrFail(*executor, first, 5000, "stop-drain first");
  CommandResult second_result = TakeOrFail(*executor, second, 5000, "stop-drain second");
  Check(first_result.status == CommandStatus::kOk &&
            second_result.status == CommandStatus::kOk,
        "stop-drain: both accepted commands have terminal results");
  Check(DispatchRaw(first_result) == "h" && DispatchRaw(second_result) == "hh",
        "stop-drain: accepted commands completed in FIFO order");
  {
    const ExecutorState stopped_state = executor->state();
    Check(stopped_state == ExecutorState::kStopped,
          "stop-drain: the executor is stopped",
          DescribeExecutorState(stopped_state));
  }
  {
    const SubmitStatus submitted = executor->TryDispatch(client, h.event, h.text, &rejected);
    Check(submitted == SubmitStatus::kNotRunning,
          "stop-drain: no command is accepted after Stop",
          DescribeSubmitStatus(submitted));
  }
}

void RunSelfStopCase() {
  ControlledBackend* backend = nullptr;
  auto executor = MakeExecutor({}, 4, &backend);
  {
    const StartStatus started = executor->Start();
    Check(started == StartStatus::kOk, "self-stop: executor starts",
          DescribeStartStatus(started));
  }
  ClientId client;
  (void)TakeOrFail(*executor,
                   SubmitCreateOrFail(*executor, "controlled", Allowed(), &client,
                                      "self-stop create"),
                   5000, "self-stop create");

  std::atomic<int> self_stop_status{-1};
  backend->SetDispatchHook(
      [&] { self_stop_status.store(static_cast<int>(executor->StopAndJoin())); });
  const Letter h = LetterKey('h');
  RequestId request;
  {
    const SubmitStatus submitted = executor->TryDispatch(client, h.event, h.text, &request);
    Check(submitted == SubmitStatus::kAccepted, "self-stop: dispatch accepted",
          DescribeSubmitStatus(submitted));
  }
  CommandResult result = TakeOrFail(*executor, request, 5000, "self-stop dispatch");
  Check(result.status == CommandStatus::kOk,
        "self-stop: the command completed despite the worker calling Stop",
        DescribeCommandStatus(result.status));
  Check(self_stop_status.load() == static_cast<int>(StopStatus::kSelfJoinRefused),
        "self-stop: Stop on the worker is refused instead of deadlocking",
        DescribeStopStatus(static_cast<StopStatus>(self_stop_status.load())));
  {
    const ExecutorState still_running = executor->state();
    Check(still_running == ExecutorState::kRunning,
          "self-stop: the executor remains running after the refused self-join",
          DescribeExecutorState(still_running));
  }
  {
    const StopStatus stopped = executor->StopAndJoin();
    Check(stopped == StopStatus::kOk,
          "self-stop: the caller can still stop the executor",
          DescribeStopStatus(stopped));
  }
}

void RunCommandFaultCase() {
  ControlledBackend::Configuration config;
  config.throw_on_dispatch_once = true;
  ControlledBackend* backend = nullptr;
  auto executor = MakeExecutor(config, 8, &backend);
  {
    const StartStatus started = executor->Start();
    Check(started == StartStatus::kOk, "fault: executor starts",
          DescribeStartStatus(started));
  }
  ClientId client;
  (void)TakeOrFail(*executor,
                   SubmitCreateOrFail(*executor, "controlled", Allowed(), &client,
                                      "fault create"),
                   5000, "fault create");
  const Letter h = LetterKey('h');
  RequestId first;
  {
    const SubmitStatus submitted = executor->TryDispatch(client, h.event, h.text, &first);
    Check(submitted == SubmitStatus::kAccepted, "fault: first dispatch accepted",
          DescribeSubmitStatus(submitted));
  }
  CommandResult failed = TakeOrFail(*executor, first, 5000, "fault first");
  Check(failed.status == CommandStatus::kInternalFault,
        "fault: a command exception becomes an explicit terminal fault",
        DescribeCommandStatus(failed.status));
  Check(std::get_if<DispatchCommandOutcome>(&failed.payload) != nullptr,
        "fault: the failed result still carries the correct payload alternative");
  RequestId second;
  {
    const SubmitStatus submitted = executor->TryDispatch(client, h.event, h.text, &second);
    Check(submitted == SubmitStatus::kAccepted,
          "fault: the executor remains usable after one command fault",
          DescribeSubmitStatus(submitted));
  }
  CommandResult recovered = TakeOrFail(*executor, second, 5000, "fault second");
  Check(recovered.status == CommandStatus::kOk, "fault: the next command succeeds",
        DescribeCommandStatus(recovered.status));
  Check(backend->dispatch_calls() == 1,
        "fault: the failed attempt made no engine dispatch",
        CountText(backend->dispatch_calls()));
  {
    const StopStatus stopped = executor->StopAndJoin();
    Check(stopped == StopStatus::kOk, "fault: executor stops",
          DescribeStopStatus(stopped));
  }
}

void RunStartFailureCase() {
  ControlledBackend::Configuration config;
  config.start_status = RuntimeStartStatus::kDllLoadFailed;
  ControlledBackend* backend = nullptr;
  auto executor = MakeExecutor(config, 4, &backend);
  {
    const StartStatus started = executor->Start();
    Check(started == StartStatus::kRuntimeStartFailed,
          "start-fail: Start reports runtime-start-failed",
          DescribeStartStatus(started));
  }
  {
    const ExecutorState failed_state = executor->state();
    Check(failed_state == ExecutorState::kFailed, "start-fail: state is failed",
          DescribeExecutorState(failed_state));
  }
  RequestId request;
  ClientId client;
  {
    const SubmitStatus submitted =
        executor->TryCreateClient("controlled", Allowed(), &request, &client);
    Check(submitted == SubmitStatus::kNotRunning,
          "start-fail: no command is accepted", DescribeSubmitStatus(submitted));
  }
  Check(executor->RuntimeReport().runtime_start_status ==
            RuntimeStartStatus::kDllLoadFailed,
        "start-fail: the report keeps the real start status");
  {
    const StopStatus stopped = executor->StopAndJoin();
    Check(stopped == StopStatus::kExecutorFailed,
          "start-fail: the first Stop reports executor-failed",
          DescribeStopStatus(stopped));
  }
  {
    const StopStatus repeated = executor->StopAndJoin();
    Check(repeated == StopStatus::kAlreadyStopped,
          "start-fail: a repeated Stop reports already-stopped",
          DescribeStopStatus(repeated));
  }
  {
    const StartStatus restart = executor->Start();
    Check(restart == StartStatus::kAlreadyStarted,
          "start-fail: the failed object does not restart",
          DescribeStartStatus(restart));
  }

  ControlledBackend* invalid_backend = nullptr;
  auto invalid = MakeExecutor({}, 0, &invalid_backend);
  {
    const StartStatus invalid_start = invalid->Start();
    Check(invalid_start == StartStatus::kInvalidOptions,
          "start-fail: zero capacity is invalid-options",
          DescribeStartStatus(invalid_start));
  }
  {
    const ExecutorState invalid_state = invalid->state();
    Check(invalid_state == ExecutorState::kFailed,
          "start-fail: zero capacity leaves the object failed",
          DescribeExecutorState(invalid_state));
  }
}

void RunCleanupFailureCase() {
  ControlledBackend::Configuration config;
  config.shutdown_status = RuntimeShutdownStatus::kFinalizeFailed;
  ControlledBackend* backend = nullptr;
  auto executor = MakeExecutor(config, 4, &backend);
  {
    const StartStatus started = executor->Start();
    Check(started == StartStatus::kOk, "cleanup-fail: executor starts",
          DescribeStartStatus(started));
  }
  ClientId client;
  (void)TakeOrFail(*executor,
                   SubmitCreateOrFail(*executor, "controlled", Allowed(), &client,
                                      "cleanup-fail create"),
                   5000, "cleanup-fail create");
  {
    const StopStatus stopped = executor->StopAndJoin();
    Check(stopped == StopStatus::kCleanupFailed,
          "cleanup-fail: Stop reports cleanup-failed", DescribeStopStatus(stopped));
  }
  {
    const ExecutorState failed_state = executor->state();
    Check(failed_state == ExecutorState::kFailed,
          "cleanup-fail: state is failed, not a clean stop",
          DescribeExecutorState(failed_state));
  }
  Check(executor->RuntimeReport().runtime_shutdown_status ==
            RuntimeShutdownStatus::kFinalizeFailed,
        "cleanup-fail: the report keeps the shutdown failure");
  {
    const StopStatus repeated = executor->StopAndJoin();
    Check(repeated == StopStatus::kAlreadyStopped,
          "cleanup-fail: a repeated Stop reports already-stopped");
  }
}

void RunStopDuringStartCase() {
  ControlledBackend* backend = nullptr;
  auto executor = MakeExecutor({}, 8, &backend);
  backend->HoldStart();

  StartStatus start_status = StartStatus::kInvalidOptions;
  std::thread starter([&] { start_status = executor->Start(); });
  Check(WaitForState(*executor, ExecutorState::kStarting, std::chrono::seconds(5)),
        "start-stop-race: the executor is starting");

  StopStatus stop_status = StopStatus::kNotStarted;
  std::thread stopper([&] { stop_status = executor->StopAndJoin(); });
  Check(WaitForState(*executor, ExecutorState::kStopping, std::chrono::seconds(5)),
        "start-stop-race: Stop closes admission while Runtime::Start is still pending");
  backend->ReleaseStart();
  starter.join();
  stopper.join();

  Check(start_status == StartStatus::kStoppedDuringStart,
        "start-stop-race: Start reports stopped-during-start",
        DescribeStartStatus(start_status));
  Check(stop_status == StopStatus::kOk, "start-stop-race: StopAndJoin reports ok",
        DescribeStopStatus(stop_status));
  const ExecutorState final_state = executor->state();
  Check(final_state == ExecutorState::kStopped,
        "start-stop-race: the executor ends stopped",
        DescribeExecutorState(final_state));
  RequestId request;
  ClientId client;
  const SubmitStatus submitted =
      executor->TryCreateClient("controlled", Allowed(), &request, &client);
  Check(submitted == SubmitStatus::kNotRunning,
        "start-stop-race: no command accepted after the stop won the race",
        DescribeSubmitStatus(submitted));
}

void RunInvalidRequestCase() {
  ControlledBackend* backend = nullptr;
  auto executor = MakeExecutor({}, 4, &backend);
  {
    const StartStatus started = executor->Start();
    Check(started == StartStatus::kOk, "invalid: executor starts",
          DescribeStartStatus(started));
  }
  RequestId request;
  ClientId client;
  Check(executor->TryCreateClient("", Allowed(), &request, &client) ==
            SubmitStatus::kInvalidRequest,
        "invalid: an empty schema id is refused");
  Check(executor->TryCreateClient("controlled", Allowed(), &request, nullptr) ==
            SubmitStatus::kInvalidRequest,
        "invalid: a null client output is refused");
  Check(executor->TryCreateClient("controlled", Allowed(), nullptr, &client) ==
            SubmitStatus::kInvalidRequest,
        "invalid: a null request output is refused");
  Check(executor->TryDispatch(ClientId{0}, KeyEventSnapshot{}, KeyText{}, &request) ==
            SubmitStatus::kInvalidRequest,
        "invalid: a zero client id is refused");
  Check(executor->TryDispatch(ClientId{999}, KeyEventSnapshot{}, KeyText{}, &request) ==
            SubmitStatus::kClientUnknown,
        "invalid: an unknown client id is named");
  Check(executor->capacity().occupied == 0,
        "invalid: refused commands consumed no capacity");
  {
    const StopStatus stopped = executor->StopAndJoin();
    Check(stopped == StopStatus::kOk, "invalid: executor stops",
          DescribeStopStatus(stopped));
  }
}

}  // namespace

void RunSchedulerMode() {
  RunLifecycleAndResultCases();
  RunFifoAndIsolationCase();
  RunCapacityCase();
  RunCompletedButUnconsumedCase();
  RunTimeoutCase();
  RunClosePreservesResultsCase();
  RunStopDrainCase();
  RunSelfStopCase();
  RunCommandFaultCase();
  RunStartFailureCase();
  RunCleanupFailureCase();
  RunStopDuringStartCase();
  RunInvalidRequestCase();
}

void RunSelfCheckFail() {
  ControlledBackend* backend = nullptr;
  auto executor = MakeExecutor({}, 4, &backend);
  (void)executor->Start();
  RequestId request;
  Check(executor->TryRefreshState(ClientId{777}, &request) == SubmitStatus::kAccepted,
        "deliberately false: an unknown client is accepted");
  (void)executor->StopAndJoin();
}

}  // namespace engine_executor
}  // namespace tests
}  // namespace epocanva
