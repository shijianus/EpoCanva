// EpoCanva P3.7 -- the TSF composition writer, executed only inside a granted edit cookie.
//
// WHAT IT DOES. Given an immutable EditPlan and the edit cookie TSF supplied to
// ITfEditSession::DoEditSession, this class performs exactly one operation:
//   Begin    ITfContextComposition::StartComposition at the verified collapsed caret, then
//            ITfRange::SetText for the preedit text;
//   Update   ITfRange::SetText over the composition range this writer owns;
//   Commit   ITfRange::SetText with the committed text and ITfComposition::EndComposition,
//            or ITfInsertAtSelection::InsertTextAtSelection when no composition exists;
//   Cancel   ITfRange::SetText(empty) over its own composition and EndComposition.
//
// SCOPE. It never writes outside its own composition range and never replaces the whole
// document. Before every Update/Commit/Cancel it reads the composition range back and
// compares it with the text this writer last wrote; a mismatch is refused
// (kCompositionRangeMismatch) instead of deleting or replacing text it does not own.
//
// UNITS. All offsets and lengths are UTF-16 code units (ACP). The caret is checked against
// the plan's expected collapsed position and against a scalar-value boundary: a position in
// the middle of a surrogate pair is refused before StartComposition. There is no clamp and no
// grapheme claim.
//
// FAILURE. After a failed SetText the range is read back:
//   * new text present  -> FailedAfterMutation, mutation_verified;
//   * old text present  -> FailedBeforeMutation, mutation_verified;
//   * unreadable        -> FailedAfterMutation, text_may_have_changed, not verified.
// There is no rollback claim and no automatic replay. A failed Begin before mutation ends the
// empty composition it created; a failed Update/Commit/Cancel keeps the composition alive so
// the caller can cancel it.
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

struct CompositionWriteOutcome {
  EditStatus status = EditStatus::kRejected;
  EditRejectReason reason = EditRejectReason::kNone;
  HRESULT operation_hr = S_OK;
  HRESULT end_composition_hr = S_OK;
  LONG composition_start_utf16 = -1;
  LONG composition_length_utf16 = -1;
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

  CompositionWriteOutcome ReplaceCompositionText(ITfContext* context,
                                                 const std::wstring& new_text,
                                                 bool verify_current_text,
                                                 TfEditCookie edit_cookie);
  HRESULT EndOwnComposition(TfEditCookie edit_cookie);
  void ReleaseCompositionAndSink();
  bool CompositionExternallyTerminated() const;
  void RefreshCompositionExtent();
  void UpdateCompositionExtent(ITfRange* range);

  ITfComposition* composition_ = nullptr;
  CompositionSink* composition_sink_ = nullptr;
  std::wstring current_text_utf16_;
  LONG composition_start_utf16_ = -1;
  LONG composition_length_utf16_ = -1;
};

}  // namespace edit
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_EDIT_COMPOSITION_WRITER_H_
