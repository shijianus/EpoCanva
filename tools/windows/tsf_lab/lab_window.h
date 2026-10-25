// EpoCanva P3.11 -- the visible native Win32 shell of the TSF integration lab.
//
// WHAT THIS IS. One ordinary top-level window with two focusable child document surfaces
// and a fixed set of scenario buttons. The child surfaces are not edit controls: they paint
// the backing text of the two real TSF documents and receive WM_SETFOCUS, which is what
// lets the application give each document its own real Win32 focus and then call
// ITfThreadMgr::SetFocus. No keyboard sink is installed anywhere; WM_CHAR is ignored.
//
// WINDOW-CREATION RULES (the P3.10 failure this round corrected). The window procedure
// forwards wParam/lParam unchanged to DefWindowProcW. The initial state pointer travels in
// CREATESTRUCT::lpCreateParams and is installed in GWLP_USERDATA during WM_NCCREATE, before
// WM_CREATE runs. CreateWindowExW's GetLastError is captured immediately. A window procedure
// that rejects WM_NCCREATE/WM_CREATE is reported as such; error 0 is never rewritten as
// "environment unavailable".
//
// SHUTDOWN. Closing the window first stops the lab session (which clears AssociateFocus,
// unadvises sinks, closes the coordinator, stops the executor and shuts the TSF host down)
// and only then destroys the child windows. A close during a synchronous scenario is
// deferred until the current step returns; the scenario pump checks the cancel flag.
#ifndef EPOCANVA_TOOLS_WINDOWS_TSF_LAB_LAB_WINDOW_H_
#define EPOCANVA_TOOLS_WINDOWS_TSF_LAB_LAB_WINDOW_H_

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "lab_session.h"
#include "scenario_runner.h"

namespace epocanva {
namespace tsf {
namespace lab {

struct LabWindowOptions {
  LabConfig config;
  bool show = true;
  bool start_session = true;
  // Interactive windows use real Win32 focus + WM_SETFOCUS. Headless tests and the
  // screenshot mode use the real ITfThreadMgr::SetFocus path and mark the bind as
  // revalidation, never as an OnSetFocus notification.
  bool use_window_focus = true;
};

class LabWindow {
 public:
  static std::unique_ptr<LabWindow> Create(const LabWindowOptions& options,
                                           std::string* error);
  ~LabWindow();

  LabWindow(const LabWindow&) = delete;
  LabWindow& operator=(const LabWindow&) = delete;

  HWND hwnd() const { return hwnd_; }
  HWND document_window(int index) const;
  HWND status_window() const { return status_window_; }
  bool session_ready() const;
  bool ready() const { return session_ready(); }
  LabSession* session() { return session_.get(); }
  ScenarioRunner* runner() { return runner_.get(); }
  const std::string& startup_error() const { return startup_error_; }

  // Blocks until the top-level window is destroyed.
  void RunMessageLoop();
  // Shuts the session down and destroys the HWND/classes. Idempotent.
  void Destroy();
  void ShutdownSession();
  bool RestartSession(std::string* error);
  void RefreshView();

  // Renders this window (including its child controls) with PrintWindow into a BMP file.
  // No screen capture of other windows is involved.
  bool CaptureToBmp(const std::filesystem::path& path, std::string* error);

  // Test/evidence accessors.
  std::uint64_t nccreate_messages() const { return nccreate_messages_; }
  std::uint64_t create_messages() const { return create_messages_; }
  std::uint64_t ncdestroy_messages() const { return ncdestroy_messages_; }
  bool create_rejected() const { return create_rejected_; }
  const std::string& create_error() const { return create_error_; }
  bool classes_registered() const { return classes_registered_; }
  bool close_requested() const { return close_requested_; }
  static bool ClassesRegistered();
  static int LiveWindowCount();

  void AddStatusLine(const std::string& line);

 private:
  struct DocumentSurface {
    LabWindow* owner = nullptr;
    int index = 0;
  };

  explicit LabWindow(const LabWindowOptions& options);

  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                     LPARAM lparam);
  static LRESULT CALLBACK DocumentProc(HWND window, UINT message, WPARAM wparam,
                                       LPARAM lparam);
  static bool EnsureClasses(std::string* error);
  static void ReleaseClasses();

  bool CreateChildren();
  bool StartSessionInternal(std::string* error);
  void Layout();
  void PaintDocument(const DocumentSurface& surface);
  void OnDocumentFocus(int index);
  void OnCommand(int command_id);
  void OnTimer();
  void RunScenario(ScenarioId id);
  void SetButtonsEnabled(bool enabled);
  void MaybeRefreshEngineState(bool force);
  std::wstring BuildStatusText();
  std::wstring BuildDocumentLine(int index, const LabSnapshot& snapshot);
  std::wstring BuildEngineLine(const LabSnapshot& snapshot);

  LabWindowOptions options_;
  HWND hwnd_ = nullptr;
  HWND document_windows_[2] = {nullptr, nullptr};
  HWND buttons_[10] = {};
  HWND status_window_ = nullptr;
  DocumentSurface surfaces_[2];
  HFONT ui_font_ = nullptr;
  HFONT mono_font_ = nullptr;

  std::unique_ptr<LabSession> session_;
  std::unique_ptr<ScenarioRunner> runner_;
  std::string startup_error_;
  std::string last_engine_error_;

  std::vector<std::string> status_lines_;
  std::wstring last_status_text_;
  bool scenario_running_ = false;
  bool close_requested_ = false;
  bool in_message_loop_ = false;
  bool counted_live_ = false;
  bool classes_registered_ = false;

  std::uint64_t nccreate_messages_ = 0;
  std::uint64_t create_messages_ = 0;
  std::uint64_t ncdestroy_messages_ = 0;
  bool create_rejected_ = false;
  std::string create_error_;
  ULONGLONG last_refresh_tick_ = 0;

  static int class_refs_;
  static int live_windows_;
};

}  // namespace lab
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TOOLS_WINDOWS_TSF_LAB_LAB_WINDOW_H_
