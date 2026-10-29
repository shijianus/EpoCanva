// EpoCanva P3.7 -- the real TSF test host.
//
// WHAT IT CREATES, AND WHY EACH OBJECT IS REAL:
//   CoInitializeEx(COINIT_APARTMENTTHREADED)  the STA the TSF manager requires
//   CLSID_TF_ThreadMgr                        the operating system's ITfThreadMgr
//   ITfThreadMgr::CreateDocumentMgr           a real document manager
//   ITfDocumentMgr::CreateContext             a real TSF context whose text store is our
//                                             ITextStoreACP (test_text_store.h)
//   ITfDocumentMgr::Push                      the context is genuinely on a document stack
//
// The test store also implements ITfContextOwnerCompositionSink. That is the documented
// installation path (MSDN, ITfContextOwnerCompositionSink): when the application calls
// CreateContext, the TSF manager queries the object passed as `punk` for that interface and
// installs it. No ITfSource::AdviseSink call is involved, and the earlier attempt to advise
// it that way is what produced TS_E_NOOBJECT on this machine.
//
// NOTHING HERE IS A PROXY FOR TSF. The tests call ITfContext::RequestEditSession on the
// returned context; msctf.dll drives the document lock through the store; the edit session's
// DoEditSession runs inside the real OnLockGranted call. The only test-specific parts are the
// store's fault injection, the input-permission field and the message window used to defer an
// asynchronous lock grant until the test pumps messages.
//
// MESSAGE LOOP. A deferred lock is granted by a posted WM_APP message handled on this STA.
// PumpUntil waits on a completion event with MsgWaitForMultipleObjects and dispatches only
// queued window messages; it never sleeps and polls. A timeout is a test failure bound, not
// a completion guess.
//
// TEARDOWN ORDER (documented because it is observable):
//   drain queued grant messages -> Pop(TF_POPF_ALL) while TSF still holds the context ->
//   fallback application-side UnadviseSink if TSF left the sink advised -> release ITfContext
//   -> release ITfDocumentMgr -> release the host's store reference -> ThreadMgr::Deactivate
//   -> release ThreadMgr -> DestroyWindow -> CoUninitialize. Pop releases the context's
//   references to the text store and the composition sink; the cleanup report records the
//   host's addref/release ledger and the zero reference count that follows. SetFocus is
//   deliberately not used: this headless process has no thread input focus, and
//   RequestEditSession does not require it.
#ifndef EPOCANVA_TESTS_WINDOWS_TSF_EDIT_TEST_HOST_H_
#define EPOCANVA_TESTS_WINDOWS_TSF_EDIT_TEST_HOST_H_

#include <windows.h>
#include <msctf.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "test_text_store.h"

namespace epocanva {
namespace tsf {
namespace edit {
namespace test {

struct TestDocumentOptions {
  std::wstring initial_text_utf16;
  LONG initial_selection_start_utf16 = 0;
  LONG initial_selection_end_utf16 = 0;
  bool read_only = false;
  bool async_locks = false;
  bool refuse_write_locks = false;
  // The context owner refuses every composition. ITfContextComposition::StartComposition
  // then returns S_OK with a null composition object, which is the exact case the layer must
  // not fake.
  bool refuse_compositions = false;
  // A busy application: every synchronous lock request is answered TS_E_SYNCHRONOUS
  // (the documented "cannot be served now" answer of the Document Locks contract) and
  // asynchronous ones are deferred through the message window like async_locks. A
  // reader that first asks TF_ES_SYNC must fall back to TF_ES_ASYNC and wait.
  bool refuse_sync_locks = false;
  // Fail the store's GetSelection answer while a lock is held. Used to produce a real
  // query-failure shape in the InputScope read (the selection the property is read over
  // is unavailable), distinct from absent metadata.
  bool refuse_selection_reads = false;
  // InputScope metadata the document's store publishes through the real attribute path.
  TestScopeAnswer scope_answer = TestScopeAnswer::kNone;
  std::vector<long> input_scopes;
};

struct TestDocumentDiagnostics {
  TestStoreDiagnostics store;
  std::uint64_t composition_start_calls = 0;
  std::uint64_t composition_update_calls = 0;
  std::uint64_t composition_end_calls = 0;
  std::uint64_t composition_end_refusals = 0;
  std::uint64_t composition_refusals = 0;
  bool composition_sink_installed = false;
  bool connected = true;
  std::uint64_t context_id = 0;
  std::uint64_t generation = 0;
};

struct TestDocumentTeardownRecord {
  HRESULT pop_hr = S_OK;
  HRESULT sink_unadvise_hr = S_OK;
  bool was_connected = false;
  bool store_sink_advised = false;
  bool store_sink_advised_after_pop = false;
  bool composition_sink_installed = false;
  ULONG store_references_before_release = 0;
  ULONG store_references_after_context_release = 0;
  ULONG store_references_after_document_manager_release = 0;
  int store_live_after_host_release = 0;
  TestStoreDiagnostics store_diagnostics_before_release;
};

struct TestHostCleanupReport {
  bool shutdown_called = false;
  bool com_initialized = false;
  bool com_uninitialized = false;
  bool window_created = false;
  bool window_destroyed = false;
  bool thread_manager_created = false;
  bool thread_manager_activated = false;
  bool thread_manager_deactivated = false;
  bool thread_manager_released = false;
  std::uint64_t documents_created = 0;
  std::uint64_t documents_destroyed = 0;
  std::uint64_t store_instances_alive = 0;
  std::uint64_t advised_store_sinks_alive = 0;
  std::uint64_t forced_store_sink_releases = 0;
  std::uint64_t grant_messages_drained = 0;
  bool any_store_sink_advised_at_teardown = false;
  bool any_composition_sink_installed = false;
  bool all_documents_disconnected = true;
  bool all_document_managers_released = true;
  bool all_contexts_released = true;
  std::uint64_t document_managers_still_enumerated = 0;
  // P3.13: with real Win32/TSF focus, msctf may enumerate one document manager this host
  // did not create (neither A nor B). Record that foreign baseline at the start of Shutdown
  // so the resource check distinguishes it from a leaked TEST document manager. Both counts
  // are real enumeration results; neither is inferred.
  std::uint64_t foreign_document_managers_at_shutdown_start = 0;
  long store_reference_count_after_shutdown = 0;
};

class TestDocument {
 public:
  ~TestDocument();

  TestDocument(const TestDocument&) = delete;
  TestDocument& operator=(const TestDocument&) = delete;

  ITfContext* context() const { return context_; }
  ITfDocumentMgr* document_manager_for_test() const { return document_manager_; }
  TestTextStore* store() const { return store_; }
  std::wstring text_utf16() const { return store_->text_utf16(); }
  LONG selection_start_utf16() const { return store_->selection_start_utf16(); }
  LONG selection_end_utf16() const { return store_->selection_end_utf16(); }
  std::uint64_t context_id() const { return context_id_; }
  std::uint64_t generation() const { return generation_; }
  bool connected() const { return connected_; }
  HRESULT disconnect_result() const { return disconnect_result_; }

  TestDocumentDiagnostics diagnostics() const;
  const TestDocumentTeardownRecord& teardown_record() const { return teardown_; }

  bool Disconnect();
  void SetRefuseCompositions(bool refuse);
  // Moves the document's default selection with a real synchronous read/write TSF edit
  // session owned by `client`. UTF-16 code units; used to prove positional refusal.
  HRESULT SetSelectionForTest(TfClientId client, LONG caret_utf16);
  // Flips the document's real read-only state (the store's GetStatus answer).
  void SetReadOnly(bool read_only);
  // Changes the InputScope metadata the document's store publishes.
  void SetScopeAnswer(TestScopeAnswer answer, std::vector<long> scopes);
  // Flips whether the store answers GetSelection with a failure while locked.
  void SetRefuseSelectionReads(bool refuse);

  // --- real application-side compartment writes -----------------------------------------
  // The application owns its context's compartments (the predefined-compartments
  // reference documents KEYBOARD_DISABLED and EMPTYCONTEXT as context-specific VT_I4
  // values). These helpers drive the real path: QI the context for
  // ITfCompartmentMgr, GetCompartment, ITfCompartment::SetValue / ClearCompartment
  // with the host's own activated client id.
  HRESULT SetContextCompartment(REFGUID guid, TfClientId client, LONG value);
  HRESULT ClearContextCompartment(REFGUID guid, TfClientId client);

 private:
  friend class TestHost;
  TestDocument(ITfThreadMgr* thread_manager, ITfDocumentMgr* document_manager,
               ITfContext* context, TestTextStore* store, std::uint64_t context_id,
               std::uint64_t generation, bool pops_single_context);
  bool Shutdown();

  ITfThreadMgr* thread_manager_ = nullptr;
  ITfDocumentMgr* document_manager_ = nullptr;
  ITfContext* context_ = nullptr;
  TestTextStore* store_ = nullptr;
  std::uint64_t context_id_ = 0;
  std::uint64_t generation_ = 0;
  // True when this document was created as an additional context on another document's
  // manager. Its teardown pops only its own top context (Pop(0)); the base document's
  // teardown still calls Pop(TF_POPF_ALL) and releases the manager itself.
  bool pops_single_context_ = false;
  bool connected_ = false;
  bool shutdown_ = false;
  HRESULT disconnect_result_ = S_OK;
  TestDocumentTeardownRecord teardown_;
};

class TestHost {
 public:
  static std::unique_ptr<TestHost> Create(std::string* error);
  ~TestHost();

  TestHost(const TestHost&) = delete;
  TestHost& operator=(const TestHost&) = delete;

  TestDocument* CreateDocument(const TestDocumentOptions& options, std::string* error);
  // Creates one additional real context on `base_document`'s document manager and pushes
  // it on top. That is the real Push path the observer's OnPushContext/OnPopContext
  // notifications describe. The returned document does not own the manager (it holds its
  // own reference) and its teardown uses Pop(0), so tests must tear children down in
  // LIFO order before the base document (Host::Shutdown does so automatically).
  TestDocument* CreateChildContext(TestDocument* base_document,
                                   const TestDocumentOptions& options,
                                   std::string* error);
  TfClientId client_id() const { return client_id_; }
  // Borrowed; valid until Shutdown(). Used by tests to drive real SetFocus /
  // AssociateFocus / GetFocus on the same thread manager the documents live on.
  ITfThreadMgr* thread_manager_for_test() const { return thread_manager_; }
  HWND message_window_for_test() const { return window_; }

  // Dispatch messages until `event` is signalled. Returns false on timeout. This is a real
  // message loop, not a sleep-poll loop.
  bool PumpUntil(HANDLE event, DWORD timeout_ms);

  // Dispatches whatever messages are already queued (deferred lock grants included)
  // without waiting for anything. Lets a test prove that a late callback publishes
  // nothing after a termination.
  void DispatchQueuedMessages();

  // P3.10 test-only ordering control: when enabled, a deferred lock-grant message is
  // dispatched by the message loop but NOT granted; the host holds the store reference
  // until ReleaseHeldGrantMessages(). This makes "the store request reached TSF, the lock
  // is still pending, and a focus/top-context switch happens first" a fixed order rather
  // than a race. P3.7/P3.8/P3.9 tests leave it disabled.
  void SetHoldGrantMessages(bool hold);
  std::size_t ReleaseHeldGrantMessages();
  // Releases the held store references without granting the lock. The application never
  // answered, so TSF's request stays pending until the context is torn down. Used to model
  // "the user switched away while the application was still holding the lock".
  std::size_t DropHeldGrantMessages();
  std::size_t held_grant_messages() const { return held_grant_messages_.size(); }

  void Shutdown();
  bool shutdown() const { return shutdown_; }
  const TestHostCleanupReport& cleanup_report() const { return cleanup_; }

  static int LiveInstanceCount();

 private:
  TestHost();
  bool Initialize(std::string* error);
  void DrainGrantMessages();

  static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                     LPARAM lparam);

  ITfThreadMgr* thread_manager_ = nullptr;
  TfClientId client_id_ = 0;
  HWND window_ = nullptr;
  bool com_initialized_ = false;
  bool window_class_registered_ = false;
  bool shutdown_ = false;
  std::uint64_t next_context_id_ = 0;
  std::vector<std::unique_ptr<TestDocument>> documents_;
  bool hold_grant_messages_ = false;
  // Each entry owns the store reference the posted message carried.
  std::vector<TestTextStore*> held_grant_messages_;
  TestHostCleanupReport cleanup_;

  static std::atomic<int> live_instances_;
};

}  // namespace test
}  // namespace edit
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TESTS_WINDOWS_TSF_EDIT_TEST_HOST_H_
