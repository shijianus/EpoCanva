// EpoCanva P3.7 -- stage 1 verification program: the controlled TSF text-store host.
//
// WHAT THIS PROGRAM PROVES IN THIS STAGE. It does not yet contain the composition layer.
// It proves that the host really is a TSF document: a real ITfThreadMgr, a real document
// manager and context, a real RequestEditSession, a real document lock granted by our
// ITextStoreACP through the real ITextStoreACPSink, and an edit that lands in the backing
// text through TSF's own ITfInsertAtSelection. Both the synchronous grant and the deferred
// asynchronous grant are exercised, the latter completed only by the message loop.
//
// CASES
//   host-sync-edit         RequestEditSession(TF_ES_SYNC|TF_ES_READWRITE) inserts through
//                          ITfInsertAtSelection; the store observes a real write lock and a
//                          real text mutation, and no direct backing-string write occurs.
//   host-async-edit        the store defers the lock (TS_S_ASYNC); RequestEditSession reports
//                          TF_S_ASYNC; the text is untouched until the message loop grants
//                          the lock and TSF runs DoEditSession.
//   host-unsupported-surface
//                          geometry returns TS_E_NOLAYOUT, GetWnd/embedded/formatted text
//                          return E_NOTIMPL, QueryInsertEmbedded reports FALSE, and the
//                          attribute queries truthfully return zero attributes.
//   host-cleanup           every documented teardown action happens: Pop(TF_POPF_ALL) (which
//                          unadvises the store sink and releases the context's references),
//                          the host's context/document-manager/store release, ThreadMgr
//                          deactivation and release, window destruction, CoUninitialize. The
//                          case asserts the store reference count reaches zero.
//   host-can-fail          the falsifiability control: a deliberately false claim must exit
//                          14 and must not print RESULT PASS.
//
// EXIT CODES: 0 pass; 2 usage; 5 a check failed; 14 the self-check's expected failure.
#include <windows.h>
#include <msctf.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "composition_tests.h"
#include "test_host.h"
#include "test_support.h"
#include "test_text_store.h"

namespace {

using epocanva::tsf::edit::test::Check;
using epocanva::tsf::edit::test::CheckHr;
using epocanva::tsf::edit::test::CheckText;
using epocanva::tsf::edit::test::CheckU64;
using epocanva::tsf::edit::test::Finish;
using epocanva::tsf::edit::test::HrText;
using epocanva::tsf::edit::test::kExitCheckFailed;
using epocanva::tsf::edit::test::kExitOk;
using epocanva::tsf::edit::test::kExitSelfCheckFailed;
using epocanva::tsf::edit::test::kExitUsage;
using epocanva::tsf::edit::test::TestDocument;
using epocanva::tsf::edit::test::TestDocumentOptions;
using epocanva::tsf::edit::test::TestHost;
using epocanva::tsf::edit::test::TestTextStore;

// A minimal edit session used only to drive the host in stage 1. It uses TSF's own
// ITfInsertAtSelection, so the text store is reached exactly as a text service would reach
// it. The production layer in stage 2 does not use this class.
class InsertTextSession final : public ITfEditSession {
 public:
  static int LiveCount() { return live_instances_.load(); }

  InsertTextSession(ITfContext* context, std::wstring text, HANDLE completion_event)
      : context_(context), text_(std::move(text)), completion_event_(completion_event) {
    context_->AddRef();
    ++live_instances_;
  }

  ~InsertTextSession() {
    --live_instances_;
    if (context_ != nullptr) {
      context_->Release();
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
    ITfInsertAtSelection* insert = nullptr;
    HRESULT hr = context_->QueryInterface(IID_ITfInsertAtSelection,
                                          reinterpret_cast<void**>(&insert));
    if (SUCCEEDED(hr) && insert != nullptr) {
      ITfRange* range = nullptr;
      hr = insert->InsertTextAtSelection(edit_cookie, 0, text_.c_str(),
                                         static_cast<LONG>(text_.size()), &range);
      if (range != nullptr) {
        range->Release();
      }
      insert->Release();
    }
    done_ = true;
    do_edit_hr_ = hr;
    if (completion_event_ != nullptr) {
      ::SetEvent(completion_event_);
    }
    return hr;
  }

  bool done() const { return done_; }
  HRESULT do_edit_hr() const { return do_edit_hr_; }

 private:
  std::atomic<LONG> reference_count_{1};
  ITfContext* context_ = nullptr;
  std::wstring text_;
  HANDLE completion_event_ = nullptr;
  bool done_ = false;
  HRESULT do_edit_hr_ = E_FAIL;
  static std::atomic<int> live_instances_;
};

std::atomic<int> InsertTextSession::live_instances_{0};

// --- cases ----------------------------------------------------------------------------
int RunHostSyncEdit() {
  std::string error;
  std::unique_ptr<TestHost> host = TestHost::Create(&error);
  Check(host != nullptr, "real TSF host created", error);
  if (host == nullptr) {
    return Finish();
  }
  TestDocumentOptions options;
  options.initial_text_utf16 = L"abc";
  options.initial_selection_start_utf16 = 1;
  options.initial_selection_end_utf16 = 1;
  TestDocument* document = host->CreateDocument(options, &error);
  Check(document != nullptr, "real document manager/context/store created", error);
  if (document == nullptr) {
    return Finish();
  }

  InsertTextSession* session = new InsertTextSession(document->context(), L"X", nullptr);
  HRESULT session_result = E_FAIL;
  const HRESULT request_hr =
      document->context()->RequestEditSession(host->client_id(), session,
                                              TF_ES_SYNC | TF_ES_READWRITE,
                                              &session_result);
  CheckHr(request_hr, S_OK, "RequestEditSession(TF_ES_SYNC|TF_ES_READWRITE) returned S_OK");
  CheckHr(session_result, S_OK, "phrSession carries DoEditSession's S_OK");
  Check(session->done(), "DoEditSession actually ran inside the granted lock", "");
  CheckHr(session->do_edit_hr(), S_OK, "the TSF insertion succeeded");
  CheckText(document->text_utf16(), L"aXbc",
            "the inserted text is in the store, through TSF (not a test write)");

  const auto diagnostics = document->diagnostics().store;
  CheckU64(diagnostics.request_lock_calls, 1, "the store received a real RequestLock");
  CheckU64(diagnostics.write_lock_grants, 1, "a real read/write lock was granted");
  CheckU64(diagnostics.insert_text_calls, 1, "the store's InsertTextAtSelection was called");
  Check(diagnostics.text_mutations == 1, "exactly one text mutation happened",
        std::to_string(diagnostics.text_mutations));
  Check(!diagnostics.lock_held, "the lock no longer exists after OnLockGranted returns", "");
  session->Release();
  return Finish();
}

int RunHostAsyncEdit() {
  std::string error;
  std::unique_ptr<TestHost> host = TestHost::Create(&error);
  Check(host != nullptr, "real TSF host created", error);
  if (host == nullptr) {
    return Finish();
  }
  TestDocumentOptions options;
  options.initial_text_utf16 = L"abc";
  options.initial_selection_start_utf16 = 1;
  options.initial_selection_end_utf16 = 1;
  options.async_locks = true;
  TestDocument* document = host->CreateDocument(options, &error);
  Check(document != nullptr, "async document created", error);
  if (document == nullptr) {
    return Finish();
  }

  HANDLE completion = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Check(completion != nullptr, "completion event created", "");
  InsertTextSession* session = new InsertTextSession(document->context(), L"Y", completion);
  HRESULT session_result = E_FAIL;
  const HRESULT request_hr =
      document->context()->RequestEditSession(host->client_id(), session,
                                              TF_ES_ASYNC | TF_ES_READWRITE,
                                              &session_result);
  CheckHr(request_hr, S_OK, "RequestEditSession(TF_ES_ASYNC|TF_ES_READWRITE) returned S_OK");
  CheckHr(session_result, TF_S_ASYNC,
          "phrSession is TF_S_ASYNC (waiting, not applied)");
  Check(!session->done(), "DoEditSession has not run before the message loop", "");
  CheckText(document->text_utf16(), L"abc",
            "the text is untouched while the lock is deferred");

  auto diagnostics = document->diagnostics().store;
  Check(diagnostics.write_lock_grants == 0,
        "no write lock was granted before the message loop",
        std::to_string(diagnostics.write_lock_grants));
  Check(diagnostics.text_mutations == 0,
        "no text mutation happened before the message loop",
        std::to_string(diagnostics.text_mutations));
  Check(diagnostics.async_lock_grants == 0, "no asynchronous grant has happened yet",
        std::to_string(diagnostics.async_lock_grants));

  Check(host->PumpUntil(completion, 10000),
        "the STA message loop delivered the deferred grant", "");
  Check(session->done(), "DoEditSession ran after the grant", "");
  CheckHr(session->do_edit_hr(), S_OK, "the asynchronous edit succeeded");
  CheckText(document->text_utf16(), L"aYbc", "the deferred edit landed exactly once");
  diagnostics = document->diagnostics().store;
  CheckU64(diagnostics.async_lock_grants, 1, "the grant is counted as asynchronous");

  session->Release();
  ::CloseHandle(completion);
  return Finish();
}

int RunHostUnsupportedSurface() {
  std::string error;
  std::unique_ptr<TestHost> host = TestHost::Create(&error);
  Check(host != nullptr, "real TSF host created", error);
  if (host == nullptr) {
    return Finish();
  }
  TestDocumentOptions options;
  options.initial_text_utf16 = L"ab";
  TestDocument* document = host->CreateDocument(options, &error);
  Check(document != nullptr, "document created", error);
  if (document == nullptr) {
    return Finish();
  }
  TestTextStore* store = document->store();

  TS_STATUS status = {};
  CheckHr(store->GetStatus(&status), S_OK, "GetStatus is implemented");
  Check(status.dwDynamicFlags == 0, "the document is not read-only", "");

  RECT rect = {1, 2, 3, 4};
  BOOL clipped = TRUE;
  CheckHr(store->GetTextExt(1, 0, 1, &rect, &clipped), TS_E_NOLAYOUT,
          "GetTextExt reports TS_E_NOLAYOUT, not a fabricated rectangle");
  CheckHr(store->GetScreenExt(1, &rect), TS_E_NOLAYOUT,
          "GetScreenExt reports TS_E_NOLAYOUT");
  HWND window = reinterpret_cast<HWND>(1);
  CheckHr(store->GetWnd(1, &window), E_NOTIMPL, "GetWnd reports E_NOTIMPL");
  Check(window == nullptr, "GetWnd writes no window", "");

  IDataObject* data_object = reinterpret_cast<IDataObject*>(1);
  CheckHr(store->GetFormattedText(0, 1, &data_object), E_NOTIMPL,
          "GetFormattedText reports E_NOTIMPL");
  BOOL insertable = TRUE;
  CheckHr(store->QueryInsertEmbedded(nullptr, nullptr, &insertable), S_OK,
          "QueryInsertEmbedded answers");
  Check(insertable == FALSE, "no embedded object is accepted", "");

  TS_ATTRVAL attributes[1] = {};
  ULONG fetched = 99;
  CheckHr(store->RetrieveRequestedAttrs(1, attributes, &fetched), S_OK,
          "RetrieveRequestedAttrs answers");
  Check(fetched == 0, "zero attributes are reported", std::to_string(fetched));
  LONG next_position = 0;
  BOOL found = TRUE;
  LONG found_offset = 0;
  CheckHr(store->FindNextAttrTransition(0, 2, 0, nullptr, 0, &next_position, &found,
                                        &found_offset),
          S_OK, "FindNextAttrTransition answers");
  Check(found == FALSE, "no attribute transition exists", "");

  LONG result_start = 0;
  LONG result_end = 0;
  CheckHr(store->QueryInsert(1, 1, 2, &result_start, &result_end), S_OK,
          "QueryInsert accepts valid positions");
  Check(result_start == 1 && result_end == 3, "QueryInsert reports the insertion span",
        std::to_string(result_start) + "," + std::to_string(result_end));
  CheckHr(store->QueryInsert(-1, 0, 0, &result_start, &result_end), E_INVALIDARG,
          "QueryInsert refuses a position outside the document");
  return Finish();
}

int RunHostCleanup() {
  std::string error;
  std::unique_ptr<TestHost> host = TestHost::Create(&error);
  Check(host != nullptr, "real TSF host created", error);
  if (host == nullptr) {
    return Finish();
  }
  TestDocumentOptions options;
  options.initial_text_utf16 = L"abc";
  options.initial_selection_start_utf16 = 3;
  options.initial_selection_end_utf16 = 3;
  TestDocument* document = host->CreateDocument(options, &error);
  Check(document != nullptr, "document created", error);
  if (document == nullptr) {
    return Finish();
  }

  InsertTextSession* session = new InsertTextSession(document->context(), L"!", nullptr);
  HRESULT session_result = E_FAIL;
  const HRESULT request_hr =
      document->context()->RequestEditSession(host->client_id(), session,
                                              TF_ES_SYNC | TF_ES_READWRITE,
                                              &session_result);
  CheckHr(request_hr, S_OK, "a real edit session ran before cleanup");
  CheckHr(session_result, S_OK, "the pre-cleanup edit succeeded");
  session->Release();

  host->Shutdown();
  const auto& report = host->cleanup_report();
  const auto& teardown = document->teardown_record();
  const auto& ledger = teardown.store_diagnostics_before_release;
  std::printf(
      "[tsf-edit] teardown: pop=%s sink_before=%d sink_after_pop=%d refs_before=%lu "
      "refs_after_context=%lu refs_after_docmgr=%lu live_after_host_release=%d "
      "final_refs=%ld addref=%llu release=%llu qi_textstore=%llu qi_sink=%llu\n",
      HrText(teardown.pop_hr).c_str(), teardown.store_sink_advised ? 1 : 0,
      teardown.store_sink_advised_after_pop ? 1 : 0,
      static_cast<unsigned long>(teardown.store_references_before_release),
      static_cast<unsigned long>(teardown.store_references_after_context_release),
      static_cast<unsigned long>(teardown.store_references_after_document_manager_release),
      teardown.store_live_after_host_release,
      report.store_reference_count_after_shutdown,
      static_cast<unsigned long long>(ledger.addref_calls),
      static_cast<unsigned long long>(ledger.release_calls),
      static_cast<unsigned long long>(ledger.qi_text_store_calls),
      static_cast<unsigned long long>(ledger.qi_composition_sink_calls));

  Check(report.shutdown_called, "Shutdown ran", "");
  Check(report.com_initialized && report.com_uninitialized,
        "CoInitializeEx/CoUninitialize are balanced", "");
  Check(report.window_created && report.window_destroyed,
        "the message-only window was created and destroyed", "");
  Check(report.thread_manager_created && report.thread_manager_activated,
        "the real ThreadMgr was created and activated", "");
  Check(report.thread_manager_deactivated && report.thread_manager_released,
        "the ThreadMgr was deactivated and released", "");
  Check(report.documents_created == 1 && report.documents_destroyed == 1,
        "one document was created and torn down",
        std::to_string(report.documents_created) + "/" +
            std::to_string(report.documents_destroyed));
  Check(report.all_documents_disconnected, "the document left the stack", "");
  Check(report.all_contexts_released,
        "every ITfContext reference owned by the host was released", "");
  Check(report.all_document_managers_released,
        "every ITfDocumentMgr reference owned by the host was released", "");
  Check(SUCCEEDED(teardown.pop_hr), "Pop(TF_POPF_ALL) succeeded",
        HrText(teardown.pop_hr));
  Check(teardown.store_sink_advised, "the manager had advised the store sink", "");
  Check(!teardown.store_sink_advised_after_pop,
        "Pop unadvised the manager sink", "");
  Check(SUCCEEDED(teardown.sink_unadvise_hr),
        "no further application-side unadvise was needed",
        HrText(teardown.sink_unadvise_hr));
  Check(report.advised_store_sinks_alive == 0, "no advised store sink survived",
        std::to_string(report.advised_store_sinks_alive));
  Check(report.forced_store_sink_releases == 0,
        "the store destructor did not force a sink release",
        std::to_string(report.forced_store_sink_releases));
  Check(report.any_composition_sink_installed,
        "TSF queried and installed the context-owner composition sink", "");
  Check(report.document_managers_still_enumerated == 0,
        "no document manager remained enumerated by the ThreadMgr",
        std::to_string(report.document_managers_still_enumerated));
  Check(InsertTextSession::LiveCount() == 0, "no edit-session object survived teardown",
        std::to_string(InsertTextSession::LiveCount()));

  // Reference accounting. Pop released every TSF reference to the store; the only one left
  // before the host releases it is the host's own, and releasing that destroys the object.
  Check(teardown.store_references_before_release == 1,
        "only the host's store reference remained after Pop",
        std::to_string(teardown.store_references_before_release));
  Check(teardown.store_references_after_context_release == 1 &&
            teardown.store_references_after_document_manager_release == 1,
        "context and document-manager release did not resurrect a TSF reference", "");
  Check(teardown.store_live_after_host_release == 0,
        "releasing the host reference destroyed the store",
        std::to_string(teardown.store_live_after_host_release));
  Check(report.store_instances_alive == 0, "no TestTextStore instance survived",
        std::to_string(report.store_instances_alive));
  Check(report.store_reference_count_after_shutdown == 0,
        "the store reference count is zero after shutdown",
        std::to_string(report.store_reference_count_after_shutdown));
  Check(ledger.addref_calls == ledger.release_calls,
        "AddRef and Release are balanced once the host reference is included",
        std::to_string(ledger.addref_calls) + "/" +
            std::to_string(ledger.release_calls));
  Check(ledger.qi_text_store_calls == 1 && ledger.qi_composition_sink_calls == 1,
        "msctf queried ITextStoreACP and the composition sink once each",
        std::to_string(ledger.qi_text_store_calls) + "/" +
            std::to_string(ledger.qi_composition_sink_calls));

  host.reset();
  Check(TestHost::LiveInstanceCount() == 0, "no TestHost instance survived", "");
  return Finish();
}

int RunSelfCheckFail() {
  Check(false, "self-check: this claim is deliberately false", "false");
  return kExitSelfCheckFailed;
}

}  // namespace

int main(int argc, char** argv) {
  std::string case_name;
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--case") == 0 && index + 1 < argc) {
      case_name = argv[++index];
    }
  }
  if (case_name.empty()) {
    std::printf("usage: tsf_edit_tests --case <host-sync-edit|host-async-edit|"
                "host-unsupported-surface|host-cleanup|host-can-fail>\n");
    return kExitUsage;
  }
  if (case_name == "host-sync-edit") {
    return RunHostSyncEdit();
  }
  if (case_name == "host-async-edit") {
    return RunHostAsyncEdit();
  }
  if (case_name == "host-unsupported-surface") {
    return RunHostUnsupportedSurface();
  }
  if (case_name == "host-cleanup") {
    return RunHostCleanup();
  }
  if (case_name == "host-can-fail") {
    return RunSelfCheckFail();
  }
  const int composition_code =
      epocanva::tsf::edit::test::RunCompositionCase(case_name);
  if (composition_code >= 0) {
    return composition_code;
  }
  std::printf("unknown case: %s\n", case_name.c_str());
  return kExitUsage;
}
