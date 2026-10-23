// EpoCanva P3.4 -- controller implementation. See input_controller.h for the contract.
#include "input_controller.h"

#include <utility>

namespace epocanva {
namespace windows {
namespace engine {
namespace {

void AppendDiagnostic(std::string* target, const std::string& value) {
  if (value.empty()) {
    return;
  }
  if (!target->empty()) {
    *target += "; ";
  }
  *target += value;
}

bool CommitResultIsClean(SessionCommitStatus status) {
  return status == SessionCommitStatus::kNoCommit || status == SessionCommitStatus::kRead;
}

}  // namespace

bool MapDecisionToEngineKey(const input::KeyDecision& decision, EngineKey* out_key) {
  if (out_key == nullptr) {
    return false;
  }
  switch (decision.disposition) {
    case input::KeyDisposition::kEngineTextCandidate:
      // The scalar P3.3 accepted from the completed translation. Printable-ASCII keysyms equal
      // their code points; no VK lookup and no shift/caps re-conversion happens here.
      if (decision.ascii_code_point < 0x21u || decision.ascii_code_point > 0x7Eu) {
        return false;
      }
      out_key->keysym = static_cast<int>(decision.ascii_code_point);
      out_key->modifiers = 0;
      return true;
    case input::KeyDisposition::kEngineSpaceCandidate:
      out_key->keysym = kEngineKeysymSpace;
      out_key->modifiers = 0;
      return true;
    case input::KeyDisposition::kEngineEnterCandidate:
      out_key->keysym = kEngineKeysymReturn;
      out_key->modifiers = 0;
      return true;
    case input::KeyDisposition::kEngineBackSpaceCandidate:
      out_key->keysym = kEngineKeysymBackSpace;
      out_key->modifiers = 0;
      return true;
    case input::KeyDisposition::kEngineEscapeCandidate:
      out_key->keysym = kEngineKeysymEscape;
      out_key->modifiers = 0;
      return true;
    case input::KeyDisposition::kPassThroughToHost:
    case input::KeyDisposition::kShortcutPassThrough:
    case input::KeyDisposition::kCompositionBoundaryRequired:
      return false;
  }
  return false;
}

InputController::InputController(RimeRuntime& runtime, RimeSession* session,
                                 ControllerOptions options)
    : runtime_(&runtime), session_(session), options_(options) {
  if (OwnerThread()) {
    (void)RefreshState();
  }
}

InputController::~InputController() = default;

bool InputController::OwnerThread() const {
  return runtime_ != nullptr && runtime_->IsOwnerThread();
}

bool InputController::SetInputEnabled(bool enabled) {
  if (!OwnerThread()) {
    return false;
  }
  options_.input_enabled = enabled;
  return true;
}

bool InputController::SetPermission(input::InputPermission permission) {
  if (!OwnerThread()) {
    return false;
  }
  options_.permission = permission;
  return true;
}

KeyEvaluation InputController::Evaluate(const input::KeyEventSnapshot& event,
                                        const input::KeyText& text) {
  KeyEvaluation evaluation;
  ++stats_.evaluations;

  if (runtime_ == nullptr) {
    evaluation.status = ControllerStatus::kNoSession;
    evaluation.diagnostic = "no runtime";
    return evaluation;
  }
  if (!OwnerThread()) {
    evaluation.status = ControllerStatus::kWrongThread;
    evaluation.diagnostic = "Evaluate refused: caller is not the runtime owner thread";
    return evaluation;
  }
  if (session_ == nullptr || !session_->IsOpen()) {
    evaluation.status = ControllerStatus::kNoSession;
    evaluation.diagnostic = "Evaluate refused: no open session";
    return evaluation;
  }
  if (commit_fault_) {
    evaluation.status = ControllerStatus::kEngineFaulted;
    evaluation.diagnostic =
        "Evaluate refused: a commit was consumed but not copied; close and recreate the "
        "session";
    return evaluation;
  }
  if (!cache_valid_ || state_fault_) {
    evaluation.status = ControllerStatus::kStateUnavailable;
    evaluation.diagnostic = "Evaluate refused: the cached engine state is unavailable";
    return evaluation;
  }

  input::InputContext context;
  context.input_enabled = options_.input_enabled;
  context.engine_ready =
      !cached_.service_disabled && cached_.valid;
  context.composition_active = cached_.is_composing;
  context.permission = options_.permission;

  evaluation.decision = input::DecideKey(event, text, context);
  evaluation.composition_active = cached_.is_composing;
  evaluation.can_dispatch = evaluation.decision.is_engine_candidate();
  evaluation.status = ControllerStatus::kOk;
  return evaluation;
}

bool InputController::CanDispatch(const KeyEvaluation& evaluation) const {
  return evaluation.status == ControllerStatus::kOk &&
         evaluation.can_dispatch &&
         !evaluation.decision.requires_composition_boundary();
}

DispatchOutcome InputController::Dispatch(const input::KeyEventSnapshot& event,
                                          const input::KeyText& text) {
  DispatchOutcome outcome;
  ++stats_.dispatch_requests;

  const KeyEvaluation evaluation = Evaluate(event, text);
  outcome.decision = evaluation.decision;
  if (evaluation.status != ControllerStatus::kOk) {
    outcome.status = DispatchStatus::kNotDispatched;
    outcome.diagnostic = evaluation.diagnostic;
    ++stats_.not_dispatched;
    return outcome;
  }
  if (evaluation.decision.requires_composition_boundary()) {
    // Explicitly unresolved. The composition is left exactly as it was, and no claim is made
    // that a pass-through would be the right ordering.
    outcome.status = DispatchStatus::kDeferredCompositionBoundary;
    outcome.diagnostic = "composition boundary requires a later coordinator; nothing dispatched";
    ++stats_.deferred_boundary;
    return outcome;
  }
  if (!evaluation.decision.is_engine_candidate()) {
    outcome.status = DispatchStatus::kNotDispatched;
    outcome.diagnostic = "the decision is not an engine candidate";
    ++stats_.not_dispatched;
    return outcome;
  }

  EngineKey key;
  if (!MapDecisionToEngineKey(evaluation.decision, &key)) {
    outcome.status = DispatchStatus::kNotDispatched;
    outcome.diagnostic = "the decision could not be mapped to an engine key";
    ++stats_.not_dispatched;
    return outcome;
  }

  const SessionKeyResult call = session_->ProcessKeyOnce(key.keysym, key.modifiers);
  if (call.status != SessionKeyStatus::kCallMade) {
    outcome.status = DispatchStatus::kNotDispatched;
    outcome.diagnostic = std::string("process_key was not made: ") +
                         DescribeSessionKeyStatus(call.status) + ": " + call.diagnostic;
    ++stats_.not_dispatched;
    return outcome;
  }
  outcome.engine_key_call_made = true;
  outcome.engine_reported_handled = call.engine_reported_handled;
  ++stats_.engine_key_calls;

  const SessionStateResult state = session_->CaptureState();
  const SessionCommitResult commit = session_->ReadCommit();
  outcome.state_status = state.status;
  outcome.commit_status = commit.status;
  if (state.ok()) {
    outcome.state = state.state;
  }
  outcome.pending_commit = commit.commit;

  std::string observation_diagnostic;
  const ControllerStatus observed =
      Observe(state.status, commit.status, state.state, &observation_diagnostic);
  AppendDiagnostic(&outcome.diagnostic, observation_diagnostic);

  if (observed == ControllerStatus::kOk) {
    if (call.engine_reported_handled) {
      outcome.status = DispatchStatus::kDispatchedHandled;
      ++stats_.dispatched_handled;
    } else {
      outcome.status = DispatchStatus::kDispatchedUnhandled;
      ++stats_.dispatched_unhandled;
    }
    return outcome;
  }

  // The engine was entered. This is a named post-dispatch failure, never a "not handled"
  // result that would invite a host to replay the key.
  outcome.status = DispatchStatus::kDispatchedStateFailed;
  ++stats_.dispatched_state_failed;
  return outcome;
}

CandidateOutcome InputController::AcceptCandidate(std::size_t candidate_index) {
  CandidateOutcome outcome;
  ++stats_.selection_requests;

  if (runtime_ == nullptr) {
    outcome.status = CandidateStatus::kNotReady;
    outcome.diagnostic = "no runtime";
    return outcome;
  }
  if (!OwnerThread()) {
    outcome.status = CandidateStatus::kWrongThread;
    outcome.diagnostic = "AcceptCandidate refused: caller is not the runtime owner thread";
    return outcome;
  }
  if (session_ == nullptr || !session_->IsOpen()) {
    outcome.status = CandidateStatus::kNotReady;
    outcome.diagnostic = "AcceptCandidate refused: no open session";
    return outcome;
  }
  if (commit_fault_) {
    outcome.status = CandidateStatus::kNotReady;
    outcome.diagnostic =
        "AcceptCandidate refused: a commit was consumed but not copied; close and recreate "
        "the session";
    return outcome;
  }
  if (!cache_valid_ || state_fault_) {
    outcome.status = CandidateStatus::kNotReady;
    outcome.diagnostic = "AcceptCandidate refused: the cached engine state is unavailable";
    return outcome;
  }
  if (candidate_index >= cached_.candidates.size()) {
    outcome.status = CandidateStatus::kOutOfRange;
    outcome.diagnostic = "candidate index is outside the cached page";
    ++stats_.selection_out_of_range;
    return outcome;
  }

  const SessionSelectionResult call =
      session_->SelectCandidateOnce(candidate_index);
  if (call.status != SessionSelectionStatus::kCallMade) {
    outcome.status = CandidateStatus::kNotReady;
    outcome.diagnostic = std::string("select_candidate was not made: ") +
                         DescribeSessionSelectionStatus(call.status) + ": " +
                         call.diagnostic;
    return outcome;
  }
  outcome.engine_selection_call_made = true;
  outcome.engine_reported_accepted = call.engine_reported_accepted;
  ++stats_.selection_calls;

  const SessionStateResult state = session_->CaptureState();
  const SessionCommitResult commit = session_->ReadCommit();
  outcome.state_status = state.status;
  outcome.commit_status = commit.status;
  if (state.ok()) {
    outcome.state = state.state;
  }
  outcome.pending_commit = commit.commit;

  std::string observation_diagnostic;
  const ControllerStatus observed =
      Observe(state.status, commit.status, state.state, &observation_diagnostic);
  AppendDiagnostic(&outcome.diagnostic, observation_diagnostic);
  if (observed != ControllerStatus::kOk) {
    outcome.status = CandidateStatus::kStateFailed;
    return outcome;
  }
  outcome.status = call.engine_reported_accepted ? CandidateStatus::kAccepted
                                                 : CandidateStatus::kRejectedByEngine;
  return outcome;
}

ControllerStatus InputController::RefreshState() {
  if (runtime_ == nullptr) {
    return ControllerStatus::kNoSession;
  }
  if (!OwnerThread()) {
    return ControllerStatus::kWrongThread;
  }
  if (session_ == nullptr || !session_->IsOpen()) {
    return ControllerStatus::kNoSession;
  }
  if (commit_fault_) {
    return ControllerStatus::kEngineFaulted;
  }

  ++stats_.state_refreshes;
  const SessionStateResult state = session_->CaptureState();
  if (!state.ok()) {
    ++stats_.state_refresh_failures;
    cache_valid_ = false;
    state_fault_ = true;
    cached_ = EngineState();
    return state.status == SessionStateStatus::kClosed ? ControllerStatus::kNoSession
                                                       : ControllerStatus::kStateUnavailable;
  }
  cached_ = state.state;
  cache_valid_ = true;
  state_fault_ = false;
  return ControllerStatus::kOk;
}

ControllerStatus InputController::Observe(SessionStateStatus state_status,
                                          SessionCommitStatus commit_status,
                                          EngineState state,
                                          std::string* diagnostic) {
  const bool state_ok = state_status == SessionStateStatus::kOk && state.valid;
  const bool commit_ok = CommitResultIsClean(commit_status);

  if (state_ok) {
    cached_ = std::move(state);
    cache_valid_ = true;
    state_fault_ = false;
  } else {
    cache_valid_ = false;
    state_fault_ = true;
    cached_ = EngineState();
    AppendDiagnostic(diagnostic, std::string("state read failed: ") +
                                     DescribeSessionStateStatus(state_status));
  }

  if (!commit_ok) {
    // The engine queue may have been advanced (kFreeFailed/conversion failure) or not
    // (wrong thread/closed). Either way a commit can no longer be attributed safely, so the
    // controller stops dispatching until the session is recreated.
    commit_fault_ = true;
    cache_valid_ = false;
    AppendDiagnostic(diagnostic, std::string("commit read failed: ") +
                                     DescribeSessionCommitStatus(commit_status));
  }

  if (state_ok && commit_ok) {
    return ControllerStatus::kOk;
  }
  return commit_fault_ ? ControllerStatus::kEngineFaulted
                       : ControllerStatus::kStateUnavailable;
}

const char* DescribeControllerStatus(ControllerStatus status) {
  switch (status) {
    case ControllerStatus::kOk:
      return "ok";
    case ControllerStatus::kWrongThread:
      return "wrong-thread";
    case ControllerStatus::kNoSession:
      return "no-session";
    case ControllerStatus::kStateUnavailable:
      return "state-unavailable";
    case ControllerStatus::kEngineFaulted:
      return "engine-faulted";
  }
  return "unknown-status";
}

const char* DescribeDispatchStatus(DispatchStatus status) {
  switch (status) {
    case DispatchStatus::kNotDispatched:
      return "not-dispatched";
    case DispatchStatus::kDeferredCompositionBoundary:
      return "deferred-composition-boundary";
    case DispatchStatus::kDispatchedUnhandled:
      return "dispatched-unhandled";
    case DispatchStatus::kDispatchedHandled:
      return "dispatched-handled";
    case DispatchStatus::kDispatchedStateFailed:
      return "dispatched-state-failed";
  }
  return "unknown-status";
}

const char* DescribeCandidateStatus(CandidateStatus status) {
  switch (status) {
    case CandidateStatus::kAccepted:
      return "accepted";
    case CandidateStatus::kRejectedByEngine:
      return "rejected-by-engine";
    case CandidateStatus::kOutOfRange:
      return "out-of-range";
    case CandidateStatus::kNotReady:
      return "not-ready";
    case CandidateStatus::kWrongThread:
      return "wrong-thread";
    case CandidateStatus::kStateFailed:
      return "state-failed";
  }
  return "unknown-status";
}

}  // namespace engine
}  // namespace windows
}  // namespace epocanva
