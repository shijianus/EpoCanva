// EpoCanva P3.8 -- stage 2: the SAME coordinator against the pinned real English engine
// (epocanva_test_english fixture, real rime.dll through the P3.5 executor's real backend)
// and the REAL P3.7 TSF host. Every letter below travels the full chain: real ToUnicodeEx
// key snapshot -> coordinator gate -> executor value command -> real process_key on the
// worker thread -> value result copied back -> pure conversion -> real ITfContext
// RequestEditSession -> real document lock -> real composition/range edit -> backing text.
//
// Fault injection uses ONLY existing seams: the P3.7 host's store knobs (read-only, lock
// refusal, mutation faults, disconnect, deferred locks) and the P3.5 controlled backend is
// NOT used here -- where an engine-side fault is needed the case says so and points at the
// stage-1 case that covers it with the controlled seam.
//
// EXIT CODES: 0 pass; 5 a check failed.
#include <windows.h>

#include <cstdio>
#include <memory>
#include <string>

#include "coordinator_test_support.h"
#include "real_engine_tests.h"

namespace epocanva {
namespace tsf {
namespace coordinator {
namespace test {

namespace {

namespace input = epocanva::windows::input;
namespace engine = epocanva::windows::engine;
namespace executor_tests = epocanva::tests::engine_executor;

input::KeyEventSnapshot SpecialEvent(std::uint32_t virtual_key) {
  return executor_tests::MakeKeyEvent(virtual_key, false, false, false, false);
}

// A key whose ToUnicodeEx translation must succeed (a fixture character this layout can
// type). Returns an empty diagnostic on success.
std::string RequireLetter(executor_tests::TextKey* key, wchar_t character,
                          const char* label) {
  *key = executor_tests::MakeTextKey(character, ::GetKeyboardLayout(0));
  const std::string diagnostic =
      key->ok ? std::string() : (std::string(label) + ": " + key->diagnostic);
  Check(key->ok, std::string(label) + ": real ToUnicodeEx produced the letter", diagnostic);
  return diagnostic;
}

struct RealCase {
  std::unique_ptr<TestHost> host;
  RealHarness harness;
  std::unique_ptr<ContextCoordinator> coordinator;
  TestDocument* document = nullptr;

  ~RealCase() {
    if (coordinator != nullptr) {
      coordinator->Close();
    }
    if (harness.executor != nullptr && harness.executor->state() == engine::ExecutorState::kRunning) {
      harness.executor->StopAndJoin();
    }
    if (host != nullptr) {
      host->Shutdown();
    }
  }
};

bool SetupRealCase(RealCase* real, const Options& options, const char* leaf,
                   const TestDocumentOptions& document_options, bool grant_allowed = true) {
  std::string error;
  real->host = TestHost::Create(&error);
  Check(real->host != nullptr, std::string(leaf) + ": real TSF host created", error);
  if (real->host == nullptr) {
    return false;
  }
  real->document = real->host->CreateDocument(document_options, &error);
  Check(real->document != nullptr, std::string(leaf) + ": real document created", error);
  if (real->document == nullptr) {
    return false;
  }
  if (!CreateRealHarness(options, leaf, &real->harness)) {
    return false;
  }
  real->coordinator = std::make_unique<ContextCoordinator>(*real->harness.executor,
                                                           real->host->client_id());
  const BindStatus bound = real->coordinator->Bind(
      real->document->context(), real->document->context_id(),
      document_options.initial_selection_start_utf16, real->harness.client_a);
  Check(bound == BindStatus::kOk, std::string(leaf) + ": bound", DescribeBindStatus(bound));
  if (grant_allowed) {
    real->coordinator->SetPermission(edit::InputPermission::kAllowed);
  }
  return bound == BindStatus::kOk;
}

// Types one letter and requires an Applied terminal state.
bool TypeLetter(RealCase& real, wchar_t character, const char* label) {
  executor_tests::TextKey key;
  if (!RequireLetter(&key, character, label).empty()) {
    return false;
  }
  const DriveResult driven = TypeKeyAndDrive(*real.coordinator, *real.host,
                                             *real.harness.executor, key.event, key.text,
                                             label);
  const bool ok = driven.completed && driven.final_state.outcome == WorkOutcome::kApplied;
  Check(ok, std::string(label) + ": applied through the real chain",
        DescribeSnapshot(driven.final_state));
  std::printf("[coordinator-win] evidence %s: outcome=%s mutations=%llu doc_len=%lld\n",
              label, DescribeWorkOutcome(driven.final_state.outcome),
              static_cast<unsigned long long>(
                  real.document->diagnostics().store.text_mutations),
              static_cast<long long>(real.document->text_utf16().size()));
  return ok;
}

// Types a whole ASCII string letter by letter.
bool TypeLetters(RealCase& real, const wchar_t* text, const char* label) {
  int index = 0;
  for (const wchar_t* cursor = text; *cursor != L'\0'; ++cursor, ++index) {
    std::string step = std::string(label) + "[" + std::to_string(index) + "]";
    if (!TypeLetter(real, *cursor, step.c_str())) {
      return false;
    }
  }
  return true;
}

// Submits one special key (Space/Return/BackSpace/Escape) and requires a terminal state.
DriveResult Special(RealCase& real, std::uint32_t virtual_key, const char* label,
                    WorkOutcome want = WorkOutcome::kApplied) {
  WorkId id;
  const SubmitStatus submitted =
      real.coordinator->SubmitKey(SpecialEvent(virtual_key), input::KeyText{}, &id);
  Check(submitted == SubmitStatus::kAccepted, std::string(label) + ": submit accepted",
        DescribeSubmitStatus(submitted));
  DriveResult driven;
  if (submitted != SubmitStatus::kAccepted) {
    driven.final_state = real.coordinator->Snapshot();
    return driven;
  }
  driven = DriveToCompletion(*real.coordinator, *real.host, *real.harness.executor, 30000);
  Check(driven.completed, std::string(label) + ": terminal state",
        DescribeSnapshot(driven.final_state));
  CheckOutcome(driven.final_state, want, std::string(label) + ": outcome");
  return driven;
}

// Advances to EditPending without dispatching the deferred grant message (async hosts).
bool ReachEditPending(RealCase& real, const char* label) {
  const ULONGLONG deadline = ::GetTickCount64() + 30000;
  CoordinatorSnapshot snapshot = real.coordinator->Snapshot();
  while (snapshot.phase != WorkPhase::kEditPending) {
    real.coordinator->Pump();
    snapshot = real.coordinator->Snapshot();
    if (snapshot.phase == WorkPhase::kIdle || ::GetTickCount64() >= deadline) {
      break;
    }
    real.harness.executor->WaitForResult(snapshot.engine_request,
                                         std::chrono::milliseconds(50));
  }
  Check(snapshot.phase == WorkPhase::kEditPending,
        std::string(label) + ": the edit waits behind the deferred lock",
        DescribeSnapshot(snapshot));
  return snapshot.phase == WorkPhase::kEditPending;
}

// 1. h -> he -> hel: the host composition updates step by step, replacing not appending.
int RunRealComposeProgress(const Options& options) {
  RealCase real;
  if (!SetupRealCase(&real, options, "real-compose", TestDocumentOptions{})) {
    return Finish();
  }
  const struct {
    wchar_t letter;
    const wchar_t* want;
  } steps[] = {{L'h', L"h"}, {L'e', L"he"}, {L'l', L"hel"}};
  for (const auto& step : steps) {
    if (!TypeLetter(real, step.letter, "compose-progress: letter")) {
      break;
    }
    CheckDocText(real.document->text_utf16(), step.want,
                 "compose-progress: the document holds the composition so far");
  }
  const auto diagnostics = real.document->diagnostics().store;
  Check(diagnostics.text_mutations == 3, "compose-progress: three real mutations",
        std::to_string(diagnostics.text_mutations));
  Check(diagnostics.set_text_calls == 3, "compose-progress: three real SetText calls",
        std::to_string(diagnostics.set_text_calls));
  Check(real.coordinator->Snapshot().composition_active,
        "compose-progress: the composition is still active", "");
  return Finish();
}

// 2. Return commits: the document ends with exactly one "hel" and no composition.
int RunRealReturnCommit(const Options& options) {
  RealCase real;
  if (!SetupRealCase(&real, options, "real-return", TestDocumentOptions{})) {
    return Finish();
  }
  if (TypeLetters(real, L"hel", "return-commit: type")) {
    Special(real, input::kVirtualKeyReturn, "return-commit: Return");
    CheckDocText(real.document->text_utf16(), L"hel",
                 "return-commit: the document holds exactly the committed word");
    Check(!real.coordinator->Snapshot().composition_active,
          "return-commit: no composition remains", "");
    const auto diagnostics = real.document->diagnostics().store;
    Check(diagnostics.composition_end_calls == 1,
          "return-commit: the composition ended once",
          std::to_string(diagnostics.composition_end_calls));
  }
  return Finish();
}

// 3. Space commits the composition plus exactly one U+0020.
int RunRealSpaceCommit(const Options& options) {
  RealCase real;
  if (!SetupRealCase(&real, options, "real-space", TestDocumentOptions{})) {
    return Finish();
  }
  if (TypeLetters(real, L"hel", "space-commit: type")) {
    Special(real, input::kVirtualKeySpace, "space-commit: Space");
    CheckDocText(real.document->text_utf16(), L"hel ",
                 "space-commit: the word plus exactly one space, nothing else");
    Check(!real.coordinator->Snapshot().composition_active,
          "space-commit: no composition remains", "");
  }
  return Finish();
}

// 4. Explicit candidate acceptance then Space: "hello " once, no duplicate word.
int RunRealCandidateCommit(const Options& options) {
  RealCase real;
  if (!SetupRealCase(&real, options, "real-candidate", TestDocumentOptions{})) {
    return Finish();
  }
  if (!TypeLetters(real, L"hel", "candidate: type")) {
    return Finish();
  }
  WorkId id;
  const SubmitStatus accepted = real.coordinator->SubmitCandidate(1, &id);
  Check(accepted == SubmitStatus::kAccepted, "candidate: acceptance accepted",
        DescribeSubmitStatus(accepted));
  if (accepted == SubmitStatus::kAccepted) {
    const DriveResult driven =
        DriveToCompletion(*real.coordinator, *real.host, *real.harness.executor, 30000);
    CheckOutcome(driven.final_state, WorkOutcome::kApplied,
                 "candidate: the selected completion replaced the preedit");
    CheckDocText(real.document->text_utf16(), L"hello",
                 "candidate: the document shows the selected completion");
    Special(real, input::kVirtualKeySpace, "candidate: Space after selection");
    CheckDocText(real.document->text_utf16(), L"hello ",
                 "candidate: hello plus one space exactly once -- no duplicated word");
  }
  return Finish();
}

// 5. BackSpace updates the composition; Escape cancels only its own composition.
int RunRealBackspaceEscape(const Options& options) {
  RealCase real;
  if (!SetupRealCase(&real, options, "real-backspace", TestDocumentOptions{})) {
    return Finish();
  }
  if (!TypeLetters(real, L"he", "backspace: type")) {
    return Finish();
  }
  Special(real, input::kVirtualKeyBack, "backspace: BackSpace");
  CheckDocText(real.document->text_utf16(), L"h",
               "backspace: the composition shrank to one letter");
  Check(real.coordinator->Snapshot().composition_active,
        "backspace: the composition is still active", "");

  Special(real, input::kVirtualKeyEscape, "escape: Escape");
  CheckDocText(real.document->text_utf16(), L"",
               "escape: cancelling removed only the composition text");
  Check(!real.coordinator->Snapshot().composition_active,
        "escape: no composition remains", "");
  const auto diagnostics = real.document->diagnostics().store;
  Check(diagnostics.composition_end_calls == 1,
        "escape: the composition ended exactly once",
        std::to_string(diagnostics.composition_end_calls));

  // BackSpace with nothing composing is not an engine candidate at all.
  WorkId id;
  Check(real.coordinator->SubmitKey(SpecialEvent(input::kVirtualKeyBack), input::KeyText{},
                                    &id) == SubmitStatus::kNotEngineCandidate,
        "backspace: with no composition it is not an engine candidate", "");
  return Finish();
}

// 6. "hello world" with a prefix and a suffix: surrounding text survives verbatim.
int RunRealSurroundingText(const Options& options) {
  RealCase real;
  TestDocumentOptions document_options;
  document_options.initial_text_utf16 = L"PRE POST";
  document_options.initial_selection_start_utf16 = 4;
  document_options.initial_selection_end_utf16 = 4;
  if (!SetupRealCase(&real, options, "real-surrounding", document_options)) {
    return Finish();
  }
  if (!TypeLetters(real, L"hello", "surrounding: hello")) {
    return Finish();
  }
  // The document started as "PRE POST" with the caret right after its one separator, so
  // the composition sits flush against the suffix until the typed space arrives.
  CheckDocText(real.document->text_utf16(), L"PRE helloPOST",
               "surrounding: the composition sits between prefix and suffix");
  Special(real, input::kVirtualKeySpace, "surrounding: Space after hello");
  CheckDocText(real.document->text_utf16(), L"PRE hello POST",
               "surrounding: hello plus the typed space, the suffix still present");
  if (!TypeLetters(real, L"world", "surrounding: world")) {
    return Finish();
  }
  CheckDocText(real.document->text_utf16(), L"PRE hello worldPOST",
               "surrounding: the second composition also sits between the markers");
  Special(real, input::kVirtualKeyReturn, "surrounding: Return");
  CheckDocText(real.document->text_utf16(), L"PRE hello worldPOST",
               "surrounding: prefix and suffix survived the whole input verbatim");
  return Finish();
}

// 7. Technical text is kept verbatim: the protected marks stay inside the composition.
int RunRealTechnicalText(const Options& options) {
  RealCase real;
  if (!SetupRealCase(&real, options, "real-technical", TestDocumentOptions{})) {
    return Finish();
  }
  const wchar_t technical[] = L"a_1@x.y";
  if (TypeLetters(real, technical, "technical: type")) {
    CheckDocText(real.document->text_utf16(), technical,
                 "technical: the composition shows the token verbatim");
    Special(real, input::kVirtualKeyReturn, "technical: Return");
    CheckDocText(real.document->text_utf16(), technical,
                 "technical: the committed text is exactly the typed token");
  }
  return Finish();
}

// 8. Unicode prefix/suffix are untouched by the English composition between them.
int RunRealUnicodeAffixes(const Options& options) {
  RealCase real;
  TestDocumentOptions document_options;
  // alpha beta gamma, then an astral emoji (surrogate pair) after the insertion point.
  document_options.initial_text_utf16 = L"\u03b1\u03b2\u03b3\xD83E\xDD00";
  document_options.initial_selection_start_utf16 = 3;
  document_options.initial_selection_end_utf16 = 3;
  if (!SetupRealCase(&real, options, "real-unicode", document_options)) {
    return Finish();
  }
  const std::wstring expected_with_composition = L"\u03b1\u03b2\u03b3hi\xD83E\xDD00";
  if (TypeLetters(real, L"hi", "unicode: type")) {
    CheckDocText(real.document->text_utf16(), expected_with_composition,
                 "unicode: the composition landed between the UTF-16-affixes");
    Special(real, input::kVirtualKeyReturn, "unicode: Return");
    CheckDocText(real.document->text_utf16(), expected_with_composition,
                 "unicode: prefix and astral suffix are intact after the commit");
    Check(real.coordinator->Snapshot().tracked_caret_utf16 == 5,
          "unicode: the tracked caret sits after the two inserted letters", "");
  }
  return Finish();
}

// 9. Repeated Evaluate submits nothing and touches nothing.
int RunRealEvaluatePure(const Options& options) {
  RealCase real;
  if (!SetupRealCase(&real, options, "real-evaluate", TestDocumentOptions{})) {
    return Finish();
  }
  executor_tests::TextKey h;
  if (!RequireLetter(&h, L'h', "evaluate").empty()) {
    return Finish();
  }
  for (int index = 0; index < 3; ++index) {
    const input::KeyDecision decision = real.coordinator->Evaluate(h.event, h.text);
    Check(decision.disposition == input::KeyDisposition::kEngineTextCandidate,
          "evaluate: the letter is an engine candidate", "");
  }
  const input::KeyDecision enter = real.coordinator->Evaluate(
      SpecialEvent(input::kVirtualKeyReturn), input::KeyText{});
  Check(enter.disposition == input::KeyDisposition::kPassThroughToHost &&
            enter.reason == input::KeyDecisionReason::kNoComposition,
        "evaluate: Return with nothing composing stays with the host", "");
  const engine::CapacityUsage capacity = real.harness.executor->capacity();
  Check(capacity.occupied == 0, "evaluate: no executor request was ever created",
        std::to_string(capacity.occupied));
  const auto diagnostics = real.document->diagnostics().store;
  Check(diagnostics.request_lock_calls == 0 && diagnostics.text_mutations == 0,
        "evaluate: no lock, no mutation", "");
  CheckDocText(real.document->text_utf16(), L"", "evaluate: the document is untouched");
  return Finish();
}

// 10. Busy: the second key is explicitly refused and never executes twice.
int RunRealBusyReject(const Options& options) {
  RealCase real;
  if (!SetupRealCase(&real, options, "real-busy", TestDocumentOptions{})) {
    return Finish();
  }
  executor_tests::TextKey h;
  executor_tests::TextKey e;
  if (!RequireLetter(&h, L'h', "busy h").empty() ||
      !RequireLetter(&e, L'e', "busy e").empty()) {
    return Finish();
  }
  WorkId first;
  WorkId second;
  Check(real.coordinator->SubmitKey(h.event, h.text, &first) == SubmitStatus::kAccepted,
        "busy: the first submit is accepted", "");
  const SubmitStatus busy = real.coordinator->SubmitKey(e.event, e.text, &second);
  Check(busy == SubmitStatus::kBusy, "busy: the second submit is Busy",
        DescribeSubmitStatus(busy));
  Check(!second.valid(), "busy: the refused submit carries no work id", "");
  const DriveResult driven =
      DriveToCompletion(*real.coordinator, *real.host, *real.harness.executor, 30000);
  Check(driven.completed && driven.final_state.outcome == WorkOutcome::kApplied,
        "busy: the one accepted work applied", DescribeSnapshot(driven.final_state));
  CheckDocText(real.document->text_utf16(), L"h", "busy: exactly one letter landed");
  Check(driven.final_state.works_accepted == 1,
        "busy: only one work item was ever accepted",
        std::to_string(driven.final_state.works_accepted));
  Check(driven.final_state.engine_results_taken == 1,
        "busy: only one engine result was consumed",
        std::to_string(driven.final_state.engine_results_taken));
  Check(real.coordinator->SubmitKey(e.event, e.text, &second) == SubmitStatus::kAccepted,
        "busy: the slot frees after completion", "");
  const DriveResult after =
      DriveToCompletion(*real.coordinator, *real.host, *real.harness.executor, 30000);
  Check(after.completed && after.final_state.outcome == WorkOutcome::kApplied,
        "busy: the second work applied", DescribeSnapshot(after.final_state));
  CheckDocText(real.document->text_utf16(), L"he", "busy: the document advanced once");
  return Finish();
}

// 11. Switching the binding while the engine result is unconsumed: the old result never
//     writes the new document, and the discard releases the executor slot.
int RunRealRebindPending(const Options& options) {
  RealCase real;
  if (!SetupRealCase(&real, options, "real-rebind", TestDocumentOptions{})) {
    return Finish();
  }
  std::string error;
  TestDocumentOptions second_options;
  TestDocument* document_b = real.host->CreateDocument(second_options, &error);
  Check(document_b != nullptr, "rebind: second document created", error);
  if (document_b == nullptr) {
    return Finish();
  }
  const engine::ClientId client_b = AddRealClient(&real.harness);
  Check(client_b.valid(), "rebind: second engine client created", "");

  executor_tests::TextKey h;
  if (!RequireLetter(&h, L'h', "rebind h").empty()) {
    return Finish();
  }
  WorkId id;
  Check(real.coordinator->SubmitKey(h.event, h.text, &id) == SubmitStatus::kAccepted,
        "rebind: the key was accepted", "");
  // No Pump: the result is, at best, ready-but-unconsumed; the coordinator still owes its
  // single consumption. Invalidating now must terminalize the work and discard that result
  // without applying it anywhere.
  real.coordinator->InvalidateBinding();
  const DriveResult discarded =
      DriveToCompletion(*real.coordinator, *real.host, *real.harness.executor, 30000);
  Check(discarded.completed && discarded.final_state.outcome == WorkOutcome::kAbandonedStale,
        "rebind: the old work terminalized as AbandonedStale",
        DescribeSnapshot(discarded.final_state));
  Check(discarded.final_state.engine_results_taken == 1,
        "rebind: the old engine result was consumed exactly once",
        std::to_string(discarded.final_state.engine_results_taken));
  CheckDocText(real.document->text_utf16(), L"",
               "rebind: the old document was never written");
  CheckDocText(document_b->text_utf16(), L"",
               "rebind: the new document was never written by the old result");
  Check(real.harness.executor->capacity().occupied == 0,
        "rebind: the executor slot was released", "");

  const BindStatus bound = real.coordinator->Bind(
      document_b->context(), document_b->context_id(), 0, client_b);
  Check(bound == BindStatus::kOk, "rebind: the new binding is accepted",
        DescribeBindStatus(bound));
  real.coordinator->SetPermission(edit::InputPermission::kAllowed);
  // From here the case's `document` is the bound document, so evidence lines speak
  // about the store that is actually receiving edits; the old store keeps its own name.
  TestDocument* old_document = real.document;
  real.document = document_b;
  if (TypeLetter(real, L'a', "rebind: type on the new binding")) {
    CheckDocText(document_b->text_utf16(), L"a",
                 "rebind: only the new key reached the new document");
    CheckDocText(old_document->text_utf16(), L"",
                 "rebind: the old document stayed untouched");
  }
  return Finish();
}

// 12. An async edit waiting behind a deferred lock when the generation changes: the old
//     callback writes to neither the old nor the new document.
int RunRealAsyncGeneration(const Options& options) {
  RealCase real;
  TestDocumentOptions document_options;
  document_options.async_locks = true;
  if (!SetupRealCase(&real, options, "real-async-generation", document_options)) {
    return Finish();
  }
  std::string error;
  TestDocumentOptions second_options;
  TestDocument* document_b = real.host->CreateDocument(second_options, &error);
  Check(document_b != nullptr, "async-generation: second document created", error);
  if (document_b == nullptr) {
    return Finish();
  }
  const engine::ClientId client_b = AddRealClient(&real.harness);
  Check(client_b.valid(), "async-generation: second engine client created", "");

  executor_tests::TextKey h;
  if (!RequireLetter(&h, L'h', "async h").empty()) {
    return Finish();
  }
  WorkId id;
  Check(real.coordinator->SubmitKey(h.event, h.text, &id) == SubmitStatus::kAccepted,
        "async-generation: submit accepted", "");
  if (!ReachEditPending(real, "async-generation")) {
    return Finish();
  }
  const HANDLE completion = real.coordinator->EditCompletionEvent();
  Check(completion != nullptr, "async-generation: completion event exposed", "");
  Check(real.document->diagnostics().store.text_mutations == 0,
        "async-generation: nothing written before the grant", "");

  real.coordinator->InvalidateBinding();
  CoordinatorSnapshot snapshot = real.coordinator->Snapshot();
  Check(snapshot.phase == WorkPhase::kIdle &&
            snapshot.outcome == WorkOutcome::kAbandonedStale,
        "async-generation: invalidation terminalized the pending edit immediately",
        DescribeSnapshot(snapshot));
  Check(real.coordinator->Bind(document_b->context(), document_b->context_id(), 0,
                               client_b) == BindStatus::kOk,
        "async-generation: the new binding is accepted at once", "");
  real.coordinator->SetPermission(edit::InputPermission::kAllowed);
  real.document = document_b;

  // The deferred grant now dispatches; the OLD callback runs against an invalidated,
  // closed session and must refuse before any TSF call.
  real.host->PumpUntil(completion, 5000);
  const auto first_store = real.document->diagnostics().store;
  Check(first_store.text_mutations == 0 && first_store.composition_start_calls == 0,
        "async-generation: the old callback changed nothing in the old document", "");
  CheckDocText(document_b->text_utf16(), L"",
               "async-generation: the old callback reached no new document");

  if (TypeLetter(real, L'a', "async-generation: type on the new binding")) {
    CheckDocText(document_b->text_utf16(), L"a",
                 "async-generation: only the new key reached the new document");
  }
  return Finish();
}

// 13. Allowed -> Denied/Unknown: no further edits, at both re-verification points.
int RunRealPermissionFlip(const Options& options) {
  {
    // (a) the flip happens while the engine result is queued/unconsumed.
    RealCase real;
    if (!SetupRealCase(&real, options, "real-flip-queued", TestDocumentOptions{})) {
      return Finish();
    }
    executor_tests::TextKey h;
    if (!RequireLetter(&h, L'h', "flip h").empty()) {
      return Finish();
    }
    WorkId id;
    Check(real.coordinator->SubmitKey(h.event, h.text, &id) == SubmitStatus::kAccepted,
          "flip-queued: accepted while allowed", "");
    real.coordinator->SetPermission(edit::InputPermission::kDenied);
    const DriveResult driven =
        DriveToCompletion(*real.coordinator, *real.host, *real.harness.executor, 30000);
    Check(driven.completed, "flip-queued: terminal", DescribeSnapshot(driven.final_state));
    CheckOutcome(driven.final_state, WorkOutcome::kRejected, "flip-queued: rejected");
    Check(driven.final_state.cause == RejectCause::kPermissionChangedWhileQueued,
          "flip-queued: the cause names the permission change",
          DescribeRejectCause(driven.final_state.cause));
    const auto store = real.document->diagnostics().store;
    Check(store.request_lock_calls == 0 && store.text_mutations == 0,
          "flip-queued: no edit was requested", "");
    CheckDocText(real.document->text_utf16(), L"", "flip-queued: the document is empty");
    executor_tests::TextKey e;
    WorkId second;
    if (RequireLetter(&e, L'e', "flip e").empty()) {
      Check(real.coordinator->SubmitKey(e.event, e.text, &second) ==
                SubmitStatus::kPermissionDenied,
            "flip-queued: later submits are refused at the gate", "");
    }
  }
  {
    // (b) the flip happens while the edit waits behind a deferred lock; the real callback
    // refuses it.
    RealCase real;
    TestDocumentOptions document_options;
    document_options.async_locks = true;
    if (!SetupRealCase(&real, options, "real-flip-pending", document_options)) {
      return Finish();
    }
    executor_tests::TextKey h;
    if (!RequireLetter(&h, L'h', "flip2 h").empty()) {
      return Finish();
    }
    WorkId id;
    Check(real.coordinator->SubmitKey(h.event, h.text, &id) == SubmitStatus::kAccepted,
          "flip-pending: accepted", "");
    if (!ReachEditPending(real, "flip-pending")) {
      return Finish();
    }
    real.coordinator->SetPermission(edit::InputPermission::kUnknown);
    real.host->PumpUntil(real.coordinator->EditCompletionEvent(), 5000);
    real.coordinator->Pump();
    const CoordinatorSnapshot snapshot = real.coordinator->Snapshot();
    Check(snapshot.phase == WorkPhase::kIdle && snapshot.outcome == WorkOutcome::kRejected,
          "flip-pending: the callback-time check rejected the edit",
          DescribeSnapshot(snapshot));
    Check(snapshot.last_edit_reason == edit::EditRejectReason::kPermissionUnknown,
          "flip-pending: P3.7 named the permission refusal",
          edit::DescribeEditRejectReason(snapshot.last_edit_reason));
    const auto store = real.document->diagnostics().store;
    Check(store.text_mutations == 0 && store.set_text_calls == 0,
          "flip-pending: no text call, no mutation", "");
    CheckDocText(real.document->text_utf16(), L"", "flip-pending: the document is empty");
  }
  return Finish();
}

// 14. Read-only, refused write locks and a disconnected context are never Applied.
int RunRealRefusalsNotApplied(const Options& options) {
  struct Shape {
    const char* label;
    const char* leaf;
    bool read_only;
    bool refuse_write_locks;
    bool disconnect;
  };
  const Shape shapes[] = {
      {"read-only", "real-readonly", true, false, false},
      {"write-lock-refused", "real-lockrefused", false, true, false},
      {"disconnected", "real-disconnected", false, false, true},
  };
  for (const Shape& shape : shapes) {
    RealCase real;
    TestDocumentOptions document_options;
    document_options.read_only = shape.read_only;
    document_options.refuse_write_locks = shape.refuse_write_locks;
    if (!SetupRealCase(&real, options, shape.leaf, document_options)) {
      continue;
    }
    if (shape.disconnect) {
      real.document->Disconnect();
    }
    executor_tests::TextKey h;
    if (!RequireLetter(&h, L'h', shape.label).empty()) {
      continue;
    }
    WorkId id;
    Check(real.coordinator->SubmitKey(h.event, h.text, &id) == SubmitStatus::kAccepted,
          std::string(shape.label) + ": accepted", "");
    // The refused-lock shape never runs its callback (msctf queued the async request but
    // the store never grants the lock): bounded wait, then the explicit invalidation gives
    // the work its terminal state -- the "cannot assume DoEditSession will run" path.
    const DriveResult driven = DriveToCompletion(
        *real.coordinator, *real.host, *real.harness.executor,
        shape.refuse_write_locks ? 1500 : 30000);
    if (shape.refuse_write_locks) {
      Check(!driven.completed && driven.final_state.outcome != WorkOutcome::kApplied,
            std::string(shape.label) + ": never applied", DescribeSnapshot(driven.final_state));
      real.coordinator->InvalidateBinding();
      const CoordinatorSnapshot after = real.coordinator->Snapshot();
      Check(after.phase == WorkPhase::kIdle &&
                after.outcome == WorkOutcome::kAbandonedStale,
            std::string(shape.label) + ": invalidation terminalized the stuck edit",
            DescribeSnapshot(after));
      const auto store = real.document->diagnostics().store;
      Check(store.write_lock_refusals >= 1,
            std::string(shape.label) + ": the store really refused the lock",
            std::to_string(store.write_lock_refusals));
    } else {
      Check(driven.completed, std::string(shape.label) + ": terminal",
            DescribeSnapshot(driven.final_state));
      CheckOutcome(driven.final_state, WorkOutcome::kRejected,
                   std::string(shape.label) + ": rejected, not Applied");
      Check(driven.final_state.last_edit_status == edit::EditStatus::kRejected,
            std::string(shape.label) + ": P3.7 reported a clean rejection",
            edit::DescribeEditStatus(driven.final_state.last_edit_status));
    }
    const auto store = real.document->diagnostics().store;
    Check(store.text_mutations == 0,
          std::string(shape.label) + ": the document was never mutated",
          std::to_string(store.text_mutations));
    CheckDocText(real.document->text_utf16(), L"",
                 std::string(shape.label) + ": the document is empty");
    std::printf("[coordinator-win] evidence real-refusal %s: reason=%s mutations=%llu\n",
                shape.label,
                edit::DescribeEditRejectReason(driven.final_state.last_edit_reason),
                static_cast<unsigned long long>(store.text_mutations));
  }
  return Finish();
}

// 15. An unconfirmed write (P3.7 FailedPossiblyMutated, injected by the HOST fault knob
//     while the engine is real) locks the coordinator down; re-binding the same context
//     cannot bypass it and later input is refused, never auto-replayed.
int RunRealUncertainLockdown(const Options& options) {
  RealCase real;
  if (!SetupRealCase(&real, options, "real-uncertain", TestDocumentOptions{})) {
    return Finish();
  }
  real.document->store()->SetMutationFault(
      epocanva::tsf::edit::test::MutationFault::kEFailAfterMutation);
  executor_tests::TextKey h;
  if (!RequireLetter(&h, L'h', "uncertain h").empty()) {
    return Finish();
  }
  WorkId id;
  Check(real.coordinator->SubmitKey(h.event, h.text, &id) == SubmitStatus::kAccepted,
        "uncertain: accepted", "");
  const DriveResult driven =
      DriveToCompletion(*real.coordinator, *real.host, *real.harness.executor, 30000);
  Check(driven.completed, "uncertain: terminal", DescribeSnapshot(driven.final_state));
  CheckOutcome(driven.final_state, WorkOutcome::kUncertain,
               "uncertain: the unconfirmed write latched Uncertain");
  Check(driven.final_state.gate == CoordinatorGate::kBlockedUncertain,
        "uncertain: the gate is blocked",
        DescribeCoordinatorGate(driven.final_state.gate));
  Check(driven.final_state.last_edit_status == edit::EditStatus::kFailedPossiblyMutated,
        "uncertain: P3.7 said FailedPossiblyMutated",
        edit::DescribeEditStatus(driven.final_state.last_edit_status));
  const auto store = real.document->diagnostics().store;
  Check(store.set_text_failures == 1, "uncertain: exactly one failed write call",
        std::to_string(store.set_text_failures));
  // The document's exact content is deliberately NOT asserted: the host applied-then-
  // failed, and the coordinator's contract is that it does not know.

  executor_tests::TextKey e;
  WorkId second;
  if (RequireLetter(&e, L'e', "uncertain e").empty()) {
    Check(real.coordinator->SubmitKey(e.event, e.text, &second) ==
              SubmitStatus::kBlockedUncertain,
          "uncertain: the next submit is refused", "");
    Check(real.coordinator->SubmitCandidate(0, &second) ==
              SubmitStatus::kBlockedUncertain,
          "uncertain: candidate submits are refused", "");
  }

  // Explicit invalidation is allowed, but re-binding the SAME context must not bypass.
  real.coordinator->InvalidateBinding();
  const BindStatus same = real.coordinator->Bind(real.document->context(),
                                                 real.document->context_id(), 0,
                                                 real.harness.client_a);
  Check(same == BindStatus::kBlockedUncertain,
        "uncertain: re-binding the same context is refused", DescribeBindStatus(same));
  const std::uint64_t works_before = real.coordinator->Snapshot().works_accepted;

  // A different context is a different document and works normally.
  std::string error;
  TestDocumentOptions second_options;
  TestDocument* document_b = real.host->CreateDocument(second_options, &error);
  Check(document_b != nullptr, "uncertain: second document created", error);
  if (document_b != nullptr) {
    const engine::ClientId client_b = AddRealClient(&real.harness);
    const BindStatus other = real.coordinator->Bind(document_b->context(),
                                                    document_b->context_id(), 0, client_b);
    Check(other == BindStatus::kOk, "uncertain: a different context binds",
          DescribeBindStatus(other));
    real.coordinator->SetPermission(edit::InputPermission::kAllowed);
    real.document = document_b;
    if (TypeLetter(real, L'a', "uncertain: type on the fresh context")) {
      CheckDocText(document_b->text_utf16(), L"a",
                   "uncertain: the fresh context received exactly the new key");
    }
    Check(real.coordinator->Snapshot().works_accepted == works_before + 1,
          "uncertain: exactly one new work was accepted on the fresh context", "");
  }
  return Finish();
}

// 16. Pumping after a terminal state writes and consumes nothing.
int RunRealPumpIdempotent(const Options& options) {
  RealCase real;
  if (!SetupRealCase(&real, options, "real-idempotent", TestDocumentOptions{})) {
    return Finish();
  }
  if (!TypeLetters(real, L"hel", "idempotent: type")) {
    return Finish();
  }
  const auto before = real.document->diagnostics().store;
  const CoordinatorSnapshot before_snapshot = real.coordinator->Snapshot();
  for (int index = 0; index < 5; ++index) {
    real.coordinator->Pump();
  }
  const auto after = real.document->diagnostics().store;
  const CoordinatorSnapshot after_snapshot = real.coordinator->Snapshot();
  Check(after.text_mutations == before.text_mutations &&
            after.request_lock_calls == before.request_lock_calls,
        "idempotent: extra pumps wrote and locked nothing",
        std::to_string(after.text_mutations));
  Check(after_snapshot.engine_results_taken == before_snapshot.engine_results_taken &&
            after_snapshot.edits_applied == before_snapshot.edits_applied,
        "idempotent: extra pumps consumed nothing", "");
  CheckDocText(real.document->text_utf16(), L"hel",
               "idempotent: the document is stable");
  return Finish();
}

// 17. Close with pending engine work and with a pending async edit: terminal states,
//     bounded cleanup, nothing written.
int RunRealClosePending(const Options& options) {
  {
    // (a) engine request unconsumed at Close.
    RealCase real;
    if (!SetupRealCase(&real, options, "real-close-engine", TestDocumentOptions{})) {
      return Finish();
    }
    executor_tests::TextKey h;
    if (!RequireLetter(&h, L'h', "close h").empty()) {
      return Finish();
    }
    WorkId id;
    Check(real.coordinator->SubmitKey(h.event, h.text, &id) == SubmitStatus::kAccepted,
          "close-engine: accepted", "");
    const engine::RequestId request = real.coordinator->Snapshot().engine_request;
    real.coordinator->Close();
    const CoordinatorSnapshot snapshot = real.coordinator->Snapshot();
    Check(snapshot.closed && snapshot.phase == WorkPhase::kIdle &&
              snapshot.outcome == WorkOutcome::kAbandonedStale,
          "close-engine: close terminalized the work", DescribeSnapshot(snapshot));
    real.coordinator->Pump();
    Check(real.coordinator->Snapshot().pumps == snapshot.pumps + 1 &&
              real.coordinator->Snapshot().engine_results_taken ==
                  snapshot.engine_results_taken,
          "close-engine: pumping a closed coordinator consumes nothing", "");
    // The engine result is either already discarded by Close (slot free) or still owed to
    // the executor; both are honest, and the slot accounting says which. If it is owed, it
    // is still executing: a bounded wait for its readiness, then the one take.
    const engine::CapacityUsage capacity = real.harness.executor->capacity();
    if (capacity.occupied == 1) {
      engine::CommandResult leftover;
      engine::WaitStatus wait = engine::WaitStatus::kTimedOut;
      for (int attempt = 0; attempt < 200 && wait != engine::WaitStatus::kReady; ++attempt) {
        wait = real.harness.executor->WaitForResult(request, std::chrono::milliseconds(50));
      }
      Check(real.harness.executor->TakeResult(request, &leftover) ==
                engine::TakeStatus::kTaken,
            "close-engine: the leftover result is taken once by the host", "");
      Check(leftover.kind == engine::CommandKind::kDispatch,
            "close-engine: it is the dispatched key command", "");
    } else {
      Check(capacity.occupied == 0, "close-engine: the slot was already released",
            std::to_string(capacity.occupied));
    }
    CheckDocText(real.document->text_utf16(), L"",
                 "close-engine: the document was never written");
  }
  {
    // (b) async edit pending at Close.
    RealCase real;
    TestDocumentOptions document_options;
    document_options.async_locks = true;
    if (!SetupRealCase(&real, options, "real-close-edit", document_options)) {
      return Finish();
    }
    executor_tests::TextKey h;
    if (!RequireLetter(&h, L'h', "close2 h").empty()) {
      return Finish();
    }
    WorkId id;
    Check(real.coordinator->SubmitKey(h.event, h.text, &id) == SubmitStatus::kAccepted,
          "close-edit: accepted", "");
    if (!ReachEditPending(real, "close-edit")) {
      return Finish();
    }
    const HANDLE completion = real.coordinator->EditCompletionEvent();
    real.coordinator->Close();
    const CoordinatorSnapshot snapshot = real.coordinator->Snapshot();
    Check(snapshot.closed && snapshot.phase == WorkPhase::kIdle &&
              snapshot.outcome == WorkOutcome::kAbandonedStale,
          "close-edit: close terminalized the pending edit without waiting for the callback",
          DescribeSnapshot(snapshot));
    // Bounded: the grant may dispatch later and the closed session's callback refuses it;
    // whether the event fires or not, this wait ends.
    real.host->PumpUntil(completion, 3000);
    real.coordinator->Pump();
    const auto store = real.document->diagnostics().store;
    Check(store.text_mutations == 0 && store.composition_start_calls == 0,
          "close-edit: no composition was started, no text written",
          std::to_string(store.text_mutations));
    CheckDocText(real.document->text_utf16(), L"", "close-edit: the document is empty");
  }
  return Finish();
}

// 18. The context is destroyed while an async edit is queued: the path terminates in a
//     bounded way and nothing is written.
int RunRealDestroyedContext(const Options& options) {
  RealCase real;
  TestDocumentOptions document_options;
  document_options.async_locks = true;
  if (!SetupRealCase(&real, options, "real-destroyed", document_options)) {
    return Finish();
  }
  executor_tests::TextKey h;
  if (!RequireLetter(&h, L'h', "destroyed h").empty()) {
    return Finish();
  }
  WorkId id;
  Check(real.coordinator->SubmitKey(h.event, h.text, &id) == SubmitStatus::kAccepted,
        "destroyed: accepted", "");
  if (!ReachEditPending(real, "destroyed")) {
    return Finish();
  }
  const HANDLE completion = real.coordinator->EditCompletionEvent();
  Check(completion != nullptr, "destroyed: completion event exposed", "");

  real.document->Disconnect();
  // Bounded wait on the real event; no assumption that the callback ever runs.
  real.host->PumpUntil(completion, 5000);
  real.coordinator->Pump();
  CoordinatorSnapshot snapshot = real.coordinator->Snapshot();
  if (snapshot.phase != WorkPhase::kIdle) {
    // The callback never ran: the documented recovery is the explicit close, and it must
    // not wait for DoEditSession.
    real.coordinator->Close();
    snapshot = real.coordinator->Snapshot();
  }
  Check(snapshot.phase == WorkPhase::kIdle,
        "destroyed: the coordinator is not waiting forever", DescribeSnapshot(snapshot));
  Check(snapshot.outcome != WorkOutcome::kApplied,
        "destroyed: the destroyed context was never Applied",
        DescribeWorkOutcome(snapshot.outcome));
  const auto store = real.document->diagnostics().store;
  Check(store.text_mutations == 0 && store.composition_start_calls == 0,
        "destroyed: nothing was written to the destroyed document",
        std::to_string(store.text_mutations));
  std::printf("[coordinator-win] evidence destroyed-context: outcome=%s phase=%s via=%s\n",
              DescribeWorkOutcome(snapshot.outcome), DescribeWorkPhase(snapshot.phase),
              snapshot.closed ? "close" : "callback");
  return Finish();
}

}  // namespace

int RunRealEngineCase(const std::string& case_name, const Options& options) {
  if (case_name == "real-compose-progress") {
    return RunRealComposeProgress(options);
  }
  if (case_name == "real-return-commit") {
    return RunRealReturnCommit(options);
  }
  if (case_name == "real-space-commit") {
    return RunRealSpaceCommit(options);
  }
  if (case_name == "real-candidate-commit") {
    return RunRealCandidateCommit(options);
  }
  if (case_name == "real-backspace-escape") {
    return RunRealBackspaceEscape(options);
  }
  if (case_name == "real-surrounding-text") {
    return RunRealSurroundingText(options);
  }
  if (case_name == "real-technical-text") {
    return RunRealTechnicalText(options);
  }
  if (case_name == "real-unicode-affixes") {
    return RunRealUnicodeAffixes(options);
  }
  if (case_name == "real-evaluate-pure") {
    return RunRealEvaluatePure(options);
  }
  if (case_name == "real-busy-reject") {
    return RunRealBusyReject(options);
  }
  if (case_name == "real-rebind-pending") {
    return RunRealRebindPending(options);
  }
  if (case_name == "real-async-generation") {
    return RunRealAsyncGeneration(options);
  }
  if (case_name == "real-permission-flip") {
    return RunRealPermissionFlip(options);
  }
  if (case_name == "real-refusals-not-applied") {
    return RunRealRefusalsNotApplied(options);
  }
  if (case_name == "real-uncertain-lockdown") {
    return RunRealUncertainLockdown(options);
  }
  if (case_name == "real-pump-idempotent") {
    return RunRealPumpIdempotent(options);
  }
  if (case_name == "real-close-pending") {
    return RunRealClosePending(options);
  }
  if (case_name == "real-destroyed-context") {
    return RunRealDestroyedContext(options);
  }
  return -1;
}

}  // namespace test
}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva
