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
#include "key_admission.h"
#include "observed_binding.h"
#include "permission_query.h"
#include "product_engine_bootstrap.h"
#include "sta_result_waker.h"

namespace epocanva {
namespace tsf {
namespace product {

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
  // W2-A-R degraded catch-up and first-token ownership. All counts are text-free; the
  // unapplied units are a length, never text.
  bool raw_mode = false;
  bool raw_submitted = false;
  bool engine_stale = false;
  std::uint64_t raw_unapplied_units = 0;
  std::uint64_t raw_keys = 0;
  std::uint64_t raw_commits = 0;
  std::uint64_t raw_entered = 0;
  std::uint64_t raw_exits = 0;
  std::uint64_t raw_ignored_edits = 0;
  std::uint64_t raw_cancelled_keys = 0;
  std::uint64_t raw_cancelled_units = 0;
  std::uint64_t raw_unrecoverable = 0;
  std::uint64_t raw_reason_unapplied = 0;
  std::uint64_t raw_reason_not_candidate = 0;
  std::uint64_t raw_reason_high_water = 0;
  std::uint64_t raw_reason_admission = 0;
  std::uint64_t token_host_keys = 0;
  bool candidate_menu_open = false;
  std::uint64_t candidate_version = 0;
  int candidate_selected = -1;
  std::size_t candidate_items = 0;
  std::uint64_t candidate_stale_selections = 0;
  CandidateUiDiagnostics candidate_ui;
  bool pacing = false;
  KeyAdmissionSnapshot keys;
  coordinator::CoordinatorSnapshot coordinator;
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

  enum class SubmitAttempt {
    kNoHead = 0,
    kSubmitted,
    kWaited,
    kNotEatenHostKeeps,
    kFailedEaten,
  };

  bool BuildKey(WPARAM wparam, LPARAM lparam, bool key_down, BuiltKey* key) const;
  bool AdmissionAvailable(bool composition_active) const;
  SubmitAttempt SubmitHead(bool immediate);
  void ResolveInFlight();
  void UpdatePacing();
  void FailQueuedAndMaybeInvalidate(KeyFailure failure, bool invalidate);
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
  // Literal mode: track a host-kept token so a later attach cannot take over the middle of
  // a word, and count the key. No engine, query or edit is touched.
  void TrackLiteralKey(WPARAM wparam, LPARAM lparam);

  // W2-A-R/W2-B degraded catch-up. Admitted input that cannot be given to the engine
  // without waiting is withdrawn from the queue in FIFO order and captured as raw UTF-16;
  // it is committed through the coordinator with NO engine call, replacing the owned
  // composition (when one exists) with its displayed text plus the captured suffix. Once
  // such a commit applied, the engine client is stale and the binding is rebuilt before
  // engine input resumes. The run is a sequence of bounded chunks: a full chunk is flushed
  // to the coordinator and the next key starts a new chunk, so the bound never splits a key
  // or a surrogate pair. A chunk that cannot be flushed (the document is not accepting
  // writes) is the only case that reports an explicit capacity loss.
  static constexpr std::size_t kRawCapacityUtf16 = RawTextRun::kDefaultCapacityUtf16;

  bool IsTextKey(const BuiltKey& key) const;
  // True when every translated UTF-16 code unit is in the English schema alphabet (letters,
  // digits and the protected marks ' - @ . _ : / \ ? & = # % +). Those marks are token
  // characters, never boundaries, so a host-owned token stays host-owned across them.
  bool IsTokenText(const BuiltKey& key) const;
  // True only for translated characters the degraded run may capture. Shortcuts, privacy
  // refusals, disabled input, dead keys and untranslatable events are never captured.
  bool RawCaptureWorthy(const BuiltKey& key) const;
  bool RawUnapplied() const;
  bool HasPendingEngineInput() const;
  std::size_t QueueDepth() const;
  void EnterRawMode();
  void FoldWithdrawnKey(const AdmissionKey& key);
  bool CaptureRawText(std::u16string_view text);
  // Escape cancels exactly the accepted-but-not-yet-submitted suffix. An already submitted
  // chunk is not retracted; it applies once and the run ends after it resolves.
  void CancelRawSuffix();
  bool HandleRawKey(const BuiltKey& key);
  void FlushRaw();
  void ResolveRaw();
  void FailRawState(KeyFailure failure);
  void FinishRawRun();

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
  KeyAdmissionQueue queue_;
  std::unique_ptr<CandidateUi> candidate_ui_;
  // The context the UI reads its caret rectangle from. Owned for the binding's duration and
  // released on target loss/Stop; never used off the STA.
  ITfContext* ui_context_ = nullptr;
  int candidate_selection_ = -1;
  std::uint64_t candidate_ui_version_ = 0;
  std::uint64_t candidate_dismissed_version_ = 0;
  std::uint64_t candidate_stale_selections_ = 0;

  bool raw_active_ = false;
  bool raw_submitted_ = false;
  bool raw_end_requested_ = false;
  bool engine_stale_ = false;
  std::uint64_t raw_work_id_ = 0;
  RawTextRun raw_run_;
  std::size_t raw_chunk_keys_ = 0;
  std::size_t raw_submitted_keys_ = 0;
  std::size_t raw_submitted_units_ = 0;
  // A token the host already started. Its remaining characters are never offered to the
  // engine; the host inserts them, so the field's text stays contiguous.
  HostTokenOwnership host_token_;

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
  std::uint64_t raw_entered_ = 0;
  std::uint64_t raw_exits_ = 0;
  std::uint64_t raw_commits_ = 0;
  std::uint64_t raw_keys_ = 0;
  std::uint64_t raw_chars_ = 0;
  std::uint64_t raw_ignored_edits_ = 0;
  std::uint64_t raw_cancelled_keys_ = 0;
  std::uint64_t raw_cancelled_units_ = 0;
  std::uint64_t raw_unrecoverable_ = 0;
  std::uint64_t raw_reason_unapplied_ = 0;
  std::uint64_t raw_reason_not_candidate_ = 0;
  std::uint64_t raw_reason_high_water_ = 0;
  std::uint64_t raw_reason_admission_ = 0;
  std::uint64_t token_host_keys_ = 0;
  std::uint64_t literal_key_downs_ = 0;
  std::uint64_t engine_attach_attempts_ = 0;
  std::uint64_t engine_attach_failures_ = 0;
  std::string engine_failure_;
};

}  // namespace product
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_PRODUCT_TEXT_SERVICE_CONTROLLER_H_
