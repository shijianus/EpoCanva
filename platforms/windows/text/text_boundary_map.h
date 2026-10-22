// EpoCanva P3.2 -- the boundary table between UTF-8 byte offsets and UTF-16 code unit
// offsets for ONE immutable text. See text_boundary_map.cc for how the table is built and
// docs/windows/text-boundary-contract.md for the contract in prose.
//
// UNITS (AGENTS.md section 5). Every member, parameter and return name here says which of
// the two units it is in: `utf8_byte_offset` counts UTF-8 code units (bytes) from the start
// of the text, `utf16_code_unit_offset` counts UTF-16 code units from the same start. There
// is no bare "cursor", "offset" or "length" in this file. Neither unit is a caret column and
// neither is a grapheme cluster; grapheme-aware caret movement is NOT implemented here and
// the names must not be read as if they were.
//
// WHICH TEXT. A map is built from one UTF-8 text and describes exactly that text. It holds
// only integers, so it never references (and can never be corrupted by) the caller's buffer,
// but it also cannot detect that the text it was built for has been replaced. The full
// identity/version discipline belongs to the asynchronous-edit work that comes after this
// round; see the contract document, section 6.
//
// REFUSAL, NOT ROUNDING. An offset that is not a boundary between two scalar values is
// refused with a status that names the kind of interior position it is. Nothing is snapped
// to the nearest boundary and nothing is clamped into range: a mapping layer that rounded
// would put preedit text one code unit off, which is exactly the corruption class AGENTS.md
// section 5 names as the classic failure.
//
// NO SECOND UNICODE FRAMEWORK. The table is built from (a) the strict system conversion in
// text_encoding.h, which decides well-formedness and the UTF-16 length, and (b) a walk of
// the lead byte of each already-accepted sequence, which contributes only the two per
// sequence sizes (how many bytes, how many code units). There is no normalisation, no case
// mapping, no grapheme segmentation, no decoding of scalar values and no re-validation here.
#ifndef EPOCANVA_WINDOWS_TEXT_BOUNDARY_MAP_H_
#define EPOCANVA_WINDOWS_TEXT_BOUNDARY_MAP_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "text_encoding.h"

namespace epocanva {
namespace windows {
namespace text {

// One legal boundary position, stated in both units at once.
struct Utf8Utf16Boundary {
  std::uint32_t utf8_byte_offset;        // UTF-8 code units (bytes) from the text start
  std::uint32_t utf16_code_unit_offset;  // UTF-16 code units from the text start
};

// The answer to a lookup. Only kMapped writes its out-parameter.
enum class BoundaryLookupStatus {
  kMapped,
  // Beyond the end offset in that unit. The end offset itself (the text's own length) IS a
  // legal boundary -- a zero-length position at the end of the text -- so "out of range"
  // starts one past it.
  kUtf8ByteOffsetOutOfRange,
  kUtf16CodeUnitOffsetOutOfRange,
  // Strictly between two consecutive byte boundaries: inside a multi-byte UTF-8 sequence.
  kInsideUtf8Sequence,
  // Strictly between two consecutive code unit boundaries: inside a surrogate pair, i.e.
  // half of one scalar value.
  kInsideUtf16SurrogatePair,
  // The caller passed no place to put the answer. Refused rather than crashed: this layer
  // eventually runs inside a host process it does not own.
  kNullOutParameter,
};

// Outcome of building a map. The status vocabulary is the converter's on purpose: a build
// fails for the same reasons a conversion does, plus kLengthMismatch for the one internal
// cross-check this file makes (documented at the check). Defined after the class below,
// because it holds one by value.
struct Utf8BoundaryMapBuildResult;

// The boundary table of one immutable UTF-8 text. Immutable after construction; safe to
// copy; holds no pointer into the text it describes.
class Utf8BoundaryMap {
 public:
  // The map of the empty text: one boundary, 0 UTF-8 bytes <-> 0 UTF-16 code units. A
  // default-constructed map is therefore a valid map, not an uninitialised one -- the
  // builder overwrites it only on success.
  Utf8BoundaryMap();

  // The text's length in each unit. The empty map reports 0 for both.
  std::size_t utf8_byte_length() const;        // UTF-8 code units (bytes)
  std::size_t utf16_code_unit_length() const;  // UTF-16 code units

  // The number of legal boundaries: the scalar value count + 1, the extra one being the end
  // offset. Scalar values, not grapheme clusters -- "e + U+0301" contributes two here.
  std::size_t boundary_count() const;

  // The full table, ascending in both columns. Read-only; exposed so a caller (and the
  // tests) can check the table itself rather than only the lookups built on it.
  const std::vector<Utf8Utf16Boundary>& boundaries() const;

  // UTF-8 byte offset -> UTF-16 code unit offset. `out_utf16_code_unit_offset` is written
  // exactly when the returned status is kMapped; on any refusal it is left untouched, so a
  // caller can seed a sentinel and prove nothing was written.
  BoundaryLookupStatus MapUtf8ByteOffsetToUtf16CodeUnitOffset(
      std::size_t utf8_byte_offset, std::size_t* out_utf16_code_unit_offset) const;

  // UTF-16 code unit offset -> UTF-8 byte offset. Same rule for the out-parameter.
  BoundaryLookupStatus MapUtf16CodeUnitOffsetToUtf8ByteOffset(
      std::size_t utf16_code_unit_offset, std::size_t* out_utf8_byte_offset) const;

  // A stable, text-free name for a lookup status, for logs.
  static const char* DescribeLookupStatus(BoundaryLookupStatus status);

 private:
  friend Utf8BoundaryMapBuildResult BuildUtf8BoundaryMap(std::string_view immutable_utf8_text);

  // The builder's own entry point; private so a map cannot be assembled from a text that
  // was never strictly validated.
  void AssignFromValidatedText(std::vector<Utf8Utf16Boundary> boundaries,
                               std::size_t utf8_byte_length,
                               std::size_t utf16_code_unit_length);

  std::vector<Utf8Utf16Boundary> boundaries_;
  std::size_t utf8_byte_length_ = 0;        // UTF-8 code units (bytes)
  std::size_t utf16_code_unit_length_ = 0;  // UTF-16 code units
};

// The build outcome, in full now that the map type is complete.
struct Utf8BoundaryMapBuildResult {
  ConversionStatus status = ConversionStatus::kInvalidInput;
  Utf8BoundaryMap boundary_map;  // usable only when status == kOk
  DWORD os_error = 0;            // carried through from the strict conversion
  std::string diagnostic;        // text-free, as in text_encoding.h
  bool ok() const { return status == ConversionStatus::kOk; }
};

// Build the boundary map of `immutable_utf8_text`. The text must be well-formed UTF-8 by
// the same strict rule as ConvertUtf8BytesToUtf16CodeUnits -- a rejected text produces no
// map (the returned map is then the empty text's map, and the status says so). The caller
// must not modify the text afterwards in a way that assumes this map still describes it;
// see the contract document, section 6.
Utf8BoundaryMapBuildResult BuildUtf8BoundaryMap(std::string_view immutable_utf8_text);

}  // namespace text
}  // namespace windows
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TEXT_BOUNDARY_MAP_H_
