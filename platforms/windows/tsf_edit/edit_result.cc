// EpoCanva P3.7 -- stable, text-free names for result values.
#include "edit_result.h"

namespace epocanva {
namespace tsf {
namespace edit {

const char* DescribeEditStatus(EditStatus status) {
  switch (status) {
    case EditStatus::kPending:
      return "Pending";
    case EditStatus::kApplied:
      return "Applied";
    case EditStatus::kRejected:
      return "Rejected";
    case EditStatus::kFailedBeforeMutation:
      return "FailedBeforeMutation";
    case EditStatus::kFailedPossiblyMutated:
      return "FailedPossiblyMutated";
    case EditStatus::kFailedAfterMutation:
      return "FailedAfterMutation";
    case EditStatus::kStale:
      return "Stale";
  }
  return "UnknownStatus";
}

const char* DescribeEditRejectReason(EditRejectReason reason) {
  switch (reason) {
    case EditRejectReason::kNone:
      return "None";
    case EditRejectReason::kNotBound:
      return "NotBound";
    case EditRejectReason::kIdentityMismatch:
      return "IdentityMismatch";
    case EditRejectReason::kStaleGeneration:
      return "StaleGeneration";
    case EditRejectReason::kBusy:
      return "Busy";
    case EditRejectReason::kPermissionDenied:
      return "PermissionDenied";
    case EditRejectReason::kPermissionUnknown:
      return "PermissionUnknown";
    case EditRejectReason::kReadOnlyContext:
      return "ReadOnlyContext";
    case EditRejectReason::kContextDisconnected:
      return "ContextDisconnected";
    case EditRejectReason::kContextLocked:
      return "ContextLocked";
    case EditRejectReason::kSyncUnavailable:
      return "SyncUnavailable";
    case EditRejectReason::kWriteLockRefused:
      return "WriteLockRefused";
    case EditRejectReason::kRequestFailed:
      return "RequestFailed";
    case EditRejectReason::kUnexpectedAsync:
      return "UnexpectedAsync";
    case EditRejectReason::kNoSelection:
      return "NoSelection";
    case EditRejectReason::kMultipleSelections:
      return "MultipleSelections";
    case EditRejectReason::kSelectionNotCollapsed:
      return "SelectionNotCollapsed";
    case EditRejectReason::kSelectionMoved:
      return "SelectionMoved";
    case EditRejectReason::kSelectionInsideSurrogatePair:
      return "SelectionInsideSurrogatePair";
    case EditRejectReason::kBoundaryUnverified:
      return "BoundaryUnverified";
    case EditRejectReason::kCompositionRejected:
      return "CompositionRejected";
    case EditRejectReason::kCompositionStartFailed:
      return "CompositionStartFailed";
    case EditRejectReason::kCompositionAlreadyActive:
      return "CompositionAlreadyActive";
    case EditRejectReason::kNoComposition:
      return "NoComposition";
    case EditRejectReason::kCompositionTerminated:
      return "CompositionTerminated";
    case EditRejectReason::kNothingToCommit:
      return "NothingToCommit";
    case EditRejectReason::kRangeUnavailable:
      return "RangeUnavailable";
    case EditRejectReason::kCompositionRangeMismatch:
      return "CompositionRangeMismatch";
    case EditRejectReason::kSetTextFailed:
      return "SetTextFailed";
    case EditRejectReason::kInsertFailed:
      return "InsertFailed";
    case EditRejectReason::kEndCompositionFailed:
      return "EndCompositionFailed";
    case EditRejectReason::kUncertainTextState:
      return "UncertainTextState";
    case EditRejectReason::kContextStatusFailed:
      return "ContextStatusFailed";
  }
  return "UnknownReason";
}

}  // namespace edit
}  // namespace tsf
}  // namespace epocanva
