// EpoCanva W2-B -- asynchronous engine start for the product TSF DLL.
//
// WHY THIS EXISTS. The synchronous activation path ran EngineStartTransaction::Prepare and
// EngineHostBroker::Acquire on the activating STA. Both can wait on a cross-process named
// mutex and on the shared engine lock for up to 60 seconds, and Acquire itself waits for a
// schema deployment. A TSF host calls Activate and OnKeyDown on its UI thread; that thread
// must never absorb such a wait.
//
// WHAT IT DOES. One process-wide start thread runs the whole transaction, broker acquisition
// and lease-lock handoff. The activating STA registers a wake callback and continues in a
// literal (pass-through) mode: every key keeps the host's own default effect, so text is
// never reordered or duplicated. When the start finishes, the thread notifies every waiter;
// the STA then adopts the lease and builds the engine pipeline on its own thread. A failed
// start is a reported state, not a silent retry: the next activation may request a new
// attempt.
//
// LIFETIME. The start thread holds a module reference, so DllCanUnloadNow stays S_FALSE
// until it really exits. Deactivation never joins it: RemoveWaiter is non-blocking, and a
// result nobody wants is released on a detached thread. Releasing a lease can stop and join
// the engine, so it never runs on the STA either.
#ifndef EPOCANVA_PRODUCT_ENGINE_BOOTSTRAP_H_
#define EPOCANVA_PRODUCT_ENGINE_BOOTSTRAP_H_

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "engine_host.h"

namespace epocanva {
namespace product {

enum class EngineBootstrapState : std::uint8_t {
  kIdle = 0,
  kStarting,
  kReady,
  kFailed,
};

const char* DescribeEngineBootstrapState(EngineBootstrapState state);

// One real engine lease plus the shared user lock that keeps its compiled output alive.
// Release may block on the engine's stop/join, so callers never do it on the STA; the
// bootstrap's ReleaseAttachmentAsync helper detaches that work.
class ProductEngineAttachment {
 public:
  ProductEngineAttachment();
  ~ProductEngineAttachment();

  ProductEngineAttachment(ProductEngineAttachment&& other) noexcept;
  ProductEngineAttachment& operator=(ProductEngineAttachment&& other) noexcept;
  ProductEngineAttachment(const ProductEngineAttachment&) = delete;
  ProductEngineAttachment& operator=(const ProductEngineAttachment&) = delete;

  ::epocanva::windows::engine::EngineExecutor* executor() const;
  bool valid() const;
  // Synchronous release. Never call this from the STA; use ReleaseAttachmentAsync.
  void Release();

 private:
  friend class ProductEngineBootstrap;
  tsf::EngineLease lease_;
  void* lease_lock_ = nullptr;
};

// Detaches a lease release onto its own thread. The thread holds a module reference until
// the release (which may stop and join the engine) returns.
void ReleaseAttachmentAsync(std::shared_ptr<ProductEngineAttachment> attachment);

class ProductEngineBootstrap {
 public:
  static ProductEngineBootstrap& Instance();

  ProductEngineBootstrap(const ProductEngineBootstrap&) = delete;
  ProductEngineBootstrap& operator=(const ProductEngineBootstrap&) = delete;

  // Called on a TSF STA. Never blocks: it either accepts interest in an attempt already in
  // progress or starts one background attempt. A failed attempt is retried by the next
  // activation.
  void RequestStart();

  // A registered STA wake. `callback` runs on the finishing thread and must only post a
  // message; it must not touch TSF.
  std::uint64_t AddWaiter(std::function<void()> callback);
  void RemoveWaiter(std::uint64_t waiter);

  // Called on the STA after a wake. On success `attachment` owns the pending lease. When
  // the engine is already running and another waiter took that lease, this returns false
  // with `engine_running` true; the caller may acquire a fast path lease itself through
  // AcquireRunning().
  bool TakeAttachment(std::shared_ptr<ProductEngineAttachment>* attachment,
                      bool* engine_running);
  // Fast path for a running engine: EngineHostBroker::Acquire. Still on the STA, so it is
  // only called when RequestStart reported kReady. Never starts an engine (returns false
  // when the broker no longer runs this configuration).
  bool AcquireRunning(std::shared_ptr<ProductEngineAttachment>* attachment,
                      std::wstring* why);

  EngineBootstrapState state() const;
  std::wstring failure() const;
  // Diagnostics only: counts, never text.
  std::uint64_t start_attempts() const;
  std::uint64_t start_failures() const;
  std::uint64_t waiters() const;

  // Test seam. The replacement runs on the bootstrap thread and returns either a real
  // attachment (ok), a failure, or an empty ok result meaning "engine already available".
  struct StartOutcome {
    bool ok = false;
    bool engine_running = false;
    std::wstring why;
    std::shared_ptr<ProductEngineAttachment> attachment;
  };
  using StartFunction = std::function<StartOutcome()>;
  void SetStartFunctionForTest(StartFunction function);
  // Test-only bounded wait. Never called by the product.
  bool WaitForIdleForTest(DWORD timeout_ms);
  // Test-only state reset for a fresh controlled attempt. Refused while a start is running.
  void ResetForTest();

 private:
  ProductEngineBootstrap();
  ~ProductEngineBootstrap();

  static StartOutcome DefaultStartImpl();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace product
}  // namespace epocanva

#endif  // EPOCANVA_PRODUCT_ENGINE_BOOTSTRAP_H_
