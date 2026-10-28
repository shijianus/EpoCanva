// EpoCanva W2-B -- the candidate popup implementation. See candidate_window.h.
#include "candidate_window.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <mutex>

namespace epocanva {
namespace tsf {
namespace product {
namespace {

constexpr wchar_t kWindowClass[] = L"EpoCanva.CandidateWindow.W2B";
constexpr int kDesignRowHeight = 22;
constexpr int kDesignPadding = 6;
constexpr int kDesignKindWidth = 54;
constexpr int kDesignFooterHeight = 18;
constexpr int kDesignMinWidth = 140;

std::mutex g_class_mutex;
HMODULE g_class_module = nullptr;
int g_class_refcount = 0;

bool EnsureWindowClass(HMODULE module, std::wstring* why) {
  if (module == nullptr) {
    if (why != nullptr) {
      *why = L"the candidate window needs the owning module instance";
    }
    return false;
  }
  std::lock_guard<std::mutex> guard(g_class_mutex);
  if (g_class_refcount > 0) {
    if (g_class_module != module) {
      if (why != nullptr) {
        *why = L"the candidate window class is owned by another module";
      }
      return false;
    }
    ++g_class_refcount;
    return true;
  }
  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_DROPSHADOW;
  window_class.lpfnWndProc = &CandidateWindow::WindowProc;
  window_class.hInstance = module;
  window_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = kWindowClass;
  if (::RegisterClassExW(&window_class) == 0) {
    if (why != nullptr) {
      *why = L"RegisterClassExW failed with error " +
             std::to_wstring(::GetLastError());
    }
    return false;
  }
  g_class_module = module;
  g_class_refcount = 1;
  return true;
}

void ReleaseWindowClass() {
  std::lock_guard<std::mutex> guard(g_class_mutex);
  if (g_class_refcount == 0) {
    return;
  }
  --g_class_refcount;
  if (g_class_refcount == 0) {
    (void)::UnregisterClassW(kWindowClass, g_class_module);
    g_class_module = nullptr;
  }
}

using GetDpiForWindowFn = UINT(WINAPI*)(HWND);

UINT WindowDpi(HWND window) {
  const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
  if (user32 != nullptr) {
    const auto fn = reinterpret_cast<GetDpiForWindowFn>(
        ::GetProcAddress(user32, "GetDpiForWindow"));
    if (fn != nullptr) {
      const UINT dpi = fn(window);
      if (dpi != 0) {
        return dpi;
      }
    }
  }
  return 96;
}

RECT WorkAreaFor(const RECT& anchor) {
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  const HMONITOR monitor = ::MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
  if (::GetMonitorInfoW(monitor, &info)) {
    return info.rcWork;
  }
  RECT fallback = {};
  ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &fallback, 0);
  if (fallback.right <= fallback.left || fallback.bottom <= fallback.top) {
    fallback = RECT{0, 0, ::GetSystemMetrics(SM_CXSCREEN),
                    ::GetSystemMetrics(SM_CYSCREEN)};
  }
  return fallback;
}

}  // namespace

CandidateWindowLayout ComputeCandidateWindowLayout(
    const RECT& anchor, int content_width, int row_count, int row_height, int padding,
    int footer_height, const RECT& work_area) {
  CandidateWindowLayout layout;
  layout.row_height = std::max(1, row_height);
  layout.padding = std::max(0, padding);
  const int rows = std::max(0, row_count);
  int width = std::max(content_width + 2 * layout.padding, kDesignMinWidth);
  const int work_width = std::max(1, static_cast<int>(work_area.right - work_area.left));
  const int work_height = std::max(1, static_cast<int>(work_area.bottom - work_area.top));
  width = std::min(width, work_width - 8);
  width = std::max(width, 40);
  int height = rows * layout.row_height + 2 * layout.padding + std::max(0, footer_height);
  height = std::min(height, work_height - 8);
  height = std::max(height, layout.row_height + 2 * layout.padding);

  int left = static_cast<int>(anchor.left);
  int top = static_cast<int>(anchor.bottom) + 2;
  left = std::max(left, static_cast<int>(work_area.left) + 4);
  left = std::min(left, static_cast<int>(work_area.right) - 4 - width);
  if (top + height > static_cast<int>(work_area.bottom) - 4) {
    top = static_cast<int>(anchor.top) - 2 - height;
  }
  top = std::max(top, static_cast<int>(work_area.top) + 4);
  top = std::min(top, static_cast<int>(work_area.bottom) - 4 - height);
  layout.bounds = RECT{left, top, left + width, top + height};

  layout.rows.reserve(static_cast<std::size_t>(rows));
  int y = top + layout.padding;
  for (int index = 0; index < rows; ++index) {
    RECT row = {left + layout.padding, y, left + width - layout.padding,
                y + layout.row_height};
    layout.rows.push_back(row);
    y += layout.row_height;
  }
  return layout;
}

CandidateWindow::CandidateWindow(HMODULE module, SelectCallback on_select,
                                 DismissCallback on_dismiss)
    : module_(module), on_select_(std::move(on_select)),
      on_dismiss_(std::move(on_dismiss)) {}

CandidateWindow::~CandidateWindow() { Destroy(); }

bool CandidateWindow::Create(std::wstring* why) {
  if (window_ != nullptr) {
    return true;
  }
  if (!EnsureWindowClass(module_, why)) {
    return false;
  }
  window_ = ::CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kWindowClass, L"EpoCanva",
      WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, module_, this);
  if (window_ == nullptr) {
    if (why != nullptr) {
      *why = L"CreateWindowExW failed with error " + std::to_wstring(::GetLastError());
    }
    ReleaseWindowClass();
    return false;
  }
  ::SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  dpi_ = WindowDpi(window_);
  EnsureFont();
  return true;
}

void CandidateWindow::Destroy() {
  if (font_ != nullptr) {
    ::DeleteObject(font_);
    font_ = nullptr;
  }
  if (window_ != nullptr) {
    ::SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
    ::DestroyWindow(window_);
    window_ = nullptr;
    ReleaseWindowClass();
  }
  visible_ = false;
}

void CandidateWindow::EnsureFont() {
  if (font_ != nullptr) {
    return;
  }
  const int height = -::MulDiv(9, static_cast<int>(dpi_), 72);
  font_ = ::CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

int CandidateWindow::MeasureContentWidth(HDC dc) const {
  const int kind_width = ::MulDiv(kDesignKindWidth, static_cast<int>(dpi_), 96);
  int widest = 0;
  for (const CandidateWindowItem& item : items_) {
    SIZE size = {};
    if (!item.text.empty() &&
        ::GetTextExtentPoint32W(dc, item.text.c_str(),
                                static_cast<int>(item.text.size()), &size)) {
      widest = std::max(widest, static_cast<int>(size.cx));
    }
  }
  return widest + kind_width;
}

void CandidateWindow::Show(const std::vector<CandidateWindowItem>& items, int selected,
                           int page_no, int page_size, bool is_last_page,
                           const RECT& anchor) {
  if (window_ == nullptr && !Create(nullptr)) {
    return;
  }
  items_ = items;
  selected_ = selected;
  page_no_ = page_no;
  page_size_ = page_size;
  is_last_page_ = is_last_page;
  if (items_.size() > static_cast<std::size_t>(kMaxVisibleRows)) {
    items_.resize(kMaxVisibleRows);
  }
  if (selected_ >= static_cast<int>(items_.size())) {
    selected_ = -1;
  }
  anchor_ = anchor;
  has_anchor_ = true;
  Position(anchor);
  visible_ = true;
  ::ShowWindow(window_, SW_SHOWNOACTIVATE);
  ::InvalidateRect(window_, nullptr, TRUE);
}

void CandidateWindow::SetSelection(int selected) {
  selected_ = selected;
  if (selected_ >= static_cast<int>(items_.size())) {
    selected_ = -1;
  }
  if (window_ != nullptr && visible_) {
    ::InvalidateRect(window_, nullptr, FALSE);
  }
}

void CandidateWindow::Hide() {
  if (window_ != nullptr && visible_) {
    ::ShowWindow(window_, SW_HIDE);
  }
  visible_ = false;
}

void CandidateWindow::Reposition(const RECT& anchor) {
  anchor_ = anchor;
  has_anchor_ = true;
  if (window_ != nullptr && visible_) {
    Position(anchor);
    ::InvalidateRect(window_, nullptr, TRUE);
  }
}

void CandidateWindow::Position(const RECT& anchor) {
  if (window_ == nullptr) {
    return;
  }
  EnsureFont();
  const int row_height = ::MulDiv(kDesignRowHeight, static_cast<int>(dpi_), 96);
  const int padding = ::MulDiv(kDesignPadding, static_cast<int>(dpi_), 96);
  const int footer = page_size_ > 0
                         ? ::MulDiv(kDesignFooterHeight, static_cast<int>(dpi_), 96)
                         : 0;
  HDC dc = ::GetDC(window_);
  int content_width = kDesignMinWidth;
  if (dc != nullptr) {
    const HGDIOBJ previous = ::SelectObject(dc, font_);
    content_width = MeasureContentWidth(dc);
    if (previous != nullptr) {
      ::SelectObject(dc, previous);
    }
    ::ReleaseDC(window_, dc);
  }
  const CandidateWindowLayout layout = ComputeCandidateWindowLayout(
      anchor, content_width, static_cast<int>(items_.size()), row_height, padding,
      footer, WorkAreaFor(anchor));
  row_height_ = layout.row_height;
  padding_ = layout.padding;
  row_rects_.clear();
  for (const RECT& screen_row : layout.rows) {
    RECT client_row = screen_row;
    client_row.left -= layout.bounds.left;
    client_row.right -= layout.bounds.left;
    client_row.top -= layout.bounds.top;
    client_row.bottom -= layout.bounds.top;
    row_rects_.push_back(client_row);
  }
  bounds_ = layout.bounds;
  ::SetWindowPos(window_, HWND_TOPMOST, layout.bounds.left, layout.bounds.top,
                 layout.bounds.right - layout.bounds.left,
                 layout.bounds.bottom - layout.bounds.top,
                 SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void CandidateWindow::OnLeftButtonDown(int x, int y) {
  for (std::size_t index = 0; index < row_rects_.size(); ++index) {
    const RECT& row = row_rects_[index];
    if (x >= row.left && x < row.right && y >= row.top && y < row.bottom) {
      if (on_select_) {
        on_select_(index);
      }
      return;
    }
  }
}

void CandidateWindow::OnPaint() {
  PAINTSTRUCT paint = {};
  HDC dc = ::BeginPaint(window_, &paint);
  if (dc == nullptr) {
    return;
  }
  RECT client = {};
  ::GetClientRect(window_, &client);
  const HBRUSH background = ::CreateSolidBrush(::GetSysColor(COLOR_WINDOW));
  ::FillRect(dc, &client, background);
  ::DeleteObject(background);
  const HGDIOBJ previous = ::SelectObject(dc, font_);
  ::SetBkMode(dc, TRANSPARENT);
  for (std::size_t index = 0; index < row_rects_.size(); ++index) {
    DrawRow(dc, row_rects_[index], index);
  }
  if (page_size_ > 0) {
    wchar_t footer[32] = {};
    if (is_last_page_) {
      swprintf_s(footer, L"page %d", page_no_ + 1);
    } else {
      swprintf_s(footer, L"page %d ...", page_no_ + 1);
    }
    RECT footer_rect = client;
    footer_rect.top = row_rects_.empty() ? client.bottom - padding_ - 14
                                         : row_rects_.back().bottom;
    footer_rect.left += padding_;
    footer_rect.right -= padding_;
    ::SetTextColor(dc, ::GetSysColor(COLOR_GRAYTEXT));
    ::DrawTextW(dc, footer, -1, &footer_rect,
                DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX);
  }
  if (previous != nullptr) {
    ::SelectObject(dc, previous);
  }
  ::EndPaint(window_, &paint);
}

void CandidateWindow::DrawRow(HDC dc, const RECT& row, std::size_t index) const {
  if (index >= items_.size()) {
    return;
  }
  const bool chosen = static_cast<int>(index) == selected_;
  if (chosen) {
    const HBRUSH highlight = ::CreateSolidBrush(::GetSysColor(COLOR_HIGHLIGHT));
    ::FillRect(dc, &row, highlight);
    ::DeleteObject(highlight);
    ::SetTextColor(dc, ::GetSysColor(COLOR_HIGHLIGHTTEXT));
  } else {
    ::SetTextColor(dc, ::GetSysColor(COLOR_WINDOWTEXT));
  }
  const int kind_width = ::MulDiv(kDesignKindWidth, static_cast<int>(dpi_), 96);
  RECT kind_rect = row;
  kind_rect.right = kind_rect.left + kind_width;
  const wchar_t* kind = L"COMP";
  switch (items_[index].kind) {
    case coordinator::CandidateKind::kLiteral:
      kind = L"RAW";
      break;
    case coordinator::CandidateKind::kSpelling:
      kind = L"SPELL";
      break;
    case coordinator::CandidateKind::kCompletion:
      kind = L"COMP";
      break;
  }
  if (!chosen) {
    COLORREF color = ::GetSysColor(COLOR_GRAYTEXT);
    if (items_[index].kind == coordinator::CandidateKind::kLiteral) {
      color = RGB(0, 110, 60);
    } else if (items_[index].kind == coordinator::CandidateKind::kSpelling) {
      color = RGB(170, 90, 0);
    }
    ::SetTextColor(dc, color);
  }
  ::DrawTextW(dc, kind, -1, &kind_rect,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
  if (!chosen) {
    ::SetTextColor(dc, ::GetSysColor(COLOR_WINDOWTEXT));
  }
  RECT text_rect = row;
  text_rect.left = kind_rect.right;
  ::DrawTextW(dc, items_[index].text.c_str(), -1, &text_rect,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
}

LRESULT CALLBACK CandidateWindow::WindowProc(HWND window, UINT message, WPARAM wparam,
                                             LPARAM lparam) {
  CandidateWindow* self =
      reinterpret_cast<CandidateWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
  if (self != nullptr) {
    return self->HandleMessage(message, wparam, lparam);
  }
  return ::DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CandidateWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_MOUSEACTIVATE:
      // A click must never move the editing focus away from the host document.
      return MA_NOACTIVATE;
    case WM_LBUTTONDOWN:
      OnLeftButtonDown(static_cast<int>(GET_X_LPARAM(lparam)),
                       static_cast<int>(GET_Y_LPARAM(lparam)));
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      OnPaint();
      return 0;
    case WM_DPICHANGED: {
      dpi_ = HIWORD(wparam);
      if (font_ != nullptr) {
        ::DeleteObject(font_);
        font_ = nullptr;
      }
      EnsureFont();
      if (has_anchor_) {
        Position(anchor_);
      }
      ::InvalidateRect(window_, nullptr, TRUE);
      return 0;
    }
    case WM_NCDESTROY: {
      const HWND window = window_;
      window_ = nullptr;
      visible_ = false;
      return ::DefWindowProcW(window, message, wparam, lparam);
    }
    default:
      return ::DefWindowProcW(window_, message, wparam, lparam);
  }
}

}  // namespace product
}  // namespace tsf
}  // namespace epocanva
