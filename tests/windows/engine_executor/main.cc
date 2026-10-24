// EpoCanva P3.5 -- engine executor verification program entry point.
//
// MODES
//   scheduler        controlled backend; no rime.dll; scheduling/state/capacity/results
//   engine           real English engine through EngineExecutor
//   race             real engine + concurrent submissions racing Stop
//   startup-failure  real runtime start failures and failed-initialization cleanup
//   self-check-fail  falsifiability control; must exit 14 and print no RESULT PASS
//
// EXIT CODES: 0 pass; 2 usage; 5 a check failed; 14 the self-check-fail control's expected
// failure. The program never prints live user text; only fixed fixture characters appear.
#include <cstdio>
#include <string>

#include "test_support.h"

namespace epocanva {
namespace tests {
namespace engine_executor {

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitCheckFailed = 5;
constexpr int kExitSelfCheckFailed = 14;

bool ParseArgs(int argc, char** argv, Options* options, std::string* error) {
  for (int i = 1; i < argc; ++i) {
    const std::string name(argv[i]);
    if (name == "--mode" || name == "--rime-dll" || name == "--baseline-dll" ||
        name == "--fixture-dir" || name == "--spelling-fixture-dir" ||
        name == "--run-root") {
      if (i + 1 >= argc) {
        *error = name + " requires a value";
        return false;
      }
      const std::string value(argv[++i]);
      if (name == "--mode") {
        options->mode = value;
      } else if (name == "--rime-dll") {
        options->rime_dll = value;
      } else if (name == "--baseline-dll") {
        options->baseline_dll = value;
      } else if (name == "--fixture-dir") {
        options->fixture_dir = value;
      } else if (name == "--spelling-fixture-dir") {
        options->spelling_fixture_dir = value;
      } else {
        options->run_root = value;
      }
      continue;
    }
    *error = "unknown argument: " + name;
    return false;
  }
  if (options->mode.empty()) {
    *error = "--mode is required";
    return false;
  }
  if (options->mode == "scheduler" || options->mode == "self-check-fail") {
    return true;
  }
  if (options->mode == "engine" || options->mode == "race" ||
      options->mode == "startup-failure") {
    if (options->rime_dll.empty() || options->run_root.empty()) {
      *error = options->mode + " mode requires --rime-dll and --run-root";
      return false;
    }
    if (options->mode != "startup-failure" &&
        (options->fixture_dir.empty() || options->spelling_fixture_dir.empty())) {
      *error = options->mode +
               " mode requires --fixture-dir and --spelling-fixture-dir";
      return false;
    }
    if (options->mode == "startup-failure" &&
        (options->fixture_dir.empty() || options->spelling_fixture_dir.empty())) {
      *error =
          "startup-failure mode requires --fixture-dir and --spelling-fixture-dir "
          "for the fixture staging check";
      return false;
    }
    return true;
  }
  *error = "unknown mode: " + options->mode;
  return false;
}

int FinishRun() {
  if (g_failed != 0) {
    std::printf("[executor-win] RESULT FAIL (%d checks, %d failed)\n", g_total,
                g_failed);
    return kExitCheckFailed;
  }
  std::printf("[executor-win] RESULT PASS (%d checks, %d failed)\n", g_total, g_failed);
  return kExitOk;
}

int FinishSelfCheck() {
  if (g_failed == 0) {
    std::printf("[executor-win] RESULT PASS (%d checks, 0 failed)\n", g_total);
    return kExitOk;
  }
  std::printf("[executor-win] RESULT FAIL (%d checks, %d failed)\n", g_total, g_failed);
  return kExitSelfCheckFailed;
}

}  // namespace

}  // namespace engine_executor
}  // namespace tests
}  // namespace epocanva

using namespace epocanva::tests::engine_executor;

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!ParseArgs(argc, argv, &options, &error)) {
    std::printf("engine_executor_tests: %s\n", error.c_str());
    std::printf(
        "usage: engine_executor_tests --mode "
        "scheduler|engine|race|startup-failure|self-check-fail "
        "[--rime-dll <abs>] [--baseline-dll <abs>] [--fixture-dir <dir>] "
        "[--spelling-fixture-dir <dir>] [--run-root <.../engine-executor-run>]\n");
    return 2;
  }

  if (options.mode == "scheduler") {
    RunSchedulerMode();
    return FinishRun();
  }
  if (options.mode == "engine") {
    RunEngineMode(options);
    return FinishRun();
  }
  if (options.mode == "race") {
    RunRaceMode(options);
    return FinishRun();
  }
  if (options.mode == "startup-failure") {
    RunStartupFailureMode(options);
    return FinishRun();
  }
  RunSelfCheckFail();
  return FinishSelfCheck();
}
