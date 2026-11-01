// EpoCanva W1 -- real window automation for EpoCanva.exe.
//
// Launches the product executable, waits for the actual window, checks DPI awareness,
// tab stops, keyboard traversal, saves settings through the Save button, verifies the
// INI/patch/marker on disk, relaunches and reads the controls back, handles a corrupt
// settings file, and captures the window with PrintWindow. No code inspection is used as
// a substitute for running the UI.
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "product_io.h"
#include "product_settings.h"
#include "ui_ids.h"

namespace {

namespace ui = epocanva::product::ui;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
  if (!condition) {
    std::cout << "FAIL: " << what << "\n";
    ++g_failures;
  }
}

std::string ToUtf8(const std::wstring& text) {
  return epocanva::product::WideToUtf8(text);
}

std::wstring WindowText(HWND window) {
  const int length = ::GetWindowTextLengthW(window);
  if (length <= 0) {
    return std::wstring();
  }
  std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
  const int written = ::GetWindowTextW(window, text.data(), length + 1);
  if (written <= 0) {
    return std::wstring();
  }
  text.resize(static_cast<std::size_t>(written));
  return text;
}

// GetWindowText cannot read a control in another process; WM_GETTEXT can.
std::wstring ControlText(HWND control) {
  if (control == nullptr) {
    return std::wstring();
  }
  std::vector<wchar_t> buffer(8192, L'\0');
  const LRESULT written = ::SendMessageW(
      control, WM_GETTEXT, static_cast<WPARAM>(buffer.size()),
      reinterpret_cast<LPARAM>(buffer.data()));
  if (written <= 0) {
    return std::wstring();
  }
  return std::wstring(buffer.data(), static_cast<std::size_t>(written));
}

struct Process {
  HANDLE handle = nullptr;
  DWORD pid = 0;
};

bool Launch(const std::filesystem::path& exe, const std::filesystem::path& user_root,
            Process* process, std::wstring* why) {
  std::error_code ec;
  std::filesystem::create_directories(user_root, ec);
  if (ec) {
    *why = L"cannot create the user data root";
    return false;
  }
  std::wstring command = L"\"" + exe.wstring() + L"\" --user-data-root \"" +
                        user_root.wstring() + L"\"";
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION info = {};
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');
  if (!::CreateProcessW(exe.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup,
                        &info)) {
    *why = L"CreateProcessW failed with error " + std::to_wstring(::GetLastError());
    return false;
  }
  ::CloseHandle(info.hThread);
  process->handle = info.hProcess;
  process->pid = info.dwProcessId;
  return true;
}

bool ProcessRunning(const Process& process, DWORD* exit_code) {
  const DWORD wait = ::WaitForSingleObject(process.handle, 0);
  if (wait == WAIT_TIMEOUT) {
    return true;
  }
  if (exit_code != nullptr && ::GetExitCodeProcess(process.handle, exit_code) == FALSE) {
    *exit_code = 0xFFFFFFFFu;
  }
  return false;
}

void CloseProcess(Process* process) {
  if (process->handle == nullptr) {
    return;
  }
  DWORD exit_code = 0;
  if (ProcessRunning(*process, &exit_code)) {
    ::TerminateProcess(process->handle, 99);
    ::WaitForSingleObject(process->handle, 5000);
  }
  ::CloseHandle(process->handle);
  process->handle = nullptr;
  process->pid = 0;
}

HWND WaitForWindow(const Process& process, std::wstring* why) {
  for (int attempt = 0; attempt < 150; ++attempt) {
    const HWND window = ::FindWindowW(ui::kSettingsWindowClass, nullptr);
    if (window != nullptr && ::IsWindowVisible(window) &&
        ::GetDlgItem(window, ui::kSave) != nullptr) {
      return window;
    }
    DWORD exit_code = 0;
    if (!ProcessRunning(process, &exit_code)) {
      *why = L"EpoCanva.exe exited before creating the window (exit code " +
             std::to_wstring(exit_code) + L")";
      return nullptr;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  *why = L"the settings window did not appear within 15 seconds";
  return nullptr;
}

bool WaitForExit(Process* process, DWORD timeout_ms, DWORD* exit_code,
                 std::wstring* why) {
  const DWORD wait = ::WaitForSingleObject(process->handle, timeout_ms);
  if (wait != WAIT_OBJECT_0) {
    *why = L"EpoCanva.exe did not exit after WM_CLOSE";
    return false;
  }
  if (::GetExitCodeProcess(process->handle, exit_code) == FALSE) {
    *why = L"GetExitCodeProcess failed";
    return false;
  }
  return true;
}

void EnsureCheck(HWND checkbox, bool checked) {
  const LRESULT current = ::SendMessageW(checkbox, BM_GETCHECK, 0, 0);
  if ((current == BST_CHECKED) != checked) {
    ::SendMessageW(checkbox, BM_CLICK, 0, 0);
  }
}

bool ControlHasTabStop(HWND control) {
  const LONG_PTR style = ::GetWindowLongPtrW(control, GWL_STYLE);
  return (style & WS_TABSTOP) != 0;
}

bool InsideClient(HWND parent, HWND control) {
  RECT client = {};
  if (!::GetClientRect(parent, &client)) {
    return false;
  }
  RECT rect = {};
  if (!::GetWindowRect(control, &rect)) {
    return false;
  }
  MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&rect), 2);
  return rect.left >= client.left && rect.top >= client.top &&
         rect.right <= client.right && rect.bottom <= client.bottom &&
         rect.right - rect.left > 0 && rect.bottom - rect.top > 0;
}

HWND FocusWindow(DWORD thread_id) {
  GUITHREADINFO info = {};
  info.cbSize = sizeof(info);
  if (!::GetGUIThreadInfo(thread_id, &info)) {
    return nullptr;
  }
  return info.hwndFocus;
}

bool SaveScreenshot(HWND window, const std::filesystem::path& path,
                    std::wstring* why) {
  ::RedrawWindow(window, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
  RECT rect = {};
  if (!::GetWindowRect(window, &rect)) {
    *why = L"GetWindowRect failed";
    return false;
  }
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  HDC screen = ::GetDC(nullptr);
  HDC memory = ::CreateCompatibleDC(screen);
  HBITMAP bitmap = ::CreateCompatibleBitmap(screen, width, height);
  if (screen == nullptr || memory == nullptr || bitmap == nullptr) {
    if (bitmap != nullptr) ::DeleteObject(bitmap);
    if (memory != nullptr) ::DeleteDC(memory);
    if (screen != nullptr) ::ReleaseDC(nullptr, screen);
    *why = L"bitmap allocation failed";
    return false;
  }
  HGDIOBJ old = ::SelectObject(memory, bitmap);
  const BOOL printed = ::PrintWindow(window, memory, 2);
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  std::vector<unsigned char> pixels(static_cast<std::size_t>(width) *
                                    static_cast<std::size_t>(height) * 4u);
  const int lines = printed ? ::GetDIBits(memory, bitmap, 0, static_cast<UINT>(height),
                                          pixels.data(), &info, DIB_RGB_COLORS)
                            : 0;
  ::SelectObject(memory, old);
  ::DeleteObject(bitmap);
  ::DeleteDC(memory);
  ::ReleaseDC(nullptr, screen);
  if (!printed || lines != height) {
    *why = L"PrintWindow or GetDIBits failed";
    return false;
  }
  BITMAPFILEHEADER file_header = {};
  file_header.bfType = 0x4D42;
  file_header.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
  file_header.bfSize = file_header.bfOffBits + static_cast<DWORD>(pixels.size());
  BITMAPINFOHEADER header = info.bmiHeader;
  header.biSizeImage = static_cast<DWORD>(pixels.size());
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    *why = L"cannot open the screenshot file";
    return false;
  }
  stream.write(reinterpret_cast<const char*>(&file_header), sizeof(file_header));
  stream.write(reinterpret_cast<const char*>(&header), sizeof(header));
  stream.write(reinterpret_cast<const char*>(pixels.data()),
               static_cast<std::streamsize>(pixels.size()));
  if (!stream) {
    *why = L"writing the screenshot failed";
    return false;
  }
  return true;
}

std::string ReadText(const std::filesystem::path& path) {
  std::vector<std::uint8_t> bytes;
  std::wstring why;
  if (!epocanva::product::ReadFileBytes(path, &bytes, &why)) {
    return std::string();
  }
  return std::string(bytes.begin(), bytes.end());
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

void CheckInitialWindow(HWND window) {
  RECT window_rect = {};
  RECT client_rect = {};
  ::GetWindowRect(window, &window_rect);
  ::GetClientRect(window, &client_rect);
  std::cout << "window=" << (window_rect.right - window_rect.left) << "x"
            << (window_rect.bottom - window_rect.top) << " client="
            << (client_rect.right - client_rect.left) << "x"
            << (client_rect.bottom - client_rect.top)
            << " dpi=" << ::GetDpiForWindow(window) << "\n";
  Check(::GetDlgItem(window, ui::kSave) != nullptr, "Save control exists");
  Check(::GetDlgItem(window, ui::kClose) != nullptr, "Close control exists");
  Check(::GetDlgItem(window, ui::kAbout) != nullptr, "About control exists");
  Check(::GetDlgItem(window, ui::kSummary) != nullptr, "summary control exists");
  Check(::GetDlgItem(window, ui::kStatusEdit) != nullptr, "status edit exists");
  Check(::GetDlgItem(window, ui::kCompletionEnabled) != nullptr,
        "completion checkbox exists");
  Check(::GetDlgItem(window, ui::kSpellingEnabled) != nullptr,
        "spelling checkbox exists");
  Check(::GetDlgItem(window, ui::kCompletionCount) != nullptr,
        "completion count combo exists");
  Check(::GetDlgItem(window, ui::kSpellingCount) != nullptr,
        "spelling count combo exists");
  Check(::GetDlgItem(window, ui::kCandidateDisplayHost) != nullptr,
        "candidate display host radio exists");
  Check(::GetDlgItem(window, ui::kCandidateDisplayAlwaysSelf) != nullptr,
        "candidate display always_self radio exists");
  Check(::GetDlgItem(window, ui::kPathInstall) != nullptr, "install path field exists");
  Check(::GetDlgItem(window, ui::kPathUser) != nullptr, "user path field exists");
  Check(::GetDlgItem(window, ui::kPathSettings) != nullptr,
        "settings path field exists");
  Check(::GetDlgItem(window, ui::kPathEngine) != nullptr, "engine path field exists");

  for (int id : {ui::kSave, ui::kClose, ui::kAbout, ui::kCompletionEnabled,
                 ui::kSpellingEnabled, ui::kCompletionCount, ui::kSpellingCount,
                 ui::kPathInstall, ui::kPathUser,
                 ui::kPathSettings, ui::kPathEngine, ui::kStatusEdit}) {
    HWND control = ::GetDlgItem(window, id);
    Check(control != nullptr && ControlHasTabStop(control),
          "control " + std::to_string(id) + " is in the tab order");
    if (control != nullptr && !ControlHasTabStop(control)) {
      std::cout << "control " << id << " style=0x" << std::hex
                << ::GetWindowLongPtrW(control, GWL_STYLE) << std::dec << "\n";
    }
    Check(control != nullptr && InsideClient(window, control),
          "control " + std::to_string(id) + " is inside the client area");
  }

  const std::wstring title = WindowText(window);
  Check(title.find(L"EpoCanva") != std::wstring::npos, "title names EpoCanva");
  const std::wstring summary = ControlText(::GetDlgItem(window, ui::kSummary));
  Check(summary.find(L"Package") != std::wstring::npos,
        "summary reports the package state");

  for (int id : {ui::kSummary, ui::kCompletionEnabled, ui::kSave}) {
    HWND control = ::GetDlgItem(window, id);
    RECT rect = {};
    ::GetWindowRect(control, &rect);
    std::cout << "control " << id << " rect=" << rect.left << "," << rect.top << " "
              << (rect.right - rect.left) << "x" << (rect.bottom - rect.top)
              << " visible=" << (::IsWindowVisible(control) ? 1 : 0)
              << " style=0x" << std::hex
              << ::GetWindowLongPtrW(control, GWL_STYLE) << std::dec << "\n";
  }
}

void CheckDpi(HANDLE process) {
  using GetProcessDpiAwarenessFn = HRESULT(WINAPI*)(HANDLE, int*);
  const HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
  if (shcore == nullptr) {
    Check(false, "shcore.dll is unavailable, so DPI awareness cannot be verified");
    return;
  }
  const auto get_awareness = reinterpret_cast<GetProcessDpiAwarenessFn>(
      ::GetProcAddress(shcore, "GetProcessDpiAwareness"));
  int awareness = -1;
  if (get_awareness == nullptr || FAILED(get_awareness(process, &awareness))) {
    Check(false, "GetProcessDpiAwareness failed");
  } else {
    Check(awareness == 2, "the window process is per-monitor DPI aware");
  }
  ::FreeLibrary(shcore);
}

void CheckKeyboardTraversal(HWND window, DWORD thread_id) {
  ::SetForegroundWindow(window);
  HWND focus = FocusWindow(thread_id);
  std::vector<HWND> seen;
  for (int attempt = 0; attempt < 8 && focus == nullptr; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    focus = FocusWindow(thread_id);
  }
  Check(focus != nullptr, "the window has a keyboard focus");
  if (focus == nullptr) {
    return;
  }
  seen.push_back(focus);
  for (int step = 0; step < 4; ++step) {
    ::PostMessageW(focus, WM_KEYDOWN, VK_TAB, 0);
    ::PostMessageW(focus, WM_KEYUP, VK_TAB, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    focus = FocusWindow(thread_id);
    if (focus != nullptr &&
        std::find(seen.begin(), seen.end(), focus) == seen.end()) {
      seen.push_back(focus);
    }
  }
  Check(seen.size() >= 2, "Tab moves focus through at least two controls");
}

}  // namespace

int main(int argc, char** argv) {
  // Without this, Windows virtualizes every cross-process rectangle on a scaled
  // monitor: the app is 1372x1052 physical at 144 DPI, this process would see
  // 915x701 and PrintWindow would capture a cropped window.
  ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  std::filesystem::path exe;
  std::filesystem::path user_root;
  std::filesystem::path screenshot;
  for (int index = 1; index < argc; ++index) {
    const std::string option = argv[index];
    if (option == "--exe" && index + 1 < argc) {
      exe = argv[++index];
    } else if (option == "--user-data-root" && index + 1 < argc) {
      user_root = argv[++index];
    } else if (option == "--screenshot" && index + 1 < argc) {
      screenshot = argv[++index];
    }
  }
  if (exe.empty() || user_root.empty() || screenshot.empty() ||
      !std::filesystem::is_regular_file(exe) || !user_root.is_absolute() ||
      !screenshot.is_absolute()) {
    std::cout << "FAIL: --exe, --user-data-root and --screenshot are required "
                 "(absolute paths)\n";
    return 2;
  }

  std::wstring why;
  // This program owns its --user-data-root; a settings file left by an earlier run must
  // not decide the default we are about to assert. Removing it is the reset, not a
  // deletion of user data outside the test root.
  std::error_code reset_ec;
  std::filesystem::remove(user_root / L"settings.ini", reset_ec);
  Process process;
  if (!Launch(exe, user_root, &process, &why)) {
    std::cout << "FAIL: " << ToUtf8(why) << "\n";
    return 2;
  }
  HWND window = WaitForWindow(process, &why);
  if (window == nullptr) {
    std::cout << "FAIL: " << ToUtf8(why) << "\n";
    CloseProcess(&process);
    return 2;
  }
  CheckInitialWindow(window);
  CheckDpi(process.handle);
  CheckKeyboardTraversal(window, ::GetWindowThreadProcessId(window, nullptr));

  HWND completion = ::GetDlgItem(window, ui::kCompletionEnabled);
  HWND spelling = ::GetDlgItem(window, ui::kSpellingEnabled);
  HWND completion_count = ::GetDlgItem(window, ui::kCompletionCount);
  HWND spelling_count = ::GetDlgItem(window, ui::kSpellingCount);
  HWND candidate_host = ::GetDlgItem(window, ui::kCandidateDisplayHost);
  HWND candidate_always = ::GetDlgItem(window, ui::kCandidateDisplayAlwaysSelf);
  const LONG_PTR always_style =
      ::GetWindowLongPtrW(candidate_always, GWL_STYLE);
  Check(candidate_always != nullptr &&
            (always_style & 0x0000000FL) == BS_AUTORADIOBUTTON &&
            (always_style & WS_VISIBLE) != 0 &&
            InsideClient(window, candidate_always),
        "the always_self radio is a visible member of the same group");
  // An auto-radio group owns exactly one tab stop, which Windows moves to the checked
  // button. Requiring both buttons to carry WS_TABSTOP would misread that standard
  // behaviour as a defect.
  Check(ControlHasTabStop(candidate_host) || ControlHasTabStop(candidate_always),
        "the candidate display group is in the tab order");
  HWND save = ::GetDlgItem(window, ui::kSave);

  Check(::SendMessageW(candidate_host, BM_GETCHECK, 0, 0) == BST_CHECKED,
        "the default candidate display follows the host");
  EnsureCheck(completion, false);
  EnsureCheck(spelling, true);
  ::SendMessageW(completion_count, CB_SETCURSEL, 2, 0);  // 3
  ::SendMessageW(spelling_count, CB_SETCURSEL, 1, 0);    // 2
  ::SendMessageW(candidate_always, BM_CLICK, 0, 0);       // user opt-in fallback
  Check(::IsWindowEnabled(completion_count) == FALSE,
        "completion count is disabled when completions are off");
  Check(::IsWindowEnabled(spelling_count) == TRUE,
        "spelling count is enabled when suggestions are on");
  ::SendMessageW(save, BM_CLICK, 0, 0);

  const std::filesystem::path settings_file = user_root / L"settings.ini";
  const std::filesystem::path patch_file =
      user_root / L"engine" / L"epocanva_english.custom.yaml";
  const std::filesystem::path marker_file =
      user_root / L"engine" / L"deploy-required";
  for (int attempt = 0; attempt < 50 && !std::filesystem::exists(settings_file);
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  const std::string settings_text = ReadText(settings_file);
  Check(Contains(settings_text, "completion_enabled=0"),
        "saved INI has completion disabled");
  Check(Contains(settings_text, "completion_candidates=3"),
        "saved INI has 3 completions");
  Check(Contains(settings_text, "spelling_enabled=1"),
        "saved INI has spelling enabled");
  Check(Contains(settings_text, "spelling_candidates=2"),
        "saved INI has 2 suggestions");
  Check(Contains(settings_text, "candidate_display=always_self"),
        "saved INI records the always_self candidate display");
  const std::string patch_text = ReadText(patch_file);
  Check(Contains(patch_text, "english/max_completions\": 0"),
        "saved patch disables completions");
  Check(Contains(patch_text, "english/enable_suggestions\": true"),
        "saved patch enables spelling");
  Check(Contains(patch_text, "english/max_suggestions\": 2"),
        "saved patch sets two suggestions");
  std::string marker;
  Check(epocanva::product::ReadDeployMarkerToken(marker_file, &marker),
        "saved deployment marker exists");
  std::string file_hash;
  std::uint64_t bytes = 0;
  std::wstring hash_why;
  Check(epocanva::product::Sha256File(settings_file, &file_hash, &bytes, &hash_why),
        "settings file can be hashed");
  Check(marker == file_hash, "the marker token is the settings SHA-256");

  ::PostMessageW(window, WM_CLOSE, 0, 0);
  DWORD exit_code = 0;
  if (!WaitForExit(&process, 15000, &exit_code, &why)) {
    std::cout << "FAIL: " << ToUtf8(why) << "\n";
    CloseProcess(&process);
    return 14;
  }
  Check(exit_code == 0, "the first process exits with code 0");
  ::CloseHandle(process.handle);
  process.handle = nullptr;

  // Relaunch: the controls must read the saved values back.
  if (!Launch(exe, user_root, &process, &why)) {
    std::cout << "FAIL: relaunch failed: " << ToUtf8(why) << "\n";
    return 14;
  }
  window = WaitForWindow(process, &why);
  if (window == nullptr) {
    std::cout << "FAIL: " << ToUtf8(why) << "\n";
    CloseProcess(&process);
    return 14;
  }
  completion = ::GetDlgItem(window, ui::kCompletionEnabled);
  spelling = ::GetDlgItem(window, ui::kSpellingEnabled);
  completion_count = ::GetDlgItem(window, ui::kCompletionCount);
  spelling_count = ::GetDlgItem(window, ui::kSpellingCount);
  candidate_host = ::GetDlgItem(window, ui::kCandidateDisplayHost);
  candidate_always = ::GetDlgItem(window, ui::kCandidateDisplayAlwaysSelf);
  Check(::SendMessageW(completion, BM_GETCHECK, 0, 0) == BST_UNCHECKED,
        "relaunch reads completion disabled");
  Check(::SendMessageW(spelling, BM_GETCHECK, 0, 0) == BST_CHECKED,
        "relaunch reads spelling enabled");
  Check(::SendMessageW(candidate_always, BM_GETCHECK, 0, 0) == BST_CHECKED &&
            ::SendMessageW(candidate_host, BM_GETCHECK, 0, 0) == BST_UNCHECKED,
        "relaunch reads the always_self candidate display");
  Check(::SendMessageW(completion_count, CB_GETCURSEL, 0, 0) == 2,
        "relaunch reads completion count 3");
  Check(::SendMessageW(spelling_count, CB_GETCURSEL, 0, 0) == 1,
        "relaunch reads spelling count 2");
  Check(::IsWindowEnabled(completion_count) == FALSE,
        "relaunch disables the completion count");
  Check(::IsWindowEnabled(spelling_count) == TRUE,
        "relaunch enables the spelling count");

  // The About / licences entry must really open a dialog, not just exist as a button.
  ::PostMessageW(::GetDlgItem(window, ui::kAbout), BM_CLICK, 0, 0);
  HWND about = nullptr;
  for (int attempt = 0; attempt < 50; ++attempt) {
    about = ::FindWindowW(nullptr, L"About EpoCanva");
    if (about != nullptr) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  Check(about != nullptr, "About / licences dialog opens");
  if (about != nullptr) {
    ::PostMessageW(about, WM_CLOSE, 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  for (int attempt = 0; attempt < 20; ++attempt) {
    HWND extra = ::FindWindowW(nullptr, L"About EpoCanva");
    if (extra == nullptr) {
      break;
    }
    ::PostMessageW(extra, WM_CLOSE, 0, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  if (!SaveScreenshot(window, screenshot, &why)) {
    std::cout << "FAIL: screenshot failed: " << ToUtf8(why) << "\n";
  } else {
    Check(std::filesystem::is_regular_file(screenshot) &&
              std::filesystem::file_size(screenshot) > 10000,
          "screenshot was written and is not empty");
  }
  ::PostMessageW(window, WM_CLOSE, 0, 0);
  if (!WaitForExit(&process, 15000, &exit_code, &why)) {
    std::cout << "FAIL: " << ToUtf8(why) << "\n";
    CloseProcess(&process);
    return 14;
  }
  Check(exit_code == 0, "the second process exits with code 0");
  ::CloseHandle(process.handle);
  process.handle = nullptr;

  // Corrupt settings: the window must still open, show the warning and offer defaults.
  // Use a separate user root so the saved evidence above stays intact.
  const std::filesystem::path corrupt_root =
      user_root.parent_path() /
      (user_root.filename().wstring() + L"-corrupt");
  std::error_code corrupt_ec;
  std::filesystem::create_directories(corrupt_root, corrupt_ec);
  {
    std::ofstream garbage(corrupt_root / L"settings.ini",
                          std::ios::binary | std::ios::trunc);
    garbage << "this is not an ini\n";
  }
  if (!Launch(exe, corrupt_root, &process, &why)) {
    std::cout << "FAIL: corrupt-config launch failed: " << ToUtf8(why) << "\n";
    return 14;
  }
  window = WaitForWindow(process, &why);
  if (window == nullptr) {
    std::cout << "FAIL: " << ToUtf8(why) << "\n";
    CloseProcess(&process);
    return 14;
  }
  const std::wstring status_text = ControlText(::GetDlgItem(window, ui::kStatusEdit));
  const std::wstring save_status = ControlText(::GetDlgItem(window, ui::kSaveStatus));
  Check(status_text.find(L"corrupt") != std::wstring::npos ||
            save_status.find(L"corrupt") != std::wstring::npos,
        "corrupt settings are reported in the window");
  Check(::SendMessageW(::GetDlgItem(window, ui::kCompletionEnabled), BM_GETCHECK, 0, 0) ==
            BST_CHECKED,
        "corrupt settings fall back to defaults in the controls");
  ::PostMessageW(window, WM_CLOSE, 0, 0);
  if (!WaitForExit(&process, 15000, &exit_code, &why)) {
    std::cout << "FAIL: " << ToUtf8(why) << "\n";
    CloseProcess(&process);
    return 14;
  }
  Check(exit_code == 0, "the corrupt-config process exits with code 0");
  ::CloseHandle(process.handle);

  if (g_failures != 0) {
    std::cout << "FAILED: " << g_failures << " check(s)\n";
    return 14;
  }
  std::cout << "RESULT PASS\n";
  return 0;
}
