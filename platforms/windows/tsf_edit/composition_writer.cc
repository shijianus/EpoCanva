// EpoCanva P3.7 -- implementation of the composition writer.
#include "composition_writer.h"

#include <atomic>
#include <utility>

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
    return S_OK;
  }

  bool terminated() const { return terminated_.load(); }

 private:
  std::atomic<LONG> reference_count_{1};
  std::atomic<bool> terminated_{false};
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
// longer range differ rather than truncate to a false match.
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

// Reads [start, start + length) from an arbitrary range by shifting a clone. The range's own
// ACP extent says where the clone starts, so the two shifts move it onto the requested span.
TextMatch ReadSpanFromRange(ITfRange* range, LONG start_utf16, LONG length_utf16,
                            TfEditCookie edit_cookie, const std::wstring& expected) {
  if (range == nullptr || start_utf16 < 0 || length_utf16 < 0) {
    return TextMatch::kUnreadable;
  }
  ITfRangeACP* acp = nullptr;
  if (FAILED(range->QueryInterface(IID_ITfRangeACP,
                                   reinterpret_cast<void**>(&acp))) ||
      acp == nullptr) {
    return TextMatch::kUnreadable;
  }
  LONG anchor = 0;
  LONG extent = 0;
  const HRESULT extent_hr = acp->GetExtent(&anchor, &extent);
  acp->Release();
  if (FAILED(extent_hr) || anchor < 0 || extent < 0) {
    return TextMatch::kUnreadable;
  }
  ITfRange* clone = nullptr;
  if (FAILED(range->Clone(&clone)) || clone == nullptr) {
    return TextMatch::kUnreadable;
  }

  const LONG start_delta = start_utf16 - anchor;
  const LONG end_delta = (start_utf16 + length_utf16) - (anchor + extent);
  // A collapsed range stays collapsed when its start is shifted, so shifting start first and
  // then end would measure the end delta from the new start and overshoot. For a target at or
  // after the range, move the end first; for a target wholly before it, move the start first.
  const bool target_before = (start_utf16 + length_utf16) <= anchor;
  LONG shifted = 0;
  auto shift_start = [&]() -> bool {
    if (start_delta == 0) {
      return true;
    }
    const HRESULT shift_hr =
        clone->ShiftStart(edit_cookie, start_delta, &shifted, nullptr);
    return SUCCEEDED(shift_hr) && shifted == start_delta;
  };
  auto shift_end = [&]() -> bool {
    if (end_delta == 0) {
      return true;
    }
    shifted = 0;
    const HRESULT shift_hr =
        clone->ShiftEnd(edit_cookie, end_delta, &shifted, nullptr);
    return SUCCEEDED(shift_hr) && shifted == end_delta;
  };
  const bool shifted_ok = target_before ? (shift_start() && shift_end())
                                        : (shift_end() && shift_start());
  if (!shifted_ok) {
    clone->Release();
    return TextMatch::kUnreadable;
  }
  HRESULT read_hr = S_OK;
  const TextMatch match = ReadRangeText(clone, edit_cookie, expected, &read_hr);
  clone->Release();
  return match;
}

// Reads the document as one range ([GetStart, GetEnd)) and compares the requested slice. This
// is the reliable read-back after a failed SetText: the composition range may not have moved,
// and a collapsed start/selection range cannot be shifted into an arbitrary span without
// re-anchoring. Bounded: a document above 1 Mi UTF-16 code units is reported unreadable rather
// than copied without limit.
TextMatch ReadWholeDocumentSpan(ITfContext* context, LONG start_utf16, LONG length_utf16,
                                TfEditCookie edit_cookie,
                                const std::wstring& expected) {
  if (context == nullptr || start_utf16 < 0 || length_utf16 < 0) {
    return TextMatch::kUnreadable;
  }
  ITfRange* start_range = nullptr;
  ITfRange* end_range = nullptr;
  if (FAILED(context->GetStart(edit_cookie, &start_range)) || start_range == nullptr) {
    return TextMatch::kUnreadable;
  }
  if (FAILED(context->GetEnd(edit_cookie, &end_range)) || end_range == nullptr) {
    start_range->Release();
    return TextMatch::kUnreadable;
  }
  const HRESULT extend_hr =
      start_range->ShiftEndToRange(edit_cookie, end_range, TF_ANCHOR_START);
  end_range->Release();
  if (FAILED(extend_hr)) {
    start_range->Release();
    return TextMatch::kUnreadable;
  }

  std::wstring document;
  ULONG capacity = 256;
  bool read_ok = false;
  for (int attempt = 0; attempt < 14; ++attempt) {
    std::wstring buffer(capacity, L'\0');
    ULONG got = 0;
    const HRESULT read_hr = start_range->GetText(edit_cookie, 0, buffer.data(),
                                                 capacity, &got);
    if (FAILED(read_hr)) {
      break;
    }
    if (got < capacity) {
      buffer.resize(got);
      document = std::move(buffer);
      read_ok = true;
      break;
    }
    capacity *= 2;
    if (capacity > (1u << 20)) {
      break;
    }
  }
  start_range->Release();
  if (!read_ok) {
    return TextMatch::kUnreadable;
  }
  const std::size_t start = static_cast<std::size_t>(start_utf16);
  const std::size_t length = static_cast<std::size_t>(length_utf16);
  if (start > document.size() || document.size() - start < length) {
    return TextMatch::kUnreadable;
  }
  return document.compare(start, length, expected) == 0 ? TextMatch::kMatches
                                                         : TextMatch::kDiffers;
}

// Reads [start, start + length) in the document, independently of where the composition
// range currently sits. This is the read-back used after a failed SetText, because TSF does
// not move the composition range when the store reports the failure: the range may still
// describe the old (pre-mutation) extent while the document already holds the new text.
// The document start range is tried first; the selection and finally the composition range
// are fallbacks. A span that cannot be read is reported unreadable, never guessed.
TextMatch CheckDocumentSpan(ITfContext* context, ITfComposition* composition,
                            LONG start_utf16, LONG length_utf16,
                            TfEditCookie edit_cookie, const std::wstring& expected) {
  if (start_utf16 < 0 || length_utf16 < 0) {
    return TextMatch::kUnreadable;
  }
  if (context != nullptr) {
    const TextMatch whole =
        ReadWholeDocumentSpan(context, start_utf16, length_utf16, edit_cookie, expected);
    if (whole != TextMatch::kUnreadable) {
      return whole;
    }
    ITfRange* start_range = nullptr;
    if (SUCCEEDED(context->GetStart(edit_cookie, &start_range)) &&
        start_range != nullptr) {
      const TextMatch match = ReadSpanFromRange(start_range, start_utf16, length_utf16,
                                                edit_cookie, expected);
      start_range->Release();
      if (match != TextMatch::kUnreadable) {
        return match;
      }
    }
    TF_SELECTION selection = {};
    ULONG fetched = 0;
    if (SUCCEEDED(context->GetSelection(edit_cookie, TF_DEFAULT_SELECTION, 1,
                                        &selection, &fetched)) &&
        fetched == 1 && selection.range != nullptr) {
      const TextMatch match = ReadSpanFromRange(selection.range, start_utf16,
                                                length_utf16, edit_cookie, expected);
      selection.range->Release();
      if (match != TextMatch::kUnreadable) {
        return match;
      }
    }
  }
  if (composition != nullptr) {
    ITfRange* composition_range = nullptr;
    if (SUCCEEDED(composition->GetRange(&composition_range)) &&
        composition_range != nullptr) {
      const TextMatch match = ReadSpanFromRange(composition_range, start_utf16,
                                                length_utf16, edit_cookie, expected);
      composition_range->Release();
      return match;
    }
  }
  return TextMatch::kUnreadable;
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
  if (composition_ != nullptr && CompositionExternallyTerminated()) {
    // The context owner terminated the composition. Do not call any method on the terminated
    // object and do not fall through to the no-preedit insert path.
    ReleaseCompositionAndSink();
    CompositionWriteOutcome outcome;
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
      replaced.status == EditStatus::kFailedAfterMutation) {
    // Begin failed. Whether the backing text changed or not, this call's composition is not a
    // usable editing context: end it (without another SetText) and leave any mutated text in
    // place. This is not a rollback and does not replay the insert. If ending fails, the
    // reference stays held and composition_active says so.
    const HRESULT end_hr = EndOwnComposition(edit_cookie);
    replaced.end_composition_hr = end_hr;
    if (FAILED(end_hr)) {
      replaced.composition_active = true;
    } else {
      replaced.composition_active = false;
      replaced.composition_start_utf16 = start;
      replaced.composition_length_utf16 =
          replaced.status == EditStatus::kFailedAfterMutation
              ? static_cast<LONG>(plan.text_utf16().size())
              : 0;
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
    const HRESULT end_hr = EndOwnComposition(edit_cookie);
    outcome.end_composition_hr = end_hr;
    outcome.composition_start_utf16 = committed_start;
    outcome.composition_length_utf16 = committed_length;
    if (FAILED(end_hr)) {
      // SetText succeeded; EndComposition failed. The text is committed, the composition
      // may still exist. No rollback and no replay is attempted.
      outcome.status = EditStatus::kFailedAfterMutation;
      outcome.reason = EditRejectReason::kEndCompositionFailed;
      outcome.text_may_have_changed = true;
      outcome.mutation_verified = true;
      outcome.composition_active = true;
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
    // An insertion failure has no reliable independent span to read back; the result says
    // the text may have changed and does not claim that it did not.
    outcome.status = EditStatus::kFailedAfterMutation;
    outcome.reason = EditRejectReason::kInsertFailed;
    outcome.operation_hr = hr;
    outcome.text_may_have_changed = true;
    outcome.mutation_verified = false;
    outcome.composition_active = false;
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
  const HRESULT end_hr = EndOwnComposition(edit_cookie);
  outcome.end_composition_hr = end_hr;
  outcome.composition_start_utf16 = cancelled_start;
  outcome.composition_length_utf16 = 0;
  if (FAILED(end_hr)) {
    outcome.status = EditStatus::kFailedAfterMutation;
    outcome.reason = EditRejectReason::kEndCompositionFailed;
    outcome.text_may_have_changed = true;
    outcome.mutation_verified = true;
    outcome.composition_active = true;
    return outcome;
  }
  outcome.status = EditStatus::kApplied;
  outcome.composition_active = false;
  return outcome;
}

CompositionWriteOutcome CompositionWriter::ReplaceCompositionText(
    ITfContext* context, const std::wstring& new_text, bool verify_current_text,
    TfEditCookie edit_cookie) {
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
  outcome.operation_hr = hr;

  // A store that partially applied the edit reports E_UNEXPECTED. TSF's cached text is still
  // the old text in that case, so a read-back through TSF would wrongly say "unchanged"; the
  // explicit signal is authoritative for the status, but the exact text is not claimed.
  if (hr == E_UNEXPECTED) {
    outcome.status = EditStatus::kFailedAfterMutation;
    outcome.text_may_have_changed = true;
    outcome.mutation_verified = false;
    return outcome;
  }

  // The call failed. Determine whether the backing text changed despite the failure. TSF does
  // not necessarily move the composition range when the store reports a failure, so the
  // read-back targets the document span [composition_start, +new_text.size()) instead.
  const TextMatch after = CheckDocumentSpan(
      context, composition_, composition_start_utf16_,
      static_cast<LONG>(new_text.size()), edit_cookie, new_text);
  if (after == TextMatch::kMatches) {
    outcome.status = EditStatus::kFailedAfterMutation;
    outcome.text_may_have_changed = true;
    outcome.mutation_verified = true;
    current_text_utf16_ = new_text;
    composition_length_utf16_ = static_cast<LONG>(new_text.size());
    return outcome;
  }
  const TextMatch before = CheckDocumentSpan(
      context, composition_, composition_start_utf16_,
      static_cast<LONG>(current_text_utf16_.size()), edit_cookie,
      current_text_utf16_);
  if (before == TextMatch::kMatches) {
    outcome.status = EditStatus::kFailedBeforeMutation;
    outcome.text_may_have_changed = false;
    outcome.mutation_verified = true;
    return outcome;
  }
  // Neither state is readable: do not claim that nothing changed.
  outcome.status = EditStatus::kFailedAfterMutation;
  outcome.text_may_have_changed = true;
  outcome.mutation_verified = false;
  return outcome;
}

HRESULT CompositionWriter::EndOwnComposition(TfEditCookie edit_cookie) {
  if (composition_ == nullptr) {
    return E_UNEXPECTED;
  }
  const HRESULT hr = composition_->EndComposition(edit_cookie);
  if (SUCCEEDED(hr)) {
    ReleaseCompositionAndSink();
  }
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
