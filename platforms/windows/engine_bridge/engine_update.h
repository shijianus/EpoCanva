// EpoCanva P3.4 -- caller-owned UTF-16 copies of one engine observation.
//
// THIS FILE IS DATA, NOT A QUERY. It contains no librime header, calls no API and owns no
// borrowed pointer. A RimeSession fills a value of these types by copying everything it needs
// out of a `RimeStatus` / `RimeContext` / `RimeCommit` before the matching free_* runs; after
// that call neither the value nor this header refers to memory inside rime.dll.
//
// UNITS (AGENTS.md section 5). Two encodings appear:
//   utf8_byte_length             UTF-8 code units (bytes), the unit librime's C API uses.
//   utf16_code_units             UTF-16 code units, the unit TSF and this bridge hand to the
//                                host. It is not a grapheme cluster count.
//   cursor/sel ..._utf16...      UTF-16 code unit offsets from the start of that text.
// RimeComposition.length/.cursor_pos/.sel_start/.sel_end are UTF-8 BYTE offsets into the
// preedit string (src/rime_api_impl.h:209-235 copies them from Composition::GetPreedit,
// which builds `preedit.text` as a std::string -- bytes; src/rime/composition.cc:24-38). They
// are mapped to UTF-16 through P3.2's boundary table, and the map is built from the preedit
// text, never from the raw input.
//
// NO CLAMPING. A negative offset is Rime's `string::npos` / "no value" sentinel (Preedit
// initialises sel_start/sel_end to 0 and caret_pos to string::npos at
// src/rime/composition.cc:31-33). It is reported as kNoCursor / kNoSelection. An offset past
// the text, a start after the end, or an offset inside a UTF-8 sequence or surrogate pair is
// reported as an explicit invalid status. Nothing is moved to the nearest legal value, because
// a silently rounded offset puts preedit text one unit off -- the corruption class AGENTS.md
// section 5 names.
//
// COMMIT IS NOT "APPLIED". EngineCommit is the bytes the engine has queued for the host. It is
// not evidence that any application accepted, displayed or persisted them. This bridge does
// not call ITfRange, SetText or SendInput.
#ifndef EPOCANVA_WINDOWS_ENGINE_BRIDGE_ENGINE_UPDATE_H_
#define EPOCANVA_WINDOWS_ENGINE_BRIDGE_ENGINE_UPDATE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "text_boundary_map.h"

namespace epocanva {
namespace windows {
namespace engine {

// One text copied out of the engine, with the P3.2 boundary table of exactly that text.
// `utf16_code_units` is the host-facing value; `boundary_map` is only useful for positions
// inside this same text and is intentionally carried rather than rebuilt per lookup.
struct EngineText {
  std::u16string utf16_code_units;
  std::size_t utf8_byte_length = 0;  // UTF-8 code units (bytes) of the source text
  text::Utf8BoundaryMap boundary_map;
};

// Outcome of copying one UTF-8 string. The status vocabulary is P3.2's on purpose: this
// adapter performs no conversion of its own. All non-kOk values leave `text` empty.
struct EngineTextCapture {
  text::ConversionStatus status = text::ConversionStatus::kInvalidInput;
  EngineText text;
  DWORD os_error = 0;
  std::string diagnostic;  // API + stage + lengths; never the text
  bool ok() const { return status == text::ConversionStatus::kOk; }
};

// Copy `utf8_bytes` through the strict P3.2 converter and build the boundary table of the
// accepted text. An empty view is kOk and yields the empty text's table.
EngineTextCapture CaptureEngineText(std::string_view utf8_bytes);

// --- preedit cursor / selection -------------------------------------------------------
// RimeComposition reports three `int` fields. Their real sentinel and range meanings are
// handled as separate outcomes instead of being folded into one "bad offset" answer.
enum class CursorOffsetStatus {
  kMapped = 0,      // a legal UTF-8 boundary; the UTF-16 offset below is meaningful
  kNoCursor,        // the API's negative / npos sentinel: no cursor position to report
  kOutOfRange,      // past the end of the preedit text
  kInsideSequence,  // strictly inside a multi-byte UTF-8 sequence
  kTextInvalid,     // the preedit text itself was not well-formed UTF-8
};

enum class SelectionOffsetStatus {
  kMapped = 0,       // start and end are legal boundaries; both UTF-16 offsets are meaningful
  kNoSelection,      // BOTH fields carry the negative / npos sentinel
  kSentinelMismatch, // exactly one field is negative: inconsistent, not "no selection"
  kOrderInvalid,     // start is after end
  kOutOfRange,       // an endpoint is past the end of the preedit text
  kInsideSequence,   // an endpoint splits a multi-byte UTF-8 sequence
  kTextInvalid,      // the preedit text itself was not well-formed UTF-8
};

struct PreeditOffsetMapping {
  CursorOffsetStatus cursor_status = CursorOffsetStatus::kTextInvalid;
  std::size_t cursor_utf16_code_unit_offset = 0;
  SelectionOffsetStatus selection_status = SelectionOffsetStatus::kTextInvalid;
  std::size_t selection_start_utf16_code_unit_offset = 0;
  std::size_t selection_end_utf16_code_unit_offset = 0;
};

// Map the three RimeComposition integers against `preedit_utf8`. Pure: no API, no allocation
// beyond the boundary table. A legal zero-length selection (start == end == end of text) is
// kMapped; an empty selection is not the same thing as "no selection".
PreeditOffsetMapping MapPreeditOffsets(std::string_view preedit_utf8,
                                       int cursor_pos_utf8_bytes,
                                       int sel_start_utf8_bytes,
                                       int sel_end_utf8_bytes);

// --- one engine observation ------------------------------------------------------------
struct EngineCandidate {
  std::u16string text_utf16_code_units;
  std::u16string comment_utf16_code_units;  // empty when the API reported no comment
};

// Everything RimeApi reports about one session at one instant. `valid` is true only when every
// slot was read, freed and converted successfully. Text is always the caller's copy.
struct EngineState {
  bool valid = false;
  std::string schema_id;    // UTF-8; an ASCII identifier in this project's schemas
  std::string schema_name;  // UTF-8; diagnostic only
  bool service_disabled = false;  // RimeStatus.is_disabled (maintenance / service not started)
  bool is_composing = false;      // RimeStatus.is_composing
  EngineText raw_input;           // RimeApi::get_input(), copied immediately
  EngineText preedit;             // RimeComposition.preedit, empty when not composing
  CursorOffsetStatus cursor_status = CursorOffsetStatus::kNoCursor;
  std::size_t cursor_utf16_code_unit_offset = 0;
  SelectionOffsetStatus selection_status = SelectionOffsetStatus::kNoSelection;
  std::size_t selection_start_utf16_code_unit_offset = 0;
  std::size_t selection_end_utf16_code_unit_offset = 0;
  bool has_menu = false;
  int page_size = 0;
  int page_no = 0;
  bool is_last_page = false;
  // Page-relative index (RimeGetContext sets selected_index % page_size at
  // src/rime_api_impl.h:238-241). -1 means the API reported no highlighted candidate.
  int highlighted_candidate_index_on_page = -1;
  std::vector<EngineCandidate> candidates;  // menu order, page-relative
  std::string diagnostic;                   // counts and stage names; never text
};

// A pending commit. `present == false` means the engine reported no commit; it does NOT mean a
// commit was applied to any host document.
struct EngineCommit {
  bool present = false;
  std::u16string text_utf16_code_units;
  std::size_t utf8_byte_length = 0;
};

const char* DescribeCursorOffsetStatus(CursorOffsetStatus status);
const char* DescribeSelectionOffsetStatus(SelectionOffsetStatus status);

}  // namespace engine
}  // namespace windows
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_ENGINE_BRIDGE_ENGINE_UPDATE_H_
