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
  std::unique_ptr<ObservedBindingController> controller;
  TestDocument* doc_a = nullptr;
  TfClientId external = 0;
  std::string error;
};

bool SetupControlledObserved(ControlledObservedEnv* env,
                             const TestDocumentOptions& options, const char* label) {
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
  env->observer = std::make_unique<ctx::ContextObserver>(*env->registry,
                                                        env->host->client_id());
  env->controller = std::make_unique<ObservedBindingController>(
      *env->observer, *env->coordinator, *env->registry, *env->clients);
  oct::FocusCallResult focus;
  if (!oct::SetThreadFocus(*env->host, env->doc_a, &focus, &env->error)) {
    Check(false, std::string(label) + ": SetFocus", env->error);
    return false;
  }
  if (!env->observer->Subscribe(env->host->thread_manager_for_test(),
                                env->controller.get(), &env->error)) {
    Check(false, std::string(label) + ": subscribed", env->error);
    return false;
  }
  const oct::ObserverDriveResult driven =
      oct::PumpUntilSettled(*env->observer, *env->host, 20000);
  Check(driven.settled && env->coordinator->bound(),
        std::string(label) + ": the observer bound the coordinator",
        oct::DescribeObserverSnapshot(driven.final_state));
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
                  const char* label) {
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
  env->permission = ctx::CreateRealPermissionSource(env->host->client_id());
  env->registry = std::make_unique<ctx::ContextRegistry>(8);
  env->observer = std::make_unique<ctx::ContextObserver>(*env->registry,
                                                        env->host->client_id());
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
  if (!env->observer->Subscribe(env->host->thread_manager_for_test(),
                                env->controller.get(), &env->error)) {
    Check(false, std::string(label) + ": subscribed", env->error);
    return false;
  }
  const oct::ObserverDriveResult driven =
      oct::PumpUntilSettled(*env->observer, *env->host, 20000);
  Check(driven.settled && env->coordinator->bound(),
        std::string(label) + ": the observer bound the coordinator",
        oct::DescribeObserverSnapshot(driven.final_state));
  Check(env->controller->Snapshot().binds_ok == 1,
        std::string(label) + ": one successful observed bind",
        std::to_string(env->controller->Snapshot().binds_ok));
  env->external =
      oct::ExternalClientId(env->host->thread_manager_for_test(), &env->error);
  Check(env->external != 0 && env->external != env->host->client_id(),
        std::string(label) + ": a distinct real external client", env->error);
  return env->coordinator->bound();
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
  oct::PumpMessagesFor(*env.host, 300);
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
  Check(env.observer->Snapshot().focus_revalidation_changes >= 1,
        "late-edit: the switch was detected by the real GetFocus revalidation "
        "(not an OnSetFocus notification)",
        std::to_string(env.observer->Snapshot().focus_revalidation_changes));
  Check(env.doc_a->diagnostics().store.write_lock_grants == 0,
        "late-edit: A's write lock is still ungranted after the switch", "");
  const ex::TextKey a = Letter(L'a');
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

  // Deliver A's held grant now, after B is bound and has processed its key.
  env.host->SetHoldGrantMessages(false);
  const std::size_t granted = env.host->ReleaseHeldGrantMessages();
  Check(granted >= 1, "late-edit: A's held grant was released late",
        std::to_string(granted));
  Check(env.doc_a->diagnostics().store.text_mutations == 0,
        "late-edit: A's late callback wrote nothing",
        std::to_string(env.doc_a->diagnostics().store.text_mutations));
  CheckDocText(doc_b->text_utf16(), L"a",
               "late-edit: A's late callback did not touch B");
  Check(env.coordinator->Snapshot().identity.context_id == child_record.context_id,
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

int RunObservedBindingCase(const std::string& case_name, const Options& options) {
  if (case_name == "observed-bind-and-type") return RunObservedBindAndType();
  if (case_name == "observed-external-selection") return RunObservedExternalSelection();
  if (case_name == "observed-external-text") return RunObservedExternalText();
  if (case_name == "observed-late-permission") return RunObservedLatePermission();
  if (case_name == "observed-late-edit") return RunObservedLateEdit();
  if (case_name == "observed-uncertain-swap") return RunObservedUncertainAcrossCoordinator();
  if (case_name == "observed-permission-requery") return RunObservedPermissionRequery();
  if (case_name == "observed-real-compose" && !options.rime_dll.empty()) {
    return RunObservedRealComposeProgress(options);
  }
  return -1;
}

}  // namespace test
}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva
