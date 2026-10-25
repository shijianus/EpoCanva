// EpoCanva P3.7 -- result vocabulary of one edit plan.
//
// STATUS SEMANTICS (what the executor knows happened, never what the store "meant"):
//   kPending               RequestEditSession reported TF_S_ASYNC. Nothing has been applied;
//                          only the *waiting* is established. The caller must keep pumping the
//                          STA message loop until the completion event is set.
//   kApplied               DoEditSession ran and every step of the plan, including its text
//                          write, succeeded.
//   kRejected              A precondition or policy refused the plan before any call that
//                          could change text was issued. Stale identity is kStale instead.
//   kFailedBeforeMutation  A TSF call failed while no call that could have changed the text
//                          had been issued yet (selection read, boundary check,
//                          StartComposition, range acquisition, the pre-write verification
//                          read). The claim is about the calls this executor issued; it is
//                          not an observation of the backing text.
//   kFailedPossiblyMutated A call that could change text was issued and failed (SetText /
//                          InsertTextAtSelection returned a failure HRESULT). The backing
//                          text may or may not have changed. No HRESULT value -- including
//                          E_UNEXPECTED -- is treated as mutation evidence, and no read-back
//                          is performed to classify the failure: a TSF read-back returning
//                          the old text cannot prove the store is unchanged. Always
//                          text_may_have_changed = true, mutation_verified = false.
//   kFailedAfterMutation   At least one text write call succeeded and a later step of the
//                          same plan failed (SetText S_OK, then EndComposition failure).
//                          The mutation is known from the succeeded call itself; there is
//                          still no rollback claim and no automatic replay.
//   kStale                 The plan's context identity or generation no longer matches the
//                          bound context. The callback in that case performs no TSF call.
//
// UNCERTAIN STATE. After kFailedPossiblyMutated or kFailedAfterMutation the session refuses
// every further plan (kRejected / kUncertainTextState) and refuses to re-bind: the previous
// composition range is no longer trusted. The only way out is Close() or destroying the
// session. A cleanup EndComposition performed inside a failed Begin is recorded with
// cleanup_end_composition = true; its success means only that the composition object was
// closed, never that the plan was applied.
//
// NO TEXT. EditResult contains offsets and HRESULTs, never the edited text, so it can be
// logged as-is under AGENTS.md section 2.
#ifndef EPOCANVA_WINDOWS_TSF_EDIT_EDIT_RESULT_H_
#define EPOCANVA_WINDOWS_TSF_EDIT_EDIT_RESULT_H_

#include <windows.h>

#include <cstdint>

#include "edit_plan.h"

namespace epocanva {
namespace tsf {
namespace edit {

enum class EditStatus : std::uint8_t {
  kPending = 0,
  kApplied,
  kRejected,
  kFailedBeforeMutation,
  kFailedPossiblyMutated,
  kFailedAfterMutation,
  kStale,
};

const char* DescribeEditStatus(EditStatus status);

enum class EditRejectReason : std::uint8_t {
  kNone = 0,
  kNotBound,
  kIdentityMismatch,
  kStaleGeneration,
  kTargetChanged,  // P3.13: the context is no longer the thread's current TSF target
  kBusy,
  kPermissionDenied,
  kPermissionUnknown,
  kReadOnlyContext,
  kContextDisconnected,
  kContextLocked,
  kSyncUnavailable,
  kWriteLockRefused,
  kRequestFailed,
  kUnexpectedAsync,
  kNoSelection,
  kMultipleSelections,
  kSelectionNotCollapsed,
  kSelectionMoved,
  kSelectionInsideSurrogatePair,
  kBoundaryUnverified,
  kCompositionRejected,
  kCompositionStartFailed,
  kCompositionAlreadyActive,
  kNoComposition,
  kCompositionTerminated,
  kNothingToCommit,
  kRangeUnavailable,
  kCompositionRangeMismatch,
  kSetTextFailed,
  kInsertFailed,
  kEndCompositionFailed,
  kUncertainTextState,
  kContextStatusFailed,
};

const char* DescribeEditRejectReason(EditRejectReason reason);

struct EditResult {
  EditStatus status = EditStatus::kRejected;
  EditRejectReason reason = EditRejectReason::kNone;
  ContextIdentity context{};
  std::uint64_t plan_sequence = 0;
  // RequestEditSession's own return value.
  HRESULT request_hr = S_OK;
  // RequestEditSession's phrSession value (TF_S_ASYNC for a queued session, otherwise the
  // synchronous session result).
  HRESULT phr_session_hr = S_OK;
  // DoEditSession's return value, when the callback ran.
  HRESULT edit_session_hr = S_OK;
  // The first TSF operation that failed inside the writer (StartComposition, SetText,
  // InsertTextAtSelection). S_OK when no operation failed.
  HRESULT operation_hr = S_OK;
  // EndComposition's return value when it was attempted.
  HRESULT end_composition_hr = S_OK;
  // True when EndComposition ran as failure cleanup inside a Begin whose insert failed, as
  // opposed to the EndComposition that is part of a normal Commit/Cancel. A successful
  // cleanup end closes the composition object only; it never means the plan was applied.
  bool cleanup_end_composition = false;
  // Composition extent in UTF-16 code units, when a composition exists or existed. -1 means
  // unknown (never asserted after an unconfirmed write failure).
  LONG composition_start_utf16 = -1;
  LONG composition_length_utf16 = -1;
  // True when this plan's calls may have changed the backing text. For kApplied and
  // kRejected this stays false because the flags describe failure uncertainty.
  bool text_may_have_changed = false;
  // True only when the mutation is known from direct evidence -- a write call that returned
  // success. Never set from a read-back: a TSF read-back matching the old text proves
  // nothing about the backing store after a failed write call.
  bool mutation_verified = false;
  bool composition_active = false;
  // True when DoEditSession ran, i.e. TSF really granted a lock for this plan.
  bool lock_observed = false;
  std::uint32_t session_id = 0;

  bool pending() const { return status == EditStatus::kPending; }
  bool applied() const { return status == EditStatus::kApplied; }
};

}  // namespace edit
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_EDIT_EDIT_RESULT_H_
