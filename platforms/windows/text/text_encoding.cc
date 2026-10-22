// EpoCanva P3.2 -- the strict UTF-8 <-> UTF-16 conversions. See text_encoding.h for the
// unit contract, the forbidden list and the two-call discipline this file implements.
#include "text_encoding.h"

#include <climits>
#include <new>

namespace epocanva {
namespace windows {
namespace text {

namespace {

// The two API names, kept as the exact strings the diagnostics print so a log line can be
// read back against <um/stringapiset.h:118-146>.
constexpr char kMultiByteToWideCharName[] = "MultiByteToWideChar";
constexpr char kWideCharToMultiByteName[] = "WideCharToMultiByte";

// The system call that failed, the stage it failed in, and GetLastError() -- and nothing
// else. The input's bytes, its code units and any converted output are deliberately absent
// from this string (AGENTS.md section 2): it is built from two literals and a number.
std::string DescribeFailure(const char* api_name, const char* stage, DWORD os_error) {
  char buffer[160];
  const int length = std::snprintf(buffer, sizeof(buffer), "%s(CP_UTF8) %s failed, gle=%lu",
                                   api_name, stage, static_cast<unsigned long>(os_error));
  if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(buffer)) {
    return std::string(api_name) + " " + stage + " failed (diagnostic did not fit)";
  }
  return std::string(buffer, static_cast<std::size_t>(length));
}

// The gle the conversion APIs set when the error flag rejects the input. This is the one
// error value that means "the text is the problem", as opposed to "the environment is".
bool IsTranslationError(DWORD os_error) {
  return os_error == static_cast<DWORD>(ERROR_NO_UNICODE_TRANSLATION);
}

// A refusal that came from the input's shape.
constexpr char kStageCapacityQuery[] = "capacity query";
constexpr char kStageConversion[] = "conversion";

}  // namespace

ConversionStatus CheckUtf8InputByteLength(std::size_t utf8_byte_length) {
  // MultiByteToWideChar takes cbMultiByte as `int` (stringapiset.h:127).
  if (utf8_byte_length > static_cast<std::size_t>(INT_MAX)) {
    return ConversionStatus::kInputTooLong;
  }
  return ConversionStatus::kOk;
}

ConversionStatus CheckUtf16InputCodeUnitLength(std::size_t utf16_code_unit_length) {
  // WideCharToMultiByte takes cchWideChar as `int` (stringapiset.h:141).
  if (utf16_code_unit_length > static_cast<std::size_t>(INT_MAX)) {
    return ConversionStatus::kInputTooLong;
  }
  return ConversionStatus::kOk;
}

ConversionStatus CheckUtf8OutputCapacityForUtf16CodeUnits(std::size_t utf16_code_unit_length) {
  // The division, not a multiply: 3 * length could itself overflow. INT_MAX / 3 is the
  // largest code unit count whose worst-case UTF-8 size still fits the `int cbMultiByte`.
  if (utf16_code_unit_length > static_cast<std::size_t>(INT_MAX) / 3u) {
    return ConversionStatus::kOutputTooLong;
  }
  return ConversionStatus::kOk;
}

Utf16ConversionResult ConvertUtf8BytesToUtf16CodeUnits(std::string_view utf8_bytes) {
  Utf16ConversionResult result;

  const ConversionStatus length_guard = CheckUtf8InputByteLength(utf8_bytes.size());
  if (length_guard != ConversionStatus::kOk) {
    result.status = length_guard;
    result.diagnostic = std::string(kMultiByteToWideCharName) + " refused: input byte length "
        + "does not fit the int the API requires";
    return result;  // utf16_code_units stays empty
  }

  if (utf8_bytes.empty()) {
    // Explicit, documented short-circuit: no system call is made for a zero-length input,
    // so "succeeded with nothing to do" never depends on how the API treats cbMultiByte == 0.
    result.status = ConversionStatus::kOk;
    return result;
  }

  const int input_byte_length = static_cast<int>(utf8_bytes.size());

  ::SetLastError(0);
  const int required_utf16_code_units = ::MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, utf8_bytes.data(), input_byte_length, nullptr, 0);
  if (required_utf16_code_units == 0) {
    result.os_error = ::GetLastError();
    result.status = IsTranslationError(result.os_error) ? ConversionStatus::kInvalidInput
                                                        : ConversionStatus::kOsFailure;
    result.diagnostic = DescribeFailure(kMultiByteToWideCharName, kStageCapacityQuery,
                                        result.os_error);
    return result;
  }
  // No capacity guard is needed in this direction: a well-formed UTF-8 sequence of N bytes
  // becomes at most N UTF-16 code units (1 byte -> 1 unit, 2 or 3 bytes -> 1 unit, 4 bytes
  // -> 2 units), so the required count is bounded by the input length the guard above
  // already accepted.

  std::wstring buffer;
  try {
    buffer.assign(static_cast<std::size_t>(required_utf16_code_units), wchar_t());
  } catch (const std::bad_alloc&) {
    result.status = ConversionStatus::kOsFailure;
    result.diagnostic = std::string(kMultiByteToWideCharName) +
        " could not allocate the queried capacity (bad_alloc)";
    return result;
  }

  ::SetLastError(0);
  const int written_utf16_code_units = ::MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, utf8_bytes.data(), input_byte_length, &buffer[0],
      required_utf16_code_units);
  if (written_utf16_code_units == 0) {
    result.os_error = ::GetLastError();
    if (IsTranslationError(result.os_error)) {
      result.status = ConversionStatus::kInvalidInput;
    } else if (result.os_error == static_cast<DWORD>(ERROR_INSUFFICIENT_BUFFER)) {
      // The buffer is exactly the size this same API just asked for, so "not enough room"
      // is the two calls disagreeing -- a length mismatch, not an input problem.
      result.status = ConversionStatus::kLengthMismatch;
    } else {
      result.status = ConversionStatus::kOsFailure;
    }
    result.diagnostic = DescribeFailure(kMultiByteToWideCharName, kStageConversion,
                                        result.os_error);
    buffer.clear();  // never hand back a half-written buffer
    return result;
  }
  if (written_utf16_code_units != required_utf16_code_units) {
    result.status = ConversionStatus::kLengthMismatch;
    result.diagnostic = std::string(kMultiByteToWideCharName) +
        " wrote a different count than its own capacity query";
    buffer.clear();
    return result;
  }

  // Sized from the written count rather than the query, so the result's length is the
  // number of units the conversion actually produced.
  buffer.resize(static_cast<std::size_t>(written_utf16_code_units));
  result.status = ConversionStatus::kOk;
  result.utf16_code_units = std::move(buffer);
  return result;
}

Utf8ConversionResult ConvertUtf16CodeUnitsToUtf8Bytes(std::wstring_view utf16_code_units) {
  Utf8ConversionResult result;

  const ConversionStatus length_guard = CheckUtf16InputCodeUnitLength(utf16_code_units.size());
  if (length_guard != ConversionStatus::kOk) {
    result.status = length_guard;
    result.diagnostic = std::string(kWideCharToMultiByteName) +
        " refused: input code unit length does not fit the int the API requires";
    return result;
  }

  if (utf16_code_units.empty()) {
    result.status = ConversionStatus::kOk;
    return result;
  }

  const int input_code_unit_length = static_cast<int>(utf16_code_units.size());

  ::SetLastError(0);
  const int required_utf8_bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                                         utf16_code_units.data(),
                                                         input_code_unit_length, nullptr, 0,
                                                         nullptr, nullptr);
  if (required_utf8_bytes == 0) {
    result.os_error = ::GetLastError();
    if (IsTranslationError(result.os_error)) {
      result.status = ConversionStatus::kInvalidInput;
    } else if (CheckUtf8OutputCapacityForUtf16CodeUnits(utf16_code_units.size()) !=
               ConversionStatus::kOk) {
      // The size query failed and the worst case provably does not fit the API's `int`, so
      // the required size is the reportable reason. The capacity arithmetic is checked
      // only after a real failure, because the worst case can over-refuse: a long run of
      // ASCII would have fitted, and the query -- not this bound -- is what decides.
      result.status = ConversionStatus::kOutputTooLong;
    } else {
      result.status = ConversionStatus::kOsFailure;
    }
    result.diagnostic = DescribeFailure(kWideCharToMultiByteName, kStageCapacityQuery,
                                        result.os_error);
    return result;
  }

  std::string buffer;
  try {
    buffer.assign(static_cast<std::size_t>(required_utf8_bytes), char());
  } catch (const std::bad_alloc&) {
    result.status = ConversionStatus::kOsFailure;
    result.diagnostic = std::string(kWideCharToMultiByteName) +
        " could not allocate the queried capacity (bad_alloc)";
    return result;
  }

  ::SetLastError(0);
  const int written_utf8_bytes = ::WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, utf16_code_units.data(), input_code_unit_length, &buffer[0],
      required_utf8_bytes, nullptr, nullptr);
  if (written_utf8_bytes == 0) {
    result.os_error = ::GetLastError();
    if (IsTranslationError(result.os_error)) {
      result.status = ConversionStatus::kInvalidInput;
    } else if (result.os_error == static_cast<DWORD>(ERROR_INSUFFICIENT_BUFFER)) {
      result.status = ConversionStatus::kLengthMismatch;
    } else {
      result.status = ConversionStatus::kOsFailure;
    }
    result.diagnostic = DescribeFailure(kWideCharToMultiByteName, kStageConversion,
                                        result.os_error);
    buffer.clear();
    return result;
  }
  if (written_utf8_bytes != required_utf8_bytes) {
    result.status = ConversionStatus::kLengthMismatch;
    result.diagnostic = std::string(kWideCharToMultiByteName) +
        " wrote a different count than its own capacity query";
    buffer.clear();
    return result;
  }

  buffer.resize(static_cast<std::size_t>(written_utf8_bytes));
  result.status = ConversionStatus::kOk;
  result.utf8_bytes = std::move(buffer);
  return result;
}

const char* DescribeConversionStatus(ConversionStatus status) {
  switch (status) {
    case ConversionStatus::kOk:
      return "ok";
    case ConversionStatus::kInvalidInput:
      return "invalid-input";
    case ConversionStatus::kInputTooLong:
      return "input-too-long";
    case ConversionStatus::kOutputTooLong:
      return "output-too-long";
    case ConversionStatus::kLengthMismatch:
      return "length-mismatch";
    case ConversionStatus::kOsFailure:
      return "os-failure";
  }
  // Unreachable for the enumerators above; a status outside the list is reported as the
  // refusal it is, never as "ok".
  return "unknown-status";
}

}  // namespace text
}  // namespace windows
}  // namespace epocanva
