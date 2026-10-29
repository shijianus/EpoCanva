// EpoCanva P3.7 -- implementation of the real TSF test host.
#include "test_host.h"

#include <objbase.h>

#include <cstdio>
#include <limits>
#include <utility>

namespace epocanva {
namespace tsf {
namespace edit {
namespace test {
namespace {

constexpr wchar_t kHostWindowClass[] = L"EpoCanvaTsfEditTestHostWindow";
constexpr UINT kGrantLockMessage = WM_APP + 0x4A;

bool IsHighSurrogate(wchar_t unit) {
  return unit >= 0xD800 && unit <= 0xDBFF;
}

bool IsLowSurrogate(wchar_t unit) {
  return unit >= 0xDC00 && unit <= 0xDFFF;
}

bool IsWellFormedUtf16(const std::wstring& text) {
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (IsHighSurrogate(text[index])) {
      if (index + 1 >= text.size() || !IsLowSurrogate(text[index + 1])) {
        return false;
      }
      ++index;
    } else if (IsLowSurrogate(text[index])) {
      return false;
    }
  }
  return true;
}

std::string HrText(HRESULT hr) {
  char buffer[24] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08lX",
                static_cast<unsigned long>(hr));
  return buffer;
}

// Validates the common document options and builds the store options. Shared by the base
// document and the additional-context helper so both exercise the same checks.
bool BuildStoreOptions(const TestDocumentOptions& options, HWND message_window,
                       UINT grant_message, TestStoreOptions* store_options,
                       std::string* error) {
  const std::size_t text_size = options.initial_text_utf16.size();
  if (text_size > static_cast<std::size_t>(std::numeric_limits<LONG>::max()) ||
      !IsWellFormedUtf16(options.initial_text_utf16)) {
    if (error != nullptr) {
      *error = "initial text is not well-formed UTF-16 or is too long";
    }
    return false;
  }
  const LONG length = static_cast<LONG>(text_size);
  if (options.initial_selection_start_utf16 < 0 ||
      options.initial_selection_end_utf16 < options.initial_selection_start_utf16 ||
      options.initial_selection_end_utf16 > length) {
    if (error != nullptr) {
      *error = "initial selection is outside the initial text (UTF-16 code units)";
    }
    return false;
  }
  TestStoreOptions built;
  built.initial_text_utf16 = options.initial_text_utf16;
  built.initial_selection_start_utf16 = options.initial_selection_start_utf16;
  built.initial_selection_end_utf16 = options.initial_selection_end_utf16;
  built.read_only = options.read_only;
  built.async_locks = options.async_locks;
  built.refuse_write_locks = options.refuse_write_locks;
  built.refuse_compositions = options.refuse_compositions;
  built.refuse_sync_locks = options.refuse_sync_locks;
  built.refuse_selection_reads = options.refuse_selection_reads;
  built.scope_answer = options.scope_answer;
  built.input_scopes = options.input_scopes;
  built.message_window = message_window;
  built.grant_message = grant_message;
  *store_options = std::move(built);
  return true;
}

}  // namespace

std::atomic<int> TestHost::live_instances_{0};

// --- TestDocument ---------------------------------------------------------------------
TestDocument::TestDocument(ITfThreadMgr* thread_manager, ITfDocumentMgr* document_manager,
                           ITfContext* context, TestTextStore* store,
                           std::uint64_t context_id, std::uint64_t generation,
                           bool pops_single_context)
    : thread_manager_(thread_manager),
      document_manager_(document_manager),
      context_(context),
      store_(store),
      context_id_(context_id),
      generation_(generation),
      pops_single_context_(pops_single_context) {}

TestDocument::~TestDocument() {
  Shutdown();
}

bool TestDocument::Shutdown() {
  if (shutdown_) {
    return true;
  }
  shutdown_ = true;
  teardown_.was_connected = connected_;
  if (store_ != nullptr) {
    const TestStoreDiagnostics before_pop = store_->diagnostics();
    teardown_.store_sink_advised = before_pop.sink_advised;
    teardown_.composition_sink_installed = before_pop.composition_sink_queried;
  }
  if (connected_ && document_manager_ != nullptr) {
    // TSF still knows the context and its composition sink here, so an active composition
    // ends through the real OnEndComposition notification instead of disappearing silently.
    // Pop(TF_POPF_ALL) also releases every reference the context holds to the text store and
    // the context-owner composition sink, which is why the count below drops to the host's
    // own reference. A child context pops only itself; the base document's teardown still
    // uses TF_POPF_ALL and releases the manager.
    teardown_.pop_hr = pops_single_context_ ? document_manager_->Pop(0)
                                            : document_manager_->Pop(TF_POPF_ALL);
    connected_ = false;
  }
  if (store_ != nullptr) {
    const TestStoreDiagnostics after_pop = store_->diagnostics();
    teardown_.store_sink_advised_after_pop = after_pop.sink_advised;
    teardown_.store_references_before_release = after_pop.reference_count;
    teardown_.store_diagnostics_before_release = after_pop;
    if (after_pop.sink_advised) {
      // Normally Pop has already unadvised the manager sink. If it did not, the application
      // side of the sink contract still gives it back before the store goes away.
      teardown_.sink_unadvise_hr = store_->ReleaseSinkAdvice() ? S_OK : E_FAIL;
    }
  }
  if (context_ != nullptr) {
    context_->Release();
    context_ = nullptr;
  }
  if (store_ != nullptr) {
    teardown_.store_references_after_context_release =
        store_->diagnostics().reference_count;
  }
  if (document_manager_ != nullptr) {
    document_manager_->Release();
    document_manager_ = nullptr;
  }
  if (store_ != nullptr) {
    teardown_.store_references_after_document_manager_release =
        store_->diagnostics().reference_count;
    // The host's own reference. TSF's references should already be gone after the document
    // manager was released; the store's destructor reports a forced sink release if not.
    store_->Release();
    store_ = nullptr;
    teardown_.store_live_after_host_release = TestTextStore::LiveInstanceCount();
  }
  return SUCCEEDED(teardown_.pop_hr);
}

TestDocumentDiagnostics TestDocument::diagnostics() const {
  TestDocumentDiagnostics result;
  if (store_ != nullptr) {
    result.store = store_->diagnostics();
    result.composition_start_calls = result.store.composition_start_calls;
    result.composition_update_calls = result.store.composition_update_calls;
    result.composition_end_calls = result.store.composition_end_calls;
    result.composition_end_refusals = result.store.composition_end_refusals;
    result.composition_refusals = result.store.composition_refusals;
    result.composition_sink_installed = result.store.composition_sink_queried;
  }
  result.connected = connected_;
  result.context_id = context_id_;
  result.generation = generation_;
  return result;
}


bool TestDocument::Disconnect() {
  if (!connected_ || document_manager_ == nullptr) {
    return false;
  }
  disconnect_result_ = pops_single_context_ ? document_manager_->Pop(0)
                                            : document_manager_->Pop(TF_POPF_ALL);
  if (SUCCEEDED(disconnect_result_)) {
    connected_ = false;
  }
  return SUCCEEDED(disconnect_result_);
}

void TestDocument::SetRefuseCompositions(bool refuse) {
  if (store_ != nullptr) {
    store_->SetRefuseCompositions(refuse);
  }
}

void TestDocument::SetReadOnly(bool read_only) {
  if (store_ != nullptr) {
    store_->SetReadOnly(read_only);
  }
}

void TestDocument::SetScopeAnswer(TestScopeAnswer answer, std::vector<long> scopes) {
  if (store_ != nullptr) {
    store_->SetScopeAnswer(answer, std::move(scopes));
  }
}

void TestDocument::SetRefuseSelectionReads(bool refuse) {
  if (store_ != nullptr) {
    store_->SetRefuseSelectionReads(refuse);
  }
}

namespace {

class SetSelectionEditSession final : public ITfEditSession {
 public:
  SetSelectionEditSession(ITfContext* context, LONG caret_utf16)
      : context_(context), caret_utf16_(caret_utf16) {
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
    TF_SELECTION current = {};
    ULONG fetched = 0;
    if (FAILED(context_->GetSelection(cookie, TF_DEFAULT_SELECTION, 1, &current,
                                      &fetched)) ||
        fetched != 1 || current.range == nullptr) {
      if (current.range != nullptr) {
        current.range->Release();
      }
      return S_OK;
    }
    ITfRangeACP* acp = nullptr;
    const HRESULT qi =
        current.range->QueryInterface(IID_ITfRangeACP, reinterpret_cast<void**>(&acp));
    if (FAILED(qi) || acp == nullptr) {
      current.range->Release();
      return S_OK;
    }
    const HRESULT extent = acp->SetExtent(caret_utf16_, 0);
    acp->Release();
    if (FAILED(extent)) {
      current.range->Release();
      return S_OK;
    }
    TF_SELECTION selection = {};
    selection.range = current.range;
    // The selection reader treats TF_AE_NONE as an unsupported shape; a real application
    // sets one end active even for a collapsed caret.
    selection.style.ase = TF_AE_END;
    selection.style.fInterimChar = FALSE;
    result_ = context_->SetSelection(cookie, 1, &selection);
    current.range->Release();
    return S_OK;
  }

  bool ran() const { return ran_; }
  HRESULT result() const { return result_; }

 private:
  ~SetSelectionEditSession() {
    if (context_ != nullptr) {
      context_->Release();
    }
  }

  std::atomic<LONG> reference_count_{1};
  ITfContext* context_ = nullptr;
  LONG caret_utf16_ = 0;
  bool ran_ = false;
  HRESULT result_ = E_FAIL;
};

}  // namespace

HRESULT TestDocument::SetSelectionForTest(TfClientId client, LONG caret_utf16) {
  if (context_ == nullptr || client == 0 || caret_utf16 < 0) {
    return E_INVALIDARG;
  }
  SetSelectionEditSession* callback = new SetSelectionEditSession(context_, caret_utf16);
  HRESULT session_hr = S_OK;
  const HRESULT request = context_->RequestEditSession(
      client, callback, TF_ES_SYNC | TF_ES_READWRITE, &session_hr);
  const bool ran = callback->ran();
  const HRESULT result = callback->result();
  callback->Release();
  if (FAILED(request) || session_hr == TF_E_SYNCHRONOUS || FAILED(session_hr) || !ran) {
    return FAILED(request) ? request : (FAILED(session_hr) ? session_hr : E_FAIL);
  }
  return result;
}

HRESULT TestDocument::SetContextCompartment(REFGUID guid, TfClientId client,
                                            LONG value) {
  if (context_ == nullptr) {
    return E_FAIL;
  }
  ITfCompartmentMgr* manager = nullptr;
  HRESULT hr = context_->QueryInterface(IID_ITfCompartmentMgr,
                                        reinterpret_cast<void**>(&manager));
  if (FAILED(hr) || manager == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }
  ITfCompartment* compartment = nullptr;
  hr = manager->GetCompartment(guid, &compartment);
  manager->Release();
  if (FAILED(hr) || compartment == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }
  VARIANT variant;
  ::VariantInit(&variant);
  variant.vt = VT_I4;
  variant.lVal = value;
  hr = compartment->SetValue(client, &variant);
  compartment->Release();
  return hr;
}

HRESULT TestDocument::ClearContextCompartment(REFGUID guid, TfClientId client) {
  if (context_ == nullptr) {
    return E_FAIL;
  }
  ITfCompartmentMgr* manager = nullptr;
  HRESULT hr = context_->QueryInterface(IID_ITfCompartmentMgr,
                                        reinterpret_cast<void**>(&manager));
  if (FAILED(hr) || manager == nullptr) {
    return FAILED(hr) ? hr : E_FAIL;
  }
  hr = manager->ClearCompartment(client, guid);
  manager->Release();
  return hr;
}

// --- TestHost -------------------------------------------------------------------------
TestHost::TestHost() {
  ++live_instances_;
}

TestHost::~TestHost() {
  Shutdown();
  --live_instances_;
}

std::unique_ptr<TestHost> TestHost::Create(std::string* error) {
  std::unique_ptr<TestHost> host(new TestHost());
  if (!host->Initialize(error)) {
    host->Shutdown();
    return nullptr;
  }
  return host;
}

bool TestHost::Initialize(std::string* error) {
  const HRESULT com_hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com_hr)) {
    if (error != nullptr) {
      *error = "CoInitializeEx(COINIT_APARTMENTTHREADED) failed " + HrText(com_hr);
    }
    return false;
  }
  com_initialized_ = true;
  cleanup_.com_initialized = true;

  HINSTANCE instance = ::GetModuleHandleW(nullptr);
  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = &TestHost::WindowProc;
  window_class.hInstance = instance;
  window_class.lpszClassName = kHostWindowClass;
  const ATOM atom = ::RegisterClassExW(&window_class);
  const DWORD register_error = ::GetLastError();
  if (atom == 0 && register_error != ERROR_CLASS_ALREADY_EXISTS) {
    if (error != nullptr) {
      *error = "RegisterClassExW failed err=" + std::to_string(register_error);
    }
    return false;
  }
  window_class_registered_ = (atom != 0);
  window_ = ::CreateWindowExW(0, kHostWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                              nullptr, instance, nullptr);
  // Capture immediately: nothing may run between the failing call and GetLastError.
  const DWORD create_error = ::GetLastError();
  if (window_ == nullptr) {
    if (error != nullptr) {
      *error = "CreateWindowExW(HWND_MESSAGE) failed err=" + std::to_string(create_error);
    }
    return false;
  }
  ::SetWindowLongPtrW(window_, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));
  cleanup_.window_created = true;

  HRESULT hr = ::CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITfThreadMgr,
                                  reinterpret_cast<void**>(&thread_manager_));
  if (FAILED(hr) || thread_manager_ == nullptr) {
    if (error != nullptr) {
      *error = "CoCreateInstance(CLSID_TF_ThreadMgr) failed " + HrText(hr);
    }
    return false;
  }
  cleanup_.thread_manager_created = true;

  hr = thread_manager_->Activate(&client_id_);
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "ITfThreadMgr::Activate failed " + HrText(hr);
    }
    return false;
  }
  cleanup_.thread_manager_activated = true;
  return true;
}

TestDocument* TestHost::CreateDocument(const TestDocumentOptions& options,
                                       std::string* error) {
  if (shutdown_ || thread_manager_ == nullptr) {
    if (error != nullptr) {
      *error = "CreateDocument called on a shut-down host";
    }
    return nullptr;
  }
  TestStoreOptions store_options;
  if (!BuildStoreOptions(options, window_, kGrantLockMessage, &store_options, error)) {
    return nullptr;
  }

  ITfDocumentMgr* document_manager = nullptr;
  HRESULT hr = thread_manager_->CreateDocumentMgr(&document_manager);
  if (FAILED(hr) || document_manager == nullptr) {
    if (error != nullptr) {
      *error = "ITfThreadMgr::CreateDocumentMgr failed " + HrText(hr);
    }
    return nullptr;
  }

  TestTextStore* store = new TestTextStore(store_options);
  ITfContext* context = nullptr;
  TfEditCookie cookie = 0;
  // CreateContext also queries `store` for ITfContextOwnerCompositionSink and installs it.
  hr = document_manager->CreateContext(client_id_, 0,
                                       static_cast<ITextStoreACP*>(store),
                                       &context, &cookie);
  if (FAILED(hr) || context == nullptr) {
    if (error != nullptr) {
      *error = "ITfDocumentMgr::CreateContext failed " + HrText(hr);
    }
    store->Release();
    document_manager->Release();
    return nullptr;
  }

  hr = document_manager->Push(context);
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "ITfDocumentMgr::Push failed " + HrText(hr);
    }
    context->Release();
    document_manager->Release();
    store->Release();
    return nullptr;
  }

  const std::uint64_t context_id = ++next_context_id_;
  std::unique_ptr<TestDocument> document(
      new TestDocument(thread_manager_, document_manager, context, store, context_id,
                       /*generation=*/1, /*pops_single_context=*/false));
  document->connected_ = true;  // the Push above succeeded
  TestDocument* borrowed = document.get();
  documents_.push_back(std::move(document));
  // SetFocus is not called here. P3.10's probe shows ITfThreadMgr::SetFocus(dm) succeeds
  // in this environment but does not deliver OnSetFocus to a thread-manager sink while the
  // process owns no OS input focus; the observer tests drive SetFocus explicitly and mark
  // the delivered-notification expectation environment-limited. RequestEditSession does
  // not require focus.
  return borrowed;
}

TestDocument* TestHost::CreateChildContext(TestDocument* base_document,
                                           const TestDocumentOptions& options,
                                           std::string* error) {
  if (shutdown_ || thread_manager_ == nullptr) {
    if (error != nullptr) {
      *error = "CreateChildContext called on a shut-down host";
    }
    return nullptr;
  }
  if (base_document == nullptr || base_document->document_manager_ == nullptr) {
    if (error != nullptr) {
      *error = "CreateChildContext requires a live base document";
    }
    return nullptr;
  }
  TestStoreOptions store_options;
  if (!BuildStoreOptions(options, window_, kGrantLockMessage, &store_options, error)) {
    return nullptr;
  }
  ITfDocumentMgr* document_manager = base_document->document_manager_;
  TestTextStore* store = new TestTextStore(store_options);
  ITfContext* context = nullptr;
  TfEditCookie cookie = 0;
  HRESULT hr = document_manager->CreateContext(client_id_, 0,
                                               static_cast<ITextStoreACP*>(store),
                                               &context, &cookie);
  if (FAILED(hr) || context == nullptr) {
    if (error != nullptr) {
      *error = "ITfDocumentMgr::CreateContext (child) failed " + HrText(hr);
    }
    store->Release();
    return nullptr;
  }
  hr = document_manager->Push(context);
  if (FAILED(hr)) {
    if (error != nullptr) {
      *error = "ITfDocumentMgr::Push (child) failed " + HrText(hr);
    }
    context->Release();
    store->Release();
    return nullptr;
  }
  // The host's store reference is handed to TestDocument, which releases it at teardown;
  // the context holds its own additional reference after CreateContext.
  document_manager->AddRef();  // the child holds a manager reference of its own
  const std::uint64_t context_id = ++next_context_id_;
  std::unique_ptr<TestDocument> document(
      new TestDocument(thread_manager_, document_manager, context, store, context_id,
                       /*generation=*/1, /*pops_single_context=*/true));
  document->connected_ = true;
  TestDocument* borrowed = document.get();
  documents_.push_back(std::move(document));
  return borrowed;
}


bool TestHost::PumpUntil(HANDLE event, DWORD timeout_ms) {
  if (event == nullptr) {
    return false;
  }
  // One overall deadline. The product's watcher posts a wake message every 10 ms while it
  // is paced (for example while an edit waits for the application's write grant), so a
  // per-wait timeout that restarts on every message would never expire.
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    const ULONGLONG now = ::GetTickCount64();
    if (now >= deadline) {
      return false;
    }
    const DWORD remaining = static_cast<DWORD>(deadline - now);
    const DWORD wait =
        ::MsgWaitForMultipleObjects(1, &event, FALSE, remaining, QS_ALLINPUT);
    if (wait == WAIT_OBJECT_0) {
      return true;
    }
    if (wait == WAIT_OBJECT_0 + 1) {
      // Each slice dispatches a bounded number of messages and then waits again. Repeated
      // wakes stay harmless because Pump is idempotent.
      MSG message = {};
      int dispatched = 0;
      while (dispatched < 64 &&
             ::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
        ++dispatched;
      }
      continue;
    }
    // WAIT_TIMEOUT or WAIT_FAILED. The caller gets a failure, not a guessed completion.
    return false;
  }
}


void TestHost::DispatchQueuedMessages() {
  MSG message = {};
  while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
    ::TranslateMessage(&message);
    ::DispatchMessageW(&message);
  }
}


void TestHost::DrainGrantMessages() {
  if (window_ == nullptr || hold_grant_messages_) {
    return;
  }
  MSG message = {};
  while (::PeekMessageW(&message, window_, kGrantLockMessage, kGrantLockMessage,
                        PM_REMOVE) != 0) {
    TestTextStore* store = reinterpret_cast<TestTextStore*>(message.lParam);
    if (store != nullptr) {
      store->GrantPendingLockFromMessage();
      store->Release();  // the posted message owned one reference
      ++cleanup_.grant_messages_drained;
    }
  }
}

void TestHost::SetHoldGrantMessages(bool hold) {
  hold_grant_messages_ = hold;
}

std::size_t TestHost::ReleaseHeldGrantMessages() {
  std::size_t granted = 0;
  for (TestTextStore* store : held_grant_messages_) {
    if (store != nullptr) {
      store->GrantPendingLockFromMessage();
      store->Release();
      ++granted;
    }
  }
  held_grant_messages_.clear();
  return granted;
}

std::size_t TestHost::DropHeldGrantMessages() {
  std::size_t dropped = 0;
  for (TestTextStore* store : held_grant_messages_) {
    if (store != nullptr) {
      store->Release();
      ++dropped;
    }
  }
  held_grant_messages_.clear();
  return dropped;
}

void TestHost::Shutdown() {
  if (shutdown_) {
    return;
  }
  shutdown_ = true;
  cleanup_.shutdown_called = true;

  // P3.13: record how many enumerated document managers were never created by this host
  // before any teardown starts. With a real focused window TSF may own one; it must not be
  // mistaken for a leaked TestDocument manager later.
  auto same_manager = [](ITfDocumentMgr* left, ITfDocumentMgr* right) {
    if (left == nullptr || right == nullptr) {
      return false;
    }
    if (left == right) {
      return true;
    }
    IUnknown* left_unknown = nullptr;
    IUnknown* right_unknown = nullptr;
    const HRESULT left_hr =
        left->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&left_unknown));
    const HRESULT right_hr =
        right->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&right_unknown));
    const bool same = SUCCEEDED(left_hr) && SUCCEEDED(right_hr) &&
                      left_unknown != nullptr && left_unknown == right_unknown;
    if (left_unknown != nullptr) {
      left_unknown->Release();
    }
    if (right_unknown != nullptr) {
      right_unknown->Release();
    }
    return same;
  };
  if (thread_manager_ != nullptr) {
    IEnumTfDocumentMgrs* managers = nullptr;
    if (SUCCEEDED(thread_manager_->EnumDocumentMgrs(&managers)) &&
        managers != nullptr) {
      ITfDocumentMgr* manager = nullptr;
      ULONG fetched = 0;
      while (managers->Next(1, &manager, &fetched) == S_OK && fetched == 1) {
        bool ours = false;
        for (const std::unique_ptr<TestDocument>& document : documents_) {
          if (same_manager(manager, document->document_manager_for_test())) {
            ours = true;
            break;
          }
        }
        if (!ours) {
          ++cleanup_.foreign_document_managers_at_shutdown_start;
        }
        manager->Release();
        fetched = 0;
      }
      managers->Release();
    }
  }

  // A held grant is deliberately NOT granted during teardown: granting it would run a
  // callback that the test chose to keep late. Only the message's store reference is
  // returned; the contexts are popped below and terminate their queued sessions.
  hold_grant_messages_ = false;
  for (TestTextStore* store : held_grant_messages_) {
    if (store != nullptr) {
      store->Release();
    }
  }
  held_grant_messages_.clear();

  // A queued grant runs while every document is still on its stack, so teardown never
  // destroys a store with a message reference pointing at it.
  DrainGrantMessages();

  std::uint64_t destroyed = 0;
  // Reverse creation order: a child context pushed on a base document's manager must be
  // popped (Pop(0)) before the base document's Pop(TF_POPF_ALL).
  for (auto iterator = documents_.rbegin(); iterator != documents_.rend(); ++iterator) {
    const std::unique_ptr<TestDocument>& document = *iterator;
    document->Shutdown();
    ++destroyed;
    const TestDocumentTeardownRecord& record = document->teardown_record();
    cleanup_.any_store_sink_advised_at_teardown |= record.store_sink_advised;
    cleanup_.any_composition_sink_installed |= record.composition_sink_installed;
    cleanup_.all_documents_disconnected &= !document->connected();
    cleanup_.all_contexts_released &= document->context() == nullptr;
    cleanup_.all_document_managers_released &=
        document->document_manager_for_test() == nullptr;
  }
  cleanup_.documents_created = documents_.size();
  cleanup_.documents_destroyed = destroyed;

  if (thread_manager_ != nullptr) {
    IEnumTfDocumentMgrs* managers = nullptr;
    if (SUCCEEDED(thread_manager_->EnumDocumentMgrs(&managers)) && managers != nullptr) {
      ITfDocumentMgr* manager = nullptr;
      ULONG fetched = 0;
      while (managers->Next(1, &manager, &fetched) == S_OK && fetched == 1) {
        ++cleanup_.document_managers_still_enumerated;
        manager->Release();
        fetched = 0;
      }
      managers->Release();
    }
  }

  if (thread_manager_ != nullptr) {
    const HRESULT deactivate_hr = thread_manager_->Deactivate();
    cleanup_.thread_manager_deactivated = SUCCEEDED(deactivate_hr);
    thread_manager_->Release();
    thread_manager_ = nullptr;
    cleanup_.thread_manager_released = true;
  }

  // Messages posted during the document teardown (there should be none, but a message that
  // was queued by a store keeps its own reference) are handled before the window goes away.
  DrainGrantMessages();

  if (window_ != nullptr) {
    ::DestroyWindow(window_);
    window_ = nullptr;
    cleanup_.window_destroyed = true;
  }
  if (window_class_registered_) {
    ::UnregisterClassW(kHostWindowClass, ::GetModuleHandleW(nullptr));
    window_class_registered_ = false;
  }
  if (com_initialized_) {
    ::CoUninitialize();
    com_initialized_ = false;
    cleanup_.com_uninitialized = true;
  }

  cleanup_.store_instances_alive = static_cast<std::uint64_t>(TestTextStore::LiveInstanceCount());
  cleanup_.advised_store_sinks_alive =
      static_cast<std::uint64_t>(TestTextStore::LiveAdvisedSinkCount());
  cleanup_.forced_store_sink_releases = TestTextStore::ForcedSinkReleaseCount();
  cleanup_.store_reference_count_after_shutdown =
      TestTextStore::LastInstanceReferenceCount();
}

int TestHost::LiveInstanceCount() {
  return live_instances_.load();
}

LRESULT CALLBACK TestHost::WindowProc(HWND window, UINT message, WPARAM wparam,
                                       LPARAM lparam) {
  if (message == kGrantLockMessage) {
    TestTextStore* store = reinterpret_cast<TestTextStore*>(lparam);
    TestHost* host = reinterpret_cast<TestHost*>(
        ::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (store != nullptr) {
      if (host != nullptr && host->hold_grant_messages_) {
        // Dispatched but not granted: the host owns the message's store reference until
        // ReleaseHeldGrantMessages() (or teardown).
        host->held_grant_messages_.push_back(store);
        return 0;
      }
      store->GrantPendingLockFromMessage();
      store->Release();  // the posted message owned one reference
    }
    return 0;
  }
  return ::DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace test
}  // namespace edit
}  // namespace tsf
}  // namespace epocanva
