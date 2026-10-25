// EpoCanva P3.7 -- the immutable edit plan handed to the TSF edit-session executor.
//
// WHAT A PLAN IS. A value-only description of one edit operation:
//   BeginPreedit   start a composition at a collapsed caret and insert the preedit text
//   UpdatePreedit  replace the text of the composition this writer started
//   Commit         replace the composition text with the committed text and end it, or --
//                  when no composition exists -- insert the committed text once at the
//                  collapsed caret (the no-preedit commit path)
//   Cancel         remove the text of the composition this writer started and end it
//   EndOwnedComposition
//                  end the composition this writer started WITHOUT SetText: the displayed
//                  text stays as ordinary document text. Used only for retirement cleanup,
//                  after the range was verified to hold exactly what this writer last wrote.
//
// WHAT A PLAN IS NOT. It contains no COM pointer, no ITfContext, no ITfRange, no edit
// cookie and no reference to a worker thread. The executor may move/copy it freely; the
// worker-side state lives only in CompositionEditSession and CompositionWriter. It also
// contains no user-visible diagnostic: text is held as immutable UTF-16 code units, and the
// only strings a rejected build produces are status names, never the text.
//
// UNITS (AGENTS.md section 5). `text_utf16` is UTF-16 code units. `selection_start_utf16` /
// `selection_end_utf16` are ACP positions, i.e. UTF-16 code-unit offsets in the document.
// They are not byte offsets, not caret columns and not grapheme clusters. Only a collapsed
// selection is representable (start == end), because this round deliberately supports only a
// controlled folded insertion point. A non-collapsed selection is not expressible rather than
// silently collapsed.
//
// VALIDATION AT BUILD TIME. The text is converted with the P3.2 strict converter
// (platforms/windows/text/text_encoding.*): malformed UTF-8, an unpaired surrogate or a
// length that does not fit TSF's LONG are refused here, with an empty plan. The converter's
// own status is preserved in the text-free diagnostic. U+FFFC (the TSF embedded-object
// placeholder) is refused because ITfRange::SetText documents that it must not be passed
// there. Nothing is trimmed, normalised or clamped.
//
// IDENTITY AND GENERATION. Every plan names the ContextIdentity it was built for. The
// executor checks that identity again inside the granted lock; a plan for another context or
// an older generation is Stale and is never submitted to TSF.
#ifndef EPOCANVA_WINDOWS_TSF_EDIT_EDIT_PLAN_H_
#define EPOCANVA_WINDOWS_TSF_EDIT_EDIT_PLAN_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace epocanva {
namespace tsf {
namespace edit {

enum class EditOperation : std::uint8_t {
  kBeginPreedit = 0,
  kUpdatePreedit,
  kCommit,
  kCancel,
  kEndOwnedComposition,
};

const char* DescribeEditOperation(EditOperation operation);

// Opaque identity of one bound TSF context. `context_id` is assigned by the host; `generation`
// changes on every invalidation/rebind. Both must be non-zero for a plan to be built.
struct ContextIdentity {
  std::uint64_t context_id = 0;
  std::uint64_t generation = 0;
};

bool operator==(const ContextIdentity& left, const ContextIdentity& right);
bool operator!=(const ContextIdentity& left, const ContextIdentity& right);

enum class EditPlanStatus : std::uint8_t {
  kOk = 0,
  kInvalidContextIdentity,
  kInvalidUtf8,
  kTextTooLong,
  // A negative selection offset.
  kInvalidSelection,
  // The caller tried to describe a non-collapsed selection. Only a collapsed insertion
  // point is supported in this round.
  kSelectionNotCollapsed,
  // U+FFFC cannot be written through ITfRange::SetText.
  kUnsupportedCharacter,
};

const char* DescribeEditPlanStatus(EditPlanStatus status);

class EditPlan;
struct EditPlanBuildResult;

// Immutable after construction. Copyable so the executor can hold a snapshot while the caller
// keeps its own value.
class EditPlan {
 public:
  EditPlan(const EditPlan&) = default;
  EditPlan(EditPlan&&) noexcept = default;
  EditPlan& operator=(const EditPlan&) = default;
  EditPlan& operator=(EditPlan&&) noexcept = default;
  ~EditPlan() = default;

  // Begin/Commit require a collapsed caret; `selection_start_utf16 == selection_end_utf16`.
  static EditPlanBuildResult BuildBegin(ContextIdentity context,
                                        std::string_view preedit_utf8,
                                        std::int32_t selection_start_utf16,
                                        std::int32_t selection_end_utf16);
  static EditPlanBuildResult BuildUpdate(ContextIdentity context,
                                         std::string_view preedit_utf8);
  static EditPlanBuildResult BuildCommit(ContextIdentity context,
                                         std::string_view committed_utf8,
                                         std::int32_t selection_start_utf16,
                                         std::int32_t selection_end_utf16);
  static EditPlanBuildResult BuildCancel(ContextIdentity context);
  // Retirement cleanup: no text, no caret. The writer verifies its own composition range
  // against its last-written text and ends only that composition.
  static EditPlanBuildResult BuildEndOwnedComposition(ContextIdentity context);

  EditOperation operation() const { return operation_; }
  ContextIdentity context() const { return context_; }
  const std::wstring& text_utf16() const { return text_utf16_; }
  // -1 when the operation does not carry a caret (Update, Cancel).
  std::int32_t selection_start_utf16() const { return selection_start_utf16_; }
  std::int32_t selection_end_utf16() const { return selection_end_utf16_; }
  std::uint64_t sequence() const { return sequence_; }

 private:
  EditPlan(EditOperation operation, ContextIdentity context, std::wstring text_utf16,
           std::int32_t selection_start_utf16, std::int32_t selection_end_utf16,
           std::uint64_t sequence);

  EditOperation operation_;
  ContextIdentity context_;
  std::wstring text_utf16_;
  std::int32_t selection_start_utf16_ = -1;
  std::int32_t selection_end_utf16_ = -1;
  std::uint64_t sequence_ = 0;
};

// A build result either carries a plan (status == kOk) or a reason plus a text-free
// diagnostic. There is no partially valid plan. Defined after EditPlan because optional<T>
// requires a complete T.
struct EditPlanBuildResult {
  EditPlanStatus status = EditPlanStatus::kInvalidUtf8;
  std::optional<EditPlan> plan;
  std::string diagnostic;
  bool ok() const { return status == EditPlanStatus::kOk && plan.has_value(); }
};

}  // namespace edit
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_EDIT_EDIT_PLAN_H_
