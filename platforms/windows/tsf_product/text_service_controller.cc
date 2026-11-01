// EpoCanva W2 -- product STA controller implementation. See the header for the contract.
#include "text_service_controller.h"

#include <atomic>
#include <cstdio>
#include <utility>

#include "key_text_translation.h"
#include "keyboard_state.h"
#include "module_lifetime.h"
#include "module_state.h"
#include "product_display_preference.h"
#include "product_paths.h"
#include "product_settings.h"

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

#ifdef EPOCANVA_PRODUCT_TRACE
void TraceStage(const char* stage, long long value) {
  char line[192];
  std::snprintf(line, sizeof(line), "EPOCANVA_TRACE|%s|%lld\n", stage, value);
  ::OutputDebugStringA(line);
}

void TraceText(const char* stage, const std::wstring& text) {
  std::string ascii;
  ascii.reserve(text.size());
  for (const wchar_t ch : text) {
    ascii.push_back(ch >= 0x20 && ch < 0x7F ? static_cast<char>(ch) : '?');
  }
  char line[512];
  std::snprintf(line, sizeof(line), "EPOCANVA_TRACE|%s|%s\n", stage, ascii.c_str());
  ::OutputDebugStringA(line);
}
#else
inline void TraceStage(const char*, long long) {}
inline void TraceText(const char*, const std::wstring&) {}
#endif

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
  // The candidate-display preference is published by the engine-start transaction on its
  // bootstrap thread (it already reads settings.ini there). The STA only reads the atomic;
  // the value is refreshed on every activation and applied when a menu updates.
  //
  // A process that attaches to an engine another process already started does not run the
  // transaction, so it would never publish the value. In that one case read it here, once
  // per activation. A fresh engine start stays free of synchronous settings I/O on the STA;
  // that timing is what the direct-sink regression tests exercise.
  if (ProductEngineBootstrap::Instance().state() == EngineBootstrapState::kReady) {
    ::epocanva::product::ProductPaths display_paths;
    std::wstring display_why;
    if (::epocanva::product::ResolveProductPaths(ModuleInstance(), &display_paths,
                                                 &display_why)) {
      const ::epocanva::product::SettingsLoadResult loaded =
          ::epocanva::product::LoadProductSettings(display_paths.settings_file);
      ::epocanva::product::SetCandidateDisplayAlwaysSelfForProcess(
          loaded.settings.candidate_display ==
          ::epocanva::product::CandidateDisplay::kAlwaysSelf);
    }
  }
  CandidateDisplayPreference display_preference =
      ::epocanva::product::CandidateDisplayAlwaysSelfForProcess()
          ? CandidateDisplayPreference::kAlwaysSelf
          : CandidateDisplayPreference::kFollowHost;
  TraceStage("candidate_display_always_self",
             display_preference == CandidateDisplayPreference::kAlwaysSelf ? 1 : 0);
  candidate_ui_.reset(
      new CandidateUi(client_id_, this, ModuleInstance(), display_preference));
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
  candidate_anchor_version_ = 0;
  candidate_anchor_caret_utf16_ = -1;
  candidate_anchor_retry_ = false;
  candidate_anchor_retry_budget_.Reset();
  predicted_host_caret_utf16_ = -1;
  host_token_origin_valid_ = false;
  host_token_origin_caret_utf16_ = -1;
  host_edits_expected_ = 0;
  pending_host_edit_carets_.clear();
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
  TraceStage("attach_attempt", static_cast<long long>(engine_attach_attempts_));
  std::shared_ptr<ProductEngineAttachment> attachment;
  bool engine_running = false;
  const bool took_pending =
      ProductEngineBootstrap::Instance().TakeAttachment(&attachment, &engine_running);
  if (!took_pending && engine_running) {
    std::wstring acquire_why;
    if (!ProductEngineBootstrap::Instance().AcquireRunning(&attachment, &acquire_why)) {
      TraceStage("attach_acquire_failed", 1);
      engine_failure_ = std::string(acquire_why.begin(), acquire_why.end());
      attaching_ = false;
      return;
    }
  }
  if (!attachment) {
    TraceStage("attach_none", 1);
    const auto& bootstrap = ProductEngineBootstrap::Instance();
    TraceStage("attach_bootstrap_state", static_cast<long long>(bootstrap.state()));
    TraceStage("attach_start_attempts",
               static_cast<long long>(bootstrap.start_attempts()));
    TraceStage("attach_start_failures",
               static_cast<long long>(bootstrap.start_failures()));
    const std::wstring bootstrap_failure = bootstrap.failure();
    if (!bootstrap_failure.empty()) {
      TraceText("attach_failure", bootstrap_failure);
    }
    attaching_ = false;
    return;
  }
  engine_ = std::move(attachment);
  std::wstring build_why;
  if (!BuildEnginePipeline(&build_why)) {
    TraceStage("attach_pipeline_failed", 1);
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
  TraceStage("attach_ok", 1);
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
  // W2-C ownership: the host inserts ordinary text; the engine only produces a cancelable
  // candidate snapshot. The coordinator therefore never builds a document edit from an
  // engine observation, and the observer treats the host's own inserts as the expected
  // text stream rather than as a reason to abandon the binding.
  coordinator_->SetSuggestionMode(true);
  ctx::ThreadFocusPolicy focus_policy = ctx::ThreadFocusPolicy::kStrictTsf;
#ifdef EPOCANVA_TSF_DIRECT_SINK_TEST
  // Controlled direct-sink tests run in a headless STA with no OS foreground focus. The
  // product DLL (which never defines this macro) keeps the StrictTsf policy.
  focus_policy = ctx::ThreadFocusPolicy::kLabWin32Fallback;
#endif
  observer_.reset(new ctx::ContextObserver(*registry_, client_id_, focus_policy,
                                           ctx::TextOwnershipPolicy::kHostOwned));
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
  suggestion_queue_.clear();
  suggestion_in_flight_ = false;
  replacement_pending_ = false;
  ResetObjects();
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
  if (IsTextKey(key) && key.event.modifiers.control == false &&
      key.event.modifiers.alt == false && key.event.modifiers.windows == false) {
    ++host_text_keys_;
  }
}

bool ProductTextController::IsTextKey(const BuiltKey& key) const {
  return key.text.status == input::KeyTextStatus::kText &&
         !key.text.utf16_code_units.empty();
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
  const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
  if (!snapshot.bound || snapshot.closed ||
      snapshot.gate != coordinator::CoordinatorGate::kOpen) {
    return false;
  }
  // Only menu interaction is ever eaten. Every key that produces, deletes or ends original
  // text stays with the host: the product cannot lose what it never owned, and no queue,
  // capacity or engine delay can become a text loss.
  const std::uint32_t vk = key.event.virtual_key;
  const bool plain_arrow = (vk == VK_UP || vk == VK_DOWN) &&
                           !key.event.modifiers.control && !key.event.modifiers.alt &&
                           !key.event.modifiers.shift && !key.event.modifiers.windows;
  if (plain_arrow && CandidateKeysActive() && CandidatesOpen()) {
    ++predicted_eaten_;
    return true;
  }
  if (CandidateKeysActive() && CandidatesOpen() && vk == VK_RETURN &&
      candidate_selection_ >= 0) {
    ++predicted_eaten_;
    return true;
  }
  if (CandidateKeysActive() && CandidatesOpen() && vk == VK_ESCAPE) {
    ++predicted_eaten_;
    return true;
  }
  return false;
}

bool ProductTextController::HandleKeyDown(WPARAM wparam, LPARAM lparam,
                                          std::wstring* why) {
  (void)why;
  if (!started_ || stopping_) {
    return false;
  }
  ++key_downs_;
  TraceStage("handle_entry", 1);
  if (!engine_ || !coordinator_) {
    // Literal mode while the engine starts, is unavailable, or failed. Every key keeps the
    // host's own default effect; nothing is captured, queued or edited.
    TraceStage("handle_no_engine", 1);
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
      snapshot.gate != coordinator::CoordinatorGate::kOpen) {
    TraceStage("handle_not_bound", snapshot.bound ? 1 : 0);
    TraceStage("handle_gate", static_cast<long long>(snapshot.gate));
    // No live binding: no suggestion stream can continue, and the host keeps the key. No
    // text is held or replayed anywhere. A key that changes text still causes one host
    // edit, which must not later be mistaken for an unrelated external change.
    const std::uint32_t blocked_vk = key.event.virtual_key;
    if (IsTextKey(key) || blocked_vk == input::kVirtualKeySpace ||
        blocked_vk == input::kVirtualKeyReturn || blocked_vk == input::kVirtualKeyBack) {
      ++host_edits_expected_;
      pending_host_edit_carets_.push_back(predicted_host_caret_utf16_);
    }
    predicted_host_caret_utf16_ = -1;
    host_token_origin_valid_ = false;
    host_token_origin_caret_utf16_ = -1;
    ResetCandidateAnchorRetry();
    ResetSuggestionStream(false);
    ++not_eaten_;
    return false;
  }
  // The only keys the product ever eats are menu interactions. They never change text.
  const std::uint32_t vk = key.event.virtual_key;
  const bool plain_arrow = (vk == VK_UP || vk == VK_DOWN) &&
                           !key.event.modifiers.control && !key.event.modifiers.alt &&
                           !key.event.modifiers.shift && !key.event.modifiers.windows;
  if (CandidateKeysActive() && CandidatesOpen() && plain_arrow) {
    TraceStage("menu_arrow", vk == VK_DOWN ? 1 : -1);
    MoveCandidateSelection(vk == VK_DOWN ? 1 : -1);
    TraceStage("menu_arrow_selection", candidate_selection_);
    ++eaten_;
    return true;
  }
  if (CandidateKeysActive() && CandidatesOpen() && vk == VK_RETURN &&
      candidate_selection_ >= 0) {
    const coordinator::CandidateSnapshot menu = coordinator_->candidate_snapshot();
    TraceStage("menu_enter_selection", candidate_selection_);
    TraceStage("menu_enter_in_flight", suggestion_in_flight_ ? 1 : 0);
    TraceStage("menu_enter_queue", static_cast<long long>(suggestion_queue_.size()));
    TraceStage("menu_enter_pending", replacement_pending_ ? 1 : 0);
    TraceStage("menu_enter_token_units", static_cast<long long>(host_token_units_));
    CandidateUiSelection selection;
    selection.snapshot_version = menu.version;
    selection.context_id = menu.identity.context_id;
    selection.generation = menu.identity.generation;
    selection.page_index = candidate_selection_;
    selection.caret_utf16 = menu.caret_utf16;
    // This is the one explicit enhancement that suppresses a host key. The product still
    // owns no original text: a refused replacement leaves the typed token in the document.
    const bool finalized = FinalizeCandidateSelection(selection);
    TraceStage("menu_enter_finalized", finalized ? 1 : 0);
    ++eaten_;
    return true;
  }
  if (CandidateKeysActive() && CandidatesOpen() && vk == VK_ESCAPE) {
    // Escape dismisses the cancelable enhancement. It never removes text the user typed.
    // The engine stream is reset below; if the user keeps typing, the continuation starts a
    // new suggestion token at the current caret (host_token_units_ restarts at zero).
    candidate_dismissed_version_ = coordinator_->candidate_snapshot().version;
    DismissCandidateMenu();
    suggestion_queue_.clear();
    host_token_units_ = 0;
    host_token_origin_valid_ = false;
    host_token_origin_caret_utf16_ = -1;
    ResetCandidateAnchorRetry();
    if (key.decision.is_engine_candidate() || suggestion_in_flight_) {
      FeedSuggestionKey(key, snapshot, /*ends_token=*/true);
    }
    ++eaten_;
    return true;
  }

  // --- host-owned original text ---------------------------------------------------------
  // The product never returns TRUE for these; the host inserts, deletes or moves exactly as
  // its own default path would. Suggestions may be fed to the engine in the background.
  const bool shortcut =
      key.decision.disposition == input::KeyDisposition::kShortcutPassThrough;
  const bool forbidden =
      key.decision.reason == input::KeyDecisionReason::kPermissionForbidden ||
      key.decision.reason == input::KeyDecisionReason::kPermissionUnknown ||
      key.decision.reason == input::KeyDecisionReason::kInputDisabled;
  if (forbidden) {
    // Privacy: no text reaches the engine and no token length is retained.
    ResetSuggestionStream(false);
    host_token_units_ = 0;
    predicted_host_caret_utf16_ = -1;
    host_token_origin_valid_ = false;
    host_token_origin_caret_utf16_ = -1;
    ResetCandidateAnchorRetry();
    ++not_eaten_;
    return false;
  }
  if (shortcut) {
    // A chord does not change the token or the suggestion stream.
    ++not_eaten_;
    return false;
  }

  const bool ends_token = vk == input::kVirtualKeySpace || vk == input::kVirtualKeyReturn;
  const bool edits_token = vk == input::kVirtualKeyBack;
  if (IsTextKey(key) || ends_token || edits_token ||
      key.decision.reason == input::KeyDecisionReason::kNoText ||
      key.decision.reason == input::KeyDecisionReason::kDeadKey) {
    // Any change to the host token invalidates the menu that described the old prefix.
    DismissCandidateMenu();
  }
  TraceStage("handle_feed_decision", key.decision.is_engine_candidate() ? 1 : 0);
  if (key.decision.is_engine_candidate()) {
    FeedSuggestionKey(key, snapshot, ends_token);
    if (ends_token && suggestion_stale_) {
      // The token ended while suggestions were stale: reset the engine stream now (or as
      // soon as its in-flight work resolves) so the next token starts clean.
      EndSuggestionToken();
    }
  } else if (IsTextKey(key) || ends_token || edits_token ||
             key.decision.requires_composition_boundary()) {
    // The engine's token is over (punctuation, unsupported content, navigation). Stop
    // suggesting for it; a fresh engine stream starts with the next token.
    EndSuggestionToken();
  }

  const LONG key_units_utf16 = IsTextKey(key)
                                   ? static_cast<LONG>(key.text.utf16_code_units.size())
                                   : 0;
  if (ends_token) {
    // Space and Return terminate the token even though ToUnicodeEx also reports a
    // character for them. Their own character is host text, not part of the next token.
    // The boundary also ends any abandoned suffix: a new token may be suggested normally.
    host_token_units_ = 0;
    token_abandoned_ = false;
    host_token_origin_valid_ = false;
    host_token_origin_caret_utf16_ = -1;
    if (vk == input::kVirtualKeySpace) {
      NoteHostCaretDeltaUtf16(key_units_utf16 > 0 ? key_units_utf16 : 1);
      ++host_text_keys_;
    } else {
      // The host decides the newline width (CR/LF or a paragraph break); do not guess it.
      predicted_host_caret_utf16_ = -1;
    }
    ResetCandidateAnchorRetry();
  } else if (edits_token) {
    NoteHostCaretDeltaUtf16(-1);
    if (host_token_units_ > 0) {
      --host_token_units_;
    }
    if (host_token_units_ == 0) {
      host_token_origin_valid_ = false;
      host_token_origin_caret_utf16_ = -1;
      ResetCandidateAnchorRetry();
    }
  } else if (IsTextKey(key) && key.decision.is_engine_candidate()) {
    if (host_token_units_ == 0) {
      host_token_origin_caret_utf16_ = predicted_host_caret_utf16_;
      host_token_origin_valid_ = host_token_origin_caret_utf16_ >= 0;
      ResetCandidateAnchorRetry();
    }
    NoteHostCaretDeltaUtf16(key_units_utf16);
    host_token_units_ += key.text.utf16_code_units.size();
    ++host_text_keys_;
  } else if (IsTextKey(key)) {
    // Punctuation outside the engine alphabet ends the token.
    NoteHostCaretDeltaUtf16(key_units_utf16);
    host_token_units_ = 0;
    host_token_origin_valid_ = false;
    host_token_origin_caret_utf16_ = -1;
    ++host_text_keys_;
    ResetCandidateAnchorRetry();
  } else if (key.decision.reason == input::KeyDecisionReason::kNoText ||
             key.decision.reason == input::KeyDecisionReason::kDeadKey ||
             key.decision.requires_composition_boundary()) {
    host_token_units_ = 0;
    host_token_origin_valid_ = false;
    host_token_origin_caret_utf16_ = -1;
    ResetCandidateAnchorRetry();
  }
  if (IsTextKey(key) || ends_token || edits_token) {
    ++host_edits_expected_;
    pending_host_edit_carets_.push_back(predicted_host_caret_utf16_);
  }
  ++not_eaten_;
  return false;
}

bool ProductTextController::FeedSuggestionKey(
    const BuiltKey& key, const coordinator::CoordinatorSnapshot& snapshot,
    bool ends_token) {
  if (suggestion_stale_ || token_abandoned_) {
    // The engine never saw the earlier part of this token. Feeding the remainder would
    // present a suffix as if it were the whole prefix, so the token waits for its boundary.
    ++suggestion_dropped_;
    return false;
  }
  if (suggestion_queue_.size() >= kSuggestionQueueCapacity) {
    // The engine is behind. Suggestions are the only thing lost; the host already has the
    // text and the product stores none of it.
    MarkSuggestionStale();
    ++suggestion_dropped_;
    return false;
  }
  SuggestionKey item;
  item.event = key.event;
  item.text = key.text;
  item.decision = key.decision;
  item.context_id = snapshot.identity.context_id;
  item.generation = snapshot.identity.generation;
  item.ends_token = ends_token;
  suggestion_queue_.push_back(std::move(item));
  suggestion_engaged_ = true;
  ++suggestion_queued_;
  SubmitNextSuggestion();
  waker_.SetPacing(true);
  return true;
}

void ProductTextController::SubmitNextSuggestion() {
  if (suggestion_in_flight_ || replacement_pending_ || !coordinator_ || !engine_) {
    return;
  }
  if (suggestion_queue_.empty()) {
    return;
  }
  const bool reset_front = suggestion_queue_.front().reset;
  if (suggestion_stale_ && !reset_front) {
    return;  // normal suggestions stay off until the engine has been reset
  }
  const SuggestionKey& head = suggestion_queue_.front();
  coordinator::WorkId work;
  const coordinator::SubmitStatus status =
      coordinator_->SubmitKey(head.event, head.text, &work);
  switch (status) {
    case coordinator::SubmitStatus::kAccepted:
      suggestion_in_flight_ = true;
      suggestion_in_flight_reset_ = reset_front;
      suggestion_work_id_ = work.value;
      suggestion_queue_.pop_front();
      ++suggestion_submitted_;
      return;
    case coordinator::SubmitStatus::kBusy:
    case coordinator::SubmitStatus::kRetiring:
    case coordinator::SubmitStatus::kQueueFull:
    case coordinator::SubmitStatus::kNotBound:
      return;  // retried from the next pump; the host is unaffected
    case coordinator::SubmitStatus::kNotEngineCandidate:
      // The engine stream does not want this key (for example Backspace or a reset with no
      // engine composition). Drop the suggestion, never the text.
      if (reset_front) {
        suggestion_stale_ = false;
        suggestion_engaged_ = false;
        ++reset_submit_refusal_;
      }
      suggestion_queue_.pop_front();
      ++suggestion_dropped_;
      return;
    default:
      // Permission/target/engine failures close suggestions for this token. No binding is
      // torn down for a suggestion failure; the host keeps typing.
      if (reset_front) {
        ++reset_submit_refusal_;
        suggestion_stale_ = false;
      }
      suggestion_queue_.pop_front();
      ++suggestion_dropped_;
      if (!reset_front) {
        MarkSuggestionStale();
      }
      return;
  }
}

void ProductTextController::ResolveSuggestionWork() {
  if (suggestion_in_flight_) {
    const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
    if (snapshot.phase == coordinator::WorkPhase::kIdle) {
      suggestion_in_flight_ = false;
      suggestion_work_id_ = 0;
      ++suggestion_completed_;
      TraceStage("suggestion_completed", 1);
      TraceStage("suggestion_outcome", static_cast<long long>(snapshot.outcome));
      const bool was_reset = suggestion_in_flight_reset_;
      suggestion_in_flight_reset_ = false;
      if (was_reset) {
        suggestion_stale_ = false;
        suggestion_engaged_ = false;
        suggestion_queue_.clear();
      } else if (snapshot.outcome != coordinator::WorkOutcome::kNoEdit &&
                 snapshot.outcome != coordinator::WorkOutcome::kApplied) {
        MarkSuggestionStale();
      }
    }
  }
  if (!suggestion_in_flight_ && !replacement_pending_ &&
      (suggestion_stale_ || stream_reset_owed_)) {
    // A token boundary or a failed/stale stream needs the engine's composition cancelled.
    // A synthetic Escape does that without touching the document or rebuilding the binding.
    stream_reset_owed_ = false;
    QueueEngineReset();
  }
  if (!suggestion_in_flight_ && !replacement_pending_) {
    // Also retries a queued reset while the stream is stale: SubmitNextSuggestion refuses
    // ordinary keys in that state but must still be able to dispatch the reset itself.
    SubmitNextSuggestion();
  }
}

void ProductTextController::QueueEngineReset() {
  if (!coordinator_ || !engine_ || !coordinator_->bound() || replacement_pending_) {
    return;
  }
  if (suggestion_in_flight_reset_) {
    return;
  }
  for (const SuggestionKey& queued : suggestion_queue_) {
    if (queued.reset) {
      // One reset is already owed; still drive it from here so a busy coordinator is retried
      // by the next pump instead of leaving the queue permanently blocked.
      SubmitNextSuggestion();
      return;
    }
  }
  SuggestionKey reset;
  reset.reset = true;
  reset.event.virtual_key = input::kVirtualKeyEscape;
  reset.event.transition = input::KeyTransition::kKeyDown;
  reset.event.repeat_count = 1;
  reset.text.status = input::KeyTextStatus::kNoText;
  const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
  if (snapshot.identity.context_id != 0) {
    reset.context_id = snapshot.identity.context_id;
    reset.generation = snapshot.identity.generation;
  }
  suggestion_queue_.push_back(std::move(reset));
  ++suggestion_resets_;
  SubmitNextSuggestion();
  waker_.SetPacing(true);
}

void ProductTextController::EndSuggestionToken() {
  const bool had_stream =
      suggestion_engaged_ || suggestion_in_flight_ ||
      (coordinator_ && coordinator_->candidate_menu_open());
  suggestion_queue_.clear();
  suggestion_engaged_ = false;
  host_token_units_ = 0;
  token_abandoned_ = false;
  if (had_stream || suggestion_stale_) {
    // Cancel the engine's stale composition with Escape; the binding and the document stay.
    if (!suggestion_in_flight_ && !replacement_pending_) {
      ++reset_boundary_;
      suggestion_stale_ = true;
      QueueEngineReset();
    } else {
      stream_reset_owed_ = true;
      ++reset_owed_;
    }
  }
}

void ProductTextController::ResetSuggestionStream(bool invalidate_binding) {
  suggestion_queue_.clear();
  suggestion_stale_ = false;
  suggestion_engaged_ = false;
  token_abandoned_ = false;
  DismissCandidateMenu();
  host_token_units_ = 0;
  (void)invalidate_binding;  // suggestion resets never tear down a binding
}

void ProductTextController::MarkSuggestionStale() {
  if (!suggestion_stale_) {
    suggestion_stale_ = true;
    ++suggestion_stale_events_;
  }
  // Whatever caused the drop, the engine's next prefix is incomplete. No suggestion from
  // this token is restarted; only a boundary or a new host target clears the abandonment.
  token_abandoned_ = true;
  suggestion_dropped_ += suggestion_queue_.size();
  suggestion_queue_.clear();
  DismissCandidateMenu();
}

void ProductTextController::DismissCandidateMenu() {
  candidate_selection_ = -1;
  candidate_anchor_version_ = 0;
  candidate_anchor_caret_utf16_ = -1;
  candidate_anchor_rect_ = {};
  if (coordinator_) {
    // A dismissed snapshot version stays dismissed. A later data update with the same
    // version must not silently reopen the menu the user or a key just closed.
    const coordinator::CandidateSnapshot snapshot = coordinator_->candidate_snapshot();
    if (snapshot.version != 0) {
      candidate_dismissed_version_ = snapshot.version;
    }
  }
  if (candidate_ui_) {
    candidate_ui_->SetSelection(-1);
    candidate_ui_->End();
  }
}

bool ProductTextController::SubmitReplacement(const CandidateUiSelection& selection) {
  if (!coordinator_ || !engine_) {
    return false;
  }
  ++replacement_attempts_;
  coordinator::WorkId work;
  const coordinator::SubmitStatus status =
      coordinator_->SubmitSuggestionReplacement(
          coordinator::CandidateSelectionRequest{
              selection.snapshot_version, selection.context_id, selection.generation,
              static_cast<std::size_t>(selection.page_index), selection.caret_utf16},
          &work);
  TraceStage("replacement_submit_status", static_cast<long long>(status));
  TraceStage("replacement_page", static_cast<long long>(selection.page_index));
  TraceStage("replacement_caret", selection.caret_utf16);
  TraceStage("replacement_snapshot_version",
             static_cast<long long>(selection.snapshot_version));
  if (status != coordinator::SubmitStatus::kAccepted) {
    ++replacement_refused_;
    DismissCandidateMenu();
    return false;
  }
  replacement_pending_ = true;
  replacement_work_id_ = work.value;
  DismissCandidateMenu();
  waker_.SetPacing(true);
  return true;
}

void ProductTextController::ResolveReplacement() {
  if (!replacement_pending_) {
    return;
  }
  const coordinator::CoordinatorSnapshot snapshot = coordinator_->Snapshot();
  if (snapshot.phase != coordinator::WorkPhase::kIdle) {
    return;
  }
  replacement_pending_ = false;
  replacement_work_id_ = 0;
  suggestion_engaged_ = false;
  last_replacement_outcome_ = static_cast<int>(snapshot.outcome);
  last_replacement_edit_status_ = static_cast<int>(snapshot.last_edit_status);
  last_replacement_edit_reason_ = static_cast<int>(snapshot.last_edit_reason);
  last_replacement_expected_units_ = snapshot.last_suggestion_expected_units;
  TraceStage("replacement_outcome", static_cast<long long>(snapshot.outcome));
  TraceStage("replacement_edit_status", static_cast<long long>(snapshot.last_edit_status));
  TraceStage("replacement_edit_reason", static_cast<long long>(snapshot.last_edit_reason));
  TraceStage("replacement_expected_units",
             static_cast<long long>(snapshot.last_suggestion_expected_units));
  if (snapshot.outcome == coordinator::WorkOutcome::kApplied) {
    ++replacement_applied_;
  } else {
    ++replacement_refused_;
    if (snapshot.last_edit_status == edit::EditStatus::kFailedPossiblyMutated ||
        snapshot.last_edit_status == edit::EditStatus::kFailedAfterMutation) {
      // The writer could not prove what the document holds. The whole expected token is
      // reported as an internal loss rather than assumed to be intact.
      internal_text_losses_ += snapshot.last_suggestion_expected_units;
    }
  }
  // Whether the candidate applied or was refused, the engine's state describes the token
  // before the selection. Cancel only that engine composition; the document and binding
  // stay as they are.
  suggestion_queue_.clear();
  suggestion_stale_ = true;
  token_abandoned_ = false;
  stream_reset_owed_ = false;
  host_token_units_ = 0;
  ++reset_replacement_;
  QueueEngineReset();
  waker_.SetPacing(true);
}

void ProductTextController::RequestRebindSoon() { rebind_soon_ = true; }

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
  if (!coordinator_->bound() && observer_ && observer_->has_current_context()) {
    // A read failure or a refused bind can leave the observer with no rebind owed while
    // the coordinator has no binding. Retry on the paced wake until a binding exists; the
    // observer still performs the real selection read and live target checks each time.
    rebind_soon_ = true;
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
    // The suggestion client failed. Text already in the document is untouched; only the
    // suggestion stream is abandoned and the next token gets a fresh client.
    MarkSuggestionStale();
  }
  binding_->Pump();
  observer_->Pump();
  if (coordinator_->bound() &&
      coordinator_->permission() == edit::InputPermission::kUnknown &&
      !suggestion_in_flight_ && suggestion_queue_.empty() && !replacement_pending_) {
    coordinator_->RefreshPermission();
  }
  ResolveSuggestionWork();
  ResolveReplacement();
  UpdateCandidateUi();
  UpdatePacing();
}

void ProductTextController::UpdatePacing() {
  bool pace = false;
  if (started_ && !stopping_ && engine_ && coordinator_ && observer_ && clients_) {
    const coordinator::CoordinatorSnapshot coordinator = coordinator_->Snapshot();
    const ctx::ContextObserverSnapshot observer = observer_->Snapshot();
    if (!coordinator.bound) {
      // Keep the paced wake alive while a rebind is owed; otherwise a failed selection read
      // would leave the controller idle with no binding and no future retry.
      pace = true;
    }
    if (suggestion_in_flight_ || !suggestion_queue_.empty() || replacement_pending_ ||
        coordinator.phase != coordinator::WorkPhase::kIdle ||
        coordinator.retirement.retiring_bindings != 0 ||
        coordinator.retirement.admission_paused || clients_->pending() != 0 ||
        (!coordinator.bound && observer.awaiting_bind) ||
        observer.selection_read_pending || candidate_anchor_retry_) {
      pace = true;
    }
  }
  waker_.SetPacing(pace);
}

bool ProductTextController::CandidateKeysActive() const {
  // A no-UIElementMgr thread still routes menu keys through the controller (state C);
  // the direct-sink test variant does the same without a window. For a real manager the
  // menu is key-active exactly when its TIP window may present (host decision plus the
  // user's always_self preference; Show(FALSE) turns it off).
  return candidate_ui_ == nullptr || candidate_ui_->interaction_active();
}

bool ProductTextController::CandidatesOpen() const {
  if (!engine_ || !coordinator_ || suggestion_stale_ || token_abandoned_ ||
      replacement_pending_) {
    return false;
  }
  if (suggestion_in_flight_ || !suggestion_queue_.empty()) {
    // The menu describes a prefix the engine has already processed; a key is still in
    // flight, so the displayed menu no longer matches everything the host has typed.
    return false;
  }
  if (!coordinator_->candidate_menu_open()) {
    return false;
  }
  const coordinator::CandidateSnapshot snapshot = coordinator_->candidate_snapshot();
  if (snapshot.input_utf16_code_units.size() != host_token_units_) {
    // A menu that is shorter than what the host has inserted must never be selectable.
    return false;
  }
  return snapshot.version != candidate_dismissed_version_;
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
    TraceStage("finalize_refuse", 1);
    return false;
  }
  const coordinator::CandidateSnapshot snapshot = coordinator_->candidate_snapshot();
  if (!snapshot.valid || snapshot.version != selection.snapshot_version ||
      snapshot.identity.context_id != selection.context_id ||
      snapshot.identity.generation != selection.generation ||
      selection.caret_utf16 != snapshot.caret_utf16) {
    TraceStage("finalize_refuse", 2);
    TraceStage("finalize_snapshot_valid", snapshot.valid ? 1 : 0);
    TraceStage("finalize_version_match",
               snapshot.version == selection.snapshot_version ? 1 : 0);
    TraceStage("finalize_identity_match",
               snapshot.identity.context_id == selection.context_id &&
                       snapshot.identity.generation == selection.generation
                   ? 1
                   : 0);
    TraceStage("finalize_caret_match",
               selection.caret_utf16 == snapshot.caret_utf16 ? 1 : 0);
    ++candidate_stale_selections_;
    if (snapshot.valid && snapshot.version == selection.snapshot_version) {
      // The request names the current version but its identity/caret does not match it.
      // Close that menu. A request from an older version must not dismiss the current one.
      candidate_dismissed_version_ = snapshot.version;
      DismissCandidateMenu();
    }
    return false;
  }
  if (suggestion_in_flight_ || replacement_pending_ || !suggestion_queue_.empty() ||
      token_abandoned_ || suggestion_stale_ ||
      snapshot.input_utf16_code_units.size() != host_token_units_) {
    TraceStage("finalize_refuse", 3);
    TraceStage("finalize_in_flight", suggestion_in_flight_ ? 1 : 0);
    TraceStage("finalize_pending", replacement_pending_ ? 1 : 0);
    TraceStage("finalize_queue", static_cast<long long>(suggestion_queue_.size()));
    TraceStage("finalize_abandoned", token_abandoned_ ? 1 : 0);
    TraceStage("finalize_stale", suggestion_stale_ ? 1 : 0);
    TraceStage("finalize_units",
               static_cast<long long>(snapshot.input_utf16_code_units.size()));
    TraceStage("finalize_host_units", static_cast<long long>(host_token_units_));
    ++candidate_stale_selections_;
    candidate_dismissed_version_ = snapshot.version;
    DismissCandidateMenu();
    return false;
  }
  if (!CandidatesOpen()) {
    TraceStage("finalize_refuse", 6);
    ++candidate_stale_selections_;
    candidate_dismissed_version_ = snapshot.version;
    DismissCandidateMenu();
    return false;
  }
  std::int32_t expected_caret = -1;
  if (!ExpectedCandidateCaretUtf16(&expected_caret)) {
    TraceStage("finalize_refuse", 4);
    ++candidate_stale_selections_;
    candidate_dismissed_version_ = snapshot.version;
    DismissCandidateMenu();
    return false;
  }
  TraceStage("finalize_expected_caret", expected_caret);
  LONG anchor_caret = -1;
  if (candidate_anchor_version_ == snapshot.version &&
      candidate_anchor_caret_utf16_ >= 0) {
    anchor_caret = candidate_anchor_caret_utf16_;
  }
  if (ui_context_ != nullptr) {
    LONG live_caret = -1;
    const bool live_ok =
        ReadCaretPosition(ui_context_, client_id_, &live_caret, nullptr);
    TraceStage("finalize_live_caret_ok", live_ok ? 1 : 0);
    if (live_ok) {
      TraceStage("finalize_live_caret", live_caret);
    }
    if (!live_ok || live_caret != expected_caret ||
        (anchor_caret >= 0 && live_caret != anchor_caret)) {
      // A different caret means a different token, even if the preceding text is the same.
      // A live read that cannot be verified is refused the same way. Never retarget the
      // menu or submit at the current cursor.
      TraceStage("finalize_refuse", 5);
      ++candidate_stale_selections_;
      candidate_dismissed_version_ = snapshot.version;
      DismissCandidateMenu();
      return false;
    }
    anchor_caret = live_caret;
    candidate_anchor_version_ = snapshot.version;
    candidate_anchor_caret_utf16_ = live_caret;
    if (coordinator_ != nullptr) {
      coordinator_->ObserveRealCaret(live_caret);
    }
  } else if (anchor_caret != expected_caret) {
    TraceStage("finalize_refuse", 7);
    ++candidate_stale_selections_;
    candidate_dismissed_version_ = snapshot.version;
    DismissCandidateMenu();
    return false;
  }
  TraceStage("finalize_anchor_caret", anchor_caret);
  TraceStage("finalize_input_units",
             static_cast<long long>(snapshot.input_utf16_code_units.size()));
  CandidateUiSelection anchored = selection;
  anchored.caret_utf16 = anchor_caret;
  if (!SubmitReplacement(anchored)) {
    TraceStage("finalize_refuse", 8);
    ++candidate_stale_selections_;
    candidate_dismissed_version_ = snapshot.version;
    return false;
  }
  TraceStage("finalize_selected", selection.page_index);
  return true;
}

bool ProductTextController::ExpectedCandidateCaretUtf16(std::int32_t* expected) const {
  return CandidateTokenEndUtf16(
      host_token_origin_valid_, static_cast<std::int32_t>(host_token_origin_caret_utf16_),
      host_token_units_, expected);
}

void ProductTextController::ResetCandidateAnchorRetry() {
  candidate_anchor_retry_ = false;
  candidate_anchor_retry_budget_.Reset();
}

void ProductTextController::NoteHostCaretDeltaUtf16(LONG delta) {
  if (predicted_host_caret_utf16_ < 0 || delta == 0) {
    return;
  }
  predicted_host_caret_utf16_ = predicted_host_caret_utf16_ + delta;
  if (predicted_host_caret_utf16_ < 0) {
    predicted_host_caret_utf16_ = 0;
  }
}

void ProductTextController::UpdateCandidateUi() {
  if (!candidate_ui_) {
    return;
  }
  candidate_ui_->SetDisplayPreference(
      ::epocanva::product::CandidateDisplayAlwaysSelfForProcess()
          ? CandidateDisplayPreference::kAlwaysSelf
          : CandidateDisplayPreference::kFollowHost);
  candidate_anchor_retry_ = false;
  if (!engine_ || !coordinator_) {
    TraceStage("candidate_no_engine", 1);
    candidate_ui_->End();
    candidate_selection_ = -1;
    ResetCandidateAnchorRetry();
    return;
  }
  const coordinator::CandidateSnapshot snapshot = coordinator_->candidate_snapshot();
  TraceStage("candidate_snapshot_valid", snapshot.valid ? 1 : 0);
  TraceStage("candidate_items", static_cast<long long>(snapshot.items.size()));
  if (!snapshot.valid || suggestion_stale_ || token_abandoned_ || replacement_pending_ ||
      suggestion_in_flight_ || !suggestion_queue_.empty()) {
    if (!snapshot.valid) TraceStage("candidate_invalid", 1);
    if (suggestion_stale_) TraceStage("candidate_stale", 1);
    if (token_abandoned_) TraceStage("candidate_abandoned", 1);
    if (replacement_pending_) TraceStage("candidate_replacement_pending", 1);
    if (suggestion_in_flight_) TraceStage("candidate_in_flight", 1);
    if (!suggestion_queue_.empty()) TraceStage("candidate_queue", static_cast<long long>(suggestion_queue_.size()));
    candidate_ui_->End();
    candidate_selection_ = -1;
    candidate_ui_version_ = snapshot.version;
    candidate_anchor_version_ = 0;
    candidate_anchor_caret_utf16_ = -1;
    candidate_anchor_rect_ = {};
    ResetCandidateAnchorRetry();
    return;
  }
  if (snapshot.version != candidate_ui_version_) {
    candidate_ui_version_ = snapshot.version;
    candidate_selection_ = -1;
    candidate_anchor_version_ = 0;
    candidate_anchor_caret_utf16_ = -1;
    candidate_anchor_rect_ = {};
    // The retry budget is per token, not per menu version. A host that keeps producing
    // versions must not extend it.
  }
  if (candidate_dismissed_version_ == snapshot.version) {
    candidate_ui_->End();
    return;
  }
  // The menu may only describe the host token that is current now. A shorter or longer
  // snapshot belongs to an older prefix and must not be presented or selectable.
  if (snapshot.input_utf16_code_units.size() != host_token_units_) {
    TraceStage("candidate_token_units_mismatch",
               static_cast<long long>(snapshot.input_utf16_code_units.size()));
    candidate_dismissed_version_ = snapshot.version;
    candidate_ui_->End();
    candidate_selection_ = -1;
    candidate_anchor_version_ = 0;
    candidate_anchor_caret_utf16_ = -1;
    return;
  }
  std::int32_t expected_caret = -1;
  if (!ExpectedCandidateCaretUtf16(&expected_caret)) {
    // No verified token origin: fail closed. A guessed caret could point at a different
    // occurrence of the same text.
    TraceStage("candidate_token_origin_unknown", 1);
    candidate_dismissed_version_ = snapshot.version;
    candidate_ui_->End();
    candidate_selection_ = -1;
    candidate_anchor_version_ = 0;
    candidate_anchor_caret_utf16_ = -1;
    return;
  }
  TraceStage("candidate_expected_caret", expected_caret);
  // A real TSF UI element manager is the only reason to create a window when the host
  // draws; a thread without one, or the user's always_self fallback, presents the TIP
  // window itself. The rectangle is the live token range [caret - input, caret), never a
  // composition. Elements the host draws still receive every data update.
  const bool wants_window = candidate_ui_->self_draw_permitted();
  TraceStage("candidate_wants_window", wants_window ? 1 : 0);
  TraceStage("candidate_tsf_manager", candidate_ui_->tsf_manager() ? 1 : 0);
  TraceStage("candidate_host_drawn", candidate_ui_->host_drawn() ? 1 : 0);
  const RECT* anchor_ptr = nullptr;
  if (wants_window) {
    if (ui_context_ == nullptr) {
      TraceStage("candidate_no_context", 1);
    } else {
      if (candidate_anchor_version_ != snapshot.version ||
          candidate_anchor_caret_utf16_ < 0) {
        LONG live_caret = -1;
        const bool live_ok =
            ReadCaretPosition(ui_context_, client_id_, &live_caret, nullptr);
        TraceStage("candidate_live_caret_ok", live_ok ? 1 : 0);
        if (live_ok) {
          TraceStage("candidate_live_caret", live_caret);
          const CandidateAnchorCheck check = CheckCandidateAnchor(
              host_token_origin_valid_,
              static_cast<std::int32_t>(host_token_origin_caret_utf16_),
              host_token_units_, static_cast<std::int32_t>(live_caret));
          if (check.decision != CandidateAnchorDecision::kAccept) {
            // A caret at another position is a different token, even when the same text
            // precedes it. Never retarget this menu to the current cursor.
            TraceStage("candidate_caret_mismatch",
                       check.expected_caret_utf16 >= 0 ? check.expected_caret_utf16 : -1);
            candidate_dismissed_version_ = snapshot.version;
            candidate_ui_->End();
            candidate_selection_ = -1;
            candidate_anchor_version_ = 0;
            candidate_anchor_caret_utf16_ = -1;
            return;
          }
          candidate_anchor_version_ = snapshot.version;
          candidate_anchor_caret_utf16_ = live_caret;
          candidate_anchor_rect_ = {};
          if (coordinator_ != nullptr) {
            coordinator_->ObserveRealCaret(live_caret);
          }
        }
      }
      if (candidate_anchor_version_ == snapshot.version &&
          candidate_anchor_caret_utf16_ >= 0) {
        bool clipped = false;
        RECT rect = {};
        LONG observed_caret = -1;
        const LONG token_units =
            static_cast<LONG>(snapshot.input_utf16_code_units.size());
        const bool token_rect_ok =
            ReadTokenRect(ui_context_, client_id_, candidate_anchor_caret_utf16_,
                          token_units, &rect, &clipped, &observed_caret, nullptr);
        TraceStage("candidate_token_rect_ok", token_rect_ok ? 1 : 0);
        if (token_rect_ok) {
          candidate_anchor_rect_ = rect;
          anchor_ptr = &candidate_anchor_rect_;
        } else if (observed_caret >= 0 &&
                   observed_caret != candidate_anchor_caret_utf16_) {
          // The caret moved after the anchor was verified. The menu and its write
          // authorization die together.
          TraceStage("candidate_token_caret_moved", observed_caret);
          candidate_dismissed_version_ = snapshot.version;
          candidate_ui_->End();
          candidate_selection_ = -1;
          candidate_anchor_version_ = 0;
          candidate_anchor_caret_utf16_ = -1;
          return;
        }
      }
    }
  }
  TraceStage("candidate_anchor_present", anchor_ptr != nullptr ? 1 : 0);
  if (wants_window && anchor_ptr == nullptr) {
    if (candidate_anchor_retry_budget_.TryAcquire()) {
      // Keep the same token and menu version alive; the window stays hidden until a
      // verified rectangle exists. The paced waker performs the next attempt.
      candidate_anchor_retry_ = true;
      TraceStage("candidate_anchor_retry",
                 static_cast<long long>(candidate_anchor_retry_budget_.used()));
    } else {
      // No layout or no readable caret after the per-token budget. Fail closed instead of
      // leaving a hidden menu that could still accept a keyboard selection.
      TraceStage("candidate_anchor_retry_exhausted", 1);
      candidate_dismissed_version_ = snapshot.version;
      candidate_ui_->End();
      candidate_selection_ = -1;
      candidate_anchor_version_ = 0;
      candidate_anchor_caret_utf16_ = -1;
      ResetCandidateAnchorRetry();
      return;
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
      selection.caret_utf16 != snapshot.caret_utf16 ||
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
  if (coordinator_) {
    const coordinator::CandidateSnapshot snapshot = coordinator_->candidate_snapshot();
    if (snapshot_version != 0 &&
        (snapshot.version != snapshot_version ||
         snapshot.identity.context_id != context_id ||
         snapshot.identity.generation != generation)) {
      // A late abort from an element of another menu/document must not dismiss this one.
      return;
    }
    candidate_dismissed_version_ =
        snapshot_version != 0 ? snapshot_version : snapshot.version;
  }
  candidate_selection_ = -1;
  candidate_anchor_version_ = 0;
  candidate_anchor_caret_utf16_ = -1;
  if (candidate_ui_) {
    candidate_ui_->SetSelection(-1);
    candidate_ui_->End();
  }
}

void ProductTextController::OnUiShowRequested() {
  if (!candidate_ui_) {
    return;
  }
  // The host called Show(TRUE). Invalidate the cached presentation so the next paced wake
  // re-reads the live caret and token rectangle; this callback never starts a TSF session.
  candidate_anchor_version_ = 0;
  candidate_anchor_caret_utf16_ = -1;
  candidate_anchor_rect_ = {};
  ResetCandidateAnchorRetry();
  waker_.SetPacing(true);
  TraceStage("candidate_show_requested", 1);
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
  // Original text is already in its document. Only the cancelable suggestion stream dies
  // here; nothing is retracted, replayed or attributed to the user.
  suggestion_queue_.clear();
  suggestion_in_flight_ = false;
  suggestion_engaged_ = false;
  suggestion_work_id_ = 0;
  suggestion_stale_ = true;
  stream_reset_owed_ = false;
  suggestion_in_flight_reset_ = false;
  if (replacement_pending_) {
    ++replacement_refused_;
    replacement_pending_ = false;
    replacement_work_id_ = 0;
  }
  host_token_units_ = 0;
  token_abandoned_ = false;
  predicted_host_caret_utf16_ = -1;
  host_token_origin_valid_ = false;
  host_token_origin_caret_utf16_ = -1;
  host_edits_expected_ = 0;
  pending_host_edit_carets_.clear();
  if (ui_context_ != nullptr) {
    ui_context_->Release();
    ui_context_ = nullptr;
  }
  candidate_selection_ = -1;
  candidate_ui_version_ = 0;
  candidate_anchor_version_ = 0;
  candidate_anchor_caret_utf16_ = -1;
  ResetCandidateAnchorRetry();
  DismissCandidateMenu();
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
  // A fresh binding starts with a fresh engine client and a fresh suggestion stream. The
  // host's text is unaffected and is never re-read or replayed.
  suggestion_queue_.clear();
  suggestion_in_flight_ = false;
  suggestion_engaged_ = false;
  suggestion_work_id_ = 0;
  suggestion_stale_ = false;
  stream_reset_owed_ = false;
  replacement_pending_ = false;
  replacement_work_id_ = 0;
  host_token_units_ = 0;
  token_abandoned_ = false;
  predicted_host_caret_utf16_ = target.caret_utf16;
  host_token_origin_valid_ = false;
  host_token_origin_caret_utf16_ = -1;
  host_edits_expected_ = 0;
  pending_host_edit_carets_.clear();
  candidate_anchor_version_ = 0;
  candidate_anchor_caret_utf16_ = -1;
  ResetCandidateAnchorRetry();
  DismissCandidateMenu();
  binding_->OnTargetBound(context, target);
  waker_.SetPacing(true);
}

void ProductTextController::OnHostEditCompleted(
    ITfContext* context, const ctx::ObservedTarget& target,
    const ctx::SelectionSnapshot& selection) {
  // The caret mirror is updated; the product never reads, copies or recovers this text.
  if (binding_) {
    binding_->OnHostEditCompleted(context, target, selection);
  }
  if (selection.ok()) {
    predicted_host_caret_utf16_ = selection.caret_utf16;
  } else {
    predicted_host_caret_utf16_ = -1;
  }
  if (host_edits_expected_ > 0 && selection.ok()) {
    // Match the notification against the projected caret history of our own passed-through
    // keys. Hosts may deliver one notification per key or coalesce several; both are
    // accepted by consuming the matching prefix. A caret that is not in this history (mouse
    // move, paste, autocorrect, another client) is not ours.
    std::size_t consumed = 0;
    for (std::size_t index = 0; index < pending_host_edit_carets_.size(); ++index) {
      if (pending_host_edit_carets_[index] >= 0 &&
          pending_host_edit_carets_[index] == selection.caret_utf16) {
        consumed = index + 1;
        break;
      }
    }
    if (consumed != 0) {
      for (std::size_t index = 0; index < consumed; ++index) {
        pending_host_edit_carets_.pop_front();
      }
      host_edits_expected_ =
          host_edits_expected_ > consumed ? host_edits_expected_ - consumed : 0;
      ++host_edits_matched_;
      if (host_edits_expected_ == 0 && host_token_origin_valid_ &&
          host_token_units_ > 0 &&
          host_token_origin_caret_utf16_ + static_cast<LONG>(host_token_units_) !=
              selection.caret_utf16) {
        // Even after coalescing, the token must end where its recorded origin and typed
        // units say. Never rebase the anchor onto a different position.
        TraceStage("host_edit_token_mismatch", selection.caret_utf16);
        host_token_origin_valid_ = false;
        host_token_origin_caret_utf16_ = -1;
        candidate_dismissed_version_ = coordinator_
                                           ? coordinator_->candidate_snapshot().version
                                           : candidate_dismissed_version_;
        DismissCandidateMenu();
      }
      return;
    }
  }
  ++host_edits_unexpected_;
  host_edits_expected_ = 0;
  pending_host_edit_carets_.clear();
  host_token_origin_valid_ = false;
  host_token_origin_caret_utf16_ = -1;
  ResetCandidateAnchorRetry();
  // A document change that none of our passed-through keys explains (paste, autocorrect,
  // another client). The engine's token no longer matches the document: stop suggesting and
  // rebuild at the next token. No text is touched. host_token_units_ is reset because the
  // caret mirror now marks a new, unknown token start; the next typed token is complete and
  // may be suggested normally.
  MarkSuggestionStale();
  token_abandoned_ = false;
  host_token_units_ = 0;
  QueueEngineReset();
}

void ProductTextController::OnOwnEditCompleted(
    ITfContext* context, const ctx::ObservedTarget& target,
    const ctx::SelectionSnapshot& selection) {
  binding_->OnOwnEditCompleted(context, target, selection);
  predicted_host_caret_utf16_ = selection.ok() ? selection.caret_utf16 : -1;
  host_token_origin_valid_ = false;
  host_token_origin_caret_utf16_ = -1;
  ResetCandidateAnchorRetry();
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
  diagnostics.host_text_keys = host_text_keys_;
  diagnostics.suggestion_queued = suggestion_queued_;
  diagnostics.suggestion_submitted = suggestion_submitted_;
  diagnostics.suggestion_completed = suggestion_completed_;
  diagnostics.suggestion_dropped = suggestion_dropped_;
  diagnostics.suggestion_stale = suggestion_stale_events_;
  diagnostics.suggestion_resets = suggestion_resets_;
  diagnostics.replacement_attempts = replacement_attempts_;
  diagnostics.replacement_applied = replacement_applied_;
  diagnostics.replacement_refused = replacement_refused_;
  diagnostics.last_replacement_outcome = last_replacement_outcome_;
  diagnostics.last_replacement_edit_status = last_replacement_edit_status_;
  diagnostics.last_replacement_edit_reason = last_replacement_edit_reason_;
  diagnostics.last_replacement_expected_units = last_replacement_expected_units_;
  diagnostics.internal_text_losses = internal_text_losses_;
  diagnostics.host_edits_matched = host_edits_matched_;
  diagnostics.host_edits_unexpected = host_edits_unexpected_;
  diagnostics.reset_boundary = reset_boundary_;
  diagnostics.reset_owed = reset_owed_;
  diagnostics.reset_submit_refusal = reset_submit_refusal_;
  diagnostics.reset_replacement = reset_replacement_;
  diagnostics.host_token_units = host_token_units_;
  diagnostics.suggestion_queue_depth = suggestion_queue_.size();
  diagnostics.suggestion_active =
      suggestion_in_flight_ || !suggestion_queue_.empty() || replacement_pending_;
  diagnostics.suggestion_in_flight = suggestion_in_flight_;
  diagnostics.token_abandoned = token_abandoned_;
  diagnostics.suggestion_stale_now = suggestion_stale_;
  diagnostics.replacement_pending = replacement_pending_;
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
    diagnostics.candidate_caret_utf16 = candidates.caret_utf16;
    diagnostics.candidate_input_units = candidates.input_utf16_code_units.size();
    diagnostics.candidate_items = candidates.items.size();
  }
  diagnostics.candidate_selected = candidate_selection_;
  diagnostics.candidate_stale_selections = candidate_stale_selections_;
  diagnostics.candidate_anchor_caret_utf16 = candidate_anchor_caret_utf16_;
  std::int32_t expected_caret = -1;
  diagnostics.candidate_anchor_expected_utf16 =
      ExpectedCandidateCaretUtf16(&expected_caret) ? expected_caret : -1;
  diagnostics.candidate_anchor_retries = candidate_anchor_retry_budget_.used();
  diagnostics.candidate_token_origin_valid = host_token_origin_valid_;
  diagnostics.candidate_token_origin_caret_utf16 = host_token_origin_caret_utf16_;
  diagnostics.predicted_host_caret_utf16 = predicted_host_caret_utf16_;
  if (candidate_ui_) {
    diagnostics.candidate_ui = candidate_ui_->Diagnostics();
  }
  if (observer_) {
    diagnostics.observer = observer_->Snapshot();
  }
  if (binding_) {
    diagnostics.binding = binding_->Snapshot();
  }
  diagnostics.pacing = waker_.Snapshot().paced;
  return diagnostics;
}

}  // namespace product
}  // namespace tsf
}  // namespace epocanva
