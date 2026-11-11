// EpoCanva W2-B -- asynchronous engine start. See product_engine_bootstrap.h.
#include "product_engine_bootstrap.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "module_state.h"
#include "product_engine_config.h"

namespace epocanva {
namespace product {
namespace {

using ::epocanva::tsf::EngineHostBroker;
using ::epocanva::tsf::EngineHostConfig;
using ::epocanva::tsf::EngineLease;
using ::epocanva::tsf::EngineLeaseStatus;

}  // namespace

struct ProductEngineLeaseLock {
  ~ProductEngineLeaseLock() { ReleaseEngineLeaseLock(handle); }
  void* handle = nullptr;
};

ProductEngineBootstrap::StartOutcome ProductEngineBootstrap::DefaultStartImpl() {
  StartOutcome outcome;
  std::wstring why;
  EngineStartTransaction transaction;
  if (!transaction.Prepare(&why)) {
    outcome.why = L"engine configuration preparation failed: " + why;
    return outcome;
  }
  EngineHostConfig config;
  if (!transaction.config(&config)) {
    outcome.why = L"the engine transaction has no configuration";
    transaction.Abort();
    return outcome;
  }
  EngineLease lease;
  const EngineLeaseStatus status = EngineHostBroker::Instance().Acquire(config, &lease);
  if (status != EngineLeaseStatus::kAcquired) {
    transaction.Abort();
    outcome.why = L"the engine did not start (status " +
                  std::to_wstring(static_cast<int>(status)) + L")";
    return outcome;
  }
  if (!transaction.Complete(&why)) {
    (void)lease.Release();
    outcome.why = L"the engine started but the deployment did not complete: " + why;
    return outcome;
  }
  // Allocate ownership before transferring the handle. If allocation throws,
  // lease unwinding stops the engine before the transaction releases its lock.
  auto lock_owner = std::make_shared<ProductEngineLeaseLock>();
  auto attachment = std::make_shared<ProductEngineAttachment>();
  lock_owner->handle = transaction.TakeLeaseLock();
  if (lock_owner->handle == nullptr) {
    (void)lease.Release();
    outcome.why = L"the engine started without a user lease lock";
    return outcome;
  }
  attachment->lease_ = std::move(lease);
  attachment->lease_lock_ = std::move(lock_owner);
  outcome.ok = true;
  outcome.attachment = std::move(attachment);
  return outcome;
}

const char* DescribeEngineBootstrapState(EngineBootstrapState state) {
  switch (state) {
    case EngineBootstrapState::kIdle:
      return "idle";
    case EngineBootstrapState::kStarting:
      return "starting";
    case EngineBootstrapState::kReady:
      return "ready";
    case EngineBootstrapState::kFailed:
      return "failed";
  }
  return "unknown";
}

ProductEngineAttachment::ProductEngineAttachment() = default;
ProductEngineAttachment::~ProductEngineAttachment() { Release(); }

ProductEngineAttachment::ProductEngineAttachment(ProductEngineAttachment&& other) noexcept
    : lease_(std::move(other.lease_)), lease_lock_(std::move(other.lease_lock_)) {}

ProductEngineAttachment& ProductEngineAttachment::operator=(
    ProductEngineAttachment&& other) noexcept {
  if (this != &other) {
    Release();
    lease_ = std::move(other.lease_);
    lease_lock_ = std::move(other.lease_lock_);
  }
  return *this;
}

::epocanva::windows::engine::EngineExecutor* ProductEngineAttachment::executor() const {
  return lease_.executor();
}

bool ProductEngineAttachment::valid() const { return lease_.valid(); }

void ProductEngineAttachment::Release() {
  if (lease_.valid()) {
    (void)lease_.Release();
  }
  // The last owner runs only after the last lease's stop/join has returned.
  // Earlier attachments cannot release the remaining attachments' file lock.
  lease_lock_.reset();
}

void ReleaseAttachmentAsync(std::shared_ptr<ProductEngineAttachment> attachment) {
  if (!attachment) {
    return;
  }
  ::epocanva::tsf::ModuleAddObject();
  try {
    std::thread([attachment]() {
      attachment->Release();
      ::epocanva::tsf::ModuleReleaseObject();
    }).detach();
  } catch (...) {
    // Thread creation failed (process-wide resource exhaustion). Releasing here may block,
    // but leaving the lease alive would keep the engine's compiled output locked forever.
    attachment->Release();
    ::epocanva::tsf::ModuleReleaseObject();
  }
}

struct ProductEngineBootstrap::Impl {
  mutable std::mutex mutex;
  std::condition_variable idle;
  EngineBootstrapState state = EngineBootstrapState::kIdle;
  std::wstring failure;
  bool starting = false;
  bool engine_running = false;
  std::shared_ptr<ProductEngineAttachment> pending;
  std::weak_ptr<ProductEngineLeaseLock> runtime_lock;
  std::unordered_map<std::uint64_t, std::function<void()>> waiters;
  std::uint64_t next_waiter = 1;
  std::uint64_t attempts = 0;
  std::uint64_t failures = 0;
  StartFunction start_function;

  void RunStart() {
    StartFunction function;
    {
      std::lock_guard<std::mutex> guard(mutex);
      function = start_function;
    }
    StartOutcome outcome;
    try {
      outcome = function ? function() : DefaultStartImpl();
    } catch (...) {
      outcome = StartOutcome{};
      outcome.why = L"the engine start raised an exception";
    }
    std::vector<std::function<void()>> callbacks;
    {
      std::lock_guard<std::mutex> guard(mutex);
      starting = false;
      if (outcome.ok) {
        state = EngineBootstrapState::kReady;
        failure.clear();
        pending = std::move(outcome.attachment);
        runtime_lock = pending != nullptr ? pending->lease_lock_
                                          : std::weak_ptr<ProductEngineLeaseLock>{};
        engine_running = outcome.engine_running || pending != nullptr;
      } else {
        state = EngineBootstrapState::kFailed;
        failure = outcome.why.empty() ? L"the engine start failed" : outcome.why;
        pending.reset();
        engine_running = false;
        ++failures;
      }
      for (const auto& waiter : waiters) {
        callbacks.push_back(waiter.second);
      }
      idle.notify_all();
    }
    for (const auto& callback : callbacks) {
      if (callback) {
        try {
          callback();
        } catch (...) {
          // A waiter callback only posts a message; a throwing one must not kill the
          // start thread or skip the remaining waiters.
        }
      }
    }
    ::epocanva::tsf::ModuleReleaseObject();
  }
};

ProductEngineBootstrap::ProductEngineBootstrap() : impl_(new Impl) {}

ProductEngineBootstrap::~ProductEngineBootstrap() = default;

ProductEngineBootstrap& ProductEngineBootstrap::Instance() {
  static ProductEngineBootstrap instance;
  return instance;
}

void ProductEngineBootstrap::RequestStart() {
  bool spawn = false;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->state == EngineBootstrapState::kStarting) {
      return;
    }
    if (impl_->state == EngineBootstrapState::kReady) {
      // A pending lease keeps the engine alive by itself. Without one, ask the broker
      // whether the engine really is running; a stopped engine must not be Acquired on the
      // STA because Acquire would start it synchronously.
      if (impl_->pending != nullptr) {
        return;
      }
      EngineHostConfig config;
      if (impl_->engine_running && ::epocanva::tsf::GetEngineHostConfig(&config) &&
          EngineHostBroker::Instance().IsRunningFor(config)) {
        return;
      }
    }
    impl_->state = EngineBootstrapState::kStarting;
    impl_->failure.clear();
    impl_->starting = true;
    ++impl_->attempts;
    spawn = true;
  }
  if (!spawn) {
    return;
  }
  ::epocanva::tsf::ModuleAddObject();
  try {
    std::thread([this]() { impl_->RunStart(); }).detach();
  } catch (...) {
    {
      std::lock_guard<std::mutex> guard(impl_->mutex);
      impl_->starting = false;
      impl_->state = EngineBootstrapState::kFailed;
      impl_->failure = L"the engine start thread could not be created";
      ++impl_->failures;
    }
    ::epocanva::tsf::ModuleReleaseObject();
  }
}

std::uint64_t ProductEngineBootstrap::AddWaiter(std::function<void()> callback) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  const std::uint64_t id = impl_->next_waiter++;
  impl_->waiters[id] = std::move(callback);
  return id;
}

void ProductEngineBootstrap::RemoveWaiter(std::uint64_t waiter) {
  std::shared_ptr<ProductEngineAttachment> abandoned;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->waiters.erase(waiter);
    if (impl_->waiters.empty()) {
      abandoned = std::move(impl_->pending);
      impl_->engine_running = false;
      if (impl_->state == EngineBootstrapState::kReady) {
        impl_->state = EngineBootstrapState::kIdle;
      }
    }
  }
  ReleaseAttachmentAsync(std::move(abandoned));
}

bool ProductEngineBootstrap::TakeAttachment(
    std::shared_ptr<ProductEngineAttachment>* attachment, bool* engine_running) {
  if (attachment == nullptr || engine_running == nullptr) {
    return false;
  }
  attachment->reset();
  *engine_running = false;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (impl_->state != EngineBootstrapState::kReady) {
    return false;
  }
  if (impl_->pending != nullptr) {
    *attachment = std::move(impl_->pending);
    *engine_running = true;
    return true;
  }
  *engine_running = impl_->engine_running;
  return false;
}

bool ProductEngineBootstrap::AcquireRunning(
    std::shared_ptr<ProductEngineAttachment>* attachment, std::wstring* why) {
  if (attachment == nullptr) {
    return false;
  }
  attachment->reset();
  // Take a strong reference before consulting/acquiring the engine lease. A first
  // attachment can leave concurrently; this reference keeps its shared lock alive.
  std::shared_ptr<ProductEngineLeaseLock> lock_owner;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    lock_owner = impl_->runtime_lock.lock();
  }
  if (!lock_owner) {
    if (why != nullptr) {
      *why = L"the running engine has no shared deployment lock owner";
    }
    return false;
  }
  EngineHostConfig config;
  if (!::epocanva::tsf::GetEngineHostConfig(&config) ||
      !EngineHostBroker::Instance().IsRunningFor(config)) {
    if (why != nullptr) {
      *why = L"the engine is not running for this configuration";
    }
    return false;
  }
  EngineLease lease;
  const EngineLeaseStatus status = EngineHostBroker::Instance().Acquire(config, &lease);
  if (status != EngineLeaseStatus::kAcquired) {
    if (why != nullptr) {
      *why = L"the running engine refused a lease (status " +
              std::to_wstring(static_cast<int>(status)) + L")";
    }
    return false;
  }
  auto taken = std::make_shared<ProductEngineAttachment>();
  taken->lease_ = std::move(lease);
  taken->lease_lock_ = std::move(lock_owner);
  *attachment = std::move(taken);
  return true;
}

EngineBootstrapState ProductEngineBootstrap::state() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->state;
}

std::wstring ProductEngineBootstrap::failure() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->failure;
}

std::uint64_t ProductEngineBootstrap::start_attempts() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->attempts;
}

std::uint64_t ProductEngineBootstrap::start_failures() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->failures;
}

std::uint64_t ProductEngineBootstrap::waiters() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return impl_->waiters.size();
}

void ProductEngineBootstrap::SetStartFunctionForTest(StartFunction function) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->start_function = std::move(function);
}

void ProductEngineBootstrap::ResetForTest() {
  std::shared_ptr<ProductEngineAttachment> abandoned;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (impl_->starting) {
      return;
    }
    abandoned = std::move(impl_->pending);
    impl_->state = EngineBootstrapState::kIdle;
    impl_->failure.clear();
    impl_->engine_running = false;
  }
  ReleaseAttachmentAsync(std::move(abandoned));
}

bool ProductEngineBootstrap::WaitForIdleForTest(DWORD timeout_ms) {
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  std::unique_lock<std::mutex> lock(impl_->mutex);
  while (impl_->starting) {
    const ULONGLONG now = ::GetTickCount64();
    if (now >= deadline) {
      return false;
    }
    impl_->idle.wait_for(lock, std::chrono::milliseconds(5));
  }
  return true;
}

}  // namespace product
}  // namespace epocanva
