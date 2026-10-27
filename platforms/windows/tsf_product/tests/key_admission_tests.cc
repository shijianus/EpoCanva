// EpoCanva W2 -- bounded key admission unit cases. No TSF, no engine, no files.
#include <cstdint>
#include <iostream>
#include <string>

#include "key_admission.h"

namespace {

using epocanva::tsf::product::AdmissionKey;
using epocanva::tsf::product::KeyAdmissionQueue;
using epocanva::tsf::product::KeyAdmissionSnapshot;
using epocanva::tsf::product::KeyFailure;
using epocanva::tsf::product::KeyState;
namespace coordinator = epocanva::tsf::product::coordinator;
namespace input = ::epocanva::windows::input;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
  std::cout << (condition ? "  ok   " : "  FAIL ") << what << "\n";
  if (!condition) {
    ++g_failures;
  }
}

input::KeyEventSnapshot Key() {
  input::KeyEventSnapshot event;
  event.virtual_key = 'A';
  event.scan_code = 0x1E;
  event.transition = input::KeyTransition::kKeyDown;
  event.repeat_count = 1;
  return event;
}

input::KeyText Text() {
  input::KeyText text;
  text.status = input::KeyTextStatus::kText;
  text.utf16_code_units = u"a";
  return text;
}

input::KeyDecision Decision() {
  input::KeyDecision decision;
  decision.disposition = input::KeyDisposition::kEngineTextCandidate;
  decision.text_class = input::TextClass::kAsciiLetter;
  decision.ascii_code_point = 'a';
  return decision;
}

void TestCapacity() {
  KeyAdmissionQueue queue;
  Check(queue.CanAccept(false), "an empty queue accepts input");
  for (std::size_t index = 0; index < KeyAdmissionQueue::kNonComposingLimit; ++index) {
    queue.Push(Key(), Text(), Decision(), 1, 1);
  }
  Check(!queue.CanAccept(false),
        "without a composition the reserved slots are not offered");
  Check(queue.CanAccept(true), "a composing session may use the reserved slots");
  while (queue.CanAccept(true)) {
    queue.Push(Key(), Text(), Decision(), 1, 1);
  }
  Check(queue.Snapshot().queued == KeyAdmissionQueue::kCapacity,
        "the queue stops exactly at its capacity");
}

void TestLifecycle() {
  KeyAdmissionQueue queue;
  const std::uint64_t first = queue.Push(Key(), Text(), Decision(), 7, 3);
  const std::uint64_t second = queue.Push(Key(), Text(), Decision(), 7, 3);
  Check(first != 0 && second > first, "key ids are unique and ordered");
  Check(queue.Snapshot().queued == 2, "both keys are queued");
  queue.ConfirmEaten(first);
  queue.ConfirmEaten(first);
  Check(queue.Snapshot().accepted == 1, "confirming twice counts one accepted key");

  Check(queue.MoveHeadToInFlight(coordinator::WorkId{42}),
        "the head moves to the single in-flight slot");
  Check(queue.Snapshot().queued == 1 && queue.HasInFlight(),
        "queued and in-flight are separate");
  queue.MarkInFlightEngineCompleted();
  queue.MarkInFlightEngineCompleted();
  Check(queue.Snapshot().engine_completed == 1,
        "engine completion is counted once");
  queue.CompleteInFlight(KeyState::kEditCompleted, KeyFailure::kNone);
  Check(queue.Snapshot().edit_completed == 1, "the edit completion is counted");
  Check(!queue.HasInFlight(), "the in-flight slot is released after a terminal");

  Check(queue.MoveHeadToInFlight(coordinator::WorkId{43}),
        "the second key becomes in flight");
  queue.MarkInFlightFailed(KeyFailure::kFocusChanged);
  queue.MarkInFlightFailed(KeyFailure::kFocusChanged);
  KeyAdmissionSnapshot snapshot = queue.Snapshot();
  Check(snapshot.failed == 1 && snapshot.failed_focus == 1,
        "a focus failure is counted once");
  Check(snapshot.in_flight_failed, "the failed in-flight key is marked");
  queue.ReleaseFailedInFlight();
  Check(!queue.HasInFlight(), "the failed in-flight key can be released");
  Check(queue.Snapshot().failed == 1, "release does not count a second failure");
}

void TestFailures() {
  KeyAdmissionQueue queue;
  queue.Push(Key(), Text(), Decision(), 1, 1);
  queue.Push(Key(), Text(), Decision(), 1, 1);
  queue.Push(Key(), Text(), Decision(), 1, 1);
  queue.ConfirmEaten(1);
  Check(queue.FailQueued(KeyFailure::kTargetChanged) == 3,
        "all queued keys are failed explicitly");
  KeyAdmissionSnapshot snapshot = queue.Snapshot();
  Check(snapshot.queued == 0 && snapshot.failed == 3 && snapshot.failed_target == 3,
        "the failure reason and count are recorded");
  Check(snapshot.last_terminal_state == KeyState::kFailed,
        "the last terminal is a failure");

  KeyAdmissionQueue dropped;
  dropped.Push(Key(), Text(), Decision(), 1, 1);
  dropped.DropHeadNotEaten();
  Check(dropped.Snapshot().not_eaten_after_prediction == 1,
        "a prediction refusal that the host keeps is recorded");
  Check(dropped.Snapshot().accepted == 0,
        "a key the host keeps is not counted as accepted");

  KeyAdmissionQueue eaten;
  eaten.Push(Key(), Text(), Decision(), 1, 1);
  eaten.FailHead(KeyFailure::kQueueOverflow, true);
  snapshot = eaten.Snapshot();
  Check(snapshot.accepted == 1 && snapshot.failed_queue_overflow == 1,
        "an eaten failure is both accepted and failed");

  KeyAdmissionQueue overflow;
  overflow.RecordOverflowEaten();
  snapshot = overflow.Snapshot();
  Check(snapshot.overflow_eaten == 1 && snapshot.failed_queue_overflow == 1,
        "the composition-full overflow is an explicit accepted failure");
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  TestCapacity();
  TestLifecycle();
  TestFailures();
  if (g_failures != 0) {
    std::cout << "FAILED: " << g_failures << " check(s)\n";
    return 14;
  }
  std::cout << "RESULT PASS\n";
  return 0;
}
