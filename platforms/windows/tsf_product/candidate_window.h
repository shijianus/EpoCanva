// EpoCanva W2-B -- the real Win32 candidate window.
//
// WHAT THIS IS. A top-level popup owned by the TSF DLL's STA. It is drawn with GDI, never
// activates, never takes focus and never sends input anywhere: a click calls one callback
// with the page-relative index, and the controller validates that click against the
// snapshot version before any engine or document operation. The window is a presentation
// surface, not an edit path.
//
// PLACEMENT. The caller supplies the caret rectangle from a real TSF text-extent read. If
// no usable rectangle exists the window is hidden, never placed at the mouse position or a
// screen corner. The placement is clamped to the work area of the monitor that owns the
// caret, and flips above the caret when there is no room below.
//
// UNITS. RECT values are physical screen pixels (TSF GetTextExt and Win32 agree). `dpi` is
// the window's DPI; metrics are scaled with MulDiv from the 96-DPI design values.
#ifndef EPOCANVA_WINDOWS_TSF_PRODUCT_CANDIDATE_WINDOW_H_
#define EPOCANVA_WINDOWS_TSF_PRODUCT_CANDIDATE_WINDOW_H_

#include <windows.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "candidate_snapshot.h"

namespace epocanva {
namespace tsf {
namespace product {

struct CandidateWindowItem {
  std::wstring text;
  coordinator::CandidateKind kind = coordinator::CandidateKind::kCompletion;
};

// One pure placement result. `rows` are client-area rectangles in physical pixels.
struct CandidateWindowLayout {
  RECT bounds{};
  std::vector<RECT> rows;
  int row_height = 0;
  int padding = 0;
};

// Pure layout, testable without creating a window. `content_width` is the measured text
// width including the kind column; `row_count` is the number of rows to show.
CandidateWindowLayout ComputeCandidateWindowLayout(
    const RECT& anchor, int content_width, int row_count, int row_height, int padding,
    int footer_height, const RECT& work_area);

class CandidateWindow {
 public:
  using SelectCallback = std::function<void(std::size_t page_index)>;
  using DismissCallback = std::function<void()>;

  CandidateWindow(HMODULE module, SelectCallback on_select, DismissCallback on_dismiss);
  ~CandidateWindow();

  CandidateWindow(const CandidateWindow&) = delete;
  CandidateWindow& operator=(const CandidateWindow&) = delete;

  bool Create(std::wstring* why);
  void Destroy();
  bool created() const { return window_ != nullptr; }
  HWND window_for_test() const { return window_; }
  bool visible() const { return visible_; }
  RECT bounds_for_test() const { return bounds_; }
  const std::vector<RECT>& rows_for_test() const { return row_rects_; }
  int row_height_for_test() const { return row_height_; }

  // Shows or updates the list. `anchor` is the composition/caret rectangle read from TSF.
  // `selected` < 0 means the user has not chosen a row; no row is highlighted.
  void Show(const std::vector<CandidateWindowItem>& items, int selected, int page_no,
            int page_size, bool is_last_page, const RECT& anchor);
  void SetSelection(int selected);
  void Hide();
  // Re-applies the last anchor; used when the host's DPI changes.
  void Reposition(const RECT& anchor);

  static constexpr int kMaxVisibleRows = 8;

  // The window class dispatches here. Public only because the process-wide class proc is
  // not a member; it is not part of the controller-facing API.
  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                     LPARAM lparam);

 private:
  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
  void OnPaint();
  void OnLeftButtonDown(int x, int y);
  void EnsureFont();
  int MeasureContentWidth(HDC dc) const;
  void Position(const RECT& anchor);
  void DrawRow(HDC dc, const RECT& row, std::size_t index) const;

  HMODULE module_ = nullptr;
  HWND window_ = nullptr;
  HFONT font_ = nullptr;
  UINT dpi_ = 96;
  SelectCallback on_select_;
  DismissCallback on_dismiss_;
  std::vector<CandidateWindowItem> items_;
  std::vector<RECT> row_rects_;
  int selected_ = -1;
  int page_no_ = 0;
  int page_size_ = 0;
  bool is_last_page_ = true;
  bool visible_ = false;
  RECT anchor_{};
  bool has_anchor_ = false;
  RECT bounds_{};
  int row_height_ = 0;
  int padding_ = 0;
};

}  // namespace product
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_PRODUCT_CANDIDATE_WINDOW_H_
