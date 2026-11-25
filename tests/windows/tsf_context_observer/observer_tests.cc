// EpoCanva P3.10 -- context-observer verification cases against the real TSF manager.
//
// All bindings, drops and rebinds in these cases come from real msctf.dll notifications
// (ITfThreadMgrEventSink / ITfTextEditSink) and real selection reads. Nothing here calls a
// sink method directly. The one deliberately non-notification path (Pump's defensive
// GetFocus revalidation) is reported as such and never counted as real focus coverage.
//
// EXIT CODES: 0 pass; 5 a check or prerequisite failed; 14 the falsifiability control.
#include <windows.h>
#include <msctf.h>
#include <wrl/client.h>

#include <array>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
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
  env->observer = std::make_unique<ContextObserver>(
      *env->registry, env->host->client_id(), ThreadFocusPolicy::kStrictTsf);
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

using Microsoft::WRL::ComPtr;

bool SameManager(ITfDocumentMgr* left, ITfDocumentMgr* right) {
  if (left == right) {
    return true;
  }
  ComPtr<IUnknown> left_identity;
  ComPtr<IUnknown> right_identity;
  return left != nullptr && right != nullptr &&
         SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&left_identity))) &&
         SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&right_identity))) &&
         left_identity != nullptr && left_identity.Get() == right_identity.Get();
}

template <typename Predicate>
bool WaitOnSta(TestHost& host, DWORD timeout_ms, Predicate ready) {
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    host.DispatchQueuedMessages();
    if (ready()) {
      return true;
    }
    const ULONGLONG now = ::GetTickCount64();
    if (now >= deadline ||
        ::MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(deadline - now),
                                      QS_ALLINPUT, MWMO_INPUTAVAILABLE) != WAIT_OBJECT_0) {
      return false;
    }
  }
}

class FocusWindows {
 public:
  explicit FocusWindows(ITfThreadMgr* manager) : manager_(manager) {}
  ~FocusWindows() { Close(); }
  FocusWindows(const FocusWindows&) = delete;
  FocusWindows& operator=(const FocusWindows&) = delete;

  bool Create(std::string* error) {
    const HINSTANCE instance = ::GetModuleHandleW(nullptr);
    root_ = ::CreateWindowExW(0, L"STATIC", L"EpoCanva TSF focus test", WS_OVERLAPPEDWINDOW,
                              120, 120, 460, 180, nullptr, nullptr, instance, nullptr);
    if (root_ != nullptr) {
      ::SetWindowLongPtrW(root_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
      original_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
          root_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&FocusWindows::WindowProc)));
      start_ = ::CreateWindowExW(0, L"BUTTON", L"Start focus verification",
          WS_CHILD | WS_TABSTOP, 12, 82, 260, 32, root_,
          reinterpret_cast<HMENU>(1000), instance, nullptr);
      const wchar_t* const labels[] = {L"A", L"B", L"Empty document"};
      for (std::size_t slot = 0; slot < windows_.size(); ++slot) {
        windows_[slot].handle = ::CreateWindowExW(
            0, L"BUTTON", labels[slot], WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            12 + static_cast<int>(slot) * 138, 24, 130, 40, root_, nullptr, instance, nullptr);
        if (windows_[slot].handle == nullptr) {
          break;
        }
      }
      if (windows_.back().handle != nullptr && start_ != nullptr && original_proc_ != nullptr) {
        return true;
      }
    }
    *error = "CreateWindowExW failed: " + std::to_string(::GetLastError());
    return false;
  }

  bool AcquireForeground(TestHost& host) {
    ::ShowWindow(root_, SW_SHOWNORMAL);
    const BOOL requested = ::SetForegroundWindow(root_);
    wchar_t interactive[2] = {};
    const bool wait_for_user = ::GetEnvironmentVariableW(
        L"EPOCANVA_TSF_FOCUS_WAIT_FOR_USER", interactive, 2) == 1 && interactive[0] == L'1';
    if (wait_for_user && ::GetForegroundWindow() != root_) {
      std::printf("Activate the EpoCanva TSF focus test window by its title bar.\n");
      std::fflush(stdout);
    }
    const bool acquired = WaitOnSta(host, wait_for_user ? 45000 : 2000,
                                    [&] { return ::GetForegroundWindow() == root_; });
    std::printf("WIN32_FOREGROUND|requested=%d|acquired=%d|manual_wait_enabled=%d\n",
                requested != FALSE, acquired, wait_for_user);
    if (!acquired || !wait_for_user) return acquired;
    // External window activation may still send focus/input messages after it
    // first makes this root foreground. Start the matrix only after a genuine
    // button release, so those messages cannot race the first A -> B assertion.
    ::ShowWindow(start_, SW_SHOW);
    std::printf("Click Start focus verification after activating the test window.\n");
    std::fflush(stdout);
    const bool ready = WaitOnSta(host, 120000, [&] { return start_requested_; });
    ::ShowWindow(start_, SW_HIDE);
    std::printf("WIN32_START|button_clicked=%d|foreground=%d\n",
                ready, ::GetForegroundWindow() == root_);
    return ready && ::GetForegroundWindow() == root_;
  }

  bool Associate(std::size_t slot, ITfDocumentMgr* next, ITfDocumentMgr* expected_previous) {
    Window& window = windows_[slot];
    ComPtr<ITfDocumentMgr> next_owner(next);
    ComPtr<ITfDocumentMgr> previous;
    const HRESULT hr = manager_->AssociateFocus(window.handle, next, &previous);
    const bool matched = SameManager(previous.Get(), expected_previous);
    std::printf("FOCUS_ASSOCIATION|slot=%zu|hr=%08lX|new=%d|previous=%d|matches=%d\n",
                slot, static_cast<unsigned long>(hr), next != nullptr, previous != nullptr, matched);
    if (SUCCEEDED(hr)) {
      if (!window.associated) {
        window.original = previous;
        window.associated = true;
      }
      window.current = std::move(next_owner);
    }
    return SUCCEEDED(hr) && matched;
  }

  bool Focus(std::size_t slot) const {
    const HWND requested = windows_[slot].handle;
    const HWND previous = ::GetFocus();
    ::SetLastError(ERROR_SUCCESS);
    ::SetFocus(requested);
    const DWORD error = ::GetLastError();
    const HWND actual = ::GetFocus();
    std::printf("WIN32_FOCUS|slot=%zu|changed=%d|match=%d|foreground=%d|thread=%lu|error=%lu\n",
                slot, previous != actual, actual == requested, ::GetForegroundWindow() == root_,
                actual == nullptr ? 0 : ::GetWindowThreadProcessId(actual, nullptr), error);
    return previous != actual && actual == requested && ::GetForegroundWindow() == root_;
  }

  bool Close() {
    bool ok = true;
    for (std::size_t slot = 0; slot < windows_.size(); ++slot) {
      Window& window = windows_[slot];
      if (window.associated) {
        ok = Associate(slot, window.original.Get(), window.current.Get()) && ok;
        window.associated = false;
      }
    }
    if (root_ != nullptr) {
      ok = ::DestroyWindow(root_) != FALSE && !::IsWindow(root_) && ok;
      root_ = nullptr;
    }
    for (Window& window : windows_) {
      ok = (window.handle == nullptr || !::IsWindow(window.handle)) && ok;
      window = {};
    }
    manager_.Reset();
    return ok;
  }

 private:
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<FocusWindows*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self != nullptr && message == WM_COMMAND && LOWORD(wparam) == 1000 &&
        HIWORD(wparam) == BN_CLICKED && reinterpret_cast<HWND>(lparam) == self->start_) {
      self->start_requested_ = true;
      return 0;
    }
    return self != nullptr && self->original_proc_ != nullptr
        ? ::CallWindowProcW(self->original_proc_, window, message, wparam, lparam)
        : ::DefWindowProcW(window, message, wparam, lparam);
  }
  struct Window {
    HWND handle = nullptr;
    bool associated = false;
    ComPtr<ITfDocumentMgr> original;
    ComPtr<ITfDocumentMgr> current;
  };
  ComPtr<ITfThreadMgr> manager_;
  HWND root_ = nullptr;
  HWND start_ = nullptr;
  WNDPROC original_proc_ = nullptr;
  bool start_requested_ = false;
  std::array<Window, 3> windows_;
};

bool TraceFocusState(CaseEnv& env, const char* stage, ITfDocumentMgr* expected) {
  const auto snapshot = env.observer->Snapshot();
  auto* manager = env.host->thread_manager_for_test();
  ComPtr<ITfDocumentMgr> actual;
  const HRESULT focus_hr = manager->GetFocus(&actual);
  ComPtr<ITfContext> top;
  const HRESULT top_hr = actual == nullptr ? S_FALSE : actual->GetTop(&top);
  BOOL thread_focused = FALSE;
  const HRESULT thread_hr = manager->IsThreadFocus(&thread_focused);
  const bool matched = SUCCEEDED(focus_hr) && SameManager(actual.Get(), expected);
  std::printf("FOCUS_TRACE|stage=%s|thread=%lu|apartment_hr=%08lX|apartment=%d|activated=%d"
              "|get_focus_hr=%08lX|manager=%d|manager_matches=%d|top_hr=%08lX|top=%d|thread_focus_hr=%08lX"
              "|thread_focus=%d|doc_enter=%llu|doc_exit=%llu|thread_set=%llu|thread_kill=%llu"
              "|doc_losses=%llu|poll_changes=%llu|drops=%llu|published=%d|id=%llu|generation=%llu\n",
              stage, snapshot.owner_thread_id, static_cast<unsigned long>(snapshot.apartment_query_hr),
              snapshot.apartment_type, env.host->cleanup_report().thread_manager_activated,
              static_cast<unsigned long>(focus_hr), actual != nullptr, matched,
              static_cast<unsigned long>(top_hr), top != nullptr, static_cast<unsigned long>(thread_hr),
              thread_focused,
              static_cast<unsigned long long>(snapshot.focus_notifications),
              static_cast<unsigned long long>(snapshot.focus_notification_exits),
              static_cast<unsigned long long>(snapshot.thread_focus_set_notifications),
              static_cast<unsigned long long>(snapshot.thread_focus_kill_notifications),
              static_cast<unsigned long long>(snapshot.focus_loss_notifications),
              static_cast<unsigned long long>(snapshot.focus_revalidation_changes),
              static_cast<unsigned long long>(snapshot.target_drops), snapshot.has_published_target,
              static_cast<unsigned long long>(snapshot.target.context_id),
              static_cast<unsigned long long>(snapshot.target.generation));
  std::printf("FOCUS_ADVICE|stage=%s|doc_iid=aa80e80e-2021-11d2-93e0-0060b067b86e"
              "|doc_hr=%08lX|doc_cookie=%lu|thread_iid=c0f1db0c-3a20-405c-a303-96b6010a885f"
              "|thread_hr=%08lX|thread_cookie=%lu|text_iid=8127d409-ccd3-4683-967a-b43d5b482bf7"
              "|text_hr=%08lX|text_cookie=%lu|unadvise_hr=%08lX|unadvise_failures=%llu\n",
              stage, static_cast<unsigned long>(snapshot.thread_sink_advise_hr), snapshot.thread_sink_cookie,
              static_cast<unsigned long>(snapshot.thread_focus_sink_advise_hr), snapshot.thread_focus_sink_cookie,
              static_cast<unsigned long>(snapshot.text_sink_advise_hr), snapshot.text_sink_cookie,
              static_cast<unsigned long>(snapshot.last_sink_unadvise_hr),
              static_cast<unsigned long long>(snapshot.sink_unadvise_failures));
  const HWND window = ::GetFocus();
  std::printf("FOCUS_LIFETIME|stage=%s|hwnd=%p|hwnd_thread=%lu|deactivated=%d"
              "|manager_refs=%llu/%llu|document_refs=%llu/%llu|context_refs=%llu/%llu"
              "|late_notifications=%llu|reentrant_pumps_refused=%llu\n",
              stage, static_cast<void*>(window),
              window == nullptr ? 0 : ::GetWindowThreadProcessId(window, nullptr),
              env.host->cleanup_report().thread_manager_deactivated,
              static_cast<unsigned long long>(snapshot.thread_mgr_addrefs),
              static_cast<unsigned long long>(snapshot.thread_mgr_releases),
              static_cast<unsigned long long>(snapshot.document_mgr_addrefs),
              static_cast<unsigned long long>(snapshot.document_mgr_releases),
              static_cast<unsigned long long>(snapshot.context_addrefs),
              static_cast<unsigned long long>(snapshot.context_releases),
              static_cast<unsigned long long>(snapshot.late_notifications_after_close),
              static_cast<unsigned long long>(snapshot.reentrant_pumps_refused));
  return matched;
}

int RunRealFocus() {
  CaseEnv env;
  const TestDocumentOptions options{L"abcdef", 2, 2};
  if (!CreateHostAndDocument(&env, options, "real-focus")) {
    return Finish();
  }
  auto* doc_b = env.host->CreateDocument(options, &env.error);
  Check(doc_b != nullptr, "real-focus: document B created", env.error);
  if (doc_b == nullptr) {
    return Finish();
  }
  auto* manager = env.host->thread_manager_for_test();
  ComPtr<ITfDocumentMgr> empty;
  const HRESULT empty_hr = manager->CreateDocumentMgr(&empty);
  Check(SUCCEEDED(empty_hr) && empty != nullptr, "real-focus: empty document manager created", "");
  FocusWindows windows(manager);
  const bool created = windows.Create(&env.error);
  Check(created, "real-focus: test windows created on the same STA", env.error);
  if (!created || empty == nullptr) {
    return Finish();
  }
  auto* manager_a = env.doc_a->document_manager_for_test();
  auto* manager_b = doc_b->document_manager_for_test();
  Check(windows.Associate(0, manager_a, nullptr) && windows.Associate(1, manager_b, nullptr) &&
            windows.Associate(2, empty.Get(), nullptr),
        "real-focus: initial associations installed", "");
  env.registry = std::make_unique<ContextRegistry>(8);
  env.observer = std::make_unique<ContextObserver>(
      *env.registry, env.host->client_id(), ThreadFocusPolicy::kStrictTsf);
  const bool subscribed = env.observer->Subscribe(manager, &env.sink, &env.error);
  Check(subscribed, "real-focus: observer subscribed", env.error);
  if (!subscribed) {
    return Finish();
  }
  TraceFocusState(env, "subscribed", nullptr);
  const bool foreground = windows.AcquireForeground(*env.host);
  Check(foreground, "real-focus: test window obtains genuine foreground focus", "");
  if (!foreground) {
    TraceFocusState(env, "foreground-refused", nullptr);
    return Finish();
  }
  Check(windows.Focus(0), "real-focus: A gains Win32 keyboard focus", "");
  env.host->DispatchQueuedMessages();
  Check(IsDocumentFocused(*env.host, env.doc_a), "real-focus: focusing A selected its document", "");
  const auto initial = PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(initial.settled && initial.final_state.has_published_target &&
            initial.final_state.thread_sink_advised && initial.final_state.thread_focus_sink_advised,
        "real-focus: initial target published with both real focus interfaces advised",
        DescribeObserverSnapshot(initial.final_state));
  TraceFocusState(env, "initial-A", manager_a);

  auto transition = [&](std::size_t slot, ITfDocumentMgr* expected, TestDocument* document,
                        const char* stage) {
    const auto before = env.observer->Snapshot();
    Check(windows.Focus(slot), std::string(stage) + ": actual Win32 keyboard focus changed", "");
    const bool notified = WaitOnSta(*env.host, 2000, [&] {
      return env.observer->Snapshot().focus_notifications > before.focus_notifications;
    });
    const auto after = env.observer->Snapshot();
    const bool matched = TraceFocusState(env, stage, expected);
    Check(notified && after.focus_notification_exits > before.focus_notification_exits &&
              after.focus_revalidations == before.focus_revalidations,
          std::string(stage) + ": real OnSetFocus completed before observer Pump", "");
    Check(!after.has_published_target && !env.sink.has_target() &&
              (!before.has_published_target || after.target_drops > before.target_drops),
          std::string(stage) + ": old target withdrawn synchronously", DescribeObserverSnapshot(after));
    Check(matched, std::string(stage) + ": GetFocus names the associated manager", "");
    const auto settled = PumpUntilSettled(*env.observer, *env.host, 20000);
    if (document != nullptr) {
      Check(settled.settled && settled.final_state.has_published_target &&
                settled.final_state.target.context_id == ObserveRecord(*env.registry, document).context_id &&
                settled.final_state.target.evidence == TargetBindingEvidence::kDocumentFocusNotification,
            std::string(stage) + ": published the expected context with notification evidence",
            DescribeObserverSnapshot(settled.final_state));
    } else {
      Check(settled.settled && !settled.final_state.has_published_target &&
                !env.sink.has_target(),
            std::string(stage) + ": no writable target for the empty or absent document",
            DescribeObserverSnapshot(settled.final_state));
      if (expected == nullptr) {
        Check(after.focus_loss_notifications > before.focus_loss_notifications &&
                  !settled.final_state.has_top_context,
              std::string(stage) + ": null document callback and no remaining context", "");
      }
    }
    return settled.final_state.target;
  };

  const auto old_b = transition(1, manager_b, doc_b, "A-to-B");
  transition(0, manager_a, env.doc_a, "B-to-A");
  const auto guard = env.observer->CreateWriteGuard();
  Check(old_b.valid() && !guard->StillCurrent(doc_b->context(), old_b.context_id, old_b.generation),
        "real-focus: late B work refused while A is focused", "");
  const auto new_b = transition(1, manager_b, doc_b, "A-to-B-again");
  Check(new_b.context_id == old_b.context_id && new_b.generation > old_b.generation &&
            !guard->StillCurrent(doc_b->context(), old_b.context_id, old_b.generation),
        "real-focus: return to B uses a new generation and refuses old work", "");
  transition(2, empty.Get(), nullptr, "B-to-empty");
  Check(!guard->StillCurrent(doc_b->context(), new_b.context_id, new_b.generation),
        "real-focus: empty document transition rejects late work for B", "");
  transition(1, manager_b, doc_b, "empty-to-B");

  Check(windows.Associate(1, nullptr, manager_b), "real-focus: clear B association returns B", "");
  TraceFocusState(env, "B-association-cleared", manager_b);
  const auto cleared = PumpUntilSettled(*env.observer, *env.host, 20000);
  const bool publishes_b = cleared.final_state.has_published_target &&
                           cleared.final_state.target.context_id == old_b.context_id;
  Check(cleared.settled && (IsDocumentFocused(*env.host, doc_b)
                              ? publishes_b : !cleared.final_state.has_published_target),
        "real-focus: compare B publication with live document focus after unassociation",
        DescribeObserverSnapshot(cleared.final_state));
  transition(0, manager_a, env.doc_a, "cleared-B-to-A");
  transition(1, nullptr, nullptr, "A-to-unassociated-B");
  transition(0, manager_a, env.doc_a, "unassociated-B-to-A");
  Check(windows.Associate(1, manager_b, nullptr), "real-focus: restore B association while A has focus", "");
  transition(1, manager_b, doc_b, "A-to-restored-B");

  env.observer->Shutdown();
  const auto closed = env.observer->Snapshot();
  TraceFocusState(env, "observer-shutdown", manager_b);
  Check(!closed.has_published_target && closed.sink_advises == closed.sink_unadvises &&
            closed.thread_focus_sink_advises == closed.thread_focus_sink_unadvises &&
            closed.sink_unadvise_failures == 0,
        "real-focus: close withdraws target and successfully unadvises each sink", "");
  Check(windows.Close(), "real-focus: original associations restored and all HWNDs destroyed", "");
  empty.Reset();
  env.host->Shutdown();
  Check(env.host->cleanup_report().thread_manager_deactivated,
        "real-focus: host deactivated the thread manager", "");
  return Finish();
}

int RunAssociationContract() {
  CaseEnv env;
  const TestDocumentOptions options{L"abcdef", 2, 2};
  if (!CreateHostAndDocument(&env, options, "association")) {
    return Finish();
  }
  auto* doc_b = env.host->CreateDocument(options, &env.error);
  Check(doc_b != nullptr, "association: document B created", env.error);
  if (doc_b == nullptr) {
    return Finish();
  }
  auto* manager_a = env.doc_a->document_manager_for_test();
  auto* manager_b = doc_b->document_manager_for_test();
  for (int cycle = 0; cycle < 12; ++cycle) {
    FocusWindows windows(env.host->thread_manager_for_test());
    const bool created = windows.Create(&env.error);
    Check(created, "association: owned windows created", env.error);
    if (!created) {
      return Finish();
    }
    Check(windows.Associate(0, manager_a, nullptr), "association: A installed", "");
    Check(windows.Associate(0, manager_b, manager_a), "association: replacement returns A", "");
    Check(windows.Associate(0, manager_a, manager_b), "association: restoration returns B", "");
    Check(windows.Associate(0, nullptr, manager_a), "association: unassociation returns A", "");
    Check(windows.Associate(0, manager_a, nullptr), "association: unassociated mapping was empty", "");
    Check(windows.Close(), "association: original mapping restored before window destruction", "");
  }
  return Finish();
}

int RunRevokedTargetGuard() {
  CaseEnv env;
  if (!CreateHostAndDocument(&env, TestDocumentOptions{L"abcdef", 2, 2}, "revoked-guard")) {
    return Finish();
  }
  const bool window_focused = EnsureTestThreadFocus(&env.error);
  Check(window_focused, "revoked-guard: real lab Win32 focus", env.error);
  FocusCallResult focus;
  const bool document_focused = SetThreadFocus(*env.host, env.doc_a, &focus, &env.error);
  Check(document_focused, "revoked-guard: document A focused", env.error);
  if (!window_focused || !document_focused) {
    return Finish();
  }
  env.registry = std::make_unique<ContextRegistry>(8);
  env.observer = std::make_unique<ContextObserver>(
      *env.registry, env.host->client_id(), ThreadFocusPolicy::kLabWin32Fallback);
  const bool subscribed =
      env.observer->Subscribe(env.host->thread_manager_for_test(), &env.sink, &env.error);
  Check(subscribed, "revoked-guard: subscribed to real TSF", env.error);
  if (!subscribed) {
    return Finish();
  }
  const auto advice = env.observer->Snapshot();
  Check(!env.observer->Subscribe(env.host->thread_manager_for_test(), &env.sink, &env.error) &&
            env.observer->Snapshot().sink_advises == advice.sink_advises &&
            env.observer->Snapshot().thread_focus_sink_advises == advice.thread_focus_sink_advises,
        "revoked-guard: duplicate subscribe cannot create more advice cookies", "");
  const auto initial = PumpUntilSettled(*env.observer, *env.host, 20000);
  Check(initial.settled && initial.final_state.has_published_target,
        "revoked-guard: initial target published", DescribeObserverSnapshot(initial.final_state));
  if (!initial.final_state.has_published_target) {
    return Finish();
  }
  const ObservedTarget original = initial.final_state.target;
  const auto guard = env.observer->CreateWriteGuard();
  Check(guard->StillCurrent(env.doc_a->context(), original.context_id, original.generation),
        "revoked-guard: live target accepted before revocation", "");

  ExternalEditResult edit;
  const bool edited = RunExternalEdit(*env.host, env.doc_a, env.external,
                                     ExternalEditMode::kSetCollapsedSelection, 3, 3, L"",
                                     &edit, &env.error);
  Check(edited, "revoked-guard: another real TSF client moved selection", env.error);
  const auto dropped = env.observer->Snapshot();
  Check(!dropped.has_published_target && !env.sink.has_target() &&
            dropped.last_target_lost_reason == ContextDropReason::kExternalSelectionEdit,
        "revoked-guard: OnEndEdit withdrew the published target before Pump",
        DescribeObserverSnapshot(dropped));
  Check(IsDocumentFocused(*env.host, env.doc_a) &&
            ObserveRecord(*env.registry, env.doc_a).generation == original.generation,
        "revoked-guard: live document and registry generation have not changed", "");
  Check(!guard->StillCurrent(env.doc_a->context(), original.context_id, original.generation),
        "revoked-guard: late edit refused before a replacement exists", "");

  const auto driven = PumpUntilSettled(*env.observer, *env.host, 20000);
  const ObservedTarget replacement = driven.final_state.target;
  Check(driven.settled && driven.final_state.has_published_target &&
            replacement.context_id == original.context_id &&
            replacement.generation > original.generation,
        "revoked-guard: replacement has a new generation", DescribeObserverSnapshot(driven.final_state));
  Check(!guard->StillCurrent(env.doc_a->context(), original.context_id, original.generation) &&
            guard->StillCurrent(env.doc_a->context(), replacement.context_id, replacement.generation),
        "revoked-guard: only the replacement capability works", "");
  const auto losses_before_shutdown = env.observer->Snapshot().target_drops;
  env.observer->Shutdown();
  env.observer->Shutdown();
  const auto closed = env.observer->Snapshot();
  Check(closed.closed && !closed.has_published_target && !env.sink.has_target() &&
            closed.target_drops == losses_before_shutdown + 1 &&
            closed.last_target_lost_reason == ContextDropReason::kClosed,
        "revoked-guard: shutdown withdraws the listener's target exactly once",
        DescribeObserverSnapshot(closed));
  Check(!guard->StillCurrent(env.doc_a->context(), replacement.context_id, replacement.generation),
        "revoked-guard: closed observer refuses late work", "");
  env.observer.reset();
  Check(!guard->StillCurrent(env.doc_a->context(), replacement.context_id, replacement.generation),
        "revoked-guard: destroyed observer refuses late work", "");
  return Finish();
}

// --- 9. shutting down with a deferred real read -----------------------------------------

class ReentrantSink final : public ContextObserverSink {
 public:
  std::function<void()> on_lost;
  void OnTargetLost(ContextDropReason) override { if (on_lost) on_lost(); }
  void OnTargetBound(ITfContext*, const ObservedTarget&) override {}
  void OnOwnEditCompleted(ITfContext*, const ObservedTarget&,
                          const SelectionSnapshot&) override {}
};

int RunReentrantShutdown() {
  ReentrantSink sink;  // outlives the observer, including early returns
  CaseEnv env;
  if (!CreateHostAndDocument(&env, TestDocumentOptions{L"abcdef", 2, 2}, "reentrant") ||
      !SubscribeAndBind(&env, env.doc_a, "reentrant")) {
    return Finish();
  }
  env.observer->SetSink(&sink);
  bool called = false;
  sink.on_lost = [&] {
    called = true;
    const auto before = env.observer->Snapshot();
    env.observer->RequestRebind();
    env.observer->Pump();
    const auto after = env.observer->Snapshot();
    Check(after.focus_revalidations == before.focus_revalidations &&
              after.selection_reads_begun == before.selection_reads_begun,
          "reentrant: Pump from OnTargetLost issues no query or edit session", "");
    env.observer->Shutdown();
  };
  ExternalEditResult edit;
  Check(RunExternalEdit(*env.host, env.doc_a, env.external,
                        ExternalEditMode::kSetCollapsedSelection, 3, 3, L"",
                        &edit, &env.error),
        "reentrant: real external edit delivered OnEndEdit", env.error);
  const auto closed = env.observer->Snapshot();
  Check(called && closed.closed && !closed.awaiting_bind && !closed.selection_read_pending &&
            !closed.has_published_target && !closed.has_top_context,
        "reentrant: shutdown inside callback stays terminal after callback returns",
        DescribeObserverSnapshot(closed));
  Check(closed.sink_advises == closed.sink_unadvises &&
            closed.thread_focus_sink_advises == closed.thread_focus_sink_unadvises &&
            closed.sink_unadvise_failures == 0 &&
            closed.context_addrefs == closed.context_releases,
        "reentrant: callback shutdown releases all subscriptions and contexts", "");
  return Finish();
}

int RunThreadOwnership() {
  CaseEnv env;
  if (!CreateHostAndDocument(&env, TestDocumentOptions{L"abcdef", 2, 2}, "owner") ||
      !SubscribeAndBind(&env, env.doc_a, "owner")) {
    return Finish();
  }
  const auto before = env.observer->Snapshot();
  auto* context = env.doc_a->context();  // never dereferenced on the foreign thread
  auto* manager = env.host->thread_manager_for_test();
  const auto guard = env.observer->CreateWriteGuard();
  bool refused = false;
  std::thread foreign([&] {
    std::string error;
    const bool subscribed = env.observer->Subscribe(manager, &env.sink, &error);
    env.observer->SetSink(nullptr);
    env.observer->RequestRebind();
    env.observer->Pump();
    env.observer->Shutdown();
    const auto result = env.observer->Revalidate(context, before.target.context_id,
                                                before.target.generation);
    const auto snapshot = env.observer->Snapshot();
    refused = !subscribed && !error.empty() && result.query_hr == RPC_E_WRONG_THREAD &&
              !guard->StillCurrent(context, before.target.context_id, before.target.generation) &&
              snapshot.access_hr == RPC_E_WRONG_THREAD && !snapshot.has_published_target &&
              env.observer->closed() && !env.observer->has_current_context() &&
              env.observer->PendingSelectionEvent() == nullptr;
  });
  foreign.join();
  const auto after = env.observer->Snapshot();
  Check(refused, "owner: all foreign-thread entry points refuse without exposing state", "");
  Check(!after.closed && after.subscribed && after.has_published_target &&
            after.target.context_id == before.target.context_id &&
            after.target.generation == before.target.generation &&
            after.pumps == before.pumps && after.target_revalidations == before.target_revalidations &&
            after.sink_advises == before.sink_advises && after.sink_unadvises == before.sink_unadvises,
        "owner: foreign calls did not change subscriptions, target or query counters", "");
  env.observer->Shutdown();
  Check(!env.sink.has_target() && env.observer->Snapshot().closed,
        "owner: owner STA still owns and completes shutdown", "");

  bool mta_refused = false;
  std::thread mta([&] {
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
      {
        ContextRegistry registry;
        ContextObserver observer(registry, 0, ThreadFocusPolicy::kStrictTsf);
        RecordingSink sink;
        std::string error;
        mta_refused = !observer.Subscribe(nullptr, &sink, &error) &&
                      error == "observer subscription requires an initialized STA" &&
                      observer.Snapshot().thread_mgr_addrefs == 0;
      }
      ::CoUninitialize();
    }
  });
  mta.join();
  Check(mta_refused, "owner: MTA cannot acquire STA resources", "");
  return Finish();
}

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
  env.observer = std::make_unique<ContextObserver>(
      *env.registry, env.host->client_id(), ThreadFocusPolicy::kStrictTsf);
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
    ContextObserver observer(registry, host->client_id(),
                             ThreadFocusPolicy::kStrictTsf);
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
    Check(snapshot.sink_unadvise_failures == 0,
          "balance: the real UnadviseSink calls all succeeded", "");
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
  // Keep window ownership separate from the controlled document-manager
  // teardown above: a real focus window can cause TSF to create its own manager.
  // Every original document/COM cleanup assertion must still pass first.
  std::string focus_error;
  Check(EnsureTestThreadFocus(&focus_error),
        "balance: focus helper created", focus_error);
  const HWND focus_window = TestThreadFocusWindow();
  bool reused_window = focus_window != nullptr && ::IsWindow(focus_window);
  for (int cycle = 0; cycle < 128; ++cycle) {
    reused_window = EnsureTestThreadFocus(&focus_error) && reused_window &&
                    TestThreadFocusWindow() == focus_window;
  }
  Check(reused_window, "balance: repeated focus setup reuses one owned HWND",
        focus_error);
  Check(DestroyTestThreadFocusWindow() && TestThreadFocusWindow() == nullptr &&
            !::IsWindow(focus_window) && DestroyTestThreadFocusWindow(),
        "balance: owner explicitly destroys the reused HWND exactly once", "");
  for (int cycle = 0; cycle < 12; ++cycle) {
    Check(EnsureTestThreadFocus(&focus_error) && DestroyTestThreadFocusWindow(),
          "balance: helper window can be recreated and destroyed on its STA", focus_error);
  }
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
  evidence.policy = ThreadFocusPolicy::kStrictTsf;
  evidence.thread_focus_query_ok = true;
  evidence.thread_focus = true;
  evidence.thread_focus_query_hr = S_OK;

  TargetRevalidation result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kNoDocumentManager,
        "target-evidence: no document manager",
        DescribeTargetRevalidationStatus(result.status));
  Check(result.failure_stage == TargetFailureStage::kNone &&
            result.query_hr == S_OK,
        "target-evidence: a successful empty GetFocus is not a query failure",
        DescribeTargetFailureStage(result.failure_stage));

  evidence.has_document_manager = true;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kNoTopContext,
        "target-evidence: no top context",
        DescribeTargetRevalidationStatus(result.status));

  evidence.has_top_context = true;
  evidence.registry_query_ok = true;
  evidence.registry_status = ContextRegistryStatus::kOk;
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
  Check(result.status == TargetRevalidationStatus::kThreadFocusUnconfirmed,
        "target-evidence: strict TSF never silently uses the Win32 fallback",
        DescribeTargetRevalidationStatus(result.status));

  evidence.policy = ThreadFocusPolicy::kLabWin32Fallback;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kOk,
        "target-evidence: the explicit lab policy accepts real Win32 thread focus",
        DescribeTargetRevalidationStatus(result.status));
  Check(result.thread_focus_evidence ==
            ThreadFocusEvidence::kWin32ThreadFocus,
        "target-evidence: the Win32 source is recorded",
        DescribeThreadFocusEvidence(result.thread_focus_evidence));

  evidence.thread_focus_notification_set = true;
  result = EvaluateTargetEvidence(evidence);
  Check(result.thread_focus_evidence == ThreadFocusEvidence::kWin32ThreadFocus,
        "target-evidence: a stale notification does not relabel the lab query", "");
  evidence.thread_focus = true;
  result = EvaluateTargetEvidence(evidence);
  Check(result.thread_focus_evidence == ThreadFocusEvidence::kNotificationSet,
        "target-evidence: a set notification with a matching live query keeps its source",
        DescribeThreadFocusEvidence(result.thread_focus_evidence));
  return Finish();
}

// P3.13-R2 pure table for query-failure classification and the explicit fallback policy.
// These are synthetic facts: no TSF call and no notification is made here.
int RunTargetPolicyPure() {
  auto good = [] {
    TargetEvidence evidence;
    evidence.policy = ThreadFocusPolicy::kStrictTsf;
    evidence.thread_focus_query_ok = true;
    evidence.thread_focus = true;
    evidence.thread_focus_query_hr = S_OK;
    evidence.has_document_manager = true;
    evidence.get_focus_hr = S_OK;
    evidence.has_top_context = true;
    evidence.get_top_hr = S_OK;
    evidence.top_context_matches = true;
    evidence.registry_query_ok = true;
    evidence.registry_status = ContextRegistryStatus::kOk;
    evidence.registry_identity_matches = true;
    evidence.registry_generation_matches = true;
    return evidence;
  };
  auto current = [](const TargetRevalidation& result) {
    return result.status == TargetRevalidationStatus::kOk &&
           result.failure_stage == TargetFailureStage::kNone &&
           result.query_hr == S_OK;
  };

  TargetEvidence evidence = good();
  TargetRevalidation result = EvaluateTargetEvidence(evidence);
  Check(current(result) &&
            result.thread_focus_evidence == ThreadFocusEvidence::kQuerySet,
        "target-policy: strict TSF accepts a successful TRUE query",
        DescribeThreadFocusEvidence(result.thread_focus_evidence));
  Check(std::string(DescribeThreadFocusPolicy(evidence.policy)) == "strict-tsf",
        "target-policy: the strict policy has its own source label", "");

  // IsThreadFocus itself fails. The BOOL is invalid and the real HRESULT is preserved.
  evidence = good();
  evidence.thread_focus_query_ok = false;
  evidence.thread_focus = false;
  evidence.thread_focus_query_hr = E_ACCESSDENIED;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kQueryFailed &&
            result.failure_stage == TargetFailureStage::kThreadFocusQuery &&
            result.query_hr == E_ACCESSDENIED,
        "target-policy: a failed IsThreadFocus is a query failure at its real stage",
        DescribeTargetFailureStage(result.failure_stage));
  Check(!result.evidence.thread_focus_query_ok &&
            result.thread_focus_evidence == ThreadFocusEvidence::kNone,
        "target-policy: a failed focus query has no valid BOOL or source", "");

  // The same failure cannot be rescued by a Win32 window, even in the lab policy.
  evidence.policy = ThreadFocusPolicy::kLabWin32Fallback;
  evidence.win32_thread_focus = true;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kQueryFailed &&
            result.failure_stage == TargetFailureStage::kThreadFocusQuery &&
            result.query_hr == E_ACCESSDENIED,
        "target-policy: the lab fallback never auto-allows a failed query",
        DescribeTargetRevalidationStatus(result.status));

  // The query succeeded and answered FALSE. Strict mode refuses; the lab policy may accept
  // a real Win32 window, and only there.
  evidence = good();
  evidence.thread_focus = false;
  evidence.thread_focus_query_hr = S_FALSE;
  evidence.win32_thread_focus = true;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kThreadFocusUnconfirmed &&
            result.failure_stage == TargetFailureStage::kNone &&
            result.query_hr == S_OK &&
            result.thread_focus_evidence == ThreadFocusEvidence::kNone,
        "target-policy: strict TSF refuses a successful FALSE BOOL without fallback",
        DescribeTargetRevalidationStatus(result.status));
  Check(result.evidence.thread_focus_query_ok &&
            !result.evidence.thread_focus,
        "target-policy: the valid FALSE BOOL is kept in the evidence", "");

  evidence.policy = ThreadFocusPolicy::kLabWin32Fallback;
  evidence.win32_thread_focus = false;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kThreadFocusUnconfirmed,
        "target-policy: lab fallback without a Win32 window still refuses",
        DescribeTargetRevalidationStatus(result.status));

  evidence.win32_thread_focus = true;
  result = EvaluateTargetEvidence(evidence);
  Check(current(result) &&
            result.thread_focus_evidence == ThreadFocusEvidence::kWin32ThreadFocus,
        "target-policy: lab fallback accepts the labelled Win32 revalidation",
        DescribeThreadFocusEvidence(result.thread_focus_evidence));
  Check(std::string(DescribeThreadFocusPolicy(evidence.policy)) ==
            "lab-win32-fallback",
        "target-policy: the fallback source label is not renamed", "");

  // A failed GetFocus is an API failure, not an empty target.
  evidence = good();
  evidence.has_document_manager = false;
  evidence.get_focus_hr = E_FAIL;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kQueryFailed &&
            result.failure_stage == TargetFailureStage::kGetFocus &&
            result.query_hr == E_FAIL,
        "target-policy: a failed GetFocus is a query failure",
        DescribeTargetFailureStage(result.failure_stage));

  // A successful GetFocus with no manager is a normal empty state.
  evidence.get_focus_hr = S_OK;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kNoDocumentManager &&
            result.failure_stage == TargetFailureStage::kNone &&
            result.query_hr == S_OK,
        "target-policy: GetFocus success with no manager is not an error",
        DescribeTargetRevalidationStatus(result.status));

  // A failed GetTop is a query failure; success with no top is an empty state.
  evidence = good();
  evidence.has_top_context = false;
  evidence.get_top_hr = HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kQueryFailed &&
            result.failure_stage == TargetFailureStage::kGetTop &&
            result.query_hr == HRESULT_FROM_WIN32(ERROR_INVALID_STATE),
        "target-policy: a failed GetTop keeps its real HRESULT",
        DescribeTargetFailureStage(result.failure_stage));
  evidence.get_top_hr = S_OK;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kNoTopContext &&
            result.failure_stage == TargetFailureStage::kNone &&
            result.query_hr == S_OK,
        "target-policy: GetTop success with no top is not an error",
        DescribeTargetRevalidationStatus(result.status));

  // The registry answered with a non-Ok status: identity was never established.
  evidence = good();
  evidence.registry_query_ok = false;
  evidence.registry_status = ContextRegistryStatus::kCapacityExceeded;
  evidence.registry_identity_matches = false;
  evidence.registry_generation_matches = false;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kQueryFailed &&
            result.failure_stage == TargetFailureStage::kRegistry &&
            result.query_hr == E_FAIL,
        "target-policy: a registry refusal is a query failure, not a mismatch",
        DescribeTargetFailureStage(result.failure_stage));
  Check(result.evidence.registry_status ==
            ContextRegistryStatus::kCapacityExceeded,
        "target-policy: the registry status is preserved", "");

  evidence = good();
  evidence.registry_generation_matches = false;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kGenerationMismatch &&
            result.failure_stage == TargetFailureStage::kNone &&
            result.query_hr == S_OK,
        "target-policy: a real generation mismatch is not a query failure",
        DescribeTargetRevalidationStatus(result.status));

  evidence = good();
  evidence.top_context_matches = false;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kContextMismatch &&
            result.failure_stage == TargetFailureStage::kNone &&
            result.query_hr == S_OK,
        "target-policy: a real top-context mismatch is not a query failure",
        DescribeTargetRevalidationStatus(result.status));

  // A kill notification is decisive even when the live query failed; the query failure
  // stays visible in evidence.
  evidence = good();
  evidence.thread_focus_query_ok = false;
  evidence.thread_focus = false;
  evidence.thread_focus_query_hr = E_ACCESSDENIED;
  evidence.thread_focus_notification_kill = true;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kThreadFocusLost &&
            result.thread_focus_evidence == ThreadFocusEvidence::kNotificationKill &&
            result.failure_stage == TargetFailureStage::kNone,
        "target-policy: a kill notification decides focus loss",
        DescribeTargetRevalidationStatus(result.status));
  Check(result.evidence.thread_focus_query_hr == E_ACCESSDENIED,
        "target-policy: the failed query is preserved under a kill", "");

  evidence = good();
  evidence.thread_focus_query_ok = false;
  evidence.thread_focus = false;
  evidence.thread_focus_query_hr = E_ACCESSDENIED;
  evidence.thread_focus_notification_set = true;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kQueryFailed &&
            result.failure_stage == TargetFailureStage::kThreadFocusQuery &&
            result.query_hr == E_ACCESSDENIED &&
            result.thread_focus_evidence == ThreadFocusEvidence::kNone,
        "target-policy: a cached set notification cannot override a failed live query",
        DescribeTargetRevalidationStatus(result.status));
  evidence.thread_focus_query_ok = true;
  evidence.thread_focus_query_hr = S_FALSE;
  result = EvaluateTargetEvidence(evidence);
  Check(result.status == TargetRevalidationStatus::kThreadFocusUnconfirmed &&
            result.thread_focus_evidence == ThreadFocusEvidence::kNone,
        "target-policy: a cached set notification cannot override a live FALSE",
        DescribeTargetRevalidationStatus(result.status));
  return Finish();
}

int RunObserverCase(const std::string& case_name) {
  if (case_name == "environment-failure") {
    Check(false, "intentional failed assertion before environmental classification", "");
    return FinishEnvironmentLimited();
  }
  if (case_name == "registry-identity") return RunRegistryIdentity();
  if (case_name == "initial-binding") return RunInitialBinding();
  if (case_name == "push-pop-top") return RunPushPopTop();
  if (case_name == "external-selection") return RunExternalSelection();
  if (case_name == "external-text") return RunExternalText();
  if (case_name == "noncollapsed-selection") return RunNonCollapsedSelection();
  if (case_name == "document-uninit") return RunDocumentUninit();
  if (case_name == "own-edit") return RunOwnEdit();
  if (case_name == "real-focus") return RunRealFocus();
  if (case_name == "association-contract") return RunAssociationContract();
  if (case_name == "revoked-target-guard") return RunRevokedTargetGuard();
  if (case_name == "shutdown-late-read") return RunShutdownLateRead();
  if (case_name == "reentrant-shutdown") return RunReentrantShutdown();
  if (case_name == "thread-ownership") return RunThreadOwnership();
  if (case_name == "target-evidence") return RunTargetEvidencePure();
  if (case_name == "target-policy") return RunTargetPolicyPure();
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
  ContextObserver observer(registry, host->client_id(),
                           ThreadFocusPolicy::kStrictTsf);
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
