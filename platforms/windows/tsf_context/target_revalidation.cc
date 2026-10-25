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

TargetRevalidation EvaluateTargetEvidence(const TargetEvidence& evidence) {
  TargetRevalidation result;
  result.evidence = evidence;

  // The evidence source names exactly where the thread-focus fact came from. A notification
  // that was superseded by a successful live query is reported as a revalidation; a kill
  // notification is never renamed as a query result.
  if (evidence.thread_focus_notification_set) {
    result.thread_focus_evidence = ThreadFocusEvidence::kNotificationSet;
  } else if (evidence.thread_focus_query_ok && evidence.thread_focus) {
    result.thread_focus_evidence = ThreadFocusEvidence::kQuerySet;
  } else if (evidence.win32_thread_focus) {
    result.thread_focus_evidence = ThreadFocusEvidence::kWin32ThreadFocus;
  } else if (evidence.thread_focus_notification_kill) {
    result.thread_focus_evidence = ThreadFocusEvidence::kNotificationKill;
  }

  const bool thread_focus_confirmed =
      evidence.thread_focus_notification_set || evidence.thread_focus ||
      evidence.win32_thread_focus;
  if (!thread_focus_confirmed) {
    result.status = evidence.thread_focus_notification_kill
                        ? TargetRevalidationStatus::kThreadFocusLost
                        : TargetRevalidationStatus::kThreadFocusUnconfirmed;
    return result;
  }
  if (!evidence.has_document_manager) {
    result.status = TargetRevalidationStatus::kNoDocumentManager;
    return result;
  }
  if (!evidence.has_top_context) {
    result.status = TargetRevalidationStatus::kNoTopContext;
    return result;
  }
  if (!evidence.top_context_matches) {
    result.status = TargetRevalidationStatus::kContextMismatch;
    return result;
  }
  if (!evidence.registry_identity_matches) {
    result.status = TargetRevalidationStatus::kContextMismatch;
    return result;
  }
  if (!evidence.registry_generation_matches) {
    result.status = TargetRevalidationStatus::kGenerationMismatch;
    return result;
  }
  result.status = TargetRevalidationStatus::kOk;
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
