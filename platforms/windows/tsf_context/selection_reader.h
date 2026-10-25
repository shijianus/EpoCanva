// EpoCanva P3.10 -- real TSF selection snapshots.
//
// WHAT IS READ. The default selection's ACP extent, from the real ITfContext:
//
//   ITfContext::GetSelection(ec, TF_DEFAULT_SELECTION, 1, &selection, &fetched)
//   -> the TF_SELECTION's range is QI'd for ITfRangeACP
//   -> ITfRangeACP::GetExtent(&anchor_acp, &length_acp)
//
// `anchor_acp` and `length_acp` are ACP positions in UTF-16 code units -- the unit TSF's
// ACP (textstor.h) speaks, not bytes and not grapheme clusters. That is why every field in
// this file carries `_utf16`.
//
// WHAT IS ACCEPTED. Only a collapsed insertion point (length_acp == 0), from the default
// selection, over a range that exposes ITfRangeACP, with a non-negative anchor and an
// active end of TF_AE_START/TF_AE_END. Everything else is refused with a distinct status:
//   * length_acp != 0                 -> kNonCollapsed
//   * no ITfRangeACP / fInterimChar   -> kUnsupportedShape (a composed interim character
//     or a non-ACP range is not a stable insertion point)
//   * TF_AE_NONE                      -> kUnsupportedShape (the default selection's active
//     end is not identifiable; this layer does not guess which end to use)
//   * failed call / refused lock      -> kSessionUnavailable / kContextUnavailable
// There is NO clamp and NO fallback offset: a caller that cannot get kOk must close
// admission for the context instead of reusing an older caret.
//
// THE COOKIE. ITfContext::GetSelection requires a real edit cookie. This module offers two
// real sources and invents none:
//   * ReadSelectionWithCookie() uses a cookie the caller already holds (for example the
//     ecReadOnly of an ITfTextEditSink::OnEndEdit callback -- read-only access is exactly
//     what that callback permits);
//   * SelectionReader requests its own read-only edit session with the production client
//     id: TF_ES_READ | TF_ES_SYNC first, and the documented TF_ES_ASYNC fallback when the
//     synchronous grant is unavailable. A pending read is never treated as a result.
//
// NO TEXT IS READ. GetSelection returns a range and its ACP extent; this file never calls
// GetText, never walks the document and never reads a clipboard.
#ifndef EPOCANVA_WINDOWS_TSF_CONTEXT_SELECTION_READER_H_
#define EPOCANVA_WINDOWS_TSF_CONTEXT_SELECTION_READER_H_

#include <windows.h>
#include <msctf.h>

#include <cstdint>
#include <memory>

namespace epocanva {
namespace tsf {
namespace context {

enum class SelectionReadStatus : std::uint8_t {
  kOk = 0,              // a collapsed insertion point; caret_utf16 is valid
  kNoSelection,         // the document reports no default selection
  kNonCollapsed,        // the default selection is a range, not an insertion point
  kUnsupportedShape,    // not an ACP range, an interim character, or no active end
  kSessionUnavailable,  // the read session could not be requested or the read failed
  kContextUnavailable,  // null/disconnected context
  kClosed,              // the reader was shut down
};

const char* DescribeSelectionReadStatus(SelectionReadStatus status);

// ACP positions are UTF-16 code units. `caret_utf16` is meaningful only for kOk.
struct SelectionSnapshot {
  SelectionReadStatus status = SelectionReadStatus::kSessionUnavailable;
  LONG caret_utf16 = 0;
  bool ok() const { return status == SelectionReadStatus::kOk; }
};

// Reads the default selection with a cookie the caller already holds. Makes no TSF call
// outside that session and requests no new session, so it is legal inside OnEndEdit.
SelectionSnapshot ReadSelectionWithCookie(ITfContext* context, TfEditCookie edit_cookie);

// Value-only diagnostics. `last_status` is the enum value of the last completed read.
struct SelectionReaderDiagnostics {
  std::uint64_t reads_begun = 0;
  std::uint64_t reads_completed = 0;
  std::uint64_t reads_abandoned = 0;
  std::uint64_t reads_taken = 0;
  std::uint64_t read_sessions_sync = 0;
  std::uint64_t read_sessions_async = 0;
  std::uint64_t read_session_failures = 0;
  std::uint64_t callbacks_after_abandon = 0;
  std::uint64_t collapsed = 0;
  std::uint64_t no_selection = 0;
  std::uint64_t non_collapsed = 0;
  std::uint64_t unsupported_shape = 0;
  std::uint64_t unavailable = 0;
  int last_status = 0;
};

// One active read at a time. Begin() AddRefs the context for the read's duration and
// releases it when the read completes or is abandoned. A callback that runs after Abandon()
// touches only its own orphaned state (shared_ptr) and publishes nothing.
class SelectionReader {
 public:
  explicit SelectionReader(TfClientId client_id);
  ~SelectionReader();

  SelectionReader(const SelectionReader&) = delete;
  SelectionReader& operator=(const SelectionReader&) = delete;

  // Starts one read-only edit session. Returns false for a null context, a closed reader,
  // or while a previous read is still pending. A synchronous grant completes the read
  // before this returns; an asynchronous one leaves Pending() true.
  bool Begin(ITfContext* context);
  bool Pending() const;
  HANDLE CompletionEvent() const;  // non-null only while pending
  // Consumes the completed read. Call only when !Pending(); returns a kSessionUnavailable
  // snapshot if no completed read exists. Taking resets the reader so Begin() can run again.
  SelectionSnapshot Take();
  // Terminates the current read without a verdict. Never waits; a late callback publishes
  // nothing and no waiter of CompletionEvent() can hang.
  void Abandon();

  SelectionReaderDiagnostics Diagnostics() const;

 private:
  struct ReadState;
  friend class SelectionReadCallback;

  std::shared_ptr<SelectionReaderDiagnostics> counters_;
  std::shared_ptr<ReadState> active_;
  TfClientId client_id_ = 0;
};

}  // namespace context
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_CONTEXT_SELECTION_READER_H_
