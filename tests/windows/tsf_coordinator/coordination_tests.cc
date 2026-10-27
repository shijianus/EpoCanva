// EpoCanva P3.8 -- stage-1 cases: the coordinator's own state machine over the EXISTING
// controlled-engine seam (tests/windows/engine_executor/controlled_backend) and the REAL
// P3.7 TSF host. The engine here is deliberately not rime: this stage pins coordination
// facts (binding, gates, single flight, generations, abandonment, lockdowns, close) that
// must hold regardless of which engine sits behind the executor. The real-engine flows
// live in real_engine_tests.cc and are separate CTest cases.
//
// EXIT CODES: 0 pass; 5 a check failed; 14 the falsifiability control's expected failure.
#include <windows.h>
#include <msctf.h>

#include <cstdio>
#include <memory>
#include <string>

#include "coordinator_test_support.h"
#include "engine_executor_internal.h"
#include "coordination_tests.h"
#include "engine_to_plan.h"
#include "observer_test_support.h"
#include "test_support.h"

namespace epocanva {
namespace tsf {
namespace coordinator {
namespace test {

namespace {

namespace input = epocanva::windows::input;
namespace engine = epocanva::windows::engine;
namespace executor_tests = epocanva::tests::engine_executor;
namespace oct = epocanva::tsf::context::test;

engine::EngineState StateWithPreedit(const std::u16string& preedit) {
  engine::EngineState state;
  state.valid = true;
  state.is_composing = !preedit.empty();
  state.preedit.utf16_code_units = preedit;
  state.preedit.utf8_byte_length = preedit.size();
  return state;
}

EngineObservation HandledObservation(engine::EngineState state) {
  EngineObservation observation;
  observation.engine_call_made = true;
  observation.engine_reported_handled = true;
  observation.state_status = engine::SessionStateStatus::kOk;
  observation.commit_status = engine::SessionCommitStatus::kNoCommit;
  observation.state = std::move(state);
  return observation;
}

LocalDocumentView ViewWithComposition(std::u16string text, LONG start, LONG caret) {
  LocalDocumentView view;
  view.composition_active = true;
  view.composition_start_utf16 = start;
  view.composition_text_utf16 = std::move(text);
  view.caret_utf16 = caret;
  return view;
}

executor_tests::TextKey Letter(wchar_t character) {
  return executor_tests::MakeTextKey(character, ::GetKeyboardLayout(0));
}

input::KeyEventSnapshot SpecialKey(std::uint32_t virtual_key) {
  return executor_tests::MakeKeyEvent(virtual_key, false, false, false, false);
}

struct ControlledCase {
  std::unique_ptr<TestHost> host;
  ControlledHarness harness;
  // The explicit test-owned permission seam. The coordinator's callback-time recheck
  // still reads the REAL metadata of the test document, so a case that needs a
  // last-moment change flips the document's real scope/read-only state, not this knob.
  ManualPermissionSource permission;
  // P3.13 controlled target authority; not a system notification.
  TestTargetRevalidator target;
  ContextCoordinator* coordinator = nullptr;
  std::unique_ptr<ContextCoordinator> coordinator_owner;
  TestDocument* document = nullptr;

  ~ControlledCase() {
    if (coordinator != nullptr) {
      coordinator->Close();
    }
    if (host != nullptr) {
      host->Shutdown();
    }
  }
};

// One document, one bound coordinator, engine client A. `grant_allowed` leaves the
// binding at its safe Unknown default so a case can exercise the permission gates itself.
bool SetupControlledCase(ControlledCase* setup, const TestDocumentOptions& options,
                         const char* label, bool grant_allowed = true) {
  std::string error;
  setup->host = TestHost::Create(&error);
  Check(setup->host != nullptr, std::string(label) + ": real TSF host created", error);
  if (setup->host == nullptr) {
    return false;
  }
  setup->document = setup->host->CreateDocument(options, &error);
  Check(setup->document != nullptr, std::string(label) + ": real document created", error);
  if (setup->document == nullptr) {
    return false;
  }
  if (!CreateControlledHarness(&setup->harness)) {
    return false;
  }
  setup->coordinator_owner = std::make_unique<ContextCoordinator>(
      *setup->harness.executor, setup->host->client_id(), setup->permission);
  setup->coordinator = setup->coordinator_owner.get();
  setup->coordinator->AttachTargetRevalidator(&setup->target);
  const BindStatus bound = setup->coordinator->Bind(
      setup->document->context(), setup->document->context_id(),
      options.initial_selection_start_utf16, setup->harness.client_a);
  Check(bound == BindStatus::kOk, std::string(label) + ": bound", DescribeBindStatus(bound));
  if (grant_allowed) {
    setup->permission.Set(epocanva::tsf::context::PermissionState::kAllowed);
  }
  return bound == BindStatus::kOk;
}

// --- 1. the pure engine-observation -> edit-decision matrix -----------------------------

int RunConversionPure() {
  const std::u16string h = u"h";
  const std::u16string he = u"he";
  const std::u16string hel = u"hel";
  const std::u16string hel_space = u"hel ";

  // No composition, engine starts one.
  PlanDecision begin = DecidePlanFromEngine(LocalDocumentView{},
                                            HandledObservation(StateWithPreedit(h)));
  Check(begin.kind == PlanDecisionKind::kBegin, "conversion: begin on fresh preedit",
        DescribePlanDecisionKind(begin.kind));
  Check(begin.text_utf16 == h, "conversion: begin text is the engine preedit", "");
  Check(begin.caret_utf16 == 0, "conversion: begin caret is the tracked caret", "");

  // Composition exists, engine preedit differs -> replace, not append.
  LocalDocumentView view_h = ViewWithComposition(h, 0, 1);
  PlanDecision update = DecidePlanFromEngine(view_h, HandledObservation(StateWithPreedit(he)));
  Check(update.kind == PlanDecisionKind::kUpdate, "conversion: update on changed preedit",
        DescribePlanDecisionKind(update.kind));
  Check(update.text_utf16 == he, "conversion: update text is the whole new preedit", "");

  // Composition exists, preedit already displayed -> no edit at all.
  PlanDecision redundant =
      DecidePlanFromEngine(view_h, HandledObservation(StateWithPreedit(h)));
  Check(redundant.kind == PlanDecisionKind::kNoEdit,
        "conversion: already-displayed preedit is NoEdit",
        DescribePlanDecisionKind(redundant.kind));

  // Commit over a live composition.
  EngineObservation commit_obs = HandledObservation(engine::EngineState{});
  commit_obs.state.valid = true;
  commit_obs.state.is_composing = false;
  commit_obs.commit.present = true;
  commit_obs.commit.text_utf16_code_units = hel;
  PlanDecision commit = DecidePlanFromEngine(view_h, commit_obs);
  Check(commit.kind == PlanDecisionKind::kCommit,
        "conversion: commit replaces the composition", DescribePlanDecisionKind(commit.kind));
  Check(commit.text_utf16 == hel, "conversion: commit text is the engine commit", "");

  // No-preedit commit inserts once at the tracked caret.
  LocalDocumentView view_empty;
  view_empty.caret_utf16 = 4;
  PlanDecision insert = DecidePlanFromEngine(view_empty, commit_obs);
  Check(insert.kind == PlanDecisionKind::kCommit,
        "conversion: no-composition commit inserts", DescribePlanDecisionKind(insert.kind));
  Check(insert.caret_utf16 == 4, "conversion: insert caret is the tracked caret", "");

  // The space policy shape: "hel " commit with no composition (P2.5's trailing space).
  EngineObservation space_obs = commit_obs;
  space_obs.commit.text_utf16_code_units = hel_space;
  PlanDecision space = DecidePlanFromEngine(view_empty, space_obs);
  Check(space.kind == PlanDecisionKind::kCommit && space.text_utf16 == hel_space,
        "conversion: commit-with-space keeps the trailing space",
        DescribePlanDecisionKind(space.kind));

  // An empty commit with no composition has nothing to insert.
  EngineObservation empty_commit = commit_obs;
  empty_commit.commit.text_utf16_code_units = std::u16string();
  PlanDecision nothing = DecidePlanFromEngine(view_empty, empty_commit);
  Check(nothing.kind == PlanDecisionKind::kNoEdit,
        "conversion: empty commit without composition is NoEdit",
        DescribePlanDecisionKind(nothing.kind));

  // The unsupported shape: a commit and a live preedit in one observation.
  EngineObservation both = HandledObservation(StateWithPreedit(he));
  both.commit.present = true;
  both.commit.text_utf16_code_units = hel;
  PlanDecision unsupported = DecidePlanFromEngine(view_h, both);
  Check(unsupported.kind == PlanDecisionKind::kUnsupportedShape,
        "conversion: commit+preedit is refused whole, not halved",
        DescribePlanDecisionKind(unsupported.kind));

  // The engine composition ended without a commit -> remove the local preedit.
  PlanDecision cancel =
      DecidePlanFromEngine(view_h, HandledObservation(StateWithPreedit(std::u16string())));
  Check(cancel.kind == PlanDecisionKind::kCancel,
        "conversion: composition end without commit cancels",
        DescribePlanDecisionKind(cancel.kind));

  // Nothing composing anywhere -> nothing to do.
  PlanDecision idle = DecidePlanFromEngine(LocalDocumentView{},
                                           HandledObservation(StateWithPreedit(std::u16string())));
  Check(idle.kind == PlanDecisionKind::kNoEdit, "conversion: idle observation is NoEdit",
        DescribePlanDecisionKind(idle.kind));

  // Engine refused / not called.
  EngineObservation refused = HandledObservation(StateWithPreedit(he));
  refused.engine_refused = true;
  Check(DecidePlanFromEngine(view_h, refused).kind == PlanDecisionKind::kNoEdit,
        "conversion: engine refusal is NoEdit", "");
  EngineObservation unhandled = HandledObservation(StateWithPreedit(he));
  unhandled.engine_call_made = false;
  Check(DecidePlanFromEngine(view_h, unhandled).kind == PlanDecisionKind::kNoEdit,
        "conversion: no engine call is NoEdit", "");

  // P3.9 ordering: a call that ran but whose observation is unreadable is never NoEdit,
  // even when the engine also reported the call unhandled. A call that never ran is
  // NoEdit regardless of the (irrelevant) observation fields.
  EngineObservation dispatched_unreadable = HandledObservation(StateWithPreedit(he));
  dispatched_unreadable.engine_refused = true;
  dispatched_unreadable.state_status = engine::SessionStateStatus::kStatusFailed;
  Check(DecidePlanFromEngine(view_h, dispatched_unreadable).kind ==
            PlanDecisionKind::kInvalidObservation,
        "conversion: dispatched + unreadable is not masked by the refusal branch", "");
  EngineObservation not_dispatched_unreadable =
      HandledObservation(StateWithPreedit(he));
  not_dispatched_unreadable.engine_call_made = false;
  not_dispatched_unreadable.engine_refused = true;
  not_dispatched_unreadable.state_status = engine::SessionStateStatus::kStatusFailed;
  Check(DecidePlanFromEngine(view_h, not_dispatched_unreadable).kind ==
            PlanDecisionKind::kNoEdit,
        "conversion: not-dispatched stays distinguishable from dispatched-but-failed",
        "");

  // An unreadable state cannot be mapped.
  EngineObservation broken = HandledObservation(StateWithPreedit(he));
  broken.state_status = engine::SessionStateStatus::kStatusFailed;
  Check(DecidePlanFromEngine(view_h, broken).kind == PlanDecisionKind::kInvalidObservation,
        "conversion: unreadable state is InvalidObservation", "");
  EngineObservation broken_commit = HandledObservation(engine::EngineState{});
  broken_commit.state.valid = true;
  broken_commit.commit_status = engine::SessionCommitStatus::kConversionFailed;
  Check(DecidePlanFromEngine(view_h, broken_commit).kind ==
            PlanDecisionKind::kInvalidObservation,
        "conversion: unreadable commit status is InvalidObservation", "");

  return Finish();
}

// --- 2. binding and gates ----------------------------------------------------------------

int RunBindAndGates() {
  ControlledCase setup;
  if (!SetupControlledCase(&setup, TestDocumentOptions{}, "bind-gates",
                           /*grant_allowed=*/false)) {
    return Finish();
  }
  ContextCoordinator& coordinator = *setup.coordinator;
  const auto& backend = *setup.harness.backend;

  Check(coordinator.identity().generation == 1, "bind: first generation is 1", "");
  Check(coordinator.Snapshot().permission == edit::InputPermission::kUnknown,
        "bind: a new binding starts Unknown (safe refusal)", "");

  // Unknown permission: no engine dispatch, no edit.
  WorkId id;
  const executor_tests::TextKey h = Letter(L'h');
  Check(coordinator.SubmitKey(h.event, h.text, &id) == SubmitStatus::kPermissionUnknown,
        "gates: Unknown permission refuses before the engine", "");
  Check(backend.dispatch_calls() == 0, "gates: Unknown made no engine call", "");
  Check(setup.document->store()->diagnostics().request_lock_calls == 0,
        "gates: Unknown requested no edit", "");

  setup.permission.Set(epocanva::tsf::context::PermissionState::kDenied);
  Check(coordinator.SubmitKey(h.event, h.text, &id) == SubmitStatus::kPermissionDenied,
        "gates: Denied permission refuses before the engine", "");
  Check(backend.dispatch_calls() == 0, "gates: Denied made no engine call", "");

  setup.permission.Set(epocanva::tsf::context::PermissionState::kAllowed);
  const BindStatus rebind = coordinator.Bind(setup.document->context(),
                                             setup.document->context_id(), 0,
                                             setup.harness.client_a);
  Check(rebind == BindStatus::kAlreadyBound, "gates: a bound coordinator refuses re-bind",
        DescribeBindStatus(rebind));

  // A shortcut is not an engine candidate.
  const input::KeyEventSnapshot ctrl_h =
      executor_tests::MakeKeyEvent(0x48, false, true, false, false);
  Check(coordinator.SubmitKey(ctrl_h, h.text, &id) == SubmitStatus::kNotEngineCandidate,
        "gates: a Control chord is not an engine candidate", "");
  Check(backend.dispatch_calls() == 0, "gates: the chord made no engine call", "");

  // Invalid bind arguments.
  Check(coordinator.Bind(nullptr, 9, 0, setup.harness.client_a) ==
            BindStatus::kInvalidRequest,
        "bind: null context refused", "");
  Check(coordinator.Bind(setup.document->context(), 0, 0, setup.harness.client_a) ==
            BindStatus::kInvalidRequest,
        "bind: zero context id refused", "");
  Check(coordinator.Bind(setup.document->context(), 9, -1, setup.harness.client_a) ==
            BindStatus::kInvalidRequest,
        "bind: negative caret refused", "");

  // Invalidation bumps the generation; the next bind is a fresh ledger.
  coordinator.InvalidateBinding();
  Check(!coordinator.bound(), "bind: invalidation unbinds", "");
  const input::KeyDecision decision = coordinator.Evaluate(h.event, h.text);
  Check(decision.disposition == input::KeyDisposition::kPassThroughToHost &&
            decision.reason == input::KeyDecisionReason::kEngineNotReady,
        "evaluate: unbound evaluation is pass-through (engine not ready)", "");
  Check(coordinator.Bind(setup.document->context(), setup.document->context_id(), 0,
                         setup.harness.client_b) == BindStatus::kOk,
        "bind: re-bind after invalidation uses a fresh client", "");
  Check(coordinator.identity().generation == 2, "bind: the generation increased", "");
  Check(coordinator.Snapshot().permission == edit::InputPermission::kUnknown,
        "bind: re-bind resets permission to Unknown", "");
  return Finish();
}

// --- 3. single flight --------------------------------------------------------------------

int RunBusySingleFlight() {
  ControlledCase setup;
  if (!SetupControlledCase(&setup, TestDocumentOptions{}, "busy")) {
    return Finish();
  }
  ContextCoordinator& coordinator = *setup.coordinator;
  const auto& backend = *setup.harness.backend;

  const executor_tests::TextKey h = Letter(L'h');
  const executor_tests::TextKey e = Letter(L'e');
  WorkId first = {};
  WorkId second = {};
  Check(coordinator.SubmitKey(h.event, h.text, &first) == SubmitStatus::kAccepted,
        "busy: first submit accepted", "");
  Check(first.valid(), "busy: the accepted work has an id", "");
  const SubmitStatus busy = coordinator.SubmitKey(e.event, e.text, &second);
  Check(busy == SubmitStatus::kBusy, "busy: a second submit is explicitly Busy",
        DescribeSubmitStatus(busy));
  Check(!second.valid(), "busy: the refused submit got no work id", "");
  Check(backend.dispatch_calls() == 0 || backend.dispatch_calls() == 1,
        "busy: at most one engine command exists", "");

  const DriveResult driven =
      DriveToCompletion(coordinator, *setup.host, *setup.harness.executor, 20000);
  Check(driven.completed && driven.final_state.outcome == WorkOutcome::kApplied,
        "busy: the one accepted work applied", DescribeSnapshot(driven.final_state));
  CheckDocText(setup.document->text_utf16(), L"h", "busy: the document holds one letter");
  Check(backend.dispatch_calls() == 1, "busy: exactly one engine dispatch happened",
        std::to_string(backend.dispatch_calls()));

  // The slot is free again after the terminal state.
  Check(coordinator.SubmitKey(e.event, e.text, &second) == SubmitStatus::kAccepted,
        "busy: submit accepted after completion", "");
  const DriveResult second_drive =
      DriveToCompletion(coordinator, *setup.host, *setup.harness.executor, 20000);
  Check(second_drive.completed && second_drive.final_state.outcome == WorkOutcome::kApplied,
        "busy: the second work applied", DescribeSnapshot(second_drive.final_state));
  CheckDocText(setup.document->text_utf16(), L"he", "busy: the document advanced once");
  return Finish();
}

// --- 4. composition progress through the real store (controlled engine) -----------------

int RunCompositionProgressControlled() {
  ControlledCase setup;
  if (!SetupControlledCase(&setup, TestDocumentOptions{}, "progress")) {
    return Finish();
  }
  ContextCoordinator& coordinator = *setup.coordinator;

  const wchar_t letters[] = {L'h', L'e', L'l'};
  const std::wstring expected[] = {L"h", L"he", L"hel"};
  for (int index = 0; index < 3; ++index) {
    const executor_tests::TextKey key = Letter(letters[index]);
    const DriveResult driven = TypeKeyAndDrive(coordinator, *setup.host,
                                               *setup.harness.executor, key.event,
                                               key.text, "progress: type letter");
    CheckOutcome(driven.final_state, WorkOutcome::kApplied, "progress: letter applied");
    CheckDocText(setup.document->text_utf16(), expected[index],
                 "progress: document holds the composition so far");
    Check(driven.final_state.composition_active,
          "progress: the composition stays active", "");
  }
  const auto diagnostics = setup.document->diagnostics().store;
  Check(diagnostics.composition_start_calls == 1,
        "progress: exactly one composition started",
        std::to_string(diagnostics.composition_start_calls));
  Check(diagnostics.composition_update_calls == 3,
        "progress: begin insert + two updates changed the composition three times",
        std::to_string(diagnostics.composition_update_calls));
  Check(diagnostics.text_mutations == 3, "progress: three real TSF mutations",
        std::to_string(diagnostics.text_mutations));
  Check(coordinator.Snapshot().tracked_caret_utf16 == 3,
        "progress: the tracked caret follows the composition", "");
  return Finish();
}

// --- 5. evaluate is pure -----------------------------------------------------------------

int RunEvaluatePure() {
  ControlledCase setup;
  if (!SetupControlledCase(&setup, TestDocumentOptions{}, "evaluate")) {
    return Finish();
  }
  ContextCoordinator& coordinator = *setup.coordinator;
  const auto& backend = *setup.harness.backend;

  // Evaluate never queries permission; the coordinator's cached verdict is populated by
  // work items. Prime it with one real work item, then prove repeated evaluation adds
  // nothing of its own.
  const executor_tests::TextKey h = Letter(L'h');
  const DriveResult primed = TypeKeyAndDrive(coordinator, *setup.host,
                                             *setup.harness.executor, h.event, h.text,
                                             "evaluate: prime the cached verdict");
  CheckOutcome(primed.final_state, WorkOutcome::kApplied,
               "evaluate: the priming work applied");
  const engine::CapacityUsage capacity_before = setup.harness.executor->capacity();
  const std::uint64_t dispatches_before = backend.dispatch_calls();
  const auto diagnostics_before = setup.document->diagnostics().store;
  const std::wstring text_before = setup.document->text_utf16();

  const executor_tests::TextKey e = Letter(L'e');
  for (int index = 0; index < 3; ++index) {
    const input::KeyDecision decision = coordinator.Evaluate(e.event, e.text);
    Check(decision.disposition == input::KeyDisposition::kEngineTextCandidate,
          "evaluate: letter is an engine candidate", "");
    Check(decision.ascii_code_point == static_cast<std::uint32_t>(L'e'),
          "evaluate: the candidate carries the scalar", "");
  }
  const engine::CapacityUsage capacity = setup.harness.executor->capacity();
  Check(capacity.occupied == capacity_before.occupied,
        "evaluate: no executor request exists",
        std::to_string(capacity.occupied));
  Check(backend.dispatch_calls() == dispatches_before && backend.evaluate_calls() == 0,
        "evaluate: neither dispatch nor evaluate was submitted", "");
  const auto diagnostics = setup.document->diagnostics().store;
  Check(diagnostics.request_lock_calls == diagnostics_before.request_lock_calls &&
            diagnostics.text_mutations == diagnostics_before.text_mutations,
        "evaluate: no edit was requested", "");
  CheckDocText(setup.document->text_utf16(), text_before,
               "evaluate: the document is untouched");
  return Finish();
}

// --- 6. invalidation while the engine request is outstanding ----------------------------

int RunEnginePendingInvalidate() {
  ControlledCase setup;
  TestDocumentOptions options;
  if (!SetupControlledCase(&setup, options, "engine-pending-invalidate")) {
    return Finish();
  }
  ContextCoordinator& coordinator = *setup.coordinator;
  auto* backend = setup.harness.backend;

  std::string error;
  TestDocumentOptions second_options;
  TestDocument* document_b = setup.host->CreateDocument(second_options, &error);
  Check(document_b != nullptr, "engine-pending: second document created", error);
  if (document_b == nullptr) {
    return Finish();
  }

  // Hold the dispatch inside the worker so the result provably cannot be ready yet.
  backend->HoldDispatches();
  const executor_tests::TextKey h = Letter(L'h');
  WorkId id;
  Check(coordinator.SubmitKey(h.event, h.text, &id) == SubmitStatus::kAccepted,
        "engine-pending: submit accepted while held", "");
  Check(coordinator.Snapshot().phase == WorkPhase::kEnginePending,
        "engine-pending: the work is engine-pending", "");

  coordinator.InvalidateBinding();
  coordinator.Pump();  // submits the old client's close and observes the owed result
  CoordinatorSnapshot snapshot = coordinator.Snapshot();
  Check(snapshot.outcome == WorkOutcome::kAbandonedStale,
        "engine-pending: invalidation terminalizes the work as AbandonedStale",
        DescribeSnapshot(snapshot));
  Check(snapshot.phase == WorkPhase::kIdle,
        "engine-pending: the work item is terminal immediately", "");
  Check(snapshot.retirement.retiring_bindings == 1,
        "engine-pending: the old binding is in the bounded retirement ledger",
        DescribeSnapshot(snapshot));
  Check(snapshot.retirement.pending_engine_request.valid(),
        "engine-pending: the old engine result is still owed by the ledger", "");
  Check(snapshot.retirement.close_requests == 1,
        "engine-pending: a close was submitted for the old client", "");

  const BindStatus admitted = coordinator.Bind(
      document_b->context(), document_b->context_id(), 0, setup.harness.client_b);
  Check(admitted == BindStatus::kOk,
        "engine-pending: the new binding is admitted while the old retires",
        DescribeBindStatus(admitted));
  setup.permission.Set(epocanva::tsf::context::PermissionState::kAllowed);
  const executor_tests::TextKey x = Letter(L'x');
  WorkId paused_id;
  const SubmitStatus paused = coordinator.SubmitKey(x.event, x.text, &paused_id);
  Check(paused == SubmitStatus::kRetiring,
        "engine-pending: input is explicitly paused by the retirement",
        DescribeSubmitStatus(paused));
  Check(!paused_id.valid(), "engine-pending: a paused key created no work item", "");

  backend->ReleaseDispatches();
  const DriveResult driven =
      DriveToCompletion(coordinator, *setup.host, *setup.harness.executor, 20000);
  Check(driven.completed, "engine-pending: the discard and close completed",
        DescribeSnapshot(driven.final_state));
  Check(driven.final_state.engine_results_taken == 1,
        "engine-pending: exactly one engine result was taken", "");
  Check(driven.final_state.retirement.engine_results_discarded == 1,
        "engine-pending: the old result was discarded exactly once", "");
  Check(driven.final_state.retirement.close_requests == 1 &&
            driven.final_state.retirement.close_results_taken == 1,
        "engine-pending: the close request and its result were both consumed",
        DescribeSnapshot(driven.final_state));
  Check(driven.final_state.retirement.retiring_bindings == 0,
        "engine-pending: the retirement ledger is empty again", "");
  CheckDocText(setup.document->text_utf16(), L"",
               "engine-pending: the old document was never written");
  CheckDocText(document_b->text_utf16(), L"",
               "engine-pending: the new document was never written by the old result");

  // The fresh binding works normally once retirement is terminal.
  const executor_tests::TextKey a = Letter(L'a');
  const DriveResult typed = TypeKeyAndDrive(coordinator, *setup.host,
                                            *setup.harness.executor, a.event, a.text,
                                            "engine-pending: new binding types");
  CheckOutcome(typed.final_state, WorkOutcome::kApplied,
               "engine-pending: the new binding's work applied");
  CheckDocText(document_b->text_utf16(), L"a",
               "engine-pending: only the new key reached the new document");
  CheckDocText(setup.document->text_utf16(), L"",
               "engine-pending: the old document stayed untouched");
  return Finish();
}

// --- 6b. a full executor queue does not lose a retirement --------------------------------

int RunRetirementQueueFull() {
  std::string error;
  auto host = TestHost::Create(&error);
  Check(host != nullptr, "retire-queue: host created", error);
  if (host == nullptr) {
    return Finish();
  }
  TestDocumentOptions options;
  TestDocument* doc_a = host->CreateDocument(options, &error);
  TestDocument* doc_b = host->CreateDocument(options, &error);
  Check(doc_a != nullptr && doc_b != nullptr, "retire-queue: two documents created",
        error);
  if (doc_a == nullptr || doc_b == nullptr) {
    return Finish();
  }

  auto* backend = new executor_tests::ControlledBackend();
  engine::ExecutorOptions executor_options;
  executor_options.capacity = 1;  // one slot: a held dispatch fills it completely
  auto executor = engine::EngineExecutorTestFactory::Create(
      executor_options, std::unique_ptr<engine::EngineBackend>(backend));
  Check(executor->Start() == engine::StartStatus::kOk, "retire-queue: executor started",
        "");
  engine::ClientId client_a{};
  engine::ClientId client_b{};
  engine::ClientId client_c{};
  for (engine::ClientId* client : {&client_a, &client_b, &client_c}) {
    engine::RequestId create = executor_tests::SubmitCreateOrFail(
        *executor, "controlled",
        engine::ControllerOptions{true, input::InputPermission::kAllowed}, client,
        "retire-queue client");
    if (!create.valid()) {
      return Finish();
    }
    executor_tests::TakeOrFail(*executor, create, 10000, "retire-queue create");
  }

  ManualPermissionSource permission;
  permission.Set(epocanva::tsf::context::PermissionState::kAllowed);
  TestTargetRevalidator target;
  ContextCoordinator coordinator(*executor, host->client_id(), permission);
  coordinator.AttachTargetRevalidator(&target);
  Check(coordinator.Bind(doc_a->context(), doc_a->context_id(), 0, client_a) ==
            BindStatus::kOk,
        "retire-queue: bound to A", "");

  backend->HoldDispatches();
  const executor_tests::TextKey h = Letter(L'h');
  WorkId id;
  Check(coordinator.SubmitKey(h.event, h.text, &id) == SubmitStatus::kAccepted,
        "retire-queue: the held dispatch was accepted", "");
  Check(coordinator.Snapshot().phase == WorkPhase::kEnginePending,
        "retire-queue: the dispatch occupies the only slot", "");

  coordinator.InvalidateBinding();
  coordinator.Pump();
  CoordinatorSnapshot snapshot = coordinator.Snapshot();
  Check(snapshot.retirement.retiring_bindings == 1,
        "retire-queue: A is in the retirement ledger", DescribeSnapshot(snapshot));
  Check(snapshot.retirement.close_requests == 0 &&
            snapshot.retirement.close_queue_full_retries >= 1,
        "retire-queue: the close was not lost; it is retried after QueueFull",
        DescribeSnapshot(snapshot));
  Check(snapshot.retirement.pending_engine_request.valid(),
        "retire-queue: the owed engine result is still named", "");

  Check(coordinator.Bind(doc_b->context(), doc_b->context_id(), 0, client_b) ==
            BindStatus::kOk,
        "retire-queue: B is admitted while A retires", "");
  const executor_tests::TextKey z = Letter(L'z');
  WorkId paused;
  Check(coordinator.SubmitKey(z.event, z.text, &paused) == SubmitStatus::kRetiring,
        "retire-queue: B's input is explicitly paused", "");

  // A second quick switch fills the ledger. Admission now refuses a third binding
  // explicitly instead of creating another client.
  coordinator.InvalidateBinding();
  coordinator.Pump();
  snapshot = coordinator.Snapshot();
  Check(snapshot.retirement.retiring_bindings == 2,
        "retire-queue: the ledger is full and bounded",
        DescribeSnapshot(snapshot));
  Check(coordinator.CheckBindingAdmission(doc_a->context(), doc_a->context_id(), 1, 0) ==
            BindStatus::kRetiring,
        "retire-queue: a full ledger refuses admission as Retiring", "");
  Check(coordinator.Bind(doc_a->context(), doc_a->context_id(), 0, client_c) ==
            BindStatus::kRetiring,
        "retire-queue: Bind refuses without dropping an old retirement", "");

  backend->ReleaseDispatches();
  const DriveResult drained =
      DriveToCompletion(coordinator, *host, *executor, 20000);
  Check(drained.completed && drained.final_state.retirement.retiring_bindings == 0,
        "retire-queue: both retirements became terminal",
        DescribeSnapshot(drained.final_state));
  Check(drained.final_state.retirement.started == 2 &&
            drained.final_state.retirement.settled == 2,
        "retire-queue: two started, two settled retirements",
        DescribeSnapshot(drained.final_state));
  Check(drained.final_state.retirement.already_ordinary == 2 &&
            drained.final_state.retirement.finished == 0 &&
            drained.final_state.retirement.externally_terminated == 0 &&
            drained.final_state.retirement.blocked == 0 &&
            drained.final_state.retirement.cleanup_end_calls == 0 &&
            !drained.final_state.retirement.last_cleanup_owned_at_start &&
            !drained.final_state.retirement.last_cleanup_plan_submitted &&
            !drained.final_state.retirement.last_cleanup_end_issued &&
            RetirementCleanupEvidenceConsistent(drained.final_state.retirement) &&
            RetirementAccountingBalanced(drained.final_state.retirement),
        "retire-queue: bindings that never owned a composition are AlreadyOrdinary",
        DescribeSnapshot(drained.final_state));
  Check(drained.final_state.retirement.close_requests == 2 &&
            drained.final_state.retirement.close_results_taken == 2,
        "retire-queue: both close requests and both results were consumed",
        DescribeSnapshot(drained.final_state));
  Check(drained.final_state.retirement.engine_results_discarded == 1,
        "retire-queue: the pre-retirement engine result was consumed once", "");
  const engine::ClientUsage clients = executor->clients();
  Check(clients.created == 3 && clients.closed == 2 && clients.live == 1,
        "retire-queue: the ledger did not create an unbounded client set",
        "created=" + std::to_string(clients.created) +
            " closed=" + std::to_string(clients.closed) +
            " live=" + std::to_string(clients.live));

  Check(coordinator.Bind(doc_a->context(), doc_a->context_id(), 0, client_c) ==
            BindStatus::kOk,
        "retire-queue: a new binding is admitted after the ledger drains", "");
  const executor_tests::TextKey a = Letter(L'a');
  const DriveResult typed =
      TypeKeyAndDrive(coordinator, *host, *executor, a.event, a.text,
                      "retire-queue: new binding types");
  CheckOutcome(typed.final_state, WorkOutcome::kApplied,
               "retire-queue: the new binding works");
  CheckDocText(doc_a->text_utf16(), L"a",
               "retire-queue: only the new key reached the document");
  coordinator.Close();
  host->Shutdown();
  return Finish();
}

// --- 6c. a withheld lock pauses the new binding until the old composition is finished -----

int RunRetirementComposeWithheld() {
  ControlledCase setup;
  TestDocumentOptions options;
  options.async_locks = true;
  if (!SetupControlledCase(&setup, options, "retire-compose-withheld")) {
    return Finish();
  }
  ContextCoordinator& coordinator = *setup.coordinator;

  for (wchar_t letter : {L'h', L'e'}) {
    const executor_tests::TextKey key = Letter(letter);
    TypeKeyAndDrive(coordinator, *setup.host, *setup.harness.executor, key.event,
                    key.text, "withheld: composition letter");
  }
  CheckDocText(setup.document->text_utf16(), L"he",
               "withheld: the composition holds he");
  ULONG compositions = 0;
  Check(SUCCEEDED(oct::CountCompositions(setup.document->context(), &compositions)) &&
            compositions == 1,
        "withheld: TSF really has one live composition",
        std::to_string(compositions));

  std::string error;
  TestDocument* document_b = setup.host->CreateDocument(TestDocumentOptions{}, &error);
  Check(document_b != nullptr, "withheld: second document created", error);
  if (document_b == nullptr) {
    return Finish();
  }

  setup.host->SetHoldGrantMessages(true);
  const executor_tests::TextKey l = Letter(L'l');
  WorkId held_id;
  Check(coordinator.SubmitKey(l.event, l.text, &held_id) == SubmitStatus::kAccepted,
        "withheld: the third letter was accepted", "");
  const ULONGLONG deadline = ::GetTickCount64() + 20000;
  CoordinatorSnapshot snapshot = coordinator.Snapshot();
  while (snapshot.phase != WorkPhase::kEditPending) {
    coordinator.Pump();
    snapshot = coordinator.Snapshot();
    if (snapshot.phase == WorkPhase::kIdle || ::GetTickCount64() >= deadline) {
      break;
    }
    setup.harness.executor->WaitForResult(snapshot.engine_request,
                                          std::chrono::milliseconds(50));
  }
  Check(snapshot.phase == WorkPhase::kEditPending,
        "withheld: the third letter waits behind the held grant",
        DescribeSnapshot(snapshot));
  // Park the real grant message in the host without granting it. This fixes the order:
  // the old accepted plan exists, its callback has not run, and the switch happens first.
  setup.host->DispatchQueuedMessages();
  Check(setup.host->held_grant_messages() >= 1,
        "withheld: the old plan's grant is held by the host",
        std::to_string(setup.host->held_grant_messages()));
  const auto before_release = setup.document->diagnostics().store;
  const std::wstring text_before_release = setup.document->text_utf16();

  coordinator.InvalidateBinding();
  coordinator.Pump();
  snapshot = coordinator.Snapshot();
  Check(snapshot.retirement.retiring_bindings == 1 &&
            snapshot.retirement.retiring_compositions == 1,
        "withheld: the retirement owns the live composition",
        DescribeSnapshot(snapshot));
  Check(snapshot.retirement.pending_edit_event != nullptr,
        "withheld: the abandoned plan's terminal is still owed before cleanup",
        DescribeSnapshot(snapshot));
  Check(coordinator.Bind(document_b->context(), document_b->context_id(), 0,
                         setup.harness.client_b) == BindStatus::kOk,
        "withheld: B is admitted while the cleanup waits", "");
  setup.permission.Set(epocanva::tsf::context::PermissionState::kAllowed);
  const executor_tests::TextKey z = Letter(L'z');
  WorkId paused;
  Check(coordinator.SubmitKey(z.event, z.text, &paused) == SubmitStatus::kRetiring,
        "withheld: B's input is explicitly paused", "");
  CheckDocText(setup.document->text_utf16(), text_before_release,
               "withheld: the abandoned third letter did not reach A while paused");
  compositions = 0;
  Check(SUCCEEDED(oct::CountCompositions(setup.document->context(), &compositions)) &&
            compositions == 1,
        "withheld: A's composition is still the retirement's to finish",
        std::to_string(compositions));

  setup.host->SetHoldGrantMessages(false);
  const std::size_t released = setup.host->ReleaseHeldGrantMessages();
  Check(released >= 1, "withheld: the held grant was released",
        std::to_string(released));
  const DriveResult drained =
      DriveToCompletion(coordinator, *setup.host, *setup.harness.executor, 20000);
  Check(drained.completed && drained.final_state.retirement.retiring_bindings == 0,
        "withheld: the cleanup reached a terminal state",
        DescribeSnapshot(drained.final_state));
  Check(drained.final_state.retirement.last_cleanup_state == CleanupState::kFinished,
        "withheld: the composition was finished (not committed/cancelled)",
        DescribeSnapshot(drained.final_state));
  std::printf("[coordinator-win] evidence retire-withheld: %s\n",
              DescribeSnapshot(drained.final_state).c_str());
  const RetirementSnapshot& retired = drained.final_state.retirement;
  Check(retired.settled == 1 && retired.finished == 1 &&
            retired.already_ordinary == 0 && retired.externally_terminated == 0 &&
            retired.blocked == 0 && retired.cleanup_end_calls == 1 &&
            retired.cleanup_end_failures == 0,
        "withheld: one settled Finished, backed by exactly one end call",
        DescribeSnapshot(drained.final_state));
  Check(retired.last_cleanup_owned_at_start && retired.last_cleanup_live_at_start &&
            !retired.last_cleanup_external_at_start &&
            !retired.last_cleanup_ended_before_at_start &&
            retired.last_cleanup_plan_submitted && retired.last_cleanup_end_issued &&
            SUCCEEDED(retired.last_cleanup_end_hr) &&
            RetirementCleanupEvidenceConsistent(retired) &&
            RetirementAccountingBalanced(retired),
        "withheld: Finished is explained only by this cleanup's own success",
        DescribeSnapshot(drained.final_state));
  const auto after_release = setup.document->diagnostics().store;
  Check(after_release.text_mutations == before_release.text_mutations &&
            after_release.set_text_calls == before_release.set_text_calls,
        "withheld: finishing changed no text",
        "mutations=" + std::to_string(after_release.text_mutations) +
            " set_text=" + std::to_string(after_release.set_text_calls));
  CheckDocText(setup.document->text_utf16(), L"he",
               "withheld: A kept exactly the text that was displayed");
  compositions = 0;
  Check(SUCCEEDED(oct::CountCompositions(setup.document->context(), &compositions)) &&
            compositions == 0,
        "withheld: A's real composition ended after the lock was granted",
        std::to_string(compositions));

  // The record is already terminal and erased; a late Pump must not settle it a second
  // time or add a second end call.
  const CoordinatorSnapshot before_repeat = coordinator.Snapshot();
  for (int pump = 0; pump < 3; ++pump) {
    coordinator.Pump();
  }
  const CoordinatorSnapshot repeated = coordinator.Snapshot();
  Check(repeated.retirement.settled == before_repeat.retirement.settled &&
            repeated.retirement.finished == before_repeat.retirement.finished &&
            repeated.retirement.externally_terminated ==
                before_repeat.retirement.externally_terminated &&
            repeated.retirement.blocked == before_repeat.retirement.blocked &&
            repeated.retirement.cleanup_end_calls ==
                before_repeat.retirement.cleanup_end_calls,
        "withheld: repeated Pump leaves the settled record unchanged",
        DescribeSnapshot(repeated));

  const executor_tests::TextKey a = Letter(L'a');
  const DriveResult typed = TypeKeyAndDrive(coordinator, *setup.host,
                                            *setup.harness.executor, a.event, a.text,
                                            "withheld: B types after cleanup");
  CheckOutcome(typed.final_state, WorkOutcome::kApplied,
               "withheld: B's input resumed");
  CheckDocText(document_b->text_utf16(), L"a",
               "withheld: only B received the resumed key");
  CheckDocText(setup.document->text_utf16(), L"he",
               "withheld: A was not touched by B");
  return Finish();
}

// --- 6d. an external owner termination, not this cleanup's end call ---------------------

bool TerminateOwnedTestComposition(ControlledCase& setup, const char* label) {
  oct::OwnerCompositionEndResult ended;
  std::string error;
  const bool terminated = oct::TerminateTestCompositionAsOwner(
      setup.document->context(), &ended, &error);
  Check(terminated, std::string(label) + ": the document owner terminated the composition",
        error);
  Check(ended.found && ended.compositions_before == 1 && SUCCEEDED(ended.end_hr) &&
            ended.completed,
        std::string(label) + ": exactly the one confirmed test composition ended",
        "found=" + std::to_string(ended.found ? 1 : 0) +
            " before=" + std::to_string(ended.compositions_before) +
            " end_hr=" + epocanva::tsf::edit::test::HrText(ended.end_hr));
  ULONG remaining = 99;
  Check(SUCCEEDED(oct::CountCompositions(setup.document->context(), &remaining)) &&
            remaining == 0,
        std::string(label) + ": the real composition enumeration is empty",
        std::to_string(remaining));
  return terminated;
}

int RunRetirementComposeExternallyTerminatedBeforePump() {
  ControlledCase setup;
  if (!SetupControlledCase(&setup, TestDocumentOptions{}, "retire-external-before")) {
    return Finish();
  }
  ContextCoordinator& coordinator = *setup.coordinator;
  for (wchar_t letter : {L'h', L'e'}) {
    const executor_tests::TextKey key = Letter(letter);
    TypeKeyAndDrive(coordinator, *setup.host, *setup.harness.executor, key.event,
                    key.text, "external-before: composition letter");
  }
  CheckDocText(setup.document->text_utf16(), L"he",
               "external-before: the composition holds he");
  if (!TerminateOwnedTestComposition(setup, "external-before")) {
    return Finish();
  }
  const auto before = setup.document->diagnostics().store;

  coordinator.InvalidateBinding();
  const DriveResult drained =
      DriveToCompletion(coordinator, *setup.host, *setup.harness.executor, 20000);
  Check(drained.completed && drained.final_state.retirement.retiring_bindings == 0,
        "external-before: the retirement reached a terminal state",
        DescribeSnapshot(drained.final_state));
  const RetirementSnapshot& retired = drained.final_state.retirement;
  std::printf("[coordinator-win] evidence retire-external-before: %s\n",
              DescribeSnapshot(drained.final_state).c_str());
  Check(retired.last_cleanup_state == CleanupState::kTerminatedExternally &&
            retired.settled == 1 && retired.externally_terminated == 1 &&
            retired.finished == 0 && retired.already_ordinary == 0 &&
            retired.blocked == 0 && retired.cleanup_end_calls == 0,
        "external-before: attributed to the external termination, not AlreadyOrdinary",
        DescribeSnapshot(drained.final_state));
  Check(retired.last_cleanup_owned_at_start &&
            !retired.last_cleanup_ended_before_at_start &&
            !retired.last_cleanup_end_issued &&
            RetirementCleanupEvidenceConsistent(retired) &&
            RetirementAccountingBalanced(retired),
        "external-before: no end call is claimed by this cleanup",
        DescribeSnapshot(drained.final_state));
  const auto after = setup.document->diagnostics().store;
  Check(after.text_mutations == before.text_mutations &&
            after.set_text_calls == before.set_text_calls &&
            after.composition_end_calls == before.composition_end_calls,
        "external-before: cleanup issued no SetText and no end call",
        "mutations=" + std::to_string(after.text_mutations) +
            " set_text=" + std::to_string(after.set_text_calls) +
            " end=" + std::to_string(after.composition_end_calls));
  CheckDocText(setup.document->text_utf16(), L"he",
               "external-before: the displayed text survived verbatim");
  return Finish();
}

int RunRetirementComposeExternallyTerminatedWhilePending() {
  ControlledCase setup;
  TestDocumentOptions options;
  options.async_locks = true;
  if (!SetupControlledCase(&setup, options, "retire-external-pending")) {
    return Finish();
  }
  ContextCoordinator& coordinator = *setup.coordinator;
  for (wchar_t letter : {L'h', L'e'}) {
    const executor_tests::TextKey key = Letter(letter);
    TypeKeyAndDrive(coordinator, *setup.host, *setup.harness.executor, key.event,
                    key.text, "external-pending: composition letter");
  }
  CheckDocText(setup.document->text_utf16(), L"he",
               "external-pending: the composition holds he");

  // Park an accepted third plan behind a held grant, then retire. The cleanup must wait for
  // that plan's Stale terminal before it can submit its own EndOwnedComposition session.
  setup.host->SetHoldGrantMessages(true);
  const executor_tests::TextKey l = Letter(L'l');
  WorkId held_id;
  Check(coordinator.SubmitKey(l.event, l.text, &held_id) == SubmitStatus::kAccepted,
        "external-pending: the third letter was accepted", "");
  const ULONGLONG phase_deadline = ::GetTickCount64() + 20000;
  CoordinatorSnapshot snapshot = coordinator.Snapshot();
  while (snapshot.phase != WorkPhase::kEditPending) {
    coordinator.Pump();
    snapshot = coordinator.Snapshot();
    if (snapshot.phase == WorkPhase::kIdle || ::GetTickCount64() >= phase_deadline) {
      break;
    }
    setup.harness.executor->WaitForResult(snapshot.engine_request,
                                          std::chrono::milliseconds(50));
  }
  Check(snapshot.phase == WorkPhase::kEditPending,
        "external-pending: the abandoned plan waits behind the held grant",
        DescribeSnapshot(snapshot));
  setup.host->DispatchQueuedMessages();
  Check(setup.host->held_grant_messages() >= 1,
        "external-pending: the abandoned plan's grant is parked",
        std::to_string(setup.host->held_grant_messages()));

  coordinator.InvalidateBinding();
  coordinator.Pump();
  snapshot = coordinator.Snapshot();
  Check(snapshot.retirement.retiring_bindings == 1 &&
            snapshot.retirement.pending_edit_event != nullptr,
        "external-pending: cleanup waits for the abandoned plan's terminal",
        DescribeSnapshot(snapshot));

  // Let the abandoned plan reach Stale without any TSF call, then hold again before the
  // cleanup's own grant is dispatched. That makes the cleanup edit session really pending.
  setup.host->SetHoldGrantMessages(false);
  const std::size_t released = setup.host->ReleaseHeldGrantMessages();
  Check(released >= 1, "external-pending: the abandoned grant was released",
        std::to_string(released));
  setup.host->SetHoldGrantMessages(true);
  const ULONGLONG pending_deadline = ::GetTickCount64() + 20000;
  bool cleanup_parked = false;
  while (::GetTickCount64() < pending_deadline) {
    coordinator.Pump();
    setup.host->DispatchQueuedMessages();
    const CoordinatorSnapshot current = coordinator.Snapshot();
    if (setup.host->held_grant_messages() >= 1 &&
        current.retirement.pending_edit_event != nullptr &&
        current.retirement.last_cleanup_state == CleanupState::kNone) {
      cleanup_parked = true;
      snapshot = current;
      break;
    }
    ::Sleep(1);
  }
  Check(cleanup_parked,
        "external-pending: the cleanup plan is pending behind the held grant",
        DescribeSnapshot(coordinator.Snapshot()));

  if (!TerminateOwnedTestComposition(setup, "external-pending")) {
    return Finish();
  }
  // The owner's own TerminateComposition is a real end call and is counted by the store.
  // Compare against the post-termination counts so only the cleanup is in scope.
  const auto after_owner = setup.document->diagnostics().store;

  setup.host->SetHoldGrantMessages(false);
  setup.host->ReleaseHeldGrantMessages();
  const DriveResult drained =
      DriveToCompletion(coordinator, *setup.host, *setup.harness.executor, 20000);
  Check(drained.completed && drained.final_state.retirement.retiring_bindings == 0,
        "external-pending: the retirement reached a terminal state",
        DescribeSnapshot(drained.final_state));
  const RetirementSnapshot& retired = drained.final_state.retirement;
  std::printf("[coordinator-win] evidence retire-external-pending: %s\n",
              DescribeSnapshot(drained.final_state).c_str());
  Check(retired.last_cleanup_state == CleanupState::kTerminatedExternally &&
            retired.settled == 1 && retired.externally_terminated == 1 &&
            retired.finished == 0 && retired.already_ordinary == 0 &&
            retired.blocked == 0 && retired.cleanup_end_calls == 0,
        "external-pending: the external end is attributed, never Finished",
        DescribeSnapshot(drained.final_state));
  Check(retired.last_cleanup_owned_at_start &&
            !retired.last_cleanup_ended_before_at_start &&
            !retired.last_cleanup_end_issued &&
            retired.last_cleanup_termination_notifications_at_settle >
                retired.last_cleanup_termination_notifications_at_start &&
            RetirementCleanupEvidenceConsistent(retired) &&
            RetirementAccountingBalanced(retired),
        "external-pending: the pending cleanup issued no end call",
        DescribeSnapshot(drained.final_state));
  const auto after = setup.document->diagnostics().store;
  Check(after.text_mutations == after_owner.text_mutations &&
            after.set_text_calls == after_owner.set_text_calls &&
            after.composition_end_calls == after_owner.composition_end_calls,
        "external-pending: cleanup issued no SetText and no end call",
        "mutations=" + std::to_string(after.text_mutations) +
            " set_text=" + std::to_string(after.set_text_calls) +
            " end=" + std::to_string(after.composition_end_calls));
  CheckDocText(setup.document->text_utf16(), L"he",
               "external-pending: the displayed text survived verbatim");
  return Finish();
}

// --- 6d2. an external termination before any cleanup plan could be submitted -------------

int RunRetirementComposeExternallyTerminatedWhilePriorPlanPending() {
  ControlledCase setup;
  TestDocumentOptions options;
  options.async_locks = true;
  if (!SetupControlledCase(&setup, options, "retire-external-prior-pending")) {
    return Finish();
  }
  ContextCoordinator& coordinator = *setup.coordinator;
  for (wchar_t letter : {L'h', L'e'}) {
    const executor_tests::TextKey key = Letter(letter);
    TypeKeyAndDrive(coordinator, *setup.host, *setup.harness.executor, key.event,
                    key.text, "external-prior-pending: composition letter");
  }
  CheckDocText(setup.document->text_utf16(), L"he",
               "external-prior-pending: the composition holds he");

  // Park a pre-retirement accepted plan behind a held grant. Retirement then waits for its
  // Stale terminal before it may submit its own cleanup plan.
  setup.host->SetHoldGrantMessages(true);
  const executor_tests::TextKey l = Letter(L'l');
  WorkId held_id;
  Check(coordinator.SubmitKey(l.event, l.text, &held_id) == SubmitStatus::kAccepted,
        "external-prior-pending: the third letter was accepted", "");
  const ULONGLONG phase_deadline = ::GetTickCount64() + 20000;
  CoordinatorSnapshot snapshot = coordinator.Snapshot();
  while (snapshot.phase != WorkPhase::kEditPending) {
    coordinator.Pump();
    snapshot = coordinator.Snapshot();
    if (snapshot.phase == WorkPhase::kIdle || ::GetTickCount64() >= phase_deadline) {
      break;
    }
    setup.harness.executor->WaitForResult(snapshot.engine_request,
                                          std::chrono::milliseconds(50));
  }
  Check(snapshot.phase == WorkPhase::kEditPending,
        "external-prior-pending: the abandoned plan waits behind the held grant",
        DescribeSnapshot(snapshot));
  setup.host->DispatchQueuedMessages();
  Check(setup.host->held_grant_messages() >= 1,
        "external-prior-pending: the abandoned plan's grant is parked",
        std::to_string(setup.host->held_grant_messages()));

  coordinator.InvalidateBinding();
  coordinator.Pump();
  snapshot = coordinator.Snapshot();
  Check(snapshot.retirement.retiring_bindings == 1 &&
            snapshot.retirement.pending_edit_event != nullptr,
        "external-prior-pending: cleanup still waits for the abandoned plan's terminal",
        DescribeSnapshot(snapshot));
  ULONG compositions = 0;
  Check(SUCCEEDED(oct::CountCompositions(setup.document->context(), &compositions)) &&
            compositions == 1,
        "external-prior-pending: the composition is still live before the owner ends it",
        std::to_string(compositions));

  // Terminate before the abandoned plan is released, so no cleanup plan exists yet. The
  // only evidence is the termination notification that arrives while cleanup waits.
  if (!TerminateOwnedTestComposition(setup, "external-prior-pending")) {
    return Finish();
  }
  const auto after_owner = setup.document->diagnostics().store;

  setup.host->SetHoldGrantMessages(false);
  const std::size_t released = setup.host->ReleaseHeldGrantMessages();
  Check(released >= 1, "external-prior-pending: the abandoned grant was released",
        std::to_string(released));
  const DriveResult drained =
      DriveToCompletion(coordinator, *setup.host, *setup.harness.executor, 20000);
  Check(drained.completed && drained.final_state.retirement.retiring_bindings == 0,
        "external-prior-pending: the retirement reached a terminal state",
        DescribeSnapshot(drained.final_state));
  const RetirementSnapshot& retired = drained.final_state.retirement;
  std::printf("[coordinator-win] evidence retire-external-prior-pending: %s\n",
              DescribeSnapshot(drained.final_state).c_str());
  Check(retired.last_cleanup_state == CleanupState::kTerminatedExternally &&
            retired.settled == 1 && retired.externally_terminated == 1 &&
            retired.finished == 0 && retired.already_ordinary == 0 &&
            retired.blocked == 0 && retired.cleanup_end_calls == 0,
        "external-prior-pending: the external end is attributed, never Finished",
        DescribeSnapshot(drained.final_state));
  Check(retired.last_cleanup_owned_at_start &&
            retired.last_cleanup_live_at_start &&
            !retired.last_cleanup_external_at_start &&
            !retired.last_cleanup_ended_before_at_start &&
            !retired.last_cleanup_plan_submitted &&
            !retired.last_cleanup_end_issued &&
            retired.last_cleanup_termination_notifications_at_start == 0 &&
            retired.last_cleanup_termination_notifications_at_settle > 0 &&
            RetirementCleanupEvidenceConsistent(retired) &&
            RetirementAccountingBalanced(retired),
        "external-prior-pending: the notification that arrived while waiting is the "
        "evidence; no cleanup plan or end call existed",
        DescribeSnapshot(drained.final_state));
  const auto after = setup.document->diagnostics().store;
  Check(after.text_mutations == after_owner.text_mutations &&
            after.set_text_calls == after_owner.set_text_calls &&
            after.composition_end_calls == after_owner.composition_end_calls,
        "external-prior-pending: cleanup issued no SetText and no end call",
        "mutations=" + std::to_string(after.text_mutations) +
            " set_text=" + std::to_string(after.set_text_calls) +
            " end=" + std::to_string(after.composition_end_calls));
  CheckDocText(setup.document->text_utf16(), L"he",
               "external-prior-pending: the displayed text survived verbatim");
  return Finish();
}

// --- 6e. a blocked cleanup stays blocked, with no destructive fallback -------------------

int RunRetirementComposePermissionDenied() {
  ControlledCase setup;
  if (!SetupControlledCase(&setup, TestDocumentOptions{}, "retire-permission-denied")) {
    return Finish();
  }
  ContextCoordinator& coordinator = *setup.coordinator;
  for (wchar_t letter : {L'h', L'e'}) {
    const executor_tests::TextKey key = Letter(letter);
    TypeKeyAndDrive(coordinator, *setup.host, *setup.harness.executor, key.event,
                    key.text, "permission-denied: composition letter");
  }
  const auto before = setup.document->diagnostics().store;
  const HRESULT set_hr = setup.document->SetContextCompartment(
      GUID_COMPARTMENT_KEYBOARD_DISABLED, setup.host->client_id(), 1);
  Check(SUCCEEDED(set_hr), "permission-denied: the real disabled compartment was set",
        epocanva::tsf::edit::test::HrText(set_hr));

  coordinator.InvalidateBinding();
  const DriveResult drained =
      DriveToCompletion(coordinator, *setup.host, *setup.harness.executor, 20000);
  Check(drained.completed && drained.final_state.retirement.retiring_bindings == 0,
        "permission-denied: the retirement reached a terminal state",
        DescribeSnapshot(drained.final_state));
  const RetirementSnapshot& retired = drained.final_state.retirement;
  std::printf("[coordinator-win] evidence retire-permission-denied: %s\n",
              DescribeSnapshot(drained.final_state).c_str());
  Check(retired.last_cleanup_state == CleanupState::kBlocked &&
            retired.last_cleanup_block == CleanupBlockReason::kPermissionDenied &&
            retired.settled == 1 && retired.blocked == 1 && retired.finished == 0 &&
            retired.already_ordinary == 0 && retired.externally_terminated == 0 &&
            retired.cleanup_end_calls == 0 && !retired.last_cleanup_end_issued &&
            RetirementCleanupEvidenceConsistent(retired) &&
            RetirementAccountingBalanced(retired),
        "permission-denied: cleanup is Blocked(PermissionDenied), not Finished",
        DescribeSnapshot(drained.final_state));
  const auto after = setup.document->diagnostics().store;
  Check(after.text_mutations == before.text_mutations &&
            after.set_text_calls == before.set_text_calls &&
            after.composition_end_calls == before.composition_end_calls,
        "permission-denied: no SetText and no end call on the refused cleanup",
        "mutations=" + std::to_string(after.text_mutations) +
            " set_text=" + std::to_string(after.set_text_calls) +
            " end=" + std::to_string(after.composition_end_calls));
  CheckDocText(setup.document->text_utf16(), L"he",
               "permission-denied: the displayed text was preserved");
  ULONG remaining = 0;
  Check(SUCCEEDED(oct::CountCompositions(setup.document->context(), &remaining)) &&
            remaining == 1,
        "permission-denied: the refused composition is still live",
        std::to_string(remaining));
  setup.document->SetContextCompartment(GUID_COMPARTMENT_KEYBOARD_DISABLED,
                                        setup.host->client_id(), 0);
  return Finish();
}

// --- 6f. a range mismatch blocks cleanup instead of deleting foreign text ---------------

int RunRetirementComposeRangeMismatch() {
  ControlledCase setup;
  if (!SetupControlledCase(&setup, TestDocumentOptions{}, "retire-range-mismatch")) {
    return Finish();
  }
  ContextCoordinator& coordinator = *setup.coordinator;
  for (wchar_t letter : {L'h', L'e'}) {
    const executor_tests::TextKey key = Letter(letter);
    TypeKeyAndDrive(coordinator, *setup.host, *setup.harness.executor, key.event,
                    key.text, "range-mismatch: composition letter");
  }
  CheckDocText(setup.document->text_utf16(), L"he",
               "range-mismatch: the composition holds he");

  std::string error;
  const TfClientId external =
      oct::ExternalClientId(setup.host->thread_manager_for_test(), &error);
  Check(external != 0, "range-mismatch: a distinct external client id", error);
  oct::ExternalEditResult edit;
  Check(oct::RunExternalEdit(*setup.host, setup.document, external,
                             oct::ExternalEditMode::kInsertText, 1, 1, L"X", &edit,
                             &error),
        "range-mismatch: the external edit inside the composition ran", error);
  CheckDocText(setup.document->text_utf16(), L"hXe",
               "range-mismatch: the backing text no longer matches the writer's cache");
  const auto before = setup.document->diagnostics().store;

  coordinator.InvalidateBinding();
  const DriveResult drained =
      DriveToCompletion(coordinator, *setup.host, *setup.harness.executor, 20000);
  Check(drained.completed && drained.final_state.retirement.retiring_bindings == 0,
        "range-mismatch: the retirement reached a terminal state",
        DescribeSnapshot(drained.final_state));
  const RetirementSnapshot& retired = drained.final_state.retirement;
  std::printf("[coordinator-win] evidence retire-range-mismatch: %s\n",
              DescribeSnapshot(drained.final_state).c_str());
  Check(retired.last_cleanup_state == CleanupState::kBlocked &&
            retired.last_cleanup_block == CleanupBlockReason::kRangeMismatch &&
            retired.settled == 1 && retired.blocked == 1 && retired.finished == 0 &&
            retired.already_ordinary == 0 && retired.externally_terminated == 0 &&
            !retired.last_cleanup_end_issued &&
            RetirementCleanupEvidenceConsistent(retired) &&
            RetirementAccountingBalanced(retired),
        "range-mismatch: cleanup is Blocked(RangeMismatch), not a success",
        DescribeSnapshot(drained.final_state));
  const auto after = setup.document->diagnostics().store;
  Check(after.text_mutations == before.text_mutations &&
            after.set_text_calls == before.set_text_calls &&
            after.composition_end_calls == before.composition_end_calls,
        "range-mismatch: no cleanup SetText and no end call",
        "mutations=" + std::to_string(after.text_mutations) +
            " set_text=" + std::to_string(after.set_text_calls) +
            " end=" + std::to_string(after.composition_end_calls));
  CheckDocText(setup.document->text_utf16(), L"hXe",
               "range-mismatch: the foreign text survived verbatim");
  return Finish();
}

// --- 7. invalidation while the edit is pending (async store) ----------------------------

int RunEditPendingInvalidate() {
  ControlledCase setup;
  TestDocumentOptions options;
  options.async_locks = true;
  if (!SetupControlledCase(&setup, options, "edit-pending-invalidate")) {
    return Finish();
  }
  ContextCoordinator& coordinator = *setup.coordinator;

  std::string error;
  TestDocumentOptions second_options;
  TestDocument* document_b = setup.host->CreateDocument(second_options, &error);
  Check(document_b != nullptr, "edit-pending: second document created", error);
  if (document_b == nullptr) {
    return Finish();
  }

  const executor_tests::TextKey h = Letter(L'h');
  WorkId id;
  Check(coordinator.SubmitKey(h.event, h.text, &id) == SubmitStatus::kAccepted,
        "edit-pending: submit accepted", "");

  // Advance to EditPending without dispatching the deferred grant message.
  const ULONGLONG deadline = ::GetTickCount64() + 20000;
  CoordinatorSnapshot snapshot = coordinator.Snapshot();
  while (snapshot.phase != WorkPhase::kEditPending) {
    coordinator.Pump();
    snapshot = coordinator.Snapshot();
    if (snapshot.phase == WorkPhase::kIdle) {
      break;
    }
    if (::GetTickCount64() >= deadline) {
      break;
    }
    setup.harness.executor->WaitForResult(snapshot.engine_request,
                                          std::chrono::milliseconds(50));
  }
  Check(snapshot.phase == WorkPhase::kEditPending,
        "edit-pending: the edit is queued behind the deferred lock",
        DescribeSnapshot(snapshot));
  Check(setup.document->diagnostics().store.text_mutations == 0,
        "edit-pending: nothing was written before the grant", "");
  const HANDLE completion = coordinator.EditCompletionEvent();
  Check(completion != nullptr, "edit-pending: the completion event is exposed", "");

  coordinator.InvalidateBinding();
  snapshot = coordinator.Snapshot();
  Check(snapshot.outcome == WorkOutcome::kAbandonedStale && snapshot.phase == WorkPhase::kIdle,
        "edit-pending: invalidation terminalizes the pending edit immediately",
        DescribeSnapshot(snapshot));
  Check(coordinator.Bind(document_b->context(), document_b->context_id(), 0,
                         setup.harness.client_b) == BindStatus::kOk,
        "edit-pending: the new binding is accepted at once", "");
  setup.permission.Set(epocanva::tsf::context::PermissionState::kAllowed);

  // The deferred grant now dispatches; the OLD callback is stale because BeginRetirement
  // abandoned it, and it must write to neither document. It publishes no completion fact,
  // so the host only dispatches the grant instead of waiting for an event that will not fire.
  setup.host->DispatchQueuedMessages();
  const auto first = setup.document->diagnostics().store;
  Check(first.text_mutations == 0 && first.composition_start_calls == 0,
        "edit-pending: the old callback changed nothing in the old document", "");
  Check(first.request_lock_calls >= 1, "edit-pending: the grant really was dispatched", "");
  CheckDocText(document_b->text_utf16(), L"",
               "edit-pending: the old callback reached no new document");

  const DriveResult retired =
      DriveToCompletion(coordinator, *setup.host, *setup.harness.executor, 20000);
  Check(retired.completed && retired.final_state.retirement.retiring_bindings == 0,
        "edit-pending: the retired client's close was consumed",
        DescribeSnapshot(retired.final_state));

  const executor_tests::TextKey a = Letter(L'a');
  const DriveResult typed = TypeKeyAndDrive(coordinator, *setup.host,
                                            *setup.harness.executor, a.event, a.text,
                                            "edit-pending: new binding types");
  CheckOutcome(typed.final_state, WorkOutcome::kApplied,
               "edit-pending: the new binding's work applied");
  CheckDocText(document_b->text_utf16(), L"a",
               "edit-pending: only the new key reached the new document");
  return Finish();
}

// --- 8. permission changed while the engine result was queued ---------------------------

int RunPermissionChangeQueued() {
  ControlledCase setup;
  if (!SetupControlledCase(&setup, TestDocumentOptions{}, "permission-queued")) {
    return Finish();
  }
  ContextCoordinator& coordinator = *setup.coordinator;
  auto* backend = setup.harness.backend;

  backend->HoldDispatches();
  const executor_tests::TextKey h = Letter(L'h');
  WorkId id;
  Check(coordinator.SubmitKey(h.event, h.text, &id) == SubmitStatus::kAccepted,
        "permission-queued: submit accepted while allowed", "");
  setup.permission.Set(epocanva::tsf::context::PermissionState::kDenied);
  backend->ReleaseDispatches();

  const DriveResult driven =
      DriveToCompletion(coordinator, *setup.host, *setup.harness.executor, 20000);
  Check(driven.completed, "permission-queued: the work terminated",
        DescribeSnapshot(driven.final_state));
  CheckOutcome(driven.final_state, WorkOutcome::kNeedsResync,
               "permission-queued: the changed permission is an engine/host divergence");
  Check(driven.final_state.cause == RejectCause::kPermissionChangedWhileQueued,
        "permission-queued: the cause names the permission change",
        DescribeRejectCause(driven.final_state.cause));
  Check(driven.final_state.gate == CoordinatorGate::kBlockedNeedsResync,
        "permission-queued: the binding is blocked, not quietly reusable",
        DescribeCoordinatorGate(driven.final_state.gate));
  Check(backend->dispatch_calls() == 1,
        "permission-queued: the engine had consumed the key (refusal is at apply time)",
        std::to_string(backend->dispatch_calls()));
  const auto diagnostics = setup.document->diagnostics().store;
  Check(diagnostics.request_lock_calls == 0,
        "permission-queued: no edit was ever requested", "");
  Check(diagnostics.text_mutations == 0,
        "permission-queued: the document is untouched", "");
  CheckDocText(setup.document->text_utf16(), L"",
               "permission-queued: the document is untouched");

  const executor_tests::TextKey e = Letter(L'e');
  WorkId second;
  const SubmitStatus blocked_submit = coordinator.SubmitKey(e.event, e.text, &second);
  Check(blocked_submit == SubmitStatus::kBlockedNeedsResync,
        "permission-queued: further submits are refused until resync",
        DescribeSubmitStatus(blocked_submit));
  Check(backend->dispatch_calls() == 1, "permission-queued: no second engine call", "");
  return Finish();
}

// --- 9. permission changed while the edit was pending (refused in the callback) ---------

int RunPermissionChangeEditPending() {
  ControlledCase setup;
  TestDocumentOptions options;
  options.async_locks = true;
  if (!SetupControlledCase(&setup, options, "permission-edit")) {
    return Finish();
  }
  ContextCoordinator& coordinator = *setup.coordinator;

  const executor_tests::TextKey h = Letter(L'h');
  WorkId id;
  Check(coordinator.SubmitKey(h.event, h.text, &id) == SubmitStatus::kAccepted,
        "permission-edit: submit accepted", "");
  const ULONGLONG deadline = ::GetTickCount64() + 20000;
  CoordinatorSnapshot snapshot = coordinator.Snapshot();
  while (snapshot.phase != WorkPhase::kEditPending) {
    coordinator.Pump();
    snapshot = coordinator.Snapshot();
    if (snapshot.phase == WorkPhase::kIdle || ::GetTickCount64() >= deadline) {
      break;
    }
    setup.harness.executor->WaitForResult(snapshot.engine_request,
                                          std::chrono::milliseconds(50));
  }
  Check(snapshot.phase == WorkPhase::kEditPending,
        "permission-edit: the edit is pending behind the deferred lock",
        DescribeSnapshot(snapshot));

  // A real metadata change while the lock is deferred. The coordinator's pre-edit query
  // already said Allowed, so only the P3.9 callback-time recheck can see this, and it must
  // refuse before any TSF call.
  setup.document->SetScopeAnswer(
      epocanva::tsf::edit::test::TestScopeAnswer::kScopes, {31L});  // IS_PASSWORD
  setup.host->PumpUntil(coordinator.EditCompletionEvent(), 5000);
  coordinator.Pump();
  snapshot = coordinator.Snapshot();
  Check(snapshot.phase == WorkPhase::kIdle &&
            snapshot.outcome == WorkOutcome::kNeedsResync,
        "permission-edit: the callback-time real-metadata check rejected the edit",
        DescribeSnapshot(snapshot));
  Check(snapshot.last_edit_reason == edit::EditRejectReason::kPermissionDenied,
        "permission-edit: P3.7 named the permission refusal",
        edit::DescribeEditRejectReason(snapshot.last_edit_reason));
  Check(snapshot.permission_rechecks == 1,
        "permission-edit: exactly one callback-time recheck ran",
        std::to_string(snapshot.permission_rechecks));
  const auto diagnostics = setup.document->diagnostics().store;
  // The deferred lock really was granted and the callback really ran; the refusal happened
  // inside the callback (P3.7's permission re-check), before any TSF call.
  Check(diagnostics.write_lock_grants == 1,
        "permission-edit: the callback ran inside one granted lock",
        std::to_string(diagnostics.write_lock_grants));
  Check(diagnostics.text_mutations == 0 && diagnostics.set_text_calls == 0,
        "permission-edit: no text call, no mutation", "");
  CheckDocText(setup.document->text_utf16(), L"", "permission-edit: the document is empty");
  return Finish();
}

// --- 10. clean edit refusals are never Applied -------------------------------------------

int RunRefusalsNotApplied() {
  struct Shape {
    const char* label;
    bool read_only;
    bool refuse_write_locks;
    bool disconnect;
  };
  const Shape shapes[] = {
      {"read-only", true, false, false},
      {"write-lock-refused", false, true, false},
      {"disconnected", false, false, true},
  };
  for (const Shape& shape : shapes) {
    ControlledCase setup;
    TestDocumentOptions options;
    options.read_only = shape.read_only;
    options.refuse_write_locks = shape.refuse_write_locks;
    if (!SetupControlledCase(&setup, options, shape.label)) {
      continue;
    }
    if (shape.disconnect) {
      setup.document->Disconnect();
    }
    const executor_tests::TextKey h = Letter(L'h');
    WorkId id;
    const SubmitStatus submitted =
        setup.coordinator->SubmitKey(h.event, h.text, &id);
    Check(submitted == SubmitStatus::kAccepted,
          std::string(shape.label) + ": submit accepted (the manual seam says Allowed)",
          DescribeSubmitStatus(submitted));
    // The engine has run by the time these shapes are exercised. A refusal or a lock that
    // is never granted therefore leaves the engine view and the document divergent; the
    // terminal state is NeedsResync, and a callback that never arrives is ended by the
    // explicit invalidation below -- never by assuming DoEditSession will run.
    DriveResult driven =
        DriveToCompletion(*setup.coordinator, *setup.host, *setup.harness.executor, 5000);
    if (!driven.completed) {
      setup.coordinator->InvalidateBinding();
      const CoordinatorSnapshot after = setup.coordinator->Snapshot();
      Check(after.phase == WorkPhase::kIdle &&
                after.outcome == WorkOutcome::kAbandonedStale,
            std::string(shape.label) +
                ": a never-completing edit is terminal after explicit invalidation",
            DescribeSnapshot(after));
      driven.final_state = after;
    } else {
      Check(driven.final_state.outcome == WorkOutcome::kNeedsResync,
            std::string(shape.label) +
                ": engine ran but the edit did not apply -> NeedsResync",
            DescribeSnapshot(driven.final_state));
      Check(driven.final_state.last_edit_status == edit::EditStatus::kRejected,
            std::string(shape.label) + ": P3.7 reported a clean rejection",
            edit::DescribeEditStatus(driven.final_state.last_edit_status));
      Check(driven.final_state.gate == CoordinatorGate::kBlockedNeedsResync,
            std::string(shape.label) + ": the binding is blocked",
            DescribeCoordinatorGate(driven.final_state.gate));
    }
    const auto diagnostics = setup.document->diagnostics().store;
    Check(diagnostics.text_mutations == 0,
          std::string(shape.label) + ": the document was never mutated",
          std::to_string(diagnostics.text_mutations));
    CheckDocText(setup.document->text_utf16(), L"",
                 std::string(shape.label) + ": the document is empty");
    std::printf("[coordinator-win] evidence refusal %s: reason=%s mutations=%llu\n",
                shape.label,
                edit::DescribeEditRejectReason(driven.final_state.last_edit_reason),
                static_cast<unsigned long long>(diagnostics.text_mutations));
  }
  return Finish();
}

// --- 11. engine fault -> NeedsResync, blocked, no replay ---------------------------------

int RunNeedsResyncFaultCase() {
  // A dedicated harness whose backend throws once inside Dispatch: the existing controlled
  // seam (tests/windows/engine_executor/controlled_backend), not a new fault framework.
  std::string error;
  auto host = TestHost::Create(&error);
  Check(host != nullptr, "needs-resync: host created", error);
  if (host == nullptr) {
    return Finish();
  }
  TestDocumentOptions options;
  TestDocument* document = host->CreateDocument(options, &error);
  Check(document != nullptr, "needs-resync: document created", error);
  if (document == nullptr) {
    return Finish();
  }

  executor_tests::ControlledBackend::Configuration configuration;
  configuration.throw_on_dispatch_once = true;
  auto* raw_backend = new executor_tests::ControlledBackend(configuration);
  engine::ExecutorOptions executor_options;
  executor_options.capacity = 8;
  auto executor = engine::EngineExecutorTestFactory::Create(
      executor_options, std::unique_ptr<engine::EngineBackend>(raw_backend));
  Check(executor->Start() == engine::StartStatus::kOk, "needs-resync: executor started", "");
  engine::ClientId client{};
  engine::RequestId create = executor_tests::SubmitCreateOrFail(
      *executor, "controlled",
      engine::ControllerOptions{true, input::InputPermission::kAllowed}, &client,
      "needs-resync client");
  if (!create.valid()) {
    return Finish();
  }
  executor_tests::TakeOrFail(*executor, create, 10000, "needs-resync create");
  engine::ClientId client_b{};
  engine::RequestId create_b = executor_tests::SubmitCreateOrFail(
      *executor, "controlled",
      engine::ControllerOptions{true, input::InputPermission::kAllowed}, &client_b,
      "needs-resync second client");
  if (!create_b.valid()) {
    return Finish();
  }
  executor_tests::TakeOrFail(*executor, create_b, 10000, "needs-resync create b");

  ManualPermissionSource permission;
  permission.Set(epocanva::tsf::context::PermissionState::kAllowed);
  TestTargetRevalidator target;
  ContextCoordinator coordinator(*executor, host->client_id(), permission);
  coordinator.AttachTargetRevalidator(&target);
  Check(coordinator.Bind(document->context(), document->context_id(), 0, client) ==
            BindStatus::kOk,
        "needs-resync: bound", "");

  const executor_tests::TextKey h = Letter(L'h');
  const DriveResult driven =
      TypeKeyAndDrive(coordinator, *host, *executor, h.event, h.text, "needs-resync: type");
  CheckOutcome(driven.final_state, WorkOutcome::kNeedsResync,
               "needs-resync: an engine fault becomes NeedsResync");
  Check(driven.final_state.gate == CoordinatorGate::kBlockedNeedsResync,
        "needs-resync: the coordinator is blocked",
        DescribeCoordinatorGate(driven.final_state.gate));
  Check(raw_backend->dispatch_calls() == 0,
        "needs-resync: the faulting call made no completed dispatch", "");
  Check(document->diagnostics().store.text_mutations == 0,
        "needs-resync: nothing was written", "");

  // Further input is refused, not auto-retried and not replayed.
  const executor_tests::TextKey e = Letter(L'e');
  WorkId second;
  Check(coordinator.SubmitKey(e.event, e.text, &second) ==
            SubmitStatus::kBlockedNeedsResync,
        "needs-resync: the next submit is refused", "");
  Check(raw_backend->dispatch_calls() == 0,
        "needs-resync: no key was replayed to the engine", "");
  Check(coordinator.SubmitCandidate(0, &second) == SubmitStatus::kBlockedNeedsResync,
        "needs-resync: candidate submits are refused too", "");

  // An explicit invalidation is the documented way out; the same document re-binds with a
  // fresh engine client after the retired one's close has been consumed.
  coordinator.InvalidateBinding();
  const DriveResult retired =
      DriveToCompletion(coordinator, *host, *executor, 20000);
  Check(retired.completed && retired.final_state.retirement.retiring_bindings == 0,
        "needs-resync: the old client was reclaimed before the re-bind",
        DescribeSnapshot(retired.final_state));
  Check(coordinator.Bind(document->context(), document->context_id(), 0,
                         client_b) == BindStatus::kOk,
        "needs-resync: re-bind after invalidation", "");
  const DriveResult next =
      TypeKeyAndDrive(coordinator, *host, *executor, e.event, e.text, "needs-resync: next");
  CheckOutcome(next.final_state, WorkOutcome::kApplied,
               "needs-resync: the re-bound coordinator works again");
  CheckDocText(document->text_utf16(), L"e", "needs-resync: only the new key is in the doc");
  coordinator.Close();
  host->Shutdown();
  return Finish();
}

// --- 12. close with pending work ----------------------------------------------------------

int RunClosePendingWork() {
  ControlledCase setup;
  if (!SetupControlledCase(&setup, TestDocumentOptions{}, "close-pending")) {
    return Finish();
  }
  ContextCoordinator& coordinator = *setup.coordinator;
  auto* backend = setup.harness.backend;

  backend->HoldDispatches();
  const executor_tests::TextKey h = Letter(L'h');
  WorkId id;
  Check(coordinator.SubmitKey(h.event, h.text, &id) == SubmitStatus::kAccepted,
        "close-pending: submit accepted while held", "");
  // Capture the executor request before Close clears the pending fields; the host needs it
  // to show the leftover result honestly.
  const engine::RequestId request = coordinator.Snapshot().engine_request;

  coordinator.Close();
  CoordinatorSnapshot snapshot = coordinator.Snapshot();
  Check(snapshot.closed && snapshot.outcome == WorkOutcome::kAbandonedStale &&
            snapshot.phase == WorkPhase::kIdle,
        "close-pending: close terminalized the work", DescribeSnapshot(snapshot));
  coordinator.Pump();
  snapshot = coordinator.Snapshot();
  Check(snapshot.outcome == WorkOutcome::kAbandonedStale && snapshot.pumps == 1,
        "close-pending: pumping a closed coordinator does nothing", "");

  const executor_tests::TextKey e = Letter(L'e');
  WorkId second;
  Check(coordinator.SubmitKey(e.event, e.text, &second) == SubmitStatus::kClosed,
        "close-pending: submit after close is refused", "");
  Check(coordinator.Bind(setup.document->context(), setup.document->context_id(), 0,
                         setup.harness.client_a) == BindStatus::kClosed,
        "close-pending: bind after close is refused", "");

  // The held dispatch completes after Close. Close also submitted a close for the retired
  // client; both requests and both results are reported through the closed-retirement
  // snapshot instead of being forgotten.
  const RetirementSnapshot pending = snapshot.retirement;
  Check(pending.pending_engine_request == request,
        "close-pending: the unconsumed dispatch request is reported", "");
  Check(pending.pending_close_request.valid(),
        "close-pending: the close request is reported", "");
  Check(pending.unresolved_engine_results >= 1 &&
            pending.unresolved_close_requests >= 1,
        "close-pending: the leftovers are counted honestly, not hidden", "");
  backend->ReleaseDispatches();
  engine::CommandResult result;
  engine::WaitStatus wait = engine::WaitStatus::kTimedOut;
  for (int attempt = 0; attempt < 200 && wait != engine::WaitStatus::kReady; ++attempt) {
    wait = setup.harness.executor->WaitForResult(request, std::chrono::milliseconds(50));
  }
  Check(wait == engine::WaitStatus::kReady, "close-pending: the leftover result completed",
        "");
  engine::WaitStatus close_wait = engine::WaitStatus::kTimedOut;
  for (int attempt = 0; attempt < 200 &&
                          close_wait != engine::WaitStatus::kReady; ++attempt) {
    close_wait = setup.harness.executor->WaitForResult(
        pending.pending_close_request, std::chrono::milliseconds(50));
  }
  Check(close_wait == engine::WaitStatus::kReady,
        "close-pending: the close result completed", "");
  const engine::CapacityUsage before_take = setup.harness.executor->capacity();
  Check(before_take.occupied == 2,
        "close-pending: dispatch and close occupy two slots (honest accounting)",
        std::to_string(before_take.occupied));
  Check(setup.harness.executor->TakeResult(request, &result) == engine::TakeStatus::kTaken,
        "close-pending: the host takes the dispatch result once", "");
  engine::CommandResult close_result;
  Check(setup.harness.executor->TakeResult(pending.pending_close_request,
                                           &close_result) ==
            engine::TakeStatus::kTaken,
        "close-pending: the host takes the close result once", "");
  Check(setup.harness.executor->capacity().occupied == 0,
        "close-pending: both slots are released", "");
  Check(result.kind == engine::CommandKind::kDispatch,
        "close-pending: the leftover result is the dispatched key command", "");
  Check(close_result.kind == engine::CommandKind::kCloseClient &&
            close_result.ok(),
        "close-pending: the close result is a successful client close", "");
  const engine::ClientUsage clients = setup.harness.executor->clients();
  Check(clients.close_requests == 1 && clients.close_results_taken == 1,
        "close-pending: one close request and one consumed close result",
        std::to_string(clients.close_requests) + "/" +
            std::to_string(clients.close_results_taken));
  Check(clients.closed == 1 && clients.live == 1,
        "close-pending: the bound client is closed; only the harness's unused client is live",
        "closed=" + std::to_string(clients.closed) +
            " live=" + std::to_string(clients.live));
  CheckDocText(setup.document->text_utf16(), L"",
               "close-pending: the document was never written");
  return Finish();
}

// --- 13. pumping is idempotent -----------------------------------------------------------

int RunPumpIdempotent() {
  ControlledCase setup;
  if (!SetupControlledCase(&setup, TestDocumentOptions{}, "pump-idempotent")) {
    return Finish();
  }
  ContextCoordinator& coordinator = *setup.coordinator;
  const wchar_t letters[] = {L'h', L'e', L'l'};
  for (wchar_t letter : letters) {
    const executor_tests::TextKey key = Letter(letter);
    TypeKeyAndDrive(coordinator, *setup.host, *setup.harness.executor, key.event, key.text,
                    "pump-idempotent: type");
  }
  const auto before = setup.document->diagnostics().store;
  const CoordinatorSnapshot before_snapshot = coordinator.Snapshot();
  for (int index = 0; index < 5; ++index) {
    coordinator.Pump();
  }
  const auto after = setup.document->diagnostics().store;
  const CoordinatorSnapshot after_snapshot = coordinator.Snapshot();
  Check(after.text_mutations == before.text_mutations,
        "pump-idempotent: extra pumps wrote nothing",
        std::to_string(after.text_mutations));
  Check(after.request_lock_calls == before.request_lock_calls,
        "pump-idempotent: extra pumps requested no lock", "");
  Check(after_snapshot.engine_results_taken == before_snapshot.engine_results_taken &&
            after_snapshot.edits_applied == before_snapshot.edits_applied,
        "pump-idempotent: extra pumps consumed nothing new", "");
  CheckDocText(setup.document->text_utf16(), L"hel",
               "pump-idempotent: the document is stable");
  return Finish();
}

// --- 14. the falsifiability control ------------------------------------------------------

int RunCanFail() {
  Check(false, "self-check: this claim is deliberately false", "false");
  return kExitSelfCheckFailed;
}

}  // namespace

// P3.13 CONTROLLED SEAM, NOT A SYSTEM NOTIFICATION. The test-owned revalidator is set to
// each live-target status in turn; this pins the coordinator's status mapping and proves
// that no refusal dispatches to the engine. The real observer-backed cases cover the actual
// TSF facts separately.
int RunTargetStatusMapping() {
  ControlledCase setup;
  if (!SetupControlledCase(&setup, TestDocumentOptions{L"", 0, 0},
                           "target-status")) {
    return Finish();
  }
  ContextCoordinator& coordinator = *setup.coordinator;
  const std::uint64_t dispatches_before =
      setup.harness.backend->dispatch_calls();
  const executor_tests::TextKey h = Letter(L'h');
  auto refuse = [&](epocanva::tsf::context::TargetRevalidationStatus status,
                    SubmitStatus expected, const char* label) {
    setup.target.SetStatus(status);
    WorkId id;
    const SubmitStatus submitted = coordinator.SubmitKey(h.event, h.text, &id);
    Check(submitted == expected, std::string("target-status: ") + label,
          DescribeSubmitStatus(submitted));
    Check(!id.valid(), std::string("target-status: ") + label +
                           " created no work item", "");
    const CoordinatorSnapshot snapshot = coordinator.Snapshot();
    Check(snapshot.last_submit_status == expected,
          std::string("target-status: ") + label + " is recorded", "");
    Check(snapshot.last_target_revalidation.status == status,
          std::string("target-status: ") + label + " names the live status",
          epocanva::tsf::context::DescribeTargetRevalidationStatus(
              snapshot.last_target_revalidation.status));
    Check(setup.harness.backend->dispatch_calls() == dispatches_before,
          std::string("target-status: ") + label + " made no engine call", "");
    Check(coordinator.Snapshot().works_accepted == 0,
          std::string("target-status: ") + label + " accepted no work", "");
  };
  refuse(epocanva::tsf::context::TargetRevalidationStatus::kNoDocumentManager,
         SubmitStatus::kTargetChanged, "no document manager -> TargetChanged");
  refuse(epocanva::tsf::context::TargetRevalidationStatus::kNoTopContext,
         SubmitStatus::kTargetChanged, "no top context -> TargetChanged");
  refuse(epocanva::tsf::context::TargetRevalidationStatus::kContextMismatch,
         SubmitStatus::kTargetChanged, "context mismatch -> TargetChanged");
  refuse(epocanva::tsf::context::TargetRevalidationStatus::kGenerationMismatch,
         SubmitStatus::kTargetChanged, "generation mismatch -> TargetChanged");
  refuse(epocanva::tsf::context::TargetRevalidationStatus::kThreadFocusUnconfirmed,
         SubmitStatus::kTargetUnverified, "unconfirmed focus -> TargetUnverified");
  refuse(epocanva::tsf::context::TargetRevalidationStatus::kThreadFocusLost,
         SubmitStatus::kThreadFocusLost, "killed focus -> ThreadFocusLost");
  refuse(epocanva::tsf::context::TargetRevalidationStatus::kQueryFailed,
         SubmitStatus::kTargetUnverified, "query failure -> TargetUnverified");

  // P3.13-R2: the refusal snapshot keeps the evidence of THAT decision. It must not be
  // replaced by live observer state refreshed later (the controller is not re-queried here).
  {
    context::TargetRevalidation failure;
    failure.status = context::TargetRevalidationStatus::kQueryFailed;
    failure.failure_stage = context::TargetFailureStage::kGetTop;
    failure.query_hr = E_ACCESSDENIED;
    failure.evidence.policy = context::ThreadFocusPolicy::kLabWin32Fallback;
    failure.evidence.thread_focus_query_ok = true;
    failure.evidence.thread_focus = false;
    failure.evidence.thread_focus_query_hr = S_FALSE;
    failure.evidence.has_document_manager = true;
    failure.evidence.get_focus_hr = S_OK;
    failure.evidence.get_top_hr = E_ACCESSDENIED;
    setup.target.SetResult(failure);
    WorkId id;
    const SubmitStatus submitted = coordinator.SubmitKey(h.event, h.text, &id);
    const CoordinatorSnapshot snapshot = coordinator.Snapshot();
    Check(submitted == SubmitStatus::kTargetUnverified && !id.valid(),
          "target-status: a GetTop query failure refuses without work",
          DescribeSubmitStatus(submitted));
    Check(snapshot.last_target_revalidation.failure_stage ==
                  context::TargetFailureStage::kGetTop &&
              snapshot.last_target_revalidation.query_hr == E_ACCESSDENIED,
          "target-status: the failure stage and real HRESULT are preserved",
          context::DescribeTargetFailureStage(
              snapshot.last_target_revalidation.failure_stage));
    Check(snapshot.last_target_revalidation.evidence.thread_focus_query_ok &&
              !snapshot.last_target_revalidation.evidence.thread_focus &&
              snapshot.last_target_revalidation.evidence.thread_focus_query_hr ==
                  S_FALSE &&
              snapshot.last_target_revalidation.evidence.get_top_hr ==
                  E_ACCESSDENIED,
          "target-status: the per-query evidence of the refusal is preserved", "");
    Check(setup.harness.backend->dispatch_calls() == dispatches_before,
          "target-status: the GetTop failure made no engine call", "");
  }

  {
    context::TargetRevalidation unconfirmed;
    unconfirmed.status =
        context::TargetRevalidationStatus::kThreadFocusUnconfirmed;
    unconfirmed.evidence.thread_focus_query_ok = true;
    unconfirmed.evidence.thread_focus = false;
    unconfirmed.evidence.thread_focus_query_hr = S_FALSE;
    setup.target.SetResult(unconfirmed);
    WorkId id;
    const SubmitStatus submitted = coordinator.SubmitKey(h.event, h.text, &id);
    const CoordinatorSnapshot snapshot = coordinator.Snapshot();
    Check(submitted == SubmitStatus::kTargetUnverified && !id.valid(),
          "target-status: a successful FALSE thread-focus query refuses", "");
    Check(snapshot.last_target_revalidation.status ==
                  context::TargetRevalidationStatus::kThreadFocusUnconfirmed &&
              snapshot.last_target_revalidation.failure_stage ==
                  context::TargetFailureStage::kNone &&
              snapshot.last_target_revalidation.query_hr == S_OK,
          "target-status: a valid FALSE BOOL is not reported as a query failure",
          context::DescribeTargetRevalidationStatus(
              snapshot.last_target_revalidation.status));
    Check(snapshot.last_target_revalidation.evidence.thread_focus_query_ok &&
              !snapshot.last_target_revalidation.evidence.thread_focus,
          "target-status: the FALSE BOOL stays in the refusal evidence", "");
  }

  setup.target.SetStatus(epocanva::tsf::context::TargetRevalidationStatus::kOk);
  WorkId id;
  Check(coordinator.SubmitKey(h.event, h.text, &id) == SubmitStatus::kAccepted,
        "target-status: a current target is accepted", "");
  const DriveResult driven = DriveToCompletion(
      coordinator, *setup.host, *setup.harness.executor, 20000);
  CheckOutcome(driven.final_state, WorkOutcome::kApplied,
               "target-status: the accepted work applies");
  return Finish();
}

int RunCoordinationCase(const std::string& case_name) {
  if (case_name == "conversion-pure") {
    return RunConversionPure();
  }
  if (case_name == "bind-gates") {
    return RunBindAndGates();
  }
  if (case_name == "target-status-mapping") {
    return RunTargetStatusMapping();
  }
  if (case_name == "busy-single-flight") {
    return RunBusySingleFlight();
  }
  if (case_name == "composition-progress-controlled") {
    return RunCompositionProgressControlled();
  }
  if (case_name == "evaluate-pure") {
    return RunEvaluatePure();
  }
  if (case_name == "engine-pending-invalidate") {
    return RunEnginePendingInvalidate();
  }
  if (case_name == "retirement-queue-full") {
    return RunRetirementQueueFull();
  }
  if (case_name == "retirement-compose-withheld") {
    return RunRetirementComposeWithheld();
  }
  if (case_name == "retirement-compose-external-before") {
    return RunRetirementComposeExternallyTerminatedBeforePump();
  }
  if (case_name == "retirement-compose-external-pending") {
    return RunRetirementComposeExternallyTerminatedWhilePending();
  }
  if (case_name == "retirement-compose-external-prior-pending") {
    return RunRetirementComposeExternallyTerminatedWhilePriorPlanPending();
  }
  if (case_name == "retirement-compose-permission-denied") {
    return RunRetirementComposePermissionDenied();
  }
  if (case_name == "retirement-compose-range-mismatch") {
    return RunRetirementComposeRangeMismatch();
  }
  if (case_name == "edit-pending-invalidate") {
    return RunEditPendingInvalidate();
  }
  if (case_name == "permission-change-queued") {
    return RunPermissionChangeQueued();
  }
  if (case_name == "permission-change-edit-pending") {
    return RunPermissionChangeEditPending();
  }
  if (case_name == "refusals-not-applied") {
    return RunRefusalsNotApplied();
  }
  if (case_name == "needs-resync-block") {
    return RunNeedsResyncFaultCase();
  }
  if (case_name == "close-pending-work") {
    return RunClosePendingWork();
  }
  if (case_name == "pump-idempotent") {
    return RunPumpIdempotent();
  }
  if (case_name == "can-fail") {
    return RunCanFail();
  }
  return -1;
}

}  // namespace test
}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva
