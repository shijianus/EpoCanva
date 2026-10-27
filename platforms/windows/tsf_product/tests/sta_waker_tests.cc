// EpoCanva W2 -- STA wake-channel tests. No engine, no TSF: a fake target counts calls and
// the test drives a real message queue, so the window/proc/thread path is the product one.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>

#include "sta_result_waker.h"

namespace {

using epocanva::tsf::product::StaResultWaker;
using epocanva::tsf::product::StaWakerDiagnostics;
using epocanva::tsf::product::StaWakeTarget;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
  std::cout << (condition ? "  ok   " : "  FAIL ") << what << "\n";
  if (!condition) {
    ++g_failures;
  }
}

class CountingTarget final : public StaWakeTarget {
 public:
  explicit CountingTarget(HANDLE seen) : seen_(seen) {}

  void OnStaWake(std::uint64_t generation) override {
    last_generation = generation;
    ++calls;
    if (seen_ != nullptr) {
      ::SetEvent(seen_);
    }
  }

  std::atomic<std::uint64_t> calls{0};
  std::atomic<std::uint64_t> last_generation{0};

 private:
  HANDLE seen_ = nullptr;
};

bool PumpUntil(HANDLE event, DWORD timeout_ms) {
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    const DWORD remaining =
        static_cast<DWORD>(deadline > ::GetTickCount64() ? deadline - ::GetTickCount64()
                                                         : 0);
    const DWORD waited = ::MsgWaitForMultipleObjectsEx(
        1, &event, remaining, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (waited == WAIT_OBJECT_0) {
      return true;
    }
    if (waited == WAIT_TIMEOUT) {
      return false;
    }
    MSG message;
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      ::TranslateMessage(&message);
      ::DispatchMessageW(&message);
    }
  }
}

void PumpFor(DWORD duration_ms) {
  const ULONGLONG deadline = ::GetTickCount64() + duration_ms;
  while (::GetTickCount64() < deadline) {
    MSG message;
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      ::TranslateMessage(&message);
      ::DispatchMessageW(&message);
    }
    ::Sleep(1);
  }
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  std::cout << "sta waker\n";
  HANDLE executor_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HANDLE wake_seen = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Check(executor_event != nullptr && wake_seen != nullptr, "test events created");
  CountingTarget target(wake_seen);

  StaResultWaker waker;
  std::wstring why;
  Check(waker.Start(&target, executor_event, 77, &why), "waker starts on this STA");
  Check(waker.window_for_test() != nullptr, "a message-only window exists");

  ::SetEvent(executor_event);
  Check(PumpUntil(wake_seen, 2000), "an executor event produces a wake");
  ::ResetEvent(wake_seen);
  PumpFor(20);
  Check(target.calls.load() == 1, "one event produces one delivered wake");
  Check(target.last_generation.load() == 77, "the generation is carried");

  // A stale-generation message must reach no target.
  const ULONGLONG stale_before = waker.Snapshot().stale_wakes_ignored;
  ::PostMessageW(waker.window_for_test(), WM_APP + 0x51, 76, 0);
  PumpFor(20);
  Check(target.calls.load() == 1, "a stale generation is ignored");
  Check(waker.Snapshot().stale_wakes_ignored > stale_before,
        "the stale wake is counted");

  // Pacing: no executor event, but the watcher wakes on the short interval.
  const std::uint64_t paced_before = target.calls.load();
  waker.SetPacing(true);
  PumpFor(120);
  Check(target.calls.load() > paced_before, "paced mode wakes without an engine event");
  waker.SetPacing(false);
  PumpFor(30);
  const std::uint64_t idle_before = target.calls.load();
  PumpFor(120);
  Check(target.calls.load() == idle_before, "unpaced mode stops the periodic wake");

  // A destroyed target window makes PostMessage fail; the watcher must count the failure
  // and keep trying until Stop, not crash or consume anything.
  ::DestroyWindow(waker.window_for_test());
  waker.SetPacing(true);
  PumpFor(80);
  Check(waker.Snapshot().wake_post_failures > 0,
        "a failed notification is counted");
  waker.Stop();
  Check(!waker.started(), "Stop reports stopped");
  const std::uint64_t calls_after_stop = target.calls.load();
  PumpFor(50);
  Check(target.calls.load() == calls_after_stop, "no wake reaches the target after Stop");

  ::CloseHandle(executor_event);
  ::CloseHandle(wake_seen);
  if (g_failures != 0) {
    std::cout << "FAILED: " << g_failures << " check(s)\n";
    return 14;
  }
  std::cout << "RESULT PASS\n";
  return 0;
}
