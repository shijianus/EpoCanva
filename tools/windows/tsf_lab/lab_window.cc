// EpoCanva P3.11 -- the visible Win32 shell. See lab_window.h.
#include "lab_window.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <utility>

namespace epocanva {
namespace tsf {
namespace lab {

namespace {

constexpr wchar_t kTopWindowClass[] = L"EpoCanvaP311TsfLabWindow";
constexpr wchar_t kDocumentWindowClass[] = L"EpoCanvaP311TsfLabDocument";
constexpr UINT kLabTimerId = 0x31;
constexpr int kFirstDocumentId = 100;
constexpr int kFirstScenarioButtonId = 2001;
constexpr int kRestartButtonId = 2010;
constexpr int kStatusControlId = 2100;
constexpr UINT kPrintWindowRenderFullContent = 0x00000002u;

std::wstring WideAscii(const std::string& text) {
  std::wstring result;
  result.reserve(text.size());
  for (char character : text) {
    result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(character)));
  }
  return result;
}

std::wstring WideUtf16(const std::u16string& text) {
  std::wstring result;
  result.reserve(text.size());
  for (char16_t unit : text) {
    result.push_back(static_cast<wchar_t>(unit));
  }
  return result;
}

std::wstring ShortWide(const std::wstring& text, std::size_t maximum) {
  if (text.size() <= maximum) {
    return text;
  }
  return text.substr(0, maximum) + L"...";
}

std::string HrText(HRESULT hr) {
  char buffer[24] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08lX",
                static_cast<unsigned long>(hr));
  return buffer;
}

void AppendLine(std::wstring* text, const std::wstring& line) {
  *text += line;
  *text += L"\r\n";
}

wchar_t DocumentLetter(int index) { return index == 0 ? L'A' : L'B'; }

std::wstring FocusLetter(int index) {
  if (index < 0 || index > 1) {
    return L"none";
  }
  return std::wstring(1, DocumentLetter(index));
}

}  // namespace

int LabWindow::class_refs_ = 0;
int LabWindow::live_windows_ = 0;

LabWindow::LabWindow(const LabWindowOptions& options) : options_(options) {
  surfaces_[0] = DocumentSurface{this, 0};
  surfaces_[1] = DocumentSurface{this, 1};
}

LabWindow::~LabWindow() {
  Destroy();
  if (ui_font_ != nullptr) {
    ::DeleteObject(ui_font_);
    ui_font_ = nullptr;
  }
  if (mono_font_ != nullptr) {
    ::DeleteObject(mono_font_);
    mono_font_ = nullptr;
  }
  if (counted_live_) {
    --live_windows_;
    counted_live_ = false;
  }
}

std::unique_ptr<LabWindow> LabWindow::Create(const LabWindowOptions& options,
                                             std::string* error) {
  std::string class_error;
  if (!EnsureClasses(&class_error)) {
    if (error != nullptr) {
      *error = class_error;
    }
    return nullptr;
  }
  std::unique_ptr<LabWindow> window(new LabWindow(options));
  window->classes_registered_ = true;

  HINSTANCE instance = ::GetModuleHandleW(nullptr);
  HWND created = ::CreateWindowExW(
      0, kTopWindowClass, L"EpoCanva P3.11 - visible TSF integration lab",
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 60, 40, 1180, 920, nullptr, nullptr,
      instance, window.get());
  // Capture the failure value before any other API call can overwrite it.
  const DWORD create_error = ::GetLastError();
  if (created == nullptr) {
    std::string reason;
    if (!window->create_error_.empty()) {
      reason = window->create_error_;
    } else if (window->create_rejected_) {
      reason = "the window procedure rejected creation";
    } else {
      reason = "CreateWindowExW failed err=" + std::to_string(create_error);
    }
    if (error != nullptr) {
      *error = reason + " (GetLastError=" + std::to_string(create_error) + ")";
    }
    return nullptr;
  }
  window->hwnd_ = created;
  window->counted_live_ = true;
  ++live_windows_;

  if (options.start_session) {
    std::string start_error;
    if (!window->StartSessionInternal(&start_error)) {
      window->startup_error_ = start_error;
    }
  }
  if (options.show) {
    ::ShowWindow(created, SW_SHOWNORMAL);
    ::UpdateWindow(created);
  }
  return window;
}

bool LabWindow::EnsureClasses(std::string* error) {
  if (class_refs_ > 0) {
    ++class_refs_;
    return true;
  }
  HINSTANCE instance = ::GetModuleHandleW(nullptr);

  WNDCLASSEXW top = {};
  top.cbSize = sizeof(top);
  top.style = CS_HREDRAW | CS_VREDRAW;
  top.lpfnWndProc = &LabWindow::WindowProc;
  top.hInstance = instance;
  top.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  top.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
  top.lpszClassName = kTopWindowClass;
  const ATOM top_atom = ::RegisterClassExW(&top);
  const DWORD top_error = ::GetLastError();
  if (top_atom == 0 && top_error != ERROR_CLASS_ALREADY_EXISTS) {
    if (error != nullptr) {
      *error = "RegisterClassExW(top) failed err=" + std::to_string(top_error);
    }
    return false;
  }

  WNDCLASSEXW document = {};
  document.cbSize = sizeof(document);
  document.style = CS_HREDRAW | CS_VREDRAW;
  document.lpfnWndProc = &LabWindow::DocumentProc;
  document.hInstance = instance;
  document.hCursor = ::LoadCursorW(nullptr, IDC_IBEAM);
  document.hbrBackground =
      reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1));
  document.lpszClassName = kDocumentWindowClass;
  const ATOM document_atom = ::RegisterClassExW(&document);
  const DWORD document_error = ::GetLastError();
  if (document_atom == 0 && document_error != ERROR_CLASS_ALREADY_EXISTS) {
    ::UnregisterClassW(kTopWindowClass, instance);
    if (error != nullptr) {
      *error = "RegisterClassExW(document) failed err=" +
               std::to_string(document_error);
    }
    return false;
  }
  class_refs_ = 1;
  return true;
}

void LabWindow::ReleaseClasses() {
  if (class_refs_ <= 0) {
    return;
  }
  --class_refs_;
  if (class_refs_ == 0) {
    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    ::UnregisterClassW(kDocumentWindowClass, instance);
    ::UnregisterClassW(kTopWindowClass, instance);
  }
}

bool LabWindow::ClassesRegistered() { return class_refs_ > 0; }
int LabWindow::LiveWindowCount() { return live_windows_; }

HWND LabWindow::document_window(int index) const {
  if (index < 0 || index > 1) {
    return nullptr;
  }
  return document_windows_[index];
}

bool LabWindow::session_ready() const {
  return session_ != nullptr && session_->ready();
}

bool LabWindow::CreateChildren() {
  HINSTANCE instance = ::GetModuleHandleW(nullptr);
  for (int index = 0; index < 2; ++index) {
    document_windows_[index] = ::CreateWindowExW(
        0, kDocumentWindowClass, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 10, 10,
        hwnd_,
        reinterpret_cast<HMENU>(
            static_cast<UINT_PTR>(kFirstDocumentId + index)),
        instance, &surfaces_[index]);
    const DWORD error = ::GetLastError();
    if (document_windows_[index] == nullptr) {
      create_error_ = "document child creation failed err=" +
                      std::to_string(error);
      return false;
    }
  }

  static const wchar_t* kButtonLabels[10] = {
      L"1. Compose hel", L"2. Return commit", L"3. hello + Space",
      L"4. BackSpace/Esc", L"5. A/B isolation", L"6. Late A edit",
      L"7. Read-only", L"8. External edit", L"9. Uncertain",
      L"Restart session"};
  for (int index = 0; index < 10; ++index) {
    buttons_[index] = ::CreateWindowExW(
        0, L"BUTTON", kButtonLabels[index],
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, hwnd_,
        reinterpret_cast<HMENU>(
            static_cast<UINT_PTR>(kFirstScenarioButtonId + index)),
        instance, nullptr);
    const DWORD error = ::GetLastError();
    if (buttons_[index] == nullptr) {
      create_error_ = "scenario button creation failed err=" +
                      std::to_string(error);
      return false;
    }
  }

  status_window_ = ::CreateWindowExW(
      WS_EX_NOACTIVATE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
          ES_AUTOVSCROLL | ES_NOHIDESEL,
      0, 0, 10, 10, hwnd_,
      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kStatusControlId)), instance,
      nullptr);
  const DWORD status_error = ::GetLastError();
  if (status_window_ == nullptr) {
    create_error_ = "status control creation failed err=" +
                    std::to_string(status_error);
    return false;
  }

  ui_font_ = ::CreateFontW(
      -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  mono_font_ = ::CreateFontW(
      -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      FIXED_PITCH | FF_MODERN, L"Consolas");
  if (ui_font_ != nullptr) {
    for (HWND button : buttons_) {
      ::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_),
                     TRUE);
    }
  }
  const HFONT status_font =
      mono_font_ != nullptr
          ? mono_font_
          : reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
  ::SendMessageW(status_window_, WM_SETFONT,
                 reinterpret_cast<WPARAM>(status_font), TRUE);

  Layout();
  SetButtonsEnabled(true);
  AddStatusLine("lab window created; click document A or B, then a scenario");
  RefreshView();
  return true;
}

void LabWindow::Layout() {
  if (hwnd_ == nullptr) {
    return;
  }
  RECT client = {};
  ::GetClientRect(hwnd_, &client);
  const int width = client.right - client.left;
  const int height = client.bottom - client.top;
  const int margin = 12;
  const int gap = 10;
  const int button_height = 30;
  const int button_rows = 2;
  const int button_area_height = button_rows * button_height + gap;
  int status_height = 430;
  int document_height =
      height - margin * 2 - button_area_height - status_height - gap * 2;
  if (document_height < 170) {
    document_height = 170;
    status_height = std::max(
        120, height - margin * 2 - button_area_height - document_height - gap * 2);
  }
  const int document_width = (width - margin * 3) / 2;
  ::MoveWindow(document_windows_[0], margin, margin, document_width,
               document_height, TRUE);
  ::MoveWindow(document_windows_[1], margin * 2 + document_width, margin,
               document_width, document_height, TRUE);

  const int buttons_y = margin + document_height + gap;
  const int button_width =
      std::max(80, (width - margin * 2 - gap * 4) / 5);
  for (int index = 0; index < 10; ++index) {
    const int row = index / 5;
    const int column = index % 5;
    ::MoveWindow(buttons_[index], margin + column * (button_width + gap),
                 buttons_y + row * (button_height + gap), button_width,
                 button_height, TRUE);
  }
  const int status_y =
      buttons_y + button_area_height + gap;
  const int status_width = std::max(100, width - margin * 2);
  const int actual_status_height =
      std::max(80, height - status_y - margin);
  ::MoveWindow(status_window_, margin, status_y, status_width,
               actual_status_height, TRUE);
}

void LabWindow::OnDocumentFocus(int index) {
  if (session_ != nullptr && session_->ready()) {
    session_->OnDocumentWindowFocused(index);
    AddStatusLine(std::string("Win32 focus -> document ") +
                  (index == 0 ? "A" : "B") + "; TSF SetFocus requested");
  }
  RefreshView();
  if (document_windows_[index] != nullptr) {
    ::InvalidateRect(document_windows_[index], nullptr, FALSE);
  }
}

void LabWindow::OnCommand(int command_id) {
  if (command_id == kRestartButtonId) {
    if (scenario_running_ || (runner_ != nullptr && runner_->busy())) {
      AddStatusLine("restart refused: a scenario is already running");
      RefreshView();
      return;
    }
    std::string error;
    if (!RestartSession(&error)) {
      AddStatusLine("restart failed: " + error);
    }
    RefreshView();
    return;
  }
  const int scenario = command_id - kFirstScenarioButtonId;
  if (scenario >= 0 && scenario < 9) {
    RunScenario(static_cast<ScenarioId>(scenario + 1));
  }
}

void LabWindow::RunScenario(ScenarioId id) {
  if (runner_ == nullptr || !session_ready()) {
    AddStatusLine("scenario refused: the lab session is not ready");
    RefreshView();
    return;
  }
  if (scenario_running_ || runner_->busy()) {
    AddStatusLine("scenario refused: another scenario is running");
    RefreshView();
    return;
  }
  scenario_running_ = true;
  SetButtonsEnabled(false);
  AddStatusLine(std::string("scenario ") + ScenarioRunner::Name(id) +
                " started");
  RefreshView();

  const ScenarioResult result = runner_->Run(id);
  scenario_running_ = false;
  std::string status;
  switch (result.status) {
    case ScenarioResult::Status::kOk:
      status = "complete";
      break;
    case ScenarioResult::Status::kBusy:
      status = "refused (busy)";
      break;
    case ScenarioResult::Status::kRefused:
      status = "refused";
      break;
    case ScenarioResult::Status::kFailed:
      status = "FAILED";
      break;
    case ScenarioResult::Status::kCancelled:
      status = "cancelled";
      break;
  }
  AddStatusLine(std::string("scenario ") + ScenarioRunner::Name(id) + ": " +
                status + " - " + result.detail);
  SetButtonsEnabled(true);
  MaybeRefreshEngineState(true);
  RefreshView();
  if (close_requested_) {
    Destroy();
  }
}

void LabWindow::SetButtonsEnabled(bool enabled) {
  for (HWND button : buttons_) {
    if (button != nullptr) {
      ::EnableWindow(button, enabled ? TRUE : FALSE);
    }
  }
}

void LabWindow::OnTimer() {
  if (!session_ready()) {
    return;
  }
  if (scenario_running_ || (runner_ != nullptr && runner_->busy())) {
    return;
  }
  session_->PumpOnce(0);
  MaybeRefreshEngineState(false);
  RefreshView();
}

void LabWindow::MaybeRefreshEngineState(bool force) {
  if (!session_ready()) {
    return;
  }
  const LabSnapshot snapshot = session_->Snapshot();
  if (!snapshot.bound || snapshot.phase != coordinator::WorkPhase::kIdle) {
    return;
  }
  const ULONGLONG now = ::GetTickCount64();
  if (!force && now - last_refresh_tick_ < 500) {
    return;
  }
  last_refresh_tick_ = now;
  std::string error;
  if (!session_->RefreshEngineState(1500, &error)) {
    if (error != last_engine_error_) {
      last_engine_error_ = error;
      AddStatusLine("engine read-only refresh: " + error);
    }
    return;
  }
  last_engine_error_.clear();
}

void LabWindow::AddStatusLine(const std::string& line) {
  status_lines_.push_back(line);
  while (status_lines_.size() > 8) {
    status_lines_.erase(status_lines_.begin());
  }
}

std::wstring LabWindow::BuildDocumentLine(int index,
                                          const LabSnapshot& snapshot) {
  const LabDocumentSnapshot& document = snapshot.documents[index];
  std::wstring line = L"Document ";
  line.push_back(DocumentLetter(index));
  line += L": ctx=" + std::to_wstring(document.context_id) +
          L" gen=" + std::to_wstring(document.generation) +
          L" len=" + std::to_wstring(document.text_length_utf16) +
          L" utf16 sel=" + std::to_wstring(document.selection_start_utf16) +
          L".." + std::to_wstring(document.selection_end_utf16) +
          L" read_only=" + (document.read_only ? L"1" : L"0") +
          L" mutations=" + std::to_wstring(document.text_mutations);
  const bool current = snapshot.identity.context_id == document.context_id;
  if (current && snapshot.composition_active) {
    line += L" comp=on start=" +
            std::to_wstring(snapshot.composition_start_utf16) +
            L" len=" + std::to_wstring(snapshot.composition_length_utf16);
  } else {
    line += L" comp=off";
  }
  return line;
}

std::wstring LabWindow::BuildEngineLine(const LabSnapshot& snapshot) {
  if (!snapshot.engine_state_valid) {
    std::wstring line = L"Engine: no valid read-only state";
    if (!snapshot.engine_error.empty()) {
      line += L" (" + WideAscii(snapshot.engine_error) + L")";
    }
    return line;
  }
  const engine::EngineState& state = snapshot.engine_state;
  std::wstring line = L"Engine: composing=";
  line += state.is_composing ? L"1" : L"0";
  line += L" preedit=\"" + ShortWide(WideUtf16(state.preedit.utf16_code_units), 48) +
          L"\" (" + std::to_wstring(state.preedit.utf16_code_units.size()) +
          L" utf16) cursor=" +
          WideAscii(engine::DescribeCursorOffsetStatus(state.cursor_status)) +
          L" candidates=" + std::to_wstring(state.candidates.size()) +
          L" highlighted=" +
          std::to_wstring(state.highlighted_candidate_index_on_page);
  const std::size_t shown = std::min<std::size_t>(state.candidates.size(), 6);
  for (std::size_t index = 0; index < shown; ++index) {
    line += L"\r\n  [" + std::to_wstring(index) + L"] " +
            ShortWide(WideUtf16(state.candidates[index].text_utf16_code_units),
                      40);
    if (!state.candidates[index].comment_utf16_code_units.empty()) {
      line += L"  (" +
              ShortWide(
                  WideUtf16(state.candidates[index].comment_utf16_code_units),
                  32) +
              L")";
    }
  }
  if (state.candidates.size() > shown) {
    line += L"\r\n  ...";
  }
  return line;
}

std::wstring LabWindow::BuildStatusText() {
  std::wstring text;
  AppendLine(&text,
             L"EpoCanva P3.11 - visible TSF integration lab (fixed-fixture experiment)");
  AppendLine(&text,
             L"NOT a registered input method. No keyboard sink, no SendInput, no "
             L"write into another application.");
  AppendLine(&text,
             L"Click document A or B for real Win32 focus. Typed keys into this "
             L"window are ignored; use the scenario buttons.");
  AppendLine(&text, L"");
  if (!session_ready()) {
    AppendLine(&text, L"SESSION NOT READY");
    if (!startup_error_.empty()) {
      AppendLine(&text, L"  " + WideAscii(startup_error_));
    } else {
      AppendLine(&text, L"  (no lab session has been started)");
    }
    return text;
  }

  const LabSnapshot snapshot = session_->Snapshot();
  AppendLine(&text, L"Focus: Win32=" + FocusLetter(snapshot.win32_focus) +
                        L"  TSF GetFocus=" +
                        FocusLetter(snapshot.tsf_focus) +
                        L"  TSF SetFocus calls=" +
                        std::to_wstring(snapshot.focus_calls) + L" last=" +
                        WideAscii(HrText(snapshot.last_set_focus_hr)) +
                        L" target=" +
                        FocusLetter(snapshot.last_focus_target));
  AppendLine(&text,
             L"Last bind path=" + WideAscii(snapshot.last_bind_path) +
                 L" (notification = real OnSetFocus; revalidation = Pump observed "
                 L"GetFocus)");
  AppendLine(&text, BuildDocumentLine(0, snapshot));
  AppendLine(&text, BuildDocumentLine(1, snapshot));
  AppendLine(&text, BuildEngineLine(snapshot));
  AppendLine(&text, L"");
  AppendLine(&text,
             L"Coordinator: bound=" + std::wstring(snapshot.bound ? L"1" : L"0") +
                 L" ctx=" + std::to_wstring(snapshot.identity.context_id) +
                 L" gen=" + std::to_wstring(snapshot.identity.generation) +
                 L" permission=" +
                 WideAscii(edit::DescribeInputPermission(snapshot.permission)) +
                 L" gate=" +
                 WideAscii(coordinator::DescribeCoordinatorGate(snapshot.gate)) +
                 L" phase=" +
                 WideAscii(coordinator::DescribeWorkPhase(snapshot.phase)));
  AppendLine(&text,
             L"  pending: " +
                 (snapshot.phase == coordinator::WorkPhase::kIdle
                      ? std::wstring(L"none")
                      : std::wstring(L"work=") +
                            std::to_wstring(snapshot.work_id) +
                            L" kind=" +
                            WideAscii(coordinator::DescribeWorkKind(
                                snapshot.work_kind)) +
                            L" engine_req=" +
                            std::to_wstring(snapshot.engine_request.value)));
  AppendLine(&text,
             L"  last terminal: work=" + std::to_wstring(snapshot.work_id) +
                 L" outcome=" +
                 WideAscii(coordinator::DescribeWorkOutcome(snapshot.outcome)) +
                 L" cause=" +
                 WideAscii(coordinator::DescribeRejectCause(snapshot.cause)) +
                 L" edit=" +
                 WideAscii(edit::DescribeEditStatus(snapshot.last_edit_status)) +
                 L"/" +
                 WideAscii(edit::DescribeEditRejectReason(
                     snapshot.last_edit_reason)));
  AppendLine(&text,
             L"  counters: works=" + std::to_wstring(snapshot.works_accepted) +
                 L" results=" + std::to_wstring(snapshot.engine_results_taken) +
                 L" plans=" + std::to_wstring(snapshot.plans_submitted) +
                 L" applied=" + std::to_wstring(snapshot.edits_applied) +
                 L" refused=" + std::to_wstring(snapshot.edits_refused) +
                 L" stale=" + std::to_wstring(snapshot.edits_stale) +
                 L" pqueries=" +
                 std::to_wstring(snapshot.permission_queries_begun) + L"/" +
                 std::to_wstring(snapshot.permission_queries_completed) +
                 L" rechecks=" +
                 std::to_wstring(snapshot.permission_rechecks));
  AppendLine(&text,
             L"Observer: OnSetFocus=" +
                 std::to_wstring(snapshot.observer.focus_notifications) +
                 L" focus_loss=" +
                 std::to_wstring(snapshot.observer.focus_loss_notifications) +
                 L" push=" + std::to_wstring(snapshot.observer.push_notifications) +
                 L" pop=" + std::to_wstring(snapshot.observer.pop_notifications) +
                 L" binds=" + std::to_wstring(snapshot.observer.target_binds) +
                 L" drops=" + std::to_wstring(snapshot.observer.target_drops) +
                 L" raw_external_edits=" +
                 std::to_wstring(snapshot.observer.external_edits) +
                 L" last_loss=" +
                 WideAscii(context::DescribeContextDropReason(
                     snapshot.observer.last_target_lost_reason)));
  AppendLine(&text,
             L"Binding: binds_ok=" + std::to_wstring(snapshot.binding.binds_ok) +
                 L" refused=" + std::to_wstring(snapshot.binding.binds_refused) +
                 L" blocked_uncertain=" +
                 std::to_wstring(snapshot.binding.binds_blocked_uncertain) +
                 L" losses=" + std::to_wstring(snapshot.binding.target_losses) +
                 L" invalidations=" +
                 std::to_wstring(snapshot.binding.invalidations) +
                 L" engine_clients=" +
                 std::to_wstring(snapshot.engine_clients_created) +
                 L" last_bind=" +
                 WideAscii(coordinator::DescribeBindStatus(
                     snapshot.binding.last_bind_status)));
  AppendLine(&text, L"");
  AppendLine(&text, L"Recent operations:");
  if (status_lines_.empty()) {
    AppendLine(&text, L"  (none yet)");
  } else {
    // Newest first: the latest scenario step stays visible in the status viewport.
    for (auto line = status_lines_.rbegin(); line != status_lines_.rend(); ++line) {
      AppendLine(&text, L"  " + WideAscii(*line));
    }
  }
  if (scenario_running_) {
    AppendLine(&text, L"SCENARIO RUNNING - buttons disabled");
  }
  if (close_requested_) {
    AppendLine(&text, L"Close requested - finishing the current step");
  }
  return text;
}

void LabWindow::RefreshView() {
  if (status_window_ == nullptr) {
    return;
  }
  std::wstring text = BuildStatusText();
  if (text != last_status_text_) {
    last_status_text_ = text;
    ::SetWindowTextW(status_window_, text.c_str());
  }
  for (HWND document : document_windows_) {
    if (document != nullptr) {
      ::InvalidateRect(document, nullptr, FALSE);
    }
  }
}

void LabWindow::PaintDocument(const DocumentSurface& surface) {
  HWND window = document_windows_[surface.index];
  if (window == nullptr) {
    return;
  }
  PAINTSTRUCT paint = {};
  HDC dc = ::BeginPaint(window, &paint);
  RECT client = {};
  ::GetClientRect(window, &client);
  ::FillRect(dc, &client, ::GetSysColorBrush(COLOR_WINDOW));

  LabSnapshot snapshot;
  if (session_ready()) {
    snapshot = session_->Snapshot();
  }
  const bool win32_focus = ::GetFocus() == window;
  const bool tsf_focus = snapshot.tsf_focus == surface.index;
  const bool read_only = snapshot.documents[surface.index].read_only;
  const bool current =
      snapshot.identity.context_id ==
      snapshot.documents[surface.index].context_id;
  const bool composition = current && snapshot.composition_active;

  const COLORREF border_color =
      (win32_focus || tsf_focus) ? ::GetSysColor(COLOR_HIGHLIGHT)
                                 : ::GetSysColor(COLOR_3DSHADOW);
  HPEN pen = ::CreatePen(PS_SOLID, (win32_focus || tsf_focus) ? 2 : 1,
                         border_color);
  HGDIOBJ old_pen = ::SelectObject(dc, pen);
  HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
  ::Rectangle(dc, 1, 1, client.right - 1, client.bottom - 1);
  ::SelectObject(dc, old_brush);
  ::SelectObject(dc, old_pen);
  ::DeleteObject(pen);

  ::SetBkMode(dc, TRANSPARENT);
  ::SetTextColor(dc, ::GetSysColor(COLOR_WINDOWTEXT));
  const HFONT ui_font =
      ui_font_ != nullptr
          ? ui_font_
          : reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
  const HFONT mono_font =
      mono_font_ != nullptr
          ? mono_font_
          : reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
  HGDIOBJ old_font = ::SelectObject(dc, ui_font);
  std::wstring title = L"Document ";
  title.push_back(DocumentLetter(surface.index));
  if (read_only) {
    title += L"  [READ-ONLY metadata]";
  }
  if (win32_focus) {
    title += L"  [Win32 focus]";
  }
  if (tsf_focus) {
    title += L"  [TSF target]";
  }
  RECT title_rect = {12, 10, client.right - 12, 34};
  ::DrawTextW(dc, title.c_str(), static_cast<int>(title.size()), &title_rect,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

  ::SelectObject(dc, mono_font);
  const std::wstring& document_text =
      snapshot.documents[surface.index].text_utf16;
  RECT text_rect = {12, 48, client.right - 12, client.bottom - 34};
  if (document_text.empty()) {
    ::SetTextColor(dc, ::GetSysColor(COLOR_GRAYTEXT));
    ::DrawTextW(dc, L"(empty fixture document)", -1, &text_rect,
                DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
  } else {
    ::DrawTextW(dc, document_text.c_str(),
                static_cast<int>(document_text.size()), &text_rect,
                DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX |
                    DT_END_ELLIPSIS);
  }

  ::SelectObject(dc, ui_font);
  ::SetTextColor(dc, ::GetSysColor(COLOR_GRAYTEXT));
  std::wstring footer =
      L"len=" +
      std::to_wstring(snapshot.documents[surface.index].text_length_utf16) +
      L" utf16  sel=" +
      std::to_wstring(
          snapshot.documents[surface.index].selection_start_utf16) +
      L".." +
      std::to_wstring(snapshot.documents[surface.index].selection_end_utf16) +
      L"  comp=" + (composition ? L"on" : L"off");
  if (composition) {
    footer += L" start=" + std::to_wstring(snapshot.composition_start_utf16) +
              L" len=" + std::to_wstring(snapshot.composition_length_utf16);
  }
  RECT footer_rect = {12, client.bottom - 30, client.right - 12,
                      client.bottom - 8};
  ::DrawTextW(dc, footer.c_str(), static_cast<int>(footer.size()), &footer_rect,
              DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);

  ::SelectObject(dc, old_font);
  ::EndPaint(window, &paint);
}

void LabWindow::Destroy() {
  close_requested_ = true;
  ShutdownSession();
  if (hwnd_ != nullptr) {
    HWND window = hwnd_;
    hwnd_ = nullptr;
    ::DestroyWindow(window);
  }
  if (classes_registered_) {
    ReleaseClasses();
    classes_registered_ = false;
  }
}

void LabWindow::ShutdownSession() {
  runner_.reset();
  if (session_ != nullptr) {
    session_->Shutdown();
    session_.reset();
  }
  last_engine_error_.clear();
  last_refresh_tick_ = 0;
}

bool LabWindow::StartSessionInternal(std::string* error) {
  if (session_ != nullptr) {
    if (error != nullptr) {
      *error = "a lab session is already started";
    }
    return false;
  }
  if (hwnd_ == nullptr || document_windows_[0] == nullptr ||
      document_windows_[1] == nullptr) {
    if (error != nullptr) {
      *error = "the document windows do not exist";
    }
    startup_error_ = error != nullptr ? *error : "";
    return false;
  }
  auto session = std::make_unique<LabSession>(options_.config);
  session->SetUiTick([this] { RefreshView(); });
  session->SetCancelCheck([this] { return close_requested_; });
  const std::array<HWND, 2> windows = {document_windows_[0],
                                       document_windows_[1]};
  std::string start_error;
  if (!session->Start(windows, &start_error)) {
    startup_error_ = start_error;
    if (error != nullptr) {
      *error = start_error;
    }
    return false;
  }
  session_ = std::move(session);
  runner_ =
      std::make_unique<ScenarioRunner>(*session_, options_.use_window_focus);
  runner_->SetProgress([this](const std::string& line) { AddStatusLine(line); });
  runner_->SetCancel([this] { return close_requested_; });
  startup_error_.clear();
  AddStatusLine("lab session started: engine and real TSF documents ready");
  RefreshView();
  return true;
}

bool LabWindow::RestartSession(std::string* error) {
  if (scenario_running_ || (runner_ != nullptr && runner_->busy())) {
    if (error != nullptr) {
      *error = "a scenario is already running";
    }
    return false;
  }
  ShutdownSession();
  close_requested_ = false;
  status_lines_.clear();
  AddStatusLine("restarting the lab session");
  std::string start_error;
  if (!StartSessionInternal(&start_error)) {
    startup_error_ = start_error;
    if (error != nullptr) {
      *error = start_error;
    }
    AddStatusLine("restart failed: " + start_error);
    RefreshView();
    return false;
  }
  RefreshView();
  return true;
}

void LabWindow::RunMessageLoop() {
  if (hwnd_ == nullptr) {
    return;
  }
  in_message_loop_ = true;
  ::SetTimer(hwnd_, kLabTimerId, 40, nullptr);
  MSG message = {};
  int result = 0;
  while ((result = ::GetMessageW(&message, nullptr, 0, 0)) > 0) {
    ::TranslateMessage(&message);
    ::DispatchMessageW(&message);
  }
  if (hwnd_ != nullptr) {
    ::KillTimer(hwnd_, kLabTimerId);
  }
  in_message_loop_ = false;
}

bool LabWindow::CaptureToBmp(const std::filesystem::path& path,
                             std::string* error) {
  if (hwnd_ == nullptr) {
    if (error != nullptr) {
      *error = "the window does not exist";
    }
    return false;
  }
  ::RedrawWindow(hwnd_, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
  RECT window_rect = {};
  if (!::GetWindowRect(hwnd_, &window_rect)) {
    const DWORD get_rect_error = ::GetLastError();
    if (error != nullptr) {
      *error = "GetWindowRect failed err=" + std::to_string(get_rect_error);
    }
    return false;
  }
  const int width = window_rect.right - window_rect.left;
  const int height = window_rect.bottom - window_rect.top;
  if (width <= 0 || height <= 0) {
    if (error != nullptr) {
      *error = "the window has no drawable area";
    }
    return false;
  }
  HDC screen = ::GetDC(nullptr);
  if (screen == nullptr) {
    if (error != nullptr) {
      *error = "GetDC failed";
    }
    return false;
  }
  HDC memory = ::CreateCompatibleDC(screen);
  HBITMAP bitmap = ::CreateCompatibleBitmap(screen, width, height);
  if (memory == nullptr || bitmap == nullptr) {
    if (bitmap != nullptr) {
      ::DeleteObject(bitmap);
    }
    if (memory != nullptr) {
      ::DeleteDC(memory);
    }
    ::ReleaseDC(nullptr, screen);
    if (error != nullptr) {
      *error = "bitmap allocation failed";
    }
    return false;
  }
  HGDIOBJ old_bitmap = ::SelectObject(memory, bitmap);
  const BOOL printed = ::PrintWindow(hwnd_, memory, kPrintWindowRenderFullContent);
  const DWORD print_error = ::GetLastError();
  if (!printed) {
    ::SelectObject(memory, old_bitmap);
    ::DeleteObject(bitmap);
    ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
    if (error != nullptr) {
      *error = "PrintWindow failed err=" + std::to_string(print_error);
    }
    return false;
  }

  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  std::vector<unsigned char> pixels(static_cast<std::size_t>(width) *
                                    static_cast<std::size_t>(height) * 4u);
  const int lines = ::GetDIBits(memory, bitmap, 0, static_cast<UINT>(height),
                                pixels.data(), &info, DIB_RGB_COLORS);
  const DWORD dib_error = ::GetLastError();
  ::SelectObject(memory, old_bitmap);
  ::DeleteObject(bitmap);
  ::DeleteDC(memory);
  ::ReleaseDC(nullptr, screen);
  if (lines != height) {
    if (error != nullptr) {
      *error = "GetDIBits returned " + std::to_string(lines) + " of " +
               std::to_string(height) + " lines err=" +
               std::to_string(dib_error);
    }
    return false;
  }

  BITMAPFILEHEADER file_header = {};
  file_header.bfType = 0x4D42;
  file_header.bfOffBits =
      sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
  file_header.bfSize = file_header.bfOffBits +
                       static_cast<DWORD>(pixels.size());
  BITMAPINFOHEADER info_header = info.bmiHeader;
  info_header.biSizeImage = static_cast<DWORD>(pixels.size());
  std::error_code directory_error;
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path(), directory_error);
    if (directory_error) {
      if (error != nullptr) {
        *error = "cannot create the screenshot directory: " +
                 directory_error.message();
      }
      return false;
    }
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    if (error != nullptr) {
      *error = "cannot open the screenshot file";
    }
    return false;
  }
  file.write(reinterpret_cast<const char*>(&file_header), sizeof(file_header));
  file.write(reinterpret_cast<const char*>(&info_header), sizeof(info_header));
  file.write(reinterpret_cast<const char*>(pixels.data()),
             static_cast<std::streamsize>(pixels.size()));
  if (!file) {
    if (error != nullptr) {
      *error = "writing the screenshot failed";
    }
    return false;
  }
  return true;
}

LRESULT CALLBACK LabWindow::WindowProc(HWND window, UINT message, WPARAM wparam,
                                       LPARAM lparam) {
  LabWindow* self = reinterpret_cast<LabWindow*>(
      ::GetWindowLongPtrW(window, GWLP_USERDATA));
  switch (message) {
    case WM_NCCREATE: {
      CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      self = static_cast<LabWindow*>(create->lpCreateParams);
      ::SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(self));
      if (self != nullptr) {
        // Available to WM_CREATE: child windows are parented to this handle.
        self->hwnd_ = window;
        ++self->nccreate_messages_;
      }
      return TRUE;
    }
    case WM_CREATE:
      if (self == nullptr || !self->CreateChildren()) {
        if (self != nullptr) {
          self->create_rejected_ = true;
        }
        return -1;
      }
      ++self->create_messages_;
      return 0;
    case WM_SIZE:
      if (self != nullptr) {
        self->Layout();
      }
      return 0;
    case WM_COMMAND:
      if (self != nullptr && HIWORD(wparam) == BN_CLICKED) {
        self->OnCommand(static_cast<int>(LOWORD(wparam)));
      }
      return 0;
    case WM_TIMER:
      if (self != nullptr && wparam == kLabTimerId) {
        self->OnTimer();
      }
      return 0;
    case WM_CLOSE:
      if (self != nullptr) {
        if (self->scenario_running_ ||
            (self->runner_ != nullptr && self->runner_->busy())) {
          self->close_requested_ = true;
          self->AddStatusLine(
              "close requested; finishing the current scenario step");
          self->RefreshView();
          return 0;
        }
        self->Destroy();
      }
      return 0;
    case WM_DESTROY:
      if (self != nullptr) {
        if (self->session_ != nullptr) {
          self->ShutdownSession();
        }
        if (self->in_message_loop_) {
          ::PostQuitMessage(0);
        }
      }
      return 0;
    case WM_NCDESTROY:
      if (self != nullptr) {
        ++self->ncdestroy_messages_;
        self->hwnd_ = nullptr;
      }
      ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      return ::DefWindowProcW(window, message, wparam, lparam);
    case WM_GETMINMAXINFO: {
      MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lparam);
      info->ptMinTrackSize.x = 1000;
      info->ptMinTrackSize.y = 720;
      return 0;
    }
    default:
      return ::DefWindowProcW(window, message, wparam, lparam);
  }
}

LRESULT CALLBACK LabWindow::DocumentProc(HWND window, UINT message,
                                         WPARAM wparam, LPARAM lparam) {
  DocumentSurface* surface = reinterpret_cast<DocumentSurface*>(
      ::GetWindowLongPtrW(window, GWLP_USERDATA));
  switch (message) {
    case WM_NCCREATE: {
      CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      surface = static_cast<DocumentSurface*>(create->lpCreateParams);
      ::SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(surface));
      return TRUE;
    }
    case WM_NCDESTROY:
      ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
      return ::DefWindowProcW(window, message, wparam, lparam);
    case WM_SETFOCUS:
      if (surface != nullptr && surface->owner != nullptr) {
        surface->owner->OnDocumentFocus(surface->index);
      }
      return 0;
    case WM_KILLFOCUS:
      ::InvalidateRect(window, nullptr, FALSE);
      return 0;
    case WM_LBUTTONDOWN:
      ::SetFocus(window);
      return 0;
    case WM_PAINT:
      if (surface != nullptr && surface->owner != nullptr) {
        surface->owner->PaintDocument(*surface);
      }
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_CHAR:
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_SYSCHAR:
      // This window is a lab surface, not a text editor and not a key sink.
      return 0;
    default:
      return ::DefWindowProcW(window, message, wparam, lparam);
  }
}

}  // namespace lab
}  // namespace tsf
}  // namespace epocanva
