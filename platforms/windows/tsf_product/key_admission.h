// EpoCanva W2 -- bounded keyboard admission in front of the single-flight coordinator.
//
// WHY A QUEUE. The coordinator deliberately allows one unresolved work item. A natural
// keyboard produces key events faster than that, so a refusal of the second key must not
// turn into "the character leaks into the document while a composition is active". This
// queue accepts a bounded number of value copies, keeps their order, and lets the STA
// dispatch exactly one at a time.
//
// FIVE PER-KEY STATES, as required by the W2 brief:
//   kNotAccepted     the key was refused before it was eaten (the host keeps it)
//   kAccepted        the key was eaten and is queued or in flight
//   kEngineCompleted the engine consumed it; an edit may still be pending
//   kEditCompleted   the document edit (or the already-current document) is confirmed
//   kFailed          a named failure; never a silent discard
//
// The queue is two regions:
//   * accepted-but-not-dispatched (FIFO, bounded);
//   * one in-flight entry (submitted to the coordinator) until its terminal state.
// The in-flight entry is kept separate so a focus change can fail the queued tail without
// disturbing the coordinator's ownership of the submitted work.
//
// CAPACITY POLICY. The limit is 64 entries. While no composition is active only 48 may be
// used; the last 16 are reserved for input that is already inside a composition. If the
// queue is completely exhausted while a composition is active there is no safe pass-through
// (the host would insert a character inside our preedit). The controller eats the key and
// records an explicit kQueueOverflow failure; this is reported as an unresolved blocker for
// "natural input complete under a stalled engine", never hidden. An unpaced engine event
// cannot make this unreachable by design, only unlikely.
#ifndef EPOCANVA_WINDOWS_TSF_PRODUCT_KEY_ADMISSION_H_
#define EPOCANVA_WINDOWS_TSF_PRODUCT_KEY_ADMISSION_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

#include "coordinator.h"
#include "key_decision.h"
#include "key_event.h"

namespace epocanva {
namespace tsf {
namespace product {

namespace input = ::epocanva::windows::input;
namespace coordinator = ::epocanva::tsf::coordinator;

enum class KeyState : std::uint8_t {
  kNotAccepted = 0,
  kAccepted,
  kEngineCompleted,
  kEditCompleted,
  kFailed,
};

enum class KeyFailure : std::uint8_t {
  kNone = 0,
  kFocusChanged,
  kBindingRetired,
  kPermissionRefused,
  kTargetChanged,
  kEngineRefused,
  kQueueOverflow,
  kPredictionMismatch,
  kEngineUnavailable,
  kBlockedGate,
  kShutdown,
  kInternal,
};

const char* DescribeKeyState(KeyState state);
const char* DescribeKeyFailure(KeyFailure failure);

struct AdmissionKey {
  std::uint64_t id = 0;
  std::uint64_t context_id = 0;
  std::uint64_t generation = 0;
  input::KeyEventSnapshot event{};
  input::KeyText text{};
  input::KeyDecision decision{};
  KeyState state = KeyState::kAccepted;
  KeyFailure failure = KeyFailure::kNone;
  bool eaten = false;
  bool submitted = false;
  coordinator::WorkId work{};
};

struct KeyAdmissionSnapshot {
  std::size_t queued = 0;
  std::size_t capacity = 0;
  bool has_in_flight = false;
  bool in_flight_failed = false;
  std::uint64_t in_flight_id = 0;
  KeyState in_flight_state = KeyState::kNotAccepted;
  KeyFailure in_flight_failure = KeyFailure::kNone;
  std::uint64_t accepted = 0;
  std::uint64_t dispatched = 0;
  std::uint64_t engine_completed = 0;
  std::uint64_t edit_completed = 0;
  std::uint64_t failed = 0;
  std::uint64_t not_eaten_after_prediction = 0;
  std::uint64_t overflow_eaten = 0;
  std::uint64_t failed_focus = 0;
  std::uint64_t failed_binding_retired = 0;
  std::uint64_t failed_permission = 0;
  std::uint64_t failed_target = 0;
  std::uint64_t failed_engine = 0;
  std::uint64_t failed_queue_overflow = 0;
  std::uint64_t failed_prediction = 0;
  std::uint64_t failed_gate = 0;
  std::uint64_t failed_shutdown = 0;
  std::uint64_t failed_internal = 0;
  std::uint64_t last_terminal_id = 0;
  KeyState last_terminal_state = KeyState::kNotAccepted;
  KeyFailure last_failure = KeyFailure::kNone;
};

class KeyAdmissionQueue {
 public:
  static constexpr std::size_t kCapacity = 64;
  static constexpr std::size_t kNonComposingLimit = 48;

  bool CanAccept(bool composition_active) const;
  // Appends one accepted key. The caller must have checked CanAccept first.
  std::uint64_t Push(const input::KeyEventSnapshot& event, const input::KeyText& text,
                     const input::KeyDecision& decision, std::uint64_t context_id,
                     std::uint64_t generation);
  // Marks the key as really eaten (OnKeyDown returned TRUE).
  void ConfirmEaten(std::uint64_t id);
  // Removes the head without submitting it because the host kept the key.
  void DropHeadNotEaten();
  // Fails and removes the head. `mark_eaten` counts it as accepted (the controller is
  // about to report pfEaten TRUE); false is used only when the host keeps the key.
  bool FailHead(KeyFailure failure, bool mark_eaten);
  // Moves the head into the single in-flight slot. Called only when the coordinator
  // accepted its work item.
  bool MoveHeadToInFlight(coordinator::WorkId work);
  // Marks the in-flight key as engine-completed (no-op when already advanced).
  void MarkInFlightEngineCompleted();
  // Terminal for the in-flight key: kEditCompleted or kFailed(reason).
  void CompleteInFlight(KeyState state, KeyFailure failure);
  // Terminal failure recorded while the coordinator still owes a result. The entry stays
  // in flight until ReleaseFailedInFlight so ordering cannot be violated.
  void MarkInFlightFailed(KeyFailure failure);
  bool in_flight_failed() const;
  void ReleaseFailedInFlight();
  // Fails and removes every queued (not in-flight) key. Returns the count.
  std::size_t FailQueued(KeyFailure failure);
  void RecordOverflowEaten();
  void RecordNotEatenAfterPrediction();

  bool empty() const { return queue_.empty(); }
  bool HasInFlight() const { return in_flight_.has_value(); }
  AdmissionKey* head();
  AdmissionKey* in_flight();
  const std::deque<AdmissionKey>& pending() const { return queue_; }
  const std::optional<AdmissionKey>& in_flight_entry() const { return in_flight_; }
  KeyAdmissionSnapshot Snapshot() const;

 private:
  void CountFailure(KeyFailure failure);
  void MarkTerminal(KeyState state, KeyFailure failure);

  std::deque<AdmissionKey> queue_;
  std::optional<AdmissionKey> in_flight_;
  bool in_flight_failed_ = false;
  std::uint64_t next_id_ = 1;
  KeyAdmissionSnapshot counters_;
};

}  // namespace product
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_PRODUCT_KEY_ADMISSION_H_
