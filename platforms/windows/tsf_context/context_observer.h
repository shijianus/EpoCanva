// EpoCanva P3.10/P3.13 -- the STA TSF context observer.
//
// WHAT THIS OBJECT DOES. It subscribes to the real thread-manager notifications
// (ITfThreadMgrEventSink), the thread input-focus notifications (ITfThreadFocusSink) and to
// the current context's edit notifications (ITfTextEditSink), identifies the top context of
// the focused document manager, assigns the project identity/generation from
// ContextRegistry, reads real selection snapshots, and publishes exactly three binding
// events to one host-provided sink:
//
//   OnTargetLost(reason)            the old binding must stop being usable NOW
//   OnTargetBound(context, target)  a new binding may be created (called from Pump only)
//   OnOwnEditCompleted(context, target, selection)
//                                   our own write completed; reconcile the caret
//
// That is a binding port, not a general event bus: three methods, one listener, no queues.
//
// THREADING AND LIFETIME. Every method, every callback and every COM pointer lives on the
// STA thread that created the observer. No COM pointer is stored in a value type that
// outlives a call and nothing crosses a thread. Subscribe() AddRefs the thread manager and
// installs the thread-manager sink; each current context is AddRef'd and advised. Shutdown()
// unadvises both sinks, abandons a pending selection read, releases every held reference
// and marks the state closed. The COM sink objects hold a shared_ptr to that state, so a
// late notification after Shutdown sees `closed` and touches no COM object.
//
// TARGET REVALIDATION (P3.13). Revalidate() answers the live question the coordinator asks
// before ordinary input and before an ordinary edit: thread focus (the focus sink, the real
// IsThreadFocus query, or an explicitly requested lab-only real Win32 thread-focus
// revalidation), the current GetFocus document manager, its canonical top context, and the
// registry id/generation. CreateWriteGuard() repeats that check inside a granted P3.7 edit
// callback. The evidence source is published verbatim; a revalidation is never renamed as a
// notification. The Win32 fallback is a construction-time policy: the product default is
// StrictTsf, and only the lab/test configurations explicitly request kLabWin32Fallback.
//
// WHERE BINDING HAPPENS. Not inside a TSF notification. OnSetFocus/OnPushContext/
// OnPopContext/OnUninitDocumentMgr and OnEndEdit may only DROP the old binding and mark a
// rebind pending; the actual read-only selection query and OnTargetBound happen in Pump(),
// which the host calls outside any notification. This is what keeps the observer from
// starting a TSF edit session inside the ecReadOnly callback and from writing there.
//
// OWN VERSUS EXTERNAL EDITS. ITfTextEditSink::OnEndEdit is called for completed read/write
// edit sessions. Inside it, ITfContext::InWriteSession(own_client_id) is the documented way
// to learn whether our client performed that session (the probe in this phase confirms the
// distinction on the real manager). Only an OnEndEdit for the currently advised context is
// considered at all; text/selection changes on other documents are never observed. An own
// edit updates the caret and never invalidates the binding. An external edit drops the
// binding immediately with a generation that can no longer be used, and schedules a fresh
// read-only snapshot for Pump(). The distinction is by the real client id + completed write
// session, not by "same thread".
//
// WHAT IS NOT HERE. No key sink, no registration, no candidate UI, no document text read,
// no clipboard, no UIA, no writes of any kind, no automatic retry or resync, no cleanup of
// a possibly-stale composition. Closing the observer never issues a TSF text operation.
#ifndef EPOCANVA_WINDOWS_TSF_CONTEXT_CONTEXT_OBSERVER_H_
#define EPOCANVA_WINDOWS_TSF_CONTEXT_CONTEXT_OBSERVER_H_

#include <windows.h>
#include <msctf.h>

#include <cstdint>
#include <memory>
#include <string>

#include "context_registry.h"
#include "selection_reader.h"
#include "target_revalidation.h"

namespace epocanva {
namespace tsf {
namespace context {

enum class ContextDropReason : std::uint8_t {
  kNone = 0,
  kNoFocusedDocument,          // OnSetFocus(NULL) / GetFocus has no document
  kNoTopContext,               // GetTop failed or the stack has no context
  kDocumentManagerUninitialized,
  kFocusMoved,                 // a different document manager received the focus
  kTopContextChanged,          // Push/Pop changed the top context of the focused document
  kExternalTextEdit,           // an external write changed document text
  kExternalSelectionEdit,      // an external write changed the selection
  kSelectionNonCollapsed,      // the real selection is a range, not an insertion point
  kSelectionUnsupported,       // not an ACP range / interim character / no active end
  kSelectionUnavailable,       // the read session could not produce a reliable position
  kEditSinkUnavailable,        // ITfTextEditSink could not be installed on the context
  kRegistryCapacity,           // the identity registry refused a new context
  kUncertainBlocked,           // this context is latched Uncertain in the registry
  kThreadFocusLost,            // ITfThreadFocusSink::OnKillThreadFocus arrived
  kThreadFocusChanged,         // thread focus returned; the target is re-read, not reused
  kClosed,
};

const char* DescribeContextDropReason(ContextDropReason reason);

// Where the publication of a binding target came from. This names the real evidence; the
// lab and the self-test display it instead of inferring a path from counters.
enum class TargetBindingEvidence : std::uint8_t {
  kNone = 0,
  kInitialAdoption,            // Subscribe adopted an already-focused document
  kDocumentFocusNotification,  // ITfThreadMgrEventSink::OnSetFocus
  kTopContextNotification,     // OnPushContext/OnPopContext
  kThreadFocusNotification,    // ITfThreadFocusSink::OnSetThreadFocus
  kRevalidation,               // Pump re-read the real GetFocus/GetTop
};

const char* DescribeTargetBindingEvidence(TargetBindingEvidence evidence);

// A published binding target. The caret is an ACP offset in UTF-16 code units, read from
// the real selection before this target was published.
struct ObservedTarget {
  std::uint64_t context_id = 0;
  std::uint64_t generation = 0;
  LONG caret_utf16 = 0;
  TargetBindingEvidence evidence = TargetBindingEvidence::kNone;
  bool valid() const { return context_id != 0 && generation != 0; }
};

// Who owns ordinary text in the bound document. The composition-owner policy treats an
// external write as a loss of the binding (the writer can no longer vouch for its range).
// The host-owned policy treats it as the expected stream of host-inserted characters: the
// caret is re-read and published to the sink WITHOUT changing the identity/generation, so
// suggestions can continue while original text is never owned by the product.
enum class TextOwnershipPolicy : std::uint8_t {
  kCompositionOwner = 0,
  kHostOwned,
};

enum class ObservedEditKind : std::uint8_t { kOwn = 0, kExternal };

struct ObservedEdit {
  ObservedEditKind kind = ObservedEditKind::kOwn;
  bool text_changed = false;
  bool selection_changed = false;
  bool own_in_write_session = false;
  // The real selection read with the callback's ecReadOnly cookie. Meaningful only when
  // status == kOk; never used as a binding caret by itself.
  SelectionSnapshot selection;
};

// The single listener. `context` is borrowed for the duration of the call only; the callee
// may AddRef it (the coordinator's Bind does) but must not store the pointer beyond the
// call. All methods run on the STA thread.
class ContextObserverSink {
 public:
  virtual ~ContextObserverSink() {}
  virtual void OnTargetLost(ContextDropReason reason) = 0;
  virtual void OnTargetBound(ITfContext* context, const ObservedTarget& target) = 0;
  virtual void OnOwnEditCompleted(ITfContext* context, const ObservedTarget& target,
                                  const SelectionSnapshot& selection) = 0;
  // Host-owned text policy only. The document changed through the host's own input path and
  // the new caret was read successfully. Runs inside OnEndEdit's read-only callback; it may
  // update a mirror but must not write.
  virtual void OnHostEditCompleted(ITfContext* context, const ObservedTarget& target,
                                   const SelectionSnapshot& selection) {
    (void)context;
    (void)target;
    (void)selection;
  }
};

// Value-only diagnostics for assertions and text-free logging.
struct ContextObserverSnapshot {
  bool subscribed = false;
  bool closed = false;
  bool has_published_target = false;
  ObservedTarget target;
  bool text_sink_advised = false;
  bool thread_sink_advised = false;
  // P3.13: the separate ITfThreadFocusSink advice. `thread_sink_advised` is the thread
  // manager event sink; this one is the thread input-focus notification sink.
  bool thread_focus_sink_advised = false;
  bool thread_focus_sink_advise_failed = false;
  bool has_top_context = false;
  TargetBindingEvidence last_bind_evidence = TargetBindingEvidence::kNone;
  ThreadFocusPolicy thread_focus_policy = ThreadFocusPolicy::kStrictTsf;
  ThreadFocusEvidence last_thread_focus_evidence = ThreadFocusEvidence::kNone;
  bool thread_focus_query_ok = false;
  bool thread_focus_query_value = false;
  HRESULT thread_focus_query_hr = S_OK;
  bool win32_thread_focus = false;
  TargetRevalidation last_target_revalidation{};
  std::uint64_t target_revalidations = 0;
  std::uint64_t target_revalidation_failures = 0;
  // Pump state: a bind is still owed, and whether it is waiting on a deferred read.
  bool awaiting_bind = false;
  bool selection_read_pending = false;

  // Host-owned policy: external edits accepted as the host's own text stream (caret
  // republished without changing identity/generation).
  std::uint64_t host_edits_observed = 0;
  std::uint64_t focus_notifications = 0;
  std::uint64_t focus_loss_notifications = 0;
  std::uint64_t push_notifications = 0;
  std::uint64_t pop_notifications = 0;
  std::uint64_t uninit_notifications = 0;
  std::uint64_t background_events_ignored = 0;
  std::uint64_t focus_revalidations = 0;
  std::uint64_t focus_revalidation_changes = 0;
  // P3.13 thread-focus sink and real focus queries.
  std::uint64_t thread_focus_set_notifications = 0;
  std::uint64_t thread_focus_kill_notifications = 0;
  std::uint64_t thread_focus_queries = 0;
  std::uint64_t thread_focus_query_failures = 0;
  std::uint64_t win32_thread_focus_checks = 0;
  std::uint64_t win32_thread_focus_accepts = 0;
  std::uint64_t thread_focus_sink_advises = 0;
  std::uint64_t thread_focus_sink_unadvises = 0;
  std::uint64_t thread_focus_sink_advise_failures = 0;

  std::uint64_t own_edits = 0;
  std::uint64_t external_edits = 0;
  std::uint64_t text_changes = 0;
  std::uint64_t selection_changes = 0;
  std::uint64_t edits_without_changes = 0;
  std::uint64_t edits_ignored_not_current = 0;
  std::uint64_t own_edits_without_target = 0;

  std::uint64_t target_drops = 0;
  std::uint64_t target_binds = 0;
  std::uint64_t pumps = 0;
  std::uint64_t selection_reads_begun = 0;
  std::uint64_t selection_reads_completed = 0;
  std::uint64_t selection_refusals = 0;
  // The last reason a published target was dropped, and the last reason a candidate could
  // not be published (no callback exists for the latter).
  ContextDropReason last_target_lost_reason = ContextDropReason::kNone;
  ContextDropReason last_refusal_reason = ContextDropReason::kNone;

  std::uint64_t sink_advises = 0;
  std::uint64_t sink_unadvises = 0;
  std::uint64_t sink_advise_failures = 0;
  std::uint64_t late_notifications_after_close = 0;

  std::uint64_t thread_mgr_addrefs = 0;
  std::uint64_t thread_mgr_releases = 0;
  std::uint64_t document_mgr_addrefs = 0;
  std::uint64_t document_mgr_releases = 0;
  std::uint64_t context_addrefs = 0;
  std::uint64_t context_releases = 0;

  ContextRegistryDiagnostics registry;
  SelectionReaderDiagnostics selection_reader;
};

class ObserverComSink;

class ContextObserver : public TargetRevalidator {
 public:
  // `registry` must outlive the observer and is used only on this STA thread. The client id
  // is the TIP's real TfClientId: it is used for the read-only selection sessions and as
  // the InWriteSession identity that distinguishes our writes from other clients'. The
  // thread-focus policy must be chosen explicitly by the caller; there is no implicit
  // fallback and no product switch that ignores it.
  ContextObserver(ContextRegistry& registry, TfClientId own_client_id,
                  ThreadFocusPolicy thread_focus_policy,
                  TextOwnershipPolicy text_ownership = TextOwnershipPolicy::kCompositionOwner);
  ~ContextObserver();

  ContextObserver(const ContextObserver&) = delete;
  ContextObserver& operator=(const ContextObserver&) = delete;

  // Installs the thread-manager sink and adopts the current focus (if any). `sink` must
  // outlive the subscription or be replaced with SetSink before it is destroyed. Returns
  // false with a message when the real sink cannot be installed; nothing is left advised
  // in that case.
  bool Subscribe(ITfThreadMgr* thread_manager, ContextObserverSink* sink,
                 std::string* error);

  // Replaces the listener without touching the TSF subscriptions. The current target is not
  // replayed; the host triggers a target change (or uses Snapshot()) if the new listener
  // needs to bind.
  void SetSink(ContextObserverSink* sink);

  // Terminal, idempotent. Unadvises every sink, abandons a pending selection read and
  // releases every held COM reference. Never issues a TSF text operation.
  void Shutdown();
  bool closed() const;

  // Non-blocking. Revalidates the real thread focus, advances a pending selection read, and
  // publishes OnTargetBound when a fresh collapsed selection exists. Never waits, never
  // pumps messages, never starts an edit session from inside a TSF notification.
  void Pump();

  // The completion fact Pump is waiting on for a deferred selection read; the host
  // bounded-waits on it and dispatches STA messages.
  HANDLE PendingSelectionEvent() const;

  // Asks Pump to re-read the real GetFocus/GetTop and publish the CURRENT target. Used by
  // the binding controller after a Retiring/Busy refusal; it never replays the refused
  // target. It does nothing when no top context is known or the observer is closed.
  void RequestRebind();
  bool has_current_context() const;

  // TargetRevalidator. Revalidate() performs the full live check (thread focus, current
  // document manager, canonical top context, registry id/generation). CreateWriteGuard()
  // returns a guard that repeats the check inside a granted edit callback and is safe after
  // observer shutdown (a lost state refuses).
  TargetRevalidation Revalidate(ITfContext* context, std::uint64_t context_id,
                                std::uint64_t generation) override;
  std::shared_ptr<TargetWriteGuard> CreateWriteGuard() override;

  ContextObserverSnapshot Snapshot() const;

 private:
  friend class ObserverComSink;
  struct State;
  std::shared_ptr<State> state_;
  // One owning reference to the COM sink; TSF's reference (if any) is returned by
  // UnadviseSink. The sink holds a shared_ptr to State, so a late notification after
  // Shutdown sees `closed` and touches no released COM pointer.
  ObserverComSink* sink_object_ = nullptr;
};

}  // namespace context
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_CONTEXT_CONTEXT_OBSERVER_H_
