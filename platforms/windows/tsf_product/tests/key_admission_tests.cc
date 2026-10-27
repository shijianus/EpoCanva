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

void TestHostTokenOwnership() {
  using epocanva::tsf::product::HostTokenOwnership;
  input::KeyText letter = Text();
  input::KeyText mark = Text();
  mark.utf16_code_units = u"@";
  input::KeyText path = Text();
  path.utf16_code_units = u"\\";
  input::KeyText space = Text();
  space.utf16_code_units = u" ";
  input::KeyText comma = Text();
  comma.utf16_code_units = u",";

  HostTokenOwnership token;
  Check(!token.owned(), "a fresh token is host-neutral");
  token.HostKept(letter, false);
  Check(token.owned(), "a host-kept letter starts a host-owned token");
  Check(HostTokenOwnership::IsTokenText(mark) &&
            HostTokenOwnership::IsTokenText(path),
        "protected marks and path separators are token characters");
  token.HostKept(space, false);
  Check(!token.owned(), "a space ends the host-owned token");
  token.HostKept(letter, false);
  token.HostKept(mark, false);
  Check(token.owned(), "a mark keeps the token host-owned");
  token.HostKept(comma, false);
  Check(!token.owned(), "a non-token punctuation ends the token");
  token.HostKept(letter, false);
  token.HostKept(letter, /*shortcut=*/true);
  Check(token.owned(), "a shortcut does not end the token");
  token.EngineKept();
  Check(!token.owned(), "an engine-consumed key ends host ownership");
  token.HostKept(letter, false);
  token.Reset();
  Check(!token.owned(), "an explicit reset clears ownership");
}

void TestHighWaterAndWithdraw() {
  Check(KeyAdmissionQueue::kHighWater < KeyAdmissionQueue::kNonComposingLimit &&
            KeyAdmissionQueue::kNonComposingLimit < KeyAdmissionQueue::kCapacity,
        "high water is below both hard limits");
  KeyAdmissionQueue queue;
  for (int index = 0; index < 3; ++index) {
    input::KeyText text = Text();
    text.utf16_code_units = std::u16string(1, static_cast<char16_t>(u'a' + index));
    queue.Push(Key(), text, Decision(), 9, 2);
  }
  queue.ConfirmEaten(1);
  queue.ConfirmEaten(2);
  queue.ConfirmEaten(3);
  const std::deque<AdmissionKey> withdrawn = queue.WithdrawQueued();
  Check(withdrawn.size() == 3, "withdraw removes the whole queued tail");
  Check(queue.empty(), "the engine queue is empty after a withdraw");
  Check(withdrawn[0].text.utf16_code_units == u"a" &&
            withdrawn[1].text.utf16_code_units == u"b" &&
            withdrawn[2].text.utf16_code_units == u"c",
        "withdraw preserves FIFO order of the captured text");
  Check(withdrawn[0].id < withdrawn[1].id && withdrawn[1].id < withdrawn[2].id,
        "withdrawn ids keep their admission order");
  queue.RecordDeferredRaw(withdrawn.size(), 3);
  KeyAdmissionSnapshot snapshot = queue.Snapshot();
  Check(snapshot.deferred_raw_keys == 3 && snapshot.deferred_raw_units == 3,
        "withdrawn keys are staged as deferred raw, not failed");
  Check(snapshot.failed == 0, "a withdraw counts no failure");
}

void TestRawAccounting() {
  KeyAdmissionQueue queue;
  queue.RecordRawApplied(4, 9);
  KeyAdmissionSnapshot snapshot = queue.Snapshot();
  Check(snapshot.raw_applied_keys == 4 && snapshot.raw_applied_units == 9,
        "an applied raw run is counted with its units");
  Check(snapshot.failed == 0, "an applied raw run is not a failure");

  queue.RecordRawFailed(2, 5);
  snapshot = queue.Snapshot();
  Check(snapshot.raw_failed_keys == 2 && snapshot.raw_failed_units == 5 &&
            snapshot.failed == 2 && snapshot.failed_raw_commit == 2,
        "a refused raw run is an explicit named failure");
  Check(snapshot.last_failure == KeyFailure::kRawCommitFailed,
        "the last failure names the raw commit refusal");

  queue.RecordRawDropped(1, 1);
  snapshot = queue.Snapshot();
  Check(snapshot.raw_dropped_keys == 1 && snapshot.failed_raw_capacity == 1,
        "a run that exceeds its bound is reported as raw-capacity, not hidden");
  Check(std::string(epocanva::tsf::product::DescribeKeyFailure(
                KeyFailure::kRawCapacity)) == "raw-capacity",
        "the raw-capacity failure has a stable name");
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
  TestHostTokenOwnership();
  TestHighWaterAndWithdraw();
  TestRawAccounting();
  TestFailures();
  if (g_failures != 0) {
    std::cout << "FAILED: " << g_failures << " check(s)\n";
    return 14;
  }
  std::cout << "RESULT PASS\n";
  return 0;
}
