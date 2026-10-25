// EpoCanva P3.10 -- implementation of the observer test scaffolding. Test code only.
#include "observer_test_support.h"

#include <objbase.h>

#include <atomic>
#include <cstdio>
#include <memory>

#include "selection_reader.h"

namespace epocanva {
namespace tsf {
namespace context {
namespace test {

namespace {

// A CLSID that exists only for these tests. ITfClientId maps it to a client id distinct
// from the host's Activate() id, which is what makes the external edits genuinely "another
// client" rather than "the same thread".
const CLSID kExternalClientClsid =
    {0x5c0f7a2e, 0x9b41, 0x4f0d, {0x8a, 0x11, 0x2c, 0x77, 0x3e, 0x51, 0x99, 0x02}};

constexpr wchar_t kVisibleWindowClass[] = L"EpoCanvaP310ObserverTestWindow";

std::string HrText(HRESULT hr) {
  char buffer[24] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08lX",
                static_cast<unsigned long>(hr));
  return buffer;
}

LRESULT CALLBACK TestWindowProc(HWND window, UINT message, WPARAM wparam,
                               LPARAM lparam) {
  // The parameters must be forwarded unchanged. DefWindowProcW handles WM_NCCREATE
  // from the CREATESTRUCT passed in lparam; replacing both parameters with zero makes
  // window creation itself fail.
  return ::DefWindowProcW(window, message, wparam, lparam);
}

class TestEditSession final : public ITfEditSession {
 public:
  TestEditSession(ITfContext* context, ExternalEditMode mode, LONG start_utf16,
                  LONG end_utf16, std::wstring text)
      : context_(context),
        mode_(mode),
        start_utf16_(start_utf16),
        end_utf16_(end_utf16),
        text_(std::move(text)) {
    done_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }

  ~TestEditSession() {
    if (done_event_ != nullptr) {
      ::CloseHandle(done_event_);
      done_event_ = nullptr;
    }
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

  HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie edit_cookie) override {
    done_ = true;
    if (context_ == nullptr) {
      edit_hr_ = E_FAIL;
    } else {
      edit_hr_ = Apply(edit_cookie);
    }
    if (done_event_ != nullptr) {
      ::SetEvent(done_event_);
    }
    return edit_hr_;
  }

  bool done() const { return done_; }
  HRESULT edit_hr() const { return edit_hr_; }
  HANDLE done_event() const { return done_event_; }

 private:
  HRESULT Apply(TfEditCookie edit_cookie) {
    ITfRange* base = nullptr;
    HRESULT hr = context_->GetStart(edit_cookie, &base);
    if (FAILED(hr) || base == nullptr) {
      return hr;
    }
    ITfRangeACP* acp = nullptr;
    hr = base->QueryInterface(IID_ITfRangeACP, reinterpret_cast<void**>(&acp));
    if (FAILED(hr) || acp == nullptr) {
      base->Release();
      return hr;
    }
    const LONG length_utf16 =
        mode_ == ExternalEditMode::kInsertText ? 0 : end_utf16_ - start_utf16_;
    hr = acp->SetExtent(start_utf16_, length_utf16);
    if (SUCCEEDED(hr)) {
      if (mode_ == ExternalEditMode::kInsertText) {
        hr = acp->SetText(edit_cookie, 0, text_.c_str(),
                          static_cast<LONG>(text_.size()));
      } else {
        TF_SELECTION selection = {};
        selection.range = acp;
        selection.style.ase = TF_AE_END;
        selection.style.fInterimChar = FALSE;
        hr = context_->SetSelection(edit_cookie, 1, &selection);
      }
    }
    acp->Release();
    base->Release();
    return hr;
  }

  std::atomic<LONG> reference_count_{1};
  ITfContext* context_ = nullptr;  // borrowed; the caller owns the document
  ExternalEditMode mode_;
  LONG start_utf16_ = 0;
  LONG end_utf16_ = 0;
  std::wstring text_;
  HANDLE done_event_ = nullptr;
  bool done_ = false;
  HRESULT edit_hr_ = E_FAIL;
};

bool RunEditWithClient(TestHost& host, TestDocument* document, TfClientId client,
                       ExternalEditMode mode, LONG start_utf16, LONG end_utf16,
                       const std::wstring& text, ExternalEditResult* result,
                       std::string* error) {
  if (document == nullptr || document->context() == nullptr) {
    if (error != nullptr) {
      *error = "no document context";
    }
    return false;
  }
  TestEditSession* session = new TestEditSession(document->context(), mode, start_utf16,
                                                 end_utf16, text);
  HRESULT session_hr = S_OK;
  const HRESULT request_hr = document->context()->RequestEditSession(
      client, session, TF_ES_SYNC | TF_ES_READWRITE, &session_hr);
  result->request_hr = request_hr;
  result->session_hr = session_hr;
  if (SUCCEEDED(request_hr) && session_hr == TF_S_ASYNC) {
    result->async = true;
    result->completed = host.PumpUntil(session->done_event(), 10000);
  } else {
    result->completed = SUCCEEDED(request_hr) && SUCCEEDED(session_hr) &&
                        session->done();
  }
  if (result->completed) {
    result->edit_hr = session->edit_hr();
  }
  session->Release();
  if (!result->completed) {
    if (error != nullptr) {
      *error = "edit session did not run: request=" + HrText(request_hr) +
               " session=" + HrText(session_hr);
    }
    return false;
  }
  return SUCCEEDED(result->session_hr);
}

}  // namespace

int g_total = 0;
int g_failed = 0;

void Check(bool condition, const std::string& what, const std::string& got) {
  ++g_total;
  if (condition) {
    std::printf("[observer-win] PASS %s\n", what.c_str());
    return;
  }
  ++g_failed;
  std::printf("[observer-win] FAIL %s", what.c_str());
  if (!got.empty()) {
    std::printf(" (got: %s)", got.c_str());
  }
  std::printf("\n");
}

std::string DescribeSelection(const SelectionSnapshot& selection) {
  std::string text = DescribeSelectionReadStatus(selection.status);
  if (selection.ok()) {
    text += " caret=" + std::to_string(selection.caret_utf16);
  }
  return text;
}

void CheckDocText(const std::wstring& actual, const std::wstring& expected,
                  const std::string& what) {
  ++g_total;
  if (actual == expected) {
    std::printf("[observer-win] PASS %s\n", what.c_str());
    return;
  }
  ++g_failed;
  std::size_t offset = 0;
  while (offset < actual.size() && offset < expected.size() &&
         actual[offset] == expected[offset]) {
    ++offset;
  }
  std::printf(
      "[observer-win] FAIL %s (got %zu utf16 units, want %zu, first difference at "
      "offset %zu)\n",
      what.c_str(), actual.size(), expected.size(), offset);
}

std::string DescribeObserverSnapshot(const ContextObserverSnapshot& snapshot) {
  std::string text;
  text += "subscribed=" + std::to_string(snapshot.subscribed ? 1 : 0);
  text += " closed=" + std::to_string(snapshot.closed ? 1 : 0);
  text += " target=" + std::to_string(snapshot.has_published_target ? 1 : 0);
  text += " id=" + std::to_string(snapshot.target.context_id);
  text += " gen=" + std::to_string(snapshot.target.generation);
  text += " caret=" + std::to_string(snapshot.target.caret_utf16);
  text += " binds=" + std::to_string(snapshot.target_binds);
  text += " drops=" + std::to_string(snapshot.target_drops);
  text += " last_lost=" + std::string(DescribeContextDropReason(
                             snapshot.last_target_lost_reason));
  text += " last_refusal=" + std::string(DescribeContextDropReason(
                                snapshot.last_refusal_reason));
  text += " focus=" + std::to_string(snapshot.focus_notifications);
  text += " push=" + std::to_string(snapshot.push_notifications);
  text += " pop=" + std::to_string(snapshot.pop_notifications);
  text += " own=" + std::to_string(snapshot.own_edits);
  text += " external=" + std::to_string(snapshot.external_edits);
  text += " text_changes=" + std::to_string(snapshot.text_changes);
  text += " selection_changes=" + std::to_string(snapshot.selection_changes);
  text += " background_ignored=" +
          std::to_string(snapshot.background_events_ignored);
  text += " reads=" + std::to_string(snapshot.selection_reads_begun) + "/" +
          std::to_string(snapshot.selection_reads_completed);
  text += " refusals=" + std::to_string(snapshot.selection_refusals);
  text += " awaiting_bind=" + std::to_string(snapshot.awaiting_bind ? 1 : 0);
  text += " read_pending=" + std::to_string(snapshot.selection_read_pending ? 1 : 0);
  return text;
}

int Finish() {
  std::printf("[observer-win] RESULT %d/%d checks, %d failed\n", g_total - g_failed,
              g_total, g_failed);
  if (g_failed == 0) {
    std::printf("RESULT PASS observer-win %d checks\n", g_total);
    return kExitOk;
  }
  return kExitCheckFailed;
}

// --- recording sink -----------------------------------------------------------------------

void RecordingSink::OnTargetLost(ContextDropReason reason) {
  SinkEvent event;
  event.kind = SinkEvent::Kind::kLost;
  event.reason = reason;
  events_.push_back(event);
  has_target_ = false;
}

void RecordingSink::OnTargetBound(ITfContext* context, const ObservedTarget& target) {
  SinkEvent event;
  event.kind = SinkEvent::Kind::kBound;
  event.context_id = target.context_id;
  event.generation = target.generation;
  event.caret_utf16 = target.caret_utf16;
  events_.push_back(event);
  last_target_ = target;
  has_target_ = true;
  last_context_non_null_ = context != nullptr;
}

void RecordingSink::OnOwnEditCompleted(ITfContext* context,
                                       const ObservedTarget& target,
                                       const SelectionSnapshot& selection) {
  SinkEvent event;
  event.kind = SinkEvent::Kind::kOwnEdit;
  event.context_id = target.context_id;
  event.generation = target.generation;
  event.caret_utf16 = target.caret_utf16;
  event.selection = selection;
  events_.push_back(event);
  last_own_selection_ = selection;
  last_context_non_null_ = context != nullptr;
}

std::size_t RecordingSink::Count(SinkEvent::Kind kind) const {
  std::size_t count = 0;
  for (const SinkEvent& event : events_) {
    if (event.kind == kind) {
      ++count;
    }
  }
  return count;
}

// --- external edits -----------------------------------------------------------------------

TfClientId ExternalClientId(ITfThreadMgr* thread_manager, std::string* error) {
  if (thread_manager == nullptr) {
    if (error != nullptr) {
      *error = "null thread manager";
    }
    return 0;
  }
  ITfClientId* provider = nullptr;
  HRESULT hr = thread_manager->QueryInterface(IID_ITfClientId,
                                              reinterpret_cast<void**>(&provider));
  if (FAILED(hr) || provider == nullptr) {
    if (error != nullptr) {
      *error = "QI ITfClientId failed " + HrText(hr);
    }
    return 0;
  }
  TfClientId id = 0;
  hr = provider->GetClientId(kExternalClientClsid, &id);
  provider->Release();
  if (FAILED(hr) || id == 0) {
    if (error != nullptr) {
      *error = "ITfClientId::GetClientId failed " + HrText(hr);
    }
    return 0;
  }
  return id;
}

bool RunExternalEdit(TestHost& host, TestDocument* document, TfClientId client,
                     ExternalEditMode mode, LONG start_utf16, LONG end_utf16,
                     const std::wstring& text, ExternalEditResult* result,
                     std::string* error) {
  return RunEditWithClient(host, document, client, mode, start_utf16, end_utf16, text,
                           result, error);
}

bool RunOwnEdit(TestHost& host, TestDocument* document, ExternalEditMode mode,
                LONG start_utf16, LONG end_utf16, const std::wstring& text,
                ExternalEditResult* result, std::string* error) {
  return RunEditWithClient(host, document, host.client_id(), mode, start_utf16,
                           end_utf16, text, result, error);
}

// --- driving ------------------------------------------------------------------------------

ObserverDriveResult PumpUntilSettled(ContextObserver& observer, TestHost& host,
                                     DWORD timeout_ms) {
  ObserverDriveResult driven;
  const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
  for (;;) {
    observer.Pump();
    driven.final_state = observer.Snapshot();
    if (!driven.final_state.awaiting_bind) {
      driven.settled = true;
      return driven;
    }
    const ULONGLONG now = ::GetTickCount64();
    if (now >= deadline) {
      return driven;
    }
    const DWORD slice =
        static_cast<DWORD>(deadline - now < 200 ? deadline - now : 200);
    HANDLE event = observer.PendingSelectionEvent();
    if (event != nullptr) {
      host.PumpUntil(event, slice);
    } else {
      host.DispatchQueuedMessages();
    }
  }
}

void PumpMessagesFor(TestHost& host, DWORD timeout_ms) {
  HANDLE never = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (never != nullptr) {
    host.PumpUntil(never, timeout_ms);
    ::CloseHandle(never);
  }
}

// --- focus helpers ------------------------------------------------------------------------

bool SetThreadFocus(TestHost& host, TestDocument* document, FocusCallResult* result,
                    std::string* error) {
  ITfThreadMgr* thread_manager = host.thread_manager_for_test();
  if (thread_manager == nullptr || document == nullptr) {
    if (error != nullptr) {
      *error = "no thread manager or document";
    }
    return false;
  }
  result->set_focus_hr =
      thread_manager->SetFocus(document->document_manager_for_test());
  if (FAILED(result->set_focus_hr)) {
    if (error != nullptr) {
      *error = "ITfThreadMgr::SetFocus failed " + HrText(result->set_focus_hr);
    }
    return false;
  }
  ITfDocumentMgr* focus = nullptr;
  result->get_focus_hr = thread_manager->GetFocus(&focus);
  result->get_focus_ok = SUCCEEDED(result->get_focus_hr) && focus != nullptr;
  if (focus != nullptr) {
    focus->Release();
  }
  if (!result->get_focus_ok) {
    if (error != nullptr) {
      *error = "GetFocus reports no document after SetFocus " +
               HrText(result->get_focus_hr);
    }
    return false;
  }
  return true;
}

HRESULT ReadThreadFocus(TestHost& host, bool* has_document) {
  *has_document = false;
  ITfThreadMgr* thread_manager = host.thread_manager_for_test();
  if (thread_manager == nullptr) {
    return E_FAIL;
  }
  ITfDocumentMgr* focus = nullptr;
  const HRESULT hr = thread_manager->GetFocus(&focus);
  if (SUCCEEDED(hr) && focus != nullptr) {
    *has_document = true;
    focus->Release();
  }
  return hr;
}

bool CreateVisibleTestWindow(HWND* window, std::string* error) {
  *window = nullptr;
  HINSTANCE instance = ::GetModuleHandleW(nullptr);
  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.lpfnWndProc = TestWindowProc;
  window_class.hInstance = instance;
  window_class.lpszClassName = kVisibleWindowClass;
  const ATOM atom = ::RegisterClassExW(&window_class);
  const DWORD register_error = ::GetLastError();
  if (atom == 0 && register_error != ERROR_CLASS_ALREADY_EXISTS) {
    if (error != nullptr) {
      *error = "RegisterClassExW failed err=" + std::to_string(register_error);
    }
    return false;
  }
  HWND created = ::CreateWindowExW(0, kVisibleWindowClass, L"EpoCanva P3.10 focus",
                                   WS_OVERLAPPEDWINDOW, 120, 120, 460, 180, nullptr,
                                   nullptr, instance, nullptr);
  // Capture immediately: nothing may run between the failing call and GetLastError.
  const DWORD create_error = ::GetLastError();
  if (created == nullptr) {
    if (error != nullptr) {
      *error = "CreateWindowExW failed err=" + std::to_string(create_error);
    }
    return false;
  }
  ::ShowWindow(created, SW_SHOWNORMAL);
  ::UpdateWindow(created);
  *window = created;
  return true;
}

HRESULT AssociateDocumentFocus(ITfThreadMgr* thread_manager, HWND window,
                               ITfDocumentMgr* document_manager,
                               ITfDocumentMgr** previous) {
  if (previous == nullptr) {
    return E_INVALIDARG;
  }
  *previous = nullptr;
  if (thread_manager == nullptr || window == nullptr) {
    return E_INVALIDARG;
  }
  return thread_manager->AssociateFocus(window, document_manager, previous);
}

void ReleaseAssociatedDocumentManager(ITfDocumentMgr* document_manager) {
  if (document_manager != nullptr) {
    document_manager->Release();
  }
}

}  // namespace test
}  // namespace context
}  // namespace tsf
}  // namespace epocanva
