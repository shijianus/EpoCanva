// EpoCanva W2 -- implementation of the fixed STA wake channel. See the header contract.
#include "sta_result_waker.h"

#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace epocanva {
namespace tsf {
namespace product {
namespace {

constexpr UINT kWakeMessage = WM_APP + 0x51;
constexpr wchar_t kWakeWindowClass[] = L"EpoCanva.StaWaker.W2";

std::mutex g_registry_mutex;
struct Registration {
  StaResultWaker* owner = nullptr;
  StaWakeTarget* target = nullptr;
  std::uint64_t generation = 0;
};
std::unordered_map<HWND, Registration> g_registry;

std::atomic<std::uint64_t> g_stale_wakes{0};
std::atomic<std::uint64_t> g_unregistered_wakes{0};

LRESULT CALLBACK WakeWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == kWakeMessage) {
    StaWakeTarget* target = nullptr;
    std::uint64_t generation = 0;
    StaResultWaker* owner = nullptr;
    {
      std::lock_guard<std::mutex> guard(g_registry_mutex);
      const auto found = g_registry.find(window);
      if (found == g_registry.end()) {
        g_unregistered_wakes.fetch_add(1);
      } else if (found->second.generation !=
                 static_cast<std::uint64_t>(wparam)) {
        g_stale_wakes.fetch_add(1);
      } else {
        target = found->second.target;
        owner = found->second.owner;
        generation = found->second.generation;
      }
    }
    (void)lparam;
    if (owner != nullptr) {
      owner->HandleWake(generation);
    }
    (void)target;
    return 0;
  }
  if (message == WM_NCDESTROY) {
    {
      std::lock_guard<std::mutex> guard(g_registry_mutex);
      g_registry.erase(window);
    }
    ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    return ::DefWindowProcW(window, message, wparam, lparam);
  }
  return ::DefWindowProcW(window, message, wparam, lparam);
}

std::mutex g_class_mutex;
HMODULE g_class_module = nullptr;
int g_class_refcount = 0;

bool EnsureWindowClass(HMODULE module, std::wstring* why) {
  if (module == nullptr) {
    if (why != nullptr) {
      *why = L"the STA waker needs the owning module instance";
    }
    return false;
  }
  std::lock_guard<std::mutex> guard(g_class_mutex);
  if (g_class_refcount > 0) {
    if (g_class_module != module) {
      if (why != nullptr) {
        *why = L"the STA wake window class is already owned by another loaded module";
      }
      return false;
    }
    ++g_class_refcount;
    return true;
  }
  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = WakeWindowProc;
  window_class.hInstance = module;
  window_class.lpszClassName = kWakeWindowClass;
  const ATOM atom = ::RegisterClassExW(&window_class);
  if (atom == 0) {
    if (why != nullptr) {
      *why = L"RegisterClassExW failed with error " +
             std::to_wstring(::GetLastError());
    }
    return false;
  }
  g_class_module = module;
  g_class_refcount = 1;
  return true;
}

void ReleaseWindowClass(HMODULE module) {
  std::lock_guard<std::mutex> guard(g_class_mutex);
  if (g_class_refcount == 0 || g_class_module != module) {
    return;
  }
  --g_class_refcount;
  if (g_class_refcount == 0) {
    (void)::UnregisterClassW(kWakeWindowClass, g_class_module);
    g_class_module = nullptr;
  }
}

}  // namespace

struct StaResultWaker::Impl {
  std::thread watcher;
};

StaResultWaker::StaResultWaker() : impl_(new Impl) {}

StaResultWaker::~StaResultWaker() { Stop(); }

bool StaResultWaker::Start(StaWakeTarget* target, HANDLE executor_event,
                           HMODULE module, std::uint64_t generation,
                           std::wstring* why) {
  if (started_) {
    if (why != nullptr) {
      *why = L"the STA waker is already started";
    }
    return false;
  }
  if (target == nullptr) {
    if (why != nullptr) {
      *why = L"the STA waker needs a target";
    }
    return false;
  }
  if (!EnsureWindowClass(module, why)) {
    return false;
  }
  const HINSTANCE instance = module;
  stop_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  reconfigure_event_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (stop_event_ == nullptr || reconfigure_event_ == nullptr) {
    if (why != nullptr) {
      *why = L"CreateEventW failed with error " + std::to_wstring(::GetLastError());
    }
    if (stop_event_ != nullptr) {
      ::CloseHandle(stop_event_);
      stop_event_ = nullptr;
    }
    if (reconfigure_event_ != nullptr) {
      ::CloseHandle(reconfigure_event_);
      reconfigure_event_ = nullptr;
    }
    return false;
  }
  window_ = ::CreateWindowExW(0, kWakeWindowClass, L"EpoCanva STA wake", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, instance, this);
  if (window_ == nullptr) {
    if (why != nullptr) {
      *why = L"CreateWindowExW(message-only) failed with error " +
             std::to_wstring(::GetLastError());
    }
    ::CloseHandle(stop_event_);
    ::CloseHandle(reconfigure_event_);
    stop_event_ = nullptr;
    reconfigure_event_ = nullptr;
    ReleaseWindowClass(module);
    return false;
  }
  ::SetWindowLongPtrW(window_, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));

  target_ = target;
  module_ = module;
  executor_event_.store(executor_event);
  generation_ = generation;
  stop_requested_.store(false);
  paced_.store(false);  {
    std::lock_guard<std::mutex> guard(g_registry_mutex);
    g_registry[window_] = Registration{this, target_, generation_};
  }

  const HWND window = window_;
  const HANDLE stop_event = stop_event_;
  const HANDLE reconfigure_event = reconfigure_event_;
  const std::uint64_t wake_generation = generation_;
  try {
    impl_->watcher = std::thread([this, window, stop_event, reconfigure_event,
                                  wake_generation] {
      bool retry = false;
      for (;;) {
        const bool paced = paced_.load();
        // The result event is installed after Start when the engine is attached
        // asynchronously; re-read it on every wait so no thread restart is needed.
        const HANDLE result_event = executor_event_.load();
        HANDLE handles[3] = {stop_event, reconfigure_event, result_event};
        const DWORD handle_count = result_event != nullptr ? 3 : 2;
        const DWORD timeout = (paced || retry) ? 10 : INFINITE;
        const DWORD waited =
            ::WaitForMultipleObjects(handle_count, handles, FALSE, timeout);
        if (waited == WAIT_OBJECT_0 || stop_requested_.load()) {
          break;
        }
        if (waited == WAIT_OBJECT_0 + 1) {
          // Reconfiguration changes the wait set; it is not a result.
          continue;
        }
        if (waited == WAIT_FAILED) {
          // A broken wait must not spin: still try the wake, then fall back to pacing.
          retry = true;
        }
        if (!::PostMessageW(window, kWakeMessage,
                            static_cast<WPARAM>(wake_generation), 0)) {
          wake_post_failures_.fetch_add(1);
          retry = true;
        } else {
          wakes_posted_.fetch_add(1);
          retry = false;
        }
      }
    });
  } catch (...) {
    {
      std::lock_guard<std::mutex> guard(g_registry_mutex);
      g_registry.erase(window_);
    }
    ::DestroyWindow(window_);
    window_ = nullptr;
    ::CloseHandle(stop_event_);
    ::CloseHandle(reconfigure_event_);
    stop_event_ = nullptr;
    reconfigure_event_ = nullptr;
    target_ = nullptr;
    executor_event_.store(nullptr);
    ReleaseWindowClass(module_);
    module_ = nullptr;
    if (why != nullptr) {
      *why = L"std::thread construction failed for the STA waker";
    }
    return false;
  }
  {
    std::lock_guard<std::mutex> guard(window_mutex_);
    started_ = true;
  }
  return true;
}void StaResultWaker::HandleWake(std::uint64_t generation) {
  if (!started_ || stop_requested_.load() || generation != generation_ ||
      target_ == nullptr) {
    g_stale_wakes.fetch_add(1);
    return;
  }
  wakes_received_.fetch_add(1);
  target_->OnStaWake(generation);
}

void StaResultWaker::Stop() {
  {
    std::lock_guard<std::mutex> guard(window_mutex_);
    if (!started_) {
      return;
    }
    started_ = false;
  }
  stop_requested_.store(true);
  if (stop_event_ != nullptr) {
    ::SetEvent(stop_event_);
  }
  if (reconfigure_event_ != nullptr) {
    ::SetEvent(reconfigure_event_);
  }
  if (impl_->watcher.joinable()) {
    impl_->watcher.join();
  }
  {
    std::lock_guard<std::mutex> guard(g_registry_mutex);
    g_registry.erase(window_);
  }
  {
    // Serialised with Notify(): after started_ is false no new post starts, and a poster
    // that already observed true has finished before this lock is granted.
    std::lock_guard<std::mutex> guard(window_mutex_);
    if (window_ != nullptr) {
      ::SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
      ::DestroyWindow(window_);
      window_ = nullptr;
    }
  }
  if (stop_event_ != nullptr) {
    ::CloseHandle(stop_event_);
    stop_event_ = nullptr;
  }
  if (reconfigure_event_ != nullptr) {
    ::CloseHandle(reconfigure_event_);
    reconfigure_event_ = nullptr;
  }
  executor_event_.store(nullptr);
  target_ = nullptr;
  if (module_ != nullptr) {
    ReleaseWindowClass(module_);
    module_ = nullptr;
  }
  stops_.fetch_add(1);
}

void StaResultWaker::Notify() {
  std::lock_guard<std::mutex> guard(window_mutex_);
  if (!started_ || stop_requested_.load() || window_ == nullptr) {
    return;
  }
  if (!::PostMessageW(window_, kWakeMessage, static_cast<WPARAM>(generation_), 0)) {
    wake_post_failures_.fetch_add(1);
  } else {
    wakes_posted_.fetch_add(1);
  }
}

void StaResultWaker::SetPacing(bool paced) {
  const bool previous = paced_.exchange(paced);
  if (started_ && previous != paced) {
    RequestWatcherReconfigure();
  }
}

void StaResultWaker::SetExecutorEvent(HANDLE executor_event) {
  executor_event_.store(executor_event);
  RequestWatcherReconfigure();
}

void StaResultWaker::RequestWatcherReconfigure() {
  if (reconfigure_event_ != nullptr) {
    ::SetEvent(reconfigure_event_);
  }
}

StaWakerDiagnostics StaResultWaker::Snapshot() const {
  StaWakerDiagnostics diagnostics;
  diagnostics.started = started_;
  diagnostics.paced = paced_.load();
  diagnostics.generation = generation_;
  diagnostics.wakes_posted = wakes_posted_.load();
  diagnostics.wake_post_failures = wake_post_failures_.load();
  diagnostics.wakes_received = wakes_received_.load();
  diagnostics.stale_wakes_ignored = g_stale_wakes.load();
  diagnostics.wakes_without_registration = g_unregistered_wakes.load();
  diagnostics.stops = stops_.load();
  return diagnostics;
}

}  // namespace product
}  // namespace tsf
}  // namespace epocanva
