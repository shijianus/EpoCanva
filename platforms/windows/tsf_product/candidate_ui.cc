// EpoCanva W2-B -- TSF candidate UI element and window. See candidate_ui.h.
#include "candidate_ui.h"

#include <oleauto.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <utility>

#include "module_lifetime.h"

namespace epocanva {
namespace tsf {
namespace product {
namespace {

constexpr GUID kCandidateUiGuid = {
    0x4f2d0e0b,
    0x7a1c,
    0x4a9e,
    {0x9b, 0x6d, 0x1e, 0x2f, 0x3c, 0x5a, 0x7e, 0x10}};

const wchar_t kElementDescription[] = L"EpoCanva candidates";

#ifdef EPOCANVA_PRODUCT_TRACE
void TraceStage(const char* stage, long long value) {
  char line[192];
  std::snprintf(line, sizeof(line), "EPOCANVA_TRACE|%s|%lld\n", stage, value);
  ::OutputDebugStringA(line);
}
#else
inline void TraceStage(const char*, long long) {}
#endif

std::wstring WidenAscii(const char* text) {
  std::wstring wide;
  if (text == nullptr) {
    return wide;
  }
  while (*text != '\0') {
    wide.push_back(static_cast<wchar_t>(*text++));
  }
  return wide;
}

}  // namespace

struct CandidateUiBridge {
  std::mutex mutex;
  CandidateUi* ui = nullptr;
  CandidateUiHost* host = nullptr;
  std::uint64_t version = 0;
  std::uint64_t context_id = 0;
  std::uint64_t generation = 0;
  LONG caret_utf16 = 0;
  int last_selected = -1;
  bool active = true;
};

class CandidateUiElement final : public ITfCandidateListUIElementBehavior {
 public:
  CandidateUiElement(std::shared_ptr<CandidateUiBridge> bridge,
                     std::uint64_t version)
      : bridge_(std::move(bridge)), version_(version) {}

  void SetData(const std::vector<std::wstring>& strings, int selected,
               ITfDocumentMgr* document, DWORD flags, std::uint64_t context_id,
               std::uint64_t generation, LONG caret_utf16) {
    strings_ = strings;
    selected_ = selected;
    context_id_ = context_id;
    generation_ = generation;
    caret_utf16_ = caret_utf16;
    if (document_ != nullptr) {
      document_->Release();
      document_ = nullptr;
    }
    if (document != nullptr) {
      document->AddRef();
      document_ = document;
    }
    flags_ = flags;
  }

  void SetSelected(int selected) { selected_ = selected; }

  void Invalidate() {
    if (document_ != nullptr) {
      document_->Release();
      document_ = nullptr;
    }
    flags_ = TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION |
             TF_CLUIE_STRING | TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) {
      return E_INVALIDARG;
    }
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfUIElement ||
        riid == IID_ITfCandidateListUIElement ||
        riid == IID_ITfCandidateListUIElementBehavior) {
      *object = static_cast<ITfCandidateListUIElementBehavior*>(this);
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

  HRESULT STDMETHODCALLTYPE GetDescription(BSTR* description) override {
    if (description == nullptr) {
      return E_INVALIDARG;
    }
    *description = ::SysAllocString(kElementDescription);
    return *description != nullptr ? S_OK : E_OUTOFMEMORY;
  }

  HRESULT STDMETHODCALLTYPE GetGUID(GUID* guid) override {
    if (guid == nullptr) {
      return E_INVALIDARG;
    }
    *guid = kCandidateUiGuid;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Show(BOOL show) override {
    CandidateUi* ui = nullptr;
    {
      std::shared_ptr<CandidateUiBridge> bridge = bridge_;
      if (bridge) {
        std::lock_guard<std::mutex> guard(bridge->mutex);
        ui = bridge->ui;
      }
    }
    if (ui != nullptr) {
      ui->OnElementShow(show != FALSE);
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE IsShown(BOOL* show) override {
    if (show == nullptr) {
      return E_INVALIDARG;
    }
    CandidateUi* ui = nullptr;
    {
      std::shared_ptr<CandidateUiBridge> bridge = bridge_;
      if (bridge) {
        std::lock_guard<std::mutex> guard(bridge->mutex);
        ui = bridge->ui;
      }
    }
    *show = ui != nullptr && ui->ElementShown() ? TRUE : FALSE;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD* flags) override {
    if (flags == nullptr) {
      return E_INVALIDARG;
    }
    *flags = flags_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr** document) override {
    if (document == nullptr) {
      return E_INVALIDARG;
    }
    *document = document_;
    if (document_ != nullptr) {
      document_->AddRef();
      return S_OK;
    }
    return E_FAIL;
  }

  HRESULT STDMETHODCALLTYPE GetCount(UINT* count) override {
    if (count == nullptr) {
      return E_INVALIDARG;
    }
    *count = static_cast<UINT>(strings_.size());
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetSelection(UINT* index) override {
    if (index == nullptr) {
      return E_INVALIDARG;
    }
    if (selected_ < 0 || strings_.empty()) {
      // UILess contract: S_FALSE means the list has no selection and *puIndex is invalid.
      *index = 0;
      return S_FALSE;
    }
    *index = static_cast<UINT>(selected_);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetString(UINT index, BSTR* string) override {
    if (string == nullptr) {
      return E_INVALIDARG;
    }
    *string = nullptr;
    if (index >= strings_.size()) {
      return E_INVALIDARG;
    }
    *string = ::SysAllocString(strings_[index].c_str());
    return *string != nullptr ? S_OK : E_OUTOFMEMORY;
  }

  HRESULT STDMETHODCALLTYPE GetPageIndex(UINT* index, UINT size,
                                        UINT* page_count) override {
    if (page_count == nullptr) {
      return E_INVALIDARG;
    }
    // The pinned product schema fits every menu on one page (1 raw + up to 7 completions,
    // or 1 raw + up to 3 suggestions). The element exposes exactly the engine page it was
    // given and never invents additional pages.
    *page_count = 1;
    if (size >= 1 && index != nullptr) {
      index[0] = 0;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetPageIndex(UINT* index, UINT page_count) override {
    (void)index;
    return page_count == 1 ? S_OK : E_INVALIDARG;
  }

  HRESULT STDMETHODCALLTYPE GetCurrentPage(UINT* page) override {
    if (page == nullptr) {
      return E_INVALIDARG;
    }
    *page = 0;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetSelection(UINT index) override {
    if (index >= strings_.size()) {
      return E_INVALIDARG;
    }
    selected_ = static_cast<int>(index);
    CandidateUi* ui = nullptr;
    {
      std::shared_ptr<CandidateUiBridge> bridge = bridge_;
      if (bridge) {
        std::lock_guard<std::mutex> guard(bridge->mutex);
        ui = bridge->ui;
      }
    }
    if (ui != nullptr) {
      CandidateUiSelection selection;
      selection.snapshot_version = version_;
      selection.context_id = context_id_;
      selection.generation = generation_;
      selection.page_index = static_cast<int>(index);
      selection.caret_utf16 = caret_utf16_;
      ui->SelectionFromElement(selection);
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Finalize() override {
    if (selected_ < 0) {
      // GetSelection answered S_FALSE: there is no explicit selection to accept. The
      // literal row is never implied just because the host finalized the element.
      return S_OK;
    }
    CandidateUi* ui = nullptr;
    {
      std::shared_ptr<CandidateUiBridge> bridge = bridge_;
      if (bridge) {
        std::lock_guard<std::mutex> guard(bridge->mutex);
        ui = bridge->ui;
      }
    }
    if (ui != nullptr) {
      CandidateUiSelection selection;
      selection.snapshot_version = version_;
      selection.context_id = context_id_;
      selection.generation = generation_;
      selection.page_index = selected_;
      selection.caret_utf16 = caret_utf16_;
      ui->FinalizeFromElement(selection);
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Abort() override {
    CandidateUi* ui = nullptr;
    {
      std::shared_ptr<CandidateUiBridge> bridge = bridge_;
      if (bridge) {
        std::lock_guard<std::mutex> guard(bridge->mutex);
        ui = bridge->ui;
      }
    }
    if (ui != nullptr) {
      ui->AbortFromElement(version_, context_id_, generation_);
    }
    return S_OK;
  }

 private:
  ~CandidateUiElement() = default;

  std::atomic<LONG> reference_count_{1};
  std::shared_ptr<CandidateUiBridge> bridge_;
  std::uint64_t version_ = 0;
  std::uint64_t context_id_ = 0;
  std::uint64_t generation_ = 0;
  LONG caret_utf16_ = 0;
  std::vector<std::wstring> strings_;
  ITfDocumentMgr* document_ = nullptr;
  DWORD flags_ = 0;
  int selected_ = -1;
  // The UI element manager may keep the element after EndUIElement; the module stays
  // loaded until its last Release reaches this object.
  epocanva::tsf::edit::ModuleObjectGuard module_guard_{
      epocanva::tsf::edit::ModuleObjectKind::kCandidateElement};
};

namespace {

class UiElementSink final : public ITfUIElementSink {
 public:
  explicit UiElementSink(std::shared_ptr<CandidateUiBridge> bridge)
      : bridge_(std::move(bridge)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) {
      return E_INVALIDARG;
    }
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfUIElementSink) {
      *object = static_cast<ITfUIElementSink*>(this);
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
  HRESULT STDMETHODCALLTYPE BeginUIElement(DWORD element_id, BOOL* show) override {
    (void)element_id;
    (void)show;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE UpdateUIElement(DWORD element_id) override {
    CandidateUi* ui = nullptr;
    {
      std::shared_ptr<CandidateUiBridge> bridge = bridge_;
      if (bridge) {
        std::lock_guard<std::mutex> guard(bridge->mutex);
        ui = bridge->ui;
      }
    }
    if (ui != nullptr) {
      ui->OnHostUpdate(element_id);
    }
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE EndUIElement(DWORD element_id) override {
    CandidateUi* ui = nullptr;
    {
      std::shared_ptr<CandidateUiBridge> bridge = bridge_;
      if (bridge) {
        std::lock_guard<std::mutex> guard(bridge->mutex);
        ui = bridge->ui;
      }
    }
    if (ui != nullptr) {
      ui->OnHostEnd(element_id);
    }
    return S_OK;
  }

 private:
  ~UiElementSink() = default;
  std::atomic<LONG> reference_count_{1};
  std::shared_ptr<CandidateUiBridge> bridge_;
  epocanva::tsf::edit::ModuleObjectGuard module_guard_{
      epocanva::tsf::edit::ModuleObjectKind::kCandidateSink};
};

class CaretReadCallback final : public ITfEditSession {
 public:
  CaretReadCallback(ITfContext* context, LONG expected_caret, LONG token_units,
                    bool want_rect)
      : context_(context),
        expected_caret_(expected_caret),
        token_units_(token_units),
        want_rect_(want_rect) {
    context_->AddRef();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) {
      return E_INVALIDARG;
    }
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfEditSession) {
      *object = static_cast<ITfEditSession*>(this);
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

  HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie cookie) override {
    ran_ = true;
    TF_SELECTION selection = {};
    ULONG fetched = 0;
    if (FAILED(context_->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &selection,
                                      &fetched)) ||
        fetched != 1 || selection.range == nullptr) {
      if (selection.range != nullptr) {
        selection.range->Release();
      }
      return S_OK;
    }
    ITfRangeACP* acp = nullptr;
    const HRESULT qi =
        selection.range->QueryInterface(IID_ITfRangeACP, reinterpret_cast<void**>(&acp));
    if (FAILED(qi) || acp == nullptr) {
      selection.range->Release();
      return S_OK;
    }
    LONG caret = 0;
    LONG length = 0;
    const HRESULT extent = acp->GetExtent(&caret, &length);
    if (FAILED(extent) || length != 0 || caret < 0) {
      acp->Release();
      selection.range->Release();
      return S_OK;
    }
    caret_ = caret;
    caret_ok_ = true;
    if (!want_rect_) {
      acp->Release();
      selection.range->Release();
      return S_OK;
    }
    if (expected_caret_ != caret || token_units_ <= 0 || caret < token_units_) {
      // The caret moved since the menu was drawn, or there is no token range to read. No
      // rectangle is returned: the window hides instead of pointing at a guessed position.
      acp->Release();
      selection.range->Release();
      return S_OK;
    }
    const LONG start = caret - token_units_;
    const HRESULT token_extent = acp->SetExtent(start, token_units_);
    acp->Release();
    if (FAILED(token_extent)) {
      selection.range->Release();
      return S_OK;
    }
    ITfContextView* view = nullptr;
    if (FAILED(context_->GetActiveView(&view)) || view == nullptr) {
      selection.range->Release();
      return S_OK;
    }
    RECT rect = {};
    BOOL clipped = FALSE;
    const HRESULT text_ext = view->GetTextExt(cookie, selection.range, &rect, &clipped);
    view->Release();
    selection.range->Release();
    if (SUCCEEDED(text_ext) && rect.right > rect.left && rect.bottom > rect.top) {
      rect_ = rect;
      clipped_ = clipped != FALSE;
      ok_ = true;
    }
    return S_OK;
  }

  bool ran() const { return ran_; }
  bool caret_ok() const { return caret_ok_; }
  LONG caret() const { return caret_; }
  bool ok() const { return ok_; }
  RECT rect() const { return rect_; }
  bool clipped() const { return clipped_; }

 private:
  ~CaretReadCallback() {
    if (context_ != nullptr) {
      context_->Release();
    }
  }

  std::atomic<LONG> reference_count_{1};
  ITfContext* context_ = nullptr;
  LONG expected_caret_ = 0;
  LONG token_units_ = 0;
  bool want_rect_ = false;
  bool ran_ = false;
  bool caret_ok_ = false;
  LONG caret_ = 0;
  bool ok_ = false;
  bool clipped_ = false;
  RECT rect_{};
  epocanva::tsf::edit::ModuleObjectGuard module_guard_{
      epocanva::tsf::edit::ModuleObjectKind::kRectRead};
};

bool RunCaretRead(ITfContext* context, TfClientId client_id, LONG expected_caret,
                  LONG token_units, bool want_rect, LONG* caret_out, RECT* rect,
                  bool* clipped, std::wstring* why) {
  if (context == nullptr || client_id == 0) {
    if (why != nullptr) {
      *why = L"no context or client for the caret read";
    }
    return false;
  }
  CaretReadCallback* callback =
      new CaretReadCallback(context, expected_caret, token_units, want_rect);
  HRESULT session_hr = S_OK;
  const HRESULT request = context->RequestEditSession(
      client_id, callback, TF_ES_SYNC | TF_ES_READ, &session_hr);
  const bool ran = callback->ran();
  const bool caret_ok = callback->caret_ok();
  const LONG caret = callback->caret();
  const bool ok = callback->ok();
  const RECT read_rect = callback->rect();
  const bool read_clipped = callback->clipped();
  callback->Release();
  if (FAILED(request) || session_hr == TF_E_SYNCHRONOUS || FAILED(session_hr) || !ran ||
      !caret_ok) {
    if (why != nullptr) {
      *why = L"the caret could not be read";
    }
    return false;
  }
  if (caret_out != nullptr) {
    *caret_out = caret;
  }
  if (!want_rect) {
    return true;
  }
  if (!ok) {
    if (why != nullptr) {
      *why = L"the token rectangle is unavailable at this caret";
    }
    return false;
  }
  *rect = read_rect;
  *clipped = read_clipped;
  return true;
}

}  // namespace

bool ReadCaretPosition(ITfContext* context, TfClientId client_id, LONG* caret_utf16,
                       std::wstring* why) {
  if (caret_utf16 == nullptr) {
    return false;
  }
  return RunCaretRead(context, client_id, -1, 0, false, caret_utf16, nullptr, nullptr,
                      why);
}

bool ReadTokenRect(ITfContext* context, TfClientId client_id, LONG caret_utf16,
                   LONG token_units_utf16, RECT* rect, bool* clipped,
                   LONG* observed_caret_utf16, std::wstring* why) {
  if (observed_caret_utf16 != nullptr) {
    *observed_caret_utf16 = -1;
  }
  if (rect == nullptr || clipped == nullptr) {
    return false;
  }
  if (caret_utf16 < 0 || token_units_utf16 <= 0) {
    if (why != nullptr) {
      *why = L"no valid token range";
    }
    return false;
  }
  return RunCaretRead(context, client_id, caret_utf16, token_units_utf16, true,
                      observed_caret_utf16, rect, clipped, why);
}

CandidateUi::CandidateUi(TfClientId client_id, CandidateUiHost* host, HMODULE module,
                         CandidateDisplayPreference preference)
    : client_id_(client_id),
      host_(host),
      module_(module),
      preference_(preference),
      window_(module,
              [this](std::size_t index) {
                ++diagnostics_.clicks;
                CandidateUiSelection selection;
                if (bridge_) {
                  std::lock_guard<std::mutex> guard(bridge_->mutex);
                  selection.snapshot_version = bridge_->version;
                  selection.context_id = bridge_->context_id;
                  selection.generation = bridge_->generation;
                  selection.caret_utf16 = bridge_->caret_utf16;
                }
                selection.page_index = static_cast<int>(index);
                if (host_ != nullptr) {
                  host_->OnUiFinalize(selection);
                }
              },
              [this]() {
                if (host_ != nullptr) {
                  host_->OnUiAbort(0, 0, 0);
                }
              }) {}

CandidateUi::~CandidateUi() { Stop(); }

bool CandidateUi::Start(ITfThreadMgr* thread_manager, std::wstring* why) {
  if (thread_manager == nullptr || host_ == nullptr || module_ == nullptr) {
    if (why != nullptr) {
      *why = L"the candidate UI needs a thread manager, host and module";
    }
    return false;
  }
#ifdef EPOCANVA_TSF_DIRECT_SINK_TEST
  // Controlled direct-sink tests exercise candidate state and selection through the real
  // coordinator, but they do not own a real TSF UI-element manager. The UIElement/window
  // lifecycle has its own controlled test; this variant never presents a floating window
  // that is not attached to a registered TSF context.
  (void)thread_manager;
  no_manager_self_draw_ = false;
  PrepareBridge();
  diagnostics_.tsf_manager = false;
  return true;
#else
  ITfUIElementMgr* manager = nullptr;
  const HRESULT qi = thread_manager->QueryInterface(
      IID_ITfUIElementMgr, reinterpret_cast<void**>(&manager));
  if (FAILED(qi) || manager == nullptr) {
    // No UI element manager: the host cannot present the candidate element at all. TSF
    // allows the TIP to draw its own window in this case (state C); there is no host
    // visibility decision to follow. The window still anchors only to a real token
    // rectangle read from the live context.
    diagnostics_.tsf_manager = false;
    PrepareBridge();
    return true;
  }
  ITfSource* source = nullptr;
  if (SUCCEEDED(manager->QueryInterface(IID_ITfSource,
                                        reinterpret_cast<void**>(&source))) &&
      source != nullptr) {
    const bool attached = AttachManager(manager, source, why);
    source->Release();
    manager->Release();
    return attached;
  }
  const bool attached = AttachManager(manager, nullptr, why);
  manager->Release();
  return attached;
#endif  // EPOCANVA_TSF_DIRECT_SINK_TEST
}

bool CandidateUi::StartWithManagerForTest(ITfUIElementMgr* manager, ITfSource* source,
                                          std::wstring* why) {
  if (host_ == nullptr || module_ == nullptr) {
    if (why != nullptr) {
      *why = L"the candidate UI test start needs a host and module";
    }
    return false;
  }
  if (manager == nullptr) {
    // Controlled state C: the thread has no UI element manager.
    PrepareBridge();
    diagnostics_.tsf_manager = false;
    return true;
  }
  return AttachManager(manager, source, why);
}

void CandidateUi::PrepareBridge() {
  if (bridge_) {
    return;
  }
  bridge_ = std::make_shared<CandidateUiBridge>();
  bridge_->ui = this;
  bridge_->host = host_;
}

bool CandidateUi::AttachManager(ITfUIElementMgr* manager, ITfSource* source,
                                std::wstring* why) {
  (void)why;
  if (manager == nullptr) {
    return false;
  }
  manager_ = manager;
  manager_->AddRef();
  diagnostics_.tsf_manager = true;
  PrepareBridge();
  if (source != nullptr) {
    source_ = source;
    source_->AddRef();
    sink_ = new UiElementSink(bridge_);
    if (FAILED(source_->AdviseSink(IID_ITfUIElementSink, sink_, &sink_cookie_))) {
      sink_cookie_ = 0;
      sink_->Release();
      sink_ = nullptr;
    } else {
      diagnostics_.sink_advised = true;
    }
  }
  return true;
}

void CandidateUi::Stop() {
  End();
  if (bridge_) {
    // No late sink or element callback may reach this object after this point.
    std::lock_guard<std::mutex> guard(bridge_->mutex);
    bridge_->ui = nullptr;
    bridge_->host = nullptr;
    bridge_->active = false;
  }
  if (source_ != nullptr) {
    if (sink_cookie_ != 0) {
      (void)source_->UnadviseSink(sink_cookie_);
      sink_cookie_ = 0;
    }
    source_->Release();
    source_ = nullptr;
  }
  if (sink_ != nullptr) {
    sink_->Release();
    sink_ = nullptr;
  }
  if (manager_ != nullptr) {
    manager_->Release();
    manager_ = nullptr;
  }
  window_.Destroy();
  diagnostics_.window_created = false;
  diagnostics_.window_visible = false;
}

void CandidateUi::End() {
  if (manager_ != nullptr && begun_ && element_id_ != 0) {
    const HRESULT ended = manager_->EndUIElement(element_id_);
    if (FAILED(ended)) {
      // The host may already have ended it. The element is inactive either way.
      ++diagnostics_.ends;
    } else {
      ++diagnostics_.ends;
    }
  }
  begun_ = false;
  host_drawn_ = false;
  host_show_false_ = false;
  tip_ui_allowed_ = false;
  element_id_ = 0;
  ReleaseElement();
  window_.Hide();
  diagnostics_.begun = false;
  diagnostics_.host_drawn = false;
  diagnostics_.window_visible = false;
}

void CandidateUi::OnElementShow(bool show) {
  if (!show) {
    ++diagnostics_.show_false_notifications;
    if (preference_ == CandidateDisplayPreference::kAlwaysSelf) {
      ++diagnostics_.host_show_false_suppressions;
    }
    // An explicit Show(FALSE) is the host withdrawing presentation now. It wins over the
    // user's always_self preference and suppresses the window until a new element begins
    // or the host hands presentation back with Show(TRUE).
    host_show_false_ = true;
    tip_ui_allowed_ = false;
    window_.Hide();
    diagnostics_.window_visible = false;
    return;
  }
  ++diagnostics_.show_true_notifications;
  host_show_false_ = false;
  // Show(TRUE) is the host handing presentation back to the TIP, including after a
  // pbShow=FALSE Begin. The next data update may present.
  host_drawn_ = false;
  tip_ui_allowed_ = true;
  // Re-read the live anchor on the next paced wake; this COM callback never starts a TSF
  // edit session or blocks on layout.
  if (host_ != nullptr) {
    host_->OnUiShowRequested();
  }
}

bool CandidateUi::ElementShown() const { return window_.visible(); }

bool CandidateUi::self_draw_permitted() const {
  if (manager_ == nullptr) {
    return no_manager_self_draw_;
  }
  if (host_show_false_) {
    return false;
  }
  if (!begun_) {
    // Before BeginUIElement the host decision is unknown; read the anchor so the first
    // update can present immediately when the host answers TRUE.
    return true;
  }
  if (!host_drawn_) {
    return true;
  }
  return preference_ == CandidateDisplayPreference::kAlwaysSelf;
}

void CandidateUi::ReleaseElement() {
  if (element_ != nullptr) {
    element_->Invalidate();
    element_->Release();
    element_ = nullptr;
  }
  if (bridge_) {
    std::lock_guard<std::mutex> guard(bridge_->mutex);
    bridge_->version = 0;
  }
}

void CandidateUi::Update(const coordinator::CandidateSnapshot& snapshot,
                         ITfContext* context, const RECT* anchor, int selected) {
  if (bridge_ == nullptr) {
    return;
  }
  if (manager_ == nullptr && !no_manager_self_draw_) {
    // Controlled direct-sink test variant: no real TSF UI element manager and no window.
    // Keep the pre-W3-B no-op path so those coordinator/selection tests see exactly the
    // same timing as before; state C (no manager in a real host) still presents.
    return;
  }
  if (!snapshot.valid || snapshot.items.empty()) {
    End();
    return;
  }
  ++diagnostics_.updates;
  TraceStage("ui_update", static_cast<long long>(snapshot.items.size()));
  std::vector<std::wstring> strings;
  strings.reserve(snapshot.items.size());
  std::vector<CandidateWindowItem> window_items;
  window_items.reserve(snapshot.items.size());
  for (const coordinator::CandidateItem& item : snapshot.items) {
    strings.emplace_back(item.text_utf16_code_units.begin(),
                         item.text_utf16_code_units.end());
    CandidateWindowItem window_item;
    window_item.text = strings.back();
    window_item.kind = item.kind;
    window_items.push_back(std::move(window_item));
  }
  bool newly_begun = false;
  if (manager_ != nullptr) {
    ITfDocumentMgr* document = nullptr;
    if (context != nullptr) {
      (void)context->GetDocumentMgr(&document);
    }
    const DWORD flags = TF_CLUIE_DOCUMENTMGR | TF_CLUIE_COUNT | TF_CLUIE_SELECTION |
                        TF_CLUIE_STRING | TF_CLUIE_PAGEINDEX | TF_CLUIE_CURRENTPAGE;
    if (element_ == nullptr) {
      element_ = new CandidateUiElement(bridge_, snapshot.version);
    }
    if (!begun_) {
      element_->SetData(strings, selected, document, flags, snapshot.identity.context_id,
                        snapshot.identity.generation, snapshot.caret_utf16);
      BOOL show = TRUE;  // The TIP asks to present; the host writes its decision here.
      DWORD element_id = 0;
      const HRESULT began = manager_->BeginUIElement(element_, &show, &element_id);
      if (FAILED(began) || element_id == 0) {
        TraceStage("ui_begin_failed", static_cast<long long>(began));
        ++diagnostics_.begin_failures;
        ReleaseElement();
        if (document != nullptr) {
          document->Release();
        }
        window_.Hide();
        return;
      }
      ++diagnostics_.begins;
      begun_ = true;
      newly_begun = true;
      element_id_ = element_id;
      host_drawn_ = show == FALSE;
      tip_ui_allowed_ = !host_drawn_;
      TraceStage("ui_host_drawn", host_drawn_ ? 1 : 0);
    } else {
      element_->SetData(strings, selected, document, flags, snapshot.identity.context_id,
                        snapshot.identity.generation, snapshot.caret_utf16);
      const HRESULT updated = manager_->UpdateUIElement(element_id_);
      if (FAILED(updated)) {
        TraceStage("ui_update_failed", static_cast<long long>(updated));
        ++diagnostics_.update_failures;
        End();
        if (document != nullptr) {
          document->Release();
        }
        return;
      }
    }
    if (newly_begun && host_drawn_) {
      // UILess contract: after BeginUIElement answers FALSE the TIP must call
      // UpdateUIElement so the host can read the full element (all update flags are set).
      // Without this notification a host-drawn list has no data to draw. This is sent even
      // when the user's always_self preference also draws the TIP window.
      const HRESULT updated = manager_->UpdateUIElement(element_id_);
      if (FAILED(updated)) {
        TraceStage("ui_host_drawn_update_failed", static_cast<long long>(updated));
        ++diagnostics_.update_failures;
        End();
        if (document != nullptr) {
          document->Release();
        }
        return;
      }
      ++diagnostics_.host_drawn_updates;
    }
    if (document != nullptr) {
      document->Release();
    }
  }
  {
    std::lock_guard<std::mutex> guard(bridge_->mutex);
    bridge_->version = snapshot.version;
    bridge_->context_id = snapshot.identity.context_id;
    bridge_->generation = snapshot.identity.generation;
    bridge_->caret_utf16 = snapshot.caret_utf16;
    bridge_->last_selected = selected;
  }
  diagnostics_.begun = begun_;
  diagnostics_.host_drawn = host_drawn_;
  if (!self_draw_permitted()) {
    // The host claimed presentation and the user kept the default (or the host explicitly
    // called Show(FALSE)). The element, when it exists, stays registered and answers every
    // query; no second UI is drawn.
    if (manager_ != nullptr) {
      TraceStage("ui_host_drawn_honored", 1);
    }
    window_.Hide();
    diagnostics_.window_created = window_.created();
    diagnostics_.window_visible = false;
    return;
  }
  if (manager_ == nullptr) {
    ++diagnostics_.no_manager_draws;
    TraceStage("ui_no_manager_self_draw", 1);
  } else if (host_drawn_) {
    ++diagnostics_.always_self_draws;
    TraceStage("ui_always_self_draw", 1);
  }
  if (anchor == nullptr) {
    TraceStage("ui_no_anchor", 1);
    window_.Hide();
    diagnostics_.window_created = window_.created();
    diagnostics_.window_visible = false;
    ++diagnostics_.no_anchor_hides;
    return;
  }
  TraceStage("ui_show_attempt", 1);
  window_.Show(window_items, selected, snapshot.page_no, snapshot.page_size,
               snapshot.is_last_page, *anchor);
  TraceStage("ui_window_visible", window_.visible() ? 1 : 0);
  diagnostics_.window_created = window_.created();
  diagnostics_.window_visible = window_.visible();
}

void CandidateUi::SetSelection(int selected) {
  if (element_ != nullptr) {
    element_->SetSelected(selected);
    if (manager_ != nullptr && begun_ && element_id_ != 0) {
      (void)manager_->UpdateUIElement(element_id_);
    }
  }
  if (window_.created()) {
    window_.SetSelection(selected);
  }
}

void CandidateUi::OnHostBegin(DWORD element_id) { (void)element_id; }

void CandidateUi::OnHostUpdate(DWORD element_id) { (void)element_id; }

void CandidateUi::OnHostEnd(DWORD element_id) {
  if (!begun_ || element_id != element_id_) {
    return;
  }
  // TSF is calling us from EndUIElement. The element must stay alive until that call
  // returns, so only the presentation state is dropped here; the COM reference is released
  // by End()/Stop() after the manager call.
  begun_ = false;
  host_drawn_ = false;
  host_show_false_ = false;
  tip_ui_allowed_ = false;
  element_id_ = 0;
  window_.Hide();
  if (bridge_) {
    std::lock_guard<std::mutex> guard(bridge_->mutex);
    bridge_->version = 0;
  }
  diagnostics_.begun = false;
  diagnostics_.host_drawn = false;
  diagnostics_.window_visible = false;
}

void CandidateUi::FinalizeFromElement(const CandidateUiSelection& selection) {
  if (host_ != nullptr) {
    host_->OnUiFinalize(selection);
  }
}

void CandidateUi::SelectionFromElement(const CandidateUiSelection& selection) {
  ++diagnostics_.selected_changes;
  if (host_ != nullptr) {
    host_->OnUiSelectionChanged(selection);
  }
}

void CandidateUi::AbortFromElement(std::uint64_t snapshot_version,
                                   std::uint64_t context_id,
                                   std::uint64_t generation) {
  if (host_ != nullptr) {
    host_->OnUiAbort(snapshot_version, context_id, generation);
  }
}

void CandidateUi::HideWindowFromElement() { window_.Hide(); }

CandidateUiDiagnostics CandidateUi::Diagnostics() const {
  CandidateUiDiagnostics diagnostics = diagnostics_;
  diagnostics.begun = begun_;
  diagnostics.host_drawn = host_drawn_;
  diagnostics.window_visible = window_.visible();
  diagnostics.window_created = window_.created();
  return diagnostics;
}

}  // namespace product
}  // namespace tsf
}  // namespace epocanva
