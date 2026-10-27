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

bool EnsureWindowClass(HINSTANCE instance, std::wstring* why) {
  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = WakeWindowProc;
  window_class.hInstance = instance;
  window_class.lpszClassName = kWakeWindowClass;
  const ATOM atom = ::RegisterClassExW(&window_class);
  if (atom == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    if (why != nullptr) {
      *why = L"RegisterClassExW failed with error " +
             std::to_wstring(::GetLastError());
    }
    return false;
  }
  return true;
}

}  // namespace

struct StaResultWaker::Impl {
  std::thread watcher;
};

StaResultWaker::StaResultWaker() : impl_(new Impl) {}

StaResultWaker::~StaResultWaker() { Stop(); }

bool StaResultWaker::Start(StaWakeTarget* target, HANDLE executor_event,
                           std::uint64_t generation, std::wstring* why) {
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
  const HINSTANCE instance = ::GetModuleHandleW(nullptr);
  if (!EnsureWindowClass(instance, why)) {
    return false;
  }
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
    return false;
  }
  ::SetWindowLongPtrW(window_, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));

  target_ = target;
  executor_event_ = executor_event;
  generation_ = generation;
  stop_requested_.store(false);
  paced_.store(false);
  {
    std::lock_guard<std::mutex> guard(g_registry_mutex);
    g_registry[window_] = Registration{this, target_, generation_};
  }

  const HWND window = window_;
  const HANDLE stop_event = stop_event_;
  const HANDLE reconfigure_event = reconfigure_event_;
  const HANDLE result_event = executor_event_;
  const std::uint64_t wake_generation = generation_;
  try {
    impl_->watcher = std::thread([this, window, stop_event, reconfigure_event,
                                  result_event, wake_generation] {
      HANDLE handles[3] = {stop_event, reconfigure_event, result_event};
      const DWORD handle_count = result_event != nullptr ? 3 : 2;
      bool retry = false;
      for (;;) {
        const bool paced = paced_.load();
        const DWORD timeout = (paced || retry) ? 10 : INFINITE;
        const DWORD waited =
            ::WaitForMultipleObjects(handle_count, handles, FALSE, timeout);
        if (waited == WAIT_OBJECT_0 || stop_requested_.load()) {
          break;
        }
        if (waited == WAIT_OBJECT_0 + 1) {
          // Reconfiguration changes the timeout for the next wait; it is not a result.
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
    executor_event_ = nullptr;
    if (why != nullptr) {
      *why = L"std::thread construction failed for the STA waker";
    }
    return false;
  }
  started_ = true;
  return true;
}

void StaResultWaker::HandleWake(std::uint64_t generation) {
  if (!started_ || stop_requested_.load() || generation != generation_ ||
      target_ == nullptr) {
    g_stale_wakes.fetch_add(1);
    return;
  }
  wakes_received_.fetch_add(1);
  target_->OnStaWake(generation);
}

void StaResultWaker::Stop() {
  if (!started_) {
    return;
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
  ::SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
  if (window_ != nullptr) {
    ::DestroyWindow(window_);
    window_ = nullptr;
  }
  if (stop_event_ != nullptr) {
    ::CloseHandle(stop_event_);
    stop_event_ = nullptr;
  }
  if (reconfigure_event_ != nullptr) {
    ::CloseHandle(reconfigure_event_);
    reconfigure_event_ = nullptr;
  }
  executor_event_ = nullptr;
  target_ = nullptr;
  started_ = false;
  stops_.fetch_add(1);
}

void StaResultWaker::SetPacing(bool paced) {
  const bool previous = paced_.exchange(paced);
  if (started_ && previous != paced) {
    RequestWatcherReconfigure();
  }
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
