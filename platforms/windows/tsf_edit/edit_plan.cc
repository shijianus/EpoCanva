// EpoCanva P3.7 -- immutable edit-plan construction and strict text validation.
#include "edit_plan.h"

#include <atomic>
#include <limits>
#include <utility>

#include "text_encoding.h"

namespace epocanva {
namespace tsf {
namespace edit {
namespace {

constexpr wchar_t kEmbeddedPlaceholder = 0xFFFC;
std::atomic<std::uint64_t> g_next_sequence{0};

EditPlanBuildResult Failure(EditPlanStatus status, std::string diagnostic) {
  EditPlanBuildResult result;
  result.status = status;
  result.diagnostic = std::move(diagnostic);
  return result;
}

// Strict UTF-8 -> UTF-16 through the P3.2 converter. The converter refuses malformed input
// with an empty output and a status; nothing here repairs, trims or normalises.
bool ConvertPlanText(std::string_view utf8, std::wstring* utf16, EditPlanBuildResult* result) {
  const auto converted =
      epocanva::windows::text::ConvertUtf8BytesToUtf16CodeUnits(utf8);
  if (!converted.ok()) {
    *result = Failure(
        EditPlanStatus::kInvalidUtf8,
        std::string("utf8 conversion: ") +
            epocanva::windows::text::DescribeConversionStatus(converted.status));
    return false;
  }
  if (converted.utf16_code_units.size() >
      static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    *result = Failure(EditPlanStatus::kTextTooLong,
                      "utf16 code-unit length exceeds INT32_MAX");
    return false;
  }
  for (const wchar_t unit : converted.utf16_code_units) {
    if (unit == kEmbeddedPlaceholder) {
      *result = Failure(EditPlanStatus::kUnsupportedCharacter,
                        "U+FFFC cannot be written through ITfRange::SetText");
      return false;
    }
  }
  *utf16 = converted.utf16_code_units;
  return true;
}

bool ValidIdentity(const ContextIdentity& context) {
  return context.context_id != 0 && context.generation != 0;
}

}  // namespace

bool operator==(const ContextIdentity& left, const ContextIdentity& right) {
  return left.context_id == right.context_id && left.generation == right.generation;
}

bool operator!=(const ContextIdentity& left, const ContextIdentity& right) {
  return !(left == right);
}

const char* DescribeEditOperation(EditOperation operation) {
  switch (operation) {
    case EditOperation::kBeginPreedit:
      return "BeginPreedit";
    case EditOperation::kUpdatePreedit:
      return "UpdatePreedit";
    case EditOperation::kCommit:
      return "Commit";
    case EditOperation::kCancel:
      return "Cancel";
    case EditOperation::kEndOwnedComposition:
      return "EndOwnedComposition";
  }
  return "UnknownOperation";
}

const char* DescribeEditPlanStatus(EditPlanStatus status) {
  switch (status) {
    case EditPlanStatus::kOk:
      return "Ok";
    case EditPlanStatus::kInvalidContextIdentity:
      return "InvalidContextIdentity";
    case EditPlanStatus::kInvalidUtf8:
      return "InvalidUtf8";
    case EditPlanStatus::kTextTooLong:
      return "TextTooLong";
    case EditPlanStatus::kInvalidSelection:
      return "InvalidSelection";
    case EditPlanStatus::kSelectionNotCollapsed:
      return "SelectionNotCollapsed";
    case EditPlanStatus::kUnsupportedCharacter:
      return "UnsupportedCharacter";
  }
  return "UnknownPlanStatus";
}

EditPlan::EditPlan(EditOperation operation, ContextIdentity context, std::wstring text_utf16,
                   std::int32_t selection_start_utf16, std::int32_t selection_end_utf16,
                   std::uint64_t sequence)
    : operation_(operation),
      context_(context),
      text_utf16_(std::move(text_utf16)),
      selection_start_utf16_(selection_start_utf16),
      selection_end_utf16_(selection_end_utf16),
      sequence_(sequence) {}

EditPlanBuildResult EditPlan::BuildBegin(ContextIdentity context,
                                         std::string_view preedit_utf8,
                                         std::int32_t selection_start_utf16,
                                         std::int32_t selection_end_utf16) {
  if (!ValidIdentity(context)) {
    return Failure(EditPlanStatus::kInvalidContextIdentity,
                   "context id and generation must be non-zero");
  }
  if (selection_start_utf16 < 0 || selection_end_utf16 < 0) {
    return Failure(EditPlanStatus::kInvalidSelection,
                   "selection offsets are UTF-16 code units and cannot be negative");
  }
  if (selection_start_utf16 != selection_end_utf16) {
    return Failure(EditPlanStatus::kSelectionNotCollapsed,
                   "only a collapsed (start == end) insertion point is supported");
  }
  std::wstring text;
  EditPlanBuildResult failure;
  if (!ConvertPlanText(preedit_utf8, &text, &failure)) {
    return failure;
  }
  EditPlan plan(EditOperation::kBeginPreedit, context, std::move(text),
                selection_start_utf16, selection_end_utf16,
                g_next_sequence.fetch_add(1) + 1);
  EditPlanBuildResult result;
  result.status = EditPlanStatus::kOk;
  result.plan = std::move(plan);
  return result;
}

EditPlanBuildResult EditPlan::BuildUpdate(ContextIdentity context,
                                          std::string_view preedit_utf8) {
  if (!ValidIdentity(context)) {
    return Failure(EditPlanStatus::kInvalidContextIdentity,
                   "context id and generation must be non-zero");
  }
  std::wstring text;
  EditPlanBuildResult failure;
  if (!ConvertPlanText(preedit_utf8, &text, &failure)) {
    return failure;
  }
  EditPlan plan(EditOperation::kUpdatePreedit, context, std::move(text), -1, -1,
                g_next_sequence.fetch_add(1) + 1);
  EditPlanBuildResult result;
  result.status = EditPlanStatus::kOk;
  result.plan = std::move(plan);
  return result;
}

EditPlanBuildResult EditPlan::BuildCommit(ContextIdentity context,
                                          std::string_view committed_utf8,
                                          std::int32_t selection_start_utf16,
                                          std::int32_t selection_end_utf16) {
  if (!ValidIdentity(context)) {
    return Failure(EditPlanStatus::kInvalidContextIdentity,
                   "context id and generation must be non-zero");
  }
  if (selection_start_utf16 < 0 || selection_end_utf16 < 0) {
    return Failure(EditPlanStatus::kInvalidSelection,
                   "selection offsets are UTF-16 code units and cannot be negative");
  }
  if (selection_start_utf16 != selection_end_utf16) {
    return Failure(EditPlanStatus::kSelectionNotCollapsed,
                   "only a collapsed (start == end) insertion point is supported");
  }
  std::wstring text;
  EditPlanBuildResult failure;
  if (!ConvertPlanText(committed_utf8, &text, &failure)) {
    return failure;
  }
  EditPlan plan(EditOperation::kCommit, context, std::move(text), selection_start_utf16,
                selection_end_utf16, g_next_sequence.fetch_add(1) + 1);
  EditPlanBuildResult result;
  result.status = EditPlanStatus::kOk;
  result.plan = std::move(plan);
  return result;
}

EditPlanBuildResult EditPlan::BuildCancel(ContextIdentity context) {
  if (!ValidIdentity(context)) {
    return Failure(EditPlanStatus::kInvalidContextIdentity,
                   "context id and generation must be non-zero");
  }
  EditPlan plan(EditOperation::kCancel, context, std::wstring(), -1, -1,
                g_next_sequence.fetch_add(1) + 1);
  EditPlanBuildResult result;
  result.status = EditPlanStatus::kOk;
  result.plan = std::move(plan);
  return result;
}

EditPlanBuildResult EditPlan::BuildEndOwnedComposition(ContextIdentity context) {
  if (!ValidIdentity(context)) {
    return Failure(EditPlanStatus::kInvalidContextIdentity,
                   "context id and generation must be non-zero");
  }
  EditPlan plan(EditOperation::kEndOwnedComposition, context, std::wstring(), -1, -1,
                g_next_sequence.fetch_add(1) + 1);
  EditPlanBuildResult result;
  result.status = EditPlanStatus::kOk;
  result.plan = std::move(plan);
  return result;
}

}  // namespace edit
}  // namespace tsf
}  // namespace epocanva
