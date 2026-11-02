// Exercises the production runtime with a test-only DLL, never with live input.
// Poison cases run in separate processes because a failed finalize reserves the slot.
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "rime_runtime.h"

namespace engine = epocanva::windows::engine;

int wmain(int argc, wchar_t** argv) {
  if (argc != 4) {
    return 2;
  }
  const int mode = std::stoi(argv[1]);
  engine::RuntimeOptions options;
  options.rime_dll_path = std::filesystem::absolute(argv[2]);
  options.shared_data_dir = std::filesystem::absolute(argv[3]);
  options.user_data_dir = options.shared_data_dir;
  options.log_dir = options.shared_data_dir;

  HMODULE fixture = ::LoadLibraryExW(options.rime_dll_path.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (fixture == nullptr) {
    return 3;
  }
  using Configure = void (*)(int);
  using Count = int (*)(int);
  const auto configure = reinterpret_cast<Configure>(
      ::GetProcAddress(fixture, "RuntimeFaultConfigure"));
  const auto count = reinterpret_cast<Count>(::GetProcAddress(fixture, "RuntimeFaultCount"));
  if (configure == nullptr || count == nullptr) {
    ::FreeLibrary(fixture);
    return 3;
  }
  configure(mode);
  int failed = 0;
  int checks = 0;
  auto check = [&](bool condition, const char* label) {
    ++checks;
    if (!condition) {
      ++failed;
      std::printf("FAIL runtime-fault %s\n", label);
    }
  };
  const bool poison = mode >= 2;
  {
    engine::RimeRuntime runtime;
    const auto started = runtime.Start(options);
    const auto want_start = mode == 0 || mode == 3
        ? engine::RuntimeStartStatus::kOk
        : (mode == 4 ? engine::RuntimeStartStatus::kEnglishModuleMissing
                     : engine::RuntimeStartStatus::kInitializeFailed);
    check(started == want_start, "start status");
    check(count(0) == 1, "initialize entered exactly once");
    const auto shutdown = runtime.Shutdown();
    check(shutdown == (poison ? engine::RuntimeShutdownStatus::kFinalizeFailed
                             : (mode == 1 ? engine::RuntimeShutdownStatus::kAlreadyShutdown
                                          : engine::RuntimeShutdownStatus::kOk)),
          "shutdown preserves cleanup status");
    check(runtime.Shutdown() == (poison ? engine::RuntimeShutdownStatus::kFinalizeFailed
                                       : engine::RuntimeShutdownStatus::kAlreadyShutdown),
          "repeated shutdown has terminal status");
    check(count(1) == 1 && runtime.Info().finalize_calls == 1,
          "finalize invoked once including a throw");
    check(!runtime.IsInitialized(), "runtime no longer admits operations");
    check(runtime.CreateSession("unused").status == engine::SessionCreateStatus::kNotStarted,
          "no session after failed shutdown");
    if (poison) {
      check(!runtime.Info().last_shutdown_diagnostic.empty(), "cleanup failure is observable");
    }
  }
  check(count(1) == 1, "destructor never retries finalize");
  check(count(2) == 0, "lifecycle cases made no session or input call");
  {
    engine::RimeRuntime next;
    const auto started = next.Start(options);
    if (poison) {
      check(started == engine::RuntimeStartStatus::kAnotherRuntimeActive,
            "failed cleanup keeps process slot reserved");
      check(count(0) == 1 && count(1) == 1, "refused runtime never reenters the API");
    } else {
      check(started == (mode == 1 ? engine::RuntimeStartStatus::kInitializeFailed
                                  : engine::RuntimeStartStatus::kOk),
            "clean cleanup permits another runtime");
      (void)next.Shutdown();
      check(count(0) == 2 && count(1) == 2, "new runtime owns a separate clean cycle");
    }
  }
  ::FreeLibrary(fixture);  // only the test's extra reference, not the runtime's retained one
  if (failed != 0) {
    std::printf("RESULT FAIL runtime-fault %d/%d checks\n", failed, checks);
    return 5;
  }
  std::printf("RESULT PASS runtime-fault mode=%d checks=%d\n", mode, checks);
  return 0;
}
