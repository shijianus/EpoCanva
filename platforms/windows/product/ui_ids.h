#ifndef EPOCANVA_PRODUCT_UI_IDS_H_
#define EPOCANVA_PRODUCT_UI_IDS_H_

namespace epocanva {
namespace product {
namespace ui {

inline constexpr wchar_t kSettingsWindowClass[] = L"EpoCanvaSettingsWindow";
inline constexpr wchar_t kSettingsWindowTitle[] =
    L"EpoCanva English Alpha - Settings";

enum ControlId : int {
  kSummary = 1001,
  kStatusEdit = 1002,
  kCompletionEnabled = 1010,
  kCompletionCount = 1011,
  kSpellingEnabled = 1020,
  kSpellingCount = 1021,
  kCandidateDisplayHost = 1025,
  kCandidateDisplayAlwaysSelf = 1026,
  kOpenInstall = 1030,
  kOpenUser = 1031,
  kSave = 1040,
  kClose = 1041,
  kAbout = 1042,
  kSaveStatus = 1050,
  kPathInstall = 1060,
  kPathUser = 1061,
  kPathSettings = 1062,
  kPathEngine = 1063,
};

}  // namespace ui
}  // namespace product
}  // namespace epocanva

#endif  // EPOCANVA_PRODUCT_UI_IDS_H_
