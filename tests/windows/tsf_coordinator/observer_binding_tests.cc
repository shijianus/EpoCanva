// EpoCanva P3.10 -- coordinator integration cases over the real context observer.
//
// Every binding here is created by ObservedBindingController from a real observer event;
// no case calls Bind() to set up its normal flow (the direct Bind calls in the uncertain
// case are the deliberate id-bypass probes). External edits are real read/write edit
// sessions issued with a distinct real TfClientId.
//
// EXIT CODES: 0 pass; 5 a check failed; 14 the falsifiability control (in the other file).
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "context_observer.h"
#include "context_registry.h"
#include "coordinator.h"
#include "coordinator_test_support.h"
#include "engine_executor/test_support.h"
#include "observed_binding.h"
#include "observer_binding_tests.h"
#include "observer_test_support.h"
#include "permission_query.h"

namespace epocanva {
namespace tsf {
namespace coordinator {
namespace test {

namespace {

namespace ctx = epocanva::tsf::context;
namespace oct = epocanva::tsf::context::test;
namespace ex = epocanva::tests::engine_executor;
namespace input = epocanva::windows::input;

using epocanva::windows::engine::ClientId;
using epocanva::windows::engine::CommandResult;
using epocanva::windows::engine::EngineExecutor;
using epocanva::windows::engine::RequestId;
using epocanva::windows::engine::TakeStatus;
using epocanva::windows::engine::WaitStatus;
using ctx::ContextIdRecord;
using epocanva::tsf::edit::test::MutationFault;

ex::TextKey Letter(wchar_t character) {
  return ex::MakeTextKey(character, ::GetKeyboardLayout(0));
}

// The host's fresh-client source for one binding. It blocks on the executor's create
// command (bounded); this is test scaffolding, not the product's client-rebuild policy.
class ExecutorClientProvider final : public ObservedEngineClients {
 public:
  ExecutorClientProvider(EngineExecutor& executor, std::string schema,
                         epocanva::windows::engine::ControllerOptions options,
                         DWORD timeout_ms)
      : executor_(executor),
        schema_(std::move(schema)),
        options_(options),
        timeout_ms_(timeout_ms) {}

  ClientId NextClient() override {
    RequestId request;
    ClientId client;
    const epocanva::windows::engine::SubmitStatus submitted =
        executor_.TryCreateClient(schema_, options_, &request, &client);
    if (submitted != epocanva::windows::engine::SubmitStatus::kAccepted) {
      last_submit_ = submitted;
      return ClientId{};
    }
    if (executor_.WaitForResult(request, std::chrono::milliseconds(timeout_ms_)) !=
        WaitStatus::kReady) {
      return ClientId{};
    }
    CommandResult result;
    if (executor_.TakeResult(request, &result) != TakeStatus::kTaken || !result.ok()) {
      return ClientId{};
    }
    created_.push_back(client.value);
    return client;
  }

  const std::vector<std::uint64_t>& created() const { return created_; }
   epocanva::windows::engine::SubmitStatus last_submit() const { return last_submit_; }

 private:
  EngineExecutor& executor_;
  std::string schema_;
  epocanva::windows::engine::ControllerOptions options_;
  DWORD timeout_ms_ = 0;
  std::vector<std::uint64_t> created_;
  epocanva::windows::engine::SubmitStatus last_submit_ = epocanva::windows::engine::SubmitStatus::kAccepted;
};

struct ControlledObservedEnv {
  std::unique_ptr<TestHost> host;
  ControlledHarness harness;
  std::unique_ptr<ctx::PermissionSource> permission;
  std::unique_ptr<ctx::ContextRegistry> registry;
  std::unique_ptr<ContextCoordinator> coordinator;
  std::unique_ptr<ExecutorClientProvider> clients;
  std::unique_ptr<ctx::ContextObserver> observer;
  class DeferredLossGate;
  std::unique_ptr<DeferredLossGate> gate;
  std::unique_ptr<ObservedBindingController> controller;
  TestDocument* doc_a = nullptr;
  TfClientId external = 0;
  std::string error;
};

// TEST-ONLY notification gate, not a product feature and not a TSF notification source. It
// forwards every observer callback to the real controller immediately, except OnTargetLost
// while deferral is enabled: those are buffered and released explicitly by the test. This
// fixes execution-time revalidation (path B) ordering when the environment delivers a real
// OnSetFocus concurrently. It cannot manufacture a notification; the observer counters show
// whether one actually arrived.
class ControlledObservedEnv::DeferredLossGate final
    : public ctx::ContextObserverSink {
 public:
  explicit DeferredLossGate(ObservedBindingController& inner) : inner_(inner) {}

  void SetDeferLosses(bool defer) { defer_losses_ = defer; }
  std::size_t pending_losses() const { return pending_.size(); }
  std::size_t ReleaseLosses() {
    std::vector<ctx::ContextDropReason> pending;
    pending.swap(pending_);
    for (ctx::ContextDropReason reason : pending) {
      inner_.OnTargetLost(reason);
    }
    return pending.size();
  }

  void OnTargetLost(ctx::ContextDropReason reason) override {
    if (defer_losses_) {
      pending_.push_back(reason);
      return;
    }
    inner_.OnTargetLost(reason);
  }
  void OnTargetBound(ITfContext* context,
                     const ctx::ObservedTarget& target) override {
    inner_.OnTargetBound(context, target);
  }
  void OnOwnEditCompleted(ITfContext* context,
                          const ctx::ObservedTarget& target,
                          const ctx::SelectionSnapshot& selection) override {
    inner_.OnOwnEditCompleted(context, target, selection);
  }

 private:
  ObservedBindingController& inner_;
  bool defer_losses_ = false;
  std::vector<ctx::ContextDropReason> pending_;
};

bool SetupControlledObserved(
    ControlledObservedEnv* env, const TestDocumentOptions& options, const char* label,
    ctx::ThreadFocusPolicy policy = ctx::ThreadFocusPolicy::kLabWin32Fallback,
    bool defer_losses = false) {
  env->host = TestHost::Create(&env->error);
  Check(env->host != nullptr, std::string(label) + ": host created", env->error);
  if (env->host == nullptr) {
    return false;
  }
  env->doc_a = env->host->CreateDocument(options, &env->error);
  Check(env->doc_a != nullptr, std::string(label) + ": document created", env->error);
  if (env->doc_a == nullptr) {
    return false;
  }
  if (!oct::EnsureTestThreadFocus(&env->error)) {
    Check(false, std::string(label) + ": real Win32 thread focus", env->error);
    return false;
  }
  if (!CreateControlledHarness(&env->harness)) {
    return false;
  }
  env->permission = ctx::CreateRealPermissionSource(env->host->client_id());
  env->registry = std::make_unique<ctx::ContextRegistry>(8);
  env->coordinator = std::make_unique<ContextCoordinator>(
      *env->harness.executor, env->host->client_id(), *env->permission);
  env->clients = std::make_unique<ExecutorClientProvider>(
      *env->harness.executor, "controlled",
      epocanva::windows::engine::ControllerOptions{
          true, epocanva::windows::input::InputPermission::kAllowed},
      10000);
  env->observer = std::make_unique<ctx::ContextObserver>(
      *env->registry, env->host->client_id(), policy);
  env->controller = std::make_unique<ObservedBindingController>(
      *env->observer, *env->coordinator, *env->registry, *env->clients);
  if (defer_losses) {
    // TEST-ONLY controlled ordering: the observer's real notifications reach this gate;
    // OnTargetLost is buffered until the test releases it. No notification is synthesized.
    env->gate = std::make_unique<ControlledObservedEnv::DeferredLossGate>(
        *env->controller);
  }
  oct::FocusCallResult focus;
  if (!oct::SetThreadFocus(*env->host, env->doc_a, &focus, &env->error)) {
    Check(false, std::string(label) + ": SetFocus", env->error);
    return false;
  }
  if (!env->observer->Subscribe(
          env->host->thread_manager_for_test(),
          env->gate != nullptr
              ? static_cast<ctx::ContextObserverSink*>(env->gate.get())
              : static_cast<ctx::ContextObserverSink*>(env->controller.get()),
          &env->error)) {
    Check(false, std::string(label) + ": subscribed", env->error);
    return false;
  }
  const oct::ObserverDriveResult driven =
      oct::PumpUntilSettled(*env->observer, *env->host, 20000);
  Check(driven.settled && env->coordinator->bound(),
        std::string(label) + ": the observer bound the coordinator",
        oct::DescribeObserverSnapshot(driven.final_state));
  // These cases need a readable collapsed selection to create an ordinary binding. If the
  // real selection is unsupported, the setup fails explicitly instead of substituting a
  // SelectionUnsupported refusal for the focus guard under test.
  Check(driven.final_state.last_refusal_reason !=
            ctx::ContextDropReason::kSelectionUnsupported,
        std::string(label) +
            ": SelectionUnsupported is not substituted for a focus result",
        ctx::DescribeContextDropReason(
            driven.final_state.last_refusal_reason));
  Check(env->controller->Snapshot().binds_ok == 1,
        std::string(label) + ": one successful observed bind",
        std::to_string(env->controller->Snapshot().binds_ok));
  env->external =
      oct::ExternalClientId(env->host->thread_manager_for_test(), &env->error);
  Check(env->external != 0 && env->external != env->host->client_id(),
        std::string(label) + ": a distinct real external client", env->error);
  return env->coordinator->bound();
}

bool WireObserved(ControlledObservedEnv* env, const TestDocumentOptions& options,
                  const char* label,
                  ctx::ThreadFocusPolicy policy =
                      ctx::ThreadFocusPolicy::kLabWin32Fallback) {
  env->host = TestHost::Create(&env->error);
  Check(env->host != nullptr, std::string(label) + ": host created", env->error);
  if (env->host == nullptr) {
    return false;
  }
  env->doc_a = env->host->CreateDocument(options, &env->error);
  Check(env->doc_a != nullptr, std::string(label) + ": document created", env->error);
  if (env->doc_a == nullptr) {
    return false;
  }
  if (!oct::EnsureTestThreadFocus(&env->error)) {
    Check(false, std::string(label) + ": real Win32 thread focus", env->error);
    return false;
  }
  env->permission = ctx::CreateRealPermissionSource(env->host->client_id());
  env->registry = std::make_unique<ctx::ContextRegistry>(8);
  env->observer = std::make_unique<ctx::ContextObserver>(
      *env->registry, env->host->client_id(), policy);
  return true;
}

bool FinishWiring(ControlledObservedEnv* env, EngineExecutor& executor,
                  const char* schema, DWORD create_timeout_ms, const char* label) {
  env->coordinator = std::make_unique<ContextCoordinator>(
      executor, env->host->client_id(), *env->permission);
  env->clients = std::make_unique<ExecutorClientProvider>(
      executor, schema,
      epocanva::windows::engine::ControllerOptions{
          true, epocanva::windows::input::InputPermission::kAllowed},
      create_timeout_ms);
  env->controller = std::make_unique<ObservedBindingController>(
      *env->observer, *env->coordinator, *env->registry, *env->clients);
  oct::FocusCallResult focus;
  if (!oct::SetThreadFocus(*env->host, env->doc_a, &focus, &env->error)) {
    Check(false, std::string(label) + ": SetFocus", env->error);
    return false;
  }
  if (!env->observer->Subscribe(
          env->host->thread_manager_for_test(),
          env->gate != nullptr
              ? static_cast<ctx::ContextObserverSink*>(env->gate.get())
              : static_cast<ctx::ContextObserverSink*>(env->controller.get()),
          &env->error)) {
    Check(false, std::string(label) + ": subscribed", env->error);
    return false;
  }
  const oct::ObserverDriveResult driven =
      oct::PumpUntilSettled(*env->observer, *env->host, 20000);
  Check(driven.settled && env->coordinator->bound(),
        std::string(label) + ": the observer bound the coordinator",
        oct::DescribeObserverSnapshot(driven.final_state));
  // These cases need a readable collapsed selection to create an ordinary binding. If the
  // real selection is unsupported, the setup fails explicitly instead of substituting a
  // SelectionUnsupported refusal for the focus guard under test.
  Check(driven.final_state.last_refusal_reason !=
            ctx::ContextDropReason::kSelectionUnsupported,
        std::string(label) +
            ": SelectionUnsupported is not substituted for a focus result",
        ctx::DescribeContextDropReason(
            driven.final_state.last_refusal_reason));
  Check(env->controller->Snapshot().binds_ok == 1,
        std::string(label) + ": one successful observed bind",
        std::to_string(env->controller->Snapshot().binds_ok));
  env->external =
      oct::ExternalClientId(env->host->thread_manager_for_test(), &env->error);
  Check(env->external != 0 && env->external != env->host->client_id(),
        std::string(label) + ": a distinct real external client", env->error);
  return env->coordinator->bound();
}

// A rebind is admitted while the old binding retires. Input on the new binding is kRetiring
// until that retirement is terminal; these cases drive it with the real completion facts and
// then assert the new binding is usable. This is the replacement for the old "old clients are
// only closed at StopAndJoin" behaviour.
void PumpRetirements(ControlledObservedEnv* env, EngineExecutor& executor,
                     const char* label) {
  (void)executor;
  const DriveResult driven =
      DriveToCompletion(*env->coordinator, *env->host, executor, 30000);
  Check(driven.completed && driven.final_state.retirement.retiring_bindings == 0,
        std::string(label) + ": the retirement ledger is terminal",
        DescribeSnapshot(driven.final_state));
  Check(driven.final_state.retirement.close_requests ==
            driven.final_state.retirement.close_results_taken,
        std::string(label) + ": this coordinator consumed every close result it submitted",
        std::to_string(driven.final_state.retirement.close_requests) + "/" +
            std::to_string(driven.final_state.retirement.close_results_taken));
}

// --- cases --------------------------------------------------------------------------------

int RunObservedBindAndType() {
  ControlledObservedEnv env;
  TestDocumentOptions options;
  options.initial_text_utf16 = L"abcdef";
  options.initial_selection_start_utf16 = 2;
  options.initial_selection_end_utf16 = 2;
  if (!SetupControlledObserved(&env, options, "observed-bind")) {
    return Finish();
  }
  const CoordinatorSnapshot bound = env.coordinator->Snapshot();
  Check(bound.bound && bound.identity.generation == 1,
        "observed-bind: generation 1 comes from the observer",
        std::to_string(bound.identity.generation));
  Check(bound.tracked_caret_utf16 == 2,
        "observed-bind: the real selection caret is adopted",
        std::to_string(bound.tracked_caret_utf16));
  Check(env.observer->Snapshot().target_binds == 1 &&
            env.controller->Snapshot().target_losses == 0,
        "observed-bind: one target, no loss", "");

  const ex::TextKey h = Letter(L'h');
  const DriveResult driven = TypeKeyAndDrive(*env.coordinator, *env.host,
                                             *env.harness.executor, h.event, h.text,
                                             "observed-bind: h");
  CheckOutcome(driven.final_state, WorkOutcome::kApplied,
               "observed-bind: h applied");
  CheckDocText(env.doc_a->text_utf16(), L"abhcdef",
               "observed-bind: the preedit landed at the real caret");
  Check(driven.final_state.tracked_caret_utf16 == 3,
        "observed-bind: the real post-edit caret won over the plan mirror",
        std::to_string(driven.final_state.tracked_caret_utf16));
  Check(driven.final_state.observed_caret_updates >= 1 &&
            driven.final_state.observed_caret_applied >= 1,
        "observed-bind: OnEndEdit's ecReadOnly caret was applied",
        std::to_string(driven.final_state.observed_caret_updates) + "/" +
            std::to_string(driven.final_state.observed_caret_applied));
  Check(env.controller->Snapshot().target_binds == 1 &&
            env.controller->Snapshot().target_losses == 0,
        "observed-bind: the own edit did not invalidate or rebind", "");
  Check(env.clients->created().size() == 1,
        "observed-bind: exactly one engine client for one binding",
        std::to_string(env.clients->created().size()));
  return Finish();
}

int RunObservedRealComposeProgress(const Options& options) {
  RealHarness harness;
  if (!CreateRealHarness(options, "observed-real-compose", &harness)) {
    return Finish();
  }
  ControlledObservedEnv env;
  if (!WireObserved(&env, TestDocumentOptions{L"", 0, 0}, "observed-real-compose")) {
    return Finish();
  }
  if (!FinishWiring(&env, *harness.executor, "epocanva_test_english", 30000,
                    "observed-real-compose")) {
    return Finish();
  }
  const CoordinatorSnapshot initial = env.coordinator->Snapshot();
  const wchar_t letters[] = {L'h', L'e', L'l'};
  const std::wstring expected[] = {L"h", L"he", L"hel"};
  for (int index = 0; index < 3; ++index) {
    const ex::TextKey key = Letter(letters[index]);
    const DriveResult driven = TypeKeyAndDrive(
        *env.coordinator, *env.host, *harness.executor, key.event, key.text,
        "observed-real-compose: letter");
    CheckOutcome(driven.final_state, WorkOutcome::kApplied,
                 "observed-real-compose: letter applied");
    CheckDocText(env.doc_a->text_utf16(), expected[index],
                 "observed-real-compose: the real English preedit progress");
  }
  const CoordinatorSnapshot final = env.coordinator->Snapshot();
  Check(final.identity.generation == initial.identity.generation,
        "observed-real-compose: no self-invalidation across h/he/hel",
        std::to_string(final.identity.generation));
  Check(final.tracked_caret_utf16 == 3,
        "observed-real-compose: the real caret follows the composition",
        std::to_string(final.tracked_caret_utf16));
  Check(env.controller->Snapshot().target_binds == 1 &&
            env.controller->Snapshot().target_losses == 0 &&
            env.controller->Snapshot().invalidations == 0,
        "observed-real-compose: one binding for the whole composition",
        "binds=" + std::to_string(env.controller->Snapshot().target_binds) +
            " losses=" + std::to_string(env.controller->Snapshot().target_losses));
  Check(env.observer->Snapshot().own_edits >= 3,
        "observed-real-compose: every letter was an own real edit",
        std::to_string(env.observer->Snapshot().own_edits));
  Check(env.clients->created().size() == 1,
        "observed-real-compose: one engine client for the binding",
        std::to_string(env.clients->created().size()));
  return Finish();
}

int RunObservedRetireRoundtrip(const Options& options) {
  RealHarness harness;
  if (!CreateRealHarness(options, "observed-retire-roundtrip", &harness)) {
    return Finish();
  }
  ControlledObservedEnv env;
  if (!WireObserved(&env, TestDocumentOptions{L"", 0, 0},
                    "observed-retire-roundtrip")) {
    return Finish();
  }
  TestDocument* doc_b = env.host->CreateDocument(TestDocumentOptions{}, &env.error);
  Check(doc_b != nullptr, "retire-roundtrip: document B created", env.error);
  if (doc_b == nullptr) {
    return Finish();
  }
  if (!FinishWiring(&env, *harness.executor, "epocanva_test_english", 30000,
                    "observed-retire-roundtrip")) {
    return Finish();
  }

  auto type_commit = [&](wchar_t first, wchar_t second, const char* label) {
    const ex::TextKey one = Letter(first);
    const ex::TextKey two = Letter(second);
    TypeKeyAndDrive(*env.coordinator, *env.host, *harness.executor, one.event,
                    one.text, std::string(label) + " first");
    TypeKeyAndDrive(*env.coordinator, *env.host, *harness.executor, two.event,
                    two.text, std::string(label) + " second");
    const input::KeyEventSnapshot enter =
        ex::MakeKeyEvent(0x0D, false, false, false, false);
    input::KeyText no_text;
    no_text.status = input::KeyTextStatus::kNoText;
    return TypeKeyAndDrive(*env.coordinator, *env.host, *harness.executor, enter,
                           no_text, std::string(label) + " Return");
  };
  auto switch_to = [&](TestDocument* document, const char* label) {
    oct::FocusCallResult focus;
    Check(oct::SetThreadFocus(*env.host, document, &focus, &env.error),
          std::string(label) + ": real SetFocus", env.error);
    const oct::ObserverDriveResult driven =
        oct::PumpUntilSettled(*env.observer, *env.host, 20000);
    Check(driven.settled && env.coordinator->bound(),
          std::string(label) + ": the observer re-bound",
          oct::DescribeObserverSnapshot(driven.final_state));
    PumpRetirements(&env, *harness.executor, label);
    const CoordinatorSnapshot now = env.coordinator->Snapshot();
    Check(now.identity.context_id ==
              [&]() {
                ContextIdRecord record;
                env.registry->Observe(document->context(), &record);
                return record.context_id;
              }(),
          std::string(label) + ": the bound identity is the focused document", "");
    const engine::ClientUsage clients = harness.executor->clients();
    // One pre-created harness client plus the current observed binding. The count must not
    // grow with the number of switches.
    Check(clients.live == 2,
          std::string(label) +
              ": exactly the harness client plus the current binding are live",
          std::to_string(clients.live));
    Check(clients.close_requests == clients.close_results_taken,
          std::string(label) + ": every close result was consumed",
          std::to_string(clients.close_requests) + "/" +
              std::to_string(clients.close_results_taken));
  };

  const DriveResult first = type_commit(L'a', L'b', "retire-roundtrip: A first");
  CheckOutcome(first.final_state, WorkOutcome::kApplied,
               "retire-roundtrip: A's committed text applied");
  CheckDocText(env.doc_a->text_utf16(), L"ab",
               "retire-roundtrip: A holds the committed text");

  switch_to(doc_b, "retire-roundtrip: A to B");
  CheckDocText(env.doc_a->text_utf16(), L"ab",
               "retire-roundtrip: A keeps its text across the switch");
  const DriveResult second = type_commit(L'c', L'd', "retire-roundtrip: B first");
  CheckOutcome(second.final_state, WorkOutcome::kApplied,
               "retire-roundtrip: B's committed text applied");
  CheckDocText(doc_b->text_utf16(), L"cd",
               "retire-roundtrip: B holds only its text");

  switch_to(env.doc_a, "retire-roundtrip: B to A");
  const DriveResult third = type_commit(L'e', L'f', "retire-roundtrip: A second");
  CheckOutcome(third.final_state, WorkOutcome::kApplied,
               "retire-roundtrip: A's second text applied");
  CheckDocText(env.doc_a->text_utf16(), L"abef",
               "retire-roundtrip: A's old text was not replaced");
  CheckDocText(doc_b->text_utf16(), L"cd",
               "retire-roundtrip: B was not touched by the return");
  Check(harness.executor->RuntimeReport().runtime_info.initialize_calls == 1,
        "retire-roundtrip: no runtime restart across the round trip",
        std::to_string(
            harness.executor->RuntimeReport().runtime_info.initialize_calls));

  const engine::ClientUsage clients = harness.executor->clients();
  Check(clients.created == 4,
        "retire-roundtrip: one harness client plus one fresh client per binding",
        std::to_string(clients.created));
  Check(clients.close_requests == 2 &&
            clients.close_results_taken == 2,
        "retire-roundtrip: both ended bindings' close results were consumed",
        std::to_string(clients.close_requests) + "/" +
            std::to_string(clients.close_results_taken));
  Check(clients.live == 2,
        "retire-roundtrip: the live count stayed constant across three bindings",
        std::to_string(clients.live));
  Check(env.controller->Snapshot().binds_ok == 3,
        "retire-roundtrip: three observed bindings",
        std::to_string(env.controller->Snapshot().binds_ok));
  return Finish();
}

int RunObservedComposeRetirement(const Options& options) {
  RealHarness harness;
  if (!CreateRealHarness(options, "observed-compose-retirement", &harness)) {
    return Finish();
  }
  ControlledObservedEnv env;
  if (!WireObserved(&env, TestDocumentOptions{L"", 0, 0},
                    "observed-compose-retirement")) {
    return Finish();
  }
  TestDocument* doc_b = env.host->CreateDocument(TestDocumentOptions{}, &env.error);
  Check(doc_b != nullptr, "compose-retirement: document B created", env.error);
  if (doc_b == nullptr) {
    return Finish();
  }
  if (!FinishWiring(&env, *harness.executor, "epocanva_test_english", 30000,
                    "observed-compose-retirement")) {
    return Finish();
  }

  const wchar_t letters[] = {L'h', L'e', L'l'};
  const std::wstring expected[] = {L"h", L"he", L"hel"};
  for (int index = 0; index < 3; ++index) {
    const ex::TextKey key = Letter(letters[index]);
    const DriveResult driven = TypeKeyAndDrive(
        *env.coordinator, *env.host, *harness.executor, key.event, key.text,
        "compose-retirement: letter");
    CheckOutcome(driven.final_state, WorkOutcome::kApplied,
                 "compose-retirement: letter applied");
    CheckDocText(env.doc_a->text_utf16(), expected[index],
                 "compose-retirement: the real composition progressed");
  }
  ULONG compositions = 0;
  Check(SUCCEEDED(oct::CountCompositions(env.doc_a->context(), &compositions)) &&
            compositions == 1,
        "compose-retirement: TSF really has one live composition on A",
        std::to_string(compositions));
  const auto before = env.doc_a->diagnostics().store;
  Check(before.composition_start_calls == 1 && before.composition_end_calls == 0,
        "compose-retirement: A's composition is owned and open", "");

  oct::FocusCallResult focus;
  Check(oct::SetThreadFocus(*env.host, doc_b, &focus, &env.error),
        "compose-retirement: real SetFocus moved to B", env.error);
  const oct::ObserverDriveResult rebound =
      oct::PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(rebound.settled && env.coordinator->bound(),
        "compose-retirement: the observer bound B",
        oct::DescribeObserverSnapshot(rebound.final_state));
  PumpRetirements(&env, *harness.executor, "compose-retirement: A to B");

  CheckDocText(env.doc_a->text_utf16(), L"hel",
               "compose-retirement: A keeps exactly the displayed text");
  compositions = 99;
  Check(SUCCEEDED(oct::CountCompositions(env.doc_a->context(), &compositions)) &&
            compositions == 0,
        "compose-retirement: the real TSF composition on A ended",
        std::to_string(compositions));
  const auto after = env.doc_a->diagnostics().store;
  Check(after.composition_end_calls == 1,
        "compose-retirement: the context owner observed exactly one end",
        std::to_string(after.composition_end_calls));
  Check(after.text_mutations == before.text_mutations &&
            after.set_text_calls == before.set_text_calls,
        "compose-retirement: finishing issued no SetText and added no text",
        "mutations=" + std::to_string(after.text_mutations) +
            " set_text=" + std::to_string(after.set_text_calls));
  const CoordinatorSnapshot retired = env.coordinator->Snapshot();
  Check(retired.retirement.last_cleanup_state == CleanupState::kFinished &&
            retired.retirement.finished == 1 &&
            retired.retirement.blocked == 0,
        "compose-retirement: the cleanup is Finished, not Blocked",
        DescribeSnapshot(retired));

  const ex::TextKey x = Letter(L'x');
  const DriveResult on_b = TypeKeyAndDrive(*env.coordinator, *env.host,
                                           *harness.executor, x.event, x.text,
                                           "compose-retirement: B types");
  CheckOutcome(on_b.final_state, WorkOutcome::kApplied,
               "compose-retirement: B's input resumed after cleanup");
  CheckDocText(doc_b->text_utf16(), L"x",
               "compose-retirement: B holds only its own text");
  CheckDocText(env.doc_a->text_utf16(), L"hel",
               "compose-retirement: A was not touched by B");

  // Returning to A starts a fresh composition after the preserved ordinary text.
  Check(oct::SetThreadFocus(*env.host, env.doc_a, &focus, &env.error),
        "compose-retirement: real SetFocus moved back to A", env.error);
  const oct::ObserverDriveResult back =
      oct::PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(back.settled && env.coordinator->bound(),
        "compose-retirement: the observer bound A again",
        oct::DescribeObserverSnapshot(back.final_state));
  PumpRetirements(&env, *harness.executor, "compose-retirement: B to A");
  const ex::TextKey i = Letter(L'i');
  const DriveResult again = TypeKeyAndDrive(*env.coordinator, *env.host,
                                            *harness.executor, i.event, i.text,
                                            "compose-retirement: A types again");
  CheckOutcome(again.final_state, WorkOutcome::kApplied,
               "compose-retirement: the new composition applied");
  CheckDocText(env.doc_a->text_utf16(), L"heli",
               "compose-retirement: the new preedit followed the preserved text");
  compositions = 0;
  Check(SUCCEEDED(oct::CountCompositions(env.doc_a->context(), &compositions)) &&
            compositions == 1,
        "compose-retirement: A owns a new live composition",
        std::to_string(compositions));
  const engine::ClientUsage clients = harness.executor->clients();
  Check(clients.created == 4 && clients.close_results_taken == 2,
        "compose-retirement: one fresh client per binding, two reclaims consumed",
        "created=" + std::to_string(clients.created) +
            " closed_results=" + std::to_string(clients.close_results_taken));
  Check(clients.live == 2,
        "compose-retirement: live clients did not grow with the switches",
        std::to_string(clients.live));
  return Finish();
}

int RunObservedComposeRetirementReadOnly(const Options& options) {
  RealHarness harness;
  if (!CreateRealHarness(options, "observed-compose-readonly", &harness)) {
    return Finish();
  }
  ControlledObservedEnv env;
  if (!WireObserved(&env, TestDocumentOptions{L"", 0, 0},
                    "observed-compose-readonly")) {
    return Finish();
  }
  TestDocument* doc_b = env.host->CreateDocument(TestDocumentOptions{}, &env.error);
  Check(doc_b != nullptr, "compose-readonly: document B created", env.error);
  if (doc_b == nullptr) {
    return Finish();
  }
  if (!FinishWiring(&env, *harness.executor, "epocanva_test_english", 30000,
                    "observed-compose-readonly")) {
    return Finish();
  }
  for (wchar_t letter : {L'h', L'e', L'l'}) {
    const ex::TextKey key = Letter(letter);
    TypeKeyAndDrive(*env.coordinator, *env.host, *harness.executor, key.event,
                    key.text, "compose-readonly: letter");
  }
  CheckDocText(env.doc_a->text_utf16(), L"hel",
               "compose-readonly: the composition holds hel");
  const auto before = env.doc_a->diagnostics().store;
  env.doc_a->SetReadOnly(true);

  oct::FocusCallResult focus;
  Check(oct::SetThreadFocus(*env.host, doc_b, &focus, &env.error),
        "compose-readonly: real SetFocus moved to B", env.error);
  const oct::ObserverDriveResult rebound =
      oct::PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(rebound.settled && env.coordinator->bound(),
        "compose-readonly: the observer bound B",
        oct::DescribeObserverSnapshot(rebound.final_state));
  PumpRetirements(&env, *harness.executor, "compose-readonly");

  const CoordinatorSnapshot retired = env.coordinator->Snapshot();
  Check(retired.retirement.last_cleanup_state == CleanupState::kBlocked &&
            retired.retirement.blocked == 1,
        "compose-readonly: cleanup is explicitly Blocked, never Completed",
        DescribeSnapshot(retired));
  // The record is dropped (completed accounting) but the cleanup did not succeed:
  // finished == 0, the real composition is still live, and no end call was made.
  Check(retired.retirement.completed == 1 &&
            retired.retirement.finished == 0 &&
            retired.retirement.already_ordinary == 0,
        "compose-readonly: a blocked cleanup is not counted as a finished one",
        "completed=" + std::to_string(retired.retirement.completed) +
            " finished=" + std::to_string(retired.retirement.finished) +
            " already_ordinary=" +
            std::to_string(retired.retirement.already_ordinary));
  Check(retired.retirement.retiring_bindings == 0 &&
            !retired.retirement.admission_paused,
        "compose-readonly: the terminal record no longer pauses input",
        std::to_string(retired.retirement.retiring_bindings));
  ULONG remaining_compositions = 0;
  Check(SUCCEEDED(oct::CountCompositions(env.doc_a->context(),
                                         &remaining_compositions)) &&
            remaining_compositions == 1,
        "compose-readonly: A's composition really remains (not silently ended)",
        std::to_string(remaining_compositions));
  Check(retired.retirement.last_cleanup_block == CleanupBlockReason::kReadOnly ||
            retired.retirement.last_cleanup_block ==
                CleanupBlockReason::kPermissionDenied,
        "compose-readonly: the block names the read-only/permission refusal",
        DescribeCleanupBlockReason(retired.retirement.last_cleanup_block));
  const auto after = env.doc_a->diagnostics().store;
  Check(after.text_mutations == before.text_mutations &&
            after.set_text_calls == before.set_text_calls &&
            after.composition_end_calls == before.composition_end_calls,
        "compose-readonly: no destructive cleanup wrote text or ended the composition",
        "mutations=" + std::to_string(after.text_mutations) +
            " set_text=" + std::to_string(after.set_text_calls) +
            " end=" + std::to_string(after.composition_end_calls));
  CheckDocText(env.doc_a->text_utf16(), L"hel",
               "compose-readonly: the displayed text was preserved");
  env.doc_a->SetReadOnly(false);

  // The blocked cleanup is terminal for admission: B is usable, and the block stays visible.
  const ex::TextKey x = Letter(L'x');
  const DriveResult on_b = TypeKeyAndDrive(*env.coordinator, *env.host,
                                           *harness.executor, x.event, x.text,
                                           "compose-readonly: B types");
  CheckOutcome(on_b.final_state, WorkOutcome::kApplied,
               "compose-readonly: B's input resumed after the blocked cleanup");
  CheckDocText(doc_b->text_utf16(), L"x",
               "compose-readonly: B holds its own text");
  Check(env.coordinator->Snapshot().retirement.last_cleanup_state ==
            CleanupState::kBlocked,
        "compose-readonly: the block is not cleared by later input", "");
  return Finish();
}

int RunObservedComposeRetirementUncertain(const Options& options) {
  RealHarness harness;
  if (!CreateRealHarness(options, "observed-compose-uncertain", &harness)) {
    return Finish();
  }
  ControlledObservedEnv env;
  if (!WireObserved(&env, TestDocumentOptions{L"", 0, 0},
                    "observed-compose-uncertain")) {
    return Finish();
  }
  TestDocument* doc_b = env.host->CreateDocument(TestDocumentOptions{}, &env.error);
  Check(doc_b != nullptr, "compose-uncertain: document B created", env.error);
  if (doc_b == nullptr) {
    return Finish();
  }
  if (!FinishWiring(&env, *harness.executor, "epocanva_test_english", 30000,
                    "observed-compose-uncertain")) {
    return Finish();
  }
  env.doc_a->store()->SetMutationFault(MutationFault::kEUnexpectedAfterMutation);
  {
    const ex::TextKey h = Letter(L'h');
    const DriveResult driven = TypeKeyAndDrive(
        *env.coordinator, *env.host, *harness.executor, h.event, h.text,
        "compose-uncertain: unconfirmed write");
    CheckOutcome(driven.final_state, WorkOutcome::kUncertain,
                 "compose-uncertain: the write left the context Uncertain");
  }
  env.doc_a->store()->SetMutationFault(MutationFault::kNone);
  const auto before = env.doc_a->diagnostics().store;

  oct::FocusCallResult focus;
  Check(oct::SetThreadFocus(*env.host, doc_b, &focus, &env.error),
        "compose-uncertain: real SetFocus moved to B", env.error);
  const oct::ObserverDriveResult rebound =
      oct::PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(rebound.settled && env.coordinator->bound(),
        "compose-uncertain: the observer bound B",
        oct::DescribeObserverSnapshot(rebound.final_state));
  PumpRetirements(&env, *harness.executor, "compose-uncertain");

  const CoordinatorSnapshot retired = env.coordinator->Snapshot();
  Check(retired.retirement.last_cleanup_state == CleanupState::kBlocked &&
            retired.retirement.last_cleanup_block ==
                CleanupBlockReason::kUncertainTextState,
        "compose-uncertain: cleanup is Blocked on the uncertain text state",
        DescribeSnapshot(retired));
  const auto after = env.doc_a->diagnostics().store;
  Check(after.text_mutations == before.text_mutations &&
            after.set_text_calls == before.set_text_calls,
        "compose-uncertain: no cleanup call touched the unknown text",
        "mutations=" + std::to_string(after.text_mutations) +
            " set_text=" + std::to_string(after.set_text_calls));
  Check(after.request_lock_calls == before.request_lock_calls,
        "compose-uncertain: no cleanup edit session was even requested",
        std::to_string(after.request_lock_calls - before.request_lock_calls));
  Check(env.registry->IsUncertain(env.doc_a->context()),
        "compose-uncertain: the real-context latch is still held", "");

  const ex::TextKey x = Letter(L'x');
  const DriveResult on_b = TypeKeyAndDrive(*env.coordinator, *env.host,
                                           *harness.executor, x.event, x.text,
                                           "compose-uncertain: B types");
  CheckOutcome(on_b.final_state, WorkOutcome::kApplied,
               "compose-uncertain: B's input resumed on the independent document");
  CheckDocText(doc_b->text_utf16(), L"x",
               "compose-uncertain: only B received the key");
  return Finish();
}

int RunObservedComposeExternalEdit(const Options& options) {
  RealHarness harness;
  if (!CreateRealHarness(options, "observed-compose-external", &harness)) {
    return Finish();
  }
  ControlledObservedEnv env;
  if (!WireObserved(&env, TestDocumentOptions{L"", 0, 0},
                    "observed-compose-external")) {
    return Finish();
  }
  if (!FinishWiring(&env, *harness.executor, "epocanva_test_english", 30000,
                    "observed-compose-external")) {
    return Finish();
  }
  for (wchar_t letter : {L'h', L'e', L'l'}) {
    const ex::TextKey key = Letter(letter);
    TypeKeyAndDrive(*env.coordinator, *env.host, *harness.executor, key.event,
                    key.text, "compose-external: letter");
  }
  CheckDocText(env.doc_a->text_utf16(), L"hel",
               "compose-external: the composition holds hel");

  oct::ExternalEditResult edit;
  Check(oct::RunExternalEdit(*env.host, env.doc_a, env.external,
                             oct::ExternalEditMode::kInsertText, 0, 0, L"X",
                             &edit, &env.error),
        "compose-external: the real external insert ran", env.error);
  const auto after_external = env.doc_a->diagnostics().store;
  CheckDocText(env.doc_a->text_utf16(), L"Xhel",
               "compose-external: the external text is next to the composition");

  const oct::ObserverDriveResult rebound =
      oct::PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(rebound.settled && env.coordinator->bound(),
        "compose-external: the observer re-bound the same document",
        oct::DescribeObserverSnapshot(rebound.final_state));
  PumpRetirements(&env, *harness.executor, "compose-external");

  const auto after_retirement = env.doc_a->diagnostics().store;
  Check(after_retirement.text_mutations == after_external.text_mutations &&
            after_retirement.set_text_calls == after_external.set_text_calls,
        "compose-external: retirement did not delete or rewrite any text",
        "mutations=" + std::to_string(after_retirement.text_mutations) +
            " set_text=" + std::to_string(after_retirement.set_text_calls));
  CheckDocText(env.doc_a->text_utf16(), L"Xhel",
               "compose-external: the existing text survived verbatim");
  const CoordinatorSnapshot retired = env.coordinator->Snapshot();
  Check(retired.retirement.last_cleanup_state == CleanupState::kFinished ||
            retired.retirement.last_cleanup_state ==
                CleanupState::kAlreadyOrdinary ||
            retired.retirement.last_cleanup_state == CleanupState::kBlocked,
        "compose-external: cleanup is explicit (Finished/AlreadyOrdinary/Blocked)",
        DescribeSnapshot(retired));
  if (retired.retirement.last_cleanup_state != CleanupState::kBlocked) {
    ULONG compositions = 99;
    Check(SUCCEEDED(oct::CountCompositions(env.doc_a->context(), &compositions)) &&
              compositions == 0,
          "compose-external: the old composition ended without touching the text",
          std::to_string(compositions));
  }
  const ex::TextKey i = Letter(L'i');
  const DriveResult typed = TypeKeyAndDrive(*env.coordinator, *env.host,
                                            *harness.executor, i.event, i.text,
                                            "compose-external: new composition");
  CheckOutcome(typed.final_state, WorkOutcome::kApplied,
               "compose-external: the new binding types after the external edit");
  CheckDocText(env.doc_a->text_utf16(), L"Xheli",
               "compose-external: the new preedit followed the preserved text");
  return Finish();
}

int RunObservedExternalSelection() {
  ControlledObservedEnv env;
  TestDocumentOptions options;
  options.initial_text_utf16 = L"abcdef";
  options.initial_selection_start_utf16 = 2;
  options.initial_selection_end_utf16 = 2;
  if (!SetupControlledObserved(&env, options, "observed-sel")) {
    return Finish();
  }
  const edit::ContextIdentity first = env.coordinator->identity();
  const std::uint64_t first_client = env.clients->created().back();
  oct::ExternalEditResult edit;
  Check(oct::RunExternalEdit(*env.host, env.doc_a, env.external,
                             oct::ExternalEditMode::kSetCollapsedSelection, 5, 5, L"",
                             &edit, &env.error),
        "observed-sel: the real external selection ran", env.error);
  Check(!env.coordinator->bound(),
        "observed-sel: the old binding was invalidated immediately", "");
  Check(env.controller->Snapshot().target_losses == 1 &&
            env.controller->Snapshot().last_loss_reason ==
                ctx::ContextDropReason::kExternalSelectionEdit,
        "observed-sel: the loss reason is the external selection",
        ctx::DescribeContextDropReason(
            env.controller->Snapshot().last_loss_reason));
  Check(env.doc_a->diagnostics().store.text_mutations == 0,
        "observed-sel: the invalidation wrote no text", "");

  const oct::ObserverDriveResult rebound =
      oct::PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(rebound.settled && env.coordinator->bound(),
        "observed-sel: the observer re-bound the context",
        oct::DescribeObserverSnapshot(rebound.final_state));
  PumpRetirements(&env, *env.harness.executor, "observed-sel");
  const CoordinatorSnapshot after = env.coordinator->Snapshot();
  Check(after.identity.context_id == first.context_id &&
            after.identity.generation == first.generation + 1,
        "observed-sel: the same context with a new generation",
        std::to_string(after.identity.generation));
  Check(after.tracked_caret_utf16 == 5,
        "observed-sel: the new caret is the external position, not the old one",
        std::to_string(after.tracked_caret_utf16));
  Check(env.clients->created().size() == 2 &&
            env.clients->created().back() != first_client,
        "observed-sel: the rebind used a fresh engine client",
        std::to_string(env.clients->created().size()));

  const ex::TextKey h = Letter(L'h');
  const DriveResult typed = TypeKeyAndDrive(*env.coordinator, *env.host,
                                            *env.harness.executor, h.event, h.text,
                                            "observed-sel: h at the new caret");
  CheckOutcome(typed.final_state, WorkOutcome::kApplied,
               "observed-sel: the new binding types");
  CheckDocText(env.doc_a->text_utf16(), L"abcdehf",
               "observed-sel: the key landed at the external caret");
  Check(env.controller->Snapshot().target_binds == 2,
        "observed-sel: exactly one rebind", "");
  return Finish();
}

int RunObservedExternalText() {
  ControlledObservedEnv env;
  TestDocumentOptions options;
  options.initial_text_utf16 = L"abcdef";
  options.initial_selection_start_utf16 = 2;
  options.initial_selection_end_utf16 = 2;
  if (!SetupControlledObserved(&env, options, "observed-text")) {
    return Finish();
  }
  const edit::ContextIdentity first = env.coordinator->identity();
  const std::uint64_t first_client = env.clients->created().back();
  oct::ExternalEditResult edit;
  Check(oct::RunExternalEdit(*env.host, env.doc_a, env.external,
                             oct::ExternalEditMode::kInsertText, 0, 0, L"X", &edit,
                             &env.error),
        "observed-text: the real external write ran", env.error);
  Check(!env.coordinator->bound(),
        "observed-text: the old engine binding was invalidated", "");
  Check(env.controller->Snapshot().last_loss_reason ==
            ctx::ContextDropReason::kExternalTextEdit,
        "observed-text: the loss reason is the external text",
        ctx::DescribeContextDropReason(
            env.controller->Snapshot().last_loss_reason));
  Check(env.doc_a->text_utf16().size() == 7,
        "observed-text: the external write changed the text", "");
  const oct::ObserverDriveResult rebound =
      oct::PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(rebound.settled && env.coordinator->bound(),
        "observed-text: the observer re-bound", "");
  PumpRetirements(&env, *env.harness.executor, "observed-text");
  const CoordinatorSnapshot after = env.coordinator->Snapshot();
  Check(after.identity.generation == first.generation + 1,
        "observed-text: a new generation", "");
  Check(after.tracked_caret_utf16 == 3,
        "observed-text: the caret followed the external insert",
        std::to_string(after.tracked_caret_utf16));
  Check(env.clients->created().size() == 2 &&
            env.clients->created().back() != first_client,
        "observed-text: a fresh engine client, not the old composition", "");
  const ex::TextKey h = Letter(L'h');
  const DriveResult typed = TypeKeyAndDrive(*env.coordinator, *env.host,
                                            *env.harness.executor, h.event, h.text,
                                            "observed-text: h after the external edit");
  CheckOutcome(typed.final_state, WorkOutcome::kApplied,
               "observed-text: the new binding types");
  CheckDocText(env.doc_a->text_utf16(), L"Xabhcdef",
               "observed-text: the key landed at the new caret");
  return Finish();
}

int RunObservedLatePermission() {
  ControlledObservedEnv env;
  TestDocumentOptions options;
  options.initial_text_utf16 = L"abcdef";
  options.initial_selection_start_utf16 = 2;
  options.initial_selection_end_utf16 = 2;
  options.refuse_sync_locks = true;  // the permission query must go async
  if (!SetupControlledObserved(&env, options, "late-permission")) {
    return Finish();
  }
  const std::uint64_t dispatches_before = env.harness.backend->dispatch_calls();
  const ex::TextKey h = Letter(L'h');
  WorkId id;
  const SubmitStatus submitted = env.coordinator->SubmitKey(h.event, h.text, &id);
  Check(submitted == SubmitStatus::kAccepted,
        "late-permission: A's key was accepted into the pending query",
        DescribeSubmitStatus(submitted));
  Check(env.coordinator->Snapshot().phase == WorkPhase::kPermissionPending,
        "late-permission: A is waiting on the real read session",
        DescribeSnapshot(env.coordinator->Snapshot()));

  // The switch happens WITHOUT draining A's read session.
  TestDocument* child = env.host->CreateChildContext(
      env.doc_a, TestDocumentOptions{}, &env.error);
  Check(child != nullptr, "late-permission: child context pushed", env.error);
  if (child == nullptr) {
    return Finish();
  }
  const CoordinatorSnapshot during = env.coordinator->Snapshot();
  Check(!during.bound && during.outcome == WorkOutcome::kAbandonedStale &&
            during.phase == WorkPhase::kIdle,
        "late-permission: the switch terminalized A's work immediately",
        DescribeSnapshot(during));
  Check(env.permission->Diagnostics().queries_abandoned >= 1,
        "late-permission: A's read session was abandoned",
        std::to_string(env.permission->Diagnostics().queries_abandoned));

  const oct::ObserverDriveResult rebound =
      oct::PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(rebound.settled && env.coordinator->bound(),
        "late-permission: the observer bound the child context",
        oct::DescribeObserverSnapshot(rebound.final_state));
  const ContextIdRecord child_record = [&]() {
    ContextIdRecord record;
    env.registry->Observe(child->context(), &record);
    return record;
  }();
  Check(env.coordinator->Snapshot().identity.context_id == child_record.context_id,
        "late-permission: B is the bound context", "");
  Check(env.coordinator->Snapshot().permission == edit::InputPermission::kUnknown,
        "late-permission: B starts at the safe Unknown verdict", "");

  PumpRetirements(&env, *env.harness.executor, "late-permission");

  // Now let A's real read session complete, late.
  env.host->DispatchQueuedMessages();
  Check(env.harness.backend->dispatch_calls() == dispatches_before,
        "late-permission: A's late callback made no engine call",
        std::to_string(env.harness.backend->dispatch_calls()) + " vs " +
            std::to_string(dispatches_before));
  Check(env.doc_a->diagnostics().store.text_mutations == 0,
        "late-permission: A's document was not written", "");
  Check(env.permission->Diagnostics().queries_completed == 0,
        "late-permission: the abandoned query published no verdict",
        std::to_string(env.permission->Diagnostics().queries_completed));

  const ex::TextKey a = Letter(L'a');
  const DriveResult typed = TypeKeyAndDrive(*env.coordinator, *env.host,
                                            *env.harness.executor, a.event, a.text,
                                            "late-permission: B types");
  CheckOutcome(typed.final_state, WorkOutcome::kApplied,
               "late-permission: B's key applied");
  CheckDocText(child->text_utf16(), L"a",
               "late-permission: only B received the key");
  Check(env.harness.backend->dispatch_calls() == dispatches_before + 1,
        "late-permission: exactly one engine call, for B",
        std::to_string(env.harness.backend->dispatch_calls()));
  Check(env.coordinator->Snapshot().identity.context_id == child_record.context_id,
        "late-permission: A's late callback did not restore A's binding", "");
  return Finish();
}

// Dispatches messages until the document's edit lock is still pending AND its deferred
// grant has been parked in the host's hold list. This is an event barrier, not a fixed
// sleep: TSF may schedule the store request asynchronously after RequestEditSession, so a
// merely queued grant message is not enough evidence.
bool PumpUntilEditGrantParked(TestHost& host, TestDocument& document,
                              std::string* error) {
  const ULONGLONG deadline = ::GetTickCount64() + 20000;
  for (;;) {
    const bool pending =
        document.diagnostics().store.pending_async_lock;
    if (pending && host.held_grant_messages() >= 1) {
      return true;
    }
    MSG message = {};
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
      ::TranslateMessage(&message);
      ::DispatchMessageW(&message);
      if (document.diagnostics().store.pending_async_lock &&
          host.held_grant_messages() >= 1) {
        return true;
      }
    }
    if (::GetTickCount64() >= deadline) {
      if (error != nullptr) {
        *error = "edit grant not parked: pending=" +
                 std::to_string(
                     document.diagnostics().store.pending_async_lock ? 1 : 0) +
                 " held=" + std::to_string(host.held_grant_messages());
      }
      return false;
    }
    ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
  }
}

// Advances coordinator, controller and observer together until a current binding exists and
// the retirement ledger is terminal. Waits only on real completion facts, never on a sleep
// that guesses a result.
bool PumpAllUntilBound(ControlledObservedEnv* env, DWORD timeout_ms) {
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    env->coordinator->Pump();
    env->controller->Pump();
    env->observer->Pump();
    env->host->DispatchQueuedMessages();
    const CoordinatorSnapshot snapshot = env->coordinator->Snapshot();
    if (snapshot.bound && snapshot.retirement.retiring_bindings == 0) {
      return true;
    }
    if (::GetTickCount64() >= deadline) {
      return false;
    }
    if (snapshot.retirement.pending_edit_event != nullptr) {
      env->host->PumpUntil(snapshot.retirement.pending_edit_event, 20);
    } else if (snapshot.retirement.pending_close_request.valid()) {
      env->harness.executor->WaitForResult(
          snapshot.retirement.pending_close_request, std::chrono::milliseconds(20));
    } else if (snapshot.retirement.pending_engine_request.valid()) {
      env->harness.executor->WaitForResult(
          snapshot.retirement.pending_engine_request, std::chrono::milliseconds(20));
    } else {
      ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
  }
}

int RunObservedLateEdit() {
  ControlledObservedEnv env;
  TestDocumentOptions options;
  options.async_locks = true;  // the edit must be queued behind a deferred grant
  if (!SetupControlledObserved(&env, options, "late-edit")) {
    return Finish();
  }
  const ex::TextKey h = Letter(L'h');
  WorkId id;
  Check(env.coordinator->SubmitKey(h.event, h.text, &id) == SubmitStatus::kAccepted,
        "late-edit: A's key accepted", "");
  const ULONGLONG deadline = ::GetTickCount64() + 20000;
  CoordinatorSnapshot snapshot = env.coordinator->Snapshot();
  while (snapshot.phase != WorkPhase::kEditPending) {
    env.coordinator->Pump();
    snapshot = env.coordinator->Snapshot();
    if (snapshot.phase == WorkPhase::kIdle || ::GetTickCount64() >= deadline) {
      break;
    }
    env.harness.executor->WaitForResult(snapshot.engine_request,
                                        std::chrono::milliseconds(50));
  }
  Check(snapshot.phase == WorkPhase::kEditPending,
        "late-edit: A's edit is queued behind the deferred lock",
        DescribeSnapshot(snapshot));
  Check(env.doc_a->diagnostics().store.text_mutations == 0,
        "late-edit: nothing was written before the grant", "");
  // Let TSF start A's edit session (its store RequestLock) and dispatch the deferred
  // grant message, but HOLD the grant. This fixes the order: the request exists, the lock
  // is still pending, and the switch happens before any callback can run.
  env.host->SetHoldGrantMessages(true);
  Check(PumpUntilEditGrantParked(*env.host, *env.doc_a, &env.error),
        "late-edit: the deferred grant message was parked without pumping focus",
        env.error);
  const auto held = env.doc_a->diagnostics().store;
  Check(held.request_lock_calls >= 4 && held.pending_async_lock &&
            held.write_lock_grants == 0,
        "late-edit: the store request is pending and its grant is held",
        "req=" + std::to_string(held.request_lock_calls) +
            " pending=" + std::to_string(held.pending_async_lock ? 1 : 0) +
            " write=" + std::to_string(held.write_lock_grants));

  // Switch to a different document (a different document manager) without draining A's
  // deferred grant. A Push of a child context on A's own manager would serialize B's
  // edit session behind A's stuck one at the manager level, which is a real TSF scheduling
  // effect but would make "B types while A's callback is late" impossible to observe.
  TestDocument* doc_b = env.host->CreateDocument(TestDocumentOptions{}, &env.error);
  Check(doc_b != nullptr, "late-edit: document B created", env.error);
  if (doc_b == nullptr) {
    return Finish();
  }
  oct::FocusCallResult focus;
  Check(oct::SetThreadFocus(*env.host, doc_b, &focus, &env.error),
        "late-edit: real SetFocus moved to B", env.error);
  {
    const auto after_focus = env.doc_a->diagnostics().store;
    std::printf(
        "[coordinator-win] evidence late-edit: after SetFocus A{req=%llu write=%llu "
        "read=%llu async=%llu pending=%d held=%d}\n",
        static_cast<unsigned long long>(after_focus.request_lock_calls),
        static_cast<unsigned long long>(after_focus.write_lock_grants),
        static_cast<unsigned long long>(after_focus.read_lock_grants),
        static_cast<unsigned long long>(after_focus.async_lock_grants),
        after_focus.pending_async_lock ? 1 : 0, after_focus.lock_held ? 1 : 0);
  }
  const oct::ObserverDriveResult rebound =
      oct::PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(rebound.settled && env.coordinator->bound(),
        "late-edit: the observer switched the binding to B",
        oct::DescribeObserverSnapshot(rebound.final_state));
  ContextIdRecord child_record;
  env.registry->Observe(doc_b->context(), &child_record);
  Check(env.coordinator->bound() &&
            env.coordinator->Snapshot().identity.context_id ==
                child_record.context_id,
        "late-edit: the new binding is document B", "");
  Check(env.controller->Snapshot().invalidations >= 1 &&
            env.controller->Snapshot().last_loss_reason ==
                ctx::ContextDropReason::kFocusMoved,
        "late-edit: A's old binding was invalidated before B was bound",
        ctx::DescribeContextDropReason(
            env.controller->Snapshot().last_loss_reason));
  const ctx::ContextObserverSnapshot after_switch = env.observer->Snapshot();
  Check(after_switch.focus_revalidation_changes >= 1 ||
            after_switch.focus_notifications >= 1,
        "late-edit: the switch was observed by a real notification or GetFocus revalidation",
        "revalidations=" +
            std::to_string(after_switch.focus_revalidation_changes) +
            " focus_notifications=" +
            std::to_string(after_switch.focus_notifications));
  Check(env.doc_a->diagnostics().store.write_lock_grants == 0,
        "late-edit: A's write lock is still ungranted after the switch", "");
  const ex::TextKey a = Letter(L'a');
  WorkId paused_id;
  const SubmitStatus paused = env.coordinator->SubmitKey(a.event, a.text, &paused_id);
  Check(paused == SubmitStatus::kRetiring,
        "late-edit: B's input is explicitly paused while A's binding retires",
        DescribeSubmitStatus(paused));
  Check(!paused_id.valid(), "late-edit: the paused key created no work item", "");
  PumpRetirements(&env, *env.harness.executor, "late-edit");
  // A focus notification may have arrived during retirement waiting. Recover the CURRENT
  // target before typing; no old key is replayed.
  Check(PumpAllUntilBound(&env, 30000),
        "late-edit: the current target is bound before B types",
        DescribeSnapshot(env.coordinator->Snapshot()));
  ContextIdRecord b_record;
  Check(env.registry->Observe(doc_b->context(), &b_record) ==
                ctx::ContextRegistryStatus::kOk,
        "late-edit: B's identity is readable", "");
  Check(env.coordinator->Snapshot().identity.context_id == b_record.context_id,
        "late-edit: the recovered binding is B", "");

  const auto before_b = doc_b->diagnostics().store;
  std::printf(
      "[coordinator-win] evidence late-edit: before B type "
      "A{req=%llu write=%llu mutations=%llu} B{req=%llu write=%llu async=%llu}\n",
      static_cast<unsigned long long>(
          env.doc_a->diagnostics().store.request_lock_calls),
      static_cast<unsigned long long>(
          env.doc_a->diagnostics().store.write_lock_grants),
      static_cast<unsigned long long>(
          env.doc_a->diagnostics().store.text_mutations),
      static_cast<unsigned long long>(before_b.request_lock_calls),
      static_cast<unsigned long long>(before_b.write_lock_grants),
      static_cast<unsigned long long>(before_b.async_lock_grants));
  const DriveResult typed = TypeKeyAndDrive(*env.coordinator, *env.host,
                                            *env.harness.executor, a.event, a.text,
                                            "late-edit: B types");
  const auto after_a = env.doc_a->diagnostics().store;
  const auto after_b = doc_b->diagnostics().store;
  std::printf(
      "[coordinator-win] evidence late-edit: after B drive "
      "A{req=%llu write=%llu mutations=%llu} B{req=%llu write=%llu async=%llu} "
      "phase=%s\n",
      static_cast<unsigned long long>(after_a.request_lock_calls),
      static_cast<unsigned long long>(after_a.write_lock_grants),
      static_cast<unsigned long long>(after_a.text_mutations),
      static_cast<unsigned long long>(after_b.request_lock_calls),
      static_cast<unsigned long long>(after_b.write_lock_grants),
      static_cast<unsigned long long>(after_b.async_lock_grants),
      DescribeWorkPhase(typed.final_state.phase));
  CheckOutcome(typed.final_state, WorkOutcome::kApplied,
               "late-edit: B's key applied");
  CheckDocText(doc_b->text_utf16(), L"a",
               "late-edit: B holds the key");
  const std::uint64_t a_mutations = env.doc_a->diagnostics().store.text_mutations;
  Check(a_mutations == 0, "late-edit: A was untouched before its late grant", "");
  const std::uint64_t callback_checks_before_late =
      env.coordinator->Snapshot().target_callback_checks;
  const std::uint64_t rejections_before_late =
      env.coordinator->Snapshot().target_rejections;
  const std::uint64_t observer_guards_before_late =
      env.observer->Snapshot().target_revalidations;

  // Deliver A's held grant now, after B is bound and has processed its key. This is path A:
  // the binding was invalidated before the late edit callback, so that callback must be
  // stale and must not run the target guard.
  env.host->SetHoldGrantMessages(false);
  const std::size_t granted = env.host->ReleaseHeldGrantMessages();
  Check(granted >= 1, "late-edit: A's held grant was released late",
        std::to_string(granted));
  Check(env.doc_a->diagnostics().store.text_mutations == 0,
        "late-edit: A's late callback wrote nothing",
        std::to_string(env.doc_a->diagnostics().store.text_mutations));
  Check(env.coordinator->Snapshot().target_callback_checks ==
                callback_checks_before_late &&
            env.coordinator->Snapshot().target_rejections ==
                rejections_before_late,
        "late-edit (path A): the retired session ran no target guard",
        "callback=" +
            std::to_string(env.coordinator->Snapshot().target_callback_checks));
  Check(env.observer->Snapshot().target_revalidations ==
            observer_guards_before_late,
        "late-edit (path A): the late callback made no live target query", "");
  CheckDocText(doc_b->text_utf16(), L"a",
               "late-edit: A's late callback did not touch B");
  Check(PumpAllUntilBound(&env, 30000),
        "late-edit: a current target is bound after the late callback",
        DescribeSnapshot(env.coordinator->Snapshot()));
  Check(env.coordinator->Snapshot().identity.context_id == b_record.context_id,
        "late-edit: A's late callback did not restore A's binding", "");
  return Finish();
}

int RunObservedUncertainAcrossCoordinator() {
  ControlledObservedEnv env;
  if (!SetupControlledObserved(&env, TestDocumentOptions{L"", 0, 0}, "uncertain-swap")) {
    return Finish();
  }
  env.doc_a->store()->SetMutationFault(MutationFault::kEUnexpectedAfterMutation);
  const ex::TextKey h = Letter(L'h');
  const DriveResult driven = TypeKeyAndDrive(*env.coordinator, *env.host,
                                             *env.harness.executor, h.event, h.text,
                                             "uncertain-swap: unconfirmed write");
  CheckOutcome(driven.final_state, WorkOutcome::kUncertain,
               "uncertain-swap: the write left the context Uncertain");
  Check(env.registry->IsUncertain(env.doc_a->context()),
        "uncertain-swap: the registry latched the REAL context", "");
  Check(driven.final_state.gate == CoordinatorGate::kBlockedUncertain,
        "uncertain-swap: the coordinator gate is blocked", "");
  ContextIdRecord a_record;
  env.registry->Observe(env.doc_a->context(), &a_record);

  // Replace the coordinator, controller, provider and permission source; keep the observer
  // and the registry alive.
  env.controller.reset();
  env.coordinator.reset();
  env.clients.reset();
  env.permission.reset();
  env.permission = ctx::CreateRealPermissionSource(env.host->client_id());
  env.coordinator = std::make_unique<ContextCoordinator>(
      *env.harness.executor, env.host->client_id(), *env.permission);
  env.clients = std::make_unique<ExecutorClientProvider>(
      *env.harness.executor, "controlled",
      epocanva::windows::engine::ControllerOptions{
          true, epocanva::windows::input::InputPermission::kAllowed},
      10000);
  env.controller = std::make_unique<ObservedBindingController>(
      *env.observer, *env.coordinator, *env.registry, *env.clients);
  Check(!env.coordinator->bound(),
        "uncertain-swap: the replacement starts unbound", "");
  Check(env.registry->IsUncertain(env.doc_a->context()),
        "uncertain-swap: the latch survived the coordinator replacement", "");

  // A real target change: push a different context (allowed), then pop back to A (blocked).
  TestDocument* child = env.host->CreateChildContext(
      env.doc_a, TestDocumentOptions{L"other", 0, 0}, &env.error);
  Check(child != nullptr, "uncertain-swap: alternate context pushed", env.error);
  if (child == nullptr) {
    return Finish();
  }
  oct::ObserverDriveResult rebound =
      oct::PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(rebound.settled && env.coordinator->bound(),
        "uncertain-swap: a different context is still bindable",
        oct::DescribeObserverSnapshot(rebound.final_state));
  Check(child->Disconnect(), "uncertain-swap: alternate context popped", "");
  rebound = oct::PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(rebound.settled && !env.coordinator->bound(),
        "uncertain-swap: the latched context is not re-bound",
        oct::DescribeObserverSnapshot(rebound.final_state));
  Check(env.observer->Snapshot().last_refusal_reason ==
            ctx::ContextDropReason::kUncertainBlocked,
        "uncertain-swap: the observer reports the blocked target",
        ctx::DescribeContextDropReason(
            env.observer->Snapshot().last_refusal_reason));

  // Direct calls with the real context: the registry, not the integer, decides.
  engine::ClientId fresh = env.clients->NextClient();
  Check(fresh.valid(), "uncertain-swap: a fresh client for the direct probe", "");
  Check(env.coordinator->BindObserved(env.doc_a->context(), a_record.context_id,
                                      a_record.generation == 0 ? 1 : a_record.generation,
                                      0, fresh) == BindStatus::kBlockedUncertain,
        "uncertain-swap: BindObserved refuses the latched context", "");
  fresh = env.clients->NextClient();
  Check(fresh.valid(), "uncertain-swap: a fresh client for the id probe", "");
  Check(env.coordinator->Bind(env.doc_a->context(), 9999, 0, fresh) ==
            BindStatus::kBlockedUncertain,
        "uncertain-swap: a different integer id does not bypass the latch", "");

  // A different document remains fully usable.
  TestDocument* other =
      env.host->CreateDocument(TestDocumentOptions{}, &env.error);
  Check(other != nullptr, "uncertain-swap: independent document created", env.error);
  if (other != nullptr) {
    oct::FocusCallResult focus;
    Check(oct::SetThreadFocus(*env.host, other, &focus, &env.error),
          "uncertain-swap: focus moved to the independent document", env.error);
    rebound = oct::PumpUntilSettled(*env.observer, *env.host, 20000);
    Check(rebound.settled && env.coordinator->bound(),
          "uncertain-swap: the independent document binds", "");
    PumpRetirements(&env, *env.harness.executor, "uncertain-swap: independent");
    const ex::TextKey a = Letter(L'a');
    const DriveResult typed = TypeKeyAndDrive(*env.coordinator, *env.host,
                                              *env.harness.executor, a.event,
                                              a.text, "uncertain-swap: other types");
    CheckOutcome(typed.final_state, WorkOutcome::kApplied,
                 "uncertain-swap: the independent document types");
    CheckDocText(other->text_utf16(), L"a",
                 "uncertain-swap: only the independent document changed");
  }
  Check(env.registry->IsUncertain(env.doc_a->context()),
        "uncertain-swap: the latch is still held after other work", "");
  return Finish();
}

int RunObservedPermissionRequery() {
  ControlledObservedEnv env;
  TestDocumentOptions options;
  options.initial_text_utf16 = L"abcdef";
  options.initial_selection_start_utf16 = 2;
  options.initial_selection_end_utf16 = 2;
  if (!SetupControlledObserved(&env, options, "permission-requery")) {
    return Finish();
  }
  const ex::TextKey h = Letter(L'h');
  const DriveResult first = TypeKeyAndDrive(*env.coordinator, *env.host,
                                            *env.harness.executor, h.event, h.text,
                                            "permission-requery: first key");
  CheckOutcome(first.final_state, WorkOutcome::kApplied,
               "permission-requery: the first key applied");
  Check(env.coordinator->Snapshot().permission == edit::InputPermission::kAllowed,
        "permission-requery: the real metadata verdict was Allowed", "");

  oct::ExternalEditResult edit;
  Check(oct::RunExternalEdit(*env.host, env.doc_a, env.external,
                             oct::ExternalEditMode::kInsertText, 0, 0, L"X", &edit,
                             &env.error),
        "permission-requery: external write invalidates", env.error);
  const oct::ObserverDriveResult rebound =
      oct::PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(rebound.settled && env.coordinator->bound(),
        "permission-requery: the observer re-bound", "");
  PumpRetirements(&env, *env.harness.executor, "permission-requery");
  Check(env.coordinator->Snapshot().permission == edit::InputPermission::kUnknown,
        "permission-requery: the new binding did not carry the old Allowed verdict",
        edit::DescribeInputPermission(env.coordinator->Snapshot().permission));

  env.doc_a->SetReadOnly(true);
  const std::uint64_t dispatches_before = env.harness.backend->dispatch_calls();
  const std::uint64_t mutations_before =
      env.doc_a->diagnostics().store.text_mutations;
  WorkId id;
  const SubmitStatus refused = env.coordinator->SubmitKey(h.event, h.text, &id);
  Check(refused == SubmitStatus::kPermissionDenied,
        "permission-requery: the fresh real query refuses the read-only field",
        DescribeSubmitStatus(refused));
  Check(env.harness.backend->dispatch_calls() == dispatches_before,
        "permission-requery: no engine call for the refused key", "");
  Check(env.doc_a->diagnostics().store.text_mutations == mutations_before,
        "permission-requery: no text mutation for the refused key", "");
  return Finish();
}

}  // namespace

// =======================================================================================
// P3.13 target-admission cases. Every binding here is created by the real observer; the
// live guard is the production ContextObserver. The hidden off-screen window supplies a
// real Win32 thread-focus fact (labelled win32-thread-focus-revalidation), not a
// synthesized TSF notification.
// =======================================================================================

bool CreateDocumentB(ControlledObservedEnv* env, TestDocument** document,
                     const char* label) {
  *document = env->host->CreateDocument(TestDocumentOptions{}, &env->error);
  Check(*document != nullptr, std::string(label) + ": document B created",
        env->error);
  return *document != nullptr;
}

// P3.13-R2 path A, real TSF notification: OnPushContext / OnPopContext invalidate the
// binding through the observer before any submit or edit guard can run. The in-flight
// engine result is consumed once and discarded, the old client is closed once, and no old
// key or result is replayed.
int RunFocusNotificationBeforeGuard() {
  ControlledObservedEnv env;
  if (!SetupControlledObserved(&env, TestDocumentOptions{L"", 0, 0},
                               "focus-notify-first")) {
    return Finish();
  }
  ContextIdRecord a_first;
  Check(env.registry->Observe(env.doc_a->context(), &a_first) ==
                ctx::ContextRegistryStatus::kOk,
        "focus-notify-first: A's initial identity is readable", "");
  Check(env.coordinator->Snapshot().identity.context_id == a_first.context_id,
        "focus-notify-first: the initial binding is A", "");

  // A real PushContext notification. No guard is involved here; this establishes the child
  // target and proves the notification path itself invalidates the old binding.
  const std::uint64_t guard_before_push =
      env.coordinator->Snapshot().target_revalidations;
  const std::uint64_t losses_before_push =
      env.controller->Snapshot().target_losses;
  TestDocument* child = env.host->CreateChildContext(
      env.doc_a, TestDocumentOptions{L"child", 0, 0}, &env.error);
  Check(child != nullptr, "focus-notify-first: child context pushed", env.error);
  if (child == nullptr) {
    return Finish();
  }
  const ctx::ContextObserverSnapshot observer_after_push =
      env.observer->Snapshot();
  const auto controller_after_push = env.controller->Snapshot();
  const CoordinatorSnapshot coordinator_after_push =
      env.coordinator->Snapshot();
  Check(observer_after_push.push_notifications >= 1,
        "focus-notify-first: OnPushContext was a real TSF delivery",
        std::to_string(observer_after_push.push_notifications));
  Check(controller_after_push.target_losses == losses_before_push + 1 &&
            controller_after_push.last_loss_reason ==
                ctx::ContextDropReason::kTopContextChanged,
        "focus-notify-first: the notification invalidated the published target",
        ctx::DescribeContextDropReason(
            controller_after_push.last_loss_reason));
  Check(!coordinator_after_push.bound &&
            coordinator_after_push.target_revalidations == guard_before_push,
        "focus-notify-first: invalidation happened before any target guard", "");
  Check(coordinator_after_push.retirement.retiring_bindings == 1,
        "focus-notify-first: the old binding moved to the retirement ledger",
        DescribeSnapshot(coordinator_after_push));
  Check(env.doc_a->diagnostics().store.text_mutations == 0,
        "focus-notify-first: the notification path wrote nothing", "");

  Check(PumpAllUntilBound(&env, 30000),
        "focus-notify-first: the controller bound the current child target",
        DescribeSnapshot(env.coordinator->Snapshot()));
  ContextIdRecord child_record;
  Check(env.registry->Observe(child->context(), &child_record) ==
                ctx::ContextRegistryStatus::kOk,
        "focus-notify-first: the child identity is readable", "");
  Check(env.coordinator->Snapshot().identity.context_id ==
            child_record.context_id &&
            env.coordinator->Snapshot().identity.generation ==
                child_record.generation,
        "focus-notify-first: the binding is the child with its own generation",
        "bound=" +
            std::to_string(env.coordinator->Snapshot().identity.context_id) +
            " child=" + std::to_string(child_record.context_id));
  Check(child_record.context_id != a_first.context_id,
        "focus-notify-first: the target identity really changed", "");

  // Now put one engine dispatch in flight on the child, then deliver a real OnPopContext.
  env.harness.backend->HoldDispatches();
  const ex::TextKey h = Letter(L'h');
  WorkId old_id;
  Check(env.coordinator->SubmitKey(h.event, h.text, &old_id) ==
            SubmitStatus::kAccepted,
        "focus-notify-first: the old key was accepted on the child", "");
  Check(env.coordinator->Snapshot().phase == WorkPhase::kEnginePending &&
            env.coordinator->Snapshot().engine_request.valid(),
        "focus-notify-first: the engine dispatch is pending", "");
  const std::uint64_t dispatches_before =
      env.harness.backend->dispatch_calls();
  const std::uint64_t guard_before_pop =
      env.coordinator->Snapshot().target_revalidations;
  const std::uint64_t works_before_pop =
      env.coordinator->Snapshot().works_accepted;
  const std::uint64_t losses_before_pop =
      env.controller->Snapshot().target_losses;
  const std::uint64_t drops_before_pop = env.observer->Snapshot().target_drops;
  const RetirementSnapshot retirement_before =
      env.coordinator->Snapshot().retirement;
  const std::uint64_t close_calls_before = env.harness.backend->close_calls();

  Check(child->Disconnect(), "focus-notify-first: child popped", "");
  const ctx::ContextObserverSnapshot observer_after_pop = env.observer->Snapshot();
  const auto controller_after_pop = env.controller->Snapshot();
  const CoordinatorSnapshot coordinator_after_pop = env.coordinator->Snapshot();
  Check(observer_after_pop.pop_notifications >= 1,
        "focus-notify-first: OnPopContext was a real TSF delivery",
        std::to_string(observer_after_pop.pop_notifications));
  Check(controller_after_pop.target_losses == losses_before_pop + 1 &&
            controller_after_pop.invalidations >= 1,
        "focus-notify-first: the pop handler invalidated the child binding", "");
  Check(env.observer->Snapshot().target_drops == drops_before_pop + 1,
        "focus-notify-first: the published target was dropped at the notification", "");
  Check(!coordinator_after_pop.bound,
        "focus-notify-first: the binding is gone before any guard could run", "");
  Check(coordinator_after_pop.outcome == WorkOutcome::kAbandonedStale &&
            coordinator_after_pop.phase == WorkPhase::kIdle,
        "focus-notify-first: the queued work reached its notification-path terminal",
        DescribeSnapshot(coordinator_after_pop));
  Check(coordinator_after_pop.retirement.retiring_bindings ==
            retirement_before.retiring_bindings + 1 &&
            coordinator_after_pop.retirement.pending_engine_request.valid(),
        "focus-notify-first: the engine result moved with the retired binding",
        DescribeSnapshot(coordinator_after_pop));
  Check(coordinator_after_pop.target_revalidations == guard_before_pop,
        "focus-notify-first: no submit/edit guard ran on the notification path", "");
  Check(env.harness.backend->dispatch_calls() == dispatches_before,
        "focus-notify-first: the notification made no new engine call", "");

  // Ordinary input while unbound. It must not create work, call the guard, dispatch to the
  // engine, or write to either context.
  WorkId refused_id;
  const SubmitStatus refused =
      env.coordinator->SubmitKey(h.event, h.text, &refused_id);
  Check(refused == SubmitStatus::kNotBound && !refused_id.valid(),
        "focus-notify-first: input while unbound creates no work",
        DescribeSubmitStatus(refused));
  Check(env.coordinator->Snapshot().target_revalidations == guard_before_pop,
        "focus-notify-first: the unbound submit did not run a guard", "");
  Check(env.coordinator->Snapshot().works_accepted == works_before_pop,
        "focus-notify-first: the refused key was not accepted or replayed", "");
  Check(env.harness.backend->dispatch_calls() == dispatches_before,
        "focus-notify-first: the refused key made no engine call", "");
  Check(env.doc_a->diagnostics().store.text_mutations == 0,
        "focus-notify-first: no document write while the old binding retired", "");

  // Release the held dispatch and drain the retirement facts.
  env.harness.backend->ReleaseDispatches();
  const DriveResult drained = DriveToCompletion(
      *env.coordinator, *env.host, *env.harness.executor, 30000);
  Check(drained.completed &&
            drained.final_state.retirement.retiring_bindings == 0,
        "focus-notify-first: the retirement ledger is terminal",
        DescribeSnapshot(drained.final_state));
  Check(drained.final_state.retirement.engine_results_discarded ==
            retirement_before.engine_results_discarded + 1,
        "focus-notify-first: the old engine result was consumed exactly once",
        "discarded=" +
            std::to_string(
                drained.final_state.retirement.engine_results_discarded) +
            " before=" +
            std::to_string(retirement_before.engine_results_discarded));
  Check(drained.final_state.retirement.close_results_taken ==
            retirement_before.close_results_taken + 1 &&
            drained.final_state.retirement.close_requests ==
                retirement_before.close_requests + 1,
        "focus-notify-first: the old client was closed and its result consumed",
        "close=" +
            std::to_string(drained.final_state.retirement.close_requests) + "/" +
            std::to_string(
                drained.final_state.retirement.close_results_taken));
  Check(drained.final_state.retirement.unresolved_engine_results == 0 &&
            drained.final_state.retirement.unresolved_close_requests == 0,
        "focus-notify-first: no retirement fact was left unresolved", "");
  Check(env.harness.backend->close_calls() == close_calls_before + 1,
        "focus-notify-first: exactly one old client was closed",
        std::to_string(env.harness.backend->close_calls()) + " vs " +
            std::to_string(close_calls_before));

  // The parent context is the current target again. It gets a NEW generation; the popped
  // child binding is never reused.
  Check(PumpAllUntilBound(&env, 30000),
        "focus-notify-first: a fresh binding was created for the parent",
        DescribeSnapshot(env.coordinator->Snapshot()));
  ContextIdRecord a_again;
  Check(env.registry->Observe(env.doc_a->context(), &a_again) ==
                ctx::ContextRegistryStatus::kOk,
        "focus-notify-first: the parent identity is readable again", "");
  Check(a_again.context_id == a_first.context_id &&
            a_again.generation > a_first.generation &&
            env.coordinator->Snapshot().identity.generation ==
                a_again.generation,
        "focus-notify-first: the parent was rebound with a newer generation",
        "gen=" + std::to_string(a_again.generation) + " first=" +
            std::to_string(a_first.generation));
  CheckDocText(env.doc_a->text_utf16(), L"",
               "focus-notify-first: the old key was not replayed to the parent");
  Check(env.doc_a->diagnostics().store.text_mutations == 0,
        "focus-notify-first: no write happened before fresh input", "");

  const ex::TextKey a = Letter(L'a');
  const DriveResult typed = TypeKeyAndDrive(*env.coordinator, *env.host,
                                            *env.harness.executor, a.event,
                                            a.text, "focus-notify-first: fresh");
  CheckOutcome(typed.final_state, WorkOutcome::kApplied,
               "focus-notify-first: fresh input on the new binding applies");
  CheckDocText(env.doc_a->text_utf16(), L"a",
               "focus-notify-first: only the fresh key reached the parent");
  return Finish();
}

// P3.13-R2 path B with a controlled notification gate. A real SetFocus moves the TSF
// target; if the environment also delivers OnSetFocus, the test-side gate buffers it so the
// coordinator binding is not invalidated before the guard. The guard still reads the real
// GetFocus/GetTop and refuses the mismatched target. This is controlled ordering, not a
// real-notification verification: the observer counters say whether a notification arrived.
int RunFocusGuardRevalidationBeforeNotification() {
  ControlledObservedEnv env;
  if (!SetupControlledObserved(&env, TestDocumentOptions{L"", 0, 0},
                               "focus-revalidate-before-notify",
                               ctx::ThreadFocusPolicy::kLabWin32Fallback,
                               /*defer_losses=*/true)) {
    return Finish();
  }
  TestDocument* doc_b = nullptr;
  if (!CreateDocumentB(&env, &doc_b, "focus-revalidate-before-notify")) {
    return Finish();
  }
  const CoordinatorSnapshot before = env.coordinator->Snapshot();
  const std::uint64_t losses_before =
      env.controller->Snapshot().target_losses;
  const std::uint64_t invalidations_before =
      env.controller->Snapshot().invalidations;
  const std::uint64_t focus_notifications_before =
      env.observer->Snapshot().focus_notifications;
  const std::uint64_t dispatches_before =
      env.harness.backend->dispatch_calls();

  env.gate->SetDeferLosses(true);
  oct::FocusCallResult focus;
  Check(oct::SetThreadFocus(*env.host, doc_b, &focus, &env.error),
        "focus-revalidate: real SetFocus moved to B", env.error);
  const std::uint64_t focus_notifications_after =
      env.observer->Snapshot().focus_notifications;
  const std::size_t held_losses = env.gate->pending_losses();
  if (focus_notifications_after > focus_notifications_before) {
    Check(held_losses >= 1,
          "focus-revalidate: the real OnSetFocus was held by the test gate",
          std::to_string(held_losses));
    Check(env.controller->Snapshot().target_losses == losses_before,
          "focus-revalidate: the held notification did not invalidate the binding", "");
  } else {
    Check(held_losses == 0,
          "focus-revalidate: no notification was invented by the gate", "");
  }
  Check(env.coordinator->bound(),
        "focus-revalidate: the binding is still current at the controller boundary", "");

  const ex::TextKey h = Letter(L'h');
  WorkId id;
  const SubmitStatus refused = env.coordinator->SubmitKey(h.event, h.text, &id);
  const CoordinatorSnapshot snapshot = env.coordinator->Snapshot();
  Check(refused == SubmitStatus::kTargetChanged && !id.valid(),
        "focus-revalidate (path B): the live guard refuses the moved target",
        DescribeSubmitStatus(refused));
  Check(snapshot.target_revalidations == before.target_revalidations + 1,
        "focus-revalidate (path B): the guard actually ran exactly once",
        std::to_string(snapshot.target_revalidations));
  Check(snapshot.last_target_revalidation.status ==
            ctx::TargetRevalidationStatus::kContextMismatch,
        "focus-revalidate (path B): the guard read a real context mismatch",
        ctx::DescribeTargetRevalidationStatus(
            snapshot.last_target_revalidation.status));
  Check(snapshot.last_target_revalidation.evidence.has_document_manager &&
            snapshot.last_target_revalidation.evidence.has_top_context &&
            !snapshot.last_target_revalidation.evidence.top_context_matches,
        "focus-revalidate (path B): the mismatch evidence is in the refusal snapshot",
        "");
  Check(env.controller->Snapshot().target_losses == losses_before &&
            env.controller->Snapshot().invalidations == invalidations_before &&
            env.coordinator->bound(),
        "focus-revalidate (path B): no notification invalidated the binding before the guard",
        "losses=" + std::to_string(env.controller->Snapshot().target_losses) +
            " invalidations=" +
            std::to_string(env.controller->Snapshot().invalidations));
  Check(env.harness.backend->dispatch_calls() == dispatches_before,
        "focus-revalidate (path B): no engine call", "");
  Check(env.doc_a->diagnostics().store.text_mutations == 0 &&
            doc_b->diagnostics().store.text_mutations == 0,
        "focus-revalidate (path B): neither document was written", "");

  // Release the controlled buffer, then let the real observer/binding recover to B.
  env.gate->SetDeferLosses(false);
  const std::size_t released = env.gate->ReleaseLosses();
  Check(released == held_losses,
        "focus-revalidate: exactly the held notifications were released",
        std::to_string(released));
  env.observer->Pump();
  env.controller->Pump();
  Check(PumpAllUntilBound(&env, 30000),
        "focus-revalidate: a fresh binding was created for B",
        DescribeSnapshot(env.coordinator->Snapshot()));
  ContextIdRecord b_record;
  Check(env.registry->Observe(doc_b->context(), &b_record) ==
                ctx::ContextRegistryStatus::kOk,
        "focus-revalidate: B's identity is readable", "");
  Check(env.coordinator->Snapshot().identity.context_id == b_record.context_id,
        "focus-revalidate: the fresh binding is B", "");
  CheckDocText(doc_b->text_utf16(), L"",
               "focus-revalidate: the refused key was not replayed");
  const ex::TextKey a = Letter(L'a');
  const DriveResult typed = TypeKeyAndDrive(*env.coordinator, *env.host,
                                            *env.harness.executor, a.event,
                                            a.text, "focus-revalidate: B types");
  CheckOutcome(typed.final_state, WorkOutcome::kApplied,
               "focus-revalidate: fresh input on B applies");
  CheckDocText(doc_b->text_utf16(), L"a",
               "focus-revalidate: only the fresh key reached B");
  CheckDocText(env.doc_a->text_utf16(), L"",
               "focus-revalidate: A was not touched");
  return Finish();
}

// P3.13-R2: StrictTsf is the product default. A real Win32 focus window exists, but the
// process is not a registered TIP, so IsThreadFocus answers S_FALSE. Strict mode must refuse
// without probing or accepting the Win32 window.
int RunFocusGuardStrictNoFallback() {
  ControlledObservedEnv env;
  if (!SetupControlledObserved(&env, TestDocumentOptions{L"", 0, 0},
                               "focus-strict-no-fallback",
                               ctx::ThreadFocusPolicy::kStrictTsf)) {
    return Finish();
  }
  Check(env.observer->Snapshot().thread_focus_policy ==
            ctx::ThreadFocusPolicy::kStrictTsf,
        "focus-strict: the observer was constructed with StrictTsf", "");
  const HWND focus = ::GetFocus();
  Check(focus != nullptr &&
            ::GetWindowThreadProcessId(focus, nullptr) ==
                ::GetCurrentThreadId(),
        "focus-strict: a real Win32 focus window exists on this thread", "");

  const std::uint64_t dispatches_before =
      env.harness.backend->dispatch_calls();
  const ex::TextKey h = Letter(L'h');
  WorkId id;
  const SubmitStatus refused = env.coordinator->SubmitKey(h.event, h.text, &id);
  const CoordinatorSnapshot snapshot = env.coordinator->Snapshot();
  const ctx::ContextObserverSnapshot observer = env.observer->Snapshot();
  Check(refused == SubmitStatus::kTargetUnverified && !id.valid(),
        "focus-strict: ordinary input is refused without a TSF focus confirmation",
        DescribeSubmitStatus(refused));
  Check(observer.win32_thread_focus_checks == 0 &&
            observer.win32_thread_focus_accepts == 0 &&
            observer.last_thread_focus_evidence ==
                ctx::ThreadFocusEvidence::kNone,
        "focus-strict: StrictTsf never probed or accepted the Win32 fallback",
        ctx::DescribeThreadFocusEvidence(
            observer.last_thread_focus_evidence));
  Check(snapshot.last_target_revalidation.evidence.policy ==
            ctx::ThreadFocusPolicy::kStrictTsf &&
            snapshot.last_target_revalidation.thread_focus_evidence ==
                ctx::ThreadFocusEvidence::kNone,
        "focus-strict: the refusal snapshot keeps the strict policy and no source",
        "");
  if (observer.thread_focus_query_ok) {
    Check(snapshot.last_target_revalidation.status ==
                  ctx::TargetRevalidationStatus::kThreadFocusUnconfirmed &&
              snapshot.last_target_revalidation.failure_stage ==
                  ctx::TargetFailureStage::kNone &&
              snapshot.last_target_revalidation.query_hr == S_OK &&
              snapshot.last_target_revalidation.evidence.thread_focus_query_ok &&
              !snapshot.last_target_revalidation.evidence.thread_focus,
          "focus-strict: a successful FALSE BOOL is unconfirmed, not a query error",
          ctx::DescribeTargetRevalidationStatus(
              snapshot.last_target_revalidation.status));
  } else {
    Check(snapshot.last_target_revalidation.status ==
                  ctx::TargetRevalidationStatus::kQueryFailed &&
              snapshot.last_target_revalidation.failure_stage ==
                  ctx::TargetFailureStage::kThreadFocusQuery &&
              snapshot.last_target_revalidation.query_hr ==
                  observer.thread_focus_query_hr,
          "focus-strict: a failed IsThreadFocus keeps its real HRESULT",
          std::to_string(static_cast<long>(
              snapshot.last_target_revalidation.query_hr)));
  }
  Check(env.harness.backend->dispatch_calls() == dispatches_before,
        "focus-strict: no engine call", "");
  Check(env.doc_a->diagnostics().store.text_mutations == 0,
        "focus-strict: no document write", "");
  Check(snapshot.works_accepted == 0,
        "focus-strict: no work was accepted", "");
  return Finish();
}

// A new submit after a real TSF focus move (A -> B) must be refused before the engine,
// even though the observer has not processed the move and the coordinator still says A.
int RunFocusGuardSubmitTargetChanged() {
  ControlledObservedEnv env;
  if (!SetupControlledObserved(&env, TestDocumentOptions{L"", 0, 0},
                               "focus-submit-target")) {
    return Finish();
  }
  TestDocument* doc_b = nullptr;
  if (!CreateDocumentB(&env, &doc_b, "focus-submit-target")) {
    return Finish();
  }
  const CoordinatorSnapshot before = env.coordinator->Snapshot();
  const std::uint64_t dispatches_before = env.harness.backend->dispatch_calls();
  const std::uint64_t a_mutations_before =
      env.doc_a->diagnostics().store.text_mutations;
  const std::uint64_t b_mutations_before =
      doc_b->diagnostics().store.text_mutations;
  const std::uint64_t losses_before =
      env.controller->Snapshot().target_losses;
  const std::uint64_t focus_notifications_before =
      env.observer->Snapshot().focus_notifications;

  oct::FocusCallResult focus;
  Check(oct::SetThreadFocus(*env.host, doc_b, &focus, &env.error),
        "focus-submit-target: real SetFocus moved to B", env.error);
  Check(oct::IsDocumentFocused(*env.host, doc_b),
        "focus-submit-target: TSF GetFocus is B immediately after SetFocus", "");

  // Independent event ordering evidence: did a TSF focus notification invalidate the old
  // binding before this submit? The guard call count after the submit decides the branch;
  // the final status only asserts the branch's contract.
  const auto controller_after_focus = env.controller->Snapshot();

  const ex::TextKey h = Letter(L'h');
  WorkId id;
  const SubmitStatus refused =
      env.coordinator->SubmitKey(h.event, h.text, &id);
  const CoordinatorSnapshot refused_state = env.coordinator->Snapshot();
  const bool guard_ran =
      refused_state.target_revalidations > before.target_revalidations;
  if (guard_ran) {
    Check(refused == SubmitStatus::kTargetChanged,
          "focus-submit-target (path B): the submit is refused as TargetChanged",
          DescribeSubmitStatus(refused));
    Check(!id.valid(),
          "focus-submit-target (path B): the refused key created no work item", "");
    Check(refused_state.target_revalidations ==
              before.target_revalidations + 1,
          "focus-submit-target (path B): the live guard actually ran",
          std::to_string(refused_state.target_revalidations));
    Check(refused_state.last_submit_status == SubmitStatus::kTargetChanged,
          "focus-submit-target: the snapshot records the refusal status", "");
    Check(refused_state.last_target_revalidation.status ==
              ctx::TargetRevalidationStatus::kContextMismatch,
          "focus-submit-target: the guard names the context mismatch",
          ctx::DescribeTargetRevalidationStatus(
              refused_state.last_target_revalidation.status));
    Check(refused_state.last_target_revalidation.evidence.has_document_manager &&
              refused_state.last_target_revalidation.evidence.has_top_context &&
              !refused_state.last_target_revalidation.evidence.top_context_matches,
          "focus-submit-target: the guard read the real B top against bound A", "");
    Check(SUCCEEDED(refused_state.last_target_revalidation.evidence.get_focus_hr) &&
              SUCCEEDED(
                  refused_state.last_target_revalidation.evidence.get_top_hr),
          "focus-submit-target: both live queries succeeded (a mismatch, not a failure)",
          std::to_string(static_cast<long>(
              refused_state.last_target_revalidation.evidence.get_focus_hr)));
    Check(refused_state.last_target_revalidation.thread_focus_evidence !=
              ctx::ThreadFocusEvidence::kNone,
          "focus-submit-target: the thread-focus evidence source is recorded",
          ctx::DescribeThreadFocusEvidence(
              refused_state.last_target_revalidation.thread_focus_evidence));
    Check(controller_after_focus.target_losses == losses_before &&
              env.observer->Snapshot().focus_notifications ==
                  focus_notifications_before,
          "focus-submit-target (path B): no notification invalidated the binding first",
          "losses=" + std::to_string(controller_after_focus.target_losses) +
              " focus_notify=" +
              std::to_string(env.observer->Snapshot().focus_notifications));
  } else {
    Check(refused == SubmitStatus::kNotBound && !id.valid(),
          "focus-submit-target (path A): the notification invalidated before the guard",
          DescribeSubmitStatus(refused));
    Check(refused_state.target_revalidations == before.target_revalidations,
          "focus-submit-target (path A): no target guard ran", "");
    Check(env.controller->Snapshot().target_losses > losses_before,
          "focus-submit-target (path A): a real loss notification is the evidence",
          std::to_string(env.controller->Snapshot().target_losses));
  }
  Check(refused_state.works_accepted == before.works_accepted,
        "focus-submit-target: no work was accepted", "");
  Check(env.harness.backend->dispatch_calls() == dispatches_before,
        "focus-submit-target: no engine dispatch happened", "");
  Check(env.doc_a->diagnostics().store.text_mutations == a_mutations_before &&
            doc_b->diagnostics().store.text_mutations == b_mutations_before,
        "focus-submit-target: neither document was written", "");

  // Recover through the real observer after the refusal. The latest target is B.
  const oct::ObserverDriveResult rebound =
      oct::PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(rebound.settled && env.coordinator->bound(),
        "focus-submit-target: the observer bound the current target B",
        oct::DescribeObserverSnapshot(rebound.final_state));
  PumpRetirements(&env, *env.harness.executor, "focus-submit-target");
  ContextIdRecord b_record;
  env.registry->Observe(doc_b->context(), &b_record);
  Check(env.coordinator->Snapshot().identity.context_id == b_record.context_id,
        "focus-submit-target: the binding is B, not the refused old target", "");
  CheckDocText(doc_b->text_utf16(), L"",
               "focus-submit-target: the refused key was not replayed");

  const ex::TextKey a = Letter(L'a');
  const DriveResult typed = TypeKeyAndDrive(*env.coordinator, *env.host,
                                            *env.harness.executor, a.event,
                                            a.text, "focus-submit-target: B types");
  CheckOutcome(typed.final_state, WorkOutcome::kApplied,
               "focus-submit-target: new input on B applies");
  CheckDocText(doc_b->text_utf16(), L"a",
               "focus-submit-target: only the new key reached B");
  CheckDocText(env.doc_a->text_utf16(), L"",
               "focus-submit-target: A was not touched");
  return Finish();
}

// The engine result arrives after the target moved. The coordinator must not build or
// apply an edit; the engine already ran, so the honest terminal is NeedsResync.
int RunFocusGuardEngineResultTargetChanged() {
  ControlledObservedEnv env;
  if (!SetupControlledObserved(&env, TestDocumentOptions{L"", 0, 0},
                               "focus-engine-target")) {
    return Finish();
  }
  TestDocument* doc_b = nullptr;
  if (!CreateDocumentB(&env, &doc_b, "focus-engine-target")) {
    return Finish();
  }
  env.harness.backend->HoldDispatches();
  const ex::TextKey h = Letter(L'h');
  WorkId id;
  Check(env.coordinator->SubmitKey(h.event, h.text, &id) ==
            SubmitStatus::kAccepted,
        "focus-engine-target: the key was accepted while the target was A", "");
  Check(env.coordinator->Snapshot().phase == WorkPhase::kEnginePending,
        "focus-engine-target: the dispatch is pending", "");
  const CoordinatorSnapshot before_focus = env.coordinator->Snapshot();
  const std::uint64_t losses_before_focus =
      env.controller->Snapshot().target_losses;

  oct::FocusCallResult focus;
  Check(oct::SetThreadFocus(*env.host, doc_b, &focus, &env.error),
        "focus-engine-target: real SetFocus moved to B before the result", env.error);

  env.harness.backend->ReleaseDispatches();
  const DriveResult driven = DriveToCompletion(
      *env.coordinator, *env.host, *env.harness.executor, 20000);
  Check(driven.completed, "focus-engine-target: the work reached a terminal state",
        DescribeSnapshot(driven.final_state));
  // Independent call-order evidence: did the result-time guard run? A notification that
  // arrived before it retired the binding, so the guard count stays unchanged.
  const bool guard_ran =
      driven.final_state.target_revalidations >
      before_focus.target_revalidations;
  if (guard_ran) {
    CheckOutcome(driven.final_state, WorkOutcome::kNeedsResync,
                 "focus-engine-target (path B): the post-engine target move is NeedsResync");
    Check(driven.final_state.cause == RejectCause::kTargetChangedWhileQueued,
          "focus-engine-target (path B): the cause names the target change",
          DescribeRejectCause(driven.final_state.cause));
    Check(driven.final_state.target_revalidations ==
              before_focus.target_revalidations + 1,
          "focus-engine-target (path B): the result-time guard actually ran",
          std::to_string(driven.final_state.target_revalidations));
    Check(driven.final_state.last_target_revalidation.status ==
              ctx::TargetRevalidationStatus::kContextMismatch,
          "focus-engine-target (path B): the live guard saw the context move",
          ctx::DescribeTargetRevalidationStatus(
              driven.final_state.last_target_revalidation.status));
    Check(driven.final_state.last_target_revalidation.evidence.has_document_manager &&
              driven.final_state.last_target_revalidation.evidence.has_top_context &&
              !driven.final_state.last_target_revalidation.evidence
                   .top_context_matches,
          "focus-engine-target (path B): the read target is a real mismatch", "");
    Check(env.controller->Snapshot().target_losses == losses_before_focus,
          "focus-engine-target (path B): no notification invalidated the binding first",
          std::to_string(env.controller->Snapshot().target_losses));
  } else {
    CheckOutcome(driven.final_state, WorkOutcome::kAbandonedStale,
                 "focus-engine-target (path A): the notification abandoned the queued work");
    Check(driven.final_state.target_revalidations ==
              before_focus.target_revalidations,
          "focus-engine-target (path A): no result-time guard ran", "");
    Check(env.controller->Snapshot().target_losses > losses_before_focus,
          "focus-engine-target (path A): a real notification is the ordering evidence",
          std::to_string(env.controller->Snapshot().target_losses));
    Check(driven.final_state.retirement.engine_results_discarded == 1 &&
              driven.final_state.retirement.unresolved_engine_results == 0,
          "focus-engine-target (path A): the old engine result was consumed once",
          std::to_string(
              driven.final_state.retirement.engine_results_discarded));
    Check(driven.final_state.retirement.close_results_taken ==
              driven.final_state.retirement.close_requests,
          "focus-engine-target (path A): the old client was retired cleanly", "");
  }
  Check(driven.final_state.plans_submitted == 0,
        "focus-engine-target: no edit plan was submitted", "");
  Check(driven.final_state.edits_applied == 0,
        "focus-engine-target: no edit applied", "");
  Check(env.doc_a->diagnostics().store.text_mutations == 0 &&
            doc_b->diagnostics().store.text_mutations == 0,
        "focus-engine-target: neither document was written", "");

  // Recover: the observer invalidates A and binds the current B. No replay of the old key.
  const oct::ObserverDriveResult rebound =
      oct::PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(rebound.settled && env.coordinator->bound(),
        "focus-engine-target: a fresh binding is created for B",
        oct::DescribeObserverSnapshot(rebound.final_state));
  PumpRetirements(&env, *env.harness.executor, "focus-engine-target");
  CheckDocText(doc_b->text_utf16(), L"",
               "focus-engine-target: the old key was not replayed");
  const ex::TextKey a = Letter(L'a');
  const DriveResult typed = TypeKeyAndDrive(*env.coordinator, *env.host,
                                            *env.harness.executor, a.event,
                                            a.text, "focus-engine-target: B types");
  CheckOutcome(typed.final_state, WorkOutcome::kApplied,
               "focus-engine-target: fresh input on B applies");
  CheckDocText(doc_b->text_utf16(), L"a",
               "focus-engine-target: only the fresh key reached B");
  CheckDocText(env.doc_a->text_utf16(), L"",
               "focus-engine-target: A was not touched");
  return Finish();
}

// The target moves while an asynchronous edit waits behind a held grant. The P3.7
// callback repeats the live target query under the granted lock and must refuse before any
// write. The observer is deliberately NOT pumped before the grant is released.
int RunFocusGuardEditWaitTargetChanged() {
  ControlledObservedEnv env;
  TestDocumentOptions options;
  options.async_locks = true;
  if (!SetupControlledObserved(&env, options, "focus-edit-target")) {
    return Finish();
  }
  TestDocument* doc_b = nullptr;
  if (!CreateDocumentB(&env, &doc_b, "focus-edit-target")) {
    return Finish();
  }
  const ex::TextKey h = Letter(L'h');
  WorkId id;
  Check(env.coordinator->SubmitKey(h.event, h.text, &id) ==
            SubmitStatus::kAccepted,
        "focus-edit-target: the key was accepted", "");
  const ULONGLONG deadline = ::GetTickCount64() + 20000;
  CoordinatorSnapshot snapshot = env.coordinator->Snapshot();
  while (snapshot.phase != WorkPhase::kEditPending) {
    env.coordinator->Pump();
    snapshot = env.coordinator->Snapshot();
    if (snapshot.phase == WorkPhase::kEditPending ||
        snapshot.phase == WorkPhase::kIdle || ::GetTickCount64() >= deadline) {
      break;
    }
    if (snapshot.phase == WorkPhase::kEnginePending) {
      env.harness.executor->WaitForResult(snapshot.engine_request,
                                          std::chrono::milliseconds(50));
    } else {
      ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
      env.host->DispatchQueuedMessages();
    }
  }
  Check(snapshot.phase == WorkPhase::kEditPending,
        "focus-edit-target: the edit is queued behind the deferred lock",
        DescribeSnapshot(snapshot));
  env.host->SetHoldGrantMessages(true);
  const auto before_park = env.doc_a->diagnostics().store;
  Check(PumpUntilEditGrantParked(*env.host, *env.doc_a, &env.error),
        "focus-edit-target: the deferred grant message was parked without pumping focus",
        env.error);
  const auto held = env.doc_a->diagnostics().store;
  std::printf(
      "[coordinator-win] evidence focus-edit-target: before park req=%llu pending=%d "
      "grants=%llu parked=%zu; after park req=%llu pending=%d grants=%llu async=%llu\n",
      static_cast<unsigned long long>(before_park.request_lock_calls),
      before_park.pending_async_lock ? 1 : 0,
      static_cast<unsigned long long>(before_park.write_lock_grants),
      env.host->held_grant_messages(),
      static_cast<unsigned long long>(held.request_lock_calls),
      held.pending_async_lock ? 1 : 0,
      static_cast<unsigned long long>(held.write_lock_grants),
      static_cast<unsigned long long>(held.async_lock_grants));
  Check(held.pending_async_lock && held.write_lock_grants == 0,
        "focus-edit-target: the write lock is held before the callback",
        "pending=" + std::to_string(held.pending_async_lock ? 1 : 0) +
            " grants=" + std::to_string(held.write_lock_grants));

  const std::uint64_t losses_before_focus =
      env.controller->Snapshot().target_losses;
  oct::FocusCallResult focus;
  Check(oct::SetThreadFocus(*env.host, doc_b, &focus, &env.error),
        "focus-edit-target: real SetFocus moved to B while the edit waited",
        env.error);
  const std::uint64_t callback_checks_before_grant =
      env.coordinator->Snapshot().target_callback_checks;
  const std::uint64_t rejections_before_grant =
      env.coordinator->Snapshot().target_rejections;
  env.host->SetHoldGrantMessages(false);
  const std::size_t granted = env.host->ReleaseHeldGrantMessages();
  Check(granted >= 1, "focus-edit-target: the held grant was released",
        std::to_string(granted));

  const ULONGLONG terminal_deadline = ::GetTickCount64() + 20000;
  while (snapshot.phase != WorkPhase::kIdle) {
    env.coordinator->Pump();
    snapshot = env.coordinator->Snapshot();
    if (snapshot.phase == WorkPhase::kIdle ||
        ::GetTickCount64() >= terminal_deadline) {
      break;
    }
    if (snapshot.phase == WorkPhase::kEditPending) {
      env.host->PumpUntil(env.coordinator->EditCompletionEvent(), 50);
    } else {
      ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
      env.host->DispatchQueuedMessages();
    }
  }
  Check(snapshot.phase == WorkPhase::kIdle,
        "focus-edit-target: the callback completed", DescribeSnapshot(snapshot));
  // Independent call-order evidence: did the granted target guard run at all? A
  // notification that arrived while the grant was held retires the session first, so the
  // guard never runs. The outcome is asserted per branch, never used to choose the branch.
  const bool guard_ran =
      snapshot.target_callback_checks > callback_checks_before_grant;
  if (guard_ran) {
    CheckOutcome(snapshot, WorkOutcome::kNeedsResync,
                 "focus-edit-target (path B): a refused post-engine write is NeedsResync");
    Check(snapshot.cause == RejectCause::kTargetChangedWhileQueued,
          "focus-edit-target (path B): the cause names the target change",
          DescribeRejectCause(snapshot.cause));
    Check(snapshot.last_edit_status == edit::EditStatus::kRejected &&
              snapshot.last_edit_reason == edit::EditRejectReason::kTargetChanged,
          "focus-edit-target (path B): the session refused with TargetChanged",
          std::string(edit::DescribeEditStatus(snapshot.last_edit_status)) + "/" +
              edit::DescribeEditRejectReason(snapshot.last_edit_reason));
    Check(snapshot.target_callback_checks ==
              callback_checks_before_grant + 1,
          "focus-edit-target (path B): the granted-callback target check ran once",
          std::to_string(snapshot.target_callback_checks));
    Check(snapshot.target_rejections == rejections_before_grant + 1,
          "focus-edit-target (path B): the granted-callback target check refused",
          std::to_string(snapshot.target_rejections));
    Check(env.controller->Snapshot().target_losses == losses_before_focus,
          "focus-edit-target (path B): no notification invalidated the binding first",
          std::to_string(env.controller->Snapshot().target_losses));
  } else {
    CheckOutcome(snapshot, WorkOutcome::kAbandonedStale,
                 "focus-edit-target (path A): the notification retired the edit first");
    Check(snapshot.target_rejections == rejections_before_grant,
          "focus-edit-target (path A): the retired session ran no target guard",
          "callback=" + std::to_string(snapshot.target_callback_checks));
    Check(env.controller->Snapshot().target_losses > losses_before_focus,
          "focus-edit-target (path A): a real notification invalidated before the guard",
          std::to_string(env.controller->Snapshot().target_losses));
  }
  Check(env.doc_a->diagnostics().store.text_mutations == 0 &&
            env.doc_a->diagnostics().store.composition_start_calls == 0,
        "focus-edit-target: A received no write and no composition", "");
  Check(doc_b->diagnostics().store.text_mutations == 0,
        "focus-edit-target: B received no write", "");

  const oct::ObserverDriveResult rebound =
      oct::PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(rebound.settled && env.coordinator->bound(),
        "focus-edit-target: the observer bound the current B after the refusal",
        oct::DescribeObserverSnapshot(rebound.final_state));
  PumpRetirements(&env, *env.harness.executor, "focus-edit-target");
  CheckDocText(doc_b->text_utf16(), L"",
               "focus-edit-target: the refused edit was not replayed");
  const ex::TextKey a = Letter(L'a');
  const DriveResult typed = TypeKeyAndDrive(*env.coordinator, *env.host,
                                            *env.harness.executor, a.event,
                                            a.text, "focus-edit-target: B types");
  CheckOutcome(typed.final_state, WorkOutcome::kApplied,
               "focus-edit-target: fresh input on B applies");
  CheckDocText(doc_b->text_utf16(), L"a",
               "focus-edit-target: only the fresh key reached B");
  CheckDocText(env.doc_a->text_utf16(), L"",
               "focus-edit-target: A was not touched");
  return Finish();
}

// The thread loses Win32 focus (SetFocus(nullptr)). Even though TSF still points at the
// bound context, ordinary input must be refused before the engine. If this environment
// delivers OnKillThreadFocus, the observer invalidates the binding first; both outcomes are
// reported, neither is renamed.
int RunFocusGuardThreadFocusLost() {
  ControlledObservedEnv env;
  if (!SetupControlledObserved(&env, TestDocumentOptions{L"", 0, 0},
                               "focus-thread-lost")) {
    return Finish();
  }
  const std::uint64_t dispatches_before = env.harness.backend->dispatch_calls();
  const std::uint64_t kill_before =
      env.observer->Snapshot().thread_focus_kill_notifications;
  const std::uint64_t focus_loss_before =
      env.observer->Snapshot().focus_loss_notifications;
  const std::uint64_t guard_before =
      env.coordinator->Snapshot().target_revalidations;
  oct::ClearTestThreadFocus();
  Check(::GetFocus() == nullptr,
        "focus-thread-lost: Win32 focus is really gone", "");

  const ex::TextKey h = Letter(L'h');
  WorkId id;
  const SubmitStatus refused =
      env.coordinator->SubmitKey(h.event, h.text, &id);
  const CoordinatorSnapshot snapshot = env.coordinator->Snapshot();
  const context::ContextObserverSnapshot observer = env.observer->Snapshot();
  // Independent call-order evidence: did the live guard run? A delivered notification
  // retires the binding first, in which case the guard count is unchanged. This environment
  // may deliver OnSetFocus(NULL) instead of OnKillThreadFocus; both are real notifications.
  const bool guard_ran = snapshot.target_revalidations > guard_before;
  Check(!id.valid(),
        "focus-thread-lost: the key creates no work", "");
  Check(env.harness.backend->dispatch_calls() == dispatches_before,
        "focus-thread-lost: no engine call", "");
  Check(env.doc_a->diagnostics().store.text_mutations == 0,
        "focus-thread-lost: no document write", "");
  if (!guard_ran) {
    Check(refused == SubmitStatus::kNotBound,
          "focus-thread-lost (path A): a real notification invalidated first",
          DescribeSubmitStatus(refused));
    Check(snapshot.target_revalidations == guard_before,
          "focus-thread-lost (path A): no target guard ran", "");
    const auto loss = env.controller->Snapshot();
    Check(loss.last_loss_reason == ctx::ContextDropReason::kNoFocusedDocument ||
              loss.last_loss_reason == ctx::ContextDropReason::kThreadFocusLost,
          "focus-thread-lost (path A): the loss reason is a real notification",
          ctx::DescribeContextDropReason(loss.last_loss_reason));
    Check(observer.thread_focus_kill_notifications > kill_before ||
              observer.focus_loss_notifications > focus_loss_before,
          "focus-thread-lost (path A): a real focus notification was delivered",
          "kill=" + std::to_string(observer.thread_focus_kill_notifications) +
              " focus_loss=" +
              std::to_string(observer.focus_loss_notifications));
  } else {
    Check(refused == SubmitStatus::kTargetUnverified,
          "focus-thread-lost (path B): the live guard refuses as TargetUnverified",
          DescribeSubmitStatus(refused));
    Check(snapshot.target_revalidations == guard_before + 1,
          "focus-thread-lost (path B): the live guard actually ran",
          std::to_string(snapshot.target_revalidations));
    if (observer.thread_focus_query_ok) {
      Check(snapshot.last_target_revalidation.status ==
                ctx::TargetRevalidationStatus::kThreadFocusUnconfirmed &&
                snapshot.last_target_revalidation.failure_stage ==
                    ctx::TargetFailureStage::kNone &&
                snapshot.last_target_revalidation.query_hr == S_OK &&
                snapshot.last_target_revalidation.evidence.thread_focus_query_ok &&
                !snapshot.last_target_revalidation.evidence.thread_focus &&
                !snapshot.last_target_revalidation.evidence.win32_thread_focus,
            "focus-thread-lost (path B): a valid FALSE BOOL is unconfirmed, not an error",
            ctx::DescribeTargetRevalidationStatus(
                snapshot.last_target_revalidation.status));
    } else {
      Check(snapshot.last_target_revalidation.status ==
                ctx::TargetRevalidationStatus::kQueryFailed &&
                snapshot.last_target_revalidation.failure_stage ==
                    ctx::TargetFailureStage::kThreadFocusQuery &&
                snapshot.last_target_revalidation.query_hr ==
                    observer.thread_focus_query_hr,
            "focus-thread-lost (path B): a failed focus query keeps its HRESULT",
            ctx::DescribeTargetFailureStage(
                snapshot.last_target_revalidation.failure_stage));
    }
    Check(observer.thread_focus_kill_notifications == kill_before,
          "focus-thread-lost (path B): no kill notification was fabricated",
          std::to_string(observer.thread_focus_kill_notifications));
  }

  // Restore real Win32 focus. If the binding was invalidated, restore the TSF document
  // target explicitly as well, then let the observer re-read it; no old input is replayed.
  Check(oct::EnsureTestThreadFocus(&env.error),
        "focus-thread-lost: Win32 focus restored", env.error);
  if (!env.coordinator->bound()) {
    oct::FocusCallResult refocus;
    Check(oct::SetThreadFocus(*env.host, env.doc_a, &refocus, &env.error),
          "focus-thread-lost: TSF focus restored to A", env.error);
    const oct::ObserverDriveResult rebound =
        oct::PumpUntilSettled(*env.observer, *env.host, 20000);
    if (!(rebound.settled && env.coordinator->bound())) {
      Check(rebound.final_state.last_refusal_reason !=
                ctx::ContextDropReason::kSelectionUnsupported,
            "focus-thread-lost: refocus did not substitute SelectionUnsupported",
            ctx::DescribeContextDropReason(
                rebound.final_state.last_refusal_reason));
    }
    Check(rebound.settled && env.coordinator->bound(),
          "focus-thread-lost: the observer bound the current target again",
          oct::DescribeObserverSnapshot(rebound.final_state));
    PumpRetirements(&env, *env.harness.executor, "focus-thread-lost");
  }
  CheckDocText(env.doc_a->text_utf16(), L"",
               "focus-thread-lost: the refused key was not replayed");
  const ex::TextKey a = Letter(L'a');
  const DriveResult typed = TypeKeyAndDrive(*env.coordinator, *env.host,
                                            *env.harness.executor, a.event,
                                            a.text, "focus-thread-lost: A types");
  CheckOutcome(typed.final_state, WorkOutcome::kApplied,
               "focus-thread-lost: input is admitted again after focus returns");
  Check(env.doc_a->diagnostics().store.text_mutations > 0,
        "focus-thread-lost: the fresh key reached A", "");
  return Finish();
}

// No focused document manager, then a real empty document manager. Both are distinct
// target failures and must be refused before the engine.
int RunFocusGuardForeignDocumentManager() {
  ControlledObservedEnv env;
  if (!SetupControlledObserved(&env, TestDocumentOptions{L"", 0, 0},
                               "focus-foreign-dm")) {
    return Finish();
  }
  ITfThreadMgr* thread_manager = env.host->thread_manager_for_test();
  const std::uint64_t dispatches_before = env.harness.backend->dispatch_calls();
  const std::uint64_t guard_before =
      env.coordinator->Snapshot().target_revalidations;
  const std::uint64_t losses_before =
      env.controller->Snapshot().target_losses;
  const std::uint64_t focus_notifications_before =
      env.observer->Snapshot().focus_notifications;
  const ex::TextKey h = Letter(L'h');
  WorkId id;

  // A real document manager that this test host did not create, focused while the
  // coordinator still holds document A. The independent GetTop result of the foreign
  // manager selects the expected branch below; it is not inferred from the final status.
  ITfDocumentMgr* foreign = nullptr;
  Check(SUCCEEDED(thread_manager->CreateDocumentMgr(&foreign)) &&
            foreign != nullptr,
        "focus-foreign-dm: a foreign document manager was created", "");
  if (foreign == nullptr) {
    return Finish();
  }
  Check(SUCCEEDED(thread_manager->SetFocus(foreign)),
        "focus-foreign-dm: the foreign manager received TSF focus", "");
  ITfDocumentMgr* current = nullptr;
  const HRESULT current_hr = thread_manager->GetFocus(&current);
  const bool focus_is_foreign = current != nullptr && current == foreign;
  if (current != nullptr) {
    current->Release();
  }
  Check(SUCCEEDED(current_hr) && focus_is_foreign,
        "focus-foreign-dm: GetFocus really is the foreign manager", "");
  Check(!oct::IsDocumentFocused(*env.host, env.doc_a),
        "focus-foreign-dm: TSF focus is not document A", "");
  ITfContext* foreign_top = nullptr;
  const HRESULT foreign_top_hr = foreign->GetTop(&foreign_top);
  const bool foreign_has_top =
      SUCCEEDED(foreign_top_hr) && foreign_top != nullptr;
  if (foreign_top != nullptr) {
    foreign_top->Release();
  }

  const SubmitStatus refused = env.coordinator->SubmitKey(h.event, h.text, &id);
  const CoordinatorSnapshot snapshot = env.coordinator->Snapshot();
  // Independent call-order evidence: the live guard ran, or a real notification had
  // already invalidated the binding. The final status only asserts that branch.
  const bool guard_ran = snapshot.target_revalidations > guard_before;
  Check(!id.valid() && env.harness.backend->dispatch_calls() == dispatches_before,
        "focus-foreign-dm: ordinary input is refused before the engine",
        DescribeSubmitStatus(refused));
  if (!guard_ran) {
    Check(refused == SubmitStatus::kNotBound,
          "focus-foreign-dm (path A): a real notification invalidated first",
          DescribeSubmitStatus(refused));
    Check(snapshot.target_revalidations == guard_before,
          "focus-foreign-dm (path A): no target guard ran", "");
    const auto loss = env.controller->Snapshot();
    Check(loss.target_losses > losses_before &&
              (loss.last_loss_reason ==
                   ctx::ContextDropReason::kFocusMoved ||
               loss.last_loss_reason ==
                   ctx::ContextDropReason::kEditSinkUnavailable),
          "focus-foreign-dm (path A): the foreign focus notification lost the target",
          ctx::DescribeContextDropReason(loss.last_loss_reason));
    Check(env.observer->Snapshot().focus_notifications >
              focus_notifications_before,
          "focus-foreign-dm (path A): OnSetFocus was really delivered", "");
  } else {
    Check(snapshot.target_revalidations == guard_before + 1,
          "focus-foreign-dm (path B): the live guard actually ran",
          std::to_string(snapshot.target_revalidations));
    Check(env.controller->Snapshot().target_losses == losses_before,
          "focus-foreign-dm (path B): no notification invalidated the binding first", "");
    if (FAILED(foreign_top_hr)) {
      Check(refused == SubmitStatus::kTargetUnverified &&
                snapshot.last_target_revalidation.status ==
                    ctx::TargetRevalidationStatus::kQueryFailed &&
                snapshot.last_target_revalidation.failure_stage ==
                    ctx::TargetFailureStage::kGetTop &&
                snapshot.last_target_revalidation.query_hr == foreign_top_hr,
            "focus-foreign-dm: a failed foreign GetTop is a query failure",
            ctx::DescribeTargetFailureStage(
                snapshot.last_target_revalidation.failure_stage));
    } else if (foreign_has_top) {
      Check(refused == SubmitStatus::kTargetChanged &&
                snapshot.last_target_revalidation.status ==
                    ctx::TargetRevalidationStatus::kContextMismatch &&
                snapshot.last_target_revalidation.failure_stage ==
                    ctx::TargetFailureStage::kNone &&
                snapshot.last_target_revalidation.query_hr == S_OK,
            "focus-foreign-dm: a real foreign top is a context mismatch",
            ctx::DescribeTargetRevalidationStatus(
                snapshot.last_target_revalidation.status));
      Check(snapshot.last_target_revalidation.evidence.has_document_manager &&
                snapshot.last_target_revalidation.evidence.has_top_context &&
                !snapshot.last_target_revalidation.evidence.top_context_matches &&
                snapshot.last_target_revalidation.evidence.get_top_hr == S_OK,
            "focus-foreign-dm: the guard read the foreign top as a mismatch", "");
    } else {
      Check(refused == SubmitStatus::kTargetChanged &&
                snapshot.last_target_revalidation.status ==
                    ctx::TargetRevalidationStatus::kNoTopContext &&
                snapshot.last_target_revalidation.failure_stage ==
                    ctx::TargetFailureStage::kNone &&
                snapshot.last_target_revalidation.query_hr == S_OK,
            "focus-foreign-dm: GetTop success with no top is an empty target",
            ctx::DescribeTargetRevalidationStatus(
                snapshot.last_target_revalidation.status));
    }
  }
  Check(snapshot.last_submit_status == refused,
        "focus-foreign-dm: the refuse status is recorded", "");
  Check(env.doc_a->diagnostics().store.text_mutations == 0,
        "focus-foreign-dm: no document write", "");
  thread_manager->SetFocus(nullptr);
  foreign->Release();
  return Finish();
}

// The same context is re-bound by the registry (generation moved on) without any TSF focus
// event. The binding's generation is no longer the current one; input is refused.
int RunFocusGuardGenerationChanged() {
  ControlledObservedEnv env;
  if (!SetupControlledObserved(&env, TestDocumentOptions{L"", 0, 0},
                               "focus-generation")) {
    return Finish();
  }
  ContextIdRecord before;
  Check(env.registry->Observe(env.doc_a->context(), &before) ==
            ctx::ContextRegistryStatus::kOk,
        "focus-generation: the current identity is readable", "");
  ContextIdRecord bumped;
  Check(env.registry->BeginBinding(env.doc_a->context(), &bumped) ==
                ctx::ContextRegistryStatus::kOk &&
            bumped.generation > before.generation,
        "focus-generation: a newer binding generation exists",
        "before=" + std::to_string(before.generation) +
            " after=" + std::to_string(bumped.generation));
  const std::uint64_t dispatches_before = env.harness.backend->dispatch_calls();
  const ex::TextKey h = Letter(L'h');
  WorkId id;
  const SubmitStatus refused = env.coordinator->SubmitKey(h.event, h.text, &id);
  const CoordinatorSnapshot snapshot = env.coordinator->Snapshot();
  Check(refused == SubmitStatus::kTargetChanged && !id.valid(),
        "focus-generation: the stale binding refuses input",
        DescribeSubmitStatus(refused));
  Check(snapshot.last_target_revalidation.status ==
            ctx::TargetRevalidationStatus::kGenerationMismatch,
        "focus-generation: the guard names the generation mismatch",
        ctx::DescribeTargetRevalidationStatus(
            snapshot.last_target_revalidation.status));
  Check(snapshot.last_target_revalidation.evidence.observed_generation ==
            bumped.generation,
        "focus-generation: the observed generation is the newer one",
        std::to_string(
            snapshot.last_target_revalidation.evidence.observed_generation));
  Check(env.harness.backend->dispatch_calls() == dispatches_before,
        "focus-generation: no engine call", "");
  Check(env.doc_a->diagnostics().store.text_mutations == 0,
        "focus-generation: no document write", "");
  return Finish();
}

// Rapid A -> B -> A -> B switching fills the bounded retirement ledger. The refused target
// is NOT promised bound and no client is created for it. After the ledger is terminal the
// controller re-reads the CURRENT focus (B), binds it, and the refused key is not replayed.
int RunFocusGuardRetireRebindLatest() {
  ControlledObservedEnv env;
  if (!SetupControlledObserved(&env, TestDocumentOptions{L"", 0, 0},
                               "focus-retire-rebind")) {
    return Finish();
  }
  TestDocument* doc_b = nullptr;
  if (!CreateDocumentB(&env, &doc_b, "focus-retire-rebind")) {
    return Finish();
  }
  const std::size_t clients_before = env.clients->created().size();
  auto switch_focus = [&](TestDocument* document, const char* label) {
    oct::FocusCallResult focus;
    Check(oct::SetThreadFocus(*env.host, document, &focus, &env.error),
          std::string(label) + ": real SetFocus", env.error);
    const oct::ObserverDriveResult driven =
        oct::PumpUntilSettled(*env.observer, *env.host, 20000);
    Check(driven.settled, std::string(label) + ": the observer settled",
          oct::DescribeObserverSnapshot(driven.final_state));
  };

  switch_focus(doc_b, "focus-retire-rebind: A to B");
  Check(env.coordinator->bound(),
        "focus-retire-rebind: B is bound while A retires", "");
  switch_focus(env.doc_a, "focus-retire-rebind: B to A");
  Check(!env.coordinator->bound(),
        "focus-retire-rebind: A is refused while the ledger is full", "");
  Check(env.coordinator->Snapshot().retirement.retiring_bindings == 2,
        "focus-retire-rebind: the bounded ledger holds two retirements",
        std::to_string(
            env.coordinator->Snapshot().retirement.retiring_bindings));
  Check(env.controller->Snapshot().rebind_pending,
        "focus-retire-rebind: a target recheck is pending, not a bound target", "");
  switch_focus(doc_b, "focus-retire-rebind: A to B again");
  Check(!env.coordinator->bound(),
        "focus-retire-rebind: the newest target is still not promised bound", "");
  Check(env.controller->Snapshot().binds_paused_retiring >= 2,
        "focus-retire-rebind: both refused targets were paused as Retiring",
        std::to_string(env.controller->Snapshot().binds_paused_retiring));
  Check(env.clients->created().size() == clients_before + 1,
        "focus-retire-rebind: refused targets created no engine client",
        "created=" + std::to_string(env.clients->created().size()) +
            " expected=" + std::to_string(clients_before + 1));

  const std::uint64_t dispatches_before = env.harness.backend->dispatch_calls();
  const ex::TextKey h = Letter(L'h');
  WorkId id;
  const SubmitStatus paused = env.coordinator->SubmitKey(h.event, h.text, &id);
  Check(paused == SubmitStatus::kNotBound && !id.valid(),
        "focus-retire-rebind: input while unbound creates no work",
        DescribeSubmitStatus(paused));
  Check(env.harness.backend->dispatch_calls() == dispatches_before,
        "focus-retire-rebind: no engine call for the unbound key", "");
  Check(env.coordinator->Snapshot().works_accepted == 0,
        "focus-retire-rebind: the refused key was never accepted", "");

  // Advance the real retirement facts, then let the controller re-read the current focus.
  const ULONGLONG deadline = ::GetTickCount64() + 30000;
  CoordinatorSnapshot snapshot = env.coordinator->Snapshot();
  for (;;) {
    env.coordinator->Pump();
    env.controller->Pump();
    env.observer->Pump();
    env.host->DispatchQueuedMessages();
    snapshot = env.coordinator->Snapshot();
    if (snapshot.bound && snapshot.retirement.retiring_bindings == 0) {
      break;
    }
    if (::GetTickCount64() >= deadline) {
      break;
    }
    if (snapshot.retirement.pending_edit_event != nullptr) {
      env.host->PumpUntil(snapshot.retirement.pending_edit_event, 20);
    } else if (snapshot.retirement.pending_close_request.valid()) {
      env.harness.executor->WaitForResult(
          snapshot.retirement.pending_close_request, std::chrono::milliseconds(20));
    } else if (snapshot.retirement.pending_engine_request.valid()) {
      env.harness.executor->WaitForResult(
          snapshot.retirement.pending_engine_request,
          std::chrono::milliseconds(20));
    } else {
      ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
    }
  }
  Check(snapshot.bound && snapshot.retirement.retiring_bindings == 0,
        "focus-retire-rebind: the binding is restored after retirement",
        DescribeSnapshot(snapshot));
  Check(env.controller->Snapshot().rebind_requests >= 1,
        "focus-retire-rebind: the controller revalidated the current target",
        std::to_string(env.controller->Snapshot().rebind_requests));
  Check(!env.controller->Snapshot().rebind_pending,
        "focus-retire-rebind: no stale target is still pending", "");
  ContextIdRecord b_record;
  env.registry->Observe(doc_b->context(), &b_record);
  Check(snapshot.identity.context_id == b_record.context_id,
        "focus-retire-rebind: the restored binding is the LATEST target B",
        "bound=" + std::to_string(snapshot.identity.context_id) +
            " latest=" + std::to_string(b_record.context_id));
  Check(env.clients->created().size() == clients_before + 2,
        "focus-retire-rebind: only the two real B bindings created clients",
        "created=" + std::to_string(env.clients->created().size()) +
            " expected=" + std::to_string(clients_before + 2));
  Check(snapshot.retirement.close_requests ==
            snapshot.retirement.close_results_taken,
        "focus-retire-rebind: every close result was consumed",
        std::to_string(snapshot.retirement.close_requests) + "/" +
            std::to_string(snapshot.retirement.close_results_taken));
  CheckDocText(doc_b->text_utf16(), L"",
               "focus-retire-rebind: the refused key was not replayed");

  const ex::TextKey a = Letter(L'a');
  const DriveResult typed = TypeKeyAndDrive(*env.coordinator, *env.host,
                                            *env.harness.executor, a.event,
                                            a.text, "focus-retire-rebind: B types");
  CheckOutcome(typed.final_state, WorkOutcome::kApplied,
               "focus-retire-rebind: fresh input on the restored target applies");
  CheckDocText(doc_b->text_utf16(), L"a",
               "focus-retire-rebind: only the fresh key reached B");
  CheckDocText(env.doc_a->text_utf16(), L"",
               "focus-retire-rebind: A was not touched");
  return Finish();
}

int RunObservedBindingCase(const std::string& case_name, const Options& options) {
  if (case_name == "observed-bind-and-type") return RunObservedBindAndType();
  if (case_name == "observed-external-selection") return RunObservedExternalSelection();
  if (case_name == "observed-external-text") return RunObservedExternalText();
  if (case_name == "observed-late-permission") return RunObservedLatePermission();
  if (case_name == "observed-late-edit") return RunObservedLateEdit();
  if (case_name == "observed-uncertain-swap") return RunObservedUncertainAcrossCoordinator();
  if (case_name == "observed-permission-requery") return RunObservedPermissionRequery();
  if (case_name == "observed-retire-roundtrip" && !options.rime_dll.empty()) {
    return RunObservedRetireRoundtrip(options);
  }
  if (case_name == "observed-compose-retirement" && !options.rime_dll.empty()) {
    return RunObservedComposeRetirement(options);
  }
  if (case_name == "observed-compose-readonly" && !options.rime_dll.empty()) {
    return RunObservedComposeRetirementReadOnly(options);
  }
  if (case_name == "observed-compose-uncertain" && !options.rime_dll.empty()) {
    return RunObservedComposeRetirementUncertain(options);
  }
  if (case_name == "observed-compose-external" && !options.rime_dll.empty()) {
    return RunObservedComposeExternalEdit(options);
  }
  if (case_name == "observed-real-compose" && !options.rime_dll.empty()) {
    return RunObservedRealComposeProgress(options);
  }
  if (case_name == "focus-guard-submit-target") {
    return RunFocusGuardSubmitTargetChanged();
  }
  if (case_name == "focus-guard-engine-target") {
    return RunFocusGuardEngineResultTargetChanged();
  }
  if (case_name == "focus-guard-edit-target") {
    return RunFocusGuardEditWaitTargetChanged();
  }
  if (case_name == "focus-guard-thread-lost") {
    return RunFocusGuardThreadFocusLost();
  }
  if (case_name == "focus-guard-foreign-dm") {
    return RunFocusGuardForeignDocumentManager();
  }
  if (case_name == "focus-guard-generation") {
    return RunFocusGuardGenerationChanged();
  }
  if (case_name == "focus-guard-retire-rebind") {
    return RunFocusGuardRetireRebindLatest();
  }
  if (case_name == "focus-notification-before-guard") {
    return RunFocusNotificationBeforeGuard();
  }
  if (case_name == "focus-revalidate-before-notification") {
    return RunFocusGuardRevalidationBeforeNotification();
  }
  if (case_name == "focus-strict-no-fallback") {
    return RunFocusGuardStrictNoFallback();
  }
  return -1;
}

}  // namespace test
}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva
