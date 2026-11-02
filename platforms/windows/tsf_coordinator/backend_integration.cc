// Unified backend work remains inside the coordinator's existing single-flight gates.
#include "coordinator.h"
#include "text_encoding.h"

namespace epocanva::tsf::coordinator {
namespace {
bool Utf16(const std::string& utf8, std::u16string* out) {
  const auto copied = engine::CaptureEngineText(utf8);
  if (!copied.ok()) return false;
  *out = copied.text.utf16_code_units; return true;
}
CandidateKind Kind(std::uint32_t value) {
  switch (value) {
    case EPOCANVA_BACKEND_RAW: return CandidateKind::kLiteral;
    case EPOCANVA_BACKEND_SPELLING: return CandidateKind::kSpelling;
    case EPOCANVA_BACKEND_MORPHOLOGY: return CandidateKind::kMorphology;
    case EPOCANVA_BACKEND_EXPANSION: return CandidateKind::kExpansion;
    case EPOCANVA_BACKEND_CONVERSION: return CandidateKind::kConversion;
    case EPOCANVA_BACKEND_PREDICTION: return CandidateKind::kPrediction;
    default: return CandidateKind::kCompletion;
  }
}
}
void ContextCoordinator::SetBackendAccess(const engine::BackendAccess& access) {
  if (!unified_backend() || !bound_ || access.context_target != identity_.context_id ||
      access.generation != identity_.generation) return;
  backend_access_ = access;
}
void ContextCoordinator::RevokeBackendAccess() {
  if (backend_access_.current) backend_access_.current->store(false, std::memory_order_release);
}
SubmitStatus ContextCoordinator::SubmitBackendWork(engine::BackendWork work, WorkId* id) {
  if (!id || !unified_backend()) return RecordSubmit(SubmitStatus::kInvalidRequest);
  *id = {};
  if (closed_) return RecordSubmit(SubmitStatus::kClosed);
  if (phase_ != WorkPhase::kIdle) return RecordSubmit(SubmitStatus::kBusy);
  if (!bound_) return RecordSubmit(SubmitStatus::kNotBound);
  if (RetirementPausesInput()) return RecordSubmit(SubmitStatus::kRetiring);
  SubmitStatus gate;
  if (GateBlocksSubmit(&gate)) return RecordSubmit(gate);
  const auto target = RevalidateForSubmit();
  if (target != SubmitStatus::kAccepted) { RevokeBackendAccess(); return RecordSubmit(target); }
  if (!backend_access_.allowed()) return RecordSubmit(SubmitStatus::kEngineClientUnavailable);
  work.access = backend_access_;
  if (work.operation != engine::BackendOperation::kSelect)
    backend_caret_utf16_ = view_.caret_utf16;
  if (!StartPermissionQuery()) return RecordSubmit(SubmitStatus::kPermissionUnknown);
  if (permission_source_.Status() == context::PermissionQueryStatus::kPending) {
    FinishWorkLedger(WorkKind::kBackendWork);
    phase_ = WorkPhase::kPermissionPending;
    pending_permission_ = {};
    pending_permission_.kind = WorkKind::kBackendWork;
    pending_permission_.backend_work = std::move(work);
    *id = WorkId{work_id_};
    return RecordSubmit(SubmitStatus::kAccepted);
  }
  bool current = false;
  if (!PermissionVerdictAllows(&current)) return RecordSubmit(
      permission_ == edit::InputPermission::kDenied ? SubmitStatus::kPermissionDenied : SubmitStatus::kPermissionUnknown);
  if (RevalidateForSubmit() != SubmitStatus::kAccepted) {
    RevokeBackendAccess(); return RecordSubmit(SubmitStatus::kTargetChanged);
  }
  engine::RequestId request;
  const auto submitted = executor_.TryBackendWork(engine_client_, work, &request);
  if (submitted == engine::SubmitStatus::kAccepted) {
    backend_work_ = std::move(work); FinishSubmit(request, WorkKind::kBackendWork);
    *id = WorkId{work_id_}; return RecordSubmit(SubmitStatus::kAccepted);
  }
  if (submitted == engine::SubmitStatus::kQueueFull) return RecordSubmit(SubmitStatus::kQueueFull);
  if (submitted == engine::SubmitStatus::kNotRunning) return RecordSubmit(SubmitStatus::kEngineNotRunning);
  return RecordSubmit(SubmitStatus::kEngineSubmitFailed);
}
bool ContextCoordinator::UpdateBackendSnapshot(const engine::BackendSnapshotOutcome& input) {
  CandidateSnapshot snapshot;
  snapshot.version = ++candidate_version_;
  snapshot.backend_snapshot_token = input.snapshot_token;
  snapshot.profile_id = input.profile_id; snapshot.language = input.language;
  snapshot.identity = identity_; snapshot.generation = identity_.generation;
  snapshot.work_id = work_id_; snapshot.caret_utf16 = backend_caret_utf16_ >= 0
      ? backend_caret_utf16_ : view_.caret_utf16;
  snapshot.next_word = input.raw_utf8.empty();
  if (!Utf16(input.raw_utf8, &snapshot.input_utf16_code_units) ||
      !Utf16(input.preedit_utf8, &snapshot.preedit_utf16_code_units)) return false;
  if (input.candidate_tokens.size() != input.candidate_text_utf8.size() ||
      input.candidate_kind.size() != input.candidate_text_utf8.size()) return false;
  for (std::size_t ordinal = 0; ordinal < input.candidate_text_utf8.size(); ++ordinal) {
    CandidateItem item;
    if (!Utf16(input.candidate_text_utf8[ordinal], &item.text_utf16_code_units)) return false;
    item.kind = Kind(input.candidate_kind[ordinal]); item.page_index = ordinal;
    item.menu_index = ordinal; item.backend_candidate_token = input.candidate_tokens[ordinal];
    snapshot.items.push_back(std::move(item));
  }
  snapshot.page_size = static_cast<int>(snapshot.items.size());
  snapshot.valid = !snapshot.items.empty() && input.snapshot_token != 0;
  engine_composing_ = !snapshot.input_utf16_code_units.empty();
  candidate_snapshot_ = std::move(snapshot);
  if (candidate_snapshot_.valid) ++candidate_snapshots_;
  return true;
}
void ContextCoordinator::ResolveBackendResult(const engine::BackendWorkOutcome& result) {
  if (backend_ack_in_flight_) {
    // Host outcome is already known. A persistence/backend failure cannot undo that fact.
    backend_ack_in_flight_ = false;
    backend_commit_token_ = 0;
    const auto outcome = backend_terminal_outcome_;
    const auto cause = backend_terminal_cause_;
    if (result.backend_status != EPOCANVA_OK || !result.acknowledgement.consumed)
      RevokeBackendAccess();
    EndWork(outcome, cause);
    return;
  }
  if (!EnsureTargetCurrent("target-changed-after-backend", true)) return;
  if (result.backend_status != EPOCANVA_OK) {
    EnterNeedsResync("backend-command-refused", RejectCause::kEngineRefused); return;
  }
  if (backend_work_.operation == engine::BackendOperation::kSelect) {
    backend_commit_token_ = result.commit.commit_token;
    if (!backend_commit_token_ || !result.commit.awaiting_ack || result.commit.text_utf8.empty()) {
      EnterNeedsResync("backend-commit-invalid", RejectCause::kEngineRefused); return;
    }
    if (!result.commit.remaining_preedit_utf8.empty()) {
      // Existing TSF single edit cannot atomically commit and retain a remaining preedit.
      EndWork(WorkOutcome::kUnsupportedShape, RejectCause::kNone); return;
    }
    std::u16string committed;
    if (!Utf16(result.commit.text_utf8, &committed)) {
      EndWork(WorkOutcome::kRejected, RejectCause::kEditRefused); return;
    }
    InvalidateCandidateSnapshot();
    work_kind_ = WorkKind::kSuggestionReplace;
    // The already-selected backend commit is the only source of replacement text.
    // SubmitSuggestionPlan uses the existing range equality, caret, generation and callback
    // permission/target rechecks. The commit token survives until that callback resolves.
    if (!StartPermissionQuery()) { EndWork(WorkOutcome::kRejected, RejectCause::kPermissionUnknown); return; }
    if (permission_source_.Status() == context::PermissionQueryStatus::kPending) {
      phase_ = WorkPhase::kPermissionPending; pending_permission_ = {};
      pending_permission_.kind = WorkKind::kSuggestionReplace;
      pending_permission_.replace_expected = backend_expected_utf16_;
      pending_permission_.replace_text = std::move(committed);
      pending_permission_.replace_caret_utf16 = backend_caret_utf16_;
      return;
    }
    bool current = false;
    if (!PermissionVerdictAllows(&current)) { EndWork(WorkOutcome::kRejected, RejectCause::kPermissionUnknown); return; }
    SubmitSuggestionPlan(backend_expected_utf16_, std::move(committed), backend_caret_utf16_);
    return;
  }
  if (!UpdateBackendSnapshot(result.snapshot)) {
    EnterNeedsResync("backend-snapshot-invalid", RejectCause::kEngineRefused); return;
  }
  EndWork(WorkOutcome::kNoEdit, RejectCause::kNone);
}
bool ContextCoordinator::QueueBackendAck(WorkOutcome outcome, RejectCause cause) {
  backend_terminal_outcome_ = outcome; backend_terminal_cause_ = cause;
  phase_ = WorkPhase::kBackendAckPending;
  PumpBackendAck(); return true;
}
void ContextCoordinator::PumpBackendAck() {
  if (!backend_commit_token_ || !bound_) return;
  engine::BackendWork work;
  work.operation = engine::BackendOperation::kAcknowledgeCommit;
  work.access = backend_access_; work.commit_token = backend_commit_token_;
  // Only callback-confirmed Applied is an affirmative acknowledgement. Personal learning
  // remains disabled in the worker; a cached permission value is not a field lock.
  work.host_applied = backend_terminal_outcome_ == WorkOutcome::kApplied &&
      permission_ == edit::InputPermission::kAllowed &&
      RevalidateForSubmit() == SubmitStatus::kAccepted;
  engine::RequestId request;
  const auto submitted = executor_.TryBackendWork(engine_client_, work, &request);
  if (submitted == engine::SubmitStatus::kQueueFull) return;
  if (submitted == engine::SubmitStatus::kAccepted) {
    backend_ack_in_flight_ = true; engine_request_ = request; phase_ = WorkPhase::kEnginePending; return;
  }
  backend_commit_token_ = 0; RevokeBackendAccess();
  EndWork(backend_terminal_outcome_, backend_terminal_cause_);
}
}  // namespace epocanva::tsf::coordinator
