// EpoCanva P3.2 -- the verification program for the Windows text adapter layer
// (platforms/windows/text/).
//
// WHAT IT JUDGES. Only two things are countable here: exact unit values, and refusals that
// actually refuse. Every legal case is checked against a HAND-WRITTEN expectation -- the
// UTF-8 bytes and the UTF-16 code units of each fixture are both spelled out below, taken
// from the Unicode code point assignments, not produced by the code under test. A pure
// round trip would pass with two mirrored bugs (an encoder that doubles every byte and a
// decoder that halves every byte), so round trips are extra checks here and never the
// judgement.
//
// INDEPENDENT EXPECTATIONS. The fixtures are the ones the P3.2 brief names: empty, ASCII,
// precomposed e-acute, "A U+1F600 e-acute", Chinese, "e + U+0301", an embedded NUL, several
// supplementary-plane characters, the U+0080 lower bound and the U+10FFFF upper bound. Each
// carries its expected byte list AND its expected code unit list.
//
// ILLEGAL INPUT IS A RESULT, NOT A LOG LINE. Every rejected case asserts the status, that
// the output is empty, and that a GetLastError value was carried out. The privacy rule is
// checked from the other side: a rejected input whose bytes include an ASCII word must
// produce a diagnostic that does NOT contain that word, because AGENTS.md section 2 forbids
// this layer from echoing user text anywhere.
//
// MODES
//   encoding         the strict conversions, legal and illegal, both directions
//   guards           the int-range arithmetic, driven with INT_MAX / INT_MAX+1 / SIZE_MAX
//                    (pure arithmetic: no allocation, and no bogus view is dereferenced --
//                    the guarded calls use a null data pointer precisely so a missing guard
//                    would show up as a refusal that never happened)
//   self-check-fail  the falsifiability control: one deliberately false claim, must fail
//
// EXIT CODES: 0 pass; 2 usage; 5 a check failed; 14 the self-check-fail mode's expected
// failure. RESULT PASS on stdout is the CTest marker.
#include <windows.h>

#include <climits>
#include <cstdio>
#include <string>
#include <vector>

#include "text_encoding.h"

namespace {

using epocanva::windows::text::CheckUtf16InputCodeUnitLength;
using epocanva::windows::text::CheckUtf8InputByteLength;
using epocanva::windows::text::CheckUtf8OutputCapacityForUtf16CodeUnits;
using epocanva::windows::text::ConversionStatus;
using epocanva::windows::text::ConvertUtf16CodeUnitsToUtf8Bytes;
using epocanva::windows::text::ConvertUtf8BytesToUtf16CodeUnits;
using epocanva::windows::text::DescribeConversionStatus;
using epocanva::windows::text::Utf16ConversionResult;
using epocanva::windows::text::Utf8ConversionResult;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitCheckFailed = 5;
constexpr int kExitSelfCheckFailed = 14;

// SIZE_MAX without depending on the macro's header: static_cast<size_t>(-1) is it.
constexpr std::size_t kSizeMax = static_cast<std::size_t>(-1);

int g_total = 0;
int g_failed = 0;

void Check(bool condition, const std::string& what, const std::string& got) {
  ++g_total;
  if (condition) {
    std::printf("[text]   ok   %s\n", what.c_str());
  } else {
    ++g_failed;
    std::printf("[text]   FAIL %s  (got: %s)\n", what.c_str(), got.c_str());
  }
}

// The end of a judging mode. The success marker is printed ONLY when nothing failed: a run
// that prints "RESULT PASS" next to its own failure lines would teach the CTest wrapper to
// trust a red run (AGENTS.md section 3), and the exit code alone should not be the only
// honest signal.
int FinishMode() {
  if (g_failed != 0) {
    std::printf("[text] RESULT FAIL (%d checks, %d failed)\n", g_total, g_failed);
    return kExitCheckFailed;
  }
  std::printf("[text] RESULT PASS (%d checks, %d failed)\n", g_total, g_failed);
  return kExitOk;
}

// --- fixture units -------------------------------------------------------------------
// Bytes are UTF-8 code units. Units are UTF-16 code units, each written as the 16-bit value
// it is, so a surrogate pair is two entries (U+1F600 = D83D DE00). Both lists in every
// fixture below are hand-written from the Unicode assignments; neither is derived from the
// other, and neither is derived from the code under test.
using Bytes = std::vector<unsigned char>;
using Units = std::vector<unsigned int>;

std::string ToUtf8Bytes(const Bytes& bytes) {
  std::string out;
  out.reserve(bytes.size());
  for (const unsigned char byte : bytes) {
    out.push_back(static_cast<char>(byte));
  }
  return out;
}

std::wstring ToUtf16Units(const Units& units) {
  std::wstring out;
  out.reserve(units.size());
  for (const unsigned int unit : units) {
    out.push_back(static_cast<wchar_t>(unit));
  }
  return out;
}

Bytes FromUtf8Bytes(const std::string& utf8_bytes) {
  Bytes out;
  out.reserve(utf8_bytes.size());
  for (const char byte : utf8_bytes) {
    out.push_back(static_cast<unsigned char>(byte));
  }
  return out;
}

Units FromUtf16Units(const std::wstring& utf16_code_units) {
  Units out;
  out.reserve(utf16_code_units.size());
  for (const wchar_t unit : utf16_code_units) {
    out.push_back(static_cast<unsigned int>(static_cast<unsigned short>(unit)));
  }
  return out;
}

// Printed as hex, never as text: the fixtures are ours, but reporting offsets and sequences
// by value is the habit that keeps this layer's future logs free of user content.
std::string HexUtf8(const Bytes& bytes) {
  std::string out;
  char item[8];
  for (const unsigned char byte : bytes) {
    std::snprintf(item, sizeof(item), "%02X ", byte);
    out += item;
  }
  if (out.empty()) {
    out = "(empty)";
  }
  return out;
}

std::string HexUtf8Of(const std::string& utf8_bytes) { return HexUtf8(FromUtf8Bytes(utf8_bytes)); }

std::string HexUtf16(const Units& units) {
  std::string out;
  char item[8];
  for (const unsigned int unit : units) {
    std::snprintf(item, sizeof(item), "%04X ", unit & 0xFFFFu);
    out += item;
  }
  if (out.empty()) {
    out = "(empty)";
  }
  return out;
}

std::string HexUtf16Of(const std::wstring& utf16_code_units) {
  return HexUtf16(FromUtf16Units(utf16_code_units));
}

std::string StatusText(ConversionStatus status) {
  return std::string(DescribeConversionStatus(status));
}

std::string GleText(DWORD os_error) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "gle=%lu", static_cast<unsigned long>(os_error));
  return buffer;
}

std::string SizeText(std::size_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%zu", value);
  return buffer;
}

// --- the legal fixtures --------------------------------------------------------------
struct ValidCase {
  const char* label;
  Bytes utf8_bytes;
  Units utf16_code_units;
};

const ValidCase kValidCases[] = {
    {"empty", {}, {}},
    {"A-B-C ascii", {0x41, 0x42, 0x43}, {0x0041, 0x0042, 0x0043}},
    {"e-acute precomposed U+00E9", {0xC3, 0xA9}, {0x00E9}},
    {"A U+1F600 e-acute",
     {0x41, 0xF0, 0x9F, 0x98, 0x80, 0xC3, 0xA9},
     {0x0041, 0xD83D, 0xDE00, 0x00E9}},
    {"CJK U+4E2D U+6587", {0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87}, {0x4E2D, 0x6587}},
    {"e + U+0301 decomposed", {0x65, 0xCC, 0x81}, {0x0065, 0x0301}},
    {"A NUL B embedded", {0x41, 0x00, 0x42}, {0x0041, 0x0000, 0x0042}},
    {"U+1D0DA A U+1F4A9 many supplementary",
     {0xF0, 0x9D, 0x83, 0x9A, 0x41, 0xF0, 0x9F, 0x92, 0xA9},
     {0xD834, 0xDCDA, 0x0041, 0xD83D, 0xDCA9}},
    {"U+0080 two-byte lower bound", {0xC2, 0x80}, {0x0080}},
    {"U+FFFF three-byte upper bound", {0xEF, 0xBF, 0xBF}, {0xFFFF}},
    {"U+10FFFF four-byte upper bound", {0xF4, 0x8F, 0xBF, 0xBF}, {0xDBFF, 0xDFFF}},
};

// --- the illegal fixtures ------------------------------------------------------------
struct InvalidUtf8Case {
  const char* label;
  Bytes utf8_bytes;
};

const InvalidUtf8Case kInvalidUtf8Cases[] = {
    {"truncated two-byte lead", {0xC3}},
    {"truncated three-byte", {0xE4, 0xB8}},
    {"truncated four-byte", {0xF0, 0x9F, 0x98}},
    {"truncated after a valid sequence", {0x41, 0xC3}},
    {"bad continuation byte", {0xE4, 0x28, 0xAD}},
    {"continuation byte as lead", {0x80}},
    {"overlong two-byte NUL", {0xC0, 0x80}},
    {"overlong three-byte", {0xE0, 0x80, 0x80}},
    {"overlong four-byte", {0xF0, 0x80, 0x80, 0x80}},
    {"UTF-8 encoded surrogate U+D800", {0xED, 0xA0, 0x80}},
    {"UTF-8 encoded surrogate U+DFFF", {0xED, 0xBF, 0xBF}},
    {"above U+10FFFF lead 0xF5", {0xF5, 0x80, 0x80, 0x80}},
    {"above U+10FFFF lead 0xF7", {0xF7, 0xBF, 0xBF, 0xBF}},
    {"byte 0xFE", {0xFE}},
    {"byte 0xFF", {0xFF}},
};

struct InvalidUtf16Case {
  const char* label;
  Units utf16_code_units;
};

const InvalidUtf16Case kInvalidUtf16Cases[] = {
    {"lone high surrogate", {0xD83D}},
    {"lone low surrogate", {0xDE00}},
    {"reversed pair low then high", {0xDE00, 0xD83D}},
    {"high surrogate then high surrogate", {0xD83D, 0xD83D}},
    {"high surrogate then BMP unit", {0xD83D, 0x0041}},
    {"BMP unit then lone low surrogate", {0x0041, 0xDE00}},
    {"high surrogate at end of run", {0x0041, 0xD83D}},
};

// --- mode: encoding ------------------------------------------------------------------
void CheckValidCase(const ValidCase& fixture) {
  const std::string utf8 = ToUtf8Bytes(fixture.utf8_bytes);
  const std::wstring utf16 = ToUtf16Units(fixture.utf16_code_units);
  const std::string name = std::string(fixture.label) + ": ";

  const Utf16ConversionResult forward = ConvertUtf8BytesToUtf16CodeUnits(utf8);
  Check(forward.status == ConversionStatus::kOk, name + "utf8 -> utf16 accepted",
        StatusText(forward.status) + ", " + GleText(forward.os_error) + ", " +
            forward.diagnostic);

  const Units got_units = FromUtf16Units(forward.utf16_code_units);
  Check(got_units == fixture.utf16_code_units,
        name + "UTF-16 code units match the hand-written list",
        HexUtf16(got_units) + " want " + HexUtf16(fixture.utf16_code_units));
  Check(forward.utf16_code_units.size() == fixture.utf16_code_units.size(),
        name + "UTF-16 code unit count matches",
        SizeText(forward.utf16_code_units.size()) + " want " +
            SizeText(fixture.utf16_code_units.size()));

  const Utf8ConversionResult backward = ConvertUtf16CodeUnitsToUtf8Bytes(utf16);
  Check(backward.status == ConversionStatus::kOk, name + "utf16 -> utf8 accepted",
        StatusText(backward.status) + ", " + GleText(backward.os_error) + ", " +
            backward.diagnostic);
  const Bytes got_bytes = FromUtf8Bytes(backward.utf8_bytes);
  Check(got_bytes == fixture.utf8_bytes, name + "UTF-8 byte list matches the hand-written list",
        HexUtf8(got_bytes) + " want " + HexUtf8(fixture.utf8_bytes));
  Check(backward.utf8_bytes.size() == fixture.utf8_bytes.size(), name + "UTF-8 byte count matches",
        SizeText(backward.utf8_bytes.size()) + " want " + SizeText(fixture.utf8_bytes.size()));

  // Round trips, as extra evidence on top of the two independent lists above.
  const Utf8ConversionResult back_again = ConvertUtf16CodeUnitsToUtf8Bytes(forward.utf16_code_units);
  Check(back_again.status == ConversionStatus::kOk && FromUtf8Bytes(back_again.utf8_bytes) ==
                                                           fixture.utf8_bytes,
        name + "round trip utf8 -> utf16 -> utf8 reproduces the bytes",
        HexUtf8Of(back_again.utf8_bytes));
  const Utf16ConversionResult forth_again =
      ConvertUtf8BytesToUtf16CodeUnits(backward.utf8_bytes);
  Check(forth_again.status == ConversionStatus::kOk &&
            FromUtf16Units(forth_again.utf16_code_units) == fixture.utf16_code_units,
        name + "round trip utf16 -> utf8 -> utf16 reproduces the code units",
        HexUtf16Of(forth_again.utf16_code_units));
}

void CheckIllegalUtf8(const InvalidUtf8Case& fixture) {
  const std::string utf8 = ToUtf8Bytes(fixture.utf8_bytes);
  const Utf16ConversionResult result = ConvertUtf8BytesToUtf16CodeUnits(utf8);
  const bool refused = result.status == ConversionStatus::kInvalidInput;
  Check(refused && !result.ok(),
        std::string(fixture.label) + ": truncated/illegal UTF-8 refused as invalid-input",
        StatusText(result.status) + ", " + GleText(result.os_error));
  Check(result.utf16_code_units.empty(), std::string(fixture.label) +
                                             ": refusal exposes no half-converted output",
        HexUtf16Of(result.utf16_code_units) + " (" +
            SizeText(result.utf16_code_units.size()) + " units)");
  Check(result.os_error != 0, std::string(fixture.label) +
                                  ": the system error value was kept for diagnosis",
        GleText(result.os_error));
  std::printf("[text]   evidence invalid-utf8 %-34s bytes=%s -> %s (%s)\n", fixture.label,
              HexUtf8(fixture.utf8_bytes).c_str(), StatusText(result.status).c_str(),
              GleText(result.os_error).c_str());
}

void CheckIllegalUtf16(const InvalidUtf16Case& fixture) {
  const std::wstring utf16 = ToUtf16Units(fixture.utf16_code_units);
  const Utf8ConversionResult result = ConvertUtf16CodeUnitsToUtf8Bytes(utf16);
  Check(result.status == ConversionStatus::kInvalidInput && !result.ok(),
        std::string(fixture.label) + ": illegal UTF-16 refused as invalid-input",
        StatusText(result.status) + ", " + GleText(result.os_error));
  Check(result.utf8_bytes.empty(), std::string(fixture.label) +
                                       ": refusal exposes no half-converted output",
        HexUtf8Of(result.utf8_bytes) + " (" + SizeText(result.utf8_bytes.size()) + " bytes)");
  Check(result.os_error != 0, std::string(fixture.label) +
                                   ": the system error value was kept for diagnosis",
        GleText(result.os_error));
  std::printf("[text]   evidence invalid-utf16 %-28s units=%s -> %s (%s)\n", fixture.label,
              HexUtf16(fixture.utf16_code_units).c_str(), StatusText(result.status).c_str(),
              GleText(result.os_error).c_str());
}

int RunEncoding() {
  std::printf("[text] mode encoding: the strict conversions\n");

  for (const ValidCase& fixture : kValidCases) {
    CheckValidCase(fixture);
  }

  // The evidence line the report quotes, printed from the values the conversion produced.
  {
    const Bytes aemoji = {0x41, 0xF0, 0x9F, 0x98, 0x80, 0xC3, 0xA9};
    const Utf16ConversionResult r = ConvertUtf8BytesToUtf16CodeUnits(ToUtf8Bytes(aemoji));
    std::printf("[text] evidence A+U+1F600+e-acute: bytes=%s units=%s\n",
                HexUtf8(aemoji).c_str(), HexUtf16Of(r.utf16_code_units).c_str());
  }

  // Empty in vs failed in: both have an empty output, so ONLY the status separates them.
  {
    const Utf16ConversionResult empty = ConvertUtf8BytesToUtf16CodeUnits(std::string());
    const Utf16ConversionResult failed =
        ConvertUtf8BytesToUtf16CodeUnits(ToUtf8Bytes(Bytes{0xE4, 0xB8}));
    Check(empty.ok() && empty.utf16_code_units.empty(),
          "empty input is a successful conversion with an empty result",
          StatusText(empty.status) + ", " + SizeText(empty.utf16_code_units.size()) + " units");
    Check(!failed.ok() && failed.utf16_code_units.empty(),
          "a failed conversion is a refusal, not an empty success",
          StatusText(failed.status));
    Check(empty.status != failed.status,
          "the two empty-output cases carry different statuses",
          StatusText(empty.status) + " vs " + StatusText(failed.status));
    Check(std::string(DescribeConversionStatus(ConversionStatus::kOk)) == "ok" &&
              std::string(DescribeConversionStatus(ConversionStatus::kInvalidInput)) ==
                  "invalid-input",
          "the status names are distinct and stable", StatusText(empty.status));
    Check(empty.os_error == 0, "the empty input made no failing system call",
          GleText(empty.os_error));
  }

  // No normalisation and no case mapping: 'A' stays U+0041, and "e + U+0301" is NOT composed
  // into U+00E9 -- the exact-list checks above already pin both, these name them explicitly
  // because the SDK offers flags (MB_PRECOMPOSED, and NormalizeString) that would.
  {
    const Utf16ConversionResult decomposed =
        ConvertUtf8BytesToUtf16CodeUnits(ToUtf8Bytes(Bytes{0x65, 0xCC, 0x81}));
    Check(decomposed.utf16_code_units != std::wstring(L"\xE9"),
          "e + U+0301 was not composed into U+00E9 (no implicit normalisation)",
          HexUtf16Of(decomposed.utf16_code_units));
    const Utf16ConversionResult upper = ConvertUtf8BytesToUtf16CodeUnits(std::string("A"));
    Check(upper.utf16_code_units == std::wstring(L"A"),
          "'A' stayed U+0041 (no case mapping)", HexUtf16Of(upper.utf16_code_units));
  }

  // An embedded NUL is data, not a terminator: the byte count and the unit count both say 3.
  {
    const Utf16ConversionResult embedded_nul =
        ConvertUtf8BytesToUtf16CodeUnits(ToUtf8Bytes(Bytes{0x41, 0x00, 0x42}));
    Check(embedded_nul.utf16_code_units.size() == 3 && embedded_nul.utf16_code_units[1] == 0,
          "an embedded NUL byte survived as U+0000 (length from the view, not strlen)",
          HexUtf16Of(embedded_nul.utf16_code_units));
    const Utf8ConversionResult back =
        ConvertUtf16CodeUnitsToUtf8Bytes(std::wstring(L"A\x00" L"B", 3));
    Check(back.ok() && back.utf8_bytes.size() == 3 && back.utf8_bytes[1] == '\0',
          "an embedded U+0000 survived back to 3 bytes (not truncated at the NUL)",
          HexUtf8Of(back.utf8_bytes) + ", " + SizeText(back.utf8_bytes.size()) + " bytes");
  }

  for (const InvalidUtf8Case& fixture : kInvalidUtf8Cases) {
    CheckIllegalUtf8(fixture);
  }
  for (const InvalidUtf16Case& fixture : kInvalidUtf16Cases) {
    CheckIllegalUtf16(fixture);
  }

  // The privacy check (AGENTS.md section 2): a diagnostic must identify the failure without
  // repeating the input. The bytes below spell an ASCII word, then break -- so if the input
  // were ever echoed, the word would be in the string.
  {
    const Bytes tagged = {0x73, 0x65, 0x63, 0x72, 0x65, 0x74, 0xC3};  // "secret" + truncated lead
    const Utf16ConversionResult result = ConvertUtf8BytesToUtf16CodeUnits(ToUtf8Bytes(tagged));
    Check(!result.ok(), "an illegal sequence ending an ASCII word is refused",
        StatusText(result.status));
    Check(result.diagnostic.find("secret") == std::string::npos &&
              result.diagnostic.find("eci") == std::string::npos,
          "the diagnostic does not contain the input text", result.diagnostic);
    Check(result.diagnostic.find("gle=") != std::string::npos &&
              result.diagnostic.find("MultiByteToWideChar") != std::string::npos,
          "the diagnostic still names the API, the stage and the error value",
          result.diagnostic);
    Check(result.utf16_code_units.empty(), "and hands back nothing half-formed",
          HexUtf16Of(result.utf16_code_units));
  }

  return FinishMode();
}

// --- mode: guards --------------------------------------------------------------------
int RunGuards() {
  std::printf("[text] mode guards: the int-range arithmetic (no allocation, no deref)\n");

  const std::size_t int_max = static_cast<std::size_t>(INT_MAX);

  Check(CheckUtf8InputByteLength(0) == ConversionStatus::kOk &&
            CheckUtf8InputByteLength(1) == ConversionStatus::kOk &&
            CheckUtf8InputByteLength(int_max) == ConversionStatus::kOk,
          "UTF-8 input lengths up to INT_MAX bytes accepted",
          StatusText(CheckUtf8InputByteLength(int_max)));
  Check(CheckUtf8InputByteLength(int_max + 1) == ConversionStatus::kInputTooLong &&
            CheckUtf8InputByteLength(kSizeMax) == ConversionStatus::kInputTooLong,
          "UTF-8 input lengths above INT_MAX bytes refused",
          StatusText(CheckUtf8InputByteLength(int_max + 1)) + ", " +
              StatusText(CheckUtf8InputByteLength(kSizeMax)));

  Check(CheckUtf16InputCodeUnitLength(0) == ConversionStatus::kOk &&
            CheckUtf16InputCodeUnitLength(int_max) == ConversionStatus::kOk,
          "UTF-16 input lengths up to INT_MAX code units accepted",
          StatusText(CheckUtf16InputCodeUnitLength(int_max)));
  Check(CheckUtf16InputCodeUnitLength(int_max + 1) == ConversionStatus::kInputTooLong &&
            CheckUtf16InputCodeUnitLength(kSizeMax) == ConversionStatus::kInputTooLong,
          "UTF-16 input lengths above INT_MAX code units refused",
          StatusText(CheckUtf16InputCodeUnitLength(int_max + 1)));

  // The worst-case UTF-8 capacity: 3 bytes per code unit, bounded by division so the
  // multiply itself cannot overflow. INT_MAX / 3 is the last accepted count; one more is
  // refused, and the arithmetic below shows the bound is the right one, not an off-by-one.
  const std::size_t last_fit = static_cast<std::size_t>(INT_MAX) / 3u;
  Check(CheckUtf8OutputCapacityForUtf16CodeUnits(0) == ConversionStatus::kOk &&
            CheckUtf8OutputCapacityForUtf16CodeUnits(last_fit) == ConversionStatus::kOk,
          "UTF-8 capacity accepted up to INT_MAX/3 code units",
          StatusText(CheckUtf8OutputCapacityForUtf16CodeUnits(last_fit)));
  Check(CheckUtf8OutputCapacityForUtf16CodeUnits(last_fit + 1) == ConversionStatus::kOutputTooLong &&
            CheckUtf8OutputCapacityForUtf16CodeUnits(kSizeMax) == ConversionStatus::kOutputTooLong,
          "UTF-8 capacity refused above INT_MAX/3 code units",
          StatusText(CheckUtf8OutputCapacityForUtf16CodeUnits(last_fit + 1)));
  Check(3LL * static_cast<long long>(last_fit) <= static_cast<long long>(INT_MAX) &&
            3LL * static_cast<long long>(last_fit + 1) > static_cast<long long>(INT_MAX),
          "the INT_MAX/3 bound is exactly where 3x expansion stops fitting an int",
          SizeText(last_fit));

  // The guards run BEFORE anything is read: a view whose length is out of range is refused
  // with an empty result and no system call, so `os_error` stays 0. The data pointer is
  // null on purpose -- a conversion that ignored the length would dereference it, so this
  // check is what makes "refused before touching the input" observable rather than claimed.
  {
    const Utf16ConversionResult too_long_utf8 =
        ConvertUtf8BytesToUtf16CodeUnits(std::string_view(nullptr, int_max + 1));
    Check(!too_long_utf8.ok() && too_long_utf8.status == ConversionStatus::kInputTooLong &&
              too_long_utf8.utf16_code_units.empty() && too_long_utf8.os_error == 0,
          "an out-of-range UTF-8 byte length is refused before any system call",
          StatusText(too_long_utf8.status) + ", " + GleText(too_long_utf8.os_error));

    const Utf16ConversionResult size_max_utf8 =
        ConvertUtf8BytesToUtf16CodeUnits(std::string_view(nullptr, kSizeMax));
    Check(!size_max_utf8.ok() && size_max_utf8.status == ConversionStatus::kInputTooLong,
          "SIZE_MAX is refused too, not truncated into the accepted range",
          StatusText(size_max_utf8.status));

    const Utf8ConversionResult too_long_utf16 =
        ConvertUtf16CodeUnitsToUtf8Bytes(std::wstring_view(nullptr, int_max + 1));
    Check(!too_long_utf16.ok() && too_long_utf16.status == ConversionStatus::kInputTooLong &&
              too_long_utf16.utf8_bytes.empty() && too_long_utf16.os_error == 0,
          "an out-of-range UTF-16 code unit length is refused before any system call",
          StatusText(too_long_utf16.status) + ", " + GleText(too_long_utf16.os_error));

    const Utf8ConversionResult size_max_utf16 =
        ConvertUtf16CodeUnitsToUtf8Bytes(std::wstring_view(nullptr, kSizeMax));
    Check(!size_max_utf16.ok() && size_max_utf16.status == ConversionStatus::kInputTooLong,
          "SIZE_MAX UTF-16 length is refused, not wrapped", StatusText(size_max_utf16.status));

    std::printf("[text]   evidence guards INT_MAX=%d INT_MAX+1 refused=%s capacity bound=%zu\n",
                INT_MAX, StatusText(too_long_utf8.status).c_str(), last_fit);
  }

  return FinishMode();
}

// --- the falsifiability control -------------------------------------------------------
int RunSelfCheckFail() {
  std::printf("[text] mode self-check-fail: one deliberately false claim\n");
  // The false claim: that a truncated UTF-8 sequence converts successfully. The encoding
  // mode pins the opposite, so this must fail here -- exit 14, no RESULT PASS. Without this
  // control, a green run whose checks had all become tautologies would be indistinguishable.
  const Utf16ConversionResult truncated =
      ConvertUtf8BytesToUtf16CodeUnits(ToUtf8Bytes(Bytes{0xE4, 0xB8}));
  Check(truncated.status == ConversionStatus::kOk,
        "deliberately asserted that a truncated UTF-8 sequence converts successfully",
        StatusText(truncated.status));
  std::printf("[text] RESULT FAIL (self-check-fail control, %d checks, %d failed)\n", g_total,
              g_failed);
  return g_failed == 0 ? kExitOk : kExitSelfCheckFailed;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::wstring mode;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--mode" && i + 1 < argc) {
      mode = argv[++i];
    } else {
      std::printf("usage: text_bridge --mode encoding|guards|self-check-fail\n");
      return kExitUsage;
    }
  }
  if (mode == L"encoding") {
    return RunEncoding();
  }
  if (mode == L"guards") {
    return RunGuards();
  }
  if (mode == L"self-check-fail") {
    return RunSelfCheckFail();
  }
  std::printf("usage: text_bridge --mode encoding|guards|self-check-fail\n");
  return kExitUsage;
}
