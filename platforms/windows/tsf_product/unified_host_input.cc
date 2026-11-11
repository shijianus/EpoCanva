#include "text_service_controller.h"
#include "product_engine_bootstrap.h"
namespace epocanva::tsf::product {
namespace engine = ::epocanva::windows::engine;
bool ProductTextController::HandleUnifiedHostKey(const BuiltKey& key) {
  // Ordinary input always belongs to the host. The real edit notification schedules the
  // bounded context read; translated key text is never treated as proof of host success.
  const auto vk = key.event.virtual_key;
  const bool shortcut = key.event.modifiers.control || key.event.modifiers.alt || key.event.modifiers.windows;
  if (shortcut) { DismissCandidateMenu(); ++not_eaten_; return false; }
  if (IsTextKey(key)) ++host_text_keys_;
  if (IsTextKey(key) || vk == input::kVirtualKeyBack || vk == input::kVirtualKeyReturn ||
      vk == input::kVirtualKeySpace) {
    DismissCandidateMenu();
    host_text_changed_ = true;
    host_context_read_owed_ = false;  // old prefix cannot be published during the host edit
    ++host_context_read_revision_;
  }
  ++not_eaten_; return false;
}
void ProductTextController::PumpUnifiedHostContext() {
  if (!coordinator_ || !observer_ || !engine_ || !ui_context_) return;
  const auto state = coordinator_->Snapshot();
  if (host_context_work_in_flight_) {
    if (state.phase != coordinator::WorkPhase::kIdle) return;
    host_context_work_in_flight_ = false;
    if (state.outcome != coordinator::WorkOutcome::kNoEdit) {
      suggestion_stale_ = true; token_abandoned_ = true; return;
    }
    // A newer host edit arrived while the older snapshot was in flight. It never becomes a
    // selectable menu for the newer caret/token; the latest bounded read replaces it.
    if (host_context_submitted_revision_ != host_context_read_revision_) DismissCandidateMenu();
  }
  if (!host_context_read_owed_ || replacement_pending_ || state.phase != coordinator::WorkPhase::kIdle ||
      state.retirement.retiring_bindings || state.gate != coordinator::CoordinatorGate::kOpen ||
      !state.bound || host_context_caret_utf16_ < 0) return;
  const auto captured = ReadHostContext(ui_context_, client_id_, host_context_caret_utf16_,
      observer_->CreateWriteGuard(), state.identity.context_id, state.identity.generation);
  if (!captured.valid) {
    host_context_read_owed_ = false;
    DismissCandidateMenu(); return;
  }
  engine::BackendWork work;
  work.operation = engine::BackendOperation::kObserveHostText;
  work.preceding_utf8 = captured.preceding_utf8;
  work.text_utf8 = captured.token_utf8;
  work.event_token = ++host_context_event_token_;
  work.query_kind = captured.token_utf8.empty() ? EPOCANVA_BACKEND_NEXT_WORD : EPOCANVA_BACKEND_COMPLETION;
  coordinator::WorkId id;
  const auto status = coordinator_->SubmitBackendWork(std::move(work), &id);
  if (status == coordinator::SubmitStatus::kBusy || status == coordinator::SubmitStatus::kQueueFull ||
      status == coordinator::SubmitStatus::kRetiring) return;
  host_context_read_owed_ = false;
  if (status != coordinator::SubmitStatus::kAccepted) {
    DismissCandidateMenu(); return;
  }
  host_text_changed_ = false;  // this read describes the current host text
  host_context_work_in_flight_ = true;
  host_context_submitted_revision_ = host_context_read_revision_;
  host_token_units_ = captured.token_utf16.size();
  host_token_origin_valid_ = true;
  host_token_origin_caret_utf16_ = captured.token_start_utf16;
  predicted_host_caret_utf16_ = captured.caret_utf16;
  suggestion_stale_ = false; token_abandoned_ = false;
  candidate_dismissed_version_ = 0;
  ResetCandidateAnchorRetry();
}
}  // namespace epocanva::tsf::product
