// EpoCanva W2 -- fixed STA wake channel for engine/TSF completion.
//
// WHAT THIS REPLACES. The P3.x tests drive the coordinator with an explicit Pump() loop.
// The product text service must be told when an engine result or an asynchronous TSF
// completion exists without polling from a key callback. This class owns that wake:
//
//   engine worker -> SetEvent(executor result event)      (Win32, no COM)
//   watcher thread -> PostMessageW(message-only HWND)     (Win32, no COM)
//   STA window proc -> OnStaWake(generation) -> Pump()    (COM only on the STA)
//
// The watcher thread never touches an ITf* object, a controller or user text. The message
// carries only the registration generation in wParam; lParam is zero. WAKE IS A HINT, NOT
// A RESULT: the coordinator still takes each engine result once, and repeated wakes are
// idempotent because Pump() only consumes what is actually ready.
//
// LIFETIME. Start() creates a message-only window owned by the STA. Stop() sets the stop
// event, joins the watcher and then removes the HWND from the registry before destroying
// it; a queued or late wake therefore reaches no object, and a recycled HWND value is a
// different registration. A failed PostMessage is retried with a short bounded wait and is
// counted; the result stays readable in the executor, so a lost wake cannot be mistaken
// for an applied edit.
#ifndef EPOCANVA_WINDOWS_TSF_PRODUCT_STA_RESULT_WAKER_H_
#define EPOCANVA_WINDOWS_TSF_PRODUCT_STA_RESULT_WAKER_H_

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace epocanva {
namespace tsf {
namespace product {

// Implemented by the STA controller. Called on the STA only.
class StaWakeTarget {
 public:
  virtual ~StaWakeTarget() {}
  virtual void OnStaWake(std::uint64_t generation) = 0;
};

struct StaWakerDiagnostics {
  bool started = false;
  bool paced = false;
  std::uint64_t generation = 0;
  std::uint64_t wakes_posted = 0;
  std::uint64_t wake_post_failures = 0;
  std::uint64_t wakes_received = 0;
  std::uint64_t stale_wakes_ignored = 0;
  std::uint64_t wakes_without_registration = 0;
  std::uint64_t stops = 0;
  std::uint64_t loop_ticks = 0;
  unsigned long window_thread_id = 0;
  std::uint64_t window_messages = 0;
};

class StaResultWaker {
 public:
  StaResultWaker();
  ~StaResultWaker();

  StaResultWaker(const StaResultWaker&) = delete;
  StaResultWaker& operator=(const StaResultWaker&) = delete;

  // `target` must outlive Stop(). `executor_event` may be null; pacing then relies on the
  // short interval alone. `generation` is copied into every wake and checked on receipt.
  // `module` owns the window class and its WndProc: the text-service DLL must pass its own
  // HINSTANCE, never the host EXE's, or unloading the DLL would leave a dangling class.
  bool Start(StaWakeTarget* target, HANDLE executor_event, HMODULE module,
             std::uint64_t generation, std::wstring* why);
  void Stop();
  bool started() const { return started_; }

  // Tell the watcher that a TSF async completion is pending without a Win32 event this
  // class can wait on. Paced mode wakes every few milliseconds; unpaced mode sleeps until
  // the executor event, a reconfiguration or Stop.
  void SetPacing(bool paced);
  // The engine result event is not known when a literal-mode controller starts. Setting it
  // later re-arms the existing watcher thread; the STA window is not recreated.
  void SetExecutorEvent(HANDLE executor_event);

  // Cross-thread wake: posts the same generation-only message as the watcher. Used by the
  // asynchronous engine bootstrap. Safe from any thread; a call racing Stop() either posts
  // before the window is destroyed or observes stopped and does nothing.
  void Notify();

  StaWakerDiagnostics Snapshot() const;
  HWND window_for_test() const { return window_; }

  // Window-proc entry. Public only because the process-wide window class dispatches here;
  // it is not part of the controller-facing API.
  void HandleWake(std::uint64_t generation);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  void RequestWatcherReconfigure();

  StaWakeTarget* target_ = nullptr;
  HMODULE module_ = nullptr;
  HWND window_ = nullptr;
  HANDLE stop_event_ = nullptr;
  HANDLE reconfigure_event_ = nullptr;
  std::atomic<HANDLE> executor_event_{nullptr};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> paced_{false};
  std::atomic<std::uint64_t> wakes_posted_{0};
  std::atomic<std::uint64_t> wake_post_failures_{0};
  std::atomic<std::uint64_t> wakes_received_{0};
  std::atomic<std::uint64_t> stops_{0};
  std::atomic<std::uint64_t> loop_ticks_{0};
  std::atomic<unsigned long> window_thread_id_{0};
  std::uint64_t generation_ = 0;
  bool started_ = false;
  // Protects window_/generation_ against a cross-thread Notify racing Stop(). The window
  // itself is created and destroyed only on the STA.
  std::mutex window_mutex_;
};

}  // namespace product
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_PRODUCT_STA_RESULT_WAKER_H_
