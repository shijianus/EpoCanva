#include "settings_window.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shellscalingapi.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "ui_ids.h"

namespace epocanva {
namespace product {
namespace {

constexpr int kClientWidth = 900;
constexpr int kClientHeight = 664;

UINT WindowDpi(HWND window) {
  const UINT dpi = ::GetDpiForWindow(window);
  return dpi == 0 ? 96 : dpi;
}

int ScaleFor(UINT dpi, int value) { return ::MulDiv(value, dpi, 96); }

UINT StartupDpi() {
  HMONITOR monitor = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  UINT dpi_x = 96;
  UINT dpi_y = 96;
  if (monitor != nullptr &&
      SUCCEEDED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) {
    return dpi_x == 0 ? 96 : dpi_x;
  }
  const UINT system = ::GetDpiForSystem();
  return system == 0 ? 96 : system;
}

HFONT CreateUiFont(UINT dpi, int points, int weight) {
  LOGFONTW font = {};
  font.lfHeight = -::MulDiv(points, static_cast<int>(dpi), 72);
  font.lfWeight = weight;
  font.lfCharSet = DEFAULT_CHARSET;
  font.lfQuality = CLEARTYPE_QUALITY;
  font.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
  wcscpy_s(font.lfFaceName, L"Segoe UI");
  return ::CreateFontIndirectW(&font);
}

std::wstring AboutContent() {
  std::wstring text = L"EpoCanva English Alpha\nVersion ";
  text += ProductVersionLine();
  text += L"\n\n";
  text += L"Project licence: undecided. This is an internal alpha build; it is not "
          L"cleared for public redistribution.\n\n";
  text += L"Engine: librime 1.17.0 (BSD-3-Clause), yaml-cpp (MIT), leveldb "
          L"(BSD-3-Clause), marisa-trie (BSD-2-Clause), OpenCC (Apache-2.0), "
          L"Boost 1.89.0 (BSL-1.0), utfcpp (MIT), darts-clone (BSD-3-Clause).\n\n";
  text += L"Word list: Alt12Dicts 2of12full (public domain; acknowledgment requested), "
          L"converted from the pinned source recorded in the lexicon provenance file. "
          L"Redistribution beyond this internal alpha needs owner and legal sign-off.";
  return text;
}

}  // namespace

SettingsWindow::SettingsWindow() = default;

SettingsWindow::~SettingsWindow() {
  if (body_font_ != nullptr) {
    ::DeleteObject(body_font_);
  }
  if (bold_font_ != nullptr) {
    ::DeleteObject(bold_font_);
  }
  if (title_font_ != nullptr) {
    ::DeleteObject(title_font_);
  }
}

bool SettingsWindow::RegisterClass(HINSTANCE instance) {
  WNDCLASSEXW cls = {};
  cls.cbSize = sizeof(cls);
  cls.style = CS_HREDRAW | CS_VREDRAW;
  cls.lpfnWndProc = &SettingsWindow::WindowProc;
  cls.cbWndExtra = sizeof(void*);
  cls.hInstance = instance;
  cls.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
  cls.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  cls.hbrBackground = ::GetSysColorBrush(COLOR_WINDOW);
  cls.lpszClassName = ui::kSettingsWindowClass;
  if (::RegisterClassExW(&cls) != 0) {
    return true;
  }
  return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool SettingsWindow::Create(HINSTANCE instance, int show_command, std::wstring* why) {
  instance_ = instance;
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
                      WS_CLIPCHILDREN;
  const DWORD ex_style = WS_EX_CONTROLPARENT;
  // GetDpiForSystem is not the monitor DPI on a per-monitor-scaled desktop, and the
  // initial client is sized before any window exists. Use the monitor's effective DPI.
  const UINT startup_dpi = StartupDpi();
  RECT rect = {0, 0, ScaleFor(startup_dpi, kClientWidth),
               ScaleFor(startup_dpi, kClientHeight)};
  ::AdjustWindowRectExForDpi(&rect, style, FALSE, ex_style, startup_dpi);

  hwnd_ = ::CreateWindowExW(ex_style, ui::kSettingsWindowClass,
                            ui::kSettingsWindowTitle, style, CW_USEDEFAULT,
                            CW_USEDEFAULT, rect.right - rect.left,
                            rect.bottom - rect.top, nullptr, nullptr, instance, this);
  if (hwnd_ == nullptr) {
    if (why != nullptr) {
      *why = L"CreateWindowExW failed with error " +
             std::to_wstring(::GetLastError());
    }
    return false;
  }

  ::ShowWindow(hwnd_, show_command);
  ::UpdateWindow(hwnd_);
  // The monitor DPI is authoritative once the window has been shown. Fit the client to
  // the 900x664 logical design at that DPI, then centre the resulting outer window.
  FitClientToDpi();

  MONITORINFO monitor = {};
  monitor.cbSize = sizeof(monitor);
  if (::GetMonitorInfoW(::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY),
                        &monitor)) {
    RECT window_rect = {};
    ::GetWindowRect(hwnd_, &window_rect);
    const int width = window_rect.right - window_rect.left;
    const int height = window_rect.bottom - window_rect.top;
    const int x = monitor.rcWork.left +
                  ((monitor.rcWork.right - monitor.rcWork.left) - width) / 2;
    const int y = monitor.rcWork.top +
                  ((monitor.rcWork.bottom - monitor.rcWork.top) - height) / 2;
    ::SetWindowPos(hwnd_, nullptr, x, y, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  return true;
}

void SettingsWindow::FitClientToDpi() {
  const UINT dpi = WindowDpi(hwnd_);
  if (dpi != dpi_) {
    dpi_ = dpi;
    ApplyFonts();
  }
  const DWORD style = static_cast<DWORD>(
      ::GetWindowLongPtrW(hwnd_, GWL_STYLE));
  const DWORD ex_style = static_cast<DWORD>(
      ::GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
  RECT rect = {0, 0, ScaleFor(dpi_, kClientWidth), ScaleFor(dpi_, kClientHeight)};
  ::AdjustWindowRectExForDpi(&rect, style, FALSE, ex_style, dpi_);
  ::SetWindowPos(hwnd_, nullptr, 0, 0, rect.right - rect.left,
                 rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  Layout();
}

HWND SettingsWindow::CreateControl(const wchar_t* class_name, const wchar_t* text,
                                   DWORD style, int id, int x, int y, int width,
                                   int height) {
  const HWND control = ::CreateWindowExW(
      0, class_name, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
      ScaleFor(dpi_, x), ScaleFor(dpi_, y), ScaleFor(dpi_, width),
      ScaleFor(dpi_, height), hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
  if (control != nullptr) {
    placements_.push_back(Placement{control, x, y, width, height,
                                    (style & 0x0000000FL) == BS_GROUPBOX});
  }
  return control;
}

bool SettingsWindow::OnCreate() {
  dpi_ = WindowDpi(hwnd_);
  const int margin = 20;
  const int left_width = 420;
  const int right_x = 460;
  const int right_width = 420;

  title_ = CreateControl(L"STATIC", L"EpoCanva", SS_LEFT, 0, margin, 14, 600, 40);
  subtitle_ = CreateControl(L"STATIC",
                            L"English input for Windows 11 x64 - internal alpha",
                            SS_LEFT, 0, margin, 56, 860, 20);
  version_ = CreateControl(L"STATIC", L"", SS_LEFT, 0, margin, 78, 860, 20);

  CreateControl(L"BUTTON", L"Status", BS_GROUPBOX, 0, right_x, 108, right_width, 300);
  summary_ = CreateControl(L"STATIC", L"", SS_LEFT, ui::kSummary, right_x + 14, 132,
                           right_width - 28, 22);
  status_edit_ = CreateControl(
      L"EDIT", L"",
      ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER |
          WS_TABSTOP,
      ui::kStatusEdit, right_x + 14, 158, right_width - 28, 236);

  CreateControl(L"BUTTON", L"English completion", BS_GROUPBOX, 0, margin, 108,
                left_width, 110);
  completion_enabled_ = CreateControl(
      L"BUTTON", L"Offer prefix &completions", BS_AUTOCHECKBOX | WS_TABSTOP,
      ui::kCompletionEnabled, margin + 14, 136, left_width - 28, 22);
  CreateControl(L"STATIC", L"Completions per query:", SS_LEFT, 0, margin + 14, 168,
                160, 20);
  completion_count_ = CreateControl(L"COMBOBOX", L"",
                                    CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                    ui::kCompletionCount, margin + 184, 166, 70, 200);

  CreateControl(L"BUTTON", L"Spelling suggestions", BS_GROUPBOX, 0, margin, 228,
                left_width, 110);
  spelling_enabled_ = CreateControl(
      L"BUTTON", L"Offer one-edit &spelling suggestions",
      BS_AUTOCHECKBOX | WS_TABSTOP, ui::kSpellingEnabled, margin + 14, 256,
      left_width - 28, 22);
  CreateControl(L"STATIC", L"Suggestions per query:", SS_LEFT, 0, margin + 14, 288,
                160, 20);
  spelling_count_ = CreateControl(L"COMBOBOX", L"",
                                  CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                  ui::kSpellingCount, margin + 184, 286, 70, 200);

  CreateControl(L"BUTTON", L"Candidate display", BS_GROUPBOX, 0, margin, 348,
                left_width, 90);
  CreateControl(L"STATIC",
                L"Candidate window: not available in this build (planned for W2-B).\r\n"
                L"Page size: 8 candidates, fixed by the deployed schema.\r\n"
                L"No candidate-layout option is exposed yet.",
                SS_LEFT, 0, margin + 14, 372, left_width - 28, 58);

  CreateControl(L"BUTTON", L"Offline and privacy", BS_GROUPBOX, 0, margin, 448,
                left_width, 140);
  CreateControl(L"STATIC",
                L"No network I/O: no telemetry, updates, dictionary sync or cloud "
                L"lookup.\r\n"
                L"Settings, logs and Rime user data stay under the user data folder.\r\n"
                L"No input history or learning. No keystroke, clipboard or field "
                L"logging.",
                SS_LEFT, 0, margin + 14, 472, left_width - 28, 106);

  CreateControl(L"BUTTON", L"Data locations", BS_GROUPBOX, 0, right_x, 418,
                right_width, 170);
  const wchar_t* labels[4] = {L"Install folder:", L"User data:", L"Settings file:",
                              L"Engine data:"};
  HWND* fields[4] = {&path_install_, &path_user_, &path_settings_, &path_engine_};
  const int field_ids[4] = {ui::kPathInstall, ui::kPathUser, ui::kPathSettings,
                            ui::kPathEngine};
  for (int index = 0; index < 4; ++index) {
    const int y = 442 + index * 24;
    CreateControl(L"STATIC", labels[index], SS_LEFT, 0, right_x + 14, y, 80, 20);
    *fields[index] = CreateControl(L"EDIT", L"",
                                   ES_AUTOHSCROLL | ES_READONLY | WS_BORDER |
                                       WS_TABSTOP,
                                   field_ids[index], right_x + 98, y - 2, 308, 22);
  }
  CreateControl(L"BUTTON", L"&Open install", BS_PUSHBUTTON | WS_TABSTOP,
                ui::kOpenInstall, right_x + 14, 542, 180, 28);
  CreateControl(L"BUTTON", L"Open &user data", BS_PUSHBUTTON | WS_TABSTOP,
                ui::kOpenUser, right_x + 204, 542, 180, 28);

  save_status_ = CreateControl(L"STATIC", L"", SS_LEFT, ui::kSaveStatus, margin, 600,
                               860, 20);
  CreateControl(L"BUTTON", L"&About / licences", BS_PUSHBUTTON | WS_TABSTOP,
                ui::kAbout, margin, 624, 180, 30);
  CreateControl(L"BUTTON", L"&Save", BS_DEFPUSHBUTTON | WS_TABSTOP, ui::kSave, 660,
                624, 100, 30);
  CreateControl(L"BUTTON", L"&Close", BS_PUSHBUTTON | WS_TABSTOP, ui::kClose, 770,
                624, 110, 30);

  // Group boxes are siblings of their contents; without this they end up above the
  // controls and paint their background over the checkboxes, edits and status text.
  for (const Placement& placement : placements_) {
    if (placement.group) {
      ::SetWindowPos(placement.hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
  }

  ApplyFonts();
  LoadState();
  RefreshControlsFromSettings();
  RefreshStatusText();
  RefreshPathFields();
  Layout();
  if (status_.settings.status == SettingsLoadStatus::kLoaded) {
    SetSaveStatus(
        L"Settings loaded. Changes take effect at the next engine start.");
  } else {
    std::wstring status_text = L"Built-in defaults are shown (";
    status_text += DescribeSettingsLoadStatus(status_.settings.status);
    status_text += L"). Press Save to write a valid settings file.";
    SetSaveStatus(status_text);
  }

  if (paths_.user_root.empty()) {
    ::EnableWindow(::GetDlgItem(hwnd_, ui::kSave), FALSE);
    ::EnableWindow(::GetDlgItem(hwnd_, ui::kOpenUser), FALSE);
  }
  ::SetWindowTextW(version_,
                   (L"Version " + ProductVersionLine() + L"  |  " + status_.host)
                       .c_str());
  return true;
}

void SettingsWindow::ApplyFonts() {
  HFONT new_body = CreateUiFont(dpi_, 9, FW_NORMAL);
  HFONT new_bold = CreateUiFont(dpi_, 9, FW_SEMIBOLD);
  HFONT new_title = CreateUiFont(dpi_, 20, FW_SEMIBOLD);
  HFONT old_body = body_font_;
  HFONT old_bold = bold_font_;
  HFONT old_title = title_font_;
  body_font_ = new_body;
  bold_font_ = new_bold;
  title_font_ = new_title;

  for (const Placement& placement : placements_) {
    HFONT font = body_font_;
    if (placement.hwnd == title_) {
      font = title_font_;
    } else if (placement.hwnd == summary_) {
      font = bold_font_;
    }
    if (font != nullptr) {
      ::SendMessageW(placement.hwnd, WM_SETFONT,
                     reinterpret_cast<WPARAM>(font), TRUE);
    }
  }
  if (old_body != nullptr) {
    ::DeleteObject(old_body);
  }
  if (old_bold != nullptr) {
    ::DeleteObject(old_bold);
  }
  if (old_title != nullptr) {
    ::DeleteObject(old_title);
  }
}

void SettingsWindow::Layout() {
  for (const Placement& placement : placements_) {
    ::SetWindowPos(placement.hwnd, nullptr, ScaleFor(dpi_, placement.x),
                   ScaleFor(dpi_, placement.y), ScaleFor(dpi_, placement.width),
                   ScaleFor(dpi_, placement.height),
                   SWP_NOZORDER | SWP_NOACTIVATE);
  }
}

void SettingsWindow::LoadState() {
  std::wstring why;
  if (ResolveProductPaths(nullptr, &paths_, &why)) {
    ProductStatus collected;
    std::wstring collect_why;
    if (CollectProductStatusForPaths(paths_, &collected, &collect_why)) {
      status_ = std::move(collected);
    } else {
      status_ = ProductStatus{};
      status_.paths = paths_;
      status_.version = ProductVersion();
      status_.commit = ProductCommit();
      status_.host = L"unavailable";
      status_.settings.status = SettingsLoadStatus::kMissing;
      status_.settings.diagnostic = collect_why.empty() ? why : collect_why;
      status_.items.push_back(StatusItem{
          L"Product status", status_.settings.diagnostic, StatusLevel::kError});
    }
  } else {
    paths_ = ProductPaths{};
    status_ = ProductStatus{};
    status_.version = ProductVersion();
    status_.commit = ProductCommit();
    status_.host = L"unavailable";
    status_.settings.status = SettingsLoadStatus::kMissing;
    status_.settings.diagnostic = why;
    status_.items.push_back(StatusItem{L"Product paths", why, StatusLevel::kError});
  }
  settings_ = status_.settings.settings;
}

void SettingsWindow::RefreshControlsFromSettings() {
  loading_ = true;
  ::SendMessageW(completion_enabled_, BM_SETCHECK,
                 settings_.completion_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
  ::SendMessageW(spelling_enabled_, BM_SETCHECK,
                 settings_.spelling_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
  if (::SendMessageW(completion_count_, CB_GETCOUNT, 0, 0) == 0) {
    for (int count = 1; count <= 7; ++count) {
      const std::wstring label = std::to_wstring(count);
      ::SendMessageW(completion_count_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(label.c_str()));
    }
    for (int count = 1; count <= 3; ++count) {
      const std::wstring label = std::to_wstring(count);
      ::SendMessageW(spelling_count_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(label.c_str()));
    }
  }
  ::SendMessageW(completion_count_, CB_SETCURSEL,
                 static_cast<WPARAM>(settings_.completion_candidates - 1), 0);
  ::SendMessageW(spelling_count_, CB_SETCURSEL,
                 static_cast<WPARAM>(settings_.spelling_candidates - 1), 0);
  ::EnableWindow(completion_count_, settings_.completion_enabled ? TRUE : FALSE);
  ::EnableWindow(spelling_count_, settings_.spelling_enabled ? TRUE : FALSE);
  loading_ = false;
}

void SettingsWindow::RefreshStatusText() {
  if (summary_ != nullptr) {
    ::SetWindowTextW(summary_, PackageSummary(status_).c_str());
  }
  if (status_edit_ != nullptr) {
    const std::wstring lines = FormatStatusLines(status_);
    ::SetWindowTextW(status_edit_, lines.c_str());
    ::SendMessageW(status_edit_, EM_SETSEL, 0, 0);
  }
}

void SettingsWindow::RefreshPathFields() {
  auto set = [](HWND control, const std::filesystem::path& path) {
    if (control != nullptr) {
      ::SetWindowTextW(control, path.empty() ? L"unavailable" : path.c_str());
    }
  };
  set(path_install_, paths_.product_root);
  set(path_user_, paths_.user_root);
  set(path_settings_, paths_.settings_file);
  set(path_engine_, paths_.engine_user_dir);
}

void SettingsWindow::SetSaveStatus(const std::wstring& text) {
  if (save_status_ != nullptr) {
    ::SetWindowTextW(save_status_, text.c_str());
  }
}

void SettingsWindow::SetDirty(bool dirty) {
  if (loading_) {
    return;
  }
  dirty_ = dirty;
  if (dirty) {
    SetSaveStatus(L"Unsaved changes.");
  }
}

bool SettingsWindow::Save() {
  if (paths_.user_root.empty()) {
    ::MessageBoxW(hwnd_, L"The user data path is unavailable; settings cannot be saved.",
                  ui::kSettingsWindowTitle, MB_OK | MB_ICONERROR);
    return false;
  }
  ProductSettings next;
  next.completion_enabled =
      ::SendMessageW(completion_enabled_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  next.spelling_enabled =
      ::SendMessageW(spelling_enabled_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  const LRESULT completion_index =
      ::SendMessageW(completion_count_, CB_GETCURSEL, 0, 0);
  const LRESULT spelling_index =
      ::SendMessageW(spelling_count_, CB_GETCURSEL, 0, 0);
  next.completion_candidates =
      completion_index == CB_ERR ? settings_.completion_candidates
                                 : static_cast<int>(completion_index) + 1;
  next.spelling_candidates =
      spelling_index == CB_ERR ? settings_.spelling_candidates
                               : static_cast<int>(spelling_index) + 1;
  if (!next.valid()) {
    ::MessageBoxW(hwnd_, L"The selected values are outside the supported range.",
                  ui::kSettingsWindowTitle, MB_OK | MB_ICONERROR);
    return false;
  }
  std::string digest;
  std::wstring why;
  if (!SaveSettingsBundle(paths_, next, &digest, &why)) {
    const std::wstring message = L"Could not save settings:\n" + why;
    ::MessageBoxW(hwnd_, message.c_str(), ui::kSettingsWindowTitle,
                  MB_OK | MB_ICONERROR);
    return false;
  }
  settings_ = next;
  dirty_ = false;
  ProductStatus refreshed;
  std::wstring refresh_why;
  if (CollectProductStatusForPaths(paths_, &refreshed, &refresh_why)) {
    status_ = std::move(refreshed);
    RefreshStatusText();
    RefreshPathFields();
  }
  SetSaveStatus(L"Saved. Deployment is requested for the next engine start.");
  return true;
}

void SettingsWindow::TryClose() {
  if (dirty_) {
    const int answer = ::MessageBoxW(
        hwnd_, L"Save changes before closing?", ui::kSettingsWindowTitle,
        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) {
      return;
    }
    if (answer == IDYES && !Save()) {
      return;
    }
  }
  ::DestroyWindow(hwnd_);
}

void SettingsWindow::OnCommand(int id, int notification, HWND control) {
  (void)control;
  switch (id) {
    case ui::kCompletionEnabled:
      if (notification == BN_CLICKED) {
        const bool enabled =
            ::SendMessageW(completion_enabled_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        ::EnableWindow(completion_count_, enabled ? TRUE : FALSE);
        SetDirty(true);
      }
      return;
    case ui::kSpellingEnabled:
      if (notification == BN_CLICKED) {
        const bool enabled =
            ::SendMessageW(spelling_enabled_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        ::EnableWindow(spelling_count_, enabled ? TRUE : FALSE);
        SetDirty(true);
      }
      return;
    case ui::kCompletionCount:
    case ui::kSpellingCount:
      if (notification == CBN_SELCHANGE) {
        SetDirty(true);
      }
      return;
    case ui::kSave:
      Save();
      return;
    case ui::kClose:
      TryClose();
      return;
    case ui::kAbout:
      ShowAbout();
      return;
    case ui::kOpenInstall:
      OpenFolder(paths_.product_root, L"install folder");
      return;
    case ui::kOpenUser: {
      std::wstring why;
      if (!EnsureUserDirectories(paths_, &why)) {
        ::MessageBoxW(hwnd_, why.c_str(), ui::kSettingsWindowTitle,
                      MB_OK | MB_ICONERROR);
        return;
      }
      OpenFolder(paths_.user_root, L"user data folder");
      return;
    }
    default:
      return;
  }
}

void SettingsWindow::ShowAbout() {
  const std::wstring content = AboutContent();
  TASKDIALOG_BUTTON custom_buttons[] = {{100, L"Open licences folder"}};
  TASKDIALOGCONFIG config = {};
  config.cbSize = sizeof(config);
  config.hwndParent = hwnd_;
  config.hInstance = instance_;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
  config.pszWindowTitle = L"About EpoCanva";
  config.pszMainIcon = TD_INFORMATION_ICON;
  config.pszMainInstruction = L"EpoCanva English Alpha";
  config.pszContent = content.c_str();
  config.cButtons = 1;
  config.pButtons = custom_buttons;
  config.nDefaultButton = IDOK;
  config.dwCommonButtons = TDCBF_OK_BUTTON;
  int pressed = 0;
  const HRESULT result = ::TaskDialogIndirect(&config, nullptr, &pressed, nullptr);
  if (FAILED(result)) {
    ::MessageBoxW(hwnd_, content.c_str(), L"About EpoCanva",
                  MB_OK | MB_ICONINFORMATION);
    if (::MessageBoxW(hwnd_, L"Open the licences folder now?", L"About EpoCanva",
                      MB_YESNO | MB_ICONQUESTION) == IDYES) {
      OpenFolder(paths_.licenses_dir, L"licences folder");
    }
    return;
  }
  if (pressed == 100) {
    OpenFolder(paths_.licenses_dir, L"licences folder");
  }
}

void SettingsWindow::OpenFolder(const std::filesystem::path& path,
                                const wchar_t* what) {
  std::error_code ec;
  if (path.empty() || !std::filesystem::is_directory(path, ec) || ec) {
    const std::wstring message =
        std::wstring(L"The ") + what + L" is not available:\n" + path.wstring();
    ::MessageBoxW(hwnd_, message.c_str(), ui::kSettingsWindowTitle,
                  MB_OK | MB_ICONERROR);
    return;
  }
  const HINSTANCE result = ::ShellExecuteW(hwnd_, L"open", path.c_str(), nullptr,
                                           nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(result) <= 32) {
    const std::wstring message =
        std::wstring(L"Windows could not open the ") + what + L":\n" + path.wstring();
    ::MessageBoxW(hwnd_, message.c_str(), ui::kSettingsWindowTitle,
                  MB_OK | MB_ICONERROR);
  }
}

HBRUSH SettingsWindow::OnCtlColor(HDC dc, HWND control, UINT message) {
  (void)control;
  (void)message;
  // The window uses the system window colour as its single surface. Group boxes and
  // themed buttons paint their own background from the same surface, so labels, check
  // boxes and edits do not produce contrasting strips.
  ::SetBkColor(dc, ::GetSysColor(COLOR_WINDOW));
  ::SetTextColor(dc, ::GetSysColor(COLOR_WINDOWTEXT));
  return ::GetSysColorBrush(COLOR_WINDOW);
}

void SettingsWindow::OnDestroy() {
  if (body_font_ != nullptr) {
    ::DeleteObject(body_font_);
    body_font_ = nullptr;
  }
  if (bold_font_ != nullptr) {
    ::DeleteObject(bold_font_);
    bold_font_ = nullptr;
  }
  if (title_font_ != nullptr) {
    ::DeleteObject(title_font_);
    title_font_ = nullptr;
  }
  hwnd_ = nullptr;
  ::PostQuitMessage(0);
}

LRESULT CALLBACK SettingsWindow::WindowProc(HWND window, UINT message,
                                            WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    auto* self = static_cast<SettingsWindow*>(create->lpCreateParams);
    ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    if (self != nullptr) {
      self->hwnd_ = window;
    }
  }
  auto* self = reinterpret_cast<SettingsWindow*>(
      ::GetWindowLongPtrW(window, GWLP_USERDATA));
  if (self == nullptr) {
    return ::DefWindowProcW(window, message, wparam, lparam);
  }
  return self->HandleMessage(window, message, wparam, lparam);
}

LRESULT SettingsWindow::HandleMessage(HWND window, UINT message, WPARAM wparam,
                                      LPARAM lparam) {
  switch (message) {
    case WM_CREATE:
      return OnCreate() ? 0 : -1;
    case WM_DESTROY:
      OnDestroy();
      return 0;
    case WM_CLOSE:
      TryClose();
      return 0;
    case WM_SIZE:
      Layout();
      return 0;
    case WM_COMMAND:
      OnCommand(LOWORD(wparam), HIWORD(wparam), reinterpret_cast<HWND>(lparam));
      return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
      return reinterpret_cast<LRESULT>(
          OnCtlColor(reinterpret_cast<HDC>(wparam),
                     reinterpret_cast<HWND>(lparam), message));
    case WM_ERASEBKGND: {
      // WS_CLIPCHILDREN means the default erase leaves the area behind transparent
      // statics and group boxes unpainted (white). Fill the whole client instead.
      RECT client = {};
      ::GetClientRect(window, &client);
      ::FillRect(reinterpret_cast<HDC>(wparam), &client,
                 ::GetSysColorBrush(COLOR_WINDOW));
      return 1;
    }
    case WM_DPICHANGED: {
      dpi_ = HIWORD(wparam);
      ApplyFonts();
      FitClientToDpi();
      const auto* suggested = reinterpret_cast<const RECT*>(lparam);
      ::SetWindowPos(window, nullptr, suggested->left, suggested->top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    default:
      return ::DefWindowProcW(window, message, wparam, lparam);
  }
}

}  // namespace product
}  // namespace epocanva
