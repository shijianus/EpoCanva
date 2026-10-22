// EpoCanva P3.2 -- the boundary table build and lookup. See text_boundary_map.h for the
// unit contract, the refusal-not-rounding rule and the scope limits.
#include "text_boundary_map.h"

#include <algorithm>

namespace epocanva {
namespace windows {
namespace text {

namespace {

// How many UTF-8 bytes a scalar value occupies and how many UTF-16 code units it becomes,
// read off its leading byte. This is the only "knowing about UTF" this file does, and it is
// a size table, not a decoder: no scalar value is ever computed, no continuation byte is
// inspected, and no validity judgement is made with it -- the strict system conversion in
// text_encoding.cc has already accepted the text before this walk runs on it.
struct Utf8SequenceShape {
  std::size_t utf8_byte_length;
  std::size_t utf16_code_unit_length;
};

bool ShapeOfLeadingByte(unsigned char leading_byte, Utf8SequenceShape* out_shape) {
  if (leading_byte <= 0x7Fu) {  // U+0000..U+007F
    *out_shape = Utf8SequenceShape{1, 1};
    return true;
  }
  if (leading_byte >= 0xC2u && leading_byte <= 0xDFu) {  // U+0080..U+07FF
    *out_shape = Utf8SequenceShape{2, 1};
    return true;
  }
  if (leading_byte >= 0xE0u && leading_byte <= 0xEFu) {  // U+0800..U+FFFF (BMP)
    *out_shape = Utf8SequenceShape{3, 1};
    return true;
  }
  if (leading_byte >= 0xF0u && leading_byte <= 0xF4u) {  // U+10000..U+10FFFF: surrogate pair
    *out_shape = Utf8SequenceShape{4, 2};
    return true;
  }
  // 0x80..0xC1 cannot start a sequence in well-formed UTF-8 (continuation bytes, plus the
  // two overlong leads), and 0xF5..0xFF cannot appear at all. Reaching this branch means the
  // walk and the strict converter disagree about the same text.
  return false;
}

// The kLengthMismatch reuses in this file are deliberate: the status vocabulary is the
// converter's (see text_boundary_map.h), and each of these three fires when two
// independently computed lengths of the SAME text disagree -- which is the same fact the
// converter's kLengthMismatch reports between an API's size query and its write count.
constexpr char kDisagreementDiagnostic[] =
    "boundary walk and the strict system conversion disagree about this text";

}  // namespace

Utf8BoundaryMap::Utf8BoundaryMap() {
  // The empty text's map: 0 UTF-8 bytes <-> 0 UTF-16 code units, one boundary. Built here
  // rather than left empty so no lookup path has to special-case an unpopulated table.
  boundaries_.push_back(Utf8Utf16Boundary{0u, 0u});
}

std::size_t Utf8BoundaryMap::utf8_byte_length() const { return utf8_byte_length_; }

std::size_t Utf8BoundaryMap::utf16_code_unit_length() const { return utf16_code_unit_length_; }

std::size_t Utf8BoundaryMap::boundary_count() const { return boundaries_.size(); }

const std::vector<Utf8Utf16Boundary>& Utf8BoundaryMap::boundaries() const { return boundaries_; }

void Utf8BoundaryMap::AssignFromValidatedText(std::vector<Utf8Utf16Boundary> boundaries,
                                              std::size_t utf8_byte_length,
                                              std::size_t utf16_code_unit_length) {
  boundaries_ = std::move(boundaries);
  utf8_byte_length_ = utf8_byte_length;
  utf16_code_unit_length_ = utf16_code_unit_length;
}

BoundaryLookupStatus Utf8BoundaryMap::MapUtf8ByteOffsetToUtf16CodeUnitOffset(
    std::size_t utf8_byte_offset, std::size_t* out_utf16_code_unit_offset) const {
  if (out_utf16_code_unit_offset == nullptr) {
    return BoundaryLookupStatus::kNullOutParameter;
  }
  if (utf8_byte_offset > utf8_byte_length_) {
    return BoundaryLookupStatus::kUtf8ByteOffsetOutOfRange;
  }
  const auto found = std::lower_bound(
      boundaries_.begin(), boundaries_.end(), utf8_byte_offset,
      [](const Utf8Utf16Boundary& boundary, std::size_t value) {
        return static_cast<std::size_t>(boundary.utf8_byte_offset) < value;
      });
  if (found != boundaries_.end() &&
      static_cast<std::size_t>(found->utf8_byte_offset) == utf8_byte_offset) {
    *out_utf16_code_unit_offset = static_cast<std::size_t>(found->utf16_code_unit_offset);
    return BoundaryLookupStatus::kMapped;
  }
  // In range, present between two boundaries: the byte offset splits one sequence. No
  // rounding is offered -- the caller either has a boundary or does not.
  return BoundaryLookupStatus::kInsideUtf8Sequence;
}

BoundaryLookupStatus Utf8BoundaryMap::MapUtf16CodeUnitOffsetToUtf8ByteOffset(
    std::size_t utf16_code_unit_offset, std::size_t* out_utf8_byte_offset) const {
  if (out_utf8_byte_offset == nullptr) {
    return BoundaryLookupStatus::kNullOutParameter;
  }
  if (utf16_code_unit_offset > utf16_code_unit_length_) {
    return BoundaryLookupStatus::kUtf16CodeUnitOffsetOutOfRange;
  }
  const auto found = std::lower_bound(
      boundaries_.begin(), boundaries_.end(), utf16_code_unit_offset,
      [](const Utf8Utf16Boundary& boundary, std::size_t value) {
        return static_cast<std::size_t>(boundary.utf16_code_unit_offset) < value;
      });
  if (found != boundaries_.end() &&
      static_cast<std::size_t>(found->utf16_code_unit_offset) == utf16_code_unit_offset) {
    *out_utf8_byte_offset = static_cast<std::size_t>(found->utf8_byte_offset);
    return BoundaryLookupStatus::kMapped;
  }
  // In range and not a boundary. The UTF-16 gaps in the table are 1 (BMP) or 2 (a surrogate
  // pair), so the only offset that can fall strictly between two entries is the low half of
  // a pair -- which is why this branch can name the reason instead of guessing.
  return BoundaryLookupStatus::kInsideUtf16SurrogatePair;
}

const char* Utf8BoundaryMap::DescribeLookupStatus(BoundaryLookupStatus status) {
  switch (status) {
    case BoundaryLookupStatus::kMapped:
      return "mapped";
    case BoundaryLookupStatus::kUtf8ByteOffsetOutOfRange:
      return "utf8-byte-offset-out-of-range";
    case BoundaryLookupStatus::kUtf16CodeUnitOffsetOutOfRange:
      return "utf16-code-unit-offset-out-of-range";
    case BoundaryLookupStatus::kInsideUtf8Sequence:
      return "inside-utf8-sequence";
    case BoundaryLookupStatus::kInsideUtf16SurrogatePair:
      return "inside-utf16-surrogate-pair";
    case BoundaryLookupStatus::kNullOutParameter:
      return "null-out-parameter";
  }
  return "unknown-status";
}

Utf8BoundaryMapBuildResult BuildUtf8BoundaryMap(std::string_view immutable_utf8_text) {
  Utf8BoundaryMapBuildResult result;

  // (1) The strict system conversion decides well-formedness, and its output length is the
  // UTF-16 length the table must end at. Nothing is rebuilt from a text it refused.
  const Utf16ConversionResult converted = ConvertUtf8BytesToUtf16CodeUnits(immutable_utf8_text);
  if (!converted.ok()) {
    result.status = converted.status;
    result.os_error = converted.os_error;
    result.diagnostic = "boundary map build refused before the walk: " + converted.diagnostic;
    return result;  // boundary_map stays the empty text's map
  }
  const std::size_t utf16_code_unit_length = converted.utf16_code_units.size();

  // (2) Walk the sequence starts. Worst case one boundary per byte (all ASCII), so that is
  // the reservation; the table is the only allocation this function makes.
  std::vector<Utf8Utf16Boundary> boundaries;
  boundaries.reserve(immutable_utf8_text.size() + 1);
  boundaries.push_back(Utf8Utf16Boundary{0u, 0u});

  std::size_t utf8_byte_offset = 0;
  std::size_t walk_utf16_code_unit_offset = 0;
  while (utf8_byte_offset < immutable_utf8_text.size()) {
    Utf8SequenceShape shape;
    if (!ShapeOfLeadingByte(static_cast<unsigned char>(immutable_utf8_text[utf8_byte_offset]),
                            &shape)) {
      result.status = ConversionStatus::kLengthMismatch;
      result.diagnostic = kDisagreementDiagnostic;
      return result;
    }
    utf8_byte_offset += shape.utf8_byte_length;
    walk_utf16_code_unit_offset += shape.utf16_code_unit_length;
    if (utf8_byte_offset > immutable_utf8_text.size()) {
      // A sequence that runs past the end of the text. The converter accepted the text, so
      // this is the disagreement case again -- checked rather than assumed, because the loop
      // above is only guaranteed to terminate by these sizes.
      result.status = ConversionStatus::kLengthMismatch;
      result.diagnostic = kDisagreementDiagnostic;
      return result;
    }
    // The casts are lossless: the input guard in the conversion bounded the byte length by
    // INT_MAX, and a UTF-8 -> UTF-16 conversion never produces more code units than bytes,
    // so both offsets here are below INT_MAX.
    boundaries.push_back(Utf8Utf16Boundary{static_cast<std::uint32_t>(utf8_byte_offset),
                                           static_cast<std::uint32_t>(walk_utf16_code_unit_offset)});
  }

  // (3) The two independent computations of the same length must agree: the system
  // conversion's code unit count and this walk's. If they do not, the table is dropped
  // whole -- a partially built boundary map is never handed back.
  if (walk_utf16_code_unit_offset != utf16_code_unit_length) {
    result.status = ConversionStatus::kLengthMismatch;
    result.diagnostic = kDisagreementDiagnostic;
    return result;
  }

  result.boundary_map.AssignFromValidatedText(std::move(boundaries),
                                              immutable_utf8_text.size(),
                                              utf16_code_unit_length);
  result.status = ConversionStatus::kOk;
  return result;
}

}  // namespace text
}  // namespace windows
}  // namespace epocanva
