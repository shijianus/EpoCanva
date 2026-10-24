// EpoCanva P3.5 -- real English engine verification through EngineExecutor.
//
// Every engine call in these cases goes through the executor's worker thread: the test never
// holds a RimeRuntime/RimeSession/InputController belonging to the executor. The text printed
// here comes from the fixed fixtures under tests/fixtures/. No host application is touched, no
// TSF sink is registered and no document is edited.
#include <windows.h>

#include <algorithm>
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

#include "engine_executor.h"
#include "input_controller.h"
#include "key_decision.h"
#include "test_support.h"

namespace epocanva {
namespace tests {
namespace engine_executor {

using epocanva::windows::engine::CandidateCommandOutcome;
using epocanva::windows::engine::CandidateStatus;
using epocanva::windows::engine::ClientId;
using epocanva::windows::engine::CommandResult;
using epocanva::windows::engine::CommandStatus;
using epocanva::windows::engine::ControllerOptions;
using epocanva::windows::engine::CreateClientOutcome;
using epocanva::windows::engine::DescribeCandidateStatus;
using epocanva::windows::engine::DescribeCommandStatus;
using epocanva::windows::engine::DescribeDispatchStatus;
using epocanva::windows::engine::DescribeExecutorState;
using epocanva::windows::engine::DescribeStartStatus;
using epocanva::windows::engine::DescribeStopStatus;
using epocanva::windows::engine::DescribeSubmitStatus;
using epocanva::windows::engine::DispatchCommandOutcome;
using epocanva::windows::engine::DispatchStatus;
using epocanva::windows::engine::EngineExecutor;
using epocanva::windows::engine::EvaluateOutcome;
using epocanva::windows::engine::ExecutorOptions;
using epocanva::windows::engine::ExecutorRuntimeReport;
using epocanva::windows::engine::ExecutorState;
using epocanva::windows::engine::RefreshOutcome;
using epocanva::windows::engine::RequestId;
using epocanva::windows::engine::RimeRuntime;
using epocanva::windows::engine::RuntimeInfo;
using epocanva::windows::engine::RuntimeOptions;
using epocanva::windows::engine::RuntimeShutdownStatus;
using epocanva::windows::engine::RuntimeStartStatus;
using epocanva::windows::engine::SessionCreateStatus;
using epocanva::windows::engine::StartStatus;
using epocanva::windows::engine::StopStatus;
using epocanva::windows::engine::SubmitStatus;
using epocanva::windows::engine::TakeStatus;
using epocanva::windows::engine::WaitStatus;
using epocanva::windows::input::DescribeKeyDecisionReason;
using epocanva::windows::input::DescribeKeyDisposition;
using epocanva::windows::input::InputPermission;
using epocanva::windows::input::KeyDecision;
using epocanva::windows::input::KeyDecisionReason;
using epocanva::windows::input::KeyDisposition;
using epocanva::windows::input::KeyEventSnapshot;
using epocanva::windows::input::KeyText;
using epocanva::windows::input::KeyTextStatus;
using epocanva::windows::input::KeyTransition;

namespace {

struct EngineEnvironment {
  std::filesystem::path shared_dir;
  std::filesystem::path user_dir;
  std::filesystem::path log_dir;
  RuntimeOptions runtime;
};

bool PrepareEngineEnvironment(const Options& options, EngineEnvironment* env) {
  if (!PrepareRunRoot(options, "executor", &env->shared_dir, &env->user_dir,
                      &env->log_dir) ||
      !StageFixtures(options, env->shared_dir)) {
    Check(false, "engine: fixture preparation");
    return false;
  }
  if (!PrepareDeployment(options, env->shared_dir, env->user_dir, env->log_dir)) {
    Check(false, "engine: deployment preparation");
    return false;
  }
  env->runtime =
      BaseOptions(options, env->shared_dir, env->user_dir, env->log_dir);
  return true;
}

ExecutorOptions ExecutorOptionsFor(const EngineEnvironment& env, std::size_t capacity) {
  ExecutorOptions options;
  options.runtime = env.runtime;
  options.capacity = capacity;
  return options;
}

ControllerOptions Allowed() {
  return ControllerOptions{true, InputPermission::kAllowed};
}

struct FixtureKeys {
  TextKey h;
  TextKey e;
  TextKey l;
  TextKey a;
  TextKey p;
  TextKey comma;
};

bool BuildFixtureKeys(FixtureKeys* keys) {
  const HKL layout = ::GetKeyboardLayout(0);
  Check(layout != nullptr, "engine: the thread has a keyboard layout");
  if (layout == nullptr) {
    return false;
  }
  wchar_t name[KL_NAMELENGTH] = {};
  if (::GetKeyboardLayoutNameW(name) != FALSE) {
    std::printf("[executor-win] layout %s\n", AsciiOf(std::wstring(name)).c_str());
  }
  keys->h = MakeTextKey(L'h', layout);
  keys->e = MakeTextKey(L'e', layout);
  keys->l = MakeTextKey(L'l', layout);
  keys->a = MakeTextKey(L'a', layout);
  keys->p = MakeTextKey(L'p', layout);
  keys->comma = MakeTextKey(L',', layout);
  return RequireTextKey(keys->h, 'h', "h") && RequireTextKey(keys->e, 'e', "e") &&
         RequireTextKey(keys->l, 'l', "l") && RequireTextKey(keys->a, 'a', "a") &&
         RequireTextKey(keys->p, 'p', "p") &&
         RequireTextKey(keys->comma, ',', "comma");
}

KeyEventSnapshot SpaceEvent() {
  return MakeKeyEvent(epocanva::windows::input::kVirtualKeySpace, false, false, false,
                      false);
}
KeyEventSnapshot ReturnEvent() {
  return MakeKeyEvent(epocanva::windows::input::kVirtualKeyReturn, false, false, false,
                      false);
}
KeyEventSnapshot BackSpaceEvent() {
  return MakeKeyEvent(epocanva::windows::input::kVirtualKeyBack, false, false, false,
                      false);
}

const CreateClientOutcome* CreatePayload(const CommandResult& result) {
  return std::get_if<CreateClientOutcome>(&result.payload);
}

const DispatchCommandOutcome* DispatchPayload(const CommandResult& result) {
  return std::get_if<DispatchCommandOutcome>(&result.payload);
}

const CandidateCommandOutcome* CandidatePayload(const CommandResult& result) {
  return std::get_if<CandidateCommandOutcome>(&result.payload);
}

const RefreshOutcome* RefreshPayload(const CommandResult& result) {
  return std::get_if<RefreshOutcome>(&result.payload);
}

void CheckWorker(const CommandResult& result, const EngineExecutor& executor,
                 const std::string& label) {
  Check(result.executing_thread_id != 0, label + ": command recorded a worker thread",
        CountText(result.executing_thread_id));
  Check(result.executing_thread_id == executor.worker_thread_id(),
        label + ": command executed on the executor worker",
        CountText(result.executing_thread_id) + " vs " +
            CountText(executor.worker_thread_id()));
}

void CheckRefreshRaw(const CommandResult& result, const EngineExecutor& executor,
                     const std::string& expected, const std::string& label) {
  Check(result.status == CommandStatus::kOk, label + ": refresh ok",
        DescribeCommandStatus(result.status));
  CheckWorker(result, executor, label);
  const RefreshOutcome* payload = RefreshPayload(result);
  Check(payload != nullptr, label + ": refresh payload alternative matches");
  if (payload == nullptr) {
    return;
  }
  Check(payload->controller_status ==
            epocanva::windows::engine::ControllerStatus::kOk,
        label + ": controller status is ok");
  Check(AsciiOf(payload->state.raw_input.utf16_code_units) == expected,
        label + ": real raw input is exact",
        AsciiOf(payload->state.raw_input.utf16_code_units));
}

void CheckDispatched(const CommandResult& result, const EngineExecutor& executor,
                     const std::string& expected_raw, const std::string& label) {
  Check(result.status == CommandStatus::kOk, label + ": command ok",
        DescribeCommandStatus(result.status));
  CheckWorker(result, executor, label);
  const DispatchCommandOutcome* payload = DispatchPayload(result);
  Check(payload != nullptr, label + ": dispatch payload alternative matches");
  if (payload == nullptr) {
    return;
  }
  Check(payload->outcome.status == DispatchStatus::kDispatchedHandled,
        label + ": dispatched-handled", DescribeDispatchStatus(payload->outcome.status));
  Check(payload->outcome.engine_key_call_made,
        label + ": exactly one process_key call was made");
  Check(AsciiOf(payload->outcome.state.raw_input.utf16_code_units) == expected_raw,
        label + ": real raw input is exact",
        AsciiOf(payload->outcome.state.raw_input.utf16_code_units));
}

CommandResult DispatchTextKey(EngineExecutor& executor, ClientId client,
                              const TextKey& key, const std::string& label) {
  return TakeOrFail(executor,
                    SubmitDispatchOrFail(executor, client, key.event, key.text, label),
                    60000, label);
}

CommandResult DispatchEvent(EngineExecutor& executor, ClientId client,
                            const KeyEventSnapshot& event, const KeyText& text,
                            const std::string& label) {
  return TakeOrFail(executor,
                    SubmitDispatchOrFail(executor, client, event, text, label), 60000,
                    label);
}

CommandResult RefreshClient(EngineExecutor& executor, ClientId client,
                            const std::string& label) {
  return TakeOrFail(executor, SubmitRefreshOrFail(executor, client, label), 60000,
                    label);
}

ClientId CreateClient(EngineExecutor& executor, const std::string& schema,
                      const ControllerOptions& options, const std::string& label) {
  ClientId client;
  CommandResult created =
      TakeOrFail(executor, SubmitCreateOrFail(executor, schema, options, &client, label),
                 60000, label);
  Check(created.status == CommandStatus::kOk, label + ": create command ok",
        DescribeCommandStatus(created.status));
  const CreateClientOutcome* payload = CreatePayload(created);
  Check(payload != nullptr && payload->client == client,
        label + ": create result carries the client id");
  if (payload != nullptr) {
    Check(payload->session_status == SessionCreateStatus::kOk,
          label + ": real session was created",
          epocanva::windows::engine::DescribeSessionCreateStatus(
              payload->session_status));
  }
  CheckWorker(created, executor, label);
  return client;
}

void CheckCandidateList(const epocanva::windows::engine::EngineState& state,
                        const std::vector<std::string>& expected,
                        const std::string& label) {
  Check(state.candidates.size() == expected.size(),
        label + ": candidate count is exact",
        CountText(state.candidates.size()) + " vs " + CountText(expected.size()));
  const std::size_t common = (std::min)(state.candidates.size(), expected.size());
  for (std::size_t i = 0; i < common; ++i) {
    CheckText(state.candidates[i].text_utf16_code_units, expected[i],
              label + ": candidate " + CountText(i) + " text");
  }
}

void CheckNotDispatched(const CommandResult& result, const EngineExecutor& executor,
                        KeyDecisionReason reason, const std::string& label) {
  Check(result.status == CommandStatus::kOk, label + ": command ok",
        DescribeCommandStatus(result.status));
  CheckWorker(result, executor, label);
  const DispatchCommandOutcome* payload = DispatchPayload(result);
  Check(payload != nullptr, label + ": dispatch payload alternative matches");
  if (payload == nullptr) {
    return;
  }
  Check(payload->outcome.status == DispatchStatus::kNotDispatched,
        label + ": not dispatched", DescribeDispatchStatus(payload->outcome.status));
  Check(!payload->outcome.engine_key_call_made,
        label + ": no process_key call was made");
  Check(payload->outcome.decision.reason == reason,
        label + ": refusal reason is exact",
        DescribeKeyDecisionReason(payload->outcome.decision.reason));
}

void CheckBoundaryDeferred(const CommandResult& result, const EngineExecutor& executor,
                           const std::string& label) {
  Check(result.status == CommandStatus::kOk, label + ": command ok",
        DescribeCommandStatus(result.status));
  CheckWorker(result, executor, label);
  const DispatchCommandOutcome* payload = DispatchPayload(result);
  Check(payload != nullptr, label + ": dispatch payload alternative matches");
  if (payload == nullptr) {
    return;
  }
  Check(payload->outcome.status == DispatchStatus::kDeferredCompositionBoundary,
        label + ": boundary deferred", DescribeDispatchStatus(payload->outcome.status));
  Check(!payload->outcome.engine_key_call_made,
        label + ": deferred boundary made no process_key call");
  Check(payload->outcome.decision.requires_composition_boundary(),
        label + ": decision reports a composition boundary",
        DescribeKeyDisposition(payload->outcome.decision.disposition));
}

struct Barrier {
  explicit Barrier(int participants) : participants_(participants) {}
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
  std::mutex mutex_;
  std::condition_variable condition_;
  int participants_ = 0;
  int arrived_ = 0;
  unsigned generation_ = 0;
};

}  // namespace

void RunEngineMode(const Options& options) {
  EngineEnvironment env;
  if (!PrepareEngineEnvironment(options, &env)) {
    return;
  }

  FixtureKeys keys;
  if (!BuildFixtureKeys(&keys)) {
    return;
  }

  EngineExecutor executor(ExecutorOptionsFor(env, 128));
  {
    const StartStatus started = executor.Start();
    Check(started == StartStatus::kOk, "engine: executor starts",
          DescribeStartStatus(started));
    if (started != StartStatus::kOk) {
      std::printf("[executor-win] start diagnostic: %s\n",
                  executor.RuntimeReport().diagnostic.c_str());
      return;
    }
  }
  const std::uint32_t worker_id = executor.worker_thread_id();
  Check(worker_id != 0 && worker_id != ::GetCurrentThreadId(),
        "engine: runtime/worker thread is not the caller thread");

  // --- item 18 while the executor owns the process-global service -----------------------
  {
    RimeRuntime competing;
    const RuntimeStartStatus status = competing.Start(env.runtime);
    Check(status == RuntimeStartStatus::kAnotherRuntimeActive,
          "exclusion: a direct RimeRuntime cannot start while the executor is running",
          epocanva::windows::engine::DescribeRuntimeStartStatus(status));
  }
  {
    EngineExecutor second(ExecutorOptionsFor(env, 4));
    const StartStatus status = second.Start();
    Check(status == StartStatus::kRuntimeStartFailed,
          "exclusion: a second executor cannot start", DescribeStartStatus(status));
    Check(second.RuntimeReport().runtime_start_status ==
              RuntimeStartStatus::kAnotherRuntimeActive,
          "exclusion: the second executor reports the active runtime slot");
    const StopStatus stopped = second.StopAndJoin();
    Check(stopped == StopStatus::kExecutorFailed,
          "exclusion: the failed second executor stops as executor-failed",
          DescribeStopStatus(stopped));
  }

  // --- items 5, 6: evaluate is pure; one dispatch moves the engine once ----------------
  const ClientId client_a =
      CreateClient(executor, "epocanva_test_english", Allowed(), "client A");
  {
    const TextKey* h = &keys.h;
    RequestId evaluate_requests[3] = {
        SubmitEvaluateOrFail(executor, client_a, h->event, h->text, "evaluate A 1"),
        SubmitEvaluateOrFail(executor, client_a, h->event, h->text, "evaluate A 2"),
        SubmitEvaluateOrFail(executor, client_a, h->event, h->text, "evaluate A 3")};
    CommandResult first =
        TakeOrFail(executor, evaluate_requests[0], 60000, "evaluate A 1");
    const EvaluateOutcome* first_payload = std::get_if<EvaluateOutcome>(&first.payload);
    Check(first_payload != nullptr, "evaluate A: payload alternative matches");
    KeyDecision first_decision;
    if (first_payload != nullptr) {
      first_decision = first_payload->evaluation.decision;
      Check(first_payload->evaluation.status ==
                epocanva::windows::engine::ControllerStatus::kOk,
            "evaluate A: controller status is ok");
      Check(first_payload->evaluation.decision.disposition ==
                KeyDisposition::kEngineTextCandidate,
            "evaluate A: h is a text candidate",
            DescribeKeyDisposition(first_payload->evaluation.decision.disposition));
    }
    CheckWorker(first, executor, "evaluate A 1");
    for (int i = 1; i < 3; ++i) {
      CommandResult repeated =
          TakeOrFail(executor, evaluate_requests[i], 60000, "evaluate A repeat");
      CheckWorker(repeated, executor, "evaluate A repeat");
      const EvaluateOutcome* payload =
          std::get_if<EvaluateOutcome>(&repeated.payload);
      Check(payload != nullptr, "evaluate A repeat: payload alternative matches");
      if (payload != nullptr && first_payload != nullptr) {
        Check(payload->evaluation.decision == first_decision,
              "evaluate A repeat: the decision is idempotent");
      }
    }
    CommandResult before = RefreshClient(executor, client_a, "refresh after evaluates");
    CheckRefreshRaw(before, executor, "", "refresh after evaluates");
  }

  CommandResult h_dispatched = DispatchTextKey(executor, client_a, keys.h, "dispatch h");
  CheckDispatched(h_dispatched, executor, "h", "dispatch h");
  CommandResult e_dispatched = DispatchTextKey(executor, client_a, keys.e, "dispatch e");
  CheckDispatched(e_dispatched, executor, "he", "dispatch e");
  CommandResult l_dispatched = DispatchTextKey(executor, client_a, keys.l, "dispatch l");
  CheckDispatched(l_dispatched, executor, "hel", "dispatch l");
  {
    const DispatchCommandOutcome* payload = DispatchPayload(l_dispatched);
    Check(payload != nullptr && payload->outcome.state.is_composing,
          "dispatch l: composition is active");
    if (payload != nullptr) {
      CheckCandidateList(payload->outcome.state, {"hel", "hello", "help", "helmet"},
                         "dispatch l");
      CheckText(payload->outcome.state.preedit.utf16_code_units, "hel",
                "dispatch l: real preedit is hel");
    }
  }

  // --- item 6: BackSpace, Return, Space; item 8: one commit -----------------------------
  CommandResult backspace =
      DispatchEvent(executor, client_a, BackSpaceEvent(), KeyText{}, "dispatch BackSpace");
  CheckDispatched(backspace, executor, "he", "dispatch BackSpace");
  (void)DispatchTextKey(executor, client_a, keys.l, "redispatch l");
  CommandResult returned =
      DispatchEvent(executor, client_a, ReturnEvent(), KeyText{}, "dispatch Return");
  {
    const DispatchCommandOutcome* payload = DispatchPayload(returned);
    Check(returned.status == CommandStatus::kOk, "dispatch Return: command ok",
          DescribeCommandStatus(returned.status));
    CheckWorker(returned, executor, "dispatch Return");
    Check(payload != nullptr, "dispatch Return: payload alternative matches");
    if (payload != nullptr) {
      Check(payload->outcome.status == DispatchStatus::kDispatchedHandled,
            "dispatch Return: handled");
      Check(payload->outcome.pending_commit.present,
            "dispatch Return: a commit is present");
      CheckText(payload->outcome.pending_commit.text_utf16_code_units, "hel",
                "dispatch Return: commit is hel");
      Check(!payload->outcome.state.is_composing,
            "dispatch Return: composition ended");
    }
  }
  {
    CommandResult after_commit = RefreshClient(executor, client_a, "refresh after commit");
    CheckRefreshRaw(after_commit, executor, "", "refresh after commit");
  }
  CommandResult space_empty =
      DispatchEvent(executor, client_a, SpaceEvent(), KeyText{}, "dispatch Space empty");
  {
    const DispatchCommandOutcome* payload = DispatchPayload(space_empty);
    Check(space_empty.status == CommandStatus::kOk, "dispatch Space empty: command ok",
          DescribeCommandStatus(space_empty.status));
    CheckWorker(space_empty, executor, "dispatch Space empty");
    Check(payload != nullptr, "dispatch Space empty: payload alternative matches");
    if (payload != nullptr) {
      Check(payload->outcome.status == DispatchStatus::kDispatchedHandled,
            "dispatch Space empty: handled");
      CheckText(payload->outcome.pending_commit.text_utf16_code_units, " ",
                "dispatch Space empty: committed exactly one space");
    }
  }
  CommandResult a_after =
      DispatchTextKey(executor, client_a, keys.a, "dispatch a after commit");
  CheckDispatched(a_after, executor, "a", "dispatch a after commit");
  {
    const DispatchCommandOutcome* payload = DispatchPayload(a_after);
    Check(payload != nullptr && !payload->outcome.pending_commit.present,
          "dispatch a after commit: the commit was consumed exactly once");
  }

  // --- item 6: explicit candidate acceptance -------------------------------------------
  const ClientId client_b =
      CreateClient(executor, "epocanva_test_english", Allowed(), "client B");
  for (const TextKey* key : {&keys.h, &keys.e, &keys.l}) {
    CommandResult dispatched = DispatchTextKey(executor, client_b, *key, "client B letter");
    Check(dispatched.status == CommandStatus::kOk, "client B: letter command ok",
          DescribeCommandStatus(dispatched.status));
  }
  {
    CommandResult selected =
        TakeOrFail(executor, SubmitAcceptOrFail(executor, client_b, 1, "accept B 1"),
                   60000, "accept B 1");
    Check(selected.status == CommandStatus::kOk, "accept B: command ok",
          DescribeCommandStatus(selected.status));
    CheckWorker(selected, executor, "accept B");
    const CandidateCommandOutcome* payload = CandidatePayload(selected);
    Check(payload != nullptr, "accept B: payload alternative matches");
    if (payload != nullptr) {
      Check(payload->outcome.status == CandidateStatus::kAccepted,
            "accept B: explicit selection accepted",
            DescribeCandidateStatus(payload->outcome.status));
      Check(payload->outcome.engine_selection_call_made,
            "accept B: exactly one select_candidate call");
      CheckText(payload->outcome.state.preedit.utf16_code_units, "hello",
                "accept B: preedit shows the selected completion");
      Check(!payload->outcome.pending_commit.present,
            "accept B: selection alone produced no commit");
    }
  }
  CommandResult selected_space =
      DispatchEvent(executor, client_b, SpaceEvent(), KeyText{}, "Space after selection");
  {
    const DispatchCommandOutcome* payload = DispatchPayload(selected_space);
    Check(selected_space.status == CommandStatus::kOk, "Space after selection: command ok",
          DescribeCommandStatus(selected_space.status));
    CheckWorker(selected_space, executor, "Space after selection");
    Check(payload != nullptr, "Space after selection: payload alternative matches");
    if (payload != nullptr) {
      Check(payload->outcome.status == DispatchStatus::kDispatchedHandled,
            "Space after selection: handled");
      CheckText(payload->outcome.pending_commit.text_utf16_code_units, "hello ",
                "Space after selection: explicit completion committed");
    }
  }

  // --- item 7: boundary, shortcut and forbidden permission do not dispatch -------------
  const ClientId client_c =
      CreateClient(executor, "epocanva_test_english", Allowed(), "client C");
  for (const TextKey* key : {&keys.h, &keys.e, &keys.l}) {
    (void)DispatchTextKey(executor, client_c, *key, "client C letter");
  }
  CommandResult boundary = DispatchTextKey(executor, client_c, keys.comma, "boundary comma");
  CheckBoundaryDeferred(boundary, executor, "boundary comma");
  {
    CommandResult after_boundary = RefreshClient(executor, client_c, "refresh after boundary");
    CheckRefreshRaw(after_boundary, executor, "hel", "refresh after boundary");
  }
  CommandResult resumed = DispatchTextKey(executor, client_c, keys.l, "resume after boundary");
  CheckDispatched(resumed, executor, "hell", "resume after boundary");

  {
    const KeyEventSnapshot control_a =
        MakeKeyEvent(0x41, false, true, false, false);
    CommandResult shortcut =
        DispatchEvent(executor, client_c, control_a, KeyText{}, "shortcut Ctrl+A");
    CheckNotDispatched(shortcut, executor, KeyDecisionReason::kShortcutModifier,
                       "shortcut Ctrl+A");
  }

  const ClientId client_d =
      CreateClient(executor, "epocanva_test_english",
                   ControllerOptions{true, InputPermission::kForbidden}, "client D");
  {
    CommandResult forbidden = DispatchTextKey(executor, client_d, keys.h, "forbidden h");
    CheckNotDispatched(forbidden, executor, KeyDecisionReason::kPermissionForbidden,
                       "forbidden h");
    CommandResult refresh = RefreshClient(executor, client_d, "refresh forbidden");
    CheckRefreshRaw(refresh, executor, "", "refresh forbidden");
  }

  // --- items 9, 10: close refuses new commands and preserves earlier results ------------
  RequestId left_request;
  {
    const SubmitStatus submitted =
        executor.TryDispatch(client_b, keys.h.event, keys.h.text, &left_request);
    Check(submitted == SubmitStatus::kAccepted,
          "client B: a dispatch is accepted but its result is left untaken",
          DescribeSubmitStatus(submitted));
  }
  RequestId close_b;
  {
    const SubmitStatus submitted = executor.TryCloseClient(client_b, &close_b);
    Check(submitted == SubmitStatus::kAccepted, "close B: close accepted",
          DescribeSubmitStatus(submitted));
  }
  {
    RequestId rejected;
    const SubmitStatus submitted =
        executor.TryDispatch(client_b, keys.h.event, keys.h.text, &rejected);
    Check(submitted == SubmitStatus::kClientNotOpen,
          "close B: a dispatch after close is refused", DescribeSubmitStatus(submitted));
  }
  {
    CommandResult closed = TakeOrFail(executor, close_b, 60000, "close B result");
    Check(closed.status == CommandStatus::kOk, "close B: close command ok",
          DescribeCommandStatus(closed.status));
    CheckWorker(closed, executor, "close B result");
  }
  {
    CommandResult left =
        TakeOrFail(executor, left_request, 60000, "close B: earlier result after close");
    CheckDispatched(left, executor, "h",
                    "close B: earlier result after close");
  }
  {
    CommandResult refresh = RefreshClient(executor, client_c, "refresh after close B");
    CheckRefreshRaw(refresh, executor, "hell", "refresh after close B");
  }

  // --- item 3: two caller threads, two clients, no crosstalk ----------------------------
  {
    const ClientId client_x =
        CreateClient(executor, "epocanva_test_english", Allowed(), "client X");
    const ClientId client_y =
        CreateClient(executor, "epocanva_test_english", Allowed(), "client Y");
    struct ThreadRun {
      std::array<RequestId, 3> requests{};
      std::array<SubmitStatus, 3> submits{};
      std::array<WaitStatus, 3> waits{};
      std::array<TakeStatus, 3> takes{};
      std::array<CommandResult, 3> results{};
    };
    ThreadRun run_x;
    ThreadRun run_y;
    Barrier barrier(2);
    auto sequence = [](EngineExecutor& target, ClientId client, Barrier* gate,
                       ThreadRun* run, const TextKey* first, const TextKey* second,
                       const TextKey* third) {
      gate->Wait();
      const TextKey* keys[3] = {first, second, third};
      for (int i = 0; i < 3; ++i) {
        run->submits[i] = target.TryDispatch(client, keys[i]->event, keys[i]->text,
                                             &run->requests[i]);
      }
      for (int i = 0; i < 3; ++i) {
        run->waits[i] =
            target.WaitForResult(run->requests[i], std::chrono::seconds(60));
        if (run->waits[i] == WaitStatus::kReady) {
          run->takes[i] = target.TakeResult(run->requests[i], &run->results[i]);
        }
      }
    };
    std::thread thread_x(sequence, std::ref(executor), client_x, &barrier, &run_x,
                         &keys.h, &keys.e, &keys.l);
    std::thread thread_y(sequence, std::ref(executor), client_y, &barrier, &run_y,
                         &keys.a, &keys.p, &keys.p);
    thread_x.join();
    thread_y.join();
    const std::string expected_x[3] = {"h", "he", "hel"};
    const std::string expected_y[3] = {"a", "ap", "app"};
    for (int i = 0; i < 3; ++i) {
      Check(run_x.submits[i] == SubmitStatus::kAccepted,
            "two threads X: dispatch accepted", DescribeSubmitStatus(run_x.submits[i]));
      Check(run_x.waits[i] == WaitStatus::kReady, "two threads X: result ready");
      Check(run_x.takes[i] == TakeStatus::kTaken, "two threads X: result taken");
      const DispatchCommandOutcome* payload_x =
          DispatchPayload(run_x.results[i]);
      Check(payload_x != nullptr &&
                AsciiOf(payload_x->outcome.state.raw_input.utf16_code_units) ==
                    expected_x[i],
            "two threads X: independent sequence is stable");
      Check(run_y.submits[i] == SubmitStatus::kAccepted,
            "two threads Y: dispatch accepted", DescribeSubmitStatus(run_y.submits[i]));
      Check(run_y.waits[i] == WaitStatus::kReady, "two threads Y: result ready");
      Check(run_y.takes[i] == TakeStatus::kTaken, "two threads Y: result taken");
      const DispatchCommandOutcome* payload_y =
          DispatchPayload(run_y.results[i]);
      Check(payload_y != nullptr &&
                AsciiOf(payload_y->outcome.state.raw_input.utf16_code_units) ==
                    expected_y[i],
            "two threads Y: independent sequence is stable");
    }
    CommandResult refresh_x =
        RefreshClient(executor, client_x, "refresh X after two threads");
    CheckRefreshRaw(refresh_x, executor, "hel", "refresh X after two threads");
    CommandResult refresh_y =
        RefreshClient(executor, client_y, "refresh Y after two threads");
    CheckRefreshRaw(refresh_y, executor, "app", "refresh Y after two threads");

    CommandResult commit_x = DispatchEvent(executor, client_x, ReturnEvent(), KeyText{},
                                           "commit X");
    const DispatchCommandOutcome* commit_x_payload = DispatchPayload(commit_x);
    Check(commit_x.status == CommandStatus::kOk, "commit X: command ok",
          DescribeCommandStatus(commit_x.status));
    Check(commit_x_payload != nullptr &&
              commit_x_payload->outcome.pending_commit.text_utf16_code_units ==
                  U16("hel"),
          "commit X: committed hel");
    CommandResult refresh_y_after =
        RefreshClient(executor, client_y, "refresh Y after X commit");
    CheckRefreshRaw(refresh_y_after, executor, "app",
                    "refresh Y after X commit: Y is untouched");
    CommandResult space_y =
        DispatchEvent(executor, client_y, SpaceEvent(), KeyText{}, "space Y");
    const DispatchCommandOutcome* space_y_payload = DispatchPayload(space_y);
    Check(space_y.status == CommandStatus::kOk, "space Y: command ok",
          DescribeCommandStatus(space_y.status));
    Check(space_y_payload != nullptr &&
              space_y_payload->outcome.pending_commit.text_utf16_code_units ==
                  U16("app "),
          "space Y: committed app plus one space");
  }

  // --- item 9: explicitly close client A ------------------------------------------------
  {
    RequestId close_a;
    const SubmitStatus submitted = executor.TryCloseClient(client_a, &close_a);
    Check(submitted == SubmitStatus::kAccepted, "close A: close accepted",
          DescribeSubmitStatus(submitted));
    CommandResult closed = TakeOrFail(executor, close_a, 60000, "close A result");
    Check(closed.status == CommandStatus::kOk, "close A: close command ok",
          DescribeCommandStatus(closed.status));
    RequestId rejected;
    const SubmitStatus after_close =
        executor.TryRefreshState(client_a, &rejected);
    Check(after_close == SubmitStatus::kClientNotOpen,
          "close A: refresh after close is refused",
          DescribeSubmitStatus(after_close));
  }

  // --- item 15: stop, join, no live sessions, no worker ---------------------------------
  {
    const StopStatus stopped = executor.StopAndJoin();
    Check(stopped == StopStatus::kOk, "stop: StopAndJoin ok",
          DescribeStopStatus(stopped));
  }
  {
    const ExecutorState stopped_state = executor.state();
    Check(stopped_state == ExecutorState::kStopped, "stop: state is stopped",
          DescribeExecutorState(stopped_state));
  }
  {
    const StopStatus repeated = executor.StopAndJoin();
    Check(repeated == StopStatus::kAlreadyStopped,
          "stop: repeated Stop is already-stopped", DescribeStopStatus(repeated));
  }
  {
    const ExecutorRuntimeReport report = executor.RuntimeReport();
    Check(report.start_thread_id == worker_id,
          "report: Runtime::Start ran on the worker",
          CountText(report.start_thread_id));
    Check(report.shutdown_thread_id == worker_id,
          "report: Runtime::Shutdown ran on the worker",
          CountText(report.shutdown_thread_id));
    Check(report.runtime_start_status == RuntimeStartStatus::kOk,
          "report: runtime start status is ok");
    Check(report.runtime_shutdown_status == RuntimeShutdownStatus::kOk,
          "report: runtime shutdown status is ok");
    Check(report.runtime_info.initialize_calls == 1,
          "report: initialize was called exactly once",
          CountText(report.runtime_info.initialize_calls));
    Check(report.runtime_info.finalize_calls == 1,
          "report: finalize was called exactly once",
          CountText(report.runtime_info.finalize_calls));
    Check(report.runtime_info.live_session_count == 0,
          "report: no live session remains",
          CountText(static_cast<std::uint64_t>(
              report.runtime_info.live_session_count)));
    Check(report.live_sessions_after_cleanup == 0,
          "report: cleanup reports no live session",
          CountText(static_cast<std::uint64_t>(report.live_sessions_after_cleanup)));
    Check(report.api_counts.balanced(), "report: API acquire/free pairs are balanced",
          DescribeCounts(report.api_counts));
    Check(report.api_counts.process_key_calls > 0,
          "report: the real engine saw process_key calls",
          CountText(report.api_counts.process_key_calls));
    Check(report.client_close_failures == 0,
          "report: no client close failed",
          CountText(report.client_close_failures));
  }

  // --- items 11, 12 with a fresh real-engine executor -----------------------------------
  {
    EngineExecutor capacity_executor(ExecutorOptionsFor(env, 2));
    const StartStatus started = capacity_executor.Start();
    Check(started == StartStatus::kOk, "capacity: real executor starts",
          DescribeStartStatus(started));
    if (started == StartStatus::kOk) {
      const ClientId client =
          CreateClient(capacity_executor, "epocanva_test_english", Allowed(),
                       "capacity client");
      RequestId first;
      RequestId second;
      const SubmitStatus first_status =
          capacity_executor.TryDispatch(client, keys.h.event, keys.h.text, &first);
      Check(first_status == SubmitStatus::kAccepted,
            "capacity: first real dispatch accepted", DescribeSubmitStatus(first_status));
      const SubmitStatus second_status =
          capacity_executor.TryDispatch(client, keys.e.event, keys.e.text, &second);
      Check(second_status == SubmitStatus::kAccepted,
            "capacity: second real dispatch accepted",
            DescribeSubmitStatus(second_status));
      RequestId third;
      const SubmitStatus full_status =
          capacity_executor.TryDispatch(client, keys.l.event, keys.l.text, &third);
      Check(full_status == SubmitStatus::kQueueFull,
            "capacity: third real dispatch is queue-full",
            DescribeSubmitStatus(full_status));
      CommandResult first_result =
          TakeOrFail(capacity_executor, first, 60000, "capacity first real");
      CheckDispatched(first_result, capacity_executor, "h", "capacity first real");
      CommandResult second_result =
          TakeOrFail(capacity_executor, second, 60000, "capacity second real");
      CheckDispatched(second_result, capacity_executor, "he", "capacity second real");
      const SubmitStatus recovered =
          capacity_executor.TryDispatch(client, keys.l.event, keys.l.text, &third);
      Check(recovered == SubmitStatus::kAccepted,
            "capacity: a real dispatch is accepted after taking results",
            DescribeSubmitStatus(recovered));
      CommandResult third_result =
          TakeOrFail(capacity_executor, third, 60000, "capacity third real");
      CheckDispatched(third_result, capacity_executor, "hel", "capacity third real");
      const StopStatus stopped = capacity_executor.StopAndJoin();
      Check(stopped == StopStatus::kOk, "capacity: real executor stops",
            DescribeStopStatus(stopped));
      Check(capacity_executor.RuntimeReport().api_counts.balanced(),
            "capacity: real engine API pairs are balanced");
    }
  }

  // --- item 18: the process-global slot is free after Stop -----------------------------
  {
    RimeRuntime recovery;
    const RuntimeStartStatus started = recovery.Start(env.runtime);
    Check(started == RuntimeStartStatus::kOk,
          "exclusion: a standalone runtime starts after the executor stops",
          epocanva::windows::engine::DescribeRuntimeStartStatus(started));
    if (started == RuntimeStartStatus::kOk) {
      const RuntimeShutdownStatus stopped = recovery.Shutdown();
      Check(stopped == RuntimeShutdownStatus::kOk,
            "exclusion: the standalone recovery runtime shuts down",
            epocanva::windows::engine::DescribeRuntimeShutdownStatus(stopped));
    }
  }
}

void RunRaceMode(const Options& options) {
  EngineEnvironment env;
  if (!PrepareRunRoot(options, "race", &env.shared_dir, &env.user_dir, &env.log_dir) ||
      !StageFixtures(options, env.shared_dir) ||
      !PrepareDeployment(options, env.shared_dir, env.user_dir, env.log_dir)) {
    Check(false, "race: fixture and deployment preparation");
    return;
  }
  env.runtime = BaseOptions(options, env.shared_dir, env.user_dir, env.log_dir);

  FixtureKeys keys;
  if (!BuildFixtureKeys(&keys)) {
    return;
  }

  EngineExecutor executor(ExecutorOptionsFor(env, 256));
  {
    const StartStatus started = executor.Start();
    Check(started == StartStatus::kOk, "race: executor starts",
          DescribeStartStatus(started));
    if (started != StartStatus::kOk) {
      return;
    }
  }
  const ClientId client =
      CreateClient(executor, "epocanva_test_english", Allowed(), "race client");

  struct SubmitterResult {
    std::vector<RequestId> accepted;
    std::array<std::uint64_t, 4> rejected_by_status{};
  };
  constexpr int kSubmitters = 4;
  SubmitterResult submitters[kSubmitters];
  std::mutex accepted_mutex;
  std::condition_variable accepted_condition;
  std::size_t accepted_total = 0;
  Barrier barrier(kSubmitters);

  auto submitter = [&](int index) {
    const bool use_h = (index % 2) == 0;
    barrier.Wait();
    for (int i = 0; i < 200; ++i) {
      RequestId request;
      const TextKey& key = use_h ? keys.h : keys.a;
      const SubmitStatus status =
          executor.TryDispatch(client, key.event, key.text, &request);
      if (status == SubmitStatus::kAccepted) {
        {
          std::lock_guard<std::mutex> lock(accepted_mutex);
          submitters[index].accepted.push_back(request);
          ++accepted_total;
        }
        accepted_condition.notify_all();
        continue;
      }
      if (status == SubmitStatus::kNotRunning) {
        break;
      }
      const std::size_t slot = static_cast<std::size_t>(status);
      if (slot < submitters[index].rejected_by_status.size()) {
        ++submitters[index].rejected_by_status[slot];
      }
      if (status == SubmitStatus::kQueueFull) {
        break;
      }
      break;
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(kSubmitters);
  for (int i = 0; i < kSubmitters; ++i) {
    threads.emplace_back(submitter, i);
  }

  {
    std::unique_lock<std::mutex> lock(accepted_mutex);
    accepted_condition.wait_for(lock, std::chrono::seconds(30), [&] {
      return accepted_total >= 20;
    });
    Check(accepted_total >= 20, "race: the submitters were racing before Stop",
          CountText(static_cast<std::uint64_t>(accepted_total)));
  }

  const StopStatus stopped = executor.StopAndJoin();
  Check(stopped == StopStatus::kOk, "race: StopAndJoin ok", DescribeStopStatus(stopped));

  for (std::thread& thread : threads) {
    thread.join();
  }

  std::vector<RequestId> accepted;
  {
    std::lock_guard<std::mutex> lock(accepted_mutex);
    for (const SubmitterResult& result : submitters) {
      accepted.insert(accepted.end(), result.accepted.begin(), result.accepted.end());
    }
  }
  Check(!accepted.empty(), "race: at least one request was accepted");
  for (const RequestId request : accepted) {
    CommandResult result;
    const WaitStatus waited =
        executor.WaitForResult(request, std::chrono::seconds(60));
    Check(waited == WaitStatus::kReady,
          "race: every accepted request has a terminal result",
          "request " + CountText(request.value) + " " + "wait failed");
    if (waited != WaitStatus::kReady) {
      continue;
    }
    const TakeStatus taken = executor.TakeResult(request, &result);
    Check(taken == TakeStatus::kTaken, "race: every terminal result is takeable");
    if (taken != TakeStatus::kTaken) {
      continue;
    }
    Check(result.request == request, "race: the result id matches the accepted request");
    Check(result.status != CommandStatus::kInvalidRequest,
          "race: an accepted request is never left invalid",
          DescribeCommandStatus(result.status));
    Check(result.executing_thread_id == executor.worker_thread_id(),
          "race: the terminal result came from the worker");
  }

  {
    const ExecutorState state = executor.state();
    Check(state == ExecutorState::kStopped, "race: the executor is stopped",
          DescribeExecutorState(state));
  }
  {
    const ExecutorRuntimeReport report = executor.RuntimeReport();
    Check(report.live_sessions_after_cleanup == 0,
          "race: no live session remains after Stop",
          CountText(static_cast<std::uint64_t>(report.live_sessions_after_cleanup)));
    Check(report.runtime_info.finalize_calls == 1,
          "race: finalize was called once",
          CountText(report.runtime_info.finalize_calls));
    Check(report.api_counts.balanced(), "race: API pairs are balanced",
          DescribeCounts(report.api_counts));
  }
}

void RunStartupFailureMode(const Options& options) {
  std::filesystem::path shared_dir;
  std::filesystem::path user_dir;
  std::filesystem::path log_dir;
  if (!PrepareRunRoot(options, "startup-failure", &shared_dir, &user_dir, &log_dir) ||
      !StageFixtures(options, shared_dir)) {
    Check(false, "startup-failure: fixture preparation");
    return;
  }

  {
    RuntimeOptions runtime = BaseOptions(options, shared_dir, user_dir, log_dir);
    runtime.rime_dll_path = options.run_root / "missing" / "rime.dll";
    EngineExecutor executor(ExecutorOptions{runtime, 4});
    const StartStatus started = executor.Start();
    Check(started == StartStatus::kRuntimeStartFailed,
          "startup-failure: a missing DLL fails Start",
          DescribeStartStatus(started));
    const ExecutorState state = executor.state();
    Check(state == ExecutorState::kFailed, "startup-failure: state is failed",
          DescribeExecutorState(state));
    Check(executor.RuntimeReport().runtime_start_status ==
              RuntimeStartStatus::kDllLoadFailed,
          "startup-failure: the start status is dll-load-failed");
    RequestId request;
    ClientId client;
    const SubmitStatus submitted =
        executor.TryCreateClient("epocanva_test_english", Allowed(), &request, &client);
    Check(submitted == SubmitStatus::kNotRunning,
          "startup-failure: no command is accepted", DescribeSubmitStatus(submitted));
    const StopStatus stopped = executor.StopAndJoin();
    Check(stopped == StopStatus::kExecutorFailed,
          "startup-failure: Stop reports executor-failed",
          DescribeStopStatus(stopped));
  }

  {
    RuntimeOptions runtime = BaseOptions(options, shared_dir, user_dir, log_dir);
    runtime.rime_dll_path = std::filesystem::path("rime.dll");
    EngineExecutor executor(ExecutorOptions{runtime, 4});
    const StartStatus started = executor.Start();
    Check(started == StartStatus::kRuntimeStartFailed,
          "startup-failure: a relative DLL path fails Start",
          DescribeStartStatus(started));
    Check(executor.RuntimeReport().runtime_start_status ==
              RuntimeStartStatus::kInvalidOptions,
          "startup-failure: the start status is invalid-options");
    (void)executor.StopAndJoin();
  }

  if (!options.baseline_dll.empty() &&
      std::filesystem::is_regular_file(options.baseline_dll)) {
    RuntimeOptions runtime = BaseOptions(options, shared_dir, user_dir, log_dir);
    runtime.rime_dll_path = options.baseline_dll;
    EngineExecutor executor(ExecutorOptions{runtime, 4});
    const StartStatus started = executor.Start();
    Check(started == StartStatus::kRuntimeStartFailed,
          "startup-failure: the baseline DLL fails Start",
          DescribeStartStatus(started));
    const ExecutorRuntimeReport report = executor.RuntimeReport();
    Check(report.runtime_start_status == RuntimeStartStatus::kEnglishModuleMissing,
          "startup-failure: the baseline failure is english-module-missing",
          epocanva::windows::engine::DescribeRuntimeStartStatus(
              report.runtime_start_status));
    Check(report.runtime_info.initialize_calls == 1,
          "startup-failure: the baseline runtime initialized once before failing",
          CountText(report.runtime_info.initialize_calls));
    Check(report.runtime_info.finalize_calls == 1,
          "startup-failure: the failed initialization was finalized",
          CountText(report.runtime_info.finalize_calls));
    Check(report.api_counts.balanced(),
          "startup-failure: the baseline failure left API pairs balanced",
          DescribeCounts(report.api_counts));
    (void)executor.StopAndJoin();

    RuntimeOptions good = BaseOptions(options, shared_dir, user_dir, log_dir);
    EngineExecutor recovery(ExecutorOptions{good, 4});
    const StartStatus recovered = recovery.Start();
    Check(recovered == StartStatus::kOk,
          "startup-failure: a valid executor starts after the failed initialization",
          DescribeStartStatus(recovered));
    if (recovered == StartStatus::kOk) {
      const StopStatus stopped = recovery.StopAndJoin();
      Check(stopped == StopStatus::kOk,
            "startup-failure: the recovery executor stops cleanly",
            DescribeStopStatus(stopped));
    }
  } else {
    std::printf(
        "[executor-win] UNVERIFIED baseline module-missing case: no --baseline-dll\n");
  }
}

}  // namespace engine_executor
}  // namespace tests
}  // namespace epocanva
