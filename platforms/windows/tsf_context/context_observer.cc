// EpoCanva P3.10 -- implementation of the STA context observer. The contract (which
// notifications are used, where binding happens, own-vs-external classification) is in
// context_observer.h; this file follows it. Everything here runs on one STA thread.
#include "context_observer.h"

#include <atomic>
#include <cstdio>
#include <utility>
#include <vector>

namespace epocanva {
namespace tsf {
namespace context {

const char* DescribeContextDropReason(ContextDropReason reason) {
  switch (reason) {
    case ContextDropReason::kNone:
      return "None";
    case ContextDropReason::kNoFocusedDocument:
      return "NoFocusedDocument";
    case ContextDropReason::kNoTopContext:
      return "NoTopContext";
    case ContextDropReason::kDocumentManagerUninitialized:
      return "DocumentManagerUninitialized";
    case ContextDropReason::kFocusMoved:
      return "FocusMoved";
    case ContextDropReason::kTopContextChanged:
      return "TopContextChanged";
    case ContextDropReason::kExternalTextEdit:
      return "ExternalTextEdit";
    case ContextDropReason::kExternalSelectionEdit:
      return "ExternalSelectionEdit";
    case ContextDropReason::kSelectionNonCollapsed:
      return "SelectionNonCollapsed";
    case ContextDropReason::kSelectionUnsupported:
      return "SelectionUnsupported";
    case ContextDropReason::kSelectionUnavailable:
      return "SelectionUnavailable";
    case ContextDropReason::kEditSinkUnavailable:
      return "EditSinkUnavailable";
    case ContextDropReason::kRegistryCapacity:
      return "RegistryCapacity";
    case ContextDropReason::kUncertainBlocked:
      return "UncertainBlocked";
    case ContextDropReason::kThreadFocusLost:
      return "ThreadFocusLost";
    case ContextDropReason::kThreadFocusChanged:
      return "ThreadFocusChanged";
    case ContextDropReason::kClosed:
      return "Closed";
  }
  return "UnknownDropReason";
}

const char* DescribeTargetBindingEvidence(TargetBindingEvidence evidence) {
  switch (evidence) {
    case TargetBindingEvidence::kNone:
      return "none";
    case TargetBindingEvidence::kInitialAdoption:
      return "initial-adoption";
    case TargetBindingEvidence::kDocumentFocusNotification:
      return "notification";
    case TargetBindingEvidence::kTopContextNotification:
      return "top-context-notification";
    case TargetBindingEvidence::kThreadFocusNotification:
      return "thread-focus-notification";
    case TargetBindingEvidence::kRevalidation:
      return "revalidation";
  }
  return "unknown-bind-evidence";
}

namespace {

std::string HrText(HRESULT hr) {
  char buffer[24] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08lX",
                static_cast<unsigned long>(hr));
  return buffer;
}

bool SameCanonicalContext(ITfContext* left, ITfContext* right) {
  if (left == nullptr || right == nullptr) {
    return false;
  }
  if (left == right) {
    return true;
  }
  IUnknown* left_unknown = nullptr;
  IUnknown* right_unknown = nullptr;
  const HRESULT left_hr =
      left->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&left_unknown));
  const HRESULT right_hr =
      right->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&right_unknown));
  const bool same = SUCCEEDED(left_hr) && SUCCEEDED(right_hr) &&
                    left_unknown != nullptr && left_unknown == right_unknown;
  if (left_unknown != nullptr) {
    left_unknown->Release();
  }
  if (right_unknown != nullptr) {
    right_unknown->Release();
  }
  return same;
}

bool SameDocumentManager(ITfDocumentMgr* left, ITfDocumentMgr* right) {
  if (left == nullptr || right == nullptr) {
    return false;
  }
  if (left == right) {
    return true;
  }
  IUnknown* left_unknown = nullptr;
  IUnknown* right_unknown = nullptr;
  const HRESULT left_hr =
      left->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&left_unknown));
  const HRESULT right_hr =
      right->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&right_unknown));
  const bool same = SUCCEEDED(left_hr) && SUCCEEDED(right_hr) &&
                    left_unknown != nullptr && left_unknown == right_unknown;
  if (left_unknown != nullptr) {
    left_unknown->Release();
  }
  if (right_unknown != nullptr) {
    right_unknown->Release();
  }
  return same;
}

ContextDropReason ReasonForSelection(SelectionReadStatus status) {
  switch (status) {
    case SelectionReadStatus::kNonCollapsed:
      return ContextDropReason::kSelectionNonCollapsed;
    case SelectionReadStatus::kUnsupportedShape:
      return ContextDropReason::kSelectionUnsupported;
    case SelectionReadStatus::kNoSelection:
    case SelectionReadStatus::kSessionUnavailable:
    case SelectionReadStatus::kContextUnavailable:
    case SelectionReadStatus::kClosed:
    case SelectionReadStatus::kOk:
      break;
  }
  return ContextDropReason::kSelectionUnavailable;
}

}  // namespace

// --- shared state -------------------------------------------------------------------------

struct ContextObserver::State {
  State(ContextRegistry& registry, TfClientId own_client_id,
        ThreadFocusPolicy thread_focus_policy, TextOwnershipPolicy text_ownership)
      : registry(&registry),
        own_client_id(own_client_id),
        policy(thread_focus_policy),
        text_ownership(text_ownership),
        reader(new SelectionReader(own_client_id)) {}

  ContextRegistry* registry = nullptr;
  TfClientId own_client_id = 0;
  // Explicit construction-time policy. StrictTsf never probes or accepts a Win32 window.
  ThreadFocusPolicy policy = ThreadFocusPolicy::kStrictTsf;
  TextOwnershipPolicy text_ownership = TextOwnershipPolicy::kCompositionOwner;
  std::unique_ptr<SelectionReader> reader;

  ContextObserverSink* sink = nullptr;
  // Borrowed, owned by ContextObserver; cleared before the sink object is released.
  IUnknown* sink_unknown = nullptr;

  bool subscribed = false;
  bool closed = false;

  ITfThreadMgr* thread_mgr = nullptr;  // held while subscribed
  ITfSource* thread_source = nullptr;  // held while advised
  DWORD thread_cookie = TF_INVALID_COOKIE;
  // P3.13: the separate ITfThreadFocusSink advice on the same ITfSource. The two cookies
  // are unadvised independently and the source is released once.
  DWORD thread_focus_cookie = TF_INVALID_COOKIE;
  bool thread_focus_sink_advise_failed = false;
  enum class ThreadFocusNotice : std::uint8_t { kNone = 0, kSet, kKill };
  ThreadFocusNotice thread_focus_notice = ThreadFocusNotice::kNone;
  bool thread_focus_query_ok = false;
  bool thread_focus_query_value = false;
  HRESULT thread_focus_query_hr = S_OK;
  bool win32_thread_focus = false;
  // The evidence of the most recent publication; consumed by PublishTarget.
  TargetBindingEvidence bind_evidence = TargetBindingEvidence::kNone;
  TargetBindingEvidence last_bind_evidence = TargetBindingEvidence::kNone;

  ITfDocumentMgr* focused_dm = nullptr;  // held while it is the focused document
  ITfContext* top_context = nullptr;      // held while it is the top context
  std::uint64_t context_id = 0;
  std::uint64_t generation = 0;

  ITfSource* text_source = nullptr;  // held while the context's sink is advised
  DWORD text_cookie = TF_INVALID_COOKIE;

  bool target_published = false;
  LONG published_caret_utf16 = 0;
  bool need_bind = false;
  bool selection_read_active = false;
  // Set when a Push/Pop notification may have arrived before the stack reflected it. Pump
  // re-reads GetTop once the notification has returned.
  bool top_recheck = false;

  ContextObserverSnapshot counters;

  // --- reference accounting --------------------------------------------------------------
  void AdoptFocusedDocumentManager(ITfDocumentMgr* borrowed) {
    if (SameDocumentManager(focused_dm, borrowed)) {
      return;
    }
    ReleaseFocusedDocumentManager();
    if (borrowed != nullptr) {
      borrowed->AddRef();
      focused_dm = borrowed;
      ++counters.document_mgr_addrefs;
    }
  }

  void ReleaseFocusedDocumentManager() {
    if (focused_dm != nullptr) {
      focused_dm->Release();
      focused_dm = nullptr;
      ++counters.document_mgr_releases;
    }
  }

  void ReleaseTopContext() {
    if (top_context != nullptr) {
      top_context->Release();
      top_context = nullptr;
      ++counters.context_releases;
    }
    context_id = 0;
    generation = 0;
  }

  void ResetSelectionRead() {
    reader->Abandon();
    selection_read_active = false;
  }

  void ReleaseTextSink() {
    if (text_source != nullptr) {
      if (text_cookie != TF_INVALID_COOKIE) {
        text_source->UnadviseSink(text_cookie);
        ++counters.sink_unadvises;
        text_cookie = TF_INVALID_COOKIE;
      }
      text_source->Release();
      text_source = nullptr;
    }
  }

  bool AdviseTextSink(ITfContext* context) {
    if (context == nullptr || sink_unknown == nullptr) {
      ++counters.sink_advise_failures;
      return false;
    }
    ITfSource* source = nullptr;
    const HRESULT qi_hr =
        context->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source));
    if (FAILED(qi_hr) || source == nullptr) {
      ++counters.sink_advise_failures;
      return false;
    }
    DWORD cookie = TF_INVALID_COOKIE;
    const HRESULT advise_hr =
        source->AdviseSink(IID_ITfTextEditSink, sink_unknown, &cookie);
    if (FAILED(advise_hr) || cookie == TF_INVALID_COOKIE) {
      source->Release();
      ++counters.sink_advise_failures;
      return false;
    }
    text_source = source;
    text_cookie = cookie;
    ++counters.sink_advises;
    return true;
  }

  // --- P3.13 thread focus and target revalidation ---------------------------------------
  void SetBindEvidenceIfNone(TargetBindingEvidence evidence) {
    if (bind_evidence == TargetBindingEvidence::kNone) {
      bind_evidence = evidence;
    }
  }

  // Reads the real thread-focus facts every time a decision needs them. IsThreadFocus is
  // the TSF statement. A Win32 focus window is probed only under the explicitly requested
  // lab fallback policy; strict mode neither reads nor accepts it. Neither source is
  // inferred from GetFocus.
  void ReadThreadFocusFacts() {
    thread_focus_query_ok = false;
    thread_focus_query_value = false;
    thread_focus_query_hr = E_UNEXPECTED;
    ++counters.thread_focus_queries;
    if (thread_mgr != nullptr) {
      BOOL value = FALSE;
      const HRESULT hr = thread_mgr->IsThreadFocus(&value);
      thread_focus_query_hr = hr;
      if (SUCCEEDED(hr)) {
        thread_focus_query_ok = true;
        thread_focus_query_value = value != FALSE;
      } else {
        ++counters.thread_focus_query_failures;
      }
    } else {
      ++counters.thread_focus_query_failures;
    }

    win32_thread_focus = false;
    if (policy == ThreadFocusPolicy::kLabWin32Fallback) {
      ++counters.win32_thread_focus_checks;
      GUITHREADINFO info = {};
      info.cbSize = sizeof(info);
      if (::GetGUIThreadInfo(::GetCurrentThreadId(), &info) != FALSE &&
          info.hwndFocus != nullptr) {
        DWORD process_id = 0;
        const DWORD thread_id =
            ::GetWindowThreadProcessId(info.hwndFocus, &process_id);
        win32_thread_focus = thread_id == ::GetCurrentThreadId();
      }
    }

    if (thread_focus_notice == ThreadFocusNotice::kSet) {
      counters.last_thread_focus_evidence = ThreadFocusEvidence::kNotificationSet;
    } else if (thread_focus_query_ok && thread_focus_query_value) {
      counters.last_thread_focus_evidence = ThreadFocusEvidence::kQuerySet;
    } else if (win32_thread_focus) {
      counters.last_thread_focus_evidence = ThreadFocusEvidence::kWin32ThreadFocus;
    } else if (thread_focus_notice == ThreadFocusNotice::kKill) {
      counters.last_thread_focus_evidence = ThreadFocusEvidence::kNotificationKill;
    } else {
      counters.last_thread_focus_evidence = ThreadFocusEvidence::kNone;
    }
  }

  // The full live target check. Callers: the coordinator before input/plan submission and
  // the P3.7 edit callback through a TargetWriteGuard. It never writes and never pumps.
  TargetRevalidation RevalidateTarget(ITfContext* expected,
                                      std::uint64_t expected_context_id,
                                      std::uint64_t expected_generation) {
    TargetRevalidation result;
    if (closed || !subscribed || thread_mgr == nullptr || expected == nullptr ||
        expected_context_id == 0 || expected_generation == 0) {
      result.status = closed ? TargetRevalidationStatus::kClosed
                             : TargetRevalidationStatus::kQueryFailed;
      result.failure_stage = closed ? TargetFailureStage::kNone
                                    : TargetFailureStage::kInvalidRequest;
      result.query_hr = closed ? S_OK : E_INVALIDARG;
      ++counters.target_revalidations;
      ++counters.target_revalidation_failures;
      counters.last_target_revalidation = result;
      return result;
    }

    ReadThreadFocusFacts();
    TargetEvidence evidence;
    evidence.policy = policy;
    evidence.thread_focus_query_ok = thread_focus_query_ok;
    evidence.thread_focus = thread_focus_query_value;
    evidence.thread_focus_query_hr = thread_focus_query_hr;
    evidence.win32_thread_focus = win32_thread_focus;
    evidence.thread_focus_notification_set =
        thread_focus_notice == ThreadFocusNotice::kSet;
    evidence.thread_focus_notification_kill =
        thread_focus_notice == ThreadFocusNotice::kKill;

    ITfDocumentMgr* focus = nullptr;
    evidence.get_focus_hr = thread_mgr->GetFocus(&focus);
    if (SUCCEEDED(evidence.get_focus_hr) && focus != nullptr) {
      evidence.has_document_manager = true;
      ITfContext* top = nullptr;
      evidence.get_top_hr = focus->GetTop(&top);
      if (SUCCEEDED(evidence.get_top_hr) && top != nullptr) {
        evidence.has_top_context = true;
        evidence.top_context_matches = TargetContextsEqual(top, expected);
      }
      if (top != nullptr) {
        top->Release();
      }
    }
    if (focus != nullptr) {
      focus->Release();
    }

    if (registry != nullptr) {
      ContextIdRecord record;
      evidence.registry_status = registry->Observe(expected, &record);
      evidence.registry_query_ok =
          evidence.registry_status == ContextRegistryStatus::kOk;
      if (evidence.registry_query_ok) {
        evidence.observed_context_id = record.context_id;
        evidence.observed_generation = record.generation;
        evidence.registry_identity_matches =
            record.context_id == expected_context_id;
        evidence.registry_generation_matches =
            record.generation == expected_generation;
      }
    }

    result = EvaluateTargetEvidence(evidence);
    if (result.ok() &&
        result.thread_focus_evidence == ThreadFocusEvidence::kWin32ThreadFocus) {
      ++counters.win32_thread_focus_accepts;
    }
    ++counters.target_revalidations;
    if (!result.ok()) {
      ++counters.target_revalidation_failures;
    }
    counters.last_target_revalidation = result;
    counters.last_thread_focus_evidence = result.thread_focus_evidence;
    return result;
  }

  // --- target publication ----------------------------------------------------------------
  void DropPublished(ContextDropReason reason) {
    if (!target_published) {
      counters.last_refusal_reason = reason;
      return;
    }
    target_published = false;
    ++counters.target_drops;
    counters.last_target_lost_reason = reason;
    if (sink != nullptr) {
      sink->OnTargetLost(reason);
    }
  }

  bool StillTopOfFocusedDocument(ITfContext* context) const {
    if (focused_dm == nullptr || context == nullptr) {
      return false;
    }
    ITfContext* top = nullptr;
    if (FAILED(focused_dm->GetTop(&top)) || top == nullptr) {
      return false;
    }
    const bool same = SameCanonicalContext(top, context);
    top->Release();
    return same;
  }

  // Switches the held top context to `context` (borrowed; AddRef'd here), restarts the
  // selection read schedule and drops the old published target. Returns false when the
  // same context is already held.
  bool SwitchTopContext(ITfContext* context, const ContextIdRecord& record,
                        ContextDropReason reason) {
    if (context == nullptr) {
      return false;
    }
    if (top_context != nullptr && SameCanonicalContext(top_context, context)) {
      return false;
    }
    ResetSelectionRead();
    ReleaseTextSink();
    ReleaseTopContext();
    context->AddRef();
    top_context = context;
    ++counters.context_addrefs;
    context_id = record.context_id;
    generation = 0;
    if (!AdviseTextSink(top_context)) {
      DropPublished(ContextDropReason::kEditSinkUnavailable);
      need_bind = false;
      bind_evidence = TargetBindingEvidence::kNone;
      return true;
    }
    DropPublished(reason);
    need_bind = true;
    return true;
  }

  // Re-reads the focused document's top context. A changed top context drops the published
  // target; no selection read and no bind happens here (Pump does that outside any
  // notification).
  void AdoptCurrentContext(ContextDropReason reason) {
    if (closed || !subscribed) {
      return;
    }
    if (focused_dm == nullptr) {
      ResetSelectionRead();
      ReleaseTextSink();
      ReleaseTopContext();
      DropPublished(ContextDropReason::kNoFocusedDocument);
      need_bind = false;
      bind_evidence = TargetBindingEvidence::kNone;
      return;
    }
    ITfContext* top = nullptr;
    const HRESULT top_hr = focused_dm->GetTop(&top);
    if (FAILED(top_hr) || top == nullptr) {
      ResetSelectionRead();
      ReleaseTextSink();
      ReleaseTopContext();
      DropPublished(ContextDropReason::kNoTopContext);
      need_bind = false;
      bind_evidence = TargetBindingEvidence::kNone;
      return;
    }
    ContextIdRecord record;
    const ContextRegistryStatus status = registry->Observe(top, &record);
    if (status != ContextRegistryStatus::kOk) {
      top->Release();
      ResetSelectionRead();
      ReleaseTextSink();
      ReleaseTopContext();
      DropPublished(ContextDropReason::kRegistryCapacity);
      need_bind = false;
      bind_evidence = TargetBindingEvidence::kNone;
      return;
    }
    // The GetTop reference is transferred only for the comparison call; SwitchTopContext
    // takes its own reference when it adopts the context.
    SwitchTopContext(top, record, reason);
    top->Release();
  }

  void PublishTarget(LONG caret_utf16) {
    if (sink == nullptr || top_context == nullptr) {
      // No listener to publish to; nothing is bound. SetSink() does not replay the target.
      need_bind = false;
      bind_evidence = TargetBindingEvidence::kNone;
      return;
    }
    ContextIdRecord record;
    const ContextRegistryStatus status = registry->BeginBinding(top_context, &record);
    if (status != ContextRegistryStatus::kOk) {
      DropPublished(ContextDropReason::kRegistryCapacity);
      need_bind = false;
      bind_evidence = TargetBindingEvidence::kNone;
      return;
    }
    if (record.uncertain_blocked) {
      DropPublished(ContextDropReason::kUncertainBlocked);
      need_bind = false;
      bind_evidence = TargetBindingEvidence::kNone;
      return;
    }
    context_id = record.context_id;
    generation = record.generation;
    published_caret_utf16 = caret_utf16;
    target_published = true;
    ++counters.target_binds;
    if (bind_evidence == TargetBindingEvidence::kNone) {
      bind_evidence = TargetBindingEvidence::kRevalidation;
    }
    last_bind_evidence = bind_evidence;
    sink->OnTargetBound(
        top_context, ObservedTarget{context_id, generation, caret_utf16, bind_evidence});
    bind_evidence = TargetBindingEvidence::kNone;
    need_bind = false;
  }

  // --- notification handlers --------------------------------------------------------------
  void HandleSetFocus(ITfDocumentMgr* focus) {
    if (closed) {
      ++counters.late_notifications_after_close;
      return;
    }
    ++counters.focus_notifications;
    if (focus == nullptr) {
      ++counters.focus_loss_notifications;
      ResetSelectionRead();
      ReleaseTextSink();
      ReleaseTopContext();
      ReleaseFocusedDocumentManager();
      DropPublished(ContextDropReason::kNoFocusedDocument);
      need_bind = false;
      bind_evidence = TargetBindingEvidence::kNone;
      return;
    }
    AdoptFocusedDocumentManager(focus);
    SetBindEvidenceIfNone(TargetBindingEvidence::kDocumentFocusNotification);
    AdoptCurrentContext(ContextDropReason::kFocusMoved);
  }

  void HandlePushPop(ITfContext* context, bool push) {
    if (closed) {
      ++counters.late_notifications_after_close;
      return;
    }
    if (push) {
      ++counters.push_notifications;
    } else {
      ++counters.pop_notifications;
    }
    if (context == nullptr) {
      return;
    }
    // A context belongs to exactly one document manager. Only events for the focused
    // document's stack may influence the current binding; background documents are
    // ignored wholesale.
    ITfDocumentMgr* owner = nullptr;
    const HRESULT owner_hr = context->GetDocumentMgr(&owner);
    if (FAILED(owner_hr) || owner == nullptr) {
      return;
    }
    const bool ours = focused_dm != nullptr &&
                      SameDocumentManager(owner, focused_dm);
    owner->Release();
    if (!ours) {
      ++counters.background_events_ignored;
      return;
    }
    if (push) {
      ContextIdRecord record;
      const ContextRegistryStatus status = registry->Observe(context, &record);
      if (status != ContextRegistryStatus::kOk) {
        DropPublished(ContextDropReason::kRegistryCapacity);
        need_bind = false;
        bind_evidence = TargetBindingEvidence::kNone;
        return;
      }
      SetBindEvidenceIfNone(TargetBindingEvidence::kTopContextNotification);
      SwitchTopContext(context, record, ContextDropReason::kTopContextChanged);
      return;
    }
    // A Pop removes the top context. msctf may deliver OnPopContext before the stack
    // reflects the removal, so the event itself is used for the immediate invalidation and
    // Pump re-reads GetTop once the notification has returned.
    if (top_context != nullptr && SameCanonicalContext(top_context, context)) {
      DropPublished(ContextDropReason::kTopContextChanged);
      need_bind = true;
    }
    SetBindEvidenceIfNone(TargetBindingEvidence::kTopContextNotification);
    top_recheck = true;
  }

  // P3.13. OnSetThreadFocus never fabricates a document target: it only notes that the
  // thread has input focus again and asks Pump to re-read the real GetFocus/GetTop. On
  // kill, the published target is dropped (the old input path must stop immediately) and
  // the new binding is impossible until focus returns; the coordinator's retirement path
  // may still finish a composition it owns, which is legal on an unfocused context.
  void HandleThreadFocus(bool set) {
    if (closed) {
      ++counters.late_notifications_after_close;
      return;
    }
    if (set) {
      ++counters.thread_focus_set_notifications;
      thread_focus_notice = ThreadFocusNotice::kSet;
      // Re-read the document focus; while the thread was unfocused the document stack may
      // have changed without a usable notification.
      top_recheck = true;
      if (target_published) {
        // The previous binding's focus facts are no longer known to be current. Force a
        // fresh generation instead of reusing the old one.
        DropPublished(ContextDropReason::kThreadFocusChanged);
      }
      need_bind = true;
      SetBindEvidenceIfNone(TargetBindingEvidence::kThreadFocusNotification);
      return;
    }

    ++counters.thread_focus_kill_notifications;
    thread_focus_notice = ThreadFocusNotice::kKill;
    ResetSelectionRead();
    ReleaseTextSink();
    ReleaseTopContext();
    ReleaseFocusedDocumentManager();
    DropPublished(ContextDropReason::kThreadFocusLost);
    need_bind = false;
    bind_evidence = TargetBindingEvidence::kNone;
  }

  void HandleUninitDocumentManager(ITfDocumentMgr* document_manager) {
    if (closed) {
      ++counters.late_notifications_after_close;
      return;
    }
    ++counters.uninit_notifications;
    if (document_manager == nullptr ||
        !SameDocumentManager(document_manager, focused_dm)) {
      return;
    }
    ResetSelectionRead();
    ReleaseTextSink();
    if (top_context != nullptr) {
      // A non-uncertain entry is dropped; an uncertain one is deliberately kept.
      registry->Forget(top_context);
    }
    ReleaseTopContext();
    ReleaseFocusedDocumentManager();
    top_recheck = false;
    DropPublished(ContextDropReason::kDocumentManagerUninitialized);
    need_bind = false;
  }

  void HandleEndEdit(ITfContext* context, TfEditCookie read_only_cookie,
                     ITfEditRecord* record) {
    if (closed) {
      ++counters.late_notifications_after_close;
      return;
    }
    if (context == nullptr) {
      return;
    }
    if (top_context == nullptr || !SameCanonicalContext(context, top_context)) {
      ++counters.edits_ignored_not_current;
      return;
    }
    if (!StillTopOfFocusedDocument(context)) {
      // A focus change was missed; treat this as a target change, not as an edit of the
      // current binding.
      ++counters.edits_ignored_not_current;
      AdoptCurrentContext(ContextDropReason::kFocusMoved);
      return;
    }
    BOOL in_write = FALSE;
    const HRESULT in_write_hr = context->InWriteSession(own_client_id, &in_write);
    const bool own = SUCCEEDED(in_write_hr) && in_write != FALSE;

    bool text_changed = false;
    bool selection_changed = false;
    bool record_read_failed = (record == nullptr);
    if (record != nullptr) {
      BOOL changed = FALSE;
      if (FAILED(record->GetSelectionStatus(&changed))) {
        record_read_failed = true;
      } else {
        selection_changed = changed != FALSE;
      }
      IEnumTfRanges* ranges = nullptr;
      const HRESULT text_hr =
          record->GetTextAndPropertyUpdates(TF_GTP_INCL_TEXT, nullptr, 0, &ranges);
      if (FAILED(text_hr)) {
        record_read_failed = true;
      } else if (ranges != nullptr) {
        ITfRange* range = nullptr;
        ULONG fetched = 0;
        const HRESULT next_hr = ranges->Next(1, &range, &fetched);
        if (next_hr == S_OK && fetched == 1) {
          text_changed = true;
        }
        if (range != nullptr) {
          range->Release();
        }
        ranges->Release();
      }
    }
    if (text_changed) {
      ++counters.text_changes;
    }
    if (selection_changed) {
      ++counters.selection_changes;
    }
    if (!text_changed && !selection_changed && !record_read_failed) {
      ++counters.edits_without_changes;
      return;
    }
    const SelectionSnapshot selection =
        ReadSelectionWithCookie(context, read_only_cookie);

    if (own) {
      ++counters.own_edits;
      if (!target_published) {
        ++counters.own_edits_without_target;
        return;
      }
      if (selection.ok()) {
        if (sink != nullptr) {
          sink->OnOwnEditCompleted(
              context, ObservedTarget{context_id, generation, published_caret_utf16},
              selection);
        }
        return;
      }
      // Our own write completed but the resulting selection is not a reliable insertion
      // point. Admission closes; a fresh read decides whether it can reopen.
      ResetSelectionRead();
      DropPublished(ReasonForSelection(selection.status));
      need_bind = true;
      return;
    }

    ++counters.external_edits;
    if (text_ownership == TextOwnershipPolicy::kHostOwned && target_published &&
        selection.ok()) {
      // The host inserted/removed its own text. That is the expected original-text stream,
      // not a loss of the target: publish the new caret while keeping the identity and
      // generation, so the engine suggestion stream can continue. No TSF call is made here.
      ++counters.host_edits_observed;
      published_caret_utf16 = selection.caret_utf16;
      if (sink != nullptr) {
        sink->OnHostEditCompleted(
            context, ObservedTarget{context_id, generation, published_caret_utf16},
            selection);
      }
      return;
    }
    // An external change makes the old generation unusable immediately. No TSF session is
    // started here and nothing is written: the controller's OnTargetLost only releases.
    ResetSelectionRead();
    DropPublished(text_changed || record_read_failed
                      ? ContextDropReason::kExternalTextEdit
                      : ContextDropReason::kExternalSelectionEdit);
    need_bind = true;
  }

  void RevalidateThreadFocus() {
    if (thread_mgr == nullptr) {
      return;
    }
    ++counters.focus_revalidations;
    ITfDocumentMgr* focus = nullptr;
    const HRESULT focus_hr = thread_mgr->GetFocus(&focus);
    if (FAILED(focus_hr) || focus == nullptr) {
      if (focused_dm != nullptr) {
        ++counters.focus_revalidation_changes;
        ResetSelectionRead();
        ReleaseTextSink();
        ReleaseTopContext();
        ReleaseFocusedDocumentManager();
        DropPublished(ContextDropReason::kNoFocusedDocument);
        need_bind = false;
        bind_evidence = TargetBindingEvidence::kNone;
      }
      return;
    }
    if (focused_dm == nullptr || !SameDocumentManager(focus, focused_dm)) {
      ++counters.focus_revalidation_changes;
      AdoptFocusedDocumentManager(focus);
      focus->Release();
      SetBindEvidenceIfNone(TargetBindingEvidence::kRevalidation);
      AdoptCurrentContext(ContextDropReason::kFocusMoved);
      return;
    }
    focus->Release();
  }

  void Pump() {
    ++counters.pumps;
    if (closed || !subscribed) {
      return;
    }
    // Keep the recorded thread-focus facts current for diagnostics and for the next guard
    // decision. The real GetFocus revalidation below remains the authority for a target
    // change; this query is the TSF thread-focus statement and never substitutes for it.
    ReadThreadFocusFacts();
    // Defensive revalidation: if a focus notification was not delivered, the real
    // GetFocus is still the authority. This is not claimed as notification coverage.
    RevalidateThreadFocus();
    if (top_recheck) {
      top_recheck = false;
      // The Push/Pop/thread-focus notification has returned; the stack now reflects the
      // change. A notification-provided evidence value is kept when one was set.
      SetBindEvidenceIfNone(TargetBindingEvidence::kRevalidation);
      AdoptCurrentContext(ContextDropReason::kTopContextChanged);
    }
    if (!need_bind || top_context == nullptr) {
      return;
    }
    if (selection_read_active) {
      if (reader->Pending()) {
        return;
      }
      const SelectionSnapshot snapshot = reader->Take();
      selection_read_active = false;
      ++counters.selection_reads_completed;
      if (!snapshot.ok()) {
        ++counters.selection_refusals;
        DropPublished(ReasonForSelection(snapshot.status));
        need_bind = false;
        bind_evidence = TargetBindingEvidence::kNone;
        return;
      }
      PublishTarget(snapshot.caret_utf16);
      return;
    }
    if (!reader->Begin(top_context)) {
      ++counters.selection_refusals;
      DropPublished(ContextDropReason::kSelectionUnavailable);
      need_bind = false;
      bind_evidence = TargetBindingEvidence::kNone;
      return;
    }
    ++counters.selection_reads_begun;
    selection_read_active = true;
    if (reader->Pending()) {
      return;
    }
    const SelectionSnapshot snapshot = reader->Take();
    selection_read_active = false;
    ++counters.selection_reads_completed;
    if (!snapshot.ok()) {
      ++counters.selection_refusals;
      DropPublished(ReasonForSelection(snapshot.status));
      need_bind = false;
      return;
    }
    PublishTarget(snapshot.caret_utf16);
  }

  void Shutdown() {
    if (closed) {
      return;
    }
    closed = true;
    counters.closed = true;
    ResetSelectionRead();
    ReleaseTextSink();
    if (thread_source != nullptr) {
      if (thread_focus_cookie != TF_INVALID_COOKIE) {
        thread_source->UnadviseSink(thread_focus_cookie);
        thread_focus_cookie = TF_INVALID_COOKIE;
        ++counters.thread_focus_sink_unadvises;
      }
      if (thread_cookie != TF_INVALID_COOKIE) {
        thread_source->UnadviseSink(thread_cookie);
        ++counters.sink_unadvises;
        thread_cookie = TF_INVALID_COOKIE;
      }
      thread_source->Release();
      thread_source = nullptr;
    }
    if (thread_mgr != nullptr) {
      thread_mgr->Release();
      thread_mgr = nullptr;
      ++counters.thread_mgr_releases;
    }
    ReleaseTopContext();
    ReleaseFocusedDocumentManager();
    sink = nullptr;
    sink_unknown = nullptr;
    subscribed = false;
  }

  bool Subscribe(ITfThreadMgr* thread_manager, ContextObserverSink* new_sink,
                 std::string* error) {
    if (closed) {
      if (error != nullptr) {
        *error = "observer is closed";
      }
      return false;
    }
    if (subscribed) {
      if (error != nullptr) {
        *error = "observer is already subscribed";
      }
      return false;
    }
    if (thread_manager == nullptr || new_sink == nullptr) {
      if (error != nullptr) {
        *error = "null thread manager or sink";
      }
      return false;
    }
    sink = new_sink;
    thread_manager->AddRef();
    thread_mgr = thread_manager;
    ++counters.thread_mgr_addrefs;
    ITfSource* source = nullptr;
    HRESULT hr =
        thread_mgr->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&source));
    if (FAILED(hr) || source == nullptr) {
      if (error != nullptr) {
        *error = "ITfThreadMgr::QueryInterface(IID_ITfSource) failed " + HrText(hr);
      }
      Shutdown();
      return false;
    }
    DWORD cookie = TF_INVALID_COOKIE;
    hr = source->AdviseSink(IID_ITfThreadMgrEventSink, sink_unknown, &cookie);
    if (FAILED(hr) || cookie == TF_INVALID_COOKIE) {
      if (error != nullptr) {
        *error = "AdviseSink(IID_ITfThreadMgrEventSink) failed " + HrText(hr);
      }
      source->Release();
      Shutdown();
      return false;
    }
    thread_source = source;
    thread_cookie = cookie;
    ++counters.sink_advises;
    // P3.13: the thread input-focus sink is advised on the same ITfSource. A failure is
    // recorded, not hidden; the real IsThreadFocus/GetFocus revalidation below still works,
    // and its evidence is labelled as a revalidation, never as a notification.
    DWORD focus_cookie = TF_INVALID_COOKIE;
    const HRESULT focus_hr =
        source->AdviseSink(IID_ITfThreadFocusSink, sink_unknown, &focus_cookie);
    if (SUCCEEDED(focus_hr) && focus_cookie != TF_INVALID_COOKIE) {
      thread_focus_cookie = focus_cookie;
      ++counters.thread_focus_sink_advises;
    } else {
      thread_focus_sink_advise_failed = true;
      ++counters.thread_focus_sink_advise_failures;
    }
    subscribed = true;
    counters.subscribed = true;

    // Adopt the current focus without assuming a notification will arrive for it. The
    // initial selection read and bind happen in Pump.
    bind_evidence = TargetBindingEvidence::kInitialAdoption;
    ITfDocumentMgr* focus = nullptr;
    const HRESULT focus_get_hr = thread_mgr->GetFocus(&focus);
    if (SUCCEEDED(focus_get_hr) && focus != nullptr) {
      AdoptFocusedDocumentManager(focus);
      focus->Release();
      AdoptCurrentContext(ContextDropReason::kNone);
    } else {
      AdoptCurrentContext(ContextDropReason::kNone);
    }
    if (top_context == nullptr) {
      bind_evidence = TargetBindingEvidence::kNone;
    }
    return true;
  }
};

// --- the COM sinks ------------------------------------------------------------------------

class ObserverComSink final : public ITfThreadMgrEventSink,
                              public ITfTextEditSink,
                              public ITfThreadFocusSink {
 public:
  explicit ObserverComSink(std::shared_ptr<ContextObserver::State> state)
      : state_(std::move(state)) {}
  ~ObserverComSink() = default;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) {
      return E_INVALIDARG;
    }
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfThreadMgrEventSink) {
      *object = static_cast<ITfThreadMgrEventSink*>(this);
    } else if (riid == IID_ITfTextEditSink) {
      *object = static_cast<ITfTextEditSink*>(this);
    } else if (riid == IID_ITfThreadFocusSink) {
      *object = static_cast<ITfThreadFocusSink*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(reference_count_.fetch_add(1) + 1);
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const LONG remaining = reference_count_.fetch_sub(1) - 1;
    if (remaining == 0) {
      delete this;
    }
    return static_cast<ULONG>(remaining);
  }

  HRESULT STDMETHODCALLTYPE OnInitDocumentMgr(ITfDocumentMgr*) override {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE OnUninitDocumentMgr(ITfDocumentMgr* document_manager)
      override {
    state_->HandleUninitDocumentManager(document_manager);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE OnSetFocus(ITfDocumentMgr* focus,
                                       ITfDocumentMgr*) override {
    state_->HandleSetFocus(focus);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE OnPushContext(ITfContext* context) override {
    state_->HandlePushPop(context, true);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE OnPopContext(ITfContext* context) override {
    state_->HandlePushPop(context, false);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE OnEndEdit(ITfContext* context, TfEditCookie read_only,
                                      ITfEditRecord* record) override {
    state_->HandleEndEdit(context, read_only, record);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE OnSetThreadFocus() override {
    state_->HandleThreadFocus(true);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE OnKillThreadFocus() override {
    state_->HandleThreadFocus(false);
    return S_OK;
  }

 private:
  std::atomic<LONG> reference_count_{1};
  std::shared_ptr<ContextObserver::State> state_;
};

// --- ContextObserver ----------------------------------------------------------------------

ContextObserver::ContextObserver(ContextRegistry& registry, TfClientId own_client_id,
                                 ThreadFocusPolicy thread_focus_policy,
                                 TextOwnershipPolicy text_ownership)
    : state_(std::make_shared<State>(registry, own_client_id, thread_focus_policy,
                                     text_ownership)) {
  auto* com_sink = new ObserverComSink(state_);
  state_->sink_unknown =
      static_cast<IUnknown*>(static_cast<ITfThreadMgrEventSink*>(com_sink));
  // One owning reference is kept by the observer; TSF's own reference (if it takes one)
  // is released by UnadviseSink.
  sink_object_ = com_sink;
}

ContextObserver::~ContextObserver() { Shutdown(); }

bool ContextObserver::Subscribe(ITfThreadMgr* thread_manager, ContextObserverSink* sink,
                                std::string* error) {
  return state_->Subscribe(thread_manager, sink, error);
}

void ContextObserver::SetSink(ContextObserverSink* sink) {
  if (!state_->closed) {
    state_->sink = sink;
  }
}

void ContextObserver::Shutdown() {
  if (state_ == nullptr) {
    return;
  }
  state_->Shutdown();
  if (sink_object_ != nullptr) {
    sink_object_->Release();
    sink_object_ = nullptr;
  }
}

bool ContextObserver::closed() const { return state_->closed; }

void ContextObserver::Pump() { state_->Pump(); }

HANDLE ContextObserver::PendingSelectionEvent() const {
  if (state_->closed || !state_->selection_read_active) {
    return nullptr;
  }
  return state_->reader->CompletionEvent();
}

void ContextObserver::RequestRebind() {
  if (state_->closed || !state_->subscribed || state_->top_context == nullptr) {
    return;
  }
  state_->need_bind = true;
  state_->SetBindEvidenceIfNone(TargetBindingEvidence::kRevalidation);
}

bool ContextObserver::has_current_context() const {
  return !state_->closed && state_->subscribed && state_->top_context != nullptr;
}

TargetRevalidation ContextObserver::Revalidate(ITfContext* context,
                                               std::uint64_t context_id,
                                               std::uint64_t generation) {
  return state_->RevalidateTarget(context, context_id, generation);
}

std::shared_ptr<TargetWriteGuard> ContextObserver::CreateWriteGuard() {
  // The callback runs later (possibly after Shutdown). A weak reference makes a destroyed
  // observer state answer "not current" instead of dereferencing a stale pointer.
  std::weak_ptr<State> weak = state_;
  return MakeTargetWriteGuard(
      [weak](ITfContext* context, std::uint64_t context_id,
             std::uint64_t generation) {
        const std::shared_ptr<State> state = weak.lock();
        if (!state) {
          return false;
        }
        return state->RevalidateTarget(context, context_id, generation).ok();
      });
}

ContextObserverSnapshot ContextObserver::Snapshot() const {
  ContextObserverSnapshot snapshot = state_->counters;
  snapshot.subscribed = state_->subscribed;
  snapshot.closed = state_->closed;
  snapshot.has_published_target = state_->target_published;
  snapshot.target = ObservedTarget{state_->context_id, state_->generation,
                                   state_->published_caret_utf16,
                                   state_->last_bind_evidence};
  snapshot.text_sink_advised = state_->text_source != nullptr &&
                               state_->text_cookie != TF_INVALID_COOKIE;
  snapshot.thread_sink_advised =
      state_->thread_source != nullptr && state_->thread_cookie != TF_INVALID_COOKIE;
  snapshot.thread_focus_sink_advised =
      state_->thread_source != nullptr &&
      state_->thread_focus_cookie != TF_INVALID_COOKIE;
  snapshot.thread_focus_sink_advise_failed =
      state_->thread_focus_sink_advise_failed;
  snapshot.has_top_context = state_->top_context != nullptr;
  snapshot.last_bind_evidence = state_->last_bind_evidence;
  snapshot.thread_focus_policy = state_->policy;
  snapshot.thread_focus_query_ok = state_->thread_focus_query_ok;
  snapshot.thread_focus_query_value = state_->thread_focus_query_value;
  snapshot.thread_focus_query_hr = state_->thread_focus_query_hr;
  snapshot.win32_thread_focus = state_->win32_thread_focus;
  snapshot.awaiting_bind = state_->need_bind;
  snapshot.selection_read_pending =
      state_->selection_read_active && state_->reader->Pending();
  snapshot.registry = state_->registry->Diagnostics();
  snapshot.selection_reader = state_->reader->Diagnostics();
  return snapshot;
}

}  // namespace context
}  // namespace tsf
}  // namespace epocanva
