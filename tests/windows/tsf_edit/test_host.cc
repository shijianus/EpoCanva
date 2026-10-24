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

}  // namespace

std::atomic<int> TestHost::live_instances_{0};

// --- TestDocument ---------------------------------------------------------------------
TestDocument::TestDocument(ITfThreadMgr* thread_manager, ITfDocumentMgr* document_manager,
                           ITfContext* context, TestTextStore* store,
                           std::uint64_t context_id, std::uint64_t generation)
    : thread_manager_(thread_manager),
      document_manager_(document_manager),
      context_(context),
      store_(store),
      context_id_(context_id),
      generation_(generation) {}

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
    // own reference.
    teardown_.pop_hr = document_manager_->Pop(TF_POPF_ALL);
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
  disconnect_result_ = document_manager_->Pop(TF_POPF_ALL);
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
  if (atom == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    if (error != nullptr) {
      *error = "RegisterClassExW failed";
    }
    return false;
  }
  window_class_registered_ = (atom != 0);
  window_ = ::CreateWindowExW(0, kHostWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                              nullptr, instance, nullptr);
  if (window_ == nullptr) {
    if (error != nullptr) {
      *error = "CreateWindowExW(HWND_MESSAGE) failed";
    }
    return false;
  }
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
  const std::size_t text_size = options.initial_text_utf16.size();
  if (text_size > static_cast<std::size_t>(std::numeric_limits<LONG>::max()) ||
      !IsWellFormedUtf16(options.initial_text_utf16)) {
    if (error != nullptr) {
      *error = "initial text is not well-formed UTF-16 or is too long";
    }
    return nullptr;
  }
  const LONG length = static_cast<LONG>(text_size);
  if (options.initial_selection_start_utf16 < 0 ||
      options.initial_selection_end_utf16 < options.initial_selection_start_utf16 ||
      options.initial_selection_end_utf16 > length) {
    if (error != nullptr) {
      *error = "initial selection is outside the initial text (UTF-16 code units)";
    }
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

  TestStoreOptions store_options;
  store_options.initial_text_utf16 = options.initial_text_utf16;
  store_options.initial_selection_start_utf16 = options.initial_selection_start_utf16;
  store_options.initial_selection_end_utf16 = options.initial_selection_end_utf16;
  store_options.read_only = options.read_only;
  store_options.async_locks = options.async_locks;
  store_options.refuse_write_locks = options.refuse_write_locks;
  store_options.refuse_compositions = options.refuse_compositions;
  store_options.message_window = window_;
  store_options.grant_message = kGrantLockMessage;

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
                       /*generation=*/1));
  document->connected_ = true;  // the Push above succeeded
  TestDocument* borrowed = document.get();
  documents_.push_back(std::move(document));
  // SetFocus is not called here. ITfThreadMgr::SetFocus sets the input focus of the thread,
  // and this headless test process has none; the real call returns E_FAIL in this
  // environment. RequestEditSession does not require focus, so the host does not manufacture
  // one. The case where focus matters is out of scope for this phase and documented as
  // unverified in docs/windows/tsf-edit-contract.md.
  return borrowed;
}


bool TestHost::PumpUntil(HANDLE event, DWORD timeout_ms) {
  if (event == nullptr) {
    return false;
  }
  for (;;) {
    const DWORD wait = ::MsgWaitForMultipleObjects(1, &event, FALSE, timeout_ms, QS_ALLINPUT);
    if (wait == WAIT_OBJECT_0) {
      return true;
    }
    if (wait == WAIT_OBJECT_0 + 1) {
      MSG message = {};
      while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
      }
      continue;
    }
    // WAIT_TIMEOUT or WAIT_FAILED. The caller gets a failure, not a guessed completion.
    return false;
  }
}


void TestHost::DrainGrantMessages() {
  if (window_ == nullptr) {
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

void TestHost::Shutdown() {
  if (shutdown_) {
    return;
  }
  shutdown_ = true;
  cleanup_.shutdown_called = true;

  // A queued grant runs while every document is still on its stack, so teardown never
  // destroys a store with a message reference pointing at it.
  DrainGrantMessages();

  std::uint64_t destroyed = 0;
  for (const std::unique_ptr<TestDocument>& document : documents_) {
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
    if (store != nullptr) {
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
