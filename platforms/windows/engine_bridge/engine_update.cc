// EpoCanva P3.4 -- result copying and offset mapping. See engine_update.h for the units.
#include "engine_update.h"

#include <utility>

namespace epocanva {
namespace windows {
namespace engine {
namespace {

using text::BoundaryLookupStatus;
using text::ConversionStatus;
using text::Utf8BoundaryMap;
using text::Utf8BoundaryMapBuildResult;

CursorOffsetStatus FromBoundaryLookup(BoundaryLookupStatus status) {
  switch (status) {
    case BoundaryLookupStatus::kMapped:
      return CursorOffsetStatus::kMapped;
    case BoundaryLookupStatus::kUtf8ByteOffsetOutOfRange:
      return CursorOffsetStatus::kOutOfRange;
    case BoundaryLookupStatus::kInsideUtf8Sequence:
      return CursorOffsetStatus::kInsideSequence;
    case BoundaryLookupStatus::kUtf16CodeUnitOffsetOutOfRange:
    case BoundaryLookupStatus::kInsideUtf16SurrogatePair:
    case BoundaryLookupStatus::kNullOutParameter:
      // Not reachable through this direction of the table; reported as invalid rather than
      // folded into a plausible-looking answer.
      return CursorOffsetStatus::kTextInvalid;
  }
  return CursorOffsetStatus::kTextInvalid;
}

SelectionOffsetStatus FromBoundaryLookupForSelection(BoundaryLookupStatus status) {
  switch (status) {
    case BoundaryLookupStatus::kMapped:
      return SelectionOffsetStatus::kMapped;
    case BoundaryLookupStatus::kUtf8ByteOffsetOutOfRange:
      return SelectionOffsetStatus::kOutOfRange;
    case BoundaryLookupStatus::kInsideUtf8Sequence:
      return SelectionOffsetStatus::kInsideSequence;
    case BoundaryLookupStatus::kUtf16CodeUnitOffsetOutOfRange:
    case BoundaryLookupStatus::kInsideUtf16SurrogatePair:
    case BoundaryLookupStatus::kNullOutParameter:
      return SelectionOffsetStatus::kTextInvalid;
  }
  return SelectionOffsetStatus::kTextInvalid;
}

}  // namespace

EngineTextCapture CaptureEngineText(std::string_view utf8_bytes) {
  EngineTextCapture result;

  // P3.2 owns validation and the UTF-16 length; the table builder runs the same strict
  // converter before its walk. Both calls are deliberate: one returns the UTF-16 copy, the
  // other the offsets, and neither is re-implemented here.
  Utf8BoundaryMapBuildResult map = text::BuildUtf8BoundaryMap(utf8_bytes);
  if (!map.ok()) {
    result.status = map.status;
    result.os_error = map.os_error;
    result.diagnostic = "boundary map: " + map.diagnostic;
    return result;
  }
  text::Utf16ConversionResult converted =
      text::ConvertUtf8BytesToUtf16CodeUnits(utf8_bytes);
  if (!converted.ok()) {
    result.status = converted.status;
    result.os_error = converted.os_error;
    result.diagnostic = "utf16 copy: " + converted.diagnostic;
    return result;
  }

  result.status = ConversionStatus::kOk;
  result.text.utf8_byte_length = utf8_bytes.size();
  result.text.boundary_map = std::move(map.boundary_map);
  // P3.2 spells UTF-16 as std::wstring (its Windows layer); this bridge's public value uses
  // std::u16string. The static_assert in text_encoding.h makes wchar_t 16 bits on this
  // toolchain, so each unit is copied without narrowing or widening.
  result.text.utf16_code_units.reserve(converted.utf16_code_units.size());
  for (const wchar_t unit : converted.utf16_code_units) {
    result.text.utf16_code_units.push_back(static_cast<char16_t>(unit));
  }
  return result;
}

PreeditOffsetMapping MapPreeditOffsets(std::string_view preedit_utf8,
                                       int cursor_pos_utf8_bytes,
                                       int sel_start_utf8_bytes,
                                       int sel_end_utf8_bytes) {
  PreeditOffsetMapping out;

  const Utf8BoundaryMapBuildResult map = text::BuildUtf8BoundaryMap(preedit_utf8);
  if (!map.ok()) {
    out.cursor_status = CursorOffsetStatus::kTextInvalid;
    out.selection_status = SelectionOffsetStatus::kTextInvalid;
    return out;
  }
  const Utf8BoundaryMap& boundaries = map.boundary_map;

  // Cursor. Negative is the API's "not set" sentinel, not an offset to clamp.
  if (cursor_pos_utf8_bytes < 0) {
    out.cursor_status = CursorOffsetStatus::kNoCursor;
  } else {
    std::size_t utf16_offset = 0;
    const BoundaryLookupStatus lookup = boundaries.MapUtf8ByteOffsetToUtf16CodeUnitOffset(
        static_cast<std::size_t>(cursor_pos_utf8_bytes), &utf16_offset);
    out.cursor_status = FromBoundaryLookup(lookup);
    if (out.cursor_status == CursorOffsetStatus::kMapped) {
      out.cursor_utf16_code_unit_offset = utf16_offset;
    }
  }

  // Selection. Both fields carry the sentinel together in a real Preedit, so a mixed pair is
  // an inconsistent observation rather than "no selection".
  if (sel_start_utf8_bytes < 0 && sel_end_utf8_bytes < 0) {
    out.selection_status = SelectionOffsetStatus::kNoSelection;
  } else if (sel_start_utf8_bytes < 0 || sel_end_utf8_bytes < 0) {
    out.selection_status = SelectionOffsetStatus::kSentinelMismatch;
  } else if (sel_start_utf8_bytes > sel_end_utf8_bytes) {
    out.selection_status = SelectionOffsetStatus::kOrderInvalid;
  } else {
    std::size_t start_utf16 = 0;
    std::size_t end_utf16 = 0;
    const BoundaryLookupStatus start_lookup =
        boundaries.MapUtf8ByteOffsetToUtf16CodeUnitOffset(
            static_cast<std::size_t>(sel_start_utf8_bytes), &start_utf16);
    const BoundaryLookupStatus end_lookup =
        boundaries.MapUtf8ByteOffsetToUtf16CodeUnitOffset(
            static_cast<std::size_t>(sel_end_utf8_bytes), &end_utf16);
    if (start_lookup != BoundaryLookupStatus::kMapped) {
      out.selection_status = FromBoundaryLookupForSelection(start_lookup);
    } else if (end_lookup != BoundaryLookupStatus::kMapped) {
      out.selection_status = FromBoundaryLookupForSelection(end_lookup);
    } else {
      out.selection_status = SelectionOffsetStatus::kMapped;
      out.selection_start_utf16_code_unit_offset = start_utf16;
      out.selection_end_utf16_code_unit_offset = end_utf16;
    }
  }
  return out;
}

const char* DescribeCursorOffsetStatus(CursorOffsetStatus status) {
  switch (status) {
    case CursorOffsetStatus::kMapped:
      return "mapped";
    case CursorOffsetStatus::kNoCursor:
      return "no-cursor";
    case CursorOffsetStatus::kOutOfRange:
      return "out-of-range";
    case CursorOffsetStatus::kInsideSequence:
      return "inside-utf8-sequence";
    case CursorOffsetStatus::kTextInvalid:
      return "text-invalid";
  }
  return "unknown-status";
}

const char* DescribeSelectionOffsetStatus(SelectionOffsetStatus status) {
  switch (status) {
    case SelectionOffsetStatus::kMapped:
      return "mapped";
    case SelectionOffsetStatus::kNoSelection:
      return "no-selection";
    case SelectionOffsetStatus::kSentinelMismatch:
      return "sentinel-mismatch";
    case SelectionOffsetStatus::kOrderInvalid:
      return "order-invalid";
    case SelectionOffsetStatus::kOutOfRange:
      return "out-of-range";
    case SelectionOffsetStatus::kInsideSequence:
      return "inside-utf8-sequence";
    case SelectionOffsetStatus::kTextInvalid:
      return "text-invalid";
  }
  return "unknown-status";
}

}  // namespace engine
}  // namespace windows
}  // namespace epocanva
