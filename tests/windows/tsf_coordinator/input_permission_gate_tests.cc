// EpoCanva P3.9 -- coordinator admission over REAL TSF context metadata.
//
// WHAT IS REAL HERE. Each case creates the production PermissionSource
// (CreateRealPermissionSource) and a real P3.7 TSF host/document. Read-only comes from
// the store's real GetStatus; KEYBOARD_DISABLED and EMPTYCONTEXT are written through the
// real ITfCompartmentMgr/ITfCompartment path; InputScope is published by the store's
// documented RequestSupportedAttrs/RetrieveRequestedAttrs half and read through
// GetAppProperty + ITfReadOnlyProperty::GetValue inside real edit sessions. The engine is
// the controlled scheduling backend, so "engine call count" is a direct counter.
//
// WHAT IS NOT HERE. No mock PermissionSource supplies an Allowed verdict to an
// integration case. The one seam class at the end (WrongGenerationSource) is a
// deliberately mis-stamped source used only to prove the coordinator's identity check;
// it is labelled as a seam and is not evidence about TSF metadata.
#include <windows.h>
#include <msctf.h>

#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "coordinator_test_support.h"
#include "input_permission_gate_tests.h"
#include "permission_query.h"

namespace epocanva {
namespace tsf {
namespace coordinator {
namespace test {

namespace {

namespace context = epocanva::tsf::context;
namespace edit = epocanva::tsf::edit;
namespace executor_tests = epocanva::tests::engine_executor;
using edit::test::TestDocument;
using edit::test::TestDocumentOptions;
using edit::test::TestInputScope;
using edit::test::TestScopeAnswer;
using edit::test::TestTextStore;

executor_tests::TextKey Letter(wchar_t character) {
  return executor_tests::MakeTextKey(character, ::GetKeyboardLayout(0));
}

struct GateCase {
  std::unique_ptr<TestHost> host;
  ControlledHarness harness;
  // Declared before the coordinator: the coordinator stores a reference, so it must be
  // destroyed first (reverse member order).
  std::unique_ptr<context::PermissionSource> permission;
  // P3.13 controlled target authority; not a system notification.
  TestTargetRevalidator target;
  std::unique_ptr<ContextCoordinator> coordinator;
  TestDocument* document = nullptr;

  ~GateCase() {
    if (coordinator != nullptr) {
      coordinator->Close();
    }
    if (host != nullptr) {
      host->Shutdown();
    }
  }
};

bool SetupGateCase(GateCase* setup, const TestDocumentOptions& options,
                   const char* label) {
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
  setup->permission = context::CreateRealPermissionSource(setup->host->client_id());
  if (setup->permission == nullptr) {
    Check(false, std::string(label) + ": production permission source created", "null");
    return false;
  }
  setup->coordinator = std::make_unique<ContextCoordinator>(
      *setup->harness.executor, setup->host->client_id(), *setup->permission);
  setup->coordinator->AttachTargetRevalidator(&setup->target);
  const BindStatus bound = setup->coordinator->Bind(
      setup->document->context(), setup->document->context_id(),
      options.initial_selection_start_utf16, setup->harness.client_a);
  Check(bound == BindStatus::kOk, std::string(label) + ": bound",
        DescribeBindStatus(bound));
  return bound == BindStatus::kOk;
}

DriveResult TypeLetter(GateCase& setup, wchar_t character, const char* label,
                       DWORD timeout_ms = 20000) {
  const executor_tests::TextKey key = Letter(character);
  WorkId id;
  const SubmitStatus submitted =
      setup.coordinator->SubmitKey(key.event, key.text, &id);
  Check(submitted == SubmitStatus::kAccepted, std::string(label) + ": submit accepted",
        DescribeSubmitStatus(submitted));
  DriveResult driven;
  if (submitted != SubmitStatus::kAccepted) {
    driven.final_state = setup.coordinator->Snapshot();
    return driven;
  }
  return DriveToCompletion(*setup.coordinator, *setup.host, *setup.harness.executor,
                           timeout_ms);
}

bool ReachEditPending(GateCase& setup, const char* label) {
  const ULONGLONG deadline = ::GetTickCount64() + 20000;
  CoordinatorSnapshot snapshot = setup.coordinator->Snapshot();
  while (snapshot.phase != WorkPhase::kEditPending) {
    setup.coordinator->Pump();
    snapshot = setup.coordinator->Snapshot();
    if (snapshot.phase == WorkPhase::kIdle || ::GetTickCount64() >= deadline) {
      break;
    }
    setup.harness.executor->WaitForResult(snapshot.engine_request,
                                          std::chrono::milliseconds(50));
  }
  Check(snapshot.phase == WorkPhase::kEditPending,
        std::string(label) + ": the edit waits behind the deferred lock",
        DescribeSnapshot(snapshot));
  return snapshot.phase == WorkPhase::kEditPending;
}

// Asserts the submission was refused before any engine call or document write, and
// prints the two forbidden-effect counters as the case's evidence.
void CheckRefusedBeforeEngine(GateCase& setup, SubmitStatus actual, SubmitStatus want,
                              const char* label) {
  Check(actual == want, std::string(label) + ": refused before the engine",
        DescribeSubmitStatus(actual));
  const std::uint64_t dispatches = setup.harness.backend->dispatch_calls();
  const auto store = setup.document->diagnostics().store;
  Check(dispatches == 0, std::string(label) + ": engine calls",
        std::to_string(dispatches));
  Check(store.write_lock_grants == 0 && store.text_mutations == 0,
        std::string(label) + ": no write lock, no mutation",
        "grants=" + std::to_string(store.write_lock_grants) +
            " mutations=" + std::to_string(store.text_mutations));
  CheckDocText(setup.document->text_utf16(), L"",
               std::string(label) + ": the document is untouched");
  const CoordinatorSnapshot snapshot = setup.coordinator->Snapshot();
  Check(snapshot.phase == WorkPhase::kIdle && snapshot.outcome == WorkOutcome::kNone,
        std::string(label) + ": no work item was created", DescribeSnapshot(snapshot));
  std::printf("[coordinator-win] evidence permission-refusal %s: submit=%s state=%s "
              "engine_calls=%llu mutations=%llu\n",
              label, DescribeSubmitStatus(actual),
              edit::DescribeInputPermission(snapshot.permission),
              static_cast<unsigned long long>(dispatches),
              static_cast<unsigned long long>(store.text_mutations));
}

void SubmitH(GateCase& setup, WorkId* id, const char* label) {
  const executor_tests::TextKey h = Letter(L'h');
  const SubmitStatus submitted = setup.coordinator->SubmitKey(h.event, h.text, id);
  Check(submitted == SubmitStatus::kAccepted, std::string(label) + ": submit accepted",
        DescribeSubmitStatus(submitted));
}

// --- 1. ordinary editable context + repeated identical queries --------------------------

int RunRealAllowAndRepeat() {
  GateCase setup;
  if (!SetupGateCase(&setup, TestDocumentOptions{}, "allow")) {
    return Finish();
  }
  const struct {
    wchar_t letter;
    const wchar_t* want;
  } steps[] = {{L'h', L"h"}, {L'e', L"he"}, {L'l', L"hel"}};
  for (const auto& step : steps) {
    const DriveResult driven = TypeLetter(setup, step.letter, "allow: type");
    CheckOutcome(driven.final_state, WorkOutcome::kApplied,
                 "allow: the work applied");
    CheckDocText(setup.document->text_utf16(), step.want,
                 "allow: the document advanced");
  }

  const auto source = setup.permission->Diagnostics();
  const CoordinatorSnapshot snapshot = setup.coordinator->Snapshot();
  Check(setup.harness.backend->dispatch_calls() == 3,
        "allow: three engine dispatches", std::to_string(
            setup.harness.backend->dispatch_calls()));
  Check(snapshot.permission == edit::InputPermission::kAllowed,
        "allow: the cached verdict is Allowed", "");
  Check(source.queries_begun >= 6,
        "allow: every work item resolved submit-time and apply-time permission",
        std::to_string(source.queries_begun));
  Check(source.queries_completed == source.queries_begun,
        "allow: every query reached a verdict",
        std::to_string(source.queries_completed) + "/" +
            std::to_string(source.queries_begun));
  Check(snapshot.permission_queries_completed ==
            snapshot.permission_queries_begun,
        "allow: the coordinator consumed every verdict it began", "");
  Check(snapshot.permission_rechecks == 3,
        "allow: one real-metadata recheck ran per edit callback",
        std::to_string(snapshot.permission_rechecks));
  std::printf("[coordinator-win] evidence permission-allow: engine_calls=3 "
              "queries=%llu rechecks=%llu\n",
              static_cast<unsigned long long>(source.queries_begun),
              static_cast<unsigned long long>(snapshot.permission_rechecks));
  return Finish();
}

// --- 2-6. hard deny metadata -------------------------------------------------------------

int RunDenyShapes() {
  {
    TestDocumentOptions options;
    options.read_only = true;
    GateCase setup;
    if (SetupGateCase(&setup, options, "read-only")) {
      WorkId id;
      const executor_tests::TextKey h = Letter(L'h');
      const SubmitStatus submitted =
          setup.coordinator->SubmitKey(h.event, h.text, &id);
      CheckRefusedBeforeEngine(setup, submitted, SubmitStatus::kPermissionDenied,
                               "read-only");
      Check(setup.coordinator->Snapshot().permission ==
                edit::InputPermission::kDenied,
            "read-only: the cached verdict is Denied", "");
    }
  }

  const LONG disabled_values[] = {1L, 2L, std::numeric_limits<LONG>::min()};
  for (const LONG value : disabled_values) {
    const std::string label = "keyboard-disabled-" + std::to_string(value);
    GateCase setup;
    if (!SetupGateCase(&setup, TestDocumentOptions{}, label.c_str())) {
      continue;
    }
    const HRESULT set_hr = setup.document->SetContextCompartment(
        GUID_COMPARTMENT_KEYBOARD_DISABLED, setup.host->client_id(), value);
    Check(SUCCEEDED(set_hr), label + ": real compartment SetValue", std::to_string(set_hr));
    WorkId id;
    const executor_tests::TextKey h = Letter(L'h');
    const SubmitStatus submitted = setup.coordinator->SubmitKey(h.event, h.text, &id);
    CheckRefusedBeforeEngine(setup, submitted, SubmitStatus::kPermissionDenied,
                             label.c_str());
  }

  {
    GateCase setup;
    if (SetupGateCase(&setup, TestDocumentOptions{}, "empty-context")) {
      const HRESULT set_hr = setup.document->SetContextCompartment(
          GUID_COMPARTMENT_EMPTYCONTEXT, setup.host->client_id(), 1);
      Check(SUCCEEDED(set_hr), "empty-context: real compartment SetValue",
            std::to_string(set_hr));
      WorkId id;
      const executor_tests::TextKey h = Letter(L'h');
      const SubmitStatus submitted =
          setup.coordinator->SubmitKey(h.event, h.text, &id);
      CheckRefusedBeforeEngine(setup, submitted, SubmitStatus::kPermissionDenied,
                               "empty-context");
    }
  }

  const long sensitive_scopes[] = {31L, 61L, 63L, 64L, 65L, 66L};
  for (const long scope : sensitive_scopes) {
    const std::string label = "sensitive-scope-" + std::to_string(scope);
    TestDocumentOptions options;
    options.scope_answer = TestScopeAnswer::kScopes;
    options.input_scopes = {scope};
    GateCase setup;
    if (!SetupGateCase(&setup, options, label.c_str())) {
      continue;
    }
    WorkId id;
    const executor_tests::TextKey h = Letter(L'h');
    const SubmitStatus submitted = setup.coordinator->SubmitKey(h.event, h.text, &id);
    CheckRefusedBeforeEngine(setup, submitted, SubmitStatus::kPermissionDenied,
                             label.c_str());
    Check(setup.coordinator->Snapshot().permission == edit::InputPermission::kDenied,
          label + ": the real scope resolved Denied", "");
  }

  const std::vector<std::vector<long>> lists = {
      {31L, 1L, 28L}, {1L, 28L, 31L}, {0L, 64L, 29L}};
  for (std::size_t index = 0; index < lists.size(); ++index) {
    const std::string label = "sensitive-list-" + std::to_string(index);
    TestDocumentOptions options;
    options.scope_answer = TestScopeAnswer::kScopes;
    options.input_scopes = lists[index];
    GateCase setup;
    if (!SetupGateCase(&setup, options, label.c_str())) {
      continue;
    }
    WorkId id;
    const executor_tests::TextKey h = Letter(L'h');
    const SubmitStatus submitted = setup.coordinator->SubmitKey(h.event, h.text, &id);
    CheckRefusedBeforeEngine(setup, submitted, SubmitStatus::kPermissionDenied,
                             label.c_str());
  }
  return Finish();
}

// --- 7-8. absent / empty / query failure / wrong type / unknown --------------------------

int RunUnknownAbsentAndGap() {
  {
    TestDocumentOptions options;
    options.scope_answer = TestScopeAnswer::kWrongType;
    GateCase setup;
    if (SetupGateCase(&setup, options, "wrong-type")) {
      WorkId id;
      const executor_tests::TextKey h = Letter(L'h');
      const SubmitStatus submitted =
          setup.coordinator->SubmitKey(h.event, h.text, &id);
      CheckRefusedBeforeEngine(setup, submitted, SubmitStatus::kPermissionUnknown,
                               "wrong-type");
      Check(setup.coordinator->Snapshot().permission ==
                edit::InputPermission::kUnknown,
            "wrong-type: a malformed payload classifies Unknown", "");
    }
  }
  {
    // A real query failure: the store refuses GetSelection while the read lock is held,
    // so the InputScope range read cannot run. Other axes are readable, which is what
    // makes the result Unknown rather than Denied.
    TestDocumentOptions options;
    options.refuse_selection_reads = true;
    GateCase setup;
    if (SetupGateCase(&setup, options, "scope-query-failure")) {
      WorkId id;
      const executor_tests::TextKey h = Letter(L'h');
      const SubmitStatus submitted =
          setup.coordinator->SubmitKey(h.event, h.text, &id);
      CheckRefusedBeforeEngine(setup, submitted, SubmitStatus::kPermissionUnknown,
                               "scope-query-failure");
      const auto diagnostics = setup.permission->Diagnostics();
      Check(diagnostics.last_scope_stage == 1,
            "scope-query-failure: the failure is the selection read",
            std::to_string(diagnostics.last_scope_stage));
    }
  }
  {
    GateCase setup;
    if (SetupGateCase(&setup, TestDocumentOptions{}, "disconnected")) {
      setup.document->Disconnect();
      WorkId id;
      const executor_tests::TextKey h = Letter(L'h');
      const SubmitStatus submitted =
          setup.coordinator->SubmitKey(h.event, h.text, &id);
      CheckRefusedBeforeEngine(setup, submitted, SubmitStatus::kPermissionUnknown,
                               "disconnected");
    }
  }
  {
    // No published InputScope metadata is the documented ordinary default; it is Allowed
    // and the work proceeds through every real axis.
    GateCase setup;
    if (SetupGateCase(&setup, TestDocumentOptions{}, "scope-absent")) {
      const DriveResult driven = TypeLetter(setup, L'h', "scope-absent");
      CheckOutcome(driven.final_state, WorkOutcome::kApplied,
                   "scope-absent: the work applied");
      CheckDocText(setup.document->text_utf16(), L"h",
                   "scope-absent: the document advanced");
    }
  }
  {
    // A well-formed empty attribute value is another absence shape, not a denial.
    TestDocumentOptions options;
    options.scope_answer = TestScopeAnswer::kEmptyValue;
    GateCase setup;
    if (SetupGateCase(&setup, options, "scope-empty-value")) {
      const DriveResult driven = TypeLetter(setup, L'h', "scope-empty-value");
      CheckOutcome(driven.final_state, WorkOutcome::kApplied,
                   "scope-empty-value: the work applied");
    }
  }
  {
    // DOCUMENTED GAP: an owner whose RetrieveRequestedAttrs fails surfaces the same
    // E_FAIL msctf returns for an attribute-free owner, so the text-service side cannot
    // distinguish "broken" from "publishes nothing". This case pins the observed
    // behaviour and the limitation is stated in input-permission-contract.md.
    TestDocumentOptions options;
    options.scope_answer = TestScopeAnswer::kFail;
    GateCase setup;
    if (SetupGateCase(&setup, options, "scope-owner-failure")) {
      const DriveResult driven = TypeLetter(setup, L'h', "scope-owner-failure");
      CheckOutcome(driven.final_state, WorkOutcome::kApplied,
                   "scope-owner-failure: indistinguishable from absence (documented gap)");
      std::printf("[coordinator-win] evidence gap scope-owner-failure: "
                  "E_FAIL-is-indistinguishable-from-absent\n");
    }
  }
  return Finish();
}

// --- 9. an asynchronous permission read is not a verdict --------------------------------

int RunAsyncPermission() {
  TestDocumentOptions options;
  options.scope_answer = TestScopeAnswer::kScopes;
  options.input_scopes = {1L};
  options.async_locks = true;
  options.refuse_sync_locks = true;
  GateCase setup;
  if (!SetupGateCase(&setup, options, "async-permission")) {
    return Finish();
  }
  WorkId id;
  SubmitH(setup, &id, "async-permission");
  CoordinatorSnapshot snapshot = setup.coordinator->Snapshot();
  Check(snapshot.phase == WorkPhase::kPermissionPending,
        "async-permission: the work is parked in PermissionPending",
        DescribeSnapshot(snapshot));
  Check(setup.permission->Status() == context::PermissionQueryStatus::kPending,
        "async-permission: the real read session is pending",
        context::DescribePermissionQueryStatus(setup.permission->Status()));
  Check(setup.coordinator->PermissionCompletionEvent() != nullptr,
        "async-permission: the completion event is exposed", "");
  Check(setup.harness.backend->dispatch_calls() == 0,
        "async-permission: no engine call before the verdict", "");
  Check(setup.document->diagnostics().store.text_mutations == 0,
        "async-permission: no document write before the verdict", "");
  setup.coordinator->Pump();
  Check(setup.coordinator->Snapshot().phase == WorkPhase::kPermissionPending,
        "async-permission: Pump alone does not invent a verdict", "");
  Check(setup.harness.backend->dispatch_calls() == 0,
        "async-permission: still no engine call", "");

  Check(setup.host->PumpUntil(setup.coordinator->PermissionCompletionEvent(), 20000),
        "async-permission: the deferred read completed", "timeout");
  const DriveResult driven = DriveToCompletion(*setup.coordinator, *setup.host,
                                               *setup.harness.executor, 20000);
  Check(driven.completed && driven.final_state.outcome == WorkOutcome::kApplied,
        "async-permission: the work applied after the verdict",
        DescribeSnapshot(driven.final_state));
  CheckDocText(setup.document->text_utf16(), L"h",
               "async-permission: the document advanced");
  const auto diagnostics = setup.permission->Diagnostics();
  Check(diagnostics.read_sessions_async >= 2,
        "async-permission: submit-time and apply-time reads both used the async path",
        std::to_string(diagnostics.read_sessions_async));
  Check(diagnostics.queries_completed == diagnostics.queries_begun,
        "async-permission: every begun query completed",
        std::to_string(diagnostics.queries_completed) + "/" +
            std::to_string(diagnostics.queries_begun));
  std::printf("[coordinator-win] evidence async-permission: engine_calls=%llu "
              "async_reads=%llu mutations=%llu\n",
              static_cast<unsigned long long>(
                  setup.harness.backend->dispatch_calls()),
              static_cast<unsigned long long>(diagnostics.read_sessions_async),
              static_cast<unsigned long long>(
                  setup.document->diagnostics().store.text_mutations));
  return Finish();
}

// --- 10. Allowed at submit, Denied before the edit --------------------------------------

int RunFlipBeforeApply() {
  GateCase setup;
  if (!SetupGateCase(&setup, TestDocumentOptions{}, "flip-before-apply")) {
    return Finish();
  }
  setup.harness.backend->HoldDispatches();
  WorkId id;
  SubmitH(setup, &id, "flip-before-apply");
  Check(setup.coordinator->Snapshot().phase == WorkPhase::kEnginePending,
        "flip-before-apply: the engine result is owed",
        DescribeSnapshot(setup.coordinator->Snapshot()));
  Check(setup.coordinator->Snapshot().permission == edit::InputPermission::kAllowed,
        "flip-before-apply: the submit-time verdict was Allowed", "");

  setup.document->SetScopeAnswer(TestScopeAnswer::kScopes, {31L});  // IS_PASSWORD
  setup.harness.backend->ReleaseDispatches();
  const DriveResult driven = DriveToCompletion(*setup.coordinator, *setup.host,
                                               *setup.harness.executor, 20000);
  Check(driven.completed, "flip-before-apply: terminal",
        DescribeSnapshot(driven.final_state));
  CheckOutcome(driven.final_state, WorkOutcome::kNeedsResync,
               "flip-before-apply: engine ran, permission changed -> NeedsResync");
  Check(driven.final_state.cause == RejectCause::kPermissionChangedWhileQueued,
        "flip-before-apply: the cause names the permission change",
        DescribeRejectCause(driven.final_state.cause));
  Check(driven.final_state.gate == CoordinatorGate::kBlockedNeedsResync,
        "flip-before-apply: the binding is blocked",
        DescribeCoordinatorGate(driven.final_state.gate));
  Check(driven.final_state.permission == edit::InputPermission::kDenied,
        "flip-before-apply: the apply-time verdict is Denied", "");
  Check(setup.harness.backend->dispatch_calls() == 1,
        "flip-before-apply: the engine consumed exactly one key", "");
  const auto store = setup.document->diagnostics().store;
  Check(store.write_lock_grants == 0 && store.text_mutations == 0,
        "flip-before-apply: no edit was requested or written", "");
  CheckDocText(setup.document->text_utf16(), L"",
               "flip-before-apply: the document is empty");

  const executor_tests::TextKey e = Letter(L'e');
  WorkId second;
  const SubmitStatus blocked = setup.coordinator->SubmitKey(e.event, e.text, &second);
  Check(blocked == SubmitStatus::kBlockedNeedsResync,
        "flip-before-apply: restoring nothing can reuse the old engine state",
        DescribeSubmitStatus(blocked));
  Check(setup.harness.backend->dispatch_calls() == 1,
        "flip-before-apply: no replay reached the engine", "");
  std::printf("[coordinator-win] evidence flip-before-apply: engine_calls=1 "
              "write_lock_grants=%llu mutations=%llu gate=%s\n",
              static_cast<unsigned long long>(store.write_lock_grants),
              static_cast<unsigned long long>(store.text_mutations),
              DescribeCoordinatorGate(driven.final_state.gate));
  return Finish();
}

// --- 11. engine already ran, the edit is refused; restoring Allowed must not replay -----

int RunEngineRanEditRefused() {
  TestDocumentOptions options;
  options.async_locks = true;
  GateCase setup;
  if (!SetupGateCase(&setup, options, "engine-ran-edit-refused")) {
    return Finish();
  }
  WorkId id;
  SubmitH(setup, &id, "engine-ran-edit-refused");
  if (!ReachEditPending(setup, "engine-ran-edit-refused")) {
    return Finish();
  }
  Check(setup.harness.backend->dispatch_calls() == 1,
        "engine-ran-edit-refused: the controlled engine processed the key", "");
  Check(setup.harness.backend->Raw(setup.harness.client_a.value) == "h",
        "engine-ran-edit-refused: the engine holds the unshown preedit", "");
  CheckDocText(setup.document->text_utf16(), L"",
               "engine-ran-edit-refused: nothing has been written yet");

  // Flip the real scope while the write lock is deferred. The coordinator's pre-edit
  // query already said Allowed; only the callback-time real-metadata recheck can see this.
  setup.document->SetScopeAnswer(TestScopeAnswer::kScopes, {31L});  // IS_PASSWORD
  const HANDLE completion = setup.coordinator->EditCompletionEvent();
  Check(completion != nullptr, "engine-ran-edit-refused: completion event exposed", "");
  Check(setup.host->PumpUntil(completion, 5000),
        "engine-ran-edit-refused: the deferred lock was granted", "timeout");
  setup.coordinator->Pump();
  const CoordinatorSnapshot refused = setup.coordinator->Snapshot();
  Check(refused.phase == WorkPhase::kIdle &&
            refused.outcome == WorkOutcome::kNeedsResync,
        "engine-ran-edit-refused: the refused edit is a divergence",
        DescribeSnapshot(refused));
  Check(refused.last_edit_reason == edit::EditRejectReason::kPermissionDenied,
        "engine-ran-edit-refused: P3.7 named the permission refusal",
        edit::DescribeEditRejectReason(refused.last_edit_reason));
  Check(refused.permission_rechecks == 1,
        "engine-ran-edit-refused: exactly one callback-time recheck ran",
        std::to_string(refused.permission_rechecks));
  const auto first_store = setup.document->diagnostics().store;
  Check(first_store.write_lock_grants == 1 && first_store.text_mutations == 0 &&
            first_store.set_text_calls == 0,
        "engine-ran-edit-refused: the callback ran but wrote nothing",
        "grants=" + std::to_string(first_store.write_lock_grants) +
            " mutations=" + std::to_string(first_store.text_mutations));
  CheckDocText(setup.document->text_utf16(), L"",
               "engine-ran-edit-refused: the preedit never reached the document");

  // Restore Allowed: the blocked binding must not silently resume with the engine's old
  // "h" state. Only an explicit invalidation plus a rebuilt binding may continue.
  setup.document->SetScopeAnswer(TestScopeAnswer::kNone, {});
  executor_tests::TextKey e = Letter(L'e');
  WorkId second;
  const SubmitStatus blocked = setup.coordinator->SubmitKey(e.event, e.text, &second);
  Check(blocked == SubmitStatus::kBlockedNeedsResync,
        "engine-ran-edit-refused: restoring Allowed does not unblock the old binding",
        DescribeSubmitStatus(blocked));
  Check(setup.harness.backend->dispatch_calls() == 1,
        "engine-ran-edit-refused: the old key was not replayed", "");
  CheckDocText(setup.document->text_utf16(), L"",
               "engine-ran-edit-refused: the document stayed empty");

  std::string error;
  TestDocument* document_b = setup.host->CreateDocument(TestDocumentOptions{}, &error);
  Check(document_b != nullptr, "engine-ran-edit-refused: second document created", error);
  if (document_b != nullptr) {
    setup.coordinator->InvalidateBinding();
    const BindStatus bound = setup.coordinator->Bind(
        document_b->context(), document_b->context_id(), 0, setup.harness.client_b);
    Check(bound == BindStatus::kOk,
          "engine-ran-edit-refused: the rebuilt binding is accepted",
          DescribeBindStatus(bound));
    const DriveResult retired = DriveToCompletion(
        *setup.coordinator, *setup.host, *setup.harness.executor, 20000);
    Check(retired.completed && retired.final_state.retirement.retiring_bindings == 0,
          "engine-ran-edit-refused: the old client's close was consumed",
          DescribeSnapshot(retired.final_state));
    TestDocument* old_document = setup.document;
    setup.document = document_b;
    const DriveResult fresh = TypeLetter(setup, L'a', "engine-ran-edit-refused: fresh");
    CheckOutcome(fresh.final_state, WorkOutcome::kApplied,
                 "engine-ran-edit-refused: the fresh binding works");
    CheckDocText(document_b->text_utf16(), L"a",
                 "engine-ran-edit-refused: only the new key reached the new document");
    CheckDocText(old_document->text_utf16(), L"",
                 "engine-ran-edit-refused: the old document never shows the old preedit");
    Check(setup.harness.backend->Raw(setup.harness.client_b.value) == "a",
          "engine-ran-edit-refused: the new engine client holds only the new key", "");
    std::printf("[coordinator-win] evidence engine-ran-edit-refused: "
                "old_engine=\"h\" old_doc_mutations=%llu fresh_engine=\"a\" "
                "rechecks=%llu\n",
                static_cast<unsigned long long>(first_store.text_mutations),
                static_cast<unsigned long long>(refused.permission_rechecks));
  }
  return Finish();
}

// --- 12. binding generation change invalidates an old permission result -----------------

class WrongGenerationSource final : public context::PermissionSource {
 public:
  bool Begin(ITfContext* context, std::uint64_t context_id,
             std::uint64_t generation) override {
    if (context == nullptr || context_id == 0 || generation == 0) {
      return false;
    }
    context_id_ = context_id;
    generation_ = generation;
    begun_ = true;
    return true;
  }
  context::PermissionQueryStatus Status() const override {
    return begun_ ? context::PermissionQueryStatus::kReady
                  : context::PermissionQueryStatus::kIdle;
  }
  context::PermissionQueryResult Result() const override {
    context::PermissionQueryResult result;
    result.context_id = context_id_;
    // A verdict stamped for a different generation than the one the coordinator bound.
    result.generation = generation_ + 1;
    result.complete = begun_;
    result.decision.state = context::PermissionState::kAllowed;
    return result;
  }
  HANDLE CompletionEvent() const override { return nullptr; }
  void Abandon() override { begun_ = false; }
  context::PermissionQueryDiagnostics Diagnostics() const override { return {}; }

 private:
  std::uint64_t context_id_ = 0;
  std::uint64_t generation_ = 0;
  bool begun_ = false;
};

int RunGenerationChange() {
  TestDocumentOptions options;
  options.async_locks = true;
  options.refuse_sync_locks = true;
  GateCase setup;
  if (!SetupGateCase(&setup, options, "generation")) {
    return Finish();
  }
  WorkId old_id;
  SubmitH(setup, &old_id, "generation");
  Check(setup.coordinator->Snapshot().phase == WorkPhase::kPermissionPending,
        "generation: the old binding's query is pending",
        DescribeSnapshot(setup.coordinator->Snapshot()));
  Check(setup.harness.backend->dispatch_calls() == 0,
        "generation: the old query dispatched nothing", "");

  setup.coordinator->InvalidateBinding();
  const CoordinatorSnapshot stale = setup.coordinator->Snapshot();
  Check(stale.phase == WorkPhase::kIdle &&
            stale.outcome == WorkOutcome::kAbandonedStale,
        "generation: invalidation terminalizes the old work immediately",
        DescribeSnapshot(stale));
  Check(setup.permission->Status() == context::PermissionQueryStatus::kIdle,
        "generation: the old query is abandoned, not left pending",
        context::DescribePermissionQueryStatus(setup.permission->Status()));
  Check(setup.coordinator->PermissionCompletionEvent() == nullptr,
        "generation: no old completion fact survives the binding", "");

  // The old read session was queued when the binding was invalidated. Dispatching its
  // deferred grant now proves the abandoned callback publishes nothing; it also leaves no
  // stale lock between the old and the new context.
  setup.host->DispatchQueuedMessages();
  Check(setup.permission->Status() == context::PermissionQueryStatus::kIdle,
        "generation: the late old callback published nothing",
        context::DescribePermissionQueryStatus(setup.permission->Status()));
  Check(setup.harness.backend->dispatch_calls() == 0,
        "generation: the late old callback dispatched nothing", "");
  const DriveResult retired = DriveToCompletion(
      *setup.coordinator, *setup.host, *setup.harness.executor, 20000);
  Check(retired.completed && retired.final_state.retirement.retiring_bindings == 0,
        "generation: the retired client's close was consumed",
        DescribeSnapshot(retired.final_state));

  TestDocumentOptions second_options;
  second_options.scope_answer = TestScopeAnswer::kScopes;
  second_options.input_scopes = {1L};
  second_options.async_locks = true;
  second_options.refuse_sync_locks = true;
  std::string error;
  TestDocument* document_b = setup.host->CreateDocument(second_options, &error);
  Check(document_b != nullptr, "generation: second document created", error);
  if (document_b == nullptr) {
    return Finish();
  }
  TestDocument* old_document = setup.document;
  setup.document = document_b;
  const BindStatus bound = setup.coordinator->Bind(
      document_b->context(), document_b->context_id(), 0, setup.harness.client_b);
  Check(bound == BindStatus::kOk, "generation: the new binding is accepted",
        DescribeBindStatus(bound));

  // The new binding's own read is asynchronous too, so its verdict cannot be inherited
  // from the old binding: the work must wait for the new context's real read.
  WorkId new_id;
  const executor_tests::TextKey a = Letter(L'a');
  const SubmitStatus new_submit = setup.coordinator->SubmitKey(a.event, a.text, &new_id);
  Check(new_submit == SubmitStatus::kAccepted,
        "generation: new binding: submit accepted", DescribeSubmitStatus(new_submit));
  Check(setup.coordinator->Snapshot().phase == WorkPhase::kPermissionPending,
        "generation: the new binding has its own pending verdict",
        DescribeSnapshot(setup.coordinator->Snapshot()));
  Check(setup.harness.backend->dispatch_calls() == 0,
        "generation: no engine call before the new verdict", "");
  const DriveResult driven = DriveToCompletion(*setup.coordinator, *setup.host,
                                               *setup.harness.executor, 20000);
  CheckOutcome(driven.final_state, WorkOutcome::kApplied,
               "generation: the new binding's own verdict allowed its work");
  CheckDocText(document_b->text_utf16(), L"a",
               "generation: only the new key reached the new document");
  CheckDocText(old_document->text_utf16(), L"",
               "generation: the old document stayed empty");
  Check(setup.harness.backend->dispatch_calls() == 1,
        "generation: only the new binding dispatched to the engine", "");
  const auto diagnostics = setup.permission->Diagnostics();
  Check(diagnostics.queries_abandoned == 1,
        "generation: the old query is counted as abandoned",
        std::to_string(diagnostics.queries_abandoned));
  Check(diagnostics.queries_completed >= 1,
        "generation: the new binding's query completed", "");

  // Seam: a source that returns a complete Allowed verdict stamped for generation+1 must
  // never grant. This proves the coordinator's use-time identity check itself; it is not
  // evidence about TSF metadata.
  {
    std::string seam_error;
    auto seam_host = TestHost::Create(&seam_error);
    Check(seam_host != nullptr, "generation-seam: host created", seam_error);
    if (seam_host != nullptr) {
      TestDocument* seam_document =
          seam_host->CreateDocument(TestDocumentOptions{}, &seam_error);
      Check(seam_document != nullptr, "generation-seam: document created", seam_error);
      ControlledHarness seam_harness;
      if (seam_document != nullptr && CreateControlledHarness(&seam_harness)) {
        WrongGenerationSource wrong;
        TestTargetRevalidator seam_target;
        ContextCoordinator seam_coordinator(*seam_harness.executor,
                                            seam_host->client_id(), wrong);
        seam_coordinator.AttachTargetRevalidator(&seam_target);
        Check(seam_coordinator.Bind(seam_document->context(),
                                    seam_document->context_id(), 0,
                                    seam_harness.client_a) == BindStatus::kOk,
              "generation-seam: bound", "");
        const executor_tests::TextKey h = Letter(L'h');
        WorkId seam_id;
        const SubmitStatus submitted =
            seam_coordinator.SubmitKey(h.event, h.text, &seam_id);
        Check(submitted == SubmitStatus::kPermissionUnknown,
              "generation-seam: a verdict from another generation is not usable",
              DescribeSubmitStatus(submitted));
        Check(seam_harness.backend->dispatch_calls() == 0,
              "generation-seam: no engine call", "");
        CheckDocText(seam_document->text_utf16(), L"",
                     "generation-seam: no document write");
        seam_coordinator.Close();
      }
      seam_host->Shutdown();
    }
  }
  std::printf("[coordinator-win] evidence generation: old_abandoned=1 "
              "new_engine_calls=%llu old_doc_mutations=0\n",
              static_cast<unsigned long long>(
                  setup.harness.backend->dispatch_calls()));
  return Finish();
}

// --- 13. close while the permission query is pending ------------------------------------

int RunClosePendingPermission() {
  TestDocumentOptions options;
  options.async_locks = true;
  options.refuse_sync_locks = true;
  options.scope_answer = TestScopeAnswer::kScopes;
  options.input_scopes = {1L};
  GateCase setup;
  if (!SetupGateCase(&setup, options, "close-permission")) {
    return Finish();
  }
  WorkId id;
  SubmitH(setup, &id, "close-permission");
  Check(setup.coordinator->Snapshot().phase == WorkPhase::kPermissionPending,
        "close-permission: pending", "");
  Check(setup.coordinator->PermissionCompletionEvent() != nullptr,
        "close-permission: completion event exposed", "");

  setup.coordinator->Close();
  const CoordinatorSnapshot closed = setup.coordinator->Snapshot();
  Check(closed.closed && closed.phase == WorkPhase::kIdle &&
            closed.outcome == WorkOutcome::kAbandonedStale,
        "close-permission: Close terminalizes the parked work without waiting",
        DescribeSnapshot(closed));
  Check(setup.permission->Status() == context::PermissionQueryStatus::kIdle,
        "close-permission: the query is idle after Close",
        context::DescribePermissionQueryStatus(setup.permission->Status()));
  Check(setup.coordinator->PermissionCompletionEvent() == nullptr,
        "close-permission: no completion handle survives", "");
  Check(setup.harness.backend->dispatch_calls() == 0,
        "close-permission: no engine call", "");
  Check(setup.document->diagnostics().store.text_mutations == 0,
        "close-permission: no document write", "");

  // The deferred grant is still queued. Draining it must not resurrect the query or the
  // work item.
  setup.host->DispatchQueuedMessages();
  setup.coordinator->Pump();
  Check(setup.permission->Status() == context::PermissionQueryStatus::kIdle,
        "close-permission: a late callback publishes nothing",
        context::DescribePermissionQueryStatus(setup.permission->Status()));
  Check(setup.coordinator->Snapshot().closed &&
            setup.coordinator->Snapshot().phase == WorkPhase::kIdle,
        "close-permission: the coordinator stays closed", "");
  Check(setup.harness.backend->dispatch_calls() == 0,
        "close-permission: still no engine call", "");
  CheckDocText(setup.document->text_utf16(), L"",
               "close-permission: the document stayed empty");
  const auto diagnostics = setup.permission->Diagnostics();
  Check(diagnostics.queries_abandoned == 1 && diagnostics.queries_completed == 0,
        "close-permission: counted as abandoned, never completed", "");
  std::printf("[coordinator-win] evidence close-permission: abandoned=%llu "
              "completed=%llu engine_calls=0 mutations=0\n",
              static_cast<unsigned long long>(diagnostics.queries_abandoned),
              static_cast<unsigned long long>(diagnostics.queries_completed));
  return Finish();
}

// --- 14. resource balance ----------------------------------------------------------------

int RunResourceBalance() {
  const int stores_before = TestTextStore::LiveInstanceCount();
  const int scopes_before = TestInputScope::LiveInstanceCount();
  {
    TestDocumentOptions options;
    options.scope_answer = TestScopeAnswer::kScopes;
    options.input_scopes = {1L, 31L};
    GateCase setup;
    if (!SetupGateCase(&setup, options, "balance")) {
      return Finish();
    }
    Check(TestTextStore::LiveInstanceCount() == stores_before + 1,
          "balance: one store alive", "");
    Check(TestInputScope::LiveInstanceCount() == scopes_before + 1,
          "balance: one scope object alive", "");

    const long references_before =
        setup.document->store()->diagnostics().reference_count;
    for (int index = 0; index < 3; ++index) {
      WorkId id;
      const executor_tests::TextKey h = Letter(L'h');
      const SubmitStatus submitted =
          setup.coordinator->SubmitKey(h.event, h.text, &id);
      Check(submitted == SubmitStatus::kPermissionDenied,
            "balance: the sensitive scope denies", DescribeSubmitStatus(submitted));
    }
    const long references_after =
        setup.document->store()->diagnostics().reference_count;
    Check(references_after == references_before,
          "balance: completed queries released their context reference",
          std::to_string(references_before) + " -> " + std::to_string(references_after));
    const auto diagnostics = setup.permission->Diagnostics();
    Check(diagnostics.queries_begun == 3 && diagnostics.queries_completed == 3,
          "balance: three queries begun and completed",
          std::to_string(diagnostics.queries_begun) + "/" +
              std::to_string(diagnostics.queries_completed));

    setup.coordinator->Close();
    setup.coordinator.reset();
    setup.permission->Abandon();
    setup.permission.reset();
    setup.host->Shutdown();
    const auto& report = setup.host->cleanup_report();
    Check(report.store_instances_alive == 0,
          "balance: no store survives host shutdown",
          std::to_string(report.store_instances_alive));
    Check(report.advised_store_sinks_alive == 0,
          "balance: no advised sink survives", "");
    Check(report.all_documents_disconnected && report.all_contexts_released,
          "balance: the document/context teardown is clean", "");
    Check(report.store_reference_count_after_shutdown == 0,
          "balance: the store reference count is zero",
          std::to_string(report.store_reference_count_after_shutdown));
    Check(TestInputScope::LiveInstanceCount() == scopes_before,
          "balance: no scope object outlives its document", "");
  }
  Check(TestTextStore::LiveInstanceCount() == stores_before,
        "balance: no store outlives the host", "");
  Check(TestHost::LiveInstanceCount() == 0, "balance: no host outlives the case", "");
  std::printf("[coordinator-win] evidence balance: stores=%d scopes=%d\n",
              TestTextStore::LiveInstanceCount(), TestInputScope::LiveInstanceCount());
  return Finish();
}

}  // namespace

int RunInputPermissionGateCase(const std::string& case_name) {
  if (case_name == "permission-real-allow") {
    return RunRealAllowAndRepeat();
  }
  if (case_name == "permission-real-deny") {
    return RunDenyShapes();
  }
  if (case_name == "permission-real-unknown") {
    return RunUnknownAbsentAndGap();
  }
  if (case_name == "permission-real-async") {
    return RunAsyncPermission();
  }
  if (case_name == "permission-real-flip-before-apply") {
    return RunFlipBeforeApply();
  }
  if (case_name == "permission-real-engine-ran-refused") {
    return RunEngineRanEditRefused();
  }
  if (case_name == "permission-real-generation") {
    return RunGenerationChange();
  }
  if (case_name == "permission-real-close-pending") {
    return RunClosePendingPermission();
  }
  if (case_name == "permission-real-balance") {
    return RunResourceBalance();
  }
  return -1;
}

}  // namespace test
}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva
