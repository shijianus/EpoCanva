#ifndef EPOCANVA_PRODUCT_SETTINGS_WINDOW_H_
#define EPOCANVA_PRODUCT_SETTINGS_WINDOW_H_

#include <windows.h>

#include <vector>

#include "product_diagnostics.h"
#include "product_paths.h"
#include "product_settings.h"

namespace epocanva {
namespace product {

class SettingsWindow {
 public:
  static bool RegisterClass(HINSTANCE instance);
  SettingsWindow();
  ~SettingsWindow();

  SettingsWindow(const SettingsWindow&) = delete;
  SettingsWindow& operator=(const SettingsWindow&) = delete;

  bool Create(HINSTANCE instance, int show_command, std::wstring* why);
  HWND hwnd() const { return hwnd_; }

 private:
  struct Placement {
    HWND hwnd = nullptr;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool group = false;
  };

  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                     LPARAM lparam);
  LRESULT HandleMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

  bool OnCreate();
  void OnDestroy();
  void Layout();
  void FitClientToDpi();
  void ApplyFonts();
  void LoadState();
  void RefreshControlsFromSettings();
  void RefreshStatusText();
  void RefreshPathFields();
  void SetSaveStatus(const std::wstring& text);
  void SetDirty(bool dirty);
  bool Save();
  void TryClose();
  void OnCommand(int id, int notification, HWND control);
  void ShowAbout();
  void OpenFolder(const std::filesystem::path& path, const wchar_t* what);
  HBRUSH OnCtlColor(HDC dc, HWND control, UINT message);

  HWND CreateControl(const wchar_t* class_name, const wchar_t* text, DWORD style,
                     int id, int x, int y, int width, int height);

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  UINT dpi_ = 96;
  HFONT body_font_ = nullptr;
  HFONT bold_font_ = nullptr;
  HFONT title_font_ = nullptr;
  bool dirty_ = false;
  bool loading_ = false;

  ProductPaths paths_;
  ProductStatus status_;
  ProductSettings settings_;
  std::vector<Placement> placements_;

  HWND title_ = nullptr;
  HWND subtitle_ = nullptr;
  HWND version_ = nullptr;
  HWND summary_ = nullptr;
  HWND status_edit_ = nullptr;
  HWND completion_enabled_ = nullptr;
  HWND completion_count_ = nullptr;
  HWND spelling_enabled_ = nullptr;
  HWND spelling_count_ = nullptr;
  HWND morphology_enabled_ = nullptr;
  HWND abbreviation_enabled_ = nullptr;
  HWND two_edit_spelling_enabled_ = nullptr;
  HWND candidate_display_host_ = nullptr;
  HWND candidate_display_always_self_ = nullptr;
  HWND save_status_ = nullptr;
  HWND path_install_ = nullptr;
  HWND path_user_ = nullptr;
  HWND path_settings_ = nullptr;
  HWND path_engine_ = nullptr;
};

}  // namespace product
}  // namespace epocanva

#endif  // EPOCANVA_PRODUCT_SETTINGS_WINDOW_H_
