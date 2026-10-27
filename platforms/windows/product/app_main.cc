// EpoCanva W1 -- the product shell entry point.
//
// GUI subsystem: normal launch shows the settings window. --diagnose writes the same
// status report the window shows and never creates a window, which is what the automated
// package tests use for missing files, corrupt settings and non-ASCII paths.
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#include "product_diagnostics.h"
#include "product_io.h"
#include "product_paths.h"
#include "settings_window.h"
#include "ui_ids.h"

namespace {

namespace ui = epocanva::product::ui;

struct Options {
  std::filesystem::path user_root;
  std::filesystem::path diagnose_path;
  bool diagnose = false;
  bool help = false;
};

std::wstring Usage() {
  return L"Usage:\n"
         L"  EpoCanva.exe [--user-data-root <absolute folder>]\n"
         L"  EpoCanva.exe --diagnose <report file> [--user-data-root <absolute folder>]\n";
}

bool ParseArguments(int argc, wchar_t** argv, Options* options, std::wstring* why) {
  for (int index = 1; index < argc; ++index) {
    const std::wstring argument = argv[index];
    if (argument == L"--user-data-root") {
      if (index + 1 >= argc) {
        *why = L"--user-data-root needs a folder";
        return false;
      }
      const std::filesystem::path root(argv[++index]);
      if (!root.is_absolute()) {
        *why = L"--user-data-root must be an absolute path";
        return false;
      }
      options->user_root = root;
    } else if (argument == L"--diagnose") {
      if (index + 1 >= argc) {
        *why = L"--diagnose needs a report file";
        return false;
      }
      options->diagnose = true;
      options->diagnose_path = std::filesystem::path(argv[++index]);
    } else if (argument == L"--help" || argument == L"-h") {
      options->help = true;
    } else {
      *why = L"unknown argument: " + argument;
      return false;
    }
  }
  return true;
}

int RunDiagnose(const Options& options) {
  epocanva::product::ProductStatus status;
  std::wstring why;
  std::error_code ec;
  const std::filesystem::path report_path =
      options.diagnose_path.is_absolute()
          ? options.diagnose_path
          : std::filesystem::absolute(options.diagnose_path, ec);
  if (report_path.empty() || ec) {
    return 1;
  }
  std::string report;
  bool collected = epocanva::product::CollectProductStatus(nullptr, &status, &why);
  if (!collected) {
    report = "product=EpoCanva\npackage=incomplete\ndiagnose_error=";
    report += epocanva::product::WideToUtf8(why);
    report += "\n";
  } else {
    report = epocanva::product::FormatProductReport(status);
  }
  std::wstring write_why;
  if (!epocanva::product::WriteTextAtomically(report_path, report, &write_why)) {
    return 1;
  }
  return collected && status.package_ready ? 0 : 2;
}

int RunWindow(HINSTANCE instance, int show_command) {
  bool com_initialized = false;
  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                    COINIT_DISABLE_OLE1DDE);
  if (SUCCEEDED(com)) {
    com_initialized = true;
  } else if (com != RPC_E_CHANGED_MODE) {
    ::MessageBoxW(nullptr, L"COM initialisation failed.", ui::kSettingsWindowTitle,
                  MB_OK | MB_ICONERROR);
    return 1;
  }

  INITCOMMONCONTROLSEX controls = {};
  controls.dwSize = sizeof(controls);
  controls.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
  ::InitCommonControlsEx(&controls);

  using SetDpiContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
  const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
  if (user32 != nullptr) {
    const auto set_context = reinterpret_cast<SetDpiContextFn>(
        ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (set_context != nullptr) {
      set_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
  }

  if (!epocanva::product::SettingsWindow::RegisterClass(instance)) {
    ::MessageBoxW(nullptr, L"RegisterClassExW failed.", ui::kSettingsWindowTitle,
                  MB_OK | MB_ICONERROR);
    if (com_initialized) {
      ::CoUninitialize();
    }
    return 1;
  }
  epocanva::product::SettingsWindow window;
  std::wstring why;
  if (!window.Create(instance, show_command, &why)) {
    const std::wstring message = L"Cannot create the settings window:\n" + why;
    ::MessageBoxW(nullptr, message.c_str(), ui::kSettingsWindowTitle,
                  MB_OK | MB_ICONERROR);
    if (com_initialized) {
      ::CoUninitialize();
    }
    return 1;
  }
  ::SetFocus(::GetDlgItem(window.hwnd(), epocanva::product::ui::kCompletionEnabled));

  ACCEL accelerators[] = {
      {FVIRTKEY | FCONTROL, 'S', epocanva::product::ui::kSave},
      {FVIRTKEY, VK_F1, epocanva::product::ui::kAbout},
  };
  HACCEL accelerator_table =
      ::CreateAcceleratorTableW(accelerators,
                                static_cast<int>(sizeof(accelerators) /
                                                 sizeof(accelerators[0])));

  MSG message = {};
  int result = 0;
  for (;;) {
    const BOOL got = ::GetMessageW(&message, nullptr, 0, 0);
    if (got == 0) {
      result = static_cast<int>(message.wParam);
      break;
    }
    if (got == -1) {
      result = 1;
      break;
    }
    if (accelerator_table != nullptr &&
        ::TranslateAcceleratorW(window.hwnd(), accelerator_table, &message) != 0) {
      continue;
    }
    if (::IsDialogMessageW(window.hwnd(), &message) != 0) {
      continue;
    }
    ::TranslateMessage(&message);
    ::DispatchMessageW(&message);
  }

  if (accelerator_table != nullptr) {
    ::DestroyAcceleratorTable(accelerator_table);
  }
  if (com_initialized) {
    ::CoUninitialize();
  }
  return result;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR command_line,
                    int show_command) {
  (void)previous;
  (void)command_line;
  int argc = 0;
  wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
  if (argv == nullptr) {
    return 1;
  }
  Options options;
  std::wstring why;
  const bool parsed = ParseArguments(argc, argv, &options, &why);
  ::LocalFree(argv);
  if (!parsed) {
    MessageBoxW(nullptr, (why + L"\n\n" + Usage()).c_str(), ui::kSettingsWindowTitle,
                MB_OK | MB_ICONERROR);
    return 2;
  }
  if (options.help) {
    MessageBoxW(nullptr, Usage().c_str(), ui::kSettingsWindowTitle,
                MB_OK | MB_ICONINFORMATION);
    return 0;
  }
  if (!options.user_root.empty()) {
    epocanva::product::SetUserRootOverrideForProcess(options.user_root);
  }
  if (options.diagnose) {
    return RunDiagnose(options);
  }
  return RunWindow(instance, show_command);
}
