// EpoCanva W2 -- product STA controller implementation. See the header for the contract.
#include "text_service_controller.h"

#include <atomic>
#include <cstdio>
#include <utility>

#include "key_text_translation.h"
#include "keyboard_state.h"
#include "module_lifetime.h"
#include "module_state.h"

namespace epocanva {
namespace tsf {
namespace product {
namespace {

namespace engine = ::epocanva::windows::engine;
namespace input = ::epocanva::windows::input;
namespace ctx = ::epocanva::tsf::context;
namespace edit = ::epocanva::tsf::edit;
using ::epocanva::product::DescribeEngineBootstrapState;
using ::epocanva::product::ReleaseAttachmentAsync;

constexpr char kProductSchema[] = "epocanva_english";
constexpr std::uint32_t kVirtualKeyProcessKey = 0xE5;

std::atomic<std::uint64_t> g_next_generation{1};

input::KeyEventSnapshot EventFromTsf(WPARAM wparam, LPARAM lparam) {
  input::KeyEventSnapshot event;
  event.virtual_key = static_cast<std::uint32_t>(wparam);
  event.scan_code =
      (static_cast<std::uint32_t>(lparam) >> 16) & 0xFFu;
  event.is_extended =
      (static_cast<std::uint32_t>(lparam) & (1u << 24)) != 0;
  event.transition = input::KeyTransition::kKeyDown;
  event.repeat_count = static_cast<std::uint32_t>(lparam) & 0xFFFFu;
  if (event.repeat_count == 0) {
    event.repeat_count = 1;
  }
  event.previously_down =
      (static_cast<std::uint32_t>(lparam) & (1u << 30)) != 0;
  return event;
}

KeyFailure FailureForReject(coordinator::RejectCause cause) {
  switch (cause) {
    case coordinator::RejectCause::kPermissionDenied:
    case coordinator::RejectCause::kPermissionUnknown:
    case coordinator::RejectCause::kPermissionChangedWhileQueued:
      return KeyFailure::kPermissionRefused;
    case coordinator::RejectCause::kEngineRefused:
    case coordinator::RejectCause::kEngineUnavailable:
    case coordinator::RejectCause::kEngineSubmitFailed:
    case coordinator::RejectCause::kEditRefused:
    case coordinator::RejectCause::kEditFailedBeforeMutation:
    case coordinator::RejectCause::kTakeFailed:
      return KeyFailure::kEngineUnavailable;
    case coordinator::RejectCause::kTargetChangedWhileQueued:
      return KeyFailure::kTargetChanged;
    case coordinator::RejectCause::kNone:
      break;
  }
  return KeyFailure::kInternal;
}

}  // namespace

class ProductTextController::ClientProvider final
    : public coordinator::ObservedEngineClients {
 public:
  explicit ClientProvider(engine::EngineExecutor& executor) : executor_(executor) {}

  engine::ClientId NextClient() override {
    engine::RequestId request;
    engine::ClientId client;
    const engine::SubmitStatus submitted = executor_.TryCreateClient(
        kProductSchema,
        engine::ControllerOptions{true, input::InputPermission::kAllowed}, &request,
        &client);
    ++requests_;
    if (submitted != engine::SubmitStatus::kAccepted) {
      ++refusals_;
      return engine::ClientId{};
    }
    pending_.push_back(request);
    last_client_ = client;
    return client;
  }

  struct Harvest {
    bool failed = false;
    engine::ClientId failed_client;
  };

  Harvest TakeReady() {
    Harvest harvest;
    std::vector<engine::RequestId> remaining;
    for (const engine::RequestId& request : pending_) {
      engine::CommandResult result;
      const engine::TakeStatus taken = executor_.TakeResult(request, &result);
      if (taken == engine::TakeStatus::kTaken) {
        ++results_;
        if (!result.ok()) {
          ++failures_;
          harvest.failed = true;
          harvest.failed_client = result.client;
        }
      } else if (taken == engine::TakeStatus::kNotReady) {
        remaining.push_back(request);
      }
    }
    pending_.swap(remaining);
    return harvest;
  }

  std::uint64_t requests() const { return requests_; }
  std::uint64_t results() const { return results_; }
  std::uint64_t failures() const { return failures_; }
  std::size_t pending() const { return pending_.size(); }
  engine::ClientId last_client() const { return last_client_; }

 private:
  engine::EngineExecutor& executor_;
  std::vector<engine::RequestId> pending_;
  engine::ClientId last_client_{};
  std::uint64_t requests_ = 0;
  std::uint64_t results_ = 0;
  std::uint64_t failures_ = 0;
  std::uint64_t refusals_ = 0;
};

ProductTextController::ProductTextController()
    : generation_(g_next_generation.fetch_add(1)) {}

ProductTextController::~ProductTextController() {
  // The normal path is Stop(). Reaching the destructor with started_ true means the host
  // violated Deactivate-before-Release; do not join threads or call COM here.
}

bool ProductTextController::Start(const Options& options, std::wstring* why) {
  if (started_ || stopping_) {
    if (why != nullptr) {
      *why = L"the product controller is already started";
    }
    return false;
  }
  if (options.thread_manager == nullptr || options.client_id == 0) {
    if (why != nullptr) {
      *why = L"the product controller needs a thread manager and client id";
    }
    return false;
  }
  thread_manager_ = options.thread_manager;
  client_id_ = options.client_id;
  // Every COM object this layer hands to TSF (edit-session callbacks, composition sinks,
  // candidate elements) counts against DllCanUnloadNow. TSF may still own one after
  // Deactivate, and unloading then would leave it a vtable in an unmapped image.
  edit::SetModuleCounters(&ModuleAddObject, &ModuleReleaseObject);

  // The wake channel exists before the engine does: the asynchronous bootstrap posts the
  // same generation-only message, and the first pump that observes readiness builds the
  // engine pipeline. No engine event is installed yet.
  if (!waker_.Start(this, nullptr, ModuleInstance(), generation_, why)) {
    return false;
  }
  bootstrap_wake_ = std::make_shared<BootstrapWake>();
  bootstrap_wake_->waker = &waker_;
  std::weak_ptr<BootstrapWake> weak_wake = bootstrap_wake_;
  bootstrap_waiter_ = ProductEngineBootstrap::Instance().AddWaiter([weak_wake]() {
    const std::shared_ptr<BootstrapWake> wake = weak_wake.lock();
    if (!wake) {
      return;
    }
    std::lock_guard<std::mutex> guard(wake->mutex);
    if (wake->waker != nullptr) {
      wake->waker->Notify();
    }
  });
  started_ = true;
  candidate_ui_.reset(new CandidateUi(client_id_, this, ModuleInstance()));
  std::wstring candidate_why;
  if (!candidate_ui_->Start(thread_manager_, &candidate_why)) {
    candidate_ui_.reset();
  }
  TryAttachEngine();
  PumpNow();
  return true;
}

void ProductTextController::RequestEngineStart() {
  if (!started_ || stopping_) {
    return;
  }
  ProductEngineBootstrap::Instance().RequestStart();
}

void ProductTextController::ResetObjects() {
  binding_.reset();
  if (observer_) {
    observer_->Shutdown();
  }
  if (coordinator_) {
    coordinator_->Close();
  }
  if (ui_context_ != nullptr) {
    ui_context_->Release();
    ui_context_ = nullptr;
  }
  if (candidate_ui_) {
    candidate_ui_->End();
  }
  candidate_selection_ = -1;
  candidate_ui_version_ = 0;
  coordinator_.reset();
  observer_.reset();
  permission_.reset();
  clients_.reset();
  registry_.reset();
  subscribed_ = false;
}

void ProductTextController::TryAttachEngine() {
  if (!started_ || stopping_ || engine_ || attaching_) {
    return;
  }
  attaching_ = true;
  ++engine_attach_attempts_;
  std::shared_ptr<ProductEngineAttachment> attachment;
  bool engine_running = false;
  const bool took_pending =
      ProductEngineBootstrap::Instance().TakeAttachment(&attachment, &engine_running);
  if (!took_pending && engine_running) {
    std::wstring acquire_why;
    if (!ProductEngineBootstrap::Instance().AcquireRunning(&attachment, &acquire_why)) {
      engine_failure_ = std::string(acquire_why.begin(), acquire_why.end());
      attaching_ = false;
      return;
    }
  }
  if (!attachment) {
    attaching_ = false;
    return;
  }
  engine_ = std::move(attachment);
  std::wstring build_why;
  if (!BuildEnginePipeline(&build_why)) {
    ++engine_attach_failures_;
    engine_failure_ = std::string(build_why.begin(), build_why.end());
    ResetObjects();
    std::shared_ptr<ProductEngineAttachment> failed = std::move(engine_);
    attaching_ = false;
    ReleaseAttachmentAsync(std::move(failed));
    return;
  }
  engine_failure_.clear();
  attaching_ = false;
  PumpNow();
}

bool ProductTextController::BuildEnginePipeline(std::wstring* why) {
  if (!engine_ || engine_->executor() == nullptr) {
    if (why != nullptr) {
      *why = L"the engine attachment has no executor";
    }
    return false;
  }
  engine::EngineExecutor* executor = engine_->executor();
  registry_.reset(new ctx::ContextRegistry(8));
  permission_ = ctx::CreateRealPermissionSource(client_id_);
  coordinator_.reset(
      new coordinator::ContextCoordinator(*executor, client_id_, *permission_));
  ctx::ThreadFocusPolicy focus_policy = ctx::ThreadFocusPolicy::kStrictTsf;
#ifdef EPOCANVA_TSF_DIRECT_SINK_TEST
  // Controlled direct-sink tests run in a headless STA with no OS foreground focus. The
  // product DLL (which never defines this macro) keeps the StrictTsf policy.
  focus_policy = ctx::ThreadFocusPolicy::kLabWin32Fallback;
#endif
  observer_.reset(
      new ctx::ContextObserver(*registry_, client_id_, focus_policy));
  clients_.reset(new ClientProvider(*executor));
  binding_.reset(new coordinator::ObservedBindingController(
      *observer_, *coordinator_, *registry_, *clients_));

  std::string subscribe_error;
  if (!observer_->Subscribe(thread_manager_, this, &subscribe_error)) {
    if (why != nullptr) {
      *why = std::wstring(subscribe_error.begin(), subscribe_error.end());
    }
    return false;
  }
  subscribed_ = true;
  // Engine results now arrive through the existing watcher thread; the window is unchanged.
  waker_.SetExecutorEvent(executor->ResultReadyEvent());
  return true;
}

void ProductTextController::Stop() {
  if (!started_ && !binding_ && !engine_) {
    return;
  }
  stopping_ = true;
  if (bootstrap_waiter_ != 0) {
    ProductEngineBootstrap::Instance().RemoveWaiter(bootstrap_waiter_);
    bootstrap_waiter_ = 0;
  }
  if (bootstrap_wake_) {
    std::lock_guard<std::mutex> guard(bootstrap_wake_->mutex);
    bootstrap_wake_->waker = nullptr;
  }
  // Unregister the wake target before any object it can reach is destroyed.
  waker_.Stop();
  if (candidate_ui_) {
    candidate_ui_->Stop();
  }
  FailRawState(KeyFailure::kShutdown);
  queue_.FailQueued(KeyFailure::kShutdown);
  if (queue_.HasInFlight()) {
    queue_.MarkInFlightFailed(KeyFailure::kShutdown);
  }
  ResetObjects();
  queue_.ReleaseFailedInFlight();
  candidate_ui_.reset();
  if (engine_) {
    // Stopping the engine can join its worker. The STA must not carry that wait, so the
    // lease release is detached; the thread holds a module reference while it runs.
    ReleaseAttachmentAsync(std::move(engine_));
  }
  started_ = false;
  stopping_ = false;
}

bool ProductTextController::BuildKey(WPARAM wparam, LPARAM lparam, bool key_down,
                                     BuiltKey* key) const {
  if (!key_down || key == nullptr) {
    return false;
  }
  if (static_cast<std::uint32_t>(wparam) == kVirtualKeyProcessKey) {
    return false;
  }
  const input::KeyboardStateCaptureResult captured =
      input::CaptureThreadKeyboardState();
  if (!captured.ok()) {
    return false;
  }
  key->event = EventFromTsf(wparam, lparam);
  key->event.modifiers = captured.snapshot.modifiers();
  const input::KeyTranslationResult translation =
      input::TranslateKeyEvent(key->event, captured.snapshot);
  key->text = translation.text;
  if (coordinator_) {
    key->decision = coordinator_->Evaluate(key->event, key->text);
    const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
    key->composition_active =
        snapshot.composition_active || snapshot.owned_composition_active;
  }
  return true;
}

void ProductTextController::TrackLiteralKey(WPARAM wparam, LPARAM lparam) {
  BuiltKey key;
  if (!BuildKey(wparam, lparam, true, &key)) {
    return;
  }
  const bool shortcut = key.event.modifiers.control || key.event.modifiers.alt ||
                        key.event.modifiers.windows;
  host_token_.HostKept(key.text, shortcut);
  if (host_token_.owned() && !shortcut) {
    ++token_host_keys_;
  }
}

bool ProductTextController::AdmissionAvailable(bool composition_active) const {
  return queue_.CanAccept(composition_active);
}

bool ProductTextController::IsTextKey(const BuiltKey& key) const {
  return key.text.status == input::KeyTextStatus::kText &&
         !key.text.utf16_code_units.empty();
}

bool ProductTextController::IsTokenText(const BuiltKey& key) const {
  return HostTokenOwnership::IsTokenText(key.text);
}

bool ProductTextController::RawCaptureWorthy(const BuiltKey& key) const {
  if (!IsTextKey(key)) {
    return false;
  }
  switch (key.decision.reason) {
    case input::KeyDecisionReason::kPermissionForbidden:
    case input::KeyDecisionReason::kPermissionUnknown:
    case input::KeyDecisionReason::kInputDisabled:
    case input::KeyDecisionReason::kShortcutModifier:
    case input::KeyDecisionReason::kKeyRelease:
    case input::KeyDecisionReason::kNoText:
    case input::KeyDecisionReason::kDeadKey:
    case input::KeyDecisionReason::kTranslationFailed:
    case input::KeyDecisionReason::kUnsupportedModifierCombination:
      return false;
    default:
      // kEngineNotReady is deliberately capturable: a raw commit needs no engine call, so
      // the degraded run can still apply the text.
      return true;
  }
}

bool ProductTextController::RawUnapplied() const {
  return raw_active_ || raw_submitted_ || !raw_run_.empty();
}

bool ProductTextController::HasPendingEngineInput() const {
  return queue_.HasInFlight() || !queue_.empty();
}

std::size_t ProductTextController::QueueDepth() const {
  return queue_.size() + (queue_.HasInFlight() ? 1 : 0);
}

bool ProductTextController::PredictKey(WPARAM wparam, LPARAM lparam) {
  if (!started_ || stopping_) {
    return false;
  }
  if (!engine_ || !coordinator_) {
    // Literal mode: the key keeps the host's own effect. Prediction stays FALSE, exactly
    // like the eventual HandleKeyDown answer.
    return false;
  }
  ++predictions_;
  BuiltKey key;
  if (!BuildKey(wparam, lparam, true, &key)) {
    return false;
  }
  // Candidate navigation is owned by this controller while a real menu is open: the host
  // must not move its caret, and no engine key is involved.
  const std::uint32_t vk = key.event.virtual_key;
  const bool plain_arrow = (vk == VK_UP || vk == VK_DOWN) &&
                           !key.event.modifiers.control && !key.event.modifiers.alt &&
                           !key.event.modifiers.shift && !key.event.modifiers.windows;
  if (plain_arrow && CandidatesOpen()) {
    return true;
  }
  const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
  if (!snapshot.bound || snapshot.closed ||
      snapshot.gate != coordinator::CoordinatorGate::kOpen) {
    return false;
  }
  if (queue_.HasInFlight() && queue_.in_flight_failed()) {
    return false;
  }
  // A token the host already started is never handed to the engine mid-token. The test is
  // on the character class, not on the decision, because the permission verdict can change
  // between two characters of the same token.
  if (host_token_.owned() && IsTokenText(key)) {
    return false;
  }
  if (RawUnapplied()) {
    // The degraded run eats captured text and the four special keys; shortcuts, privacy
    // refusals and untranslatable keys stay with the host. This must agree exactly with
    // HandleKeyDown.
    const std::uint32_t vk = key.event.virtual_key;
    if (key.decision.reason != input::KeyDecisionReason::kPermissionForbidden &&
        key.decision.reason != input::KeyDecisionReason::kPermissionUnknown &&
        key.decision.reason != input::KeyDecisionReason::kInputDisabled &&
        key.decision.disposition != input::KeyDisposition::kShortcutPassThrough &&
        (vk == input::kVirtualKeySpace || vk == input::kVirtualKeyReturn ||
         vk == input::kVirtualKeyEscape || vk == input::kVirtualKeyBack)) {
      return true;
    }
    return RawCaptureWorthy(key);
  }
  if (!key.decision.is_engine_candidate()) {
    // Translated text the decision layer does not offer to the engine is still eaten when
    // earlier input is pending, because a host insertion now would be placed after text the
    // engine has not applied yet. HandleKeyDown enters the raw catch-up for it.
    if (HasPendingEngineInput() && RawCaptureWorthy(key)) {
      return true;
    }
    return false;
  }
  if (QueueDepth() >= KeyAdmissionQueue::kHighWater) {
    return true;  // the degraded catch-up eats it
  }
  if (!AdmissionAvailable(key.composition_active)) {
    // Full without pending input is a safe pass-through. Full with pending input has no safe
    // pass-through; the key is eaten into the degraded run.
    if (!HasPendingEngineInput() && !key.composition_active) {
      return false;
    }
    return true;
  }
  ++predicted_eaten_;
  return true;
}

bool ProductTextController::HandleKeyDown(WPARAM wparam, LPARAM lparam,
                                          std::wstring* why) {
  (void)why;
  if (!started_ || stopping_) {
    return false;
  }
  ++key_downs_;
  if (!engine_ || !coordinator_) {
    // Literal mode while the engine starts, is unavailable, or failed. Every key keeps the
    // host's own default effect: no queue, no composition, no edit, nothing reordered. The
    // host-owned token is tracked so a later attach cannot take over mid-word.
    ++literal_key_downs_;
    TrackLiteralKey(wparam, lparam);
    return false;
  }
  BuiltKey key;
  if (!BuildKey(wparam, lparam, true, &key)) {
    ++not_eaten_;
    return false;
  }
  const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
  if (!snapshot.bound || snapshot.closed ||
      snapshot.gate != coordinator::CoordinatorGate::kOpen ||
      (queue_.HasInFlight() && queue_.in_flight_failed())) {
    // No live binding. Captured text cannot be applied to a context it was not typed into,
    // so it is reported as an explicit failure rather than replayed elsewhere.
    if (RawUnapplied()) {
      FailRawState(KeyFailure::kFocusChanged);
    }
    host_token_.HostKept(
        key.text, key.decision.disposition == input::KeyDisposition::kShortcutPassThrough);
    if (host_token_.owned()) {
      ++token_host_keys_;
    }
    ++not_eaten_;
    return false;
  }
  // W2-B candidate navigation and acceptance. Arrow keys never reach the host while a real
  // menu is open; Enter with an explicit selection is a validated selection, not an engine
  // Return. Space is deliberately not intercepted: it keeps the existing literal-commit
  // contract. Escape dismisses the menu and then follows the normal engine Escape path.
  const std::uint32_t vk = key.event.virtual_key;
  const bool plain_arrow = (vk == VK_UP || vk == VK_DOWN) &&
                           !key.event.modifiers.control && !key.event.modifiers.alt &&
                           !key.event.modifiers.shift && !key.event.modifiers.windows;
  if (CandidatesOpen() && plain_arrow) {
    MoveCandidateSelection(vk == VK_DOWN ? 1 : -1);
    host_token_.EngineKept();
    ++eaten_;
    return true;
  }
  if (CandidatesOpen() && vk == VK_RETURN && candidate_selection_ >= 0) {
    const coordinator::CandidateSnapshot snapshot = coordinator_->candidate_snapshot();
    CandidateUiSelection selection;
    selection.snapshot_version = snapshot.version;
    selection.context_id = snapshot.identity.context_id;
    selection.generation = snapshot.identity.generation;
    selection.page_index = candidate_selection_;
    (void)FinalizeCandidateSelection(selection);
    host_token_.EngineKept();
    ++eaten_;
    return true;
  }
  if (CandidatesOpen() && vk == VK_ESCAPE) {
    // The window closes here; the composition is cancelled through the engine path below,
    // which also clears the preedit. No captured text is written after this point.
    candidate_dismissed_version_ = coordinator_->candidate_snapshot().version;
    candidate_selection_ = -1;
    if (candidate_ui_) {
      candidate_ui_->SetSelection(-1);
      candidate_ui_->End();
    }
  }
  if (host_token_.owned() && IsTokenText(key)) {
    ++token_host_keys_;
    ++not_eaten_;
    return false;
  }
  if (RawUnapplied()) {
    ++raw_reason_unapplied_;
    if (HandleRawKey(key)) {
      host_token_.EngineKept();
      ++eaten_;
      waker_.SetPacing(true);
      return true;
    }
    const input::KeyDecisionReason reason = key.decision.reason;
    const bool shortcut =
        key.decision.disposition == input::KeyDisposition::kShortcutPassThrough;
    if (reason == input::KeyDecisionReason::kPermissionForbidden ||
        reason == input::KeyDecisionReason::kPermissionUnknown ||
        reason == input::KeyDecisionReason::kInputDisabled) {
      // The field changed class while a raw run was pending. Nothing captured may be written
      // there any more; the loss is reported and the host gets its own key.
      FailRawState(KeyFailure::kPermissionRefused);
      host_token_.HostKept(key.text, shortcut);
    } else if (!shortcut) {
      // A shortcut does not end a word. Every other non-captured key does.
      host_token_.HostKept(key.text, false);
    }
    if (host_token_.owned() && IsTokenText(key)) {
      ++token_host_keys_;
    }
    ++not_eaten_;
    return false;
  }
  if (!key.decision.is_engine_candidate()) {
    if (HasPendingEngineInput() && RawCaptureWorthy(key)) {
      // Earlier input is still queued for the engine; letting the host insert this character
      // now would place it before text that has not been applied yet. Capture it in order.
      ++raw_reason_not_candidate_;
      EnterRawMode();
      if (HandleRawKey(key)) {
        ++eaten_;
        waker_.SetPacing(true);
        return true;
      }
    }
    // A shortcut, key-up-shaped event, or untranslatable key is never admitted. Translated
    // text here is a genuine pass-through: the host owns the token and keeps owning it.
    const bool shortcut =
        key.decision.disposition == input::KeyDisposition::kShortcutPassThrough;
    host_token_.HostKept(key.text, shortcut);
    if (host_token_.owned() && !shortcut) {
      ++token_host_keys_;
    }
    ++not_eaten_;
    return false;
  }
  if (QueueDepth() >= KeyAdmissionQueue::kHighWater) {
    // High water: the queued tail moves to the raw run before the engine can stall further.
    ++raw_reason_high_water_;
    EnterRawMode();
    if (HandleRawKey(key)) {
      host_token_.EngineKept();
      ++eaten_;
      waker_.SetPacing(true);
      return true;
    }
  }
  if (!AdmissionAvailable(key.composition_active)) {
    if (!HasPendingEngineInput() && !key.composition_active) {
      host_token_.HostKept(key.text, /*shortcut=*/false);
      ++token_host_keys_;
      ++not_eaten_;
      return false;
    }
    ++raw_reason_admission_;
    EnterRawMode();
    if (HandleRawKey(key)) {
      host_token_.EngineKept();
      ++eaten_;
      waker_.SetPacing(true);
      return true;
    }
  }

  const bool was_empty = queue_.empty() && !queue_.HasInFlight();
  const std::uint64_t id = queue_.Push(key.event, key.text, key.decision,
                                       snapshot.identity.context_id,
                                       snapshot.identity.generation);
  const SubmitAttempt attempt =
      was_empty ? SubmitHead(true) : SubmitAttempt::kWaited;
  if (attempt == SubmitAttempt::kNotEatenHostKeeps) {
    // The key was never queued (SubmitHead dropped it) and no earlier input is pending, so
    // the host owns this token from its first character.
    host_token_.HostKept(key.text, /*shortcut=*/false);
    ++token_host_keys_;
    ++not_eaten_;
    return false;
  }
  if (attempt == SubmitAttempt::kFailedEaten) {
    host_token_.EngineKept();
    ++eaten_;
    waker_.SetPacing(true);
    return true;
  }
  queue_.ConfirmEaten(id);
  host_token_.EngineKept();
  ++eaten_;
  waker_.SetPacing(true);
  return true;
}

void ProductTextController::EnterRawMode() {
  if (raw_active_) {
    return;
  }
  raw_active_ = true;
  ++raw_entered_;
  host_token_.Reset();
  std::deque<AdmissionKey> withdrawn = queue_.WithdrawQueued();
  const std::size_t before = raw_run_.size();
  for (const AdmissionKey& key : withdrawn) {
    FoldWithdrawnKey(key);
  }
  queue_.RecordDeferredRaw(withdrawn.size(), raw_run_.size() - before);
}

void ProductTextController::FoldWithdrawnKey(const AdmissionKey& key) {
  switch (key.decision.disposition) {
    case input::KeyDisposition::kEngineTextCandidate:
    case input::KeyDisposition::kEngineSpaceCandidate:
      CaptureRawText(key.decision.disposition == input::KeyDisposition::kEngineSpaceCandidate
                         ? std::u16string_view(u" ")
                         : std::u16string_view(key.text.utf16_code_units));
      break;
    case input::KeyDisposition::kEngineEnterCandidate:
      raw_end_requested_ = true;
      ++raw_chunk_keys_;
      break;
    case input::KeyDisposition::kEngineEscapeCandidate:
      // Escape inside an already-withdrawn run cancels only what is not yet submitted.
      CancelRawSuffix();
      raw_end_requested_ = true;
      ++raw_chunk_keys_;
      break;
    case input::KeyDisposition::kEngineBackSpaceCandidate:
      if (!raw_run_.BackspaceOneCodePoint()) {
        ++raw_ignored_edits_;
      }
      ++raw_chunk_keys_;
      break;
    default:
      ++raw_ignored_edits_;
      break;
  }
}

bool ProductTextController::CaptureRawText(std::u16string_view text) {
  if (text.empty()) {
    return true;
  }
  RawTextRun::AppendStatus status = raw_run_.Append(text, kRawCapacityUtf16);
  if (status == RawTextRun::AppendStatus::kNeedsFlush) {
    // Hand the full chunk to the coordinator and retry this key in a new chunk. This is
    // what keeps the bound from turning into a loss while the document is still draining.
    FlushRaw();
    if (raw_submitted_) {
      status = raw_run_.Append(text, kRawCapacityUtf16);
    }
  }
  if (status != RawTextRun::AppendStatus::kAppended) {
    // The current chunk could not be flushed (an earlier chunk or engine key is still
    // unresolved). The key is still eaten so the text that IS captured keeps its order; the
    // loss is reported as an explicit unrecoverable capacity failure, never hidden.
    queue_.RecordRawDropped(1, text.size());
    ++raw_unrecoverable_;
    return false;
  }
  ++raw_chunk_keys_;
  ++raw_keys_;
  raw_chars_ += text.size();
  return true;
}

void ProductTextController::CancelRawSuffix() {
  if (raw_run_.empty()) {
    return;
  }
  raw_cancelled_keys_ += raw_chunk_keys_;
  raw_cancelled_units_ += raw_run_.size();
  queue_.RecordRawCancelled(raw_chunk_keys_, raw_run_.size());
  raw_run_.Clear();
  raw_chunk_keys_ = 0;
}

bool ProductTextController::HandleRawKey(const BuiltKey& key) {
  switch (key.decision.reason) {
    case input::KeyDecisionReason::kPermissionForbidden:
    case input::KeyDecisionReason::kPermissionUnknown:
    case input::KeyDecisionReason::kInputDisabled:
    case input::KeyDecisionReason::kShortcutModifier:
    case input::KeyDecisionReason::kKeyRelease:
      // Never captured: privacy, disabled input or a host-owned chord.
      return false;
    default:
      break;
  }
  const std::uint32_t vk = key.event.virtual_key;
  if (vk == input::kVirtualKeySpace) {
    CaptureRawText(u" ");
    raw_end_requested_ = true;
    host_token_.Reset();
    FlushRaw();
    return true;
  }
  if (vk == input::kVirtualKeyReturn) {
    // Enter ends the run and applies the captured suffix; it inserts no newline, exactly as
    // the engine path commits a composition without adding a line break.
    raw_end_requested_ = true;
    ++raw_chunk_keys_;
    FlushRaw();
    return true;
  }
  if (vk == input::kVirtualKeyEscape) {
    // Escape cancels the accepted-but-not-yet-submitted suffix. A chunk already handed to
    // the coordinator before this key is not retracted (there is no safe cancel for a TSF
    // write that may be in flight); it applies exactly once. Nothing captured earlier is
    // submitted later: the suffix is cleared here and the run ends when the chunk resolves.
    CancelRawSuffix();
    raw_end_requested_ = true;
    ++raw_chunk_keys_;
    ++raw_ignored_edits_;
    FlushRaw();
    return true;
  }
  if (vk == input::kVirtualKeyBack) {
    // Backspace edits only the captured suffix. It never deletes displayed composition text
    // or host text: when the suffix is empty, the key is eaten and counted, not forwarded.
    if (!raw_run_.BackspaceOneCodePoint()) {
      ++raw_ignored_edits_;
    }
    ++raw_chunk_keys_;
    FlushRaw();
    return true;
  }
  if (RawCaptureWorthy(key)) {
    if (!IsTokenText(key)) {
      // e.g. a comma the engine would not consume: still captured raw, in order.
      host_token_.Reset();
    }
    CaptureRawText(key.text.utf16_code_units);
    FlushRaw();
    return true;
  }
  return false;  // shortcuts and untranslatable keys stay with the host
}

void ProductTextController::FlushRaw() {
  if (!raw_active_ || raw_submitted_ || raw_run_.empty()) {
    return;
  }
  if (HasPendingEngineInput()) {
    return;  // the in-flight engine key must resolve before the raw run can be applied
  }
  const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
  if (!snapshot.bound || snapshot.closed ||
      snapshot.gate != coordinator::CoordinatorGate::kOpen ||
      snapshot.phase != coordinator::WorkPhase::kIdle ||
      snapshot.retirement.retiring_bindings != 0 ||
      snapshot.retirement.admission_paused) {
    return;
  }
  if (snapshot.permission != edit::InputPermission::kAllowed) {
    return;  // a pending refresh resolves; nothing is written under Unknown/Denied
  }
  coordinator::WorkId work;
  const coordinator::SubmitStatus status =
      coordinator_->SubmitRawCommit(raw_run_.text(), &work);
  switch (status) {
    case coordinator::SubmitStatus::kAccepted:
      raw_submitted_ = true;
      raw_work_id_ = work.value;
      raw_submitted_keys_ = raw_chunk_keys_;
      raw_submitted_units_ = raw_run_.size();
      raw_run_.Clear();
      raw_chunk_keys_ = 0;
      ++raw_commits_;
      waker_.SetPacing(true);
      return;
    case coordinator::SubmitStatus::kBusy:
    case coordinator::SubmitStatus::kRetiring:
    case coordinator::SubmitStatus::kPermissionUnknown:
      return;  // retried by the next pump
    case coordinator::SubmitStatus::kPermissionDenied:
      FailRawState(KeyFailure::kPermissionRefused);
      return;
    case coordinator::SubmitStatus::kClosed:
      FailRawState(KeyFailure::kShutdown);
      return;
    case coordinator::SubmitStatus::kNotBound:
    case coordinator::SubmitStatus::kTargetChanged:
    case coordinator::SubmitStatus::kThreadFocusLost:
    case coordinator::SubmitStatus::kTargetUnverified:
    case coordinator::SubmitStatus::kBlockedUncertain:
    case coordinator::SubmitStatus::kBlockedNeedsResync:
    case coordinator::SubmitStatus::kBlockedUnsupportedShape:
      FailRawState(KeyFailure::kTargetChanged);
      if (coordinator_->bound()) {
        coordinator_->InvalidateBinding();
      }
      RequestRebindSoon();
      return;
    case coordinator::SubmitStatus::kEngineNotRunning:
    case coordinator::SubmitStatus::kQueueFull:
    case coordinator::SubmitStatus::kEngineClientUnavailable:
    case coordinator::SubmitStatus::kEngineSubmitFailed:
    case coordinator::SubmitStatus::kNotEngineCandidate:
    case coordinator::SubmitStatus::kInvalidRequest:
      FailRawState(KeyFailure::kInternal);
      return;
  }
}

void ProductTextController::ResolveRaw() {
  if (raw_submitted_) {
    const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
    if (snapshot.work_id != raw_work_id_) {
      FailRawState(KeyFailure::kRawCommitFailed);
      if (queue_.HasInFlight() && !queue_.in_flight_failed()) {
        queue_.MarkInFlightFailed(KeyFailure::kRawCommitFailed);
      }
    } else if (snapshot.phase == coordinator::WorkPhase::kIdle) {
      const bool applied = snapshot.outcome == coordinator::WorkOutcome::kApplied;
      if (applied) {
        queue_.RecordRawApplied(raw_submitted_keys_, raw_submitted_units_);
        engine_stale_ = true;
        raw_submitted_ = false;
        raw_work_id_ = 0;
        raw_submitted_keys_ = 0;
        raw_submitted_units_ = 0;
      } else {
        // FailRawState records exactly this submitted chunk and any later captured text.
        FailRawState(KeyFailure::kRawCommitFailed);
        if (coordinator_->bound()) {
          coordinator_->InvalidateBinding();
        }
        RequestRebindSoon();
      }
      waker_.SetPacing(true);
    }
  }
  FlushRaw();
  if (raw_active_ && !raw_submitted_ && raw_run_.empty() &&
      !HasPendingEngineInput() && raw_end_requested_) {
    FinishRawRun();
  }
}

void ProductTextController::FinishRawRun() {
  raw_active_ = false;
  raw_end_requested_ = false;
  ++raw_exits_;
  if (engine_stale_) {
    // The engine client never saw the captured keys. It must not be used again on this
    // binding; a fresh client is built for the same context by the normal rebind path.
    if (coordinator_->bound()) {
      coordinator_->InvalidateBinding();
    }
    RequestRebindSoon();
  }
  waker_.SetPacing(true);
}

void ProductTextController::FailRawState(KeyFailure failure) {
  if (raw_submitted_) {
    queue_.RecordRawFailed(raw_submitted_keys_, raw_submitted_units_);
  }
  if (raw_chunk_keys_ != 0 || !raw_run_.empty()) {
    queue_.RecordRawFailed(raw_chunk_keys_, raw_run_.size());
  }
  raw_submitted_ = false;
  raw_work_id_ = 0;
  raw_submitted_keys_ = 0;
  raw_submitted_units_ = 0;
  raw_chunk_keys_ = 0;
  raw_run_.Clear();
  raw_end_requested_ = false;
  raw_active_ = false;
  (void)failure;
  waker_.SetPacing(true);
}

ProductTextController::SubmitAttempt ProductTextController::SubmitHead(bool immediate) {
  if (RawUnapplied()) {
    // While a degraded run owns the caret order, no engine key may overtake it.
    return SubmitAttempt::kWaited;
  }
  AdmissionKey* head = queue_.head();
  if (head == nullptr) {
    return SubmitAttempt::kNoHead;
  }
  const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
  const bool composing =
      snapshot.composition_active || snapshot.owned_composition_active;
  auto keep = [this, immediate, composing]() {
    if (immediate && !composing) {
      queue_.DropHeadNotEaten();
      return SubmitAttempt::kNotEatenHostKeeps;
    }
    return SubmitAttempt::kWaited;
  };

  if (snapshot.closed) {
    queue_.FailHead(KeyFailure::kShutdown, true);
    queue_.FailQueued(KeyFailure::kShutdown);
    return SubmitAttempt::kFailedEaten;
  }
  if (!snapshot.bound || snapshot.gate != coordinator::CoordinatorGate::kOpen) {
    if (immediate && !composing) {
      queue_.DropHeadNotEaten();
      queue_.FailQueued(KeyFailure::kBlockedGate);
      RequestRebindSoon();
      return SubmitAttempt::kNotEatenHostKeeps;
    }
    queue_.FailHead(KeyFailure::kBlockedGate, true);
    queue_.FailQueued(KeyFailure::kBlockedGate);
    coordinator_->InvalidateBinding();
    RequestRebindSoon();
    return SubmitAttempt::kFailedEaten;
  }
  if (head->context_id != snapshot.identity.context_id ||
      head->generation != snapshot.identity.generation) {
    if (immediate && !composing) {
      queue_.DropHeadNotEaten();
      queue_.FailQueued(KeyFailure::kBindingRetired);
      RequestRebindSoon();
      return SubmitAttempt::kNotEatenHostKeeps;
    }
    queue_.FailHead(KeyFailure::kBindingRetired, true);
    queue_.FailQueued(KeyFailure::kBindingRetired);
    coordinator_->InvalidateBinding();
    RequestRebindSoon();
    return SubmitAttempt::kFailedEaten;
  }
  if (snapshot.phase != coordinator::WorkPhase::kIdle ||
      snapshot.retirement.retiring_bindings != 0 ||
      snapshot.retirement.admission_paused) {
    return SubmitAttempt::kWaited;
  }

  coordinator::WorkId work;
  const coordinator::SubmitStatus submitted =
      coordinator_->SubmitKey(head->event, head->text, &work);
  switch (submitted) {
    case coordinator::SubmitStatus::kAccepted:
      if (!queue_.MoveHeadToInFlight(work)) {
        return SubmitAttempt::kWaited;
      }
      return SubmitAttempt::kSubmitted;
    case coordinator::SubmitStatus::kBusy:
    case coordinator::SubmitStatus::kRetiring:
    case coordinator::SubmitStatus::kQueueFull:
      return SubmitAttempt::kWaited;
    case coordinator::SubmitStatus::kNotEngineCandidate:
      if (immediate && !composing) {
        return keep();
      }
      queue_.FailHead(KeyFailure::kPredictionMismatch, true);
      return SubmitAttempt::kFailedEaten;
    case coordinator::SubmitStatus::kPermissionDenied:
    case coordinator::SubmitStatus::kPermissionUnknown:
      if (immediate && !composing) {
        return keep();
      }
      queue_.FailHead(KeyFailure::kPermissionRefused, true);
      if (composing) {
        coordinator_->InvalidateBinding();
        RequestRebindSoon();
      }
      return SubmitAttempt::kFailedEaten;
    case coordinator::SubmitStatus::kThreadFocusLost:
    case coordinator::SubmitStatus::kTargetChanged:
    case coordinator::SubmitStatus::kTargetUnverified:
    case coordinator::SubmitStatus::kNotBound:
      if (immediate && !composing) {
        queue_.DropHeadNotEaten();
        queue_.FailQueued(KeyFailure::kTargetChanged);
        RequestRebindSoon();
        return SubmitAttempt::kNotEatenHostKeeps;
      }
      queue_.FailHead(KeyFailure::kTargetChanged, true);
      queue_.FailQueued(KeyFailure::kTargetChanged);
      coordinator_->InvalidateBinding();
      RequestRebindSoon();
      return SubmitAttempt::kFailedEaten;
    case coordinator::SubmitStatus::kBlockedUncertain:
    case coordinator::SubmitStatus::kBlockedNeedsResync:
    case coordinator::SubmitStatus::kBlockedUnsupportedShape:
      if (immediate && !composing) {
        queue_.DropHeadNotEaten();
        queue_.FailQueued(KeyFailure::kBlockedGate);
        RequestRebindSoon();
        return SubmitAttempt::kNotEatenHostKeeps;
      }
      queue_.FailHead(KeyFailure::kBlockedGate, true);
      queue_.FailQueued(KeyFailure::kBlockedGate);
      coordinator_->InvalidateBinding();
      RequestRebindSoon();
      return SubmitAttempt::kFailedEaten;
    case coordinator::SubmitStatus::kEngineNotRunning:
    case coordinator::SubmitStatus::kEngineClientUnavailable:
    case coordinator::SubmitStatus::kEngineSubmitFailed:
      if (immediate && !composing) {
        queue_.DropHeadNotEaten();
        queue_.FailQueued(KeyFailure::kEngineUnavailable);
        RequestRebindSoon();
        return SubmitAttempt::kNotEatenHostKeeps;
      }
      queue_.FailHead(KeyFailure::kEngineUnavailable, true);
      queue_.FailQueued(KeyFailure::kEngineUnavailable);
      coordinator_->InvalidateBinding();
      RequestRebindSoon();
      return SubmitAttempt::kFailedEaten;
    case coordinator::SubmitStatus::kClosed:
      queue_.FailHead(KeyFailure::kShutdown, true);
      queue_.FailQueued(KeyFailure::kShutdown);
      return SubmitAttempt::kFailedEaten;
    case coordinator::SubmitStatus::kInvalidRequest:
      queue_.FailHead(KeyFailure::kInternal, true);
      return SubmitAttempt::kFailedEaten;
  }
  return SubmitAttempt::kWaited;
}

void ProductTextController::FailQueuedAndMaybeInvalidate(KeyFailure failure,
                                                         bool invalidate) {
  queue_.FailQueued(failure);
  if (invalidate && coordinator_->bound()) {
    coordinator_->InvalidateBinding();
    RequestRebindSoon();
  }
}

void ProductTextController::RequestRebindSoon() { rebind_soon_ = true; }

void ProductTextController::ResolveInFlight() {
  if (!queue_.HasInFlight()) {
    return;
  }
  const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
  if (queue_.in_flight_failed()) {
    if (snapshot.phase == coordinator::WorkPhase::kIdle) {
      queue_.ReleaseFailedInFlight();
    }
    return;
  }
  AdmissionKey* key = queue_.in_flight();
  if (key == nullptr || snapshot.work_id != key->work.value) {
    return;
  }
  if (snapshot.phase == coordinator::WorkPhase::kEditPending ||
      (snapshot.phase == coordinator::WorkPhase::kIdle &&
       (snapshot.outcome == coordinator::WorkOutcome::kApplied ||
        snapshot.outcome == coordinator::WorkOutcome::kNoEdit))) {
    queue_.MarkInFlightEngineCompleted();
  }
  if (snapshot.phase != coordinator::WorkPhase::kIdle) {
    return;
  }
  switch (snapshot.outcome) {
    case coordinator::WorkOutcome::kApplied:
    case coordinator::WorkOutcome::kNoEdit:
      queue_.CompleteInFlight(KeyState::kEditCompleted, KeyFailure::kNone);
      break;
    case coordinator::WorkOutcome::kRejected:
      queue_.CompleteInFlight(KeyState::kFailed,
                              FailureForReject(snapshot.cause));
      break;
    case coordinator::WorkOutcome::kNeedsResync:
      queue_.CompleteInFlight(KeyState::kFailed,
                              snapshot.cause ==
                                      coordinator::RejectCause::kTargetChangedWhileQueued
                                  ? KeyFailure::kTargetChanged
                                  : KeyFailure::kEngineUnavailable);
      break;
    case coordinator::WorkOutcome::kUncertain:
    case coordinator::WorkOutcome::kUnsupportedShape:
      queue_.CompleteInFlight(KeyState::kFailed, KeyFailure::kBlockedGate);
      break;
    case coordinator::WorkOutcome::kAbandonedStale:
      queue_.CompleteInFlight(KeyState::kFailed, KeyFailure::kFocusChanged);
      break;
    case coordinator::WorkOutcome::kNone:
      return;
  }
  if (snapshot.gate != coordinator::CoordinatorGate::kOpen) {
    queue_.FailQueued(KeyFailure::kBlockedGate);
  }
}

void ProductTextController::PumpNow() {
  if (!started_ || stopping_) {
    return;
  }
  ++pumps_;
  if (!engine_ || !coordinator_) {
    // The engine start is asynchronous; this pump is the STA-side adoption point. It is
    // non-blocking: a not-yet-ready start simply leaves the controller literal.
    TryAttachEngine();
    if (!engine_ || !coordinator_) {
      if (candidate_ui_) {
        candidate_ui_->End();
      }
      waker_.SetPacing(false);
      return;
    }
  }
  if (rebind_soon_) {
    rebind_soon_ = false;
    if (observer_) {
      observer_->RequestRebind();
      ++rebind_requests_;
    }
  }
  coordinator_->Pump();
  const ClientProvider::Harvest harvest = clients_->TakeReady();
  if (harvest.failed && harvest.failed_client == clients_->last_client()) {
    FailRawState(KeyFailure::kEngineUnavailable);
    FailQueuedAndMaybeInvalidate(KeyFailure::kEngineUnavailable, true);
    if (queue_.HasInFlight()) {
      queue_.MarkInFlightFailed(KeyFailure::kEngineUnavailable);
    }
  }
  binding_->Pump();
  observer_->Pump();
  if (coordinator_->bound() &&
      coordinator_->permission() == edit::InputPermission::kUnknown &&
      !queue_.HasInFlight() && queue_.empty() && !RawUnapplied()) {
    coordinator_->RefreshPermission();
  }
  ResolveInFlight();
  if (RawUnapplied()) {
    const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
    if (!snapshot.bound || snapshot.closed ||
        snapshot.gate != coordinator::CoordinatorGate::kOpen) {
      // The binding that owned the captured run is gone or blocked; its text cannot be
      // written anywhere else, so the loss is reported explicitly.
      FailRawState(KeyFailure::kTargetChanged);
    }
  }
  ResolveRaw();
  SubmitHead(false);
  UpdateCandidateUi();
  UpdatePacing();
}

void ProductTextController::UpdatePacing() {
  bool pace = false;
  if (started_ && !stopping_ && engine_ && coordinator_ && observer_ && clients_) {
    const coordinator::CoordinatorSnapshot coordinator = coordinator_->Snapshot();
    const ctx::ContextObserverSnapshot observer = observer_->Snapshot();
    if (queue_.HasInFlight() || !queue_.empty() ||
        coordinator.phase != coordinator::WorkPhase::kIdle ||
        coordinator.retirement.retiring_bindings != 0 ||
        coordinator.retirement.admission_paused || clients_->pending() != 0 ||
        (!coordinator.bound && observer.awaiting_bind) ||
        observer.selection_read_pending) {
      pace = true;
    }
  }
  waker_.SetPacing(pace);
}

bool ProductTextController::CandidatesOpen() const {
  if (!engine_ || !coordinator_ || raw_active_ || raw_submitted_) {
    return false;
  }
  if (!coordinator_->candidate_menu_open()) {
    return false;
  }
  return coordinator_->candidate_snapshot().version != candidate_dismissed_version_;
}

void ProductTextController::MoveCandidateSelection(int delta) {
  if (!coordinator_) {
    return;
  }
  const coordinator::CandidateSnapshot snapshot = coordinator_->candidate_snapshot();
  if (!snapshot.valid || snapshot.items.empty()) {
    return;
  }
  const int count = static_cast<int>(snapshot.items.size());
  if (delta > 0) {
    candidate_selection_ =
        candidate_selection_ < 0 ? 0 : (candidate_selection_ + 1) % count;
  } else if (delta < 0) {
    candidate_selection_ =
        candidate_selection_ < 0 ? count - 1 : (candidate_selection_ - 1 + count) % count;
  }
  if (candidate_ui_) {
    candidate_ui_->SetSelection(candidate_selection_);
  }
}

bool ProductTextController::FinalizeCandidateSelection(
    const CandidateUiSelection& selection) {
  if (!engine_ || !coordinator_ || selection.page_index < 0) {
    return false;
  }
  const coordinator::CandidateSnapshot snapshot = coordinator_->candidate_snapshot();
  if (!snapshot.valid || snapshot.version != selection.snapshot_version) {
    ++candidate_stale_selections_;
    candidate_selection_ = -1;
    candidate_dismissed_version_ = snapshot.version;
    if (candidate_ui_) {
      candidate_ui_->SetSelection(-1);
      candidate_ui_->End();
    }
    return false;
  }
  coordinator::CandidateSelectionRequest request;
  request.snapshot_version = selection.snapshot_version;
  request.context_id = selection.context_id;
  request.generation = selection.generation;
  request.page_index = static_cast<std::size_t>(selection.page_index);
  coordinator::WorkId work;
  const coordinator::SubmitStatus status =
      coordinator_->SubmitCandidateSelection(request, &work);
  if (status != coordinator::SubmitStatus::kAccepted) {
    ++candidate_stale_selections_;
    candidate_selection_ = -1;
    if (status == coordinator::SubmitStatus::kCandidateStale) {
      candidate_dismissed_version_ = snapshot.version;
      if (candidate_ui_) {
        candidate_ui_->SetSelection(-1);
        candidate_ui_->End();
      }
    } else if (candidate_ui_) {
      candidate_ui_->SetSelection(-1);
    }
    return false;
  }
  candidate_selection_ = -1;
  if (candidate_ui_) {
    candidate_ui_->SetSelection(-1);
  }
  waker_.SetPacing(true);
  return true;
}

void ProductTextController::UpdateCandidateUi() {
  if (!candidate_ui_) {
    return;
  }
  if (!engine_ || !coordinator_) {
    candidate_ui_->End();
    candidate_selection_ = -1;
    return;
  }
  const coordinator::CandidateSnapshot snapshot = coordinator_->candidate_snapshot();
  if (!snapshot.valid || raw_active_ || raw_submitted_) {
    candidate_ui_->End();
    candidate_selection_ = -1;
    candidate_ui_version_ = snapshot.version;
    return;
  }
  if (snapshot.version != candidate_ui_version_) {
    candidate_ui_version_ = snapshot.version;
    candidate_selection_ = -1;
  }
  if (candidate_dismissed_version_ == snapshot.version) {
    candidate_ui_->End();
    return;
  }
  const coordinator::CoordinatorSnapshot coordinator = coordinator_->Snapshot();
  RECT anchor = {};
  const RECT* anchor_ptr = nullptr;
  // A real TSF UI element manager is the only reason to start a read-only edit session for
  // the caret rectangle. Without one (the controlled direct-sink host) the element answers
  // no UI queries, so no TSF call is made. A host-drawn element needs no rectangle either.
  const bool wants_window = candidate_ui_->tsf_manager() &&
                            (!candidate_ui_->begun() || !candidate_ui_->host_drawn());
  if (wants_window && ui_context_ != nullptr && coordinator.composition_active &&
      coordinator.composition_start_utf16 >= 0 &&
      coordinator.composition_length_utf16 > 0) {
    bool clipped = false;
    if (ReadCompositionRect(ui_context_, client_id_, coordinator.composition_start_utf16,
                            coordinator.composition_length_utf16, &anchor, &clipped,
                            nullptr)) {
      anchor_ptr = &anchor;
    }
  }
  candidate_ui_->Update(snapshot, ui_context_, anchor_ptr, candidate_selection_);
}

void ProductTextController::OnUiSelectionChanged(
    const CandidateUiSelection& selection) {
  if (!coordinator_) {
    return;
  }
  const coordinator::CandidateSnapshot snapshot = coordinator_->candidate_snapshot();
  if (!snapshot.valid || snapshot.version != selection.snapshot_version ||
      snapshot.identity.context_id != selection.context_id ||
      snapshot.identity.generation != selection.generation ||
      selection.page_index < 0 ||
      selection.page_index >= static_cast<int>(snapshot.items.size())) {
    ++candidate_stale_selections_;
    return;
  }
  candidate_selection_ = selection.page_index;
  if (candidate_ui_) {
    candidate_ui_->SetSelection(selection.page_index);
  }
}

void ProductTextController::OnUiFinalize(const CandidateUiSelection& selection) {
  (void)FinalizeCandidateSelection(selection);
}

void ProductTextController::OnUiAbort(std::uint64_t snapshot_version,
                                     std::uint64_t context_id,
                                     std::uint64_t generation) {
  candidate_selection_ = -1;
  if (snapshot_version != 0) {
    candidate_dismissed_version_ = snapshot_version;
  } else if (coordinator_) {
    candidate_dismissed_version_ = coordinator_->candidate_snapshot().version;
  }
  (void)context_id;
  (void)generation;
  if (candidate_ui_) {
    candidate_ui_->SetSelection(-1);
    candidate_ui_->End();
  }
}

void ProductTextController::OnStaWake(std::uint64_t generation) {
  if (!started_ || stopping_ || generation != generation_) {
    ++stale_wakes_;
    return;
  }
  ++wakes_;
  PumpNow();
}
void ProductTextController::OnTargetLost(ctx::ContextDropReason reason) {
  ++target_losses_;
  FailRawState(KeyFailure::kFocusChanged);
  if (ui_context_ != nullptr) {
    ui_context_->Release();
    ui_context_ = nullptr;
  }
  candidate_selection_ = -1;
  candidate_ui_version_ = 0;
  // A host-owned token survives an external edit caused by the host inserting the very
  // characters we passed through; it ends only when the token itself ends or the target
  // really moves to another document.
  if (reason != ctx::ContextDropReason::kExternalTextEdit &&
      reason != ctx::ContextDropReason::kExternalSelectionEdit) {
    host_token_.Reset();
  }
  queue_.FailQueued(KeyFailure::kFocusChanged);
  if (queue_.HasInFlight() && !queue_.in_flight_failed()) {
    queue_.MarkInFlightFailed(KeyFailure::kFocusChanged);
  }
  binding_->OnTargetLost(reason);
  waker_.SetPacing(true);
}

void ProductTextController::OnTargetBound(ITfContext* context,
                                          const ctx::ObservedTarget& target) {
  ++target_binds_;
  if (ui_context_ != nullptr) {
    ui_context_->Release();
    ui_context_ = nullptr;
  }
  if (context != nullptr) {
    context->AddRef();
    ui_context_ = context;
  }
  // A fresh binding starts with a fresh engine client. The host-owned token is NOT a
  // binding fact: a rebind caused by the host inserting passed-through text must not
  // reopen the token to the engine.
  engine_stale_ = false;
  binding_->OnTargetBound(context, target);
  waker_.SetPacing(true);
}

void ProductTextController::OnOwnEditCompleted(
    ITfContext* context, const ctx::ObservedTarget& target,
    const ctx::SelectionSnapshot& selection) {
  binding_->OnOwnEditCompleted(context, target, selection);
}

coordinator::CandidateSnapshot ProductTextController::CandidateSnapshotForTest() const {
  if (!coordinator_) {
    return coordinator::CandidateSnapshot{};
  }
  return coordinator_->candidate_snapshot();
}

ControllerDiagnostics ProductTextController::Snapshot() const {
  ControllerDiagnostics diagnostics;
  diagnostics.started = started_;
  diagnostics.stopping = stopping_;
  diagnostics.subscribed = subscribed_;
  diagnostics.observer_closed = observer_ ? observer_->closed() : true;
  diagnostics.engine_ready = engine_ != nullptr && coordinator_ != nullptr;
  diagnostics.engine_attaching = attaching_;
  diagnostics.engine_state =
      DescribeEngineBootstrapState(ProductEngineBootstrap::Instance().state());
  const std::wstring bootstrap_failure = ProductEngineBootstrap::Instance().failure();
  diagnostics.engine_failure =
      engine_failure_.empty()
          ? std::string(bootstrap_failure.begin(), bootstrap_failure.end())
          : engine_failure_;
  diagnostics.literal_key_downs = literal_key_downs_;
  diagnostics.engine_attach_attempts = engine_attach_attempts_;
  diagnostics.engine_attach_failures = engine_attach_failures_;
  diagnostics.generation = generation_;
  diagnostics.pumps = pumps_;
  diagnostics.wakes = wakes_;
  diagnostics.stale_wakes = stale_wakes_;
  diagnostics.predictions = predictions_;
  diagnostics.predicted_eaten = predicted_eaten_;
  diagnostics.key_downs = key_downs_;
  diagnostics.eaten = eaten_;
  diagnostics.not_eaten = not_eaten_;
  diagnostics.overflow_eaten = overflow_eaten_;
  diagnostics.target_losses = target_losses_;
  diagnostics.target_binds = target_binds_;
  diagnostics.rebind_requests = rebind_requests_;
  diagnostics.raw_mode = raw_active_;
  diagnostics.raw_submitted = raw_submitted_;
  diagnostics.engine_stale = engine_stale_;
  diagnostics.raw_unapplied_units =
      static_cast<std::uint64_t>(raw_run_.size());
  diagnostics.raw_keys = raw_keys_;
  diagnostics.raw_commits = raw_commits_;
  diagnostics.raw_entered = raw_entered_;
  diagnostics.raw_exits = raw_exits_;
  diagnostics.raw_ignored_edits = raw_ignored_edits_;
  diagnostics.raw_cancelled_keys = raw_cancelled_keys_;
  diagnostics.raw_cancelled_units = raw_cancelled_units_;
  diagnostics.raw_unrecoverable = raw_unrecoverable_;
  diagnostics.raw_reason_unapplied = raw_reason_unapplied_;
  diagnostics.raw_reason_not_candidate = raw_reason_not_candidate_;
  diagnostics.raw_reason_high_water = raw_reason_high_water_;
  diagnostics.raw_reason_admission = raw_reason_admission_;
  diagnostics.token_host_keys = token_host_keys_;
  if (clients_) {
    diagnostics.engine_client_requests = clients_->requests();
    diagnostics.engine_client_results = clients_->results();
    diagnostics.engine_client_failures = clients_->failures();
  }
  if (coordinator_) {
    diagnostics.coordinator_bound = coordinator_->bound();
    diagnostics.coordinator = coordinator_->Snapshot();
    const coordinator::CandidateSnapshot candidates = coordinator_->candidate_snapshot();
    diagnostics.candidate_menu_open = candidates.valid;
    diagnostics.candidate_version = candidates.version;
    diagnostics.candidate_items = candidates.items.size();
  }
  diagnostics.candidate_selected = candidate_selection_;
  diagnostics.candidate_stale_selections = candidate_stale_selections_;
  if (candidate_ui_) {
    diagnostics.candidate_ui = candidate_ui_->Diagnostics();
  }
  if (observer_) {
    diagnostics.observer = observer_->Snapshot();
  }
  diagnostics.keys = queue_.Snapshot();
  diagnostics.pacing = waker_.Snapshot().paced;
  return diagnostics;
}

}  // namespace product
}  // namespace tsf
}  // namespace epocanva
