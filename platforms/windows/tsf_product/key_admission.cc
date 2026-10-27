// EpoCanva W2 -- bounded admission queue implementation. See key_admission.h.
#include "key_admission.h"

#include <utility>

namespace epocanva {
namespace tsf {
namespace product {

const char* DescribeKeyState(KeyState state) {
  switch (state) {
    case KeyState::kNotAccepted:
      return "not-accepted";
    case KeyState::kAccepted:
      return "accepted";
    case KeyState::kEngineCompleted:
      return "engine-completed";
    case KeyState::kEditCompleted:
      return "edit-completed";
    case KeyState::kFailed:
      return "failed";
  }
  return "unknown";
}

const char* DescribeKeyFailure(KeyFailure failure) {
  switch (failure) {
    case KeyFailure::kNone:
      return "none";
    case KeyFailure::kFocusChanged:
      return "focus-changed";
    case KeyFailure::kBindingRetired:
      return "binding-retired";
    case KeyFailure::kPermissionRefused:
      return "permission-refused";
    case KeyFailure::kTargetChanged:
      return "target-changed";
    case KeyFailure::kEngineRefused:
      return "engine-refused";
    case KeyFailure::kQueueOverflow:
      return "queue-overflow";
    case KeyFailure::kPredictionMismatch:
      return "prediction-mismatch";
    case KeyFailure::kEngineUnavailable:
      return "engine-unavailable";
    case KeyFailure::kBlockedGate:
      return "blocked-gate";
    case KeyFailure::kRawCommitFailed:
      return "raw-commit-failed";
    case KeyFailure::kRawCapacity:
      return "raw-capacity";
    case KeyFailure::kShutdown:
      return "shutdown";
    case KeyFailure::kInternal:
      return "internal";
  }
  return "unknown";
}

bool HostTokenOwnership::IsTokenText(const input::KeyText& text) {
  if (text.status != input::KeyTextStatus::kText || text.utf16_code_units.empty()) {
    return false;
  }
  for (const char16_t unit : text.utf16_code_units) {
    if (unit > 0x7F || !input::IsForwardableAscii(unit)) {
      return false;
    }
  }
  return true;
}

void HostTokenOwnership::HostKept(const input::KeyText& text, bool shortcut) {
  if (shortcut) {
    return;  // a chord does not change what the current token is
  }
  owned_ = IsTokenText(text);
}

bool KeyAdmissionQueue::CanAccept(bool composition_active) const {
  const std::size_t limit = composition_active ? kCapacity : kNonComposingLimit;
  return queue_.size() < limit;
}

std::uint64_t KeyAdmissionQueue::Push(const input::KeyEventSnapshot& event,
                                      const input::KeyText& text,
                                      const input::KeyDecision& decision,
                                      std::uint64_t context_id,
                                      std::uint64_t generation) {
  AdmissionKey key;
  key.id = next_id_++;
  key.context_id = context_id;
  key.generation = generation;
  key.event = event;
  key.text = text;
  key.decision = decision;
  key.state = KeyState::kAccepted;
  queue_.push_back(std::move(key));
  return queue_.back().id;
}

void KeyAdmissionQueue::ConfirmEaten(std::uint64_t id) {
  if (in_flight_.has_value() && in_flight_->id == id) {
    if (!in_flight_->eaten) {
      in_flight_->eaten = true;
      ++counters_.accepted;
    }
    return;
  }
  for (AdmissionKey& key : queue_) {
    if (key.id == id) {
      if (!key.eaten) {
        key.eaten = true;
        ++counters_.accepted;
      }
      return;
    }
  }
}

void KeyAdmissionQueue::DropHeadNotEaten() {
  if (queue_.empty()) {
    return;
  }
  queue_.pop_front();
  ++counters_.not_eaten_after_prediction;
}

bool KeyAdmissionQueue::FailHead(KeyFailure failure, bool mark_eaten) {
  if (queue_.empty()) {
    return false;
  }
  AdmissionKey key = std::move(queue_.front());
  queue_.pop_front();
  if (mark_eaten && !key.eaten) {
    key.eaten = true;
    ++counters_.accepted;
  }
  key.state = KeyState::kFailed;
  key.failure = failure;
  CountFailure(failure);
  counters_.last_terminal_id = key.id;
  counters_.last_terminal_state = KeyState::kFailed;
  counters_.last_failure = failure;
  return true;
}

bool KeyAdmissionQueue::MoveHeadToInFlight(coordinator::WorkId work) {
  if (in_flight_.has_value() || queue_.empty()) {
    return false;
  }
  AdmissionKey key = std::move(queue_.front());
  queue_.pop_front();
  key.submitted = true;
  key.work = work;
  in_flight_ = std::move(key);
  in_flight_failed_ = false;
  ++counters_.dispatched;
  return true;
}

void KeyAdmissionQueue::MarkInFlightEngineCompleted() {
  if (!in_flight_.has_value() || in_flight_failed_) {
    return;
  }
  if (in_flight_->state == KeyState::kAccepted) {
    in_flight_->state = KeyState::kEngineCompleted;
    ++counters_.engine_completed;
  }
}

void KeyAdmissionQueue::MarkTerminal(KeyState state, KeyFailure failure) {
  counters_.last_terminal_id = in_flight_.has_value() ? in_flight_->id : 0;
  counters_.last_terminal_state = state;
  counters_.last_failure = failure;
}

void KeyAdmissionQueue::CountFailure(KeyFailure failure) {
  ++counters_.failed;
  switch (failure) {
    case KeyFailure::kFocusChanged:
      ++counters_.failed_focus;
      break;
    case KeyFailure::kBindingRetired:
      ++counters_.failed_binding_retired;
      break;
    case KeyFailure::kPermissionRefused:
      ++counters_.failed_permission;
      break;
    case KeyFailure::kTargetChanged:
      ++counters_.failed_target;
      break;
    case KeyFailure::kEngineRefused:
    case KeyFailure::kEngineUnavailable:
      ++counters_.failed_engine;
      break;
    case KeyFailure::kQueueOverflow:
      ++counters_.failed_queue_overflow;
      break;
    case KeyFailure::kPredictionMismatch:
      ++counters_.failed_prediction;
      break;
    case KeyFailure::kBlockedGate:
      ++counters_.failed_gate;
      break;
    case KeyFailure::kRawCommitFailed:
      ++counters_.failed_raw_commit;
      break;
    case KeyFailure::kRawCapacity:
      ++counters_.failed_raw_capacity;
      break;
    case KeyFailure::kShutdown:
      ++counters_.failed_shutdown;
      break;
    case KeyFailure::kInternal:
    case KeyFailure::kNone:
      ++counters_.failed_internal;
      break;
  }
}

void KeyAdmissionQueue::CompleteInFlight(KeyState state, KeyFailure failure) {
  if (!in_flight_.has_value()) {
    return;
  }
  in_flight_->state = state;
  in_flight_->failure = failure;
  if (state == KeyState::kEditCompleted) {
    ++counters_.edit_completed;
    MarkTerminal(state, KeyFailure::kNone);
  } else if (state == KeyState::kFailed) {
    CountFailure(failure);
    MarkTerminal(state, failure);
  }
  in_flight_.reset();
  in_flight_failed_ = false;
}

void KeyAdmissionQueue::MarkInFlightFailed(KeyFailure failure) {
  if (!in_flight_.has_value() || in_flight_failed_) {
    return;
  }
  in_flight_->state = KeyState::kFailed;
  in_flight_->failure = failure;
  in_flight_failed_ = true;
  CountFailure(failure);
  MarkTerminal(KeyState::kFailed, failure);
}

bool KeyAdmissionQueue::in_flight_failed() const { return in_flight_failed_; }

void KeyAdmissionQueue::ReleaseFailedInFlight() {
  if (!in_flight_.has_value()) {
    in_flight_failed_ = false;
    return;
  }
  in_flight_.reset();
  in_flight_failed_ = false;
}

std::size_t KeyAdmissionQueue::FailQueued(KeyFailure failure) {
  std::size_t count = 0;
  while (!queue_.empty()) {
    AdmissionKey key = std::move(queue_.front());
    queue_.pop_front();
    key.state = KeyState::kFailed;
    key.failure = failure;
    CountFailure(failure);
    counters_.last_terminal_id = key.id;
    counters_.last_terminal_state = KeyState::kFailed;
    counters_.last_failure = failure;
    ++count;
  }
  return count;
}

std::deque<AdmissionKey> KeyAdmissionQueue::WithdrawQueued() {
  std::deque<AdmissionKey> withdrawn;
  withdrawn.swap(queue_);
  return withdrawn;
}

void KeyAdmissionQueue::RecordDeferredRaw(std::size_t keys, std::size_t units) {
  counters_.deferred_raw_keys += keys;
  counters_.deferred_raw_units += units;
}

void KeyAdmissionQueue::RecordRawApplied(std::size_t keys, std::size_t units) {
  counters_.raw_applied_keys += keys;
  counters_.raw_applied_units += units;
}

void KeyAdmissionQueue::RecordRawFailed(std::size_t keys, std::size_t units) {
  counters_.raw_failed_keys += keys;
  counters_.raw_failed_units += units;
  for (std::size_t index = 0; index < keys; ++index) {
    CountFailure(KeyFailure::kRawCommitFailed);
  }
  if (keys == 0 && units != 0) {
    CountFailure(KeyFailure::kRawCommitFailed);
  }
  counters_.last_terminal_state = KeyState::kFailed;
  counters_.last_failure = KeyFailure::kRawCommitFailed;
}

void KeyAdmissionQueue::RecordRawDropped(std::size_t keys, std::size_t units) {
  counters_.raw_dropped_keys += keys;
  counters_.raw_failed_units += units;
  for (std::size_t index = 0; index < keys; ++index) {
    CountFailure(KeyFailure::kRawCapacity);
  }
  counters_.last_terminal_state = KeyState::kFailed;
  counters_.last_failure = KeyFailure::kRawCapacity;
}

void KeyAdmissionQueue::RecordOverflowEaten() {
  ++counters_.accepted;
  ++counters_.overflow_eaten;
  CountFailure(KeyFailure::kQueueOverflow);
  counters_.last_terminal_state = KeyState::kFailed;
  counters_.last_failure = KeyFailure::kQueueOverflow;
}

void KeyAdmissionQueue::RecordNotEatenAfterPrediction() {
  ++counters_.not_eaten_after_prediction;
}

AdmissionKey* KeyAdmissionQueue::head() {
  return queue_.empty() ? nullptr : &queue_.front();
}

AdmissionKey* KeyAdmissionQueue::in_flight() {
  return in_flight_.has_value() ? &*in_flight_ : nullptr;
}

KeyAdmissionSnapshot KeyAdmissionQueue::Snapshot() const {
  KeyAdmissionSnapshot snapshot = counters_;
  snapshot.queued = queue_.size();
  snapshot.capacity = kCapacity;
  snapshot.has_in_flight = in_flight_.has_value();
  snapshot.in_flight_failed = in_flight_failed_;
  if (in_flight_.has_value()) {
    snapshot.in_flight_id = in_flight_->id;
    snapshot.in_flight_state = in_flight_->state;
    snapshot.in_flight_failure = in_flight_->failure;
  }
  return snapshot;
}

}  // namespace product
}  // namespace tsf
}  // namespace epocanva
