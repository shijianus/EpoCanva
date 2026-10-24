// EpoCanva P3.7 -- the controlled ACP text store the TSF test host gives to msctf.dll.
//
// WHAT THIS IS. A deliberately small, but real, ITextStoreACP implementation. It is created
// as the application-side text store of a REAL TSF document (ITfThreadMgr ->
// ITfDocumentMgr::CreateContext -> ITfContext). Every text edit asserted by the P3.7 tests
// therefore travels the real path: the composition layer calls ITfContext::RequestEditSession,
// msctf.dll calls ITextStoreACP::RequestLock here, this store calls the REAL
// ITextStoreACPSink::OnLockGranted, and msctf.dll runs ITfEditSession::DoEditSession inside
// that grant. The backing text is never written by a test-only "service write" entry.
//
// WHAT IT DOES NOT DO. There is no layout, no window, no embedded object, no attribute
// storage and no region support. Those methods return the refusal each interface documents
// (see the .cc for the per-method reason) rather than S_OK. Geometry is reported as
// TS_E_NOLAYOUT because this host genuinely has no layout; GetWnd is E_NOTIMPL because it
// genuinely has no window.
//
// UNITS. Everything named `_utf16` or carrying an ACP position is in UTF-16 code units, the
// unit TSF's ACP speaks. `text_length_utf16` is that unit; it is not a grapheme count and not
// a byte count. The store validates that the text it is asked to insert is well-formed UTF-16
// and refuses unpaired surrogates (E_INVALIDARG): the layer above must not split them, and
// this store is the last place that can notice if it does.
//
// LOCK PROTOCOL (verified against textstor.h and the documented RequestLock /
// OnLockGranted / Document Locks contracts):
//   * a synchronous request is `TS_LF_READ` or `TS_LF_READWRITE` plus `TS_LF_SYNC`;
//   * a request without `TS_LF_SYNC` is asynchronous. This store only defers it when
//     `async_locks` is set; otherwise it grants it immediately, which the contract allows
//     for an available lock;
//   * a synchronous request that cannot be served because a lock is held sets *phrSession to
//     TS_E_SYNCHRONOUS and returns S_OK. It does NOT call OnLockGranted;
//   * a deferred request sets *phrSession to TS_S_ASYNC and returns S_OK, then grants the lock
//     from the host's message loop;
//   * a read/write request made reentrantly while a read lock is held is queued as an upgrade;
//     it is granted after the current OnLockGranted returns, per the Document Locks contract;
//   * the lock exists only for the duration of OnLockGranted, exactly as documented.
//
// FAULT INJECTION. `MutationFault` and `SetFailCompositionEnd` belong to this test host only.
// They let a test pair one executor-visible outcome with different host truths (a write call
// that failed after changing nothing vs. after changing the text; a later composition-end
// refusal), which is how the conservative classification and the uncertain-state refusal are
// exercised. The product layer has no test seam, no fault-injection callback, and no way to
// read the backing text directly; it observes only what TSF shows it.
#ifndef EPOCANVA_TESTS_WINDOWS_TSF_EDIT_TEST_TEXT_STORE_H_
#define EPOCANVA_TESTS_WINDOWS_TSF_EDIT_TEST_TEXT_STORE_H_

#include <windows.h>
#include <msctf.h>
#include <textstor.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace epocanva {
namespace tsf {
namespace edit {
namespace test {

// The permission the test harness attaches to a document. The product layer has its own
// equivalent enum; the tests map one to the other explicitly, so the host is not allowed to
// decide what the product's permission values mean.
enum class TestInputPermission {
  kAllowed = 0,
  kDenied,
  kUnknown,
};

// Test-only failure injected into SetText / InsertTextAtSelection. Each value states what the
// HOST really does (host truth). None of it is observable by the product layer: a mutating
// call that returns any failure HRESULT -- E_FAIL or E_UNEXPECTED alike -- must be classified
// as "possibly mutated", because the HRESULT is not mutation evidence in either direction.
// These modes exist so the tests can prove exactly that, by pairing one executor conclusion
// with two different host truths.
enum class MutationFault {
  kNone = 0,
  // Validate everything, change nothing, return E_FAIL.
  kEFailWithoutMutation,
  // Apply the text, then return E_FAIL (no rollback).
  kEFailAfterMutation,
  // Validate everything, change nothing, return E_UNEXPECTED.
  kEUnexpectedWithoutMutation,
  // Apply the text, then return E_UNEXPECTED (no rollback).
  kEUnexpectedAfterMutation,
};

struct TestStoreOptions {
  // The document's initial text. This is HOST SETUP, not a service write: tests use it only
  // before any edit begins. All later changes go through TSF.
  std::wstring initial_text_utf16;
  LONG initial_selection_start_utf16 = 0;
  LONG initial_selection_end_utf16 = 0;
  bool read_only = false;
  bool async_locks = false;
  // Report a writable document (GetStatus) but refuse every read/write lock. This is a
  // separate observable from read_only: it tests the lock refusal path itself.
  bool refuse_write_locks = false;
  // Refuse every composition through the context-owner composition sink. This is the real
  // TSF path: TSF queries the object passed to CreateContext for
  // ITfContextOwnerCompositionSink, and ITfContextComposition::StartComposition returns
  // S_OK with a null composition when that sink sets *pfOk to FALSE.
  bool refuse_compositions = false;
  HWND message_window = nullptr;
  UINT grant_message = 0;
};

// Value-only snapshot. It contains no text, only lengths and counters, so a test can print it
// without echoing document content.
struct TestStoreDiagnostics {
  std::uint64_t advise_sink_calls = 0;
  std::uint64_t unadvise_sink_calls = 0;
  std::uint64_t request_lock_calls = 0;
  std::uint64_t sync_lock_grants = 0;
  std::uint64_t async_lock_grants = 0;
  std::uint64_t sync_lock_refusals = 0;
  std::uint64_t read_lock_grants = 0;
  std::uint64_t write_lock_grants = 0;
  std::uint64_t write_lock_refusals = 0;
  std::uint64_t get_text_calls = 0;
  std::uint64_t get_selection_calls = 0;
  std::uint64_t set_selection_calls = 0;
  std::uint64_t set_text_calls = 0;
  std::uint64_t insert_text_calls = 0;
  std::uint64_t query_insert_calls = 0;
  std::uint64_t get_end_acp_calls = 0;
  std::uint64_t get_status_calls = 0;
  std::uint64_t text_mutations = 0;
  std::uint64_t rejected_writes = 0;
  std::uint64_t set_text_failures = 0;
  std::uint64_t addref_calls = 0;
  std::uint64_t release_calls = 0;
  std::uint64_t qi_unknown_calls = 0;
  std::uint64_t qi_text_store_calls = 0;
  std::uint64_t qi_composition_sink_calls = 0;
  std::uint64_t composition_start_calls = 0;
  std::uint64_t composition_update_calls = 0;
  std::uint64_t composition_end_calls = 0;
  std::uint64_t composition_end_refusals = 0;
  std::uint64_t composition_refusals = 0;
  bool sink_advised = false;
  bool composition_sink_queried = false;
  bool lock_held = false;
  bool pending_async_lock = false;
  bool read_only = false;
  bool async_locks = false;
  bool refuse_write_locks = false;
  LONG text_length_utf16 = 0;
  LONG selection_start_utf16 = 0;
  LONG selection_end_utf16 = 0;
  ULONG reference_count = 0;
};

class TestTextStore final : public ITextStoreACP,
                           public ITfContextOwnerCompositionSink {
 public:
  explicit TestTextStore(const TestStoreOptions& options);
  ~TestTextStore();

  TestTextStore(const TestTextStore&) = delete;
  TestTextStore& operator=(const TestTextStore&) = delete;

  // IUnknown (one implementation serves both interfaces)
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  // ITextStoreACP -- supported subset, documented per method in the .cc.
  HRESULT STDMETHODCALLTYPE AdviseSink(REFIID riid, IUnknown* unknown,
                                       DWORD mask) override;
  HRESULT STDMETHODCALLTYPE UnadviseSink(IUnknown* unknown) override;
  HRESULT STDMETHODCALLTYPE RequestLock(DWORD lock_flags, HRESULT* session_hr) override;
  HRESULT STDMETHODCALLTYPE GetStatus(TS_STATUS* status) override;
  HRESULT STDMETHODCALLTYPE QueryInsert(LONG test_start, LONG test_end, ULONG cch,
                                        LONG* result_start, LONG* result_end) override;
  HRESULT STDMETHODCALLTYPE GetSelection(ULONG index, ULONG count,
                                         TS_SELECTION_ACP* selection,
                                         ULONG* fetched) override;
  HRESULT STDMETHODCALLTYPE SetSelection(ULONG count,
                                         const TS_SELECTION_ACP* selection) override;
  HRESULT STDMETHODCALLTYPE GetText(LONG start, LONG end, WCHAR* plain,
                                    ULONG plain_capacity, ULONG* plain_length,
                                    TS_RUNINFO* run_info, ULONG run_info_capacity,
                                    ULONG* run_info_length, LONG* next_position) override;
  HRESULT STDMETHODCALLTYPE SetText(DWORD flags, LONG start, LONG end, const WCHAR* text,
                                    ULONG cch, TS_TEXTCHANGE* change) override;
  HRESULT STDMETHODCALLTYPE GetFormattedText(LONG start, LONG end,
                                             IDataObject** data_object) override;
  HRESULT STDMETHODCALLTYPE GetEmbedded(LONG position, REFGUID service, REFIID riid,
                                        IUnknown** unknown) override;
  HRESULT STDMETHODCALLTYPE QueryInsertEmbedded(const GUID* service,
                                                const FORMATETC* format,
                                                BOOL* insertable) override;
  HRESULT STDMETHODCALLTYPE InsertEmbedded(DWORD flags, LONG start, LONG end,
                                           IDataObject* data_object,
                                           TS_TEXTCHANGE* change) override;
  HRESULT STDMETHODCALLTYPE InsertTextAtSelection(DWORD flags, const WCHAR* text, ULONG cch,
                                                  LONG* acp_start, LONG* acp_end,
                                                  TS_TEXTCHANGE* change) override;
  HRESULT STDMETHODCALLTYPE InsertEmbeddedAtSelection(DWORD flags, IDataObject* data_object,
                                                      LONG* acp_start, LONG* acp_end,
                                                      TS_TEXTCHANGE* change) override;
  HRESULT STDMETHODCALLTYPE RequestSupportedAttrs(DWORD flags, ULONG filter_count,
                                                  const TS_ATTRID* filter) override;
  HRESULT STDMETHODCALLTYPE RequestAttrsAtPosition(LONG position, ULONG filter_count,
                                                   const TS_ATTRID* filter,
                                                   DWORD flags) override;
  HRESULT STDMETHODCALLTYPE RequestAttrsTransitioningAtPosition(
      LONG position, ULONG filter_count, const TS_ATTRID* filter, DWORD flags) override;
  HRESULT STDMETHODCALLTYPE FindNextAttrTransition(LONG start, LONG halt, ULONG filter_count,
                                                   const TS_ATTRID* filter, DWORD flags,
                                                   LONG* next_position, BOOL* found,
                                                   LONG* found_offset) override;
  HRESULT STDMETHODCALLTYPE RetrieveRequestedAttrs(ULONG count, TS_ATTRVAL* attributes,
                                                   ULONG* fetched) override;
  HRESULT STDMETHODCALLTYPE GetEndACP(LONG* position) override;
  HRESULT STDMETHODCALLTYPE GetActiveView(TsViewCookie* view) override;
  HRESULT STDMETHODCALLTYPE GetACPFromPoint(TsViewCookie view, const POINT* point,
                                            DWORD flags, LONG* position) override;
  HRESULT STDMETHODCALLTYPE GetTextExt(TsViewCookie view, LONG start, LONG end, RECT* rect,
                                       BOOL* clipped) override;
  HRESULT STDMETHODCALLTYPE GetScreenExt(TsViewCookie view, RECT* rect) override;
  HRESULT STDMETHODCALLTYPE GetWnd(TsViewCookie view, HWND* window) override;

  // ITfContextOwnerCompositionSink -- TSF installs this by querying the object given to
  // ITfDocumentMgr::CreateContext, so the text store itself implements it.
  HRESULT STDMETHODCALLTYPE OnStartComposition(ITfCompositionView* composition,
                                               BOOL* ok) override;
  HRESULT STDMETHODCALLTYPE OnUpdateComposition(ITfCompositionView* composition,
                                                ITfRange* new_range) override;
  HRESULT STDMETHODCALLTYPE OnEndComposition(ITfCompositionView* composition) override;

  // --- test controls -----------------------------------------------------------------
  void SetMutationFault(MutationFault fault);
  void SetAsyncLocks(bool enabled);
  void SetRefuseWriteLocks(bool enabled);
  void SetRefuseCompositions(bool refuse);
  // Makes the context owner's OnEndComposition return E_FAIL. Whether msctf.dll propagates
  // that refusal to ITfComposition::EndComposition's caller is an empirical question this
  // knob exists to answer; if it does not, a TSF-side EndComposition failure cannot be
  // induced from a store and stays unverified.
  void SetFailCompositionEnd(bool fail);

  // Called by the host's message loop. Grants a deferred lock if the document is free.
  // Returns true when a lock was actually granted.
  bool GrantPendingLockFromMessage();

  // The application's half of the sink contract: release the manager's ITextStoreACPSink
  // before the document is torn down. Without this, the store can remain referenced through
  // the sink/context relationship after every other host reference is gone.
  bool ReleaseSinkAdvice();

  // Copy for assertions. The store itself is the only writer after construction.
  std::wstring text_utf16() const;
  LONG text_length_utf16() const;
  LONG selection_start_utf16() const;
  LONG selection_end_utf16() const;
  TestStoreDiagnostics diagnostics() const;

  // Process-wide accounting, used by the cleanup case. A live instance after the host is
  // shut down is a leak; an advised sink after shutdown is a leaked TSF reference.
  static int LiveInstanceCount();
  static int LiveAdvisedSinkCount();
  static std::uint64_t ForcedSinkReleaseCount();
  // Test-only observation of the last live instance's COM reference count. It does not
  // add a reference; it exists to distinguish "our references are gone" from "msctf still
  // holds a reference" during teardown.
  static long LastInstanceReferenceCount();

 private:
  bool HasReadLock() const;
  bool HasWriteLock() const;
  HRESULT GrantLockNow(DWORD requested_flags, bool asynchronous);
  bool PostGrantMessage();
  bool TextIsWellFormedUtf16(const WCHAR* text, ULONG cch) const;
  void ReplaceText(LONG start, LONG end, const WCHAR* text, ULONG cch);
  void AdjustSelectionAfterReplace(LONG start, LONG end, LONG new_end);
  void ClearPending();

  std::atomic<LONG> reference_count_;
  TestStoreOptions options_;
  std::wstring text_utf16_;
  TS_SELECTION_ACP selection_{};

  IUnknown* sink_unknown_ = nullptr;         // AddRef'd, exactly the pointer TSF advised
  ITextStoreACPSink* sink_ = nullptr;        // QI(ITextStoreACPSink), AddRef'd
  DWORD sink_mask_ = 0;

  bool lock_held_ = false;
  DWORD lock_flags_ = 0;
  bool pending_async_lock_ = false;
  DWORD pending_flags_ = 0;
  // True when the pending request was made while an OnLockGranted call was on the stack;
  // such a request is granted as soon as that grant returns.
  bool queued_during_lock_ = false;

  MutationFault mutation_fault_ = MutationFault::kNone;
  bool refuse_compositions_ = false;
  bool fail_composition_end_ = false;
  bool composition_sink_queried_ = false;

  // Counters. The store lives on one STA thread; no test calls it concurrently.
  TestStoreDiagnostics counters_;

  static std::atomic<int> live_instances_;
  static std::atomic<int> live_advised_sinks_;
  static std::atomic<std::uint64_t> forced_sink_releases_;
  static std::atomic<TestTextStore*> last_instance_;
};

}  // namespace test
}  // namespace edit
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TESTS_WINDOWS_TSF_EDIT_TEST_TEXT_STORE_H_
