// EpoCanva W2 -- the STA product controller behind the real TSF key sink.
//
// WHAT THIS OWNS. One activation's complete input pipeline on the STA:
//   ContextObserver (StrictTsf focus policy) -> ObservedBindingController ->
//   ContextCoordinator (single flight) -> CompositionEditSession -> the application.
// It also owns the bounded keyboard admission queue and the fixed STA wake channel. It
// never creates a second engine, a second translation rule set, or a second permission
// reader: every allowed text key travels through the existing coordinator path.
//
// KEY CONTRACT.
//   TestKey / OnTestKeyDown: prediction only. It captures keyboard state, translates with
//     ToUnicodeEx (no state change), asks the coordinator's pure Evaluate(), and checks the
//     queue capacity. It never submits a command, starts a query or requests an edit.
//   HandleKeyDown: accepts the key into the bounded queue and, when the coordinator is
//     idle, tries one immediate submission. pfEaten is TRUE only when the key is really
//     queued/submitted. A key the coordinator refuses before any engine call is NOT eaten
//     when no composition exists, so the host keeps it. With a live composition the
//     controller eats it and records an explicit failure instead of inserting the
//     character inside our preedit.
//   Key-up: never eaten, never translated, never dispatched.
//
// FOCUS AND GENERATION. Every accepted key stores the context id and generation that were
// current when it was accepted. Dispatch requires the coordinator's live identity to match;
// a focus change fails the queued tail and never delivers it to the new document. An
// in-flight key whose target was lost stays in flight until the coordinator is idle again,
// so the next key cannot overtake it.
//
// WAKE. The controller implements StaWakeTarget. The waker posts a generation-only message;
// OnStaWake verifies it and calls PumpNow(), which advances retirements, permission reads,
// engine results, edits, rebinds and the next queued key. Repeated wakes are harmless.
//
// LIFECYCLE. Start() subscribes the observer and starts the waker; Stop() unregisters the
// waker target first, then fails every queued key explicitly, closes the coordinator
// (release-only, no commits) and shuts the observer down. Stop never waits for the engine:
// the service releases its lease after Stop returns. The destructor does not call Stop.
#ifndef EPOCANVA_WINDOWS_TSF_PRODUCT_TEXT_SERVICE_CONTROLLER_H_
#define EPOCANVA_WINDOWS_TSF_PRODUCT_TEXT_SERVICE_CONTROLLER_H_

#include <windows.h>
#include <msctf.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "candidate_ui.h"
#include "context_observer.h"
#include "context_registry.h"
#include "coordinator.h"
#include "engine_executor.h"
#include "key_decision.h"
#include "key_event.h"
#include "observed_binding.h"
#include "permission_query.h"
#include "product_engine_bootstrap.h"
#include "sta_result_waker.h"

namespace epocanva {
namespace tsf {
namespace product {

namespace input = ::epocanva::windows::input;
namespace coordinator = ::epocanva::tsf::coordinator;

using ProductEngineAttachment = ::epocanva::product::ProductEngineAttachment;
using ProductEngineBootstrap = ::epocanva::product::ProductEngineBootstrap;

struct ControllerDiagnostics {
  bool started = false;
  bool stopping = false;
  bool subscribed = false;
  bool coordinator_bound = false;
  bool observer_closed = false;
  // W2-B engine startup. A controller starts in literal mode immediately; the engine is
  // adopted later on this STA when the asynchronous start reports ready.
  bool engine_ready = false;
  bool engine_attaching = false;
  std::string engine_state;    // idle/starting/ready/failed, from the bootstrap
  std::string engine_failure;  // text-free status/path message, empty while healthy
  std::uint64_t literal_key_downs = 0;
  std::uint64_t engine_attach_attempts = 0;
  std::uint64_t engine_attach_failures = 0;
  std::uint64_t generation = 0;
  std::uint64_t pumps = 0;
  std::uint64_t wakes = 0;
  std::uint64_t stale_wakes = 0;
  std::uint64_t predictions = 0;
  std::uint64_t predicted_eaten = 0;
  std::uint64_t key_downs = 0;
  std::uint64_t eaten = 0;
  std::uint64_t not_eaten = 0;
  std::uint64_t overflow_eaten = 0;
  std::uint64_t engine_client_requests = 0;
  std::uint64_t engine_client_results = 0;
  std::uint64_t engine_client_failures = 0;
  std::uint64_t target_losses = 0;
  std::uint64_t target_binds = 0;
  std::uint64_t rebind_requests = 0;
  // W2-C input ownership. Ordinary text is never captured: every text key returns
  // pfEaten=FALSE and the host inserts it. The engine only produces cancelable suggestions.
  // All counts are text-free and lengths are UTF-16 code units.
  std::uint64_t host_text_keys = 0;
  std::uint64_t suggestion_queued = 0;
  std::uint64_t suggestion_submitted = 0;
  std::uint64_t suggestion_completed = 0;
  std::uint64_t suggestion_dropped = 0;
  std::uint64_t suggestion_stale = 0;
  std::uint64_t suggestion_resets = 0;
  std::uint64_t replacement_attempts = 0;
  std::uint64_t replacement_applied = 0;
  std::uint64_t replacement_refused = 0;
  // The last resolved replacement, as the controller saw it (enum values; text-free).
  int last_replacement_outcome = 0;
  int last_replacement_edit_status = 0;
  int last_replacement_edit_reason = 0;
  long long last_replacement_expected_units = 0;
  std::uint64_t internal_text_losses = 0;
  std::uint64_t host_edits_matched = 0;
  std::uint64_t host_edits_unexpected = 0;
  std::uint64_t reset_boundary = 0;
  std::uint64_t reset_owed = 0;
  std::uint64_t reset_submit_refusal = 0;
  std::uint64_t reset_replacement = 0;
  std::size_t host_token_units = 0;
  std::size_t suggestion_queue_depth = 0;
  bool suggestion_active = false;
  bool suggestion_in_flight = false;
  bool token_abandoned = false;
  bool suggestion_stale_now = false;
  bool replacement_pending = false;
  bool candidate_menu_open = false;
  std::uint64_t candidate_version = 0;
  long long candidate_caret_utf16 = -1;
  std::size_t candidate_input_units = 0;
  int candidate_selected = -1;
  std::size_t candidate_items = 0;
  std::uint64_t candidate_stale_selections = 0;
  CandidateUiDiagnostics candidate_ui;
  bool pacing = false;
  coordinator::CoordinatorSnapshot coordinator;
  coordinator::ObservedBindingController::Diagnostics binding;
  context::ContextObserverSnapshot observer;
};

class ProductTextController final : public StaWakeTarget,
                                    public context::ContextObserverSink,
                                    public CandidateUiHost {
 public:
  struct Options {
    ITfThreadMgr* thread_manager = nullptr;
    TfClientId client_id = 0;
  };

  ProductTextController();
  ~ProductTextController() override;

  ProductTextController(const ProductTextController&) = delete;
  ProductTextController& operator=(const ProductTextController&) = delete;

  // Both run on the activating STA. Start never waits for the engine: it subscribes the
  // asynchronous engine start, opens the wake channel and returns in literal mode. The
  // engine pipeline is built on this STA by the first pump after the bootstrap reports
  // ready; until then every key keeps the host's own default effect.
  bool Start(const Options& options, std::wstring* why);
  // Requests the asynchronous engine start. Called only after the key sink was advised: a
  // failed Activate must not leave a background engine starting with no owner.
  void RequestEngineStart();
  void Stop();

  // OnTestKeyDown: prediction only, no engine/query/edit.
  bool PredictKey(WPARAM wparam, LPARAM lparam);
  // OnKeyDown: returns TRUE only when the key was accepted/eaten.
  bool HandleKeyDown(WPARAM wparam, LPARAM lparam, std::wstring* why);

  void PumpNow();

  std::uint64_t generation() const { return generation_; }
  bool started() const { return started_; }
  bool stopping() const { return stopping_; }

  // StaWakeTarget. Called on the STA from the message-only window.
  void OnStaWake(std::uint64_t generation) override;

  // ContextObserverSink. Every method runs on the STA; OnTargetLost performs no TSF write.
  void OnTargetLost(context::ContextDropReason reason) override;
  void OnTargetBound(ITfContext* context,
                     const context::ObservedTarget& target) override;
  void OnOwnEditCompleted(ITfContext* context,
                          const context::ObservedTarget& target,
                          const context::SelectionSnapshot& selection) override;
  void OnHostEditCompleted(ITfContext* context,
                           const context::ObservedTarget& target,
                           const context::SelectionSnapshot& selection) override;

  // CandidateUiHost. The UI never calls the engine or writes text itself: every click or
  // host-drawn selection arrives here and is validated against the snapshot version and the
  // identity/generation that snapshot was read on.
  void OnUiSelectionChanged(const CandidateUiSelection& selection) override;
  void OnUiFinalize(const CandidateUiSelection& selection) override;
  void OnUiAbort(std::uint64_t snapshot_version, std::uint64_t context_id,
                 std::uint64_t generation) override;

  ControllerDiagnostics Snapshot() const;
  // Test support only: a copy of the coordinator's current menu. No product caller uses
  // this; it lets the controlled direct-sink tests assert selection behaviour without
  // reaching into the coordinator.
  coordinator::CandidateSnapshot CandidateSnapshotForTest() const;

 private:
  class ClientProvider;

  struct BuiltKey {
    input::KeyEventSnapshot event;
    input::KeyText text;
    input::KeyDecision decision;
    bool composition_active = false;
  };

  bool BuildKey(WPARAM wparam, LPARAM lparam, bool key_down, BuiltKey* key) const;
  void UpdatePacing();
  void RequestRebindSoon();
  void ResetObjects();
  // W2-B candidate presentation and selection. UpdateCandidateUi copies the coordinator's
  // current snapshot into the TSF element/window; it is called from Pump (STA) only.
  void UpdateCandidateUi();
  void MoveCandidateSelection(int delta);
  bool FinalizeCandidateSelection(const CandidateUiSelection& selection);
  bool CandidatesOpen() const;
  // W2-B asynchronous engine adoption. Runs on the STA outside every TSF notification. It
  // takes the pending lease (or a fast lease on an already running engine) and builds the
  // observer/coordinator pipeline. Failure leaves the controller in literal mode with a
  // named status; it never blocks and never retries inside a key callback.
  void TryAttachEngine();
  bool BuildEnginePipeline(std::wstring* why);
  // Literal mode: count the key. No engine, query or edit is touched.
  void TrackLiteralKey(WPARAM wparam, LPARAM lparam);

  // W2-C host-owned text. Ordinary keys are never captured or queued for the document; the
  // engine receives a bounded, droppable suggestion stream instead. A suggestion that
  // cannot be submitted is dropped (menu closes); the original text is unaffected.
  struct SuggestionKey {
    input::KeyEventSnapshot event;
    input::KeyText text;
    input::KeyDecision decision;
    std::uint64_t context_id = 0;
    std::uint64_t generation = 0;
    bool ends_token = false;
    // A synthetic Escape sent only to reset the engine's suggestion stream. It never
    // produces a document edit in host-owned mode.
    bool reset = false;
  };
  static constexpr std::size_t kSuggestionQueueCapacity = 64;

  bool IsTextKey(const BuiltKey& key) const;
  bool FeedSuggestionKey(const BuiltKey& key, const coordinator::CoordinatorSnapshot& snapshot,
                         bool ends_token);
  void ResolveSuggestionWork();
  void SubmitNextSuggestion();
  void QueueEngineReset();
  void EndSuggestionToken();
  void ResetSuggestionStream(bool invalidate_binding);
  void MarkSuggestionStale();
  void DismissCandidateMenu();
  bool SubmitReplacement(const CandidateUiSelection& selection);
  void ResolveReplacement();
  // UTF-16 code units the host has inserted since the current token started. Used only to
  // reject a menu drawn for a shorter prefix; the text itself is never copied or stored.
  std::size_t host_token_units_ = 0;

  ITfThreadMgr* thread_manager_ = nullptr;
  TfClientId client_id_ = 0;
  std::uint64_t generation_ = 0;
  bool started_ = false;
  bool stopping_ = false;
  bool subscribed_ = false;
  bool rebind_soon_ = false;
  bool attaching_ = false;

  // The engine lease and its shared lock, owned for exactly this activation. The lease is
  // released off the STA (ReleaseAttachmentAsync): stopping the engine can join its worker.
  std::shared_ptr<ProductEngineAttachment> engine_;
  std::uint64_t bootstrap_waiter_ = 0;
  struct BootstrapWake {
    std::mutex mutex;
    StaResultWaker* waker = nullptr;
  };
  std::shared_ptr<BootstrapWake> bootstrap_wake_;

  // Destruction order is deliberate: the registry outlives the coordinator and observer.
  std::unique_ptr<context::ContextRegistry> registry_;
  std::unique_ptr<context::PermissionSource> permission_;
  std::unique_ptr<coordinator::ContextCoordinator> coordinator_;
  std::unique_ptr<context::ContextObserver> observer_;
  std::unique_ptr<ClientProvider> clients_;
  std::unique_ptr<coordinator::ObservedBindingController> binding_;
  StaResultWaker waker_;
  std::deque<SuggestionKey> suggestion_queue_;
  bool suggestion_in_flight_ = false;
  bool suggestion_in_flight_reset_ = false;
  bool suggestion_engaged_ = false;
  // One host edit is expected per passed-through key that changes text. A document edit
  // observed without this credit came from outside the key stream (paste, autocorrect,
  // another client); suggestions must not continue across it.
  std::size_t host_edits_expected_ = 0;
  std::uint64_t suggestion_work_id_ = 0;
  bool suggestion_stale_ = false;
  // The current token's suggestion stream was abandoned after a drop. The rest of that
  // token must not be fed to the engine as if it were a complete prefix; the flag clears at
  // the next token boundary or when the host state is re-read on a new target.
  bool token_abandoned_ = false;
  // A token boundary arrived while the stream was stale or a reset could not run yet.
  // The reset is performed once no work item owns the binding.
  bool stream_reset_owed_ = false;
  bool replacement_pending_ = false;
  std::uint64_t replacement_work_id_ = 0;
  std::unique_ptr<CandidateUi> candidate_ui_;
  // The context the UI reads its caret rectangle from. Owned for the binding's duration and
  // released on target loss/Stop; never used off the STA.
  ITfContext* ui_context_ = nullptr;
  int candidate_selection_ = -1;
  std::uint64_t candidate_ui_version_ = 0;
  std::uint64_t candidate_dismissed_version_ = 0;
  std::uint64_t candidate_stale_selections_ = 0;
  // The last real TSF caret/rectangle read for the displayed menu version. -1 means the
  // menu has no verified anchor and the window must stay hidden.
  std::uint64_t candidate_anchor_version_ = 0;
  LONG candidate_anchor_caret_utf16_ = -1;
  RECT candidate_anchor_rect_{};

  std::uint64_t wakes_ = 0;
  std::uint64_t stale_wakes_ = 0;
  std::uint64_t predictions_ = 0;
  std::uint64_t predicted_eaten_ = 0;
  std::uint64_t key_downs_ = 0;
  std::uint64_t eaten_ = 0;
  std::uint64_t not_eaten_ = 0;
  std::uint64_t overflow_eaten_ = 0;
  std::uint64_t target_losses_ = 0;
  std::uint64_t target_binds_ = 0;
  std::uint64_t rebind_requests_ = 0;
  std::uint64_t pumps_ = 0;
  std::uint64_t host_text_keys_ = 0;
  std::uint64_t suggestion_queued_ = 0;
  std::uint64_t suggestion_submitted_ = 0;
  std::uint64_t suggestion_completed_ = 0;
  std::uint64_t suggestion_dropped_ = 0;
  std::uint64_t suggestion_stale_events_ = 0;
  std::uint64_t suggestion_resets_ = 0;
  std::uint64_t replacement_attempts_ = 0;
  std::uint64_t replacement_applied_ = 0;
  std::uint64_t replacement_refused_ = 0;
  int last_replacement_outcome_ = 0;
  int last_replacement_edit_status_ = 0;
  int last_replacement_edit_reason_ = 0;
  long long last_replacement_expected_units_ = 0;
  // UTF-16 units the product suppressed or could not account for. Ordinary text keys never
  // enter this counter because they are never eaten; a replacement that may have mutated the
  // document without verification counts its whole expected token.
  std::uint64_t internal_text_losses_ = 0;
  std::uint64_t host_edits_matched_ = 0;
  std::uint64_t host_edits_unexpected_ = 0;
  std::uint64_t reset_boundary_ = 0;
  std::uint64_t reset_owed_ = 0;
  std::uint64_t reset_submit_refusal_ = 0;
  std::uint64_t reset_replacement_ = 0;
  std::uint64_t literal_key_downs_ = 0;
  std::uint64_t engine_attach_attempts_ = 0;
  std::uint64_t engine_attach_failures_ = 0;
  std::string engine_failure_;
};

}  // namespace product
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_PRODUCT_TEXT_SERVICE_CONTROLLER_H_
