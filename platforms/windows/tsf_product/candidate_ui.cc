// EpoCanva W2-B -- TSF candidate UI element and window. See candidate_ui.h.
#include "candidate_ui.h"

#include <oleauto.h>

#include <atomic>
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
               std::uint64_t generation) {
    strings_ = strings;
    selected_ = selected;
    context_id_ = context_id;
    generation_ = generation;
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
    shown_ = show != FALSE;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE IsShown(BOOL* show) override {
    if (show == nullptr) {
      return E_INVALIDARG;
    }
    *show = shown_ ? TRUE : FALSE;
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
      *index = 0;
    } else {
      *index = static_cast<UINT>(selected_);
    }
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
      ui->SelectionFromElement(selection);
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Finalize() override {
    const int selected = selected_ < 0 ? 0 : selected_;
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
      selection.page_index = selected;
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
  std::vector<std::wstring> strings_;
  ITfDocumentMgr* document_ = nullptr;
  DWORD flags_ = 0;
  int selected_ = -1;
  bool shown_ = true;
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

class RectReadCallback final : public ITfEditSession {
 public:
  RectReadCallback(ITfContext* context, LONG start, LONG length)
      : context_(context), start_(start), length_(length) {
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
    const HRESULT extent = acp->SetExtent(start_, length_);
    acp->Release();
    if (FAILED(extent)) {
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
  bool ok() const { return ok_; }
  RECT rect() const { return rect_; }
  bool clipped() const { return clipped_; }

 private:
  ~RectReadCallback() {
    if (context_ != nullptr) {
      context_->Release();
    }
  }

  std::atomic<LONG> reference_count_{1};
  ITfContext* context_ = nullptr;
  LONG start_ = 0;
  LONG length_ = 0;
  bool ran_ = false;
  bool ok_ = false;
  bool clipped_ = false;
  RECT rect_{};
  epocanva::tsf::edit::ModuleObjectGuard module_guard_{
      epocanva::tsf::edit::ModuleObjectKind::kRectRead};
};

}  // namespace

bool ReadCompositionRect(ITfContext* context, TfClientId client_id, LONG start_utf16,
                         LONG length_utf16, RECT* rect, bool* clipped,
                         std::wstring* why) {
  if (rect == nullptr || clipped == nullptr) {
    return false;
  }
  if (context == nullptr || client_id == 0 || start_utf16 < 0 || length_utf16 <= 0) {
    if (why != nullptr) {
      *why = L"no valid composition range";
    }
    return false;
  }
  RectReadCallback* callback = new RectReadCallback(context, start_utf16, length_utf16);
  HRESULT session_hr = S_OK;
  const HRESULT request = context->RequestEditSession(
      client_id, callback, TF_ES_SYNC | TF_ES_READ, &session_hr);
  const bool ran = callback->ran();
  const bool ok = callback->ok();
  const RECT read_rect = callback->rect();
  const bool read_clipped = callback->clipped();
  callback->Release();
  if (FAILED(request) || session_hr == TF_E_SYNCHRONOUS || FAILED(session_hr) || !ran ||
      !ok) {
    if (why != nullptr) {
      *why = L"the composition rectangle could not be read";
    }
    return false;
  }
  *rect = read_rect;
  *clipped = read_clipped;
  return true;
}

CandidateUi::CandidateUi(TfClientId client_id, CandidateUiHost* host, HMODULE module)
    : client_id_(client_id),
      host_(host),
      module_(module),
      window_(module,
              [this](std::size_t index) {
                ++diagnostics_.clicks;
                CandidateUiSelection selection;
                if (bridge_) {
                  std::lock_guard<std::mutex> guard(bridge_->mutex);
                  selection.snapshot_version = bridge_->version;
                  selection.context_id = bridge_->context_id;
                  selection.generation = bridge_->generation;
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
  diagnostics_.tsf_manager = false;
  return true;
#else
  ITfUIElementMgr* manager = nullptr;
  const HRESULT qi = thread_manager->QueryInterface(
      IID_ITfUIElementMgr, reinterpret_cast<void**>(&manager));
  if (FAILED(qi) || manager == nullptr) {
    // A controlled direct-sink host may have no UI element manager. The controller then
    // runs without TSF candidate presentation; it never falls back to a floating window
    // that is not attached to a real text range.
    diagnostics_.tsf_manager = false;
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
  if (manager == nullptr || host_ == nullptr || module_ == nullptr) {
    if (why != nullptr) {
      *why = L"the candidate UI test start needs a manager, host and module";
    }
    return false;
  }
  return AttachManager(manager, source, why);
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
  bridge_ = std::make_shared<CandidateUiBridge>();
  bridge_->ui = this;
  bridge_->host = host_;
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
  element_id_ = 0;
  ReleaseElement();
  window_.Hide();
  diagnostics_.begun = false;
  diagnostics_.host_drawn = false;
  diagnostics_.window_visible = false;
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
  if (manager_ == nullptr || bridge_ == nullptr) {
    return;
  }
  if (!snapshot.valid || snapshot.items.empty()) {
    End();
    return;
  }
  ++diagnostics_.updates;
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
                      snapshot.identity.generation);
    BOOL show = TRUE;
    DWORD element_id = 0;
    const HRESULT began = manager_->BeginUIElement(element_, &show, &element_id);
    if (FAILED(began) || element_id == 0) {
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
    element_id_ = element_id;
    host_drawn_ = show == FALSE;
    std::lock_guard<std::mutex> guard(bridge_->mutex);
    bridge_->version = snapshot.version;
    bridge_->context_id = snapshot.identity.context_id;
    bridge_->generation = snapshot.identity.generation;
    bridge_->last_selected = selected;
  } else {
    element_->SetData(strings, selected, document, flags, snapshot.identity.context_id,
                      snapshot.identity.generation);
    const HRESULT updated = manager_->UpdateUIElement(element_id_);
    if (FAILED(updated)) {
      ++diagnostics_.update_failures;
      End();
      if (document != nullptr) {
        document->Release();
      }
      return;
    }
    std::lock_guard<std::mutex> guard(bridge_->mutex);
    bridge_->version = snapshot.version;
    bridge_->context_id = snapshot.identity.context_id;
    bridge_->generation = snapshot.identity.generation;
    bridge_->last_selected = selected;
  }
  if (document != nullptr) {
    document->Release();
  }
  diagnostics_.begun = begun_;
  diagnostics_.host_drawn = host_drawn_;
  if (host_drawn_) {
    // TSF told us the host draws this element. Never create a second, floating list.
    window_.Hide();
    diagnostics_.window_visible = false;
    return;
  }
  if (anchor == nullptr) {
    window_.Hide();
    diagnostics_.window_visible = false;
    ++diagnostics_.no_anchor_hides;
    return;
  }
  window_.Show(window_items, selected, snapshot.page_no, snapshot.page_size,
               snapshot.is_last_page, *anchor);
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
