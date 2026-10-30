// EpoCanva W2-B -- controlled candidate window and UIElement lifecycle tests.
//
// LAYER. This is a controlled Win32/COM test: a fake ITfUIElementMgr and ITfSource stand in
// for the TSF UI element manager, and the real CandidateWindow is created on the test's STA.
// It does not register the DLL and does not prove real TSF host integration; that remains
// layer C and is not claimed here. What it does prove: the real Begin/Update/End lifecycle,
// host-drawn suppression, pointer-safe teardown, click routing through the validated
// callback, no-activate window styles, deterministic placement at 100/150/200% metrics and
// small/multi-monitor work areas, and a real screenshot of the rendered window.
#include <windows.h>
#include <msctf.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "candidate_anchor.h"
#include "candidate_snapshot.h"
#include "candidate_ui.h"
#include "candidate_window.h"

namespace {

using epocanva::tsf::coordinator::CandidateKind;
using epocanva::tsf::coordinator::CandidateSnapshot;
using epocanva::tsf::product::CandidateUi;
using epocanva::tsf::product::CandidateUiHost;
using epocanva::tsf::product::CandidateUiSelection;
using epocanva::tsf::product::CandidateWindow;
using epocanva::tsf::product::CandidateWindowItem;
using epocanva::tsf::product::ComputeCandidateWindowLayout;

int g_failures = 0;

void Check(bool condition, const std::string& what) {
  std::cout << (condition ? "  ok   " : "  FAIL ") << what << "\n";
  if (!condition) {
    ++g_failures;
  }
}

bool SaveScreenshot(HWND window, const std::filesystem::path& path,
                    std::wstring* why) {
  ::RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
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
    *why = L"cannot write the screenshot";
    return false;
  }
  return true;
}

class FakeUiElementManager final : public ITfUIElementMgr, public ITfSource {
 public:
  bool show = true;
  BOOL show_on_entry = FALSE;
  int begins = 0;
  int updates = 0;
  int ends = 0;
  ITfUIElement* element = nullptr;
  ITfUIElementSink* sink = nullptr;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) {
      return E_INVALIDARG;
    }
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfUIElementMgr ||
        riid == IID_ITfSource) {
      *object = static_cast<ITfUIElementMgr*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(reference_count_.fetch_add(1) + 1);
  }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG remaining = reference_count_.fetch_sub(1) - 1;
    if (remaining == 0) {
      delete this;
    }
    return static_cast<ULONG>(remaining);
  }

  HRESULT STDMETHODCALLTYPE BeginUIElement(ITfUIElement* pElement, BOOL* pbShow,
                                           DWORD* id) override {
    if (pElement == nullptr || pbShow == nullptr || id == nullptr) {
      return E_INVALIDARG;
    }
    if (element != nullptr) {
      element->Release();
    }
    element = pElement;
    element->AddRef();
    show_on_entry = *pbShow;
    *pbShow = show ? TRUE : FALSE;
    *id = 1;
    ++begins;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE UpdateUIElement(DWORD) override {
    ++updates;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EndUIElement(DWORD) override {
    ++ends;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetUIElement(DWORD, ITfUIElement**) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE EnumUIElements(IEnumTfUIElements**) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE AdviseSink(REFIID riid, IUnknown* unknown,
                                      DWORD* cookie) override {
    if (cookie == nullptr || unknown == nullptr ||
        riid != IID_ITfUIElementSink) {
      return E_NOINTERFACE;
    }
    ITfUIElementSink* sink = nullptr;
    if (FAILED(unknown->QueryInterface(IID_ITfUIElementSink,
                                       reinterpret_cast<void**>(&sink))) ||
        sink == nullptr) {
      return E_NOINTERFACE;
    }
    this->sink = sink;  // QI added the reference we keep
    *cookie = 7;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE UnadviseSink(DWORD) override {
    if (sink != nullptr) {
      sink->Release();
      sink = nullptr;
    }
    return S_OK;
  }

 private:
  ~FakeUiElementManager() {
    if (element != nullptr) {
      element->Release();
      element = nullptr;
    }
    if (sink != nullptr) {
      sink->Release();
      sink = nullptr;
    }
  }
  std::atomic<LONG> reference_count_{1};
};

struct FakeHost final : CandidateUiHost {
  int selections = 0;
  int finals = 0;
  int aborts = 0;
  int show_requests = 0;
  CandidateUiSelection last;

  void OnUiSelectionChanged(const CandidateUiSelection& selection) override {
    ++selections;
    last = selection;
  }
  void OnUiFinalize(const CandidateUiSelection& selection) override {
    ++finals;
    last = selection;
  }
  void OnUiAbort(std::uint64_t, std::uint64_t, std::uint64_t) override {
    ++aborts;
  }
  void OnUiShowRequested() override { ++show_requests; }
};

std::vector<CandidateWindowItem> WindowItems() {
  std::vector<CandidateWindowItem> items;
  items.push_back({L"hel", CandidateKind::kLiteral});
  items.push_back({L"hello", CandidateKind::kCompletion});
  items.push_back({L"help", CandidateKind::kCompletion});
  items.push_back({L"held", CandidateKind::kCompletion});
  items.push_back({L"hell", CandidateKind::kCompletion});
  items.push_back({L"helix", CandidateKind::kCompletion});
  items.push_back({L"helicopter", CandidateKind::kCompletion});
  items.push_back({L"helo", CandidateKind::kSpelling});
  return items;
}

CandidateSnapshot MakeSnapshot() {
  CandidateSnapshot snapshot;
  snapshot.valid = true;
  snapshot.version = 9;
  snapshot.identity = epocanva::tsf::edit::ContextIdentity{42, 7};
  snapshot.generation = 7;
  snapshot.work_id = 3;
  snapshot.page_no = 0;
  snapshot.page_size = 8;
  snapshot.is_last_page = true;
  snapshot.highlighted_page_index = 0;
  std::size_t index = 0;
  for (const CandidateWindowItem& item : WindowItems()) {
    epocanva::tsf::coordinator::CandidateItem candidate;
    candidate.text_utf16_code_units.assign(item.text.begin(), item.text.end());
    candidate.kind = item.kind;
    candidate.page_index = index;
    candidate.menu_index = index;
    if (item.kind == CandidateKind::kSpelling) {
      candidate.comment_utf16_code_units = u"spell";
    }
    snapshot.items.push_back(std::move(candidate));
    ++index;
  }
  return snapshot;
}

void TestLayout() {
  const RECT anchor = {100, 100, 120, 140};
  const RECT work = {0, 0, 1920, 1080};
  const auto layout = ComputeCandidateWindowLayout(anchor, 300, 8, 24, 6, 18, work);
  Check(layout.rows.size() == 8, "layout: eight rows for eight candidates");
  Check(layout.bounds.left == 100 && layout.bounds.top == 142,
        "layout: the popup sits just below the caret");
  Check(layout.bounds.right <= work.right && layout.bounds.bottom <= work.bottom,
        "layout: the popup stays inside the work area");

  const RECT near_bottom = {800, 1040, 820, 1070};
  const auto flipped = ComputeCandidateWindowLayout(near_bottom, 300, 8, 24, 6, 18, work);
  Check(flipped.bounds.bottom <= near_bottom.top,
        "layout: no room below flips the popup above the caret");

  const RECT narrow_work = {0, 0, 200, 200};
  const auto narrow =
      ComputeCandidateWindowLayout(anchor, 900, 8, 24, 6, 18, narrow_work);
  Check(narrow.bounds.right <= narrow_work.right &&
            narrow.bounds.bottom <= narrow_work.bottom,
        "layout: a small work area clamps instead of overflowing");
  Check(narrow.rows.size() == 8, "layout: the small work area keeps the real row count");

  // 100% / 150% / 200% metrics. The pure layout receives already-scaled metrics, exactly
  // as CandidateWindow passes them.
  const RECT monitor2 = {1920, 0, 3840, 1080};
  for (int dpi : {96, 144, 192}) {
    const int row = ::MulDiv(22, dpi, 96);
    const int padding = ::MulDiv(6, dpi, 96);
    const int footer = ::MulDiv(18, dpi, 96);
    const RECT high_dpi_anchor = {1920 + 100, 100, 1920 + 140, 150};
    const auto scaled = ComputeCandidateWindowLayout(high_dpi_anchor, 600, 8, row, padding,
                                                     footer, monitor2);
    Check(scaled.bounds.left >= monitor2.left && scaled.bounds.right <= monitor2.right &&
              scaled.bounds.bottom <= monitor2.bottom,
          "layout: " + std::to_string(dpi) + " DPI stays inside the second monitor");
    Check(scaled.row_height == row, "layout: " + std::to_string(dpi) +
                                         " DPI scales the row height");
  }
}

void TestWindow(const std::filesystem::path& screenshot) {
  int clicked = -1;
  int dismissed = 0;
  CandidateWindow window(::GetModuleHandleW(nullptr),
                         [&clicked](std::size_t index) {
                           clicked = static_cast<int>(index);
                         },
                         [&dismissed]() { ++dismissed; });
  std::wstring why;
  Check(window.Create(&why), "window: created");
  Check(window.window_for_test() != nullptr, "window: has a real HWND");
  const LONG_PTR styles =
      ::GetWindowLongPtrW(window.window_for_test(), GWL_EXSTYLE);
  Check((styles & WS_EX_NOACTIVATE) != 0,
        "window: WS_EX_NOACTIVATE prevents focus stealing");
  Check((styles & WS_EX_TOOLWINDOW) != 0,
        "window: WS_EX_TOOLWINDOW keeps it out of the task switcher");

  const RECT anchor = {80, 80, 100, 110};
  window.Show(WindowItems(), 1, 0, 8, true, anchor);
  Check(window.visible(), "window: shown at the caret rectangle");
  Check(window.rows_for_test().size() == 8, "window: eight visible rows");
  const RECT window_bounds = window.bounds_for_test();
  Check(!::IsRectEmpty(&window_bounds), "window: has a non-empty bounds");

  const LRESULT activate =
      ::SendMessageW(window.window_for_test(), WM_MOUSEACTIVATE, 0, 0);
  Check(activate == MA_NOACTIVATE, "window: a click does not activate the popup");

  if (window.rows_for_test().size() >= 3) {
    RECT row = window.rows_for_test()[2];
    ::SendMessageW(window.window_for_test(), WM_LBUTTONDOWN, MK_LBUTTON,
                   MAKELPARAM((row.left + row.right) / 2, (row.top + row.bottom) / 2));
    Check(clicked == 2, "window: a real click reports the clicked page index");
  } else {
    Check(false, "window: row rectangles available for the click");
  }

  window.SetSelection(3);
  Check(window.visible(), "window: selection update keeps the popup visible");

  if (!screenshot.empty()) {
    std::wstring shot_why;
    Check(SaveScreenshot(window.window_for_test(), screenshot, &shot_why),
          "window: screenshot captured at " + screenshot.string());
  }
  window.Hide();
  Check(!window.visible(), "window: hidden by the caller");
  window.Destroy();
  Check(!window.created(), "window: destroyed without touching the document");
}

void TestUiLifecycle() {
  FakeUiElementManager* manager = new FakeUiElementManager();
  FakeHost host;
  CandidateUi ui(1, &host, ::GetModuleHandleW(nullptr));
  std::wstring why;
  Check(ui.StartWithManagerForTest(manager, manager, &why),
        "ui: started with the controlled manager");
  const CandidateSnapshot snapshot = MakeSnapshot();
  const RECT anchor = {120, 120, 160, 150};

  ui.Update(snapshot, nullptr, &anchor, 1);
  Check(manager->begins == 1, "ui: one BeginUIElement");
  Check(manager->show_on_entry == TRUE,
        "ui: the TIP initializes pbShow=TRUE before the host decides");
  Check(ui.begun(), "ui: the element is begun");
  Check(!ui.host_drawn(), "ui: the fake host allows TIP drawing");
  Check(ui.window_visible(), "ui: the real window is visible");
  if (manager->element != nullptr) {
    BOOL shown = FALSE;
    Check(SUCCEEDED(manager->element->IsShown(&shown)) && shown == TRUE,
          "ui: IsShown reports the actually visible TIP window");
    ITfCandidateListUIElement* list = nullptr;
    const HRESULT qi = manager->element->QueryInterface(
        IID_ITfCandidateListUIElement, reinterpret_cast<void**>(&list));
    Check(SUCCEEDED(qi) && list != nullptr, "ui: the element answers the list interface");
    if (list != nullptr) {
      UINT count = 0;
      UINT selection = 0;
      UINT page_count = 0;
      UINT page_index = 0;
      BSTR first = nullptr;
      Check(SUCCEEDED(list->GetCount(&count)) && count == 8,
            "ui: GetCount reports the real page");
      Check(SUCCEEDED(list->GetSelection(&selection)) && selection == 1,
            "ui: GetSelection reports the explicit selection");
      Check(SUCCEEDED(list->GetPageIndex(&page_index, 1, &page_count)) &&
                page_count == 1 && page_index == 0,
            "ui: the page model has one real page and starts at 0");
      Check(SUCCEEDED(list->GetString(0, &first)) && first != nullptr &&
                std::wstring(first) == L"hel",
            "ui: GetString returns the literal row");
      ::SysFreeString(first);
      list->Release();
    }
  } else {
    Check(false, "ui: the manager received the element");
  }

  ui.Update(snapshot, nullptr, &anchor, 2);
  Check(manager->updates >= 1, "ui: an existing element is updated, not begun again");
  Check(manager->begins == 1, "ui: no duplicate BeginUIElement");

  // With no explicit selection the UILess contract is S_FALSE: the index is invalid and the
  // host never draws a suggestion (or the literal row) as preselected.
  ui.Update(snapshot, nullptr, &anchor, -1);
  if (manager->element != nullptr) {
    ITfCandidateListUIElement* list = nullptr;
    if (SUCCEEDED(manager->element->QueryInterface(
            IID_ITfCandidateListUIElement, reinterpret_cast<void**>(&list))) &&
        list != nullptr) {
      UINT selection = 99;
      DWORD flags = 0;
      Check(list->GetSelection(&selection) == S_FALSE,
            "ui: GetSelection answers S_FALSE when nothing is selected");
      Check(SUCCEEDED(list->GetUpdatedFlags(&flags)) &&
                flags == (TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION |
                          TF_CLUIE_STRING | TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE),
            "ui: every update advertises the complete data set");
      list->Release();
    }
  }
  if (manager->element != nullptr) {
    ITfCandidateListUIElementBehavior* behavior = nullptr;
    if (SUCCEEDED(manager->element->QueryInterface(
            IID_ITfCandidateListUIElementBehavior,
            reinterpret_cast<void**>(&behavior))) &&
        behavior != nullptr) {
      const int before = host.finals;
      Check(SUCCEEDED(behavior->Finalize()), "ui: Finalize with no selection is accepted");
      Check(host.finals == before,
            "ui: Finalize with no selection never accepts the literal row implicitly");
      behavior->Release();
    }
  }

  if (manager->element != nullptr) {
    ITfCandidateListUIElementBehavior* behavior = nullptr;
    if (SUCCEEDED(manager->element->QueryInterface(
            IID_ITfCandidateListUIElementBehavior,
            reinterpret_cast<void**>(&behavior))) &&
        behavior != nullptr) {
      Check(SUCCEEDED(behavior->SetSelection(2)), "ui: host-drawn SetSelection accepted");
      Check(host.selections == 1 && host.last.snapshot_version == 9 &&
                host.last.context_id == 42 && host.last.generation == 7 &&
                host.last.page_index == 2,
            "ui: SetSelection carries the snapshot version and identity");
      Check(SUCCEEDED(behavior->Finalize()), "ui: host-drawn Finalize accepted");
      Check(host.finals == 1 && host.last.page_index == 2,
            "ui: Finalize reports the selected index for validation");
      Check(SUCCEEDED(behavior->Abort()), "ui: host-drawn Abort accepted");
      Check(host.aborts == 1, "ui: Abort is delivered");
      behavior->Release();
    } else {
      Check(false, "ui: the element answers the behavior interface");
    }
  }

  // ITfUIElement::Show is the host's live visibility control. FALSE must hide the real
  // window and IsShown must follow; a later TRUE may be honoured after the controller
  // re-reads the anchor.
  if (manager->element != nullptr) {
    Check(SUCCEEDED(manager->element->Show(FALSE)),
          "ui: the host can hide the TIP UI");
    BOOL shown = TRUE;
    Check(!ui.window_visible() && SUCCEEDED(manager->element->IsShown(&shown)) &&
              shown == FALSE,
          "ui: Show(FALSE) hides the window and IsShown reports it");
    const int updates_before = manager->updates;
    ui.Update(snapshot, nullptr, &anchor, 1);
    Check(manager->updates == updates_before + 1 && !ui.window_visible(),
          "ui: a data update after Show(FALSE) never reopens the window");
    Check(SUCCEEDED(manager->element->Show(TRUE)),
          "ui: the host can hand presentation back to the TIP");
    Check(host.show_requests == 1,
          "ui: Show(TRUE) asks the controller for a fresh presentation");
    ui.Update(snapshot, nullptr, &anchor, 1);
    Check(ui.window_visible(), "ui: the next update presents after Show(TRUE)");
    Check(SUCCEEDED(manager->element->IsShown(&shown)) && shown == TRUE,
          "ui: IsShown follows the visible window again");
  }

  ui.End();
  Check(manager->ends == 1, "ui: EndUIElement issued exactly once");
  Check(!ui.begun() && !ui.window_visible(), "ui: End hides the window");

  // The manager may legally hold the element after EndUIElement. Pure queries must stay
  // valid; a selection arriving only after Stop must not reach the controller.
  if (manager->element != nullptr) {
    ITfCandidateListUIElement* list = nullptr;
    if (SUCCEEDED(manager->element->QueryInterface(
            IID_ITfCandidateListUIElement, reinterpret_cast<void**>(&list))) &&
        list != nullptr) {
      UINT count = 0;
      Check(SUCCEEDED(list->GetCount(&count)) && count == 8,
            "ui: an element held after End still answers GetCount");
      list->Release();
    }
  }

  // Host-drawn: BeginUIElement answers FALSE; no floating window may appear, and the TIP
  // must send the initial UpdateUIElement the UILess contract requires.
  manager->show = false;
  const int begin_updates = manager->updates;
  ui.Update(snapshot, nullptr, &anchor, 0);
  Check(ui.begun() && ui.host_drawn(),
        "ui: the host's drawing decision is honoured");
  Check(manager->show_on_entry == TRUE,
        "ui: the host receives the TIP's initial pbShow=TRUE");
  Check(manager->updates == begin_updates + 1,
        "ui: BeginUIElement FALSE is followed by one full UpdateUIElement");
  Check(!ui.window_visible(), "ui: no second window when the host draws");
  if (manager->element != nullptr) {
    BOOL shown = TRUE;
    Check(SUCCEEDED(manager->element->IsShown(&shown)) && shown == FALSE,
          "ui: a host-drawn element reports no visible TIP window");
    ui.Update(snapshot, nullptr, &anchor, 1);
    Check(manager->updates == begin_updates + 2 && !ui.window_visible(),
          "ui: host-drawn data updates never create a TIP window");
    Check(SUCCEEDED(manager->element->Show(TRUE)) && host.show_requests == 2,
          "ui: the host can take the TIP UI back after host drawing");
    ui.Update(snapshot, nullptr, &anchor, 1);
    Check(ui.window_visible(),
          "ui: Show(TRUE) after host drawing presents the TIP window");
    Check(SUCCEEDED(manager->element->Show(FALSE)) && !ui.window_visible(),
          "ui: another Show(FALSE) hides it again");
  }
  ui.End();
  Check(manager->ends == 2, "ui: host-drawn element ended");

  // Host-initiated end: the advised sink's EndUIElement must hide the window and make a
  // later End harmless.
  manager->show = true;
  ui.Update(snapshot, nullptr, &anchor, 0);
  Check(manager->sink != nullptr && ui.window_visible(),
        "ui: the element sink is advised and the window is visible");
  if (manager->sink != nullptr) {
    Check(SUCCEEDED(manager->sink->EndUIElement(1)),
          "ui: the host can end the element");
    Check(!ui.begun() && !ui.window_visible(),
          "ui: a host end hides the window");
  }
  ui.End();
  ui.Stop();
  if (manager->element != nullptr) {
    ITfCandidateListUIElementBehavior* behavior = nullptr;
    if (SUCCEEDED(manager->element->QueryInterface(
            IID_ITfCandidateListUIElementBehavior,
            reinterpret_cast<void**>(&behavior))) &&
        behavior != nullptr) {
      const int before = host.selections + host.finals;
      Check(SUCCEEDED(behavior->SetSelection(1)),
            "ui: a late element callback is accepted without a product behind it");
      Check(host.selections + host.finals == before,
            "ui: a callback after Stop cannot reach the controller");
      behavior->Release();
    }
  }
  manager->Release();
  Check(true, "ui: teardown completed without a crash");
}

void TestAnchorPolicy() {
  using epocanva::tsf::product::CandidateAnchorDecision;
  using epocanva::tsf::product::CandidateAnchorRetryBudget;
  using epocanva::tsf::product::CheckCandidateAnchor;
  using epocanva::tsf::product::kCandidateAnchorMaxRetries;

  // The token starts at 6 and has three host UTF-16 units. The engine snapshot caret may
  // lag at 8; only the real end 9 is the same token.
  auto check = CheckCandidateAnchor(true, 6, 3, 9);
  Check(check.decision == CandidateAnchorDecision::kAccept &&
            check.expected_caret_utf16 == 9,
        "anchor: the live caret at the token end is accepted (8 -> 9 lag)");
  check = CheckCandidateAnchor(true, 6, 3, 3);
  Check(check.decision == CandidateAnchorDecision::kCaretMismatch &&
            check.expected_caret_utf16 == 9,
        "anchor: the same text at another caret is a different token");
  check = CheckCandidateAnchor(true, 6, 3, 20);
  Check(check.decision == CandidateAnchorDecision::kCaretMismatch,
        "anchor: an arbitrary current cursor never retargets the menu");
  check = CheckCandidateAnchor(false, -1, 3, 9);
  Check(check.decision == CandidateAnchorDecision::kUnknownToken,
        "anchor: an unknown token origin fails closed");
  check = CheckCandidateAnchor(true, 6, 0, 6);
  Check(check.decision == CandidateAnchorDecision::kUnknownToken,
        "anchor: an empty token has no menu anchor");

  CandidateAnchorRetryBudget budget;
  bool all = true;
  for (int attempt = 0; attempt < kCandidateAnchorMaxRetries; ++attempt) {
    all = budget.TryAcquire() && all;
  }
  Check(all && budget.used() == kCandidateAnchorMaxRetries && budget.exhausted(),
        "anchor: the per-token retry budget grants exactly 40 attempts");
  Check(!budget.TryAcquire(),
        "anchor: the 41st layout retry is refused");
  budget.Reset();
  Check(!budget.exhausted() && budget.used() == 0 && budget.TryAcquire(),
        "anchor: a new token resets the retry budget");
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path screenshot;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--screenshot" && index + 1 < argc) {
      screenshot = std::filesystem::path(argv[++index]);
    }
  }
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::cout << "EpoCanva candidate window / UIElement controlled tests\n";
  TestLayout();
  TestWindow(screenshot);
  TestAnchorPolicy();
  TestUiLifecycle();
  if (g_failures != 0) {
    std::cout << "FAILED: " << g_failures << " check(s)\n";
    return 14;
  }
  std::cout << "RESULT PASS\n";
  return 0;
}
