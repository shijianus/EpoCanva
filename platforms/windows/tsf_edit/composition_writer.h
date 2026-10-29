// EpoCanva P3.7 -- the TSF composition writer, executed only inside a granted edit cookie.
//
// WHAT IT DOES. Given an immutable EditPlan and the edit cookie TSF supplied to
// ITfEditSession::DoEditSession, this class performs exactly one operation:
//   Begin    ITfContextComposition::StartComposition at the verified collapsed caret, then
//            ITfRange::SetText for the preedit text;
//   Update   ITfRange::SetText over the composition range this writer owns;
//   Commit   ITfRange::SetText with the committed text and ITfComposition::EndComposition,
//            or ITfInsertAtSelection::InsertTextAtSelection when no composition exists;
//   Cancel   ITfRange::SetText(empty) over its own composition and EndComposition;
//   EndOwnedComposition
//            verify the range still holds exactly what this writer last wrote, then call
//            ITfComposition::EndComposition without any SetText. The displayed text stays
//            in the document as ordinary text. This is the only retirement cleanup path and
//            it never appends a commit, accepts a suggestion or clears unknown text.
//
// SCOPE. It never writes outside its own composition range and never replaces the whole
// document. Before every Update/Commit/Cancel it reads the composition range back and
// compares it with the text this writer last wrote; a mismatch is refused
// (kCompositionRangeMismatch) instead of deleting or replacing text it does not own. The
// only reads it performs are bounded: the collapsed selection, one code unit on each side of
// the caret, and its own composition range. It never reads the document as a whole and never
// reads anything to classify a failed write.
//
// UNITS. All offsets and lengths are UTF-16 code units (ACP). The caret is checked against
// the plan's expected collapsed position and against a scalar-value boundary: a position in
// the middle of a surrogate pair is refused before StartComposition. There is no clamp and no
// grapheme claim.
//
// FAILURE. A failed write call (SetText / InsertTextAtSelection) is kFailedPossiblyMutated:
// the HRESULT is not mutation evidence in either direction, and no read-back is attempted,
// because a TSF read-back returning the old text cannot prove the backing store is unchanged.
// A write call that succeeded followed by a failed later step (EndComposition) is
// kFailedAfterMutation, verified from the succeeded call itself. Either outcome latches an
// uncertain state: every further plan is refused (kUncertainTextState) and the recorded
// composition extent becomes unknown (-1), because the previous composition range can no
// longer be trusted. There is no rollback claim and no automatic replay. A failed Begin ends
// the composition it created as a separately recorded cleanup action
// (cleanup_end_composition) without another SetText; a successful cleanup end does not mean
// the plan was applied and never clears unknown text.
//
// LIFETIME. The held ITfComposition is released only by EndComposition success,
// ReleaseHeldObjects, or the destructor. After a successful EndComposition the object is
// never used again. All methods run on the bound STA thread inside an edit session.
#ifndef EPOCANVA_WINDOWS_TSF_EDIT_COMPOSITION_WRITER_H_
#define EPOCANVA_WINDOWS_TSF_EDIT_COMPOSITION_WRITER_H_

#include <windows.h>
#include <msctf.h>

#include <string>

#include "edit_plan.h"
#include "edit_result.h"

namespace epocanva {
namespace tsf {
namespace edit {

class CompositionSink;

// Lifetime evidence of the composition this writer most recently owned. Every field is
// recorded by the operation that observed or performed it; none is derived later from a
// member pointer, which a callback may already have cleared. `externally_terminated` is
// true only when the last lifetime ended through TSF's termination notification and not
// through this writer's own successful EndComposition.
struct CompositionOwnershipEvidence {
  bool ever_owned = false;             // StartComposition really produced a composition
  bool live = false;                   // an owned composition is held and not terminated
  bool externally_terminated = false;  // TSF reported the last own composition terminated elsewhere
  bool ended_by_writer = false;        // the last own composition ended through this writer's call
  std::uint64_t end_calls = 0;
  std::uint64_t end_successes = 0;
  std::uint64_t end_failures = 0;
  std::uint64_t termination_notifications = 0;
};

struct CompositionWriteOutcome {
  EditStatus status = EditStatus::kRejected;
  EditRejectReason reason = EditRejectReason::kNone;
  HRESULT operation_hr = S_OK;
  HRESULT end_composition_hr = S_OK;
  // True when EndComposition ran as failure cleanup inside a Begin, not as part of a normal
  // Commit/Cancel. Success closes the composition object only; it is not an Applied claim.
  bool cleanup_end_composition = false;
  // Per-operation retirement evidence, taken from this operation's own calls/notifications.
  bool owned_composition_at_entry = false;
  bool externally_terminated_at_entry = false;
  bool end_composition_issued = false;
  std::uint64_t termination_notifications_at_entry = 0;
  std::uint64_t termination_notifications_after_call = 0;
  LONG composition_start_utf16 = -1;
  LONG composition_length_utf16 = -1;
  // The live default-selection caret this operation actually read (ACP, UTF-16 code
  // units), or -1 when no selection could be read. Diagnostics only; never a write target.
  LONG observed_selection_start_utf16 = -1;
  LONG observed_selection_length_utf16 = -1;
  bool text_may_have_changed = false;
  bool mutation_verified = false;
  bool composition_active = false;
};

class CompositionWriter {
 public:
  CompositionWriter() = default;
  ~CompositionWriter();

  CompositionWriter(const CompositionWriter&) = delete;
  CompositionWriter& operator=(const CompositionWriter&) = delete;

  // Executes `plan` with the lock represented by `edit_cookie`. The caller has already
  // verified context identity/generation and that the context is writable.
  CompositionWriteOutcome Apply(const EditPlan& plan, ITfContext* context,
                                TfEditCookie edit_cookie);

  bool active() const;
  LONG composition_start_utf16() const { return composition_start_utf16_; }
  LONG composition_length_utf16() const { return composition_length_utf16_; }
  // Lifetime evidence for retirement attribution. Safe to call after the composition
  // object was released; the flags survive ReleaseCompositionAndSink().
  CompositionOwnershipEvidence ownership() const;
  // True once a write call failed or a post-write step failed: the backing text's exact
  // state is no longer known. While set, Apply refuses every plan. Not cleared by anything
  // except destruction; the recovery path is a new session.
  bool HasUncertainOutcome() const { return uncertain_; }

  // Releases the held composition reference without calling EndComposition. Used when the
  // context is already gone or the session is closing; the document owner will terminate an
  // abandoned composition. This is not a rollback and makes no text claim.
  void ReleaseHeldObjects();

 private:
  CompositionWriteOutcome Begin(const EditPlan& plan, ITfContext* context,
                                TfEditCookie edit_cookie);
  CompositionWriteOutcome Update(const EditPlan& plan, ITfContext* context,
                                 TfEditCookie edit_cookie);
  CompositionWriteOutcome Commit(const EditPlan& plan, ITfContext* context,
                                 TfEditCookie edit_cookie);
  CompositionWriteOutcome Cancel(ITfContext* context, TfEditCookie edit_cookie);
  // Ends this writer's composition only after re-reading its range and comparing it with
  // current_text_utf16_. No text call is issued on any path.
  CompositionWriteOutcome FinishOwnedComposition(ITfContext* context,
                                                  TfEditCookie edit_cookie);
  // Host-owned text: replace exactly the expected token ending at the collapsed caret.
  CompositionWriteOutcome ReplaceToken(const EditPlan& plan, ITfContext* context,
                                       TfEditCookie edit_cookie);

  CompositionWriteOutcome ReplaceCompositionText(ITfContext* context,
                                                 const std::wstring& new_text,
                                                 bool verify_current_text,
                                                 TfEditCookie edit_cookie);
  HRESULT EndOwnComposition(TfEditCookie edit_cookie, CompositionWriteOutcome* outcome);
  void ReleaseCompositionAndSink();
  bool CompositionExternallyTerminated() const;
  // Copies the sink's termination count into the writer and latches the external state.
  void ObserveSinkTermination();
  std::uint64_t TerminationNotifications() const;
  void RefreshCompositionExtent();
  void UpdateCompositionExtent(ITfRange* range);

  ITfComposition* composition_ = nullptr;
  CompositionSink* composition_sink_ = nullptr;
  std::wstring current_text_utf16_;
  LONG composition_start_utf16_ = -1;
  LONG composition_length_utf16_ = -1;
  bool uncertain_ = false;
  // Ownership history; deliberately NOT cleared by ReleaseCompositionAndSink().
  bool ever_owned_ = false;
  bool externally_terminated_ = false;
  bool ended_by_writer_ = false;
  std::uint64_t end_calls_ = 0;
  std::uint64_t end_successes_ = 0;
  std::uint64_t end_failures_ = 0;
  std::uint64_t termination_notifications_ = 0;
};

}  // namespace edit
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_EDIT_COMPOSITION_WRITER_H_
