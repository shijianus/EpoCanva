// EpoCanva P3.7 -- implementation of the composition writer.
#include "composition_writer.h"

#include <atomic>
#include <utility>

#include "module_lifetime.h"

namespace epocanva {
namespace tsf {
namespace edit {

// TSF requires a non-NULL ITfCompositionSink for ITfContextComposition::StartComposition in
// this build: with NULL it returns E_INVALIDARG even for a valid collapsed ACP range inside a
// granted write session (verified by the diag-start case during this phase). This sink is
// per-composition; it only records that TSF terminated the composition from the owner side, so
// the writer never reuses a terminated object.
class CompositionSink final : public ITfCompositionSink {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
    if (object == nullptr) {
      return E_INVALIDARG;
    }
    *object = nullptr;
    if (riid == IID_IUnknown || riid == IID_ITfCompositionSink) {
      *object = static_cast<ITfCompositionSink*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(reference_count_.fetch_add(1) + 1);
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const LONG remaining = reference_count_.fetch_sub(1) - 1;
    if (remaining == 0) {
      delete this;
    }
    return static_cast<ULONG>(remaining);
  }

  HRESULT STDMETHODCALLTYPE OnCompositionTerminated(
      TfEditCookie edit_cookie, ITfComposition* composition) override {
    (void)edit_cookie;
    (void)composition;
    terminated_.store(true);
    terminations_.fetch_add(1);
    return S_OK;
  }

  bool terminated() const { return terminated_.load(); }
  std::uint64_t termination_count() const { return terminations_.load(); }

 private:
  std::atomic<LONG> reference_count_{1};
  std::atomic<bool> terminated_{false};
  std::atomic<std::uint64_t> terminations_{0};
  // TSF may keep the sink after EndComposition; the module must stay loaded until its last
  // Release reaches this object.
  ModuleObjectGuard module_guard_{ModuleObjectKind::kCompositionSink};
};

namespace {

bool IsHighSurrogate(wchar_t unit) {
  return unit >= 0xD800 && unit <= 0xDBFF;
}

bool IsLowSurrogate(wchar_t unit) {
  return unit >= 0xDC00 && unit <= 0xDFFF;
}

enum class TextMatch {
  kMatches,
  kDiffers,
  kUnreadable,
};

// Reads at most expected.size() + 1 code units from `range` and compares them. The +1 makes a
// longer range differ rather than truncate to a false match. This is the pre-write
// verification of the writer's own composition range; it is never used to classify a failure.
TextMatch ReadRangeText(ITfRange* range, TfEditCookie edit_cookie,
                        const std::wstring& expected, HRESULT* read_hr) {
  std::wstring buffer(expected.size() + 1, L'\0');
  ULONG got = 0;
  *read_hr = range->GetText(edit_cookie, 0, buffer.data(),
                            static_cast<ULONG>(buffer.size()), &got);
  if (FAILED(*read_hr)) {
    return TextMatch::kUnreadable;
  }
  buffer.resize(got);
  return buffer == expected ? TextMatch::kMatches : TextMatch::kDiffers;
}

bool ReadOneUnit(ITfRange* range, TfEditCookie edit_cookie, wchar_t* unit,
                 HRESULT* read_hr) {
  ULONG got = 0;
  *read_hr = range->GetText(edit_cookie, 0, unit, 1, &got);
  return SUCCEEDED(*read_hr) && got == 1;
}

bool ReadCollapsedSelection(ITfContext* context, TfEditCookie edit_cookie,
                            ITfRange** range, LONG* start_utf16, LONG* length_utf16,
                            EditRejectReason* reason, HRESULT* result_hr) {
  *range = nullptr;
  *start_utf16 = 0;
  *length_utf16 = 0;
  TF_SELECTION selection = {};
  ULONG fetched = 0;
  HRESULT hr = context->GetSelection(edit_cookie, TF_DEFAULT_SELECTION, 1, &selection,
                                     &fetched);
  if (FAILED(hr)) {
    *reason = EditRejectReason::kNoSelection;
    *result_hr = hr;
    return false;
  }
  if (fetched != 1 || selection.range == nullptr) {
    if (selection.range != nullptr) {
      selection.range->Release();
    }
    *reason = fetched == 0 ? EditRejectReason::kNoSelection
                           : EditRejectReason::kMultipleSelections;
    *result_hr = E_UNEXPECTED;
    return false;
  }

  ITfRangeACP* acp = nullptr;
  hr = selection.range->QueryInterface(IID_ITfRangeACP,
                                       reinterpret_cast<void**>(&acp));
  if (FAILED(hr) || acp == nullptr) {
    selection.range->Release();
    *reason = EditRejectReason::kRangeUnavailable;
    *result_hr = hr;
    return false;
  }
  LONG anchor = 0;
  LONG cch = 0;
  hr = acp->GetExtent(&anchor, &cch);
  acp->Release();
  if (FAILED(hr) || anchor < 0 || cch < 0) {
    selection.range->Release();
    *reason = EditRejectReason::kRangeUnavailable;
    *result_hr = hr;
    return false;
  }
  *range = selection.range;
  *start_utf16 = anchor;
  *length_utf16 = cch;
  return true;
}

// Refuses a caret in the middle of a surrogate pair. Uses only ITfRange::Clone, ShiftStart,
// ShiftEnd and GetText, so it needs no ACP back door. A boundary that cannot be read is
// reported as unverified and refused; it is never assumed safe.
EditRejectReason CheckScalarBoundary(ITfRange* caret, TfEditCookie edit_cookie,
                                     bool* verified, HRESULT* result_hr) {
  *verified = false;
  *result_hr = S_OK;
  ITfRange* left = nullptr;
  ITfRange* right = nullptr;
  HRESULT hr = caret->Clone(&left);
  if (FAILED(hr) || left == nullptr) {
    *result_hr = hr;
    return EditRejectReason::kBoundaryUnverified;
  }
  hr = caret->Clone(&right);
  if (FAILED(hr) || right == nullptr) {
    left->Release();
    *result_hr = hr;
    return EditRejectReason::kBoundaryUnverified;
  }

  LONG shifted = 0;
  wchar_t left_unit = 0;
  wchar_t right_unit = 0;
  bool have_left = false;
  bool have_right = false;

  hr = left->ShiftStart(edit_cookie, -1, &shifted, nullptr);
  if (FAILED(hr) || (shifted != 0 && shifted != -1)) {
    left->Release();
    right->Release();
    *result_hr = hr;
    return EditRejectReason::kBoundaryUnverified;
  }
  if (shifted == -1) {
    if (!ReadOneUnit(left, edit_cookie, &left_unit, &hr)) {
      left->Release();
      right->Release();
      *result_hr = hr;
      return EditRejectReason::kBoundaryUnverified;
    }
    have_left = true;
  }

  shifted = 0;
  hr = right->ShiftEnd(edit_cookie, 1, &shifted, nullptr);
  if (FAILED(hr) || (shifted != 0 && shifted != 1)) {
    left->Release();
    right->Release();
    *result_hr = hr;
    return EditRejectReason::kBoundaryUnverified;
  }
  if (shifted == 1) {
    if (!ReadOneUnit(right, edit_cookie, &right_unit, &hr)) {
      left->Release();
      right->Release();
      *result_hr = hr;
      return EditRejectReason::kBoundaryUnverified;
    }
    have_right = true;
  }

  left->Release();
  right->Release();
  *verified = true;
  if (have_left && have_right && IsHighSurrogate(left_unit) &&
      IsLowSurrogate(right_unit)) {
    return EditRejectReason::kSelectionInsideSurrogatePair;
  }
  return EditRejectReason::kNone;
}

}  // namespace

CompositionWriter::~CompositionWriter() {
  ReleaseHeldObjects();
}

bool CompositionWriter::active() const {
  if (composition_ == nullptr) {
    return false;
  }
  return !(composition_sink_ != nullptr && composition_sink_->terminated());
}

bool CompositionWriter::CompositionExternallyTerminated() const {
  return composition_sink_ != nullptr && composition_sink_->terminated();
}

void CompositionWriter::ObserveSinkTermination() {
  if (composition_sink_ == nullptr) {
    return;
  }
  const std::uint64_t count = composition_sink_->termination_count();
  if (count > termination_notifications_) {
    termination_notifications_ = count;
  }
  if (count > 0) {
    externally_terminated_ = true;
  }
}

std::uint64_t CompositionWriter::TerminationNotifications() const {
  if (composition_sink_ == nullptr) {
    return termination_notifications_;
  }
  const std::uint64_t count = composition_sink_->termination_count();
  return count > termination_notifications_ ? count : termination_notifications_;
}

CompositionOwnershipEvidence CompositionWriter::ownership() const {
  CompositionOwnershipEvidence evidence;
  evidence.ever_owned = ever_owned_;
  evidence.live = active();
  evidence.ended_by_writer = ended_by_writer_;
  evidence.end_calls = end_calls_;
  evidence.end_successes = end_successes_;
  evidence.end_failures = end_failures_;
  evidence.termination_notifications = TerminationNotifications();
  // A notified termination can arrive before any Apply/End call observes it. Derive it here
  // from the held sink, but never let a notification that belongs to this writer's own
  // successful EndComposition masquerade as a third-party end.
  evidence.externally_terminated =
      externally_terminated_ ||
      (!ended_by_writer_ && CompositionExternallyTerminated());
  return evidence;
}

void CompositionWriter::ReleaseCompositionAndSink() {
  if (composition_ != nullptr) {
    composition_->Release();
    composition_ = nullptr;
  }
  if (composition_sink_ != nullptr) {
    composition_sink_->Release();
    composition_sink_ = nullptr;
  }
  current_text_utf16_.clear();
  composition_start_utf16_ = -1;
  composition_length_utf16_ = -1;
}

void CompositionWriter::ReleaseHeldObjects() {
  ReleaseCompositionAndSink();
}

CompositionWriteOutcome CompositionWriter::Apply(const EditPlan& plan,
                                                 ITfContext* context,
                                                 TfEditCookie edit_cookie) {
  if (uncertain_) {
    // A previous write's effect on the backing text could not be confirmed. The recorded
    // composition range and the cached last-written text are no longer trustworthy, so no
    // further plan -- not even a Cancel -- is executed on this session. The caller recovers
    // by closing this session and creating a new one.
    CompositionWriteOutcome outcome;
    outcome.status = EditStatus::kRejected;
    outcome.reason = EditRejectReason::kUncertainTextState;
    return outcome;
  }
  ObserveSinkTermination();
  if (composition_ != nullptr && CompositionExternallyTerminated()) {
    // The context owner terminated the composition. Do not call any method on the terminated
    // object and do not fall through to the no-preedit insert path.
    CompositionWriteOutcome outcome;
    outcome.owned_composition_at_entry = true;
    outcome.externally_terminated_at_entry = true;
    outcome.termination_notifications_at_entry = termination_notifications_;
    outcome.termination_notifications_after_call = TerminationNotifications();
    ReleaseCompositionAndSink();
    outcome.status = EditStatus::kRejected;
    outcome.reason = EditRejectReason::kCompositionTerminated;
    return outcome;
  }
  switch (plan.operation()) {
    case EditOperation::kBeginPreedit:
      return Begin(plan, context, edit_cookie);
    case EditOperation::kUpdatePreedit:
      return Update(plan, context, edit_cookie);
    case EditOperation::kCommit:
      return Commit(plan, context, edit_cookie);
    case EditOperation::kCancel:
      return Cancel(context, edit_cookie);
    case EditOperation::kEndOwnedComposition:
      return FinishOwnedComposition(context, edit_cookie);
  }
  CompositionWriteOutcome outcome;
  outcome.status = EditStatus::kRejected;
  outcome.reason = EditRejectReason::kRequestFailed;
  return outcome;
}

CompositionWriteOutcome CompositionWriter::Begin(const EditPlan& plan,
                                                 ITfContext* context,
                                                 TfEditCookie edit_cookie) {
  CompositionWriteOutcome outcome;
  if (composition_ != nullptr) {
    outcome.reason = EditRejectReason::kCompositionAlreadyActive;
    return outcome;
  }

  ITfRange* caret = nullptr;
  LONG start = 0;
  LONG length = 0;
  EditRejectReason reason = EditRejectReason::kNone;
  HRESULT hr = S_OK;
  if (!ReadCollapsedSelection(context, edit_cookie, &caret, &start, &length, &reason,
                              &hr)) {
    outcome.status = EditStatus::kRejected;
    outcome.reason = reason;
    outcome.operation_hr = hr;
    return outcome;
  }
  if (length != 0) {
    caret->Release();
    outcome.reason = EditRejectReason::kSelectionNotCollapsed;
    return outcome;
  }
  if (start != plan.selection_start_utf16() ||
      plan.selection_start_utf16() != plan.selection_end_utf16()) {
    caret->Release();
    outcome.reason = EditRejectReason::kSelectionMoved;
    return outcome;
  }
  bool boundary_verified = false;
  reason = CheckScalarBoundary(caret, edit_cookie, &boundary_verified, &hr);
  if (reason == EditRejectReason::kSelectionInsideSurrogatePair) {
    caret->Release();
    outcome.reason = reason;
    return outcome;
  }
  if (!boundary_verified) {
    caret->Release();
    outcome.reason = EditRejectReason::kBoundaryUnverified;
    outcome.operation_hr = hr;
    return outcome;
  }

  ITfContextComposition* composition_service = nullptr;
  hr = context->QueryInterface(IID_ITfContextComposition,
                               reinterpret_cast<void**>(&composition_service));
  if (FAILED(hr) || composition_service == nullptr) {
    caret->Release();
    outcome.status = EditStatus::kFailedBeforeMutation;
    outcome.reason = EditRejectReason::kCompositionStartFailed;
    outcome.operation_hr = hr;
    return outcome;
  }

  CompositionSink* sink = new CompositionSink();
  ITfComposition* composition = nullptr;
  hr = composition_service->StartComposition(edit_cookie, caret, sink, &composition);
  composition_service->Release();
  caret->Release();
  if (FAILED(hr)) {
    sink->Release();
    outcome.status = EditStatus::kFailedBeforeMutation;
    outcome.reason = EditRejectReason::kCompositionStartFailed;
    outcome.operation_hr = hr;
    return outcome;
  }
  if (composition == nullptr) {
    // Documented: S_OK + NULL means the context owner rejected the composition.
    sink->Release();
    outcome.status = EditStatus::kRejected;
    outcome.reason = EditRejectReason::kCompositionRejected;
    return outcome;
  }

  composition_ = composition;
  composition_sink_ = sink;
  // A new owned composition starts a new lifetime: previous termination/end evidence does
  // not describe it. Notification counts are per lifetime as well, so a late notification
  // from the previous sink cannot be mistaken for one about this composition.
  ever_owned_ = true;
  externally_terminated_ = false;
  ended_by_writer_ = false;
  termination_notifications_ = 0;
  current_text_utf16_.clear();
  composition_start_utf16_ = start;
  composition_length_utf16_ = 0;

  if (plan.text_utf16().empty()) {
    outcome.status = EditStatus::kApplied;
    outcome.composition_active = true;
    outcome.composition_start_utf16 = start;
    outcome.composition_length_utf16 = 0;
    return outcome;
  }

  CompositionWriteOutcome replaced =
      ReplaceCompositionText(context, plan.text_utf16(),
                             /*verify_current_text=*/false, edit_cookie);
  if (replaced.status == EditStatus::kApplied) {
    replaced.composition_start_utf16 = composition_start_utf16_;
    replaced.composition_length_utf16 = composition_length_utf16_;
    replaced.composition_active = true;
    return replaced;
  }

  replaced.composition_start_utf16 = composition_start_utf16_;
  replaced.composition_length_utf16 = composition_length_utf16_;
  if (replaced.status == EditStatus::kFailedBeforeMutation ||
      replaced.status == EditStatus::kFailedPossiblyMutated ||
      replaced.status == EditStatus::kFailedAfterMutation) {
    // Begin failed after the composition object existed. Whatever the backing text now holds,
    // this composition is not a usable editing context: end it as a separate cleanup action,
    // without another SetText. This is not a rollback, it does not replay the insert, and it
    // never clears text whose current value is unknown. A successful cleanup end closes the
    // composition object only; it does not make the plan Applied. If ending fails, the
    // reference stays held and composition_active says so.
    const HRESULT end_hr = EndOwnComposition(edit_cookie, &replaced);
    replaced.end_composition_hr = end_hr;
    replaced.cleanup_end_composition = true;
    if (FAILED(end_hr)) {
      replaced.composition_active = true;
    } else {
      replaced.composition_active = false;
      replaced.composition_start_utf16 = start;
      // After kFailedPossiblyMutated the store's extent is unknown (-1): the failed write may
      // or may not have landed. Only a failure that predates every write call can report 0.
      replaced.composition_length_utf16 =
          replaced.status == EditStatus::kFailedBeforeMutation ? 0 : -1;
    }
  } else {
    replaced.composition_active = true;
  }
  return replaced;
}

CompositionWriteOutcome CompositionWriter::Update(const EditPlan& plan,
                                                  ITfContext* context,
                                                  TfEditCookie edit_cookie) {
  CompositionWriteOutcome outcome;
  if (composition_ == nullptr) {
    outcome.reason = EditRejectReason::kNoComposition;
    return outcome;
  }
  outcome = ReplaceCompositionText(context, plan.text_utf16(),
                                   /*verify_current_text=*/true, edit_cookie);
  outcome.composition_active = composition_ != nullptr;
  outcome.composition_start_utf16 = composition_start_utf16_;
  outcome.composition_length_utf16 = composition_length_utf16_;
  return outcome;
}

CompositionWriteOutcome CompositionWriter::Commit(const EditPlan& plan,
                                                  ITfContext* context,
                                                  TfEditCookie edit_cookie) {
  CompositionWriteOutcome outcome;
  if (composition_ != nullptr) {
    outcome = ReplaceCompositionText(context, plan.text_utf16(),
                                     /*verify_current_text=*/true, edit_cookie);
    if (outcome.status != EditStatus::kApplied) {
      outcome.composition_active = true;
      outcome.composition_start_utf16 = composition_start_utf16_;
      outcome.composition_length_utf16 = composition_length_utf16_;
      return outcome;
    }
    const LONG committed_start = composition_start_utf16_;
    const LONG committed_length = composition_length_utf16_;
    const HRESULT end_hr = EndOwnComposition(edit_cookie, &outcome);
    outcome.end_composition_hr = end_hr;
    outcome.composition_start_utf16 = committed_start;
    outcome.composition_length_utf16 = committed_length;
    if (FAILED(end_hr)) {
      // SetText succeeded; EndComposition failed. The text change is known from the
      // succeeded write call; the composition may still exist. No rollback and no replay is
      // attempted, and the session becomes uncertain: further plans are refused.
      outcome.status = EditStatus::kFailedAfterMutation;
      outcome.reason = EditRejectReason::kEndCompositionFailed;
      outcome.text_may_have_changed = true;
      outcome.mutation_verified = true;
      outcome.composition_active = true;
      uncertain_ = true;
      return outcome;
    }
    outcome.status = EditStatus::kApplied;
    outcome.composition_active = false;
    return outcome;
  }

  // No composition: the no-preedit commit path inserts the text exactly once at the
  // verified collapsed caret.
  if (plan.text_utf16().empty()) {
    outcome.reason = EditRejectReason::kNothingToCommit;
    return outcome;
  }
  ITfRange* caret = nullptr;
  LONG start = 0;
  LONG length = 0;
  EditRejectReason reason = EditRejectReason::kNone;
  HRESULT hr = S_OK;
  if (!ReadCollapsedSelection(context, edit_cookie, &caret, &start, &length, &reason,
                              &hr)) {
    outcome.reason = reason;
    outcome.operation_hr = hr;
    return outcome;
  }
  if (length != 0) {
    caret->Release();
    outcome.reason = EditRejectReason::kSelectionNotCollapsed;
    return outcome;
  }
  if (start != plan.selection_start_utf16() ||
      plan.selection_start_utf16() != plan.selection_end_utf16()) {
    caret->Release();
    outcome.reason = EditRejectReason::kSelectionMoved;
    return outcome;
  }
  bool boundary_verified = false;
  reason = CheckScalarBoundary(caret, edit_cookie, &boundary_verified, &hr);
  caret->Release();
  if (reason == EditRejectReason::kSelectionInsideSurrogatePair) {
    outcome.reason = reason;
    return outcome;
  }
  if (!boundary_verified) {
    outcome.reason = EditRejectReason::kBoundaryUnverified;
    outcome.operation_hr = hr;
    return outcome;
  }

  ITfInsertAtSelection* insert = nullptr;
  hr = context->QueryInterface(IID_ITfInsertAtSelection,
                               reinterpret_cast<void**>(&insert));
  if (FAILED(hr) || insert == nullptr) {
    outcome.status = EditStatus::kFailedBeforeMutation;
    outcome.reason = EditRejectReason::kInsertFailed;
    outcome.operation_hr = hr;
    return outcome;
  }
  ITfRange* inserted_range = nullptr;
  hr = insert->InsertTextAtSelection(
      edit_cookie, 0, plan.text_utf16().c_str(),
      static_cast<LONG>(plan.text_utf16().size()), &inserted_range);
  if (inserted_range != nullptr) {
    inserted_range->Release();
  }
  insert->Release();
  if (FAILED(hr)) {
    // The insertion call was issued and failed. Whether the store applied it is not
    // observable through the HRESULT, and there is no owned range to verify against; the
    // result claims a possible change and claims no knowledge, and the session becomes
    // uncertain.
    outcome.status = EditStatus::kFailedPossiblyMutated;
    outcome.reason = EditRejectReason::kInsertFailed;
    outcome.operation_hr = hr;
    outcome.text_may_have_changed = true;
    outcome.mutation_verified = false;
    outcome.composition_active = false;
    uncertain_ = true;
    return outcome;
  }
  outcome.status = EditStatus::kApplied;
  outcome.composition_start_utf16 = start;
  outcome.composition_length_utf16 = static_cast<LONG>(plan.text_utf16().size());
  outcome.composition_active = false;
  return outcome;
}

CompositionWriteOutcome CompositionWriter::Cancel(ITfContext* context,
                                                  TfEditCookie edit_cookie) {
  CompositionWriteOutcome outcome;
  if (composition_ == nullptr) {
    outcome.reason = EditRejectReason::kNoComposition;
    return outcome;
  }
  outcome = ReplaceCompositionText(context, std::wstring(),
                                   /*verify_current_text=*/true, edit_cookie);
  if (outcome.status != EditStatus::kApplied) {
    outcome.composition_active = true;
    outcome.composition_start_utf16 = composition_start_utf16_;
    outcome.composition_length_utf16 = composition_length_utf16_;
    return outcome;
  }
  const LONG cancelled_start = composition_start_utf16_;
  const HRESULT end_hr = EndOwnComposition(edit_cookie, &outcome);
  outcome.end_composition_hr = end_hr;
  outcome.composition_start_utf16 = cancelled_start;
  outcome.composition_length_utf16 = 0;
  if (FAILED(end_hr)) {
    outcome.status = EditStatus::kFailedAfterMutation;
    outcome.reason = EditRejectReason::kEndCompositionFailed;
    outcome.text_may_have_changed = true;
    outcome.mutation_verified = true;
    outcome.composition_active = true;
    uncertain_ = true;
    return outcome;
  }
  outcome.status = EditStatus::kApplied;
  outcome.composition_active = false;
  return outcome;
}

CompositionWriteOutcome CompositionWriter::FinishOwnedComposition(
    ITfContext* context, TfEditCookie edit_cookie) {
  (void)context;
  CompositionWriteOutcome outcome;
  ObserveSinkTermination();
  outcome.owned_composition_at_entry = composition_ != nullptr;
  outcome.externally_terminated_at_entry = CompositionExternallyTerminated();
  outcome.termination_notifications_at_entry = termination_notifications_;
  outcome.termination_notifications_after_call = TerminationNotifications();
  if (composition_ == nullptr) {
    outcome.status = EditStatus::kRejected;
    outcome.reason = EditRejectReason::kNoComposition;
    return outcome;
  }
  if (CompositionExternallyTerminated()) {
    // TSF (or another party) ended this composition; this operation issued no end call.
    ReleaseCompositionAndSink();
    outcome.status = EditStatus::kRejected;
    outcome.reason = EditRejectReason::kCompositionTerminated;
    return outcome;
  }

  ITfRange* range = nullptr;
  const HRESULT get_hr = composition_->GetRange(&range);
  if (FAILED(get_hr) || range == nullptr) {
    outcome.status = EditStatus::kFailedBeforeMutation;
    outcome.reason = EditRejectReason::kRangeUnavailable;
    outcome.operation_hr = get_hr;
    return outcome;
  }
  HRESULT read_hr = S_OK;
  const TextMatch match =
      ReadRangeText(range, edit_cookie, current_text_utf16_, &read_hr);
  range->Release();
  if (match == TextMatch::kUnreadable) {
    outcome.status = EditStatus::kFailedBeforeMutation;
    outcome.reason = EditRejectReason::kRangeUnavailable;
    outcome.operation_hr = read_hr;
    return outcome;
  }
  if (match == TextMatch::kDiffers) {
    // Someone else's text now occupies the range. Never delete or replace it, and never
    // end a composition whose contents this writer can no longer vouch for.
    outcome.status = EditStatus::kRejected;
    outcome.reason = EditRejectReason::kCompositionRangeMismatch;
    return outcome;
  }

  const LONG start_utf16 = composition_start_utf16_;
  const LONG length_utf16 = composition_length_utf16_;
  const HRESULT end_hr = EndOwnComposition(edit_cookie, &outcome);
  outcome.end_composition_hr = end_hr;
  outcome.composition_start_utf16 = start_utf16;
  outcome.composition_length_utf16 = length_utf16;
  if (FAILED(end_hr)) {
    // No text call was issued by this plan, so this is a failed-before-mutation edit even
    // though the composition object may still exist. It must never be reported Applied.
    outcome.status = EditStatus::kFailedBeforeMutation;
    outcome.reason = EditRejectReason::kEndCompositionFailed;
    outcome.operation_hr = end_hr;
    outcome.composition_active = true;
    return outcome;
  }
  // Success means only that the composition object was closed. The preedit text remains in
  // the document as ordinary text; this plan did not write, commit, cancel or replace it.
  outcome.status = EditStatus::kApplied;
  outcome.composition_active = false;
  outcome.text_may_have_changed = false;
  return outcome;
}

CompositionWriteOutcome CompositionWriter::ReplaceCompositionText(
    ITfContext* context, const std::wstring& new_text, bool verify_current_text,
    TfEditCookie edit_cookie) {
  // `context` is intentionally unused: after a failed write no context read can classify the
  // failure, so this function never reads through the context again.
  (void)context;
  CompositionWriteOutcome outcome;
  ITfRange* range = nullptr;
  HRESULT hr = composition_->GetRange(&range);
  if (FAILED(hr) || range == nullptr) {
    outcome.status = EditStatus::kFailedBeforeMutation;
    outcome.reason = EditRejectReason::kRangeUnavailable;
    outcome.operation_hr = hr;
    return outcome;
  }

  if (verify_current_text) {
    HRESULT read_hr = S_OK;
    const TextMatch match =
        ReadRangeText(range, edit_cookie, current_text_utf16_, &read_hr);
    if (match == TextMatch::kUnreadable) {
      range->Release();
      outcome.status = EditStatus::kFailedBeforeMutation;
      outcome.reason = EditRejectReason::kRangeUnavailable;
      outcome.operation_hr = read_hr;
      return outcome;
    }
    if (match == TextMatch::kDiffers) {
      // The range no longer holds what this writer last wrote. Do not delete or replace
      // text that belongs to someone else.
      range->Release();
      outcome.status = EditStatus::kRejected;
      outcome.reason = EditRejectReason::kCompositionRangeMismatch;
      return outcome;
    }
  }

  hr = range->SetText(edit_cookie, 0, new_text.empty() ? nullptr : new_text.c_str(),
                      static_cast<LONG>(new_text.size()));
  if (SUCCEEDED(hr)) {
    range->Release();
    current_text_utf16_ = new_text;
    composition_length_utf16_ = static_cast<LONG>(new_text.size());
    RefreshCompositionExtent();
    outcome.status = EditStatus::kApplied;
    outcome.operation_hr = S_OK;
    return outcome;  }

  range->Release();
  // The write call was issued and failed. The HRESULT is not mutation evidence in either
  // direction: a store may apply the text and then fail, or change nothing and fail, under
  // any failure code including E_UNEXPECTED. A TSF read-back cannot resolve this -- after a
  // failed write TSF may serve its cached pre-call text, so reading the old value back would
  // prove nothing about the backing store -- so no read-back is attempted. The recorded
  // range and text become unknown, and the only honest classification is "possibly mutated".
  uncertain_ = true;
  current_text_utf16_.clear();
  composition_start_utf16_ = -1;
  composition_length_utf16_ = -1;
  outcome.status = EditStatus::kFailedPossiblyMutated;
  outcome.reason = EditRejectReason::kSetTextFailed;
  outcome.operation_hr = hr;
  outcome.text_may_have_changed = true;
  outcome.mutation_verified = false;
  return outcome;
}

HRESULT CompositionWriter::EndOwnComposition(TfEditCookie edit_cookie,
                                             CompositionWriteOutcome* outcome) {
  ITfComposition* composition = composition_;
  if (composition == nullptr) {
    return E_UNEXPECTED;
  }
  // The call may reenter this writer through the sink or another callback. Hold local COM
  // references so the objects stay valid even if the member pair is released reentrantly;
  // the evidence below is recorded from this operation, not from a later member state.
  CompositionSink* sink = composition_sink_;
  composition->AddRef();
  if (sink != nullptr) {
    sink->AddRef();
  }
  ++end_calls_;
  if (outcome != nullptr) {
    outcome->end_composition_issued = true;
  }
  const HRESULT hr = composition->EndComposition(edit_cookie);
  std::uint64_t notifications = termination_notifications_;
  if (sink != nullptr) {
    const std::uint64_t observed = sink->termination_count();
    if (observed > notifications) {
      notifications = observed;
    }
  }
  if (outcome != nullptr) {
    outcome->termination_notifications_after_call = notifications;
  }
  if (SUCCEEDED(hr)) {
    ++end_successes_;
    // This call is the authority for the end: a notification that arrived while it ran is
    // the TSF report of this same termination, not a third-party end.
    ended_by_writer_ = true;
    externally_terminated_ = false;
    termination_notifications_ = notifications;
    if (composition_ == composition && composition_sink_ == sink) {
      ReleaseCompositionAndSink();
    }
  } else {
    ++end_failures_;
    if (notifications > termination_notifications_) {
      termination_notifications_ = notifications;
      externally_terminated_ = true;
    }
  }
  if (sink != nullptr) {
    sink->Release();
  }
  composition->Release();
  return hr;
}

void CompositionWriter::RefreshCompositionExtent() {
  if (composition_ == nullptr) {
    return;
  }
  ITfRange* range = nullptr;
  if (FAILED(composition_->GetRange(&range)) || range == nullptr) {
    return;
  }
  UpdateCompositionExtent(range);
  range->Release();
}

void CompositionWriter::UpdateCompositionExtent(ITfRange* range) {
  if (range == nullptr) {
    return;
  }
  ITfRangeACP* acp = nullptr;
  if (SUCCEEDED(range->QueryInterface(IID_ITfRangeACP,
                                      reinterpret_cast<void**>(&acp))) &&
      acp != nullptr) {
    LONG anchor = 0;
    LONG cch = 0;
    if (SUCCEEDED(acp->GetExtent(&anchor, &cch)) && anchor >= 0 && cch >= 0) {
      composition_start_utf16_ = anchor;
      composition_length_utf16_ = cch;
    }
    acp->Release();
  }
}

}  // namespace edit
}  // namespace tsf
}  // namespace epocanva
