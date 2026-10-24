// EpoCanva P3.7 -- implementation of the controlled ITextStoreACP test host.
//
// The comments at each method below state which contract clause it follows: a supported
// method explains what it enforces, and an unsupported one names the refusal it returns. No
// unsupported method returns S_OK to look cooperative.
#include "test_text_store.h"

#include <algorithm>
#include <cwchar>
#include <limits>
#include <utility>

namespace epocanva {
namespace tsf {
namespace edit {
namespace test {
namespace {

constexpr LONG kMaxLong = std::numeric_limits<LONG>::max();
// The TSF embedded-object placeholder. ITfRange::SetText documents that this character must
// not be passed to it; the store refuses it as well, so a bug in the layer cannot store it.
constexpr wchar_t kEmbeddedPlaceholder = 0xFFFC;

bool IsHighSurrogate(wchar_t unit) {
  return unit >= 0xD800 && unit <= 0xDBFF;
}

bool IsLowSurrogate(wchar_t unit) {
  return unit >= 0xDC00 && unit <= 0xDFFF;
}

}  // namespace

std::atomic<int> TestTextStore::live_instances_{0};
std::atomic<int> TestTextStore::live_advised_sinks_{0};
std::atomic<std::uint64_t> TestTextStore::forced_sink_releases_{0};
std::atomic<TestTextStore*> TestTextStore::last_instance_{nullptr};

TestTextStore::TestTextStore(const TestStoreOptions& options)
    : reference_count_(1),
      options_(options),
      text_utf16_(options.initial_text_utf16),
      refuse_compositions_(options.refuse_compositions) {
  selection_.acpStart = options.initial_selection_start_utf16;
  selection_.acpEnd = options.initial_selection_end_utf16;
  selection_.style.ase = TS_AE_END;
  selection_.style.fInterimChar = FALSE;

  counters_.text_length_utf16 = static_cast<LONG>(text_utf16_.size());
  counters_.selection_start_utf16 = selection_.acpStart;
  counters_.selection_end_utf16 = selection_.acpEnd;
  counters_.read_only = options_.read_only;
  counters_.async_locks = options_.async_locks;
  counters_.refuse_write_locks = options_.refuse_write_locks;
  ++live_instances_;
  last_instance_.store(this);
}

TestTextStore::~TestTextStore() {
  if (sink_ != nullptr) {
    // TSF normally unadvises before the store dies. If it did not, release the reference
    // instead of leaking it, and count the event so the cleanup case can report it.
    sink_->Release();
    sink_ = nullptr;
    sink_unknown_->Release();
    sink_unknown_ = nullptr;
    --live_advised_sinks_;
    ++forced_sink_releases_;
  }
  if (last_instance_.load() == this) {
    last_instance_.store(nullptr);
  }
  --live_instances_;
}

// --- IUnknown -------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE TestTextStore::QueryInterface(REFIID riid, void** object) {
  if (object == nullptr) {
    return E_INVALIDARG;
  }
  *object = nullptr;
  if (riid == IID_IUnknown) {
    ++counters_.qi_unknown_calls;
    *object = static_cast<ITextStoreACP*>(this);
    AddRef();
    return S_OK;
  }
  if (riid == IID_ITextStoreACP) {
    ++counters_.qi_text_store_calls;
    *object = static_cast<ITextStoreACP*>(this);
    AddRef();
    return S_OK;
  }
  if (riid == IID_ITfContextOwnerCompositionSink) {
    // TSF queries the object passed to CreateContext for this interface and installs the
    // sink itself; there is no separate AdviseSink call for it.
    ++counters_.qi_composition_sink_calls;
    composition_sink_queried_ = true;
    counters_.composition_sink_queried = true;
    *object = static_cast<ITfContextOwnerCompositionSink*>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE TestTextStore::AddRef() {
  ++counters_.addref_calls;
  return static_cast<ULONG>(reference_count_.fetch_add(1) + 1);
}

ULONG STDMETHODCALLTYPE TestTextStore::Release() {
  ++counters_.release_calls;
  const LONG remaining = reference_count_.fetch_sub(1) - 1;
  if (remaining == 0) {
    delete this;
  }
  return static_cast<ULONG>(remaining);
}

// --- sink management ------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE TestTextStore::AdviseSink(REFIID riid, IUnknown* unknown,
                                                    DWORD mask) {
  ++counters_.advise_sink_calls;
  if (unknown == nullptr || riid != IID_ITextStoreACPSink) {
    return E_INVALIDARG;
  }
  ITextStoreACPSink* sink = nullptr;
  HRESULT hr = unknown->QueryInterface(IID_ITextStoreACPSink,
                                       reinterpret_cast<void**>(&sink));
  if (FAILED(hr) || sink == nullptr) {
    return E_NOINTERFACE;
  }
  if (sink_ != nullptr) {
    sink_->Release();
    sink_unknown_->Release();
    --live_advised_sinks_;
  }
  sink_ = sink;
  sink_unknown_ = unknown;
  sink_unknown_->AddRef();
  sink_mask_ = mask;
  ++live_advised_sinks_;
  counters_.sink_advised = true;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE TestTextStore::UnadviseSink(IUnknown* unknown) {
  ++counters_.unadvise_sink_calls;
  if (unknown == nullptr || sink_unknown_ == nullptr || unknown != sink_unknown_) {
    return E_INVALIDARG;
  }
  sink_->Release();
  sink_ = nullptr;
  sink_unknown_->Release();
  sink_unknown_ = nullptr;
  sink_mask_ = 0;
  --live_advised_sinks_;
  counters_.sink_advised = false;
  return S_OK;
}

// --- the lock protocol ----------------------------------------------------------------
HRESULT STDMETHODCALLTYPE TestTextStore::RequestLock(DWORD lock_flags, HRESULT* session_hr) {
  if (session_hr == nullptr) {
    return E_INVALIDARG;
  }
  *session_hr = E_FAIL;
  ++counters_.request_lock_calls;
  if (sink_ == nullptr) {
    return E_FAIL;  // no sink, no lock: never fake a grant
  }

  const bool wants_write = (lock_flags & TS_LF_READWRITE) == TS_LF_READWRITE;
  const bool wants_sync = (lock_flags & TS_LF_SYNC) != 0;

  // A read-only document, or a store configured to refuse write locks, refuses the write
  // request itself. The layer also checks ITfContext::GetStatus first; this is the second,
  // independent refusal point.
  if (wants_write && (options_.read_only || options_.refuse_write_locks)) {
    ++counters_.write_lock_refusals;
    ++counters_.rejected_writes;
    *session_hr = TS_E_READONLY;
    return S_OK;
  }

  if (lock_held_) {
    if (wants_sync) {
      // Document Locks: a synchronous request that cannot be served says so and is not
      // granted. It does not supersede a pending asynchronous request.
      ++counters_.sync_lock_refusals;
      *session_hr = TS_E_SYNCHRONOUS;
      return S_OK;
    }
    // Asynchronous while locked: queue it. A write request during a read lock is the one
    // documented reentrant upgrade; it is granted after the current OnLockGranted returns.
    pending_async_lock_ = true;
    pending_flags_ = lock_flags;
    queued_during_lock_ = true;
    *session_hr = TS_S_ASYNC;
    return S_OK;
  }

  if (wants_sync || !options_.async_locks) {
    // Available and either synchronous or a store that does not defer. A successful request
    // for a synchronous lock supersedes a pending asynchronous one.
    pending_async_lock_ = false;
    pending_flags_ = 0;
    *session_hr = GrantLockNow(lock_flags, /*asynchronous=*/!wants_sync);
    return S_OK;
  }

  pending_async_lock_ = true;
  pending_flags_ = lock_flags;
  queued_during_lock_ = false;
  *session_hr = TS_S_ASYNC;
  if (!PostGrantMessage()) {
    // Without a message loop there is no way to grant this; report failure instead of
    // leaving TSF waiting forever.
    ClearPending();
    *session_hr = E_FAIL;
  }
  return S_OK;
}

HRESULT TestTextStore::GrantLockNow(DWORD requested_flags, bool asynchronous) {
  const bool write = (requested_flags & TS_LF_READWRITE) == TS_LF_READWRITE;
  // The lock duration is OnLockGranted only; TS_LF_SYNC is a request modifier and is not
  // part of the granted lock type.
  const DWORD grant_flags = write ? TS_LF_READWRITE : TS_LF_READ;
  lock_held_ = true;
  lock_flags_ = grant_flags;
  counters_.lock_held = true;
  if (write) {
    ++counters_.write_lock_grants;
  } else {
    ++counters_.read_lock_grants;
  }
  if (asynchronous) {
    ++counters_.async_lock_grants;
  } else {
    ++counters_.sync_lock_grants;
  }

  const HRESULT hr = sink_->OnLockGranted(grant_flags);

  lock_held_ = false;
  lock_flags_ = 0;
  counters_.lock_held = false;

  if (queued_during_lock_) {
    // The reentrant request was made while this grant was on the stack. Grant it now, once,
    // as the Document Locks contract requires.
    queued_during_lock_ = false;
    if (pending_async_lock_) {
      const DWORD queued_flags = pending_flags_;
      ClearPending();
      GrantLockNow(queued_flags, /*asynchronous=*/true);
    }
  }
  return hr;
}

bool TestTextStore::GrantPendingLockFromMessage() {
  if (sink_ == nullptr || !pending_async_lock_) {
    return false;
  }
  if (lock_held_) {
    // The message was dispatched while another OnLockGranted was on the stack. Keep the
    // request queued; GrantLockNow grants it as soon as the current lock ends.
    queued_during_lock_ = true;
    return false;
  }
  const DWORD flags = pending_flags_;
  ClearPending();
  GrantLockNow(flags, /*asynchronous=*/true);
  return true;
}

bool TestTextStore::ReleaseSinkAdvice() {
  if (sink_unknown_ == nullptr) {
    return false;
  }
  // UnadviseSink releases both the QI reference and the advised pointer. The raw pointer is
  // not used after the call.
  IUnknown* advised = sink_unknown_;
  return SUCCEEDED(UnadviseSink(advised));
}

void TestTextStore::ClearPending() {
  pending_async_lock_ = false;
  pending_flags_ = 0;
}

bool TestTextStore::PostGrantMessage() {
  if (options_.message_window == nullptr || options_.grant_message == 0) {
    return false;
  }
  // The message owns a reference until the window procedure runs. That keeps the store
  // alive if the host is torn down with a grant message still queued.
  AddRef();
  if (::PostMessageW(options_.message_window, options_.grant_message, 0,
                     reinterpret_cast<LPARAM>(this)) == 0) {
    Release();
    return false;
  }
  return true;
}

bool TestTextStore::HasReadLock() const {
  return lock_held_ && (lock_flags_ & TS_LF_READ) != 0;
}

bool TestTextStore::HasWriteLock() const {
  return lock_held_ && (lock_flags_ & TS_LF_READWRITE) == TS_LF_READWRITE;
}

// --- status, selection, text ----------------------------------------------------------
HRESULT STDMETHODCALLTYPE TestTextStore::GetStatus(TS_STATUS* status) {
  ++counters_.get_status_calls;
  if (status == nullptr) {
    return E_INVALIDARG;
  }
  status->dwDynamicFlags = options_.read_only ? TS_SD_READONLY : 0;
  status->dwStaticFlags = 0;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE TestTextStore::QueryInsert(LONG test_start, LONG test_end,
                                                     ULONG cch, LONG* result_start,
                                                     LONG* result_end) {
  ++counters_.query_insert_calls;
  if (result_start == nullptr || result_end == nullptr) {
    return E_INVALIDARG;
  }
  *result_start = 0;
  *result_end = 0;
  const LONG length = static_cast<LONG>(text_utf16_.size());
  if (test_start < 0 || test_end < test_start || test_end > length) {
    return E_INVALIDARG;
  }
  if (cch > static_cast<ULONG>(kMaxLong) ||
      test_start > kMaxLong - static_cast<LONG>(cch)) {
    return E_INVALIDARG;
  }
  *result_start = test_start;
  *result_end = test_start + static_cast<LONG>(cch);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE TestTextStore::GetSelection(ULONG index, ULONG count,
                                                      TS_SELECTION_ACP* selection,
                                                      ULONG* fetched) {
  ++counters_.get_selection_calls;
  if (selection == nullptr || fetched == nullptr) {
    return E_INVALIDARG;
  }
  *fetched = 0;
  if (count == 0) {
    return E_INVALIDARG;
  }
  if (!HasReadLock()) {
    return TS_E_NOLOCK;
  }
  if (index != 0 && index != TS_DEFAULT_SELECTION) {
    return TS_E_NOSELECTION;
  }
  selection[0] = selection_;
  *fetched = 1;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE TestTextStore::SetSelection(
    ULONG count, const TS_SELECTION_ACP* selection) {
  ++counters_.set_selection_calls;
  if (selection == nullptr || count != 1) {
    return E_INVALIDARG;
  }
  if (options_.read_only) {
    return TS_E_READONLY;
  }
  if (!HasWriteLock()) {
    return TS_E_NOLOCK;
  }
  const LONG length = static_cast<LONG>(text_utf16_.size());
  if (selection[0].acpStart < 0 || selection[0].acpEnd < selection[0].acpStart ||
      selection[0].acpEnd > length) {
    return TS_E_INVALIDPOS;
  }
  selection_ = selection[0];
  counters_.selection_start_utf16 = selection_.acpStart;
  counters_.selection_end_utf16 = selection_.acpEnd;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE TestTextStore::GetText(
    LONG start, LONG end, WCHAR* plain, ULONG plain_capacity, ULONG* plain_length,
    TS_RUNINFO* run_info, ULONG run_info_capacity, ULONG* run_info_length,
    LONG* next_position) {
  ++counters_.get_text_calls;
  if (plain_length == nullptr || run_info_length == nullptr || next_position == nullptr) {
    return E_INVALIDARG;
  }
  *plain_length = 0;
  *run_info_length = 0;
  *next_position = 0;
  if (plain == nullptr && plain_capacity != 0) {
    return E_INVALIDARG;
  }
  if (run_info == nullptr && run_info_capacity != 0) {
    return E_INVALIDARG;
  }
  if (!HasReadLock()) {
    return TS_E_NOLOCK;
  }
  const LONG length = static_cast<LONG>(text_utf16_.size());
  const LONG effective_end = (end == -1) ? length : end;
  if (start < 0 || effective_end < start || effective_end > length) {
    return TS_E_INVALIDPOS;
  }
  const LONG available = effective_end - start;
  const ULONG copied =
      std::min<ULONG>(static_cast<ULONG>(available), plain_capacity);
  if (copied != 0) {
    std::wmemcpy(plain, text_utf16_.data() + start, copied);
  }
  *plain_length = copied;
  if (run_info_capacity != 0) {
    run_info[0].uCount = copied;
    run_info[0].type = TS_RT_PLAIN;
    *run_info_length = 1;
  }
  *next_position = start + static_cast<LONG>(copied);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE TestTextStore::SetText(DWORD flags, LONG start, LONG end,
                                                 const WCHAR* text, ULONG cch,
                                                 TS_TEXTCHANGE* change) {
  (void)flags;
  ++counters_.set_text_calls;
  if (change == nullptr) {
    return E_INVALIDARG;
  }
  change->acpStart = 0;
  change->acpOldEnd = 0;
  change->acpNewEnd = 0;
  if (options_.read_only) {
    ++counters_.rejected_writes;
    return TS_E_READONLY;
  }
  if (!HasWriteLock()) {
    ++counters_.rejected_writes;
    return TS_E_NOLOCK;
  }
  const LONG length = static_cast<LONG>(text_utf16_.size());
  if (start < 0 || end < start || end > length) {
    ++counters_.rejected_writes;
    return TS_E_INVALIDPOS;
  }
  if (cch != 0 && text == nullptr) {
    return E_INVALIDARG;
  }
  if (cch > static_cast<ULONG>(kMaxLong) || !TextIsWellFormedUtf16(text, cch)) {
    return E_INVALIDARG;
  }
  if (mutation_fault_ == MutationFault::kFailBeforeMutation) {
    ++counters_.set_text_failures;
    return E_FAIL;
  }

  const LONG old_end = end;
  const LONG new_end = start + static_cast<LONG>(cch);
  ReplaceText(start, end, text, cch);
  ++counters_.text_mutations;
  change->acpStart = start;
  change->acpOldEnd = old_end;
  change->acpNewEnd = new_end;

  if (mutation_fault_ == MutationFault::kFailAfterMutation) {
    ++counters_.set_text_failures;
    // E_UNEXPECTED is the test host's explicit partial-application signal: the backing text
    // changed even though the call failed. TSF keeps its cached view of the old text, so a
    // read-back through TSF cannot see this change; the layer must classify the signal.
    return E_UNEXPECTED;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE TestTextStore::InsertTextAtSelection(DWORD flags, const WCHAR* text,
                                                               ULONG cch, LONG* acp_start,
                                                               LONG* acp_end,
                                                               TS_TEXTCHANGE* change) {
  ++counters_.insert_text_calls;
  if ((flags & TS_IAS_NOQUERY) != 0 && (flags & TS_IAS_QUERYONLY) != 0) {
    return E_INVALIDARG;
  }
  if ((flags & TS_IAS_NOQUERY) == 0 && (acp_start == nullptr || acp_end == nullptr)) {
    return E_INVALIDARG;
  }
  if ((flags & TS_IAS_QUERYONLY) == 0 && change == nullptr) {
    return E_INVALIDARG;
  }
  if (change != nullptr) {
    change->acpStart = 0;
    change->acpOldEnd = 0;
    change->acpNewEnd = 0;
  }
  if (acp_start != nullptr) {
    *acp_start = 0;
  }
  if (acp_end != nullptr) {
    *acp_end = 0;
  }
  if (cch != 0 && text == nullptr) {
    return E_INVALIDARG;
  }
  if (cch > static_cast<ULONG>(kMaxLong) || !TextIsWellFormedUtf16(text, cch)) {
    return E_INVALIDARG;
  }

  const LONG start = selection_.acpStart;
  const LONG end = selection_.acpEnd;
  if (start < 0 || end < start || end > static_cast<LONG>(text_utf16_.size())) {
    return TS_E_INVALIDPOS;
  }

  if ((flags & TS_IAS_QUERYONLY) != 0) {
    if (acp_start != nullptr) {
      *acp_start = start + static_cast<LONG>(cch);
    }
    if (acp_end != nullptr) {
      *acp_end = start + static_cast<LONG>(cch);
    }
    if (change != nullptr) {
      change->acpStart = start;
      change->acpOldEnd = end;
      change->acpNewEnd = start + static_cast<LONG>(cch);
    }
    return S_OK;
  }

  if (options_.read_only) {
    ++counters_.rejected_writes;
    return TS_E_READONLY;
  }
  if (!HasWriteLock()) {
    ++counters_.rejected_writes;
    return TS_E_NOLOCK;
  }
  if (mutation_fault_ == MutationFault::kFailBeforeMutation) {
    ++counters_.set_text_failures;
    return E_FAIL;
  }

  ReplaceText(start, end, text, cch);
  ++counters_.text_mutations;
  if (change != nullptr) {
    change->acpStart = start;
    change->acpOldEnd = end;
    change->acpNewEnd = start + static_cast<LONG>(cch);
  }
  if (acp_start != nullptr) {
    *acp_start = start + static_cast<LONG>(cch);
  }
  if (acp_end != nullptr) {
    *acp_end = start + static_cast<LONG>(cch);
  }

  if (mutation_fault_ == MutationFault::kFailAfterMutation) {
    ++counters_.set_text_failures;
    // E_UNEXPECTED is the test host's explicit partial-application signal: the backing text
    // changed even though the call failed. TSF keeps its cached view of the old text, so a
    // read-back through TSF cannot see this change; the layer must classify the signal.
    return E_UNEXPECTED;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE TestTextStore::GetEndACP(LONG* position) {
  ++counters_.get_end_acp_calls;
  if (position == nullptr) {
    return E_INVALIDARG;
  }
  *position = 0;
  if (!HasReadLock()) {
    return TS_E_NOLOCK;
  }
  *position = static_cast<LONG>(text_utf16_.size());
  return S_OK;
}

// --- unsupported surfaces, with the contract's own refusal ----------------------------
HRESULT STDMETHODCALLTYPE TestTextStore::GetFormattedText(LONG start, LONG end,
                                                          IDataObject** data_object) {
  (void)start;
  (void)end;
  if (data_object == nullptr) {
    return E_INVALIDARG;
  }
  *data_object = nullptr;
  return E_NOTIMPL;  // this host stores plain text only
}

HRESULT STDMETHODCALLTYPE TestTextStore::GetEmbedded(LONG position, REFGUID service,
                                                     REFIID riid, IUnknown** unknown) {
  (void)position;
  (void)service;
  (void)riid;
  if (unknown == nullptr) {
    return E_INVALIDARG;
  }
  *unknown = nullptr;
  return E_NOTIMPL;  // no embedded objects
}

HRESULT STDMETHODCALLTYPE TestTextStore::QueryInsertEmbedded(const GUID* service,
                                                             const FORMATETC* format,
                                                             BOOL* insertable) {
  (void)service;
  (void)format;
  if (insertable == nullptr) {
    return E_INVALIDARG;
  }
  *insertable = FALSE;
  return S_OK;  // the documented answer for a store that accepts no embedded objects
}

HRESULT STDMETHODCALLTYPE TestTextStore::InsertEmbedded(DWORD flags, LONG start, LONG end,
                                                        IDataObject* data_object,
                                                        TS_TEXTCHANGE* change) {
  (void)flags;
  (void)start;
  (void)end;
  (void)data_object;
  if (change != nullptr) {
    change->acpStart = 0;
    change->acpOldEnd = 0;
    change->acpNewEnd = 0;
  }
  return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE TestTextStore::InsertEmbeddedAtSelection(
    DWORD flags, IDataObject* data_object, LONG* acp_start, LONG* acp_end,
    TS_TEXTCHANGE* change) {
  (void)flags;
  (void)data_object;
  if (acp_start != nullptr) {
    *acp_start = 0;
  }
  if (acp_end != nullptr) {
    *acp_end = 0;
  }
  if (change != nullptr) {
    change->acpStart = 0;
    change->acpOldEnd = 0;
    change->acpNewEnd = 0;
  }
  return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE TestTextStore::RequestSupportedAttrs(DWORD flags,
                                                               ULONG filter_count,
                                                               const TS_ATTRID* filter) {
  (void)flags;
  (void)filter_count;
  (void)filter;
  return S_OK;  // no attributes are supported; RetrieveRequestedAttrs returns zero
}

HRESULT STDMETHODCALLTYPE TestTextStore::RequestAttrsAtPosition(
    LONG position, ULONG filter_count, const TS_ATTRID* filter, DWORD flags) {
  (void)position;
  (void)filter_count;
  (void)filter;
  (void)flags;
  return S_OK;  // no attributes are supported
}

HRESULT STDMETHODCALLTYPE TestTextStore::RequestAttrsTransitioningAtPosition(
    LONG position, ULONG filter_count, const TS_ATTRID* filter, DWORD flags) {
  (void)position;
  (void)filter_count;
  (void)filter;
  (void)flags;
  return S_OK;  // no attributes are supported
}

HRESULT STDMETHODCALLTYPE TestTextStore::FindNextAttrTransition(
    LONG start, LONG halt, ULONG filter_count, const TS_ATTRID* filter, DWORD flags,
    LONG* next_position, BOOL* found, LONG* found_offset) {
  (void)start;
  (void)filter_count;
  (void)filter;
  (void)flags;
  if (next_position == nullptr || found == nullptr || found_offset == nullptr) {
    return E_INVALIDARG;
  }
  *next_position = halt;  // no transition exists; report the halt position
  *found = FALSE;
  *found_offset = 0;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE TestTextStore::RetrieveRequestedAttrs(ULONG count,
                                                                TS_ATTRVAL* attributes,
                                                                ULONG* fetched) {
  (void)count;
  (void)attributes;
  if (fetched == nullptr) {
    return E_INVALIDARG;
  }
  *fetched = 0;  // nothing was stored by the Request*Attr* calls
  return S_OK;
}

HRESULT STDMETHODCALLTYPE TestTextStore::GetActiveView(TsViewCookie* view) {
  if (view == nullptr) {
    return E_INVALIDARG;
  }
  *view = 1;  // one synthetic view; it has no layout
  return S_OK;
}

HRESULT STDMETHODCALLTYPE TestTextStore::GetACPFromPoint(TsViewCookie view,
                                                          const POINT* point, DWORD flags,
                                                          LONG* position) {
  (void)view;
  (void)point;
  (void)flags;
  if (position == nullptr) {
    return E_INVALIDARG;
  }
  *position = 0;
  return TS_E_NOLAYOUT;  // there is no layout to hit-test against
}

HRESULT STDMETHODCALLTYPE TestTextStore::GetTextExt(TsViewCookie view, LONG start, LONG end,
                                                     RECT* rect, BOOL* clipped) {
  (void)view;
  (void)start;
  (void)end;
  if (rect == nullptr || clipped == nullptr) {
    return E_INVALIDARG;
  }
  *rect = RECT{};
  *clipped = FALSE;
  return TS_E_NOLAYOUT;  // honestly no text geometry
}

HRESULT STDMETHODCALLTYPE TestTextStore::GetScreenExt(TsViewCookie view, RECT* rect) {
  (void)view;
  if (rect == nullptr) {
    return E_INVALIDARG;
  }
  *rect = RECT{};
  return TS_E_NOLAYOUT;
}

HRESULT STDMETHODCALLTYPE TestTextStore::GetWnd(TsViewCookie view, HWND* window) {
  (void)view;
  if (window == nullptr) {
    return E_INVALIDARG;
  }
  *window = nullptr;
  return E_NOTIMPL;  // the text store is not an edit control
}

// --- context-owner composition sink ---------------------------------------------------
HRESULT STDMETHODCALLTYPE TestTextStore::OnStartComposition(
    ITfCompositionView* composition, BOOL* ok) {
  (void)composition;
  ++counters_.composition_start_calls;
  if (ok == nullptr) {
    return E_INVALIDARG;
  }
  if (refuse_compositions_) {
    ++counters_.composition_refusals;
    *ok = FALSE;  // documented rejection: StartComposition returns S_OK and a null object
  } else {
    *ok = TRUE;
  }
  return S_OK;
}

HRESULT STDMETHODCALLTYPE TestTextStore::OnUpdateComposition(
    ITfCompositionView* composition, ITfRange* new_range) {
  (void)composition;
  (void)new_range;
  ++counters_.composition_update_calls;
  return S_OK;
}

HRESULT STDMETHODCALLTYPE TestTextStore::OnEndComposition(
    ITfCompositionView* composition) {
  (void)composition;
  ++counters_.composition_end_calls;
  return S_OK;
}

// --- test controls --------------------------------------------------------------------
void TestTextStore::SetMutationFault(MutationFault fault) {
  mutation_fault_ = fault;
}

void TestTextStore::SetAsyncLocks(bool enabled) {
  options_.async_locks = enabled;
  counters_.async_locks = enabled;
}

void TestTextStore::SetRefuseWriteLocks(bool enabled) {
  options_.refuse_write_locks = enabled;
  counters_.refuse_write_locks = enabled;
}

void TestTextStore::SetRefuseCompositions(bool refuse) {
  refuse_compositions_ = refuse;
}

std::wstring TestTextStore::text_utf16() const {
  return text_utf16_;
}

LONG TestTextStore::text_length_utf16() const {
  return static_cast<LONG>(text_utf16_.size());
}

LONG TestTextStore::selection_start_utf16() const {
  return selection_.acpStart;
}

LONG TestTextStore::selection_end_utf16() const {
  return selection_.acpEnd;
}

TestStoreDiagnostics TestTextStore::diagnostics() const {
  TestStoreDiagnostics copy = counters_;
  copy.sink_advised = sink_ != nullptr;
  copy.composition_sink_queried = composition_sink_queried_;
  copy.lock_held = lock_held_;
  copy.pending_async_lock = pending_async_lock_;
  copy.read_only = options_.read_only;
  copy.async_locks = options_.async_locks;
  copy.refuse_write_locks = options_.refuse_write_locks;
  copy.text_length_utf16 = static_cast<LONG>(text_utf16_.size());
  copy.selection_start_utf16 = selection_.acpStart;
  copy.selection_end_utf16 = selection_.acpEnd;
  copy.reference_count = static_cast<ULONG>(reference_count_.load());
  return copy;
}

int TestTextStore::LiveInstanceCount() {
  return live_instances_.load();
}

int TestTextStore::LiveAdvisedSinkCount() {
  return live_advised_sinks_.load();
}

std::uint64_t TestTextStore::ForcedSinkReleaseCount() {
  return forced_sink_releases_.load();
}

long TestTextStore::LastInstanceReferenceCount() {
  TestTextStore* instance = last_instance_.load();
  return instance != nullptr ? instance->reference_count_.load() : 0;
}


// --- helpers --------------------------------------------------------------------------
bool TestTextStore::TextIsWellFormedUtf16(const WCHAR* text, ULONG cch) const {
  if (cch == 0) {
    return true;
  }
  if (text == nullptr) {
    return false;
  }
  for (ULONG index = 0; index < cch; ++index) {
    const wchar_t unit = text[index];
    if (unit == kEmbeddedPlaceholder) {
      return false;
    }
    if (IsHighSurrogate(unit)) {
      if (index + 1 >= cch || !IsLowSurrogate(text[index + 1])) {
        return false;
      }
      ++index;  // consume the low half
    } else if (IsLowSurrogate(unit)) {
      return false;
    }
  }
  return true;
}

void TestTextStore::ReplaceText(LONG start, LONG end, const WCHAR* text, ULONG cch) {
  if (cch == 0) {
    text_utf16_.erase(static_cast<std::size_t>(start),
                      static_cast<std::size_t>(end - start));
  } else {
    text_utf16_.replace(static_cast<std::size_t>(start),
                        static_cast<std::size_t>(end - start), text,
                        static_cast<std::size_t>(cch));
  }
  AdjustSelectionAfterReplace(start, end, start + static_cast<LONG>(cch));
  counters_.text_length_utf16 = static_cast<LONG>(text_utf16_.size());
}

void TestTextStore::AdjustSelectionAfterReplace(LONG start, LONG end, LONG new_end) {
  const LONG delta = new_end - end;
  const LONG new_length = static_cast<LONG>(text_utf16_.size());
  auto adjust = [&](LONG position) {
    if (position >= end) {
      position += delta;
    } else if (position > start) {
      position = new_end;
    }
    return std::max<LONG>(0, std::min<LONG>(position, new_length));
  };
  selection_.acpStart = adjust(selection_.acpStart);
  selection_.acpEnd = adjust(selection_.acpEnd);
  if (selection_.acpStart > selection_.acpEnd) {
    std::swap(selection_.acpStart, selection_.acpEnd);
  }
  counters_.selection_start_utf16 = selection_.acpStart;
  counters_.selection_end_utf16 = selection_.acpEnd;
}

}  // namespace test
}  // namespace edit
}  // namespace tsf
}  // namespace epocanva
