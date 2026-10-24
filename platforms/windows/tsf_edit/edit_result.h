// EpoCanva P3.7 -- result vocabulary of one edit plan.
//
// STATUS SEMANTICS (the distinctions the brief requires):
//   kPending              RequestEditSession reported TF_S_ASYNC. Nothing has been applied;
//                         only the *waiting* is established. The caller must keep pumping the
//                         STA message loop until the completion event is set.
//   kApplied              DoEditSession ran and the planned write succeeded.
//   kRejected             A precondition or policy refused the write before any TSF call
//                         could mutate text: stale identity is kStale instead, and an API
//                         failure with no text change is kFailedBeforeMutation.
//   kFailedBeforeMutation A TSF call failed and the text was verified unchanged.
//   kFailedAfterMutation  A TSF call failed after the backing text changed. There is no
//                         rollback claim and no automatic replay; the caller must decide.
//   kStale                The plan's context identity or generation no longer matches the
//                         bound context. The callback in that case performs no TSF call.
//
// SetText success followed by EndComposition failure is kFailedAfterMutation with
// `end_composition_hr` naming the second failure. This library does not retry and does not
// claim the text is reverted.
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
  kFailedAfterMutation,
  kStale,
};

const char* DescribeEditStatus(EditStatus status);

enum class EditRejectReason : std::uint8_t {
  kNone = 0,
  kNotBound,
  kIdentityMismatch,
  kStaleGeneration,
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
  kInsertFailed,
  kEndCompositionFailed,
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
  // Composition extent in UTF-16 code units, when a composition exists or existed.
  LONG composition_start_utf16 = -1;
  LONG composition_length_utf16 = -1;
  // True when the text may have changed because a mutating call failed. `mutation_verified`
  // says whether a read-back proved it (true) or the result is conservative (false).
  bool text_may_have_changed = false;
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
