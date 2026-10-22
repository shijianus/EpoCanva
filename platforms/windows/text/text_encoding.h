// EpoCanva P3.2 -- the strict UTF-8 <-> UTF-16 conversions of the Windows adapter.
//
// UNITS (AGENTS.md section 5). Two encodings and two units appear in every signature here:
//   utf8_bytes          a sequence of UTF-8 code units, i.e. BYTES. Lengths and offsets
//                       involving it are in bytes.
//   utf16_code_units    a sequence of UTF-16 code units. Lengths and offsets involving it
//                       are in UTF-16 code units -- NOT in "wide characters", "characters"
//                       or grapheme clusters. std::wstring is used here because this is the
//                       Windows UTF-16 layer (TSF, the host edit controls and the Win32
//                       conversion APIs all speak UTF-16); it is not a portable wide-char
//                       protocol and nothing outside platforms/windows may adopt it as one.
//                       The static_assert below is the compile-time half of that claim.
//
// THE CONVERTER IS THE OPERATING SYSTEM. MultiByteToWideChar / WideCharToMultiByte with
// CodePage == CP_UTF8 and the error flag set, declared in the installed SDK at
// <um/stringapiset.h:118-146>; the flags are <um/WinNls.h:148> (MB_ERR_INVALID_CHARS
// 0x00000008) and <um/WinNls.h:158-160> (WC_ERR_INVALID_CHARS 0x00000080, gated on
// WINVER >= 0x0600 -- the fact that this header compiles at all is the evidence that the
// gate is open on this toolchain). There is no hand-written UTF codec in this repository
// layer, and none is implied by the byte-length walk in text_boundary_map.cc, which only
// ever runs over text these functions have already accepted.
//
// WHAT IS FORBIDDEN, and it is forbidden in the implementation, not just in spirit:
//   CP_ACP / any other code page      -- the ANSI code page is user state; the same bytes
//                                        would mean different things on two machines.
//   silent replacement               -- no default char, no best-fit char, no '?' and no
//                                        U+FFFD. WideCharToMultiByte's lpDefaultChar and
//                                        lpUsedDefaultChar are NULL for exactly this reason
//                                        (the SDK documents them as unavailable for CP_UTF8).
//   clean-then-convert               -- the input is passed to the system verbatim; nothing
//                                        strips, trims, NFC/NFD-normalises or case-changes it
//                                        first. NormalizeString is not called anywhere here.
//   failure reported as success      -- a failed conversion returns an EMPTY output plus a
//                                        non-kOk status, so "empty because the input was
//                                        empty" and "empty because it failed" are separable
//                                        by the status alone.
//   logging the text                 -- a result's diagnostic names the API, the stage and
//                                        GetLastError(). It never contains input bytes,
//                                        input code units or converted output.
//
// TWO-CALL DISCIPLINE. Every conversion queries the required capacity first (the SDK's
// documented sizing mode: the destination parameter is _Out_writes_to_opt_, so NULL with a
// size of 0 is legal and returns the required count), then converts into a buffer of that
// size, then compares the returned written count against the query. A mismatch is reported
// as kLengthMismatch with no output rather than trusted.
//
// RANGE. The Win32 conversion APIs take their lengths as `int` (stringapiset.h:127,129,141,143),
// so any length above INT_MAX must be refused BEFORE the call, and the maximum UTF-8
// expansion (3 bytes per UTF-16 code unit -- see CheckUtf8OutputCapacityForUtf16CodeUnits)
// must be checked too. Those checks are the plain functions below: they are pure arithmetic,
// so a test can drive them with SIZE_MAX without allocating anything.
#ifndef EPOCANVA_WINDOWS_TEXT_ENCODING_H_
#define EPOCANVA_WINDOWS_TEXT_ENCODING_H_

#include <windows.h>

#include <cstddef>
#include <string>
#include <string_view>

// The UTF-16 claim is only true on platforms where wchar_t is 16 bits. MSVC always is;
// the assert makes it a build failure instead of a silent mis-interpretation of every
// offset this layer reports.
static_assert(sizeof(wchar_t) == 2,
              "this layer's utf16_code_units are UTF-16 code units, which requires a "
              "16-bit wchar_t; on a 32-bit wchar_t platform the type name would lie");

namespace epocanva {
namespace windows {
namespace text {

// The outcome vocabulary shared by the conversions and by the boundary-map builder that
// uses them. Exactly one value is kOk; every other value means "the output field is
// empty and must not be looked at".
enum class ConversionStatus {
  // The conversion ran and produced `output` (which may legitimately be empty because the
  // input length was 0).
  kOk,
  // The input is not well-formed for its encoding: a bad UTF-8 sequence (truncated,
  // illegal continuation, overlong, an encoded surrogate, above U+10FFFF) or a
  // not-well-formed UTF-16 sequence (lone high surrogate, lone low surrogate, an
  // out-of-order pair). Refused by the system flag, reported here unchanged.
  kInvalidInput,
  // The input length itself does not fit the `int` the Win32 API requires. Refused before
  // any system call, so no buffer of that size is ever requested.
  kInputTooLong,
  // The UTF-8 output the conversion would need cannot be represented by the `int`
  // cbMultiByte parameter. Determined by the capacity check below, so it can be reported
  // without allocating the buffer that would not fit.
  kOutputTooLong,
  // A system call succeeded but reported a written length that disagrees with its own
  // capacity query (or with the buffer size for the empty-input case). Nothing is handed
  // back when this fires -- it is the check that keeps "trust the API" from becoming
  // "trust the API's first answer".
  kLengthMismatch,
  // The API failed for a reason that is not about the input's well-formedness or length
  // (out of memory, an unexpected parameter rejection). `os_error` carries GetLastError().
  kOsFailure,
};

// Result of a UTF-8 -> UTF-16 conversion.
struct Utf16ConversionResult {
  // A result nobody assigned to is a refusal, never a success: the default is the
  // invalid-input value, and every path in the converter sets the real status.
  ConversionStatus status = ConversionStatus::kInvalidInput;
  std::wstring utf16_code_units;  // meaningful only when status == kOk; empty otherwise
  DWORD os_error = 0;             // GetLastError() when a system call failed; else 0
  std::string diagnostic;         // API + stage + gle; never contains text (see header)
  bool ok() const { return status == ConversionStatus::kOk; }
};

// Result of a UTF-16 -> UTF-8 conversion.
struct Utf8ConversionResult {
  ConversionStatus status = ConversionStatus::kInvalidInput;
  std::string utf8_bytes;             // meaningful only when status == kOk; empty otherwise
  DWORD os_error = 0;
  std::string diagnostic;
  bool ok() const { return status == ConversionStatus::kOk; }
};

// --- the range guards (pure; no allocation, no system call, no input touched) ---------
// kOk when a UTF-8 byte length can be handed to MultiByteToWideChar's `int cbMultiByte`.
ConversionStatus CheckUtf8InputByteLength(std::size_t utf8_byte_length);

// kOk when a UTF-16 code unit length can be handed to WideCharToMultiByte's `int
// cchWideChar`.
ConversionStatus CheckUtf16InputCodeUnitLength(std::size_t utf16_code_unit_length);

// kOk when the WORST CASE UTF-8 size for that many UTF-16 code units still fits an `int`.
// The worst case is 3 bytes per code unit: a BMP scalar value encodes to at most 3 bytes
// from 1 unit, and a supplementary scalar value encodes to 4 bytes from 2 units (2 bytes
// per unit). Because this is a worst case, it can refuse a length whose actual output
// would have fitted; the refusal is documented and is deliberate -- an adapter that
// cannot bound its own buffer is not a strict one.
ConversionStatus CheckUtf8OutputCapacityForUtf16CodeUnits(std::size_t utf16_code_unit_length);

// --- the conversions ------------------------------------------------------------------
// UTF-8 bytes -> UTF-16 code units. The length comes from the view, so an embedded NUL
// byte is just U+0000 and survives; nothing here treats the input as C-string terminated.
// An empty view is kOk with an empty result and no system call.
Utf16ConversionResult ConvertUtf8BytesToUtf16CodeUnits(std::string_view utf8_bytes);

// UTF-16 code units -> UTF-8 bytes. Same rules: explicit length, embedded U+0000
// preserved, unpaired surrogate refused, empty in -> kOk with empty out.
Utf8ConversionResult ConvertUtf16CodeUnitsToUtf8Bytes(std::wstring_view utf16_code_units);

// A stable, text-free name for a status. Returned as ASCII so a caller can print it under
// any console code page.
const char* DescribeConversionStatus(ConversionStatus status);

}  // namespace text
}  // namespace windows
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TEXT_ENCODING_H_
