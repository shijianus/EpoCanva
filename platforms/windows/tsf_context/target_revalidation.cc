// EpoCanva P3.13 -- implementation of the pure target-revalidation decision and the
// lifetime-safe write-guard wrapper. See target_revalidation.h for the contract.
#include "target_revalidation.h"

#include <utility>

namespace epocanva {
namespace tsf {
namespace context {

const char* DescribeTargetRevalidationStatus(TargetRevalidationStatus status) {
  switch (status) {
    case TargetRevalidationStatus::kOk:
      return "Ok";
    case TargetRevalidationStatus::kClosed:
      return "Closed";
    case TargetRevalidationStatus::kThreadFocusUnconfirmed:
      return "ThreadFocusUnconfirmed";
    case TargetRevalidationStatus::kThreadFocusLost:
      return "ThreadFocusLost";
    case TargetRevalidationStatus::kNoDocumentManager:
      return "NoDocumentManager";
    case TargetRevalidationStatus::kNoTopContext:
      return "NoTopContext";
    case TargetRevalidationStatus::kContextMismatch:
      return "ContextMismatch";
    case TargetRevalidationStatus::kGenerationMismatch:
      return "GenerationMismatch";
    case TargetRevalidationStatus::kQueryFailed:
      return "QueryFailed";
  }
  return "UnknownTargetRevalidationStatus";
}

const char* DescribeThreadFocusEvidence(ThreadFocusEvidence evidence) {
  switch (evidence) {
    case ThreadFocusEvidence::kNone:
      return "none";
    case ThreadFocusEvidence::kNotificationSet:
      return "thread-focus-notification";
    case ThreadFocusEvidence::kNotificationKill:
      return "thread-focus-kill-notification";
    case ThreadFocusEvidence::kQuerySet:
      return "query-revalidation";
    case ThreadFocusEvidence::kWin32ThreadFocus:
      return "win32-thread-focus-revalidation";
  }
  return "unknown-thread-focus-evidence";
}

const char* DescribeThreadFocusPolicy(ThreadFocusPolicy policy) {
  switch (policy) {
    case ThreadFocusPolicy::kStrictTsf:
      return "strict-tsf";
    case ThreadFocusPolicy::kLabWin32Fallback:
      return "lab-win32-fallback";
  }
  return "unknown-thread-focus-policy";
}

const char* DescribeTargetFailureStage(TargetFailureStage stage) {
  switch (stage) {
    case TargetFailureStage::kNone:
      return "none";
    case TargetFailureStage::kInvalidRequest:
      return "invalid-request";
    case TargetFailureStage::kThreadFocusQuery:
      return "thread-focus-query";
    case TargetFailureStage::kGetFocus:
      return "get-focus";
    case TargetFailureStage::kGetTop:
      return "get-top";
    case TargetFailureStage::kRegistry:
      return "registry";
  }
  return "unknown-target-failure-stage";
}

TargetRevalidation EvaluateTargetEvidence(const TargetEvidence& evidence) {
  TargetRevalidation result;
  result.evidence = evidence;

  // Thread focus. A notification that was superseded by a successful live query is reported
  // as a revalidation; a kill notification is never renamed as a query result. The Win32
  // window is consulted only under the explicitly requested lab policy AND only when the
  // TSF query itself succeeded with FALSE. A failed query is never turned into permission
  // by the fallback.
  const bool notification_confirms = evidence.thread_focus_notification_set;
  const bool query_confirms = evidence.thread_focus_query_ok && evidence.thread_focus;
  const bool fallback_confirms =
      evidence.policy == ThreadFocusPolicy::kLabWin32Fallback &&
      evidence.thread_focus_query_ok && !evidence.thread_focus &&
      evidence.win32_thread_focus;

  if (notification_confirms) {
    result.thread_focus_evidence = ThreadFocusEvidence::kNotificationSet;
  } else if (query_confirms) {
    result.thread_focus_evidence = ThreadFocusEvidence::kQuerySet;
  } else if (fallback_confirms) {
    result.thread_focus_evidence = ThreadFocusEvidence::kWin32ThreadFocus;
  } else if (evidence.thread_focus_notification_kill) {
    result.thread_focus_evidence = ThreadFocusEvidence::kNotificationKill;
  }

  if (!notification_confirms && !query_confirms && !fallback_confirms) {
    if (evidence.thread_focus_notification_kill) {
      // The kill notification is the deciding fact. A coincident query failure stays in
      // evidence; it is not the reason the target was refused.
      result.status = TargetRevalidationStatus::kThreadFocusLost;
      result.query_hr = S_OK;
      return result;
    }
    if (!evidence.thread_focus_query_ok) {
      result.status = TargetRevalidationStatus::kQueryFailed;
      result.failure_stage = TargetFailureStage::kThreadFocusQuery;
      result.query_hr = FAILED(evidence.thread_focus_query_hr)
                            ? evidence.thread_focus_query_hr
                            : E_UNEXPECTED;
      return result;
    }
    // The query succeeded and answered FALSE. This is a real focus fact, not an API
    // failure; strict mode refuses it here instead of using a Win32 fallback.
    result.status = TargetRevalidationStatus::kThreadFocusUnconfirmed;
    result.query_hr = S_OK;
    return result;
  }

  if (!evidence.has_document_manager) {
    if (FAILED(evidence.get_focus_hr)) {
      result.status = TargetRevalidationStatus::kQueryFailed;
      result.failure_stage = TargetFailureStage::kGetFocus;
      result.query_hr = evidence.get_focus_hr;
    } else {
      // GetFocus answered successfully and named no document manager. That is an empty
      // state, not an API failure.
      result.status = TargetRevalidationStatus::kNoDocumentManager;
      result.query_hr = S_OK;
    }
    return result;
  }
  if (!evidence.has_top_context) {
    if (FAILED(evidence.get_top_hr)) {
      result.status = TargetRevalidationStatus::kQueryFailed;
      result.failure_stage = TargetFailureStage::kGetTop;
      result.query_hr = evidence.get_top_hr;
    } else {
      result.status = TargetRevalidationStatus::kNoTopContext;
      result.query_hr = S_OK;
    }
    return result;
  }
  if (!evidence.top_context_matches) {
    result.status = TargetRevalidationStatus::kContextMismatch;
    result.query_hr = S_OK;
    return result;
  }
  if (!evidence.registry_query_ok) {
    result.status = TargetRevalidationStatus::kQueryFailed;
    result.failure_stage = TargetFailureStage::kRegistry;
    result.query_hr = E_FAIL;
    return result;
  }
  if (!evidence.registry_identity_matches) {
    result.status = TargetRevalidationStatus::kContextMismatch;
    result.query_hr = S_OK;
    return result;
  }
  if (!evidence.registry_generation_matches) {
    result.status = TargetRevalidationStatus::kGenerationMismatch;
    result.query_hr = S_OK;
    return result;
  }
  result.status = TargetRevalidationStatus::kOk;
  result.query_hr = S_OK;
  return result;
}

bool TargetContextsEqual(ITfContext* left, ITfContext* right) {
  if (left == nullptr || right == nullptr) {
    return false;
  }
  if (left == right) {
    return true;
  }
  IUnknown* left_unknown = nullptr;
  IUnknown* right_unknown = nullptr;
  const HRESULT left_hr =
      left->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&left_unknown));
  const HRESULT right_hr =
      right->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&right_unknown));
  const bool same = SUCCEEDED(left_hr) && SUCCEEDED(right_hr) &&
                    left_unknown != nullptr && left_unknown == right_unknown;
  if (left_unknown != nullptr) {
    left_unknown->Release();
  }
  if (right_unknown != nullptr) {
    right_unknown->Release();
  }
  return same;
}

namespace {

class FunctionTargetWriteGuard final : public TargetWriteGuard {
 public:
  explicit FunctionTargetWriteGuard(
      std::function<bool(ITfContext*, std::uint64_t, std::uint64_t)> check)
      : check_(std::move(check)) {}

  bool StillCurrent(ITfContext* context, std::uint64_t context_id,
                    std::uint64_t generation) const override {
    if (!check_) {
      return false;
    }
    try {
      return check_(context, context_id, generation);
    } catch (...) {
      // A guard that cannot answer refuses the write.
      return false;
    }
  }

 private:
  std::function<bool(ITfContext*, std::uint64_t, std::uint64_t)> check_;
};

}  // namespace

std::shared_ptr<TargetWriteGuard> MakeTargetWriteGuard(
    std::function<bool(ITfContext*, std::uint64_t, std::uint64_t)> check) {
  return std::make_shared<FunctionTargetWriteGuard>(std::move(check));
}

}  // namespace context
}  // namespace tsf
}  // namespace epocanva
