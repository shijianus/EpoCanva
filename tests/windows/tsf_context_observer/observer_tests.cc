// EpoCanva P3.10 -- context-observer verification cases against the real TSF manager.
//
// All bindings, drops and rebinds in these cases come from real msctf.dll notifications
// (ITfThreadMgrEventSink / ITfTextEditSink) and real selection reads. Nothing here calls a
// sink method directly. The one deliberately non-notification path (Pump's defensive
// GetFocus revalidation) is reported as such and never counted as real focus coverage.
//
// EXIT CODES: 0 pass; 5 a check failed; 14 the falsifiability control; 77 the real-focus
// case could not be exercised by this environment (CTest reports it as SKIPPED).
#include <windows.h>
#include <msctf.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "observer_test_support.h"
#include "observer_tests.h"
#include "target_revalidation.h"

namespace epocanva {
namespace tsf {
namespace context {
namespace test {

namespace {

struct CaseEnv {
  std::unique_ptr<TestHost> host;
  std::unique_ptr<ContextRegistry> registry;
  std::unique_ptr<ContextObserver> observer;
  RecordingSink sink;
  TestDocument* doc_a = nullptr;
  TestDocument* doc_b = nullptr;
  TfClientId external = 0;
  TestDocumentOptions a_options;
  std::string error;

  ~CaseEnv() {
    if (observer != nullptr) {
      observer->Shutdown();
    }
    if (host != nullptr) {
      host->Shutdown();
    }
  }
};

bool CreateHostAndDocument(CaseEnv* env, const TestDocumentOptions& options,
                           const char* label) {
  env->host = TestHost::Create(&env->error);
  Check(env->host != nullptr, std::string(label) + ": real TSF host created",
        env->error);
  if (env->host == nullptr) {
    return false;
  }
  env->doc_a = env->host->CreateDocument(options, &env->error);
  Check(env->doc_a != nullptr, std::string(label) + ": document A created", env->error);
  if (env->doc_a == nullptr) {
    return false;
  }
  env->external = ExternalClientId(env->host->thread_manager_for_test(), &env->error);
  Check(env->external != 0 && env->external != env->host->client_id(),
        std::string(label) + ": external client id is real and distinct",
        "external=" + std::to_string(env->external) +
            " own=" + std::to_string(env->host->client_id()) + " " + env->error);
  return env->external != 0;
}

bool SubscribeAndBind(CaseEnv* env, TestDocument* document, const char* label,
                      DWORD timeout_ms = 20000) {
  FocusCallResult focus;
  if (!SetThreadFocus(*env->host, document, &focus, &env->error)) {
    Check(false, std::string(label) + ": SetFocus + GetFocus", env->error);
    return false;
  }
  env->registry = std::make_unique<ContextRegistry>(8);
  env->observer = std::make_unique<ContextObserver>(*env->registry,
                                                    env->host->client_id());
  if (!env->observer->Subscribe(env->host->thread_manager_for_test(), &env->sink,
                                &env->error)) {
    Check(false, std::string(label) + ": subscribed to the real sinks", env->error);
    return false;
  }
  const ObserverDriveResult driven =
      PumpUntilSettled(*env->observer, *env->host, timeout_ms);
  Check(driven.settled, std::string(label) + ": observer settled", env->error);
  Check(driven.final_state.has_published_target,
        std::string(label) + ": a target was published",
        DescribeObserverSnapshot(driven.final_state));
  return driven.final_state.has_published_target;
}

ContextIdRecord ObserveRecord(ContextRegistry& registry, TestDocument* document) {
  ContextIdRecord record;
  registry.Observe(document->context(), &record);
  return record;
}

// --- 1. canonical identity, generations, the latch, capacity ------------------------------

int RunRegistryIdentity() {
  CaseEnv env;
  if (!CreateHostAndDocument(&env, TestDocumentOptions{}, "registry")) {
    return Finish();
  }
  TestDocument* child = env.host->CreateChildContext(env.doc_a, TestDocumentOptions{},
                                                     &env.error);
  Check(child != nullptr, "registry: child context created", env.error);
  if (child == nullptr) {
    return Finish();
  }
  {
    ContextRegistry registry(4);
    ContextIdRecord first;
    ContextIdRecord second;
    ContextIdRecord child_record;
    Check(registry.Observe(env.doc_a->context(), &first) ==
              ContextRegistryStatus::kOk,
          "registry: first observation succeeds", "");
    Check(registry.Observe(env.doc_a->context(), &second) ==
              ContextRegistryStatus::kOk &&
              first.context_id != 0 && second.context_id == first.context_id,
          "registry: the same context keeps one id",
          std::to_string(first.context_id) + "/" + std::to_string(second.context_id));
    Check(registry.Observe(child->context(), &child_record) ==
              ContextRegistryStatus::kOk &&
              child_record.context_id != first.context_id,
          "registry: a different context gets a different id", "");
    ContextIdRecord bound;
    Check(registry.BeginBinding(env.doc_a->context(), &bound) ==
              ContextRegistryStatus::kOk &&
              bound.generation == 1,
          "registry: the first bind starts generation 1",
          std::to_string(bound.generation));
    Check(registry.Observe(env.doc_a->context(), &second) ==
              ContextRegistryStatus::kOk &&
              second.generation == 1,
          "registry: Observe reports the current generation", "");
    Check(registry.BeginBinding(env.doc_a->context(), &bound) ==
              ContextRegistryStatus::kOk &&
              bound.generation == 2,
          "registry: a rebind advances the generation",
          std::to_string(bound.generation));
    Check(!registry.IsUncertain(env.doc_a->context()),
          "registry: no latch before a mark", "");
    Check(registry.MarkUncertain(env.doc_a->context()) ==
              ContextRegistryStatus::kOk,
          "registry: the latch can be recorded", "");
    Check(registry.IsUncertain(env.doc_a->context()),
          "registry: the latch reads back by real context identity", "");
    Check(!registry.IsUncertain(child->context()),
          "registry: the latch is per context, not global", "");
    ContextIdRecord blocked;
    Check(registry.BeginBinding(env.doc_a->context(), &blocked) ==
              ContextRegistryStatus::kOk &&
              blocked.uncertain_blocked && blocked.generation == 2,
          "registry: a blocked context does not begin a new binding",
          "blocked=" + std::to_string(blocked.uncertain_blocked ? 1 : 0) +
              " gen=" + std::to_string(blocked.generation));
    ContextIdRecord looked_up;
    Check(registry.FindById(first.context_id, &looked_up) &&
              looked_up.uncertain_blocked,
          "registry: the latch is visible by project id", "");
    ContextRegistryDiagnostics diagnostics = registry.Diagnostics();
    Check(diagnostics.entries_balanced(),
          "registry: canonical references balanced while entries live",
          "acquired=" + std::to_string(diagnostics.canonical_references_acquired) +
              " released=" +
              std::to_string(diagnostics.canonical_references_released) +
              " live=" + std::to_string(diagnostics.live_entries));
  }
  {
    ContextRegistry capped(2);
    ContextIdRecord a;
    ContextIdRecord b;
    ContextIdRecord c;
    Check(capped.Observe(env.doc_a->context(), &a) == ContextRegistryStatus::kOk,
          "registry-capacity: first entry accepted", "");
    Check(capped.Observe(child->context(), &b) == ContextRegistryStatus::kOk,
          "registry-capacity: second entry accepted", "");
    Check(capped.Observe(env.doc_a->context(), &c) == ContextRegistryStatus::kOk &&
              c.context_id == a.context_id,
          "registry-capacity: an existing identity is still found at capacity", "");
    TestDocument* third = env.host->CreateDocument(TestDocumentOptions{}, &env.error);
    Check(third != nullptr, "registry-capacity: third context created", env.error);
    if (third != nullptr) {
      Check(capped.Observe(third->context(), &c) ==
                ContextRegistryStatus::kCapacityExceeded,
            "registry-capacity: a new identity is refused at capacity", "");
      ContextRegistryDiagnostics diagnostics = capped.Diagnostics();
      Check(diagnostics.live_entries == 2 && diagnostics.capacity_refusals == 1,
            "registry-capacity: nothing was silently evicted",
            "live=" + std::to_string(diagnostics.live_entries));
      Check(!capped.IsUncertain(third->context()),
            "registry-capacity: the refused context was never latched", "");
      Check(capped.MarkUncertain(third->context()) ==
                ContextRegistryStatus::kCapacityExceeded,
            "registry-capacity: a new latch is refused, not forgotten", "");
      ContextIdRecord still_there;
      Check(capped.FindById(a.context_id, &still_there),
            "registry-capacity: the older identity is still present", "");
    }
    capped.ReleaseAll();
    Check(capped.Observe(env.doc_a->context(), &a) ==
              ContextRegistryStatus::kClosed,
          "registry-capacity: ReleaseAll is terminal", "");
    ContextRegistryDiagnostics diagnostics = capped.Diagnostics();
    Check(diagnostics.entries_balanced(),
          "registry-capacity: all canonical references returned after release",
          "acquired=" + std::to_string(diagnostics.canonical_references_acquired) +
              " released=" +
              std::to_string(diagnostics.canonical_references_released));
  }
  return Finish();
}

// --- 2. the current top context's real binding -------------------------------------------

int RunInitialBinding() {
  CaseEnv env;
  if (!CreateHostAndDocument(&env, TestDocumentOptions{L"abcdef", 2, 2}, "initial")) {
    return Finish();
  }
  if (!SubscribeAndBind(&env, env.doc_a, "initial")) {
    return Finish();
  }
  const ContextObserverSnapshot snapshot = env.observer->Snapshot();
  ContextIdRecord record = ObserveRecord(*env.registry, env.doc_a);
  Check(env.sink.Count(SinkEvent::Kind::kBound) == 1,
        "initial: exactly one bind event", std::to_string(env.sink.Count(
                                                 SinkEvent::Kind::kBound)));
  Check(env.sink.Count(SinkEvent::Kind::kLost) == 0,
        "initial: no target-lost event", "");
  Check(env.sink.has_target() && env.sink.last_target().context_id == record.context_id,
        "initial: the bound id is the real context's registry id", "");
  Check(env.sink.last_target().generation == 1,
        "initial: generation 1", std::to_string(env.sink.last_target().generation));
  Check(env.sink.last_target().caret_utf16 == 2,
        "initial: the caret is the real selection (2 utf16 units)",
        std::to_string(env.sink.last_target().caret_utf16));
  Check(env.sink.last_context_non_null(),
        "initial: the callback carried a real context pointer", "");
  Check(snapshot.text_sink_advised && snapshot.thread_sink_advised,
        "initial: both real sinks are advised", DescribeObserverSnapshot(snapshot));
  Check(snapshot.selection_reader.collapsed == 1 &&
            snapshot.selection_reader.read_sessions_sync >= 1,
        "initial: one real synchronous collapsed-selection read",
        "reads=" + std::to_string(snapshot.selection_reader.reads_completed) +
            " collapsed=" +
            std::to_string(snapshot.selection_reader.collapsed));
  Check(snapshot.registry.entries_balanced(),
        "initial: registry references balanced", "");
  return Finish();
}

// --- 3. Push/Pop changes the top context through real notifications -----------------------

int RunPushPopTop() {
  CaseEnv env;
  if (!CreateHostAndDocument(&env, TestDocumentOptions{L"abcdef", 2, 2}, "push-pop")) {
    return Finish();
  }
  if (!SubscribeAndBind(&env, env.doc_a, "push-pop")) {
    return Finish();
  }
  ContextIdRecord a_record = ObserveRecord(*env.registry, env.doc_a);
  const ObservedTarget first = env.sink.last_target();
  Check(first.context_id == a_record.context_id,
        "push-pop: initially bound to A", "");

  TestDocument* child = env.host->CreateChildContext(
      env.doc_a, TestDocumentOptions{L"second", 0, 0}, &env.error);
  Check(child != nullptr, "push-pop: child context pushed", env.error);
  if (child == nullptr) {
    return Finish();
  }
  Check(env.sink.Count(SinkEvent::Kind::kLost) == 1 &&
            env.sink.events().back().reason == ContextDropReason::kTopContextChanged,
        "push-pop: the real OnPushContext dropped the old target",
        "lost=" + std::to_string(env.sink.Count(SinkEvent::Kind::kLost)));
  Check(!env.observer->Snapshot().has_published_target,
        "push-pop: no target is published between the notification and Pump", "");

  ObserverDriveResult driven =
      PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(driven.settled && driven.final_state.has_published_target,
        "push-pop: Pump published the new top context",
        DescribeObserverSnapshot(driven.final_state));
  ContextIdRecord child_record = ObserveRecord(*env.registry, child);
  Check(env.sink.last_target().context_id == child_record.context_id &&
            env.sink.last_target().context_id != first.context_id,
        "push-pop: the new target is the child context",
        std::to_string(env.sink.last_target().context_id) + " vs " +
            std::to_string(child_record.context_id));
  Check(env.sink.last_target().generation == 1 &&
            env.sink.last_target().caret_utf16 == 0,
        "push-pop: the child binding has its own generation and caret",
        "gen=" + std::to_string(env.sink.last_target().generation) +
            " caret=" + std::to_string(env.sink.last_target().caret_utf16));

  Check(child->Disconnect(), "push-pop: child Popped", "");
  ITfContext* top_after_pop = nullptr;
  const HRESULT top_hr =
      env.doc_a->document_manager_for_test()->GetTop(&top_after_pop);
  std::printf(
      "[observer-win] evidence push-pop: GetTop hr=0x%08lX top=%p child=%p a=%p "
      "lost=%zu background_ignored=%llu\n",
      static_cast<unsigned long>(top_hr), static_cast<void*>(top_after_pop),
      static_cast<void*>(child->context()), static_cast<void*>(env.doc_a->context()),
      env.sink.Count(SinkEvent::Kind::kLost),
      static_cast<unsigned long long>(
          env.observer->Snapshot().background_events_ignored));
  if (top_after_pop != nullptr) top_after_pop->Release();
  Check(env.sink.Count(SinkEvent::Kind::kLost) == 2 &&
            env.sink.events().back().reason == ContextDropReason::kTopContextChanged,
        "push-pop: the real OnPopContext dropped the child target",
        "lost=" + std::to_string(env.sink.Count(SinkEvent::Kind::kLost)));
  driven = PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(driven.settled && driven.final_state.has_published_target,
        "push-pop: Pump published A again",
        DescribeObserverSnapshot(driven.final_state));
  Check(env.sink.last_target().context_id == first.context_id,
        "push-pop: the target is A again", "");
  Check(env.sink.last_target().generation == first.generation + 1,
        "push-pop: the re-bind advanced A's generation",
        std::to_string(env.sink.last_target().generation));
  Check(env.sink.last_target().caret_utf16 == 2,
        "push-pop: A's real caret is read again",
        std::to_string(env.sink.last_target().caret_utf16));
  Check(env.observer->Snapshot().push_notifications >= 1 &&
            env.observer->Snapshot().pop_notifications >= 1,
        "push-pop: both notifications were real TSF deliveries",
        "push=" + std::to_string(env.observer->Snapshot().push_notifications) +
            " pop=" + std::to_string(env.observer->Snapshot().pop_notifications));
  return Finish();
}

// --- 4. an external selection move invalidates the old position --------------------------

int RunExternalSelection() {
  CaseEnv env;
  if (!CreateHostAndDocument(&env, TestDocumentOptions{L"abcdef", 2, 2}, "external-sel")) {
    return Finish();
  }
  if (!SubscribeAndBind(&env, env.doc_a, "external-sel")) {
    return Finish();
  }
  const ObservedTarget first = env.sink.last_target();
  ExternalEditResult edit;
  Check(RunExternalEdit(*env.host, env.doc_a, env.external,
                        ExternalEditMode::kSetCollapsedSelection, 5, 5, L"", &edit,
                        &env.error),
        "external-sel: the real external edit session ran", env.error);
  Check(env.sink.Count(SinkEvent::Kind::kLost) == 1 &&
            env.sink.events().back().reason ==
                ContextDropReason::kExternalSelectionEdit,
        "external-sel: the external selection change dropped the target immediately",
        "lost=" + std::to_string(env.sink.Count(SinkEvent::Kind::kLost)));
  Check(!env.observer->Snapshot().has_published_target,
        "external-sel: the old generation is not published after the notification", "");
  Check(env.doc_a->diagnostics().store.text_mutations == 0,
        "external-sel: the observer wrote no text", "");
  CheckDocText(env.doc_a->text_utf16(), L"abcdef",
               "external-sel: the document text is unchanged");

  const ObserverDriveResult driven =
      PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(driven.settled && driven.final_state.has_published_target,
        "external-sel: a fresh read re-bound the context",
        DescribeObserverSnapshot(driven.final_state));
  Check(env.sink.last_target().context_id == first.context_id,
        "external-sel: the same real context is bound", "");
  Check(env.sink.last_target().generation == first.generation + 1,
        "external-sel: the new generation is strictly newer",
        std::to_string(env.sink.last_target().generation));
  Check(env.sink.last_target().caret_utf16 == 5,
        "external-sel: the new caret is the external selection, not the old one",
        std::to_string(env.sink.last_target().caret_utf16));
  Check(env.observer->Snapshot().external_edits == 1 &&
            env.observer->Snapshot().selection_changes >= 1,
        "external-sel: classified as an external selection change",
        DescribeObserverSnapshot(env.observer->Snapshot()));
  return Finish();
}

// --- 5. an external text edit invalidates the old engine binding -------------------------

int RunExternalText() {
  CaseEnv env;
  if (!CreateHostAndDocument(&env, TestDocumentOptions{L"abcdef", 2, 2}, "external-text")) {
    return Finish();
  }
  if (!SubscribeAndBind(&env, env.doc_a, "external-text")) {
    return Finish();
  }
  const ObservedTarget first = env.sink.last_target();
  ExternalEditResult edit;
  Check(RunExternalEdit(*env.host, env.doc_a, env.external, ExternalEditMode::kInsertText,
                        2, 2, L"x", &edit, &env.error),
        "external-text: the real external write ran", env.error);
  Check(env.sink.Count(SinkEvent::Kind::kLost) == 1 &&
            env.sink.events().back().reason == ContextDropReason::kExternalTextEdit,
        "external-text: the external text change dropped the target immediately",
        "lost=" + std::to_string(env.sink.Count(SinkEvent::Kind::kLost)));
  Check(env.doc_a->text_utf16().size() == 7,
        "external-text: the external write really changed the document",
        std::to_string(env.doc_a->text_utf16().size()) + " utf16 units");
  const ObserverDriveResult driven =
      PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(driven.settled && driven.final_state.has_published_target,
        "external-text: a fresh read re-bound the context",
        DescribeObserverSnapshot(driven.final_state));
  Check(env.sink.last_target().generation == first.generation + 1,
        "external-text: the binding generation advanced", "");
  Check(env.sink.last_target().caret_utf16 == 3,
        "external-text: the caret is the post-insert real position",
        std::to_string(env.sink.last_target().caret_utf16));
  Check(env.observer->Snapshot().text_changes >= 1,
        "external-text: the edit record reported a text change", "");
  return Finish();
}

// --- 6. a non-collapsed selection closes admission ---------------------------------------

int RunNonCollapsedSelection() {
  CaseEnv env;
  if (!CreateHostAndDocument(&env, TestDocumentOptions{L"abcdef", 2, 2}, "noncollapsed")) {
    return Finish();
  }
  if (!SubscribeAndBind(&env, env.doc_a, "noncollapsed")) {
    return Finish();
  }
  const std::size_t binds_before = env.sink.Count(SinkEvent::Kind::kBound);
  ExternalEditResult edit;
  Check(RunExternalEdit(*env.host, env.doc_a, env.external,
                        ExternalEditMode::kSetRangeSelection, 1, 3, L"", &edit,
                        &env.error),
        "noncollapsed: the real range selection ran", env.error);
  const ObserverDriveResult driven =
      PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(driven.settled, "noncollapsed: the observer settled", env.error);
  Check(!driven.final_state.has_published_target,
        "noncollapsed: no target was published for a range selection",
        DescribeObserverSnapshot(driven.final_state));
  Check(driven.final_state.last_refusal_reason ==
            ContextDropReason::kSelectionNonCollapsed,
        "noncollapsed: the refusal is explicit, not a clamp",
        std::string(DescribeContextDropReason(
            driven.final_state.last_refusal_reason)));
  Check(env.sink.Count(SinkEvent::Kind::kBound) == binds_before,
        "noncollapsed: no bind event was produced", "");
  Check(env.doc_a->text_utf16() == L"abcdef",
        "noncollapsed: the text was not touched", "");

  // A later real collapse reopens admission through a fresh read.
  Check(RunExternalEdit(*env.host, env.doc_a, env.external,
                        ExternalEditMode::kSetCollapsedSelection, 4, 4, L"", &edit,
                        &env.error),
        "noncollapsed: the collapse edit ran", env.error);
  const ObserverDriveResult reopened =
      PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(reopened.settled && reopened.final_state.has_published_target,
        "noncollapsed: a collapsed selection reopens admission",
        DescribeObserverSnapshot(reopened.final_state));
  Check(env.sink.last_target().caret_utf16 == 4,
        "noncollapsed: the reopened caret is the real collapsed position",
        std::to_string(env.sink.last_target().caret_utf16));
  Check(env.observer->Snapshot().selection_refusals >= 1,
        "noncollapsed: the refusal is counted separately", "");
  return Finish();
}

// --- 7. document-manager invalidation leaves no usable target ----------------------------

int RunDocumentUninit() {
  CaseEnv env;
  if (!CreateHostAndDocument(&env, TestDocumentOptions{L"abcdef", 2, 2}, "uninit")) {
    return Finish();
  }
  if (!SubscribeAndBind(&env, env.doc_a, "uninit")) {
    return Finish();
  }
  Check(env.doc_a->Disconnect(), "uninit: Pop(TF_POPF_ALL) succeeded", "");
  const ContextObserverSnapshot after = env.observer->Snapshot();
  Check(env.sink.Count(SinkEvent::Kind::kLost) >= 1,
        "uninit: the real pop/uninit notification dropped the target", "");
  Check(after.uninit_notifications >= 1,
        "uninit: OnUninitDocumentMgr was delivered",
        std::to_string(after.uninit_notifications));
  Check(!after.has_published_target,
        "uninit: no target is published after the manager is destroyed", "");
  Check(after.last_refusal_reason ==
            ContextDropReason::kDocumentManagerUninitialized,
        "uninit: the reason names the document-manager invalidation",
        std::string(DescribeContextDropReason(after.last_refusal_reason)));
  bool has_document = true;
  const HRESULT focus_hr = ReadThreadFocus(*env.host, &has_document);
  Check(!has_document, "uninit: the thread manager reports no focused document",
        "get_focus=0x" + std::to_string(static_cast<unsigned long>(focus_hr)));
  const ObserverDriveResult driven =
      PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(driven.settled && !driven.final_state.has_published_target,
        "uninit: Pump settles with no target",
        DescribeObserverSnapshot(driven.final_state));
  return Finish();
}

// --- 8. our own write does not invalidate the binding ------------------------------------

int RunOwnEdit() {
  CaseEnv env;
  if (!CreateHostAndDocument(&env, TestDocumentOptions{L"abcdef", 2, 2}, "own-edit")) {
    return Finish();
  }
  if (!SubscribeAndBind(&env, env.doc_a, "own-edit")) {
    return Finish();
  }
  const ObservedTarget first = env.sink.last_target();
  const std::size_t lost_before = env.sink.Count(SinkEvent::Kind::kLost);
  const std::size_t binds_before = env.sink.Count(SinkEvent::Kind::kBound);
  ExternalEditResult edit;
  Check(RunOwnEdit(*env.host, env.doc_a, ExternalEditMode::kInsertText, 2, 2, L"y",
                   &edit, &env.error),
        "own-edit: the real own-client write ran", env.error);
  const ContextObserverSnapshot snapshot = env.observer->Snapshot();
  Check(snapshot.own_edits == 1 && snapshot.external_edits == 0,
        "own-edit: InWriteSession(own id) classified the session as ours",
        "own=" + std::to_string(snapshot.own_edits) +
            " external=" + std::to_string(snapshot.external_edits));
  Check(env.sink.Count(SinkEvent::Kind::kLost) == lost_before &&
            env.sink.Count(SinkEvent::Kind::kBound) == binds_before,
        "own-edit: the binding was not invalidated or rebound", "");
  Check(env.sink.Count(SinkEvent::Kind::kOwnEdit) == 1,
        "own-edit: one own-edit event was delivered", "");
  Check(env.sink.last_own_selection().ok() &&
            env.sink.last_own_selection().caret_utf16 == 3,
        "own-edit: the real post-edit caret came from ecReadOnly",
        DescribeSelection(env.sink.last_own_selection()));
  Check(env.sink.last_target().generation == first.generation,
        "own-edit: the generation did not change",
        std::to_string(env.sink.last_target().generation));
  Check(env.doc_a->text_utf16().size() == 7,
        "own-edit: the own write really happened", "");

  // The same thread, a different client: now it is external and must drop.
  Check(RunExternalEdit(*env.host, env.doc_a, env.external,
                        ExternalEditMode::kSetCollapsedSelection, 1, 1, L"", &edit,
                        &env.error),
        "own-edit: the external-client write ran", env.error);
  Check(env.sink.Count(SinkEvent::Kind::kLost) == lost_before + 1 &&
            env.observer->Snapshot().external_edits == 1,
        "own-edit: the other client's write is external, not own",
        DescribeObserverSnapshot(env.observer->Snapshot()));
  Check(env.observer->Snapshot().own_edits == 1,
        "own-edit: the own counter did not absorb the external write", "");
  return Finish();
}

// --- 8. real focus: notification or environment-limited ---------------------------------

int RunRealFocus() {
  CaseEnv env;
  if (!CreateHostAndDocument(&env, TestDocumentOptions{L"abcdef", 2, 2}, "real-focus")) {
    return Finish();
  }
  TestDocument* doc_b =
      env.host->CreateDocument(TestDocumentOptions{L"abcdef", 2, 2}, &env.error);
  Check(doc_b != nullptr, "real-focus: document B created", env.error);
  if (doc_b == nullptr) {
    return Finish();
  }
  if (!SubscribeAndBind(&env, env.doc_a, "real-focus")) {
    return Finish();
  }
  const std::uint64_t focus_before = env.observer->Snapshot().focus_notifications;
  FocusCallResult focus;
  const bool set_ok = SetThreadFocus(*env.host, doc_b, &focus, &env.error);
  Check(set_ok, "real-focus: ITfThreadMgr::SetFocus(B) succeeded", env.error);
  // Give TSF a chance to deliver OnSetFocus, without calling observer.Pump() (which would
  // revalidate GetFocus and manufacture the same drop without a notification).
  PumpMessagesFor(*env.host, 400);
  const ContextObserverSnapshot after_focus = env.observer->Snapshot();
  const bool notification_delivered =
      after_focus.focus_notifications > focus_before;
  const bool notification_dropped =
      notification_delivered && !after_focus.has_published_target &&
      after_focus.last_target_lost_reason == ContextDropReason::kFocusMoved;

  std::printf(
      "[observer-win] evidence real-focus: set_focus=%s get_focus=%d "
      "focus_notifications=%llu->%llu target_published=%d last_lost=%s\n",
      set_ok ? "S_OK" : "FAILED", focus.get_focus_ok ? 1 : 0,
      static_cast<unsigned long long>(focus_before),
      static_cast<unsigned long long>(after_focus.focus_notifications),
      after_focus.has_published_target ? 1 : 0,
      DescribeContextDropReason(after_focus.last_target_lost_reason));

  // AssociateFocus reference ownership: the manager does not AddRef pdim_new, and the
  // pointer returned in ppdimPrev belongs to the caller. This part is independent of
  // whether a window can receive focus.
  HWND window = nullptr;
  std::string window_error;
  if (CreateVisibleTestWindow(&window, &window_error)) {
    ITfDocumentMgr* previous = nullptr;
    const HRESULT associate_hr = AssociateDocumentFocus(
        env.host->thread_manager_for_test(), window,
        doc_b->document_manager_for_test(), &previous);
    Check(SUCCEEDED(associate_hr),
          "real-focus: AssociateFocus(window, B) succeeded",
          "hr=0x" + std::to_string(static_cast<unsigned long>(associate_hr)));
    ReleaseAssociatedDocumentManager(previous);
    ITfDocumentMgr* cleared = nullptr;
    const HRESULT clear_hr = AssociateDocumentFocus(
        env.host->thread_manager_for_test(), window, nullptr, &cleared);
    Check(SUCCEEDED(clear_hr) && cleared != nullptr,
          "real-focus: clearing the association returns the old manager",
          "hr=0x" + std::to_string(static_cast<unsigned long>(clear_hr)));
    if (cleared != nullptr) {
      IUnknown* cleared_unknown = nullptr;
      IUnknown* expected_unknown = nullptr;
      cleared->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&cleared_unknown));
      doc_b->document_manager_for_test()->QueryInterface(
          IID_IUnknown, reinterpret_cast<void**>(&expected_unknown));
      Check(cleared_unknown != nullptr && expected_unknown != nullptr &&
                cleared_unknown == expected_unknown,
            "real-focus: the returned association is the manager we associated", "");
      if (cleared_unknown != nullptr) cleared_unknown->Release();
      if (expected_unknown != nullptr) expected_unknown->Release();
      cleared->Release();
    }
    // SetFocus on our own window is thread-local; it never forces the system foreground.
    ::SetFocus(window);
    PumpMessagesFor(*env.host, 400);
    const ContextObserverSnapshot after_window = env.observer->Snapshot();
    std::printf(
        "[observer-win] evidence associate-focus: focus_notifications=%llu "
        "target_published=%d\n",
        static_cast<unsigned long long>(after_window.focus_notifications),
        after_window.has_published_target ? 1 : 0);
    ::DestroyWindow(window);
  } else {
    std::printf(
        "[observer-win] evidence associate-focus: visible window unavailable (%s) "
        "-> environmental limitation\n",
        window_error.c_str());
  }

  if (notification_delivered && notification_dropped) {
    const ObserverDriveResult driven =
        PumpUntilSettled(*env.observer, *env.host, 20000);
    Check(driven.settled && driven.final_state.has_published_target,
          "real-focus: Pump bound the newly focused document",
          DescribeObserverSnapshot(driven.final_state));
    Check(env.sink.last_target().context_id == ObserveRecord(*env.registry, doc_b).context_id,
          "real-focus: the target is document B", "");
    std::printf("[observer-win] RESULT PASS real focus notification delivered\n");
    return Finish();
  }

  // The notification path was not exercised. Prove the fallback separately, and report
  // the case as environment-limited rather than green.
  const ObserverDriveResult fallback =
      PumpUntilSettled(*env.observer, *env.host, 20000);
  std::printf(
      "[observer-win] evidence focus-revalidation (not a notification): "
      "revalidation_changes=%llu bound=%d\n",
      static_cast<unsigned long long>(
          fallback.final_state.focus_revalidation_changes),
      fallback.final_state.has_published_target ? 1 : 0);
  Check(fallback.settled && fallback.final_state.has_published_target,
        "real-focus: Pump's GetFocus revalidation still finds B", "");
  std::printf(
      "[observer-win] RESULT ENV-LIMITED real focus notification not delivered: "
      "SetFocus changed GetFocus but TSF sent no OnSetFocus to this thread's sink\n");
  return kExitEnvironmentLimited;
}

// --- 9. shutting down with a deferred real read -----------------------------------------

int RunShutdownLateRead() {
  CaseEnv env;
  TestDocumentOptions options;
  options.initial_text_utf16 = L"abcdef";
  options.initial_selection_start_utf16 = 2;
  options.initial_selection_end_utf16 = 2;
  options.refuse_sync_locks = true;  // the read must fall back to a deferred grant
  if (!CreateHostAndDocument(&env, options, "shutdown-late")) {
    return Finish();
  }
  FocusCallResult focus;
  if (!SetThreadFocus(*env.host, env.doc_a, &focus, &env.error)) {
    Check(false, "shutdown-late: SetFocus + GetFocus", env.error);
    return Finish();
  }
  env.registry = std::make_unique<ContextRegistry>(8);
  env.observer = std::make_unique<ContextObserver>(*env.registry,
                                                   env.host->client_id());
  Check(env.observer->Subscribe(env.host->thread_manager_for_test(), &env.sink,
                                &env.error),
        "shutdown-late: subscribed", env.error);
  env.observer->Pump();  // begins the read; the store defers the grant
  ContextObserverSnapshot pending = env.observer->Snapshot();
  Check(pending.selection_read_pending && env.observer->PendingSelectionEvent() != nullptr,
        "shutdown-late: the real read is pending behind a deferred grant",
        DescribeObserverSnapshot(pending));
  Check(env.sink.Count(SinkEvent::Kind::kBound) == 0,
        "shutdown-late: nothing was bound while the read was pending", "");

  env.observer->Shutdown();
  ContextObserverSnapshot closed = env.observer->Snapshot();
  Check(closed.closed && !closed.subscribed,
        "shutdown-late: the observer is closed", "");
  Check(!closed.text_sink_advised && !closed.thread_sink_advised &&
            closed.sink_unadvises == closed.sink_advises,
        "shutdown-late: every advised sink was unadvised",
        "advises=" + std::to_string(closed.sink_advises) +
            " unadvises=" + std::to_string(closed.sink_unadvises));
  Check(!closed.thread_focus_sink_advised &&
            closed.thread_focus_sink_unadvises ==
                closed.thread_focus_sink_advises,
        "shutdown-late: the thread-focus sink was unadvised in pairs",
        "advises=" + std::to_string(closed.thread_focus_sink_advises) +
            " unadvises=" + std::to_string(closed.thread_focus_sink_unadvises));
  Check(closed.selection_reader.reads_abandoned == 1,
        "shutdown-late: the pending read was abandoned", "");

  // The grant was already posted to the real message loop; deliver it after Shutdown.
  env.host->DispatchQueuedMessages();
  ContextObserverSnapshot after = env.observer->Snapshot();
  Check(after.selection_reader.callbacks_after_abandon >= 1,
        "shutdown-late: the late real callback ran on orphaned state",
        "late_callbacks=" +
            std::to_string(after.selection_reader.callbacks_after_abandon));
  Check(env.sink.Count(SinkEvent::Kind::kBound) == 0 &&
            env.sink.Count(SinkEvent::Kind::kLost) == 0,
        "shutdown-late: the late callback published no binding event", "");
  Check(after.thread_mgr_addrefs == after.thread_mgr_releases &&
            after.document_mgr_addrefs == after.document_mgr_releases &&
            after.context_addrefs == after.context_releases,
        "shutdown-late: held COM references are balanced",
        "thread=" + std::to_string(after.thread_mgr_addrefs) + "/" +
            std::to_string(after.thread_mgr_releases) + " dm=" +
            std::to_string(after.document_mgr_addrefs) + "/" +
            std::to_string(after.document_mgr_releases) + " ctx=" +
            std::to_string(after.context_addrefs) + "/" +
            std::to_string(after.context_releases));
  env.observer.reset();
  env.registry.reset();
  env.host->Shutdown();
  Check(TestTextStore::LiveInstanceCount() == 0 &&
            TestTextStore::LiveAdvisedSinkCount() == 0 &&
            TestTextStore::ForcedSinkReleaseCount() == 0,
        "shutdown-late: no store or sink leaked after host teardown", "");
  return Finish();
}

// --- 10. reference and lifetime balance over repeated observers ---------------------------

int RunResourceBalance() {
  std::unique_ptr<TestHost> host = TestHost::Create(nullptr);
  Check(host != nullptr, "balance: host created", "");
  if (host == nullptr) {
    return Finish();
  }
  TestDocument* doc_a = host->CreateDocument(TestDocumentOptions{L"a", 0, 0}, nullptr);
  TestDocument* doc_b = host->CreateDocument(TestDocumentOptions{L"b", 0, 0}, nullptr);
  Check(doc_a != nullptr && doc_b != nullptr, "balance: two documents created", "");
  if (doc_a == nullptr || doc_b == nullptr) {
    return Finish();
  }
  for (int cycle = 0; cycle < 3; ++cycle) {
    ContextRegistry registry(8);
    RecordingSink sink;
    ContextObserver observer(registry, host->client_id());
    FocusCallResult focus;
    Check(SetThreadFocus(*host, doc_a, &focus, nullptr),
          "balance: focus A", "");
    Check(observer.Subscribe(host->thread_manager_for_test(), &sink, nullptr),
          "balance: subscribed", "");
    ObserverDriveResult driven = PumpUntilSettled(observer, *host, 20000);
    Check(driven.settled && driven.final_state.has_published_target,
          "balance: bound A", DescribeObserverSnapshot(driven.final_state));
    Check(SetThreadFocus(*host, doc_b, &focus, nullptr),
          "balance: focus B", "");
    driven = PumpUntilSettled(observer, *host, 20000);
    Check(driven.settled && driven.final_state.has_published_target,
          "balance: revalidated to B", DescribeObserverSnapshot(driven.final_state));
    observer.Shutdown();
    const ContextObserverSnapshot snapshot = observer.Snapshot();
    Check(snapshot.sink_advises == snapshot.sink_unadvises,
          "balance: sink advises and unadvises match",
          std::to_string(snapshot.sink_advises) + "/" +
              std::to_string(snapshot.sink_unadvises));
    Check(!snapshot.thread_focus_sink_advised &&
              snapshot.thread_focus_sink_unadvises ==
                  snapshot.thread_focus_sink_advises &&
              snapshot.thread_focus_sink_advise_failures == 0,
          "balance: the thread-focus sink was advised and unadvised in pairs",
          "advises=" + std::to_string(snapshot.thread_focus_sink_advises) +
              " unadvises=" +
              std::to_string(snapshot.thread_focus_sink_unadvises) +
              " failures=" +
              std::to_string(snapshot.thread_focus_sink_advise_failures));
    Check(snapshot.thread_mgr_addrefs == snapshot.thread_mgr_releases &&
              snapshot.document_mgr_addrefs == snapshot.document_mgr_releases &&
              snapshot.context_addrefs == snapshot.context_releases,
          "balance: all held COM references returned", DescribeObserverSnapshot(snapshot));
    registry.ReleaseAll();
    Check(registry.Diagnostics().entries_balanced(),
          "balance: registry canonical references returned", "");
  }
  host->Shutdown();
  const epocanva::tsf::edit::test::TestHostCleanupReport& cleanup =
      host->cleanup_report();
  Check(cleanup.documents_created == cleanup.documents_destroyed,
        "balance: every document destroyed",
        std::to_string(cleanup.documents_created) + "/" +
            std::to_string(cleanup.documents_destroyed));
  Check(cleanup.all_documents_disconnected && cleanup.all_contexts_released &&
            cleanup.all_document_managers_released,
        "balance: documents, contexts and managers released", "");
  Check(cleanup.document_managers_still_enumerated == 0,
        "balance: the thread manager enumerates no document managers", "");
  Check(cleanup.store_instances_alive == 0 && cleanup.advised_store_sinks_alive == 0 &&
            cleanup.forced_store_sink_releases == 0,
        "balance: no store, advised sink or forced release", "");
  return Finish();
}

// --- falsifiability control ---------------------------------------------------------------

int RunCanFail() {
  Check(false, "can-fail: deliberately false claim");
  const int code = Finish();
  return code == kExitCheckFailed ? kExitSelfCheckFailed : kExitOk;
}

}  // namespace

// P3.13 pure decision table. These are synthetic evidence facts, not TSF notifications;
// the real observer-backed cases call Revalidate() against the live manager separately.
int RunTargetEvidencePure() {
  TargetEvidence evidence;
  evidence.thread_focus_query_ok = true;
  evidence.thread_focus = true;

  TargetRevalidation result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kNoDocumentManager,
        "target-evidence: no document manager",
        DescribeTargetRevalidationStatus(result.status));
  Check(!result.evidence.has_document_manager,
        "target-evidence: the missing document manager is in the evidence", "");

  evidence.has_document_manager = true;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kNoTopContext,
        "target-evidence: no top context",
        DescribeTargetRevalidationStatus(result.status));

  evidence.has_top_context = true;
  evidence.top_context_matches = false;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kContextMismatch,
        "target-evidence: a different top context",
        DescribeTargetRevalidationStatus(result.status));

  evidence.top_context_matches = true;
  evidence.registry_identity_matches = false;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kContextMismatch,
        "target-evidence: a different registry identity",
        DescribeTargetRevalidationStatus(result.status));

  evidence.registry_identity_matches = true;
  evidence.registry_generation_matches = false;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kGenerationMismatch,
        "target-evidence: a stale generation",
        DescribeTargetRevalidationStatus(result.status));

  evidence.registry_generation_matches = true;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kOk,
        "target-evidence: every fact matches",
        DescribeTargetRevalidationStatus(result.status));
  Check(result.thread_focus_evidence == ThreadFocusEvidence::kQuerySet,
        "target-evidence: the source is the IsThreadFocus query",
        DescribeThreadFocusEvidence(result.thread_focus_evidence));

  evidence.thread_focus = false;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kThreadFocusUnconfirmed,
        "target-evidence: unconfirmed thread focus",
        DescribeTargetRevalidationStatus(result.status));

  evidence.thread_focus_notification_kill = true;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kThreadFocusLost,
        "target-evidence: a kill notification is focus lost",
        DescribeTargetRevalidationStatus(result.status));
  Check(result.thread_focus_evidence == ThreadFocusEvidence::kNotificationKill,
        "target-evidence: the kill source is not renamed",
        DescribeThreadFocusEvidence(result.thread_focus_evidence));

  evidence.thread_focus_notification_kill = false;
  evidence.win32_thread_focus = true;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kOk,
        "target-evidence: real Win32 thread focus is an accepted revalidation",
        DescribeTargetRevalidationStatus(result.status));
  Check(result.thread_focus_evidence ==
            ThreadFocusEvidence::kWin32ThreadFocus,
        "target-evidence: the Win32 source is recorded",
        DescribeThreadFocusEvidence(result.thread_focus_evidence));

  evidence.thread_focus_notification_set = true;
  result = EvaluateTargetEvidence(evidence);
  Check(result.thread_focus_evidence == ThreadFocusEvidence::kNotificationSet,
        "target-evidence: a set notification is reported as a notification",
        DescribeThreadFocusEvidence(result.thread_focus_evidence));
  return Finish();
}

int RunObserverCase(const std::string& case_name) {
  if (case_name == "registry-identity") return RunRegistryIdentity();
  if (case_name == "initial-binding") return RunInitialBinding();
  if (case_name == "push-pop-top") return RunPushPopTop();
  if (case_name == "external-selection") return RunExternalSelection();
  if (case_name == "external-text") return RunExternalText();
  if (case_name == "noncollapsed-selection") return RunNonCollapsedSelection();
  if (case_name == "document-uninit") return RunDocumentUninit();
  if (case_name == "own-edit") return RunOwnEdit();
  if (case_name == "real-focus") return RunRealFocus();
  if (case_name == "shutdown-late-read") return RunShutdownLateRead();
  if (case_name == "target-evidence") return RunTargetEvidencePure();
  if (case_name == "resource-balance") return RunResourceBalance();
  if (case_name == "can-fail") return RunCanFail();
  return -1;
}

int RunInteractiveFocus() {
  std::string error;
  std::unique_ptr<TestHost> host = TestHost::Create(&error);
  if (host == nullptr) {
    std::printf("interactive-focus: host creation failed: %s\n", error.c_str());
    return kExitUsage;
  }
  TestDocument* document =
      host->CreateDocument(TestDocumentOptions{L"interactive focus", 0, 0}, &error);
  if (document == nullptr) {
    std::printf("interactive-focus: document creation failed: %s\n", error.c_str());
    return kExitUsage;
  }
  HWND window = nullptr;
  if (!CreateVisibleTestWindow(&window, &error)) {
    std::printf(
        "interactive-focus: this session cannot create a visible window (%s). "
        "Run this mode from a normal interactive desktop.\n",
        error.c_str());
    host->Shutdown();
    return kExitUsage;
  }
  ContextRegistry registry(8);
  RecordingSink sink;
  ContextObserver observer(registry, host->client_id());
  ITfDocumentMgr* previous = nullptr;
  const HRESULT associate_hr = AssociateDocumentFocus(
      host->thread_manager_for_test(), window,
      document->document_manager_for_test(), &previous);
  std::printf(
      "interactive-focus: AssociateFocus hr=0x%08lX previous=%p "
      "(TSF does not AddRef pdimNew; the returned previous is ours to release)\n",
      static_cast<unsigned long>(associate_hr), static_cast<void*>(previous));
  ReleaseAssociatedDocumentManager(previous);
  if (!observer.Subscribe(host->thread_manager_for_test(), &sink, &error)) {
    std::printf("interactive-focus: subscribe failed: %s\n", error.c_str());
    ::DestroyWindow(window);
    host->Shutdown();
    return kExitUsage;
  }
  observer.Pump();
  std::printf(
      "interactive-focus: click the test window now (20 seconds). The program prints "
      "every OnSetFocus/OnPushContext/OnPopContext/OnEndEdit observation.\n");
  const ULONGLONG deadline = ::GetTickCount64() + 20000;
  while (::GetTickCount64() < deadline) {
    PumpMessagesFor(*host, 100);
    observer.Pump();
    const ContextObserverSnapshot snapshot = observer.Snapshot();
    if (snapshot.focus_notifications > 0 && snapshot.has_published_target) {
      break;
    }
  }
  const ContextObserverSnapshot snapshot = observer.Snapshot();
  std::printf(
      "interactive-focus: focus_notifications=%llu bound=%d id=%llu gen=%llu "
      "caret=%ld binds=%llu drops=%llu\n",
      static_cast<unsigned long long>(snapshot.focus_notifications),
      snapshot.has_published_target ? 1 : 0,
      static_cast<unsigned long long>(snapshot.target.context_id),
      static_cast<unsigned long long>(snapshot.target.generation),
      snapshot.target.caret_utf16,
      static_cast<unsigned long long>(snapshot.target_binds),
      static_cast<unsigned long long>(snapshot.target_drops));
  observer.Shutdown();
  ITfDocumentMgr* cleared = nullptr;
  const HRESULT clear_hr = AssociateDocumentFocus(
      host->thread_manager_for_test(), window, nullptr, &cleared);
  std::printf("interactive-focus: clear hr=0x%08lX previous=%p\n",
              static_cast<unsigned long>(clear_hr), static_cast<void*>(cleared));
  ReleaseAssociatedDocumentManager(cleared);
  ::DestroyWindow(window);
  host->Shutdown();
  if (snapshot.focus_notifications > 0) {
    std::printf("interactive-focus: OBSERVED a real OnSetFocus notification\n");
  } else {
    std::printf("interactive-focus: NOT-OBSERVED (no focus notification arrived)\n");
  }
  return kExitOk;
}

}  // namespace test
}  // namespace context
}  // namespace tsf
}  // namespace epocanva
