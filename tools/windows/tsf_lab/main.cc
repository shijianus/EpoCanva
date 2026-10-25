// EpoCanva P3.11/P3.13 -- the visible TSF integration lab entry point.
//
// DEFAULT PATHS are supplied by CMake as compile definitions derived from the existing
// engine-bridge cache variables. They are not hardcoded machine paths in this source. Every
// one can be overridden on the command line; see --help.
//
// This program is NOT an input method: it never registers a TSF service, advises a key
// event sink, calls SendInput, installs a hook, or writes to another application.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#include "lab_self_test.h"
#include "lab_session.h"
#include "lab_window.h"
#include "scenario_runner.h"

#ifndef EPOCANVA_LAB_DEFAULT_RIME_DLL
#define EPOCANVA_LAB_DEFAULT_RIME_DLL ""
#endif
#ifndef EPOCANVA_LAB_DEFAULT_FIXTURE_DIR
#define EPOCANVA_LAB_DEFAULT_FIXTURE_DIR ""
#endif
#ifndef EPOCANVA_LAB_DEFAULT_SPELLING_FIXTURE_DIR
#define EPOCANVA_LAB_DEFAULT_SPELLING_FIXTURE_DIR ""
#endif
#ifndef EPOCANVA_LAB_DEFAULT_RUN_ROOT
#define EPOCANVA_LAB_DEFAULT_RUN_ROOT ""
#endif

namespace {

using epocanva::tsf::lab::LabConfig;
using epocanva::tsf::lab::LabWindow;
using epocanva::tsf::lab::LabWindowOptions;
using epocanva::tsf::lab::RunLabSelfTest;
using epocanva::tsf::lab::ScenarioId;

void PrintUsage() {
  std::printf(
      "EpoCanva P3.13 visible TSF integration lab\n"
      "\n"
      "usage: tsf_lab [--rime-dll PATH] [--fixture-dir DIR]\n"
      "               [--spelling-fixture-dir DIR] [--run-root DIR]\n"
      "               [--schema ID] [--self-test [--case NAME]]\n"
      "               [--screenshot FILE.bmp] [--screenshot-scenario N]\n"
      "\n"
      "interactive (default): opens the lab window. Click A or B for real Win32\n"
      "focus, then a fixed scenario button. Typed keys are ignored.\n"
      "\n"
      "--self-test --case window-lifecycle  hidden window creation/destruction\n"
      "--self-test --case scenarios         all nine scenarios + resource balance\n"
      "--self-test --case all               both (default)\n"
      "--screenshot FILE.bmp                show the window, run scenario 1 (or\n"
      "                                     --screenshot-scenario N), render the\n"
      "                                     window to BMP, then exit\n"
      "--screenshot-scenario N              scenario number 1..9 for the capture\n"
      "\n"
      "No input method is registered and no system keyboard sink is installed.\n");
}

bool ParseArgs(int argc, char** argv, LabConfig* config, bool* self_test,
               std::string* case_name, std::filesystem::path* screenshot,
               int* screenshot_scenario, bool* show_help) {
  config->rime_dll = EPOCANVA_LAB_DEFAULT_RIME_DLL;
  config->fixture_dir = EPOCANVA_LAB_DEFAULT_FIXTURE_DIR;
  config->spelling_fixture_dir = EPOCANVA_LAB_DEFAULT_SPELLING_FIXTURE_DIR;
  config->run_root = EPOCANVA_LAB_DEFAULT_RUN_ROOT;
  *self_test = false;
  *case_name = "all";
  *show_help = false;
  for (int index = 1; index < argc; ++index) {
    const char* argument = argv[index];
    if (std::strcmp(argument, "--help") == 0 ||
        std::strcmp(argument, "-h") == 0) {
      *show_help = true;
    } else if (std::strcmp(argument, "--rime-dll") == 0 && index + 1 < argc) {
      config->rime_dll = argv[++index];
    } else if (std::strcmp(argument, "--fixture-dir") == 0 && index + 1 < argc) {
      config->fixture_dir = argv[++index];
    } else if (std::strcmp(argument, "--spelling-fixture-dir") == 0 &&
               index + 1 < argc) {
      config->spelling_fixture_dir = argv[++index];
    } else if (std::strcmp(argument, "--run-root") == 0 && index + 1 < argc) {
      config->run_root = argv[++index];
    } else if (std::strcmp(argument, "--schema") == 0 && index + 1 < argc) {
      config->schema_id = argv[++index];
    } else if (std::strcmp(argument, "--self-test") == 0) {
      *self_test = true;
    } else if (std::strcmp(argument, "--case") == 0 && index + 1 < argc) {
      *case_name = argv[++index];
    } else if (std::strcmp(argument, "--screenshot") == 0 && index + 1 < argc) {
      *screenshot = argv[++index];
    } else if (std::strcmp(argument, "--screenshot-scenario") == 0 &&
               index + 1 < argc) {
      const int scenario = std::atoi(argv[++index]);
      if (scenario < 1 || scenario > 9) {
        std::fprintf(stderr, "tsf_lab: --screenshot-scenario must be 1..9\n");
        return false;
      }
      *screenshot_scenario = scenario;
    } else {
      std::fprintf(stderr, "tsf_lab: unknown or incomplete argument: %s\n",
                   argument);
      return false;
    }
  }
  return true;
}

bool RequireSessionPaths(const LabConfig& config) {
  if (config.rime_dll.empty() || config.fixture_dir.empty() ||
      config.spelling_fixture_dir.empty() || config.run_root.empty()) {
    std::fprintf(stderr,
                 "tsf_lab: rime dll, both fixture directories and run-root are "
                 "required (pass --rime-dll/--fixture-dir/"
                 "--spelling-fixture-dir/--run-root)\n");
    return false;
  }
  return true;
}

bool NormalizePaths(LabConfig* config, std::string* error) {
  std::error_code ec;
  config->rime_dll = std::filesystem::absolute(config->rime_dll, ec);
  if (!ec) {
    config->fixture_dir = std::filesystem::absolute(config->fixture_dir, ec);
  }
  if (!ec) {
    config->spelling_fixture_dir =
        std::filesystem::absolute(config->spelling_fixture_dir, ec);
  }
  if (!ec) {
    config->run_root = std::filesystem::absolute(config->run_root, ec);
  }
  if (ec) {
    if (error != nullptr) {
      *error = "cannot make a lab path absolute: " + ec.message();
    }
    return false;
  }
  return true;
}

void PumpUiBriefly(HWND window) {
  for (int pump = 0; pump < 8; ++pump) {
    MSG message = {};
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
      ::TranslateMessage(&message);
      ::DispatchMessageW(&message);
    }
    ::RedrawWindow(window, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 15, QS_ALLINPUT);
  }
}

}  // namespace

int main(int argc, char** argv) {
  LabConfig config;
  bool self_test = false;
  bool show_help = false;
  std::string case_name;
  std::filesystem::path screenshot;
  int screenshot_scenario = 1;
  if (!ParseArgs(argc, argv, &config, &self_test, &case_name, &screenshot,
                 &screenshot_scenario, &show_help)) {
    PrintUsage();
    return 2;
  }
  if (show_help) {
    PrintUsage();
    return 0;
  }

  if (self_test) {
    if (case_name != "window-lifecycle" && !RequireSessionPaths(config)) {
      return 2;
    }
    if (case_name != "window-lifecycle") {
      std::string path_error;
      if (!NormalizePaths(&config, &path_error)) {
        std::fprintf(stderr, "tsf_lab: %s\n", path_error.c_str());
        return 2;
      }
    }
    return RunLabSelfTest(config, case_name);
  }

  if (!RequireSessionPaths(config)) {
    PrintUsage();
    return 2;
  }
  {
    std::string path_error;
    if (!NormalizePaths(&config, &path_error)) {
      std::fprintf(stderr, "tsf_lab: %s\n", path_error.c_str());
      return 2;
    }
  }

  LabWindowOptions options;
  options.config = config;
  options.show = true;
  options.start_session = true;
  // The visible lab uses real Win32 focus: the child surface receives WM_SETFOCUS and the
  // application then calls ITfThreadMgr::SetFocus. The headless self-test explicitly uses
  // the programmatic path instead.
  options.use_window_focus = true;

  std::string error;
  std::unique_ptr<LabWindow> window = LabWindow::Create(options, &error);
  if (window == nullptr) {
    std::fprintf(stderr, "tsf_lab: window creation failed: %s\n", error.c_str());
    return 5;
  }
  if (!window->session_ready()) {
    std::fprintf(stderr, "tsf_lab: session start failed: %s\n",
                 window->startup_error().c_str());
    window->Destroy();
    return 5;
  }

  if (!screenshot.empty()) {
    // Let the activation/focus messages produced by ShowWindow settle before the scripted
    // focus call. This is a real message pump, not a synthesized focus event.
    PumpUiBriefly(window->hwnd());
    bool scenario_ok = true;
    if (window->runner() != nullptr) {
      const auto result = window->runner()->Run(
          static_cast<ScenarioId>(screenshot_scenario));
      scenario_ok = result.ok();
      if (!result.ok()) {
        std::fprintf(stderr, "tsf_lab: screenshot scenario failed: %s\n",
                     result.detail.c_str());
      }
    }
    PumpUiBriefly(window->hwnd());
    window->RefreshView();
    std::string capture_error;
    if (!window->CaptureToBmp(screenshot, &capture_error)) {
      std::fprintf(stderr, "tsf_lab: screenshot failed: %s\n",
                   capture_error.c_str());
      window->Destroy();
      return 5;
    }
    std::printf("tsf_lab: screenshot written to %s\n",
                screenshot.string().c_str());
    window->Destroy();
    return scenario_ok ? 0 : 5;
  }

  window->RunMessageLoop();
  return 0;
}
