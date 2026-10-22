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
// carries its expected byte list AND its expected code unit list. The middle fixture's
// offsets are the contract test: UTF-8 0,1,5,7 against UTF-16 0,1,3,4, with UTF-16 offset 2
// (the low half of the emoji's surrogate pair) refused.
//
// ILLEGAL INPUT IS A RESULT, NOT A LOG LINE. Every rejected case asserts the status, that
// the output is empty, and that a GetLastError value was carried out. The privacy rule is
// checked from the other side: a rejected input whose bytes include an ASCII word must
// produce a diagnostic that does NOT contain that word, because AGENTS.md section 2 forbids
// this layer from echoing user text anywhere.
//
// MODES
//   encoding         the strict conversions, legal and illegal, both directions
//   boundary         the boundary table: exact contents, two-way round trips, refusals
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

#include "text_boundary_map.h"
#include "text_encoding.h"

namespace {

using epocanva::windows::text::BoundaryLookupStatus;
using epocanva::windows::text::BuildUtf8BoundaryMap;
using epocanva::windows::text::CheckUtf16InputCodeUnitLength;
using epocanva::windows::text::CheckUtf8InputByteLength;
using epocanva::windows::text::CheckUtf8OutputCapacityForUtf16CodeUnits;
using epocanva::windows::text::ConversionStatus;
using epocanva::windows::text::ConvertUtf16CodeUnitsToUtf8Bytes;
using epocanva::windows::text::ConvertUtf8BytesToUtf16CodeUnits;
using epocanva::windows::text::DescribeConversionStatus;
using epocanva::windows::text::Utf16ConversionResult;
using epocanva::windows::text::Utf8BoundaryMap;
using epocanva::windows::text::Utf8BoundaryMapBuildResult;
using epocanva::windows::text::Utf8ConversionResult;
using epocanva::windows::text::Utf8Utf16Boundary;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitCheckFailed = 5;
constexpr int kExitSelfCheckFailed = 14;

// A value no legitimate offset can be (it is above INT_MAX, which bounds every offset this
// layer accepts), seeded into every out-parameter that a refused lookup is expected NOT to
// touch.
constexpr std::size_t kSentinel = static_cast<std::size_t>(0xDEADBEEFu);
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

std::string LookupStatusText(BoundaryLookupStatus status) {
  return std::string(Utf8BoundaryMap::DescribeLookupStatus(status));
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

// --- the boundary fixtures -----------------------------------------------------------
struct BoundaryCase {
  const char* label;
  Bytes utf8_bytes;
  // The exact expected table: every legal boundary, in both units, ascending.
  std::vector<Utf8Utf16Boundary> expected_table;
  // Offsets strictly inside a multi-byte UTF-8 sequence: must be refused.
  std::vector<std::size_t> interior_utf8_byte_offsets;
  // Offsets strictly inside a surrogate pair: must be refused.
  std::vector<std::size_t> interior_utf16_code_unit_offsets;
};

const BoundaryCase kBoundaryCases[] = {
    {"empty", {}, {{0u, 0u}}, {}, {}},
    {"A-B-C ascii",
     {0x41, 0x42, 0x43},
     {{0u, 0u}, {1u, 1u}, {2u, 2u}, {3u, 3u}},
     {},
     {}},
    {"A U+1F600 e-acute",
     {0x41, 0xF0, 0x9F, 0x98, 0x80, 0xC3, 0xA9},
     // UTF-8 byte offsets 0,1,5,7 <-> UTF-16 code unit offsets 0,1,3,4: the offset lists the
     // P3.2 brief states, transcribed by hand rather than computed.
     {{0u, 0u}, {1u, 1u}, {5u, 3u}, {7u, 4u}},
     {2u, 3u, 4u, 6u},  // inside the emoji's bytes 2..4 and inside e-acute's byte 6
     {2u}},             // the low half of the emoji's surrogate pair
    {"CJK U+4E2D U+6587",
     {0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87},
     {{0u, 0u}, {3u, 1u}, {6u, 2u}},
     {1u, 2u, 4u, 5u},
     {}},
    {"e + U+0301 decomposed",
     {0x65, 0xCC, 0x81},
     {{0u, 0u}, {1u, 1u}, {3u, 2u}},
     {2u},
     {}},  // every UTF-16 gap here is 1, so there is no interior code unit offset to refuse
    {"A NUL B embedded",
     {0x41, 0x00, 0x42},
     {{0u, 0u}, {1u, 1u}, {2u, 2u}, {3u, 3u}},
     {},
     {}},
    {"U+1D0DA A U+1F4A9 many supplementary",
     {0xF0, 0x9D, 0x83, 0x9A, 0x41, 0xF0, 0x9F, 0x92, 0xA9},
     {{0u, 0u}, {4u, 2u}, {5u, 3u}, {9u, 5u}},
     {1u, 2u, 3u, 6u, 7u, 8u},
     {1u, 4u}},
    {"U+10FFFF four-byte upper bound",
     {0xF4, 0x8F, 0xBF, 0xBF},
     {{0u, 0u}, {4u, 2u}},
     {1u, 2u, 3u},
     {1u}},
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

// --- mode: boundary ------------------------------------------------------------------
void CheckTable(const BoundaryCase& fixture, const Utf8BoundaryMap& map) {
  const std::string name = std::string(fixture.label) + ": ";
  Check(map.boundary_count() == fixture.expected_table.size(),
        name + "boundary count is the scalar value count + 1",
        SizeText(map.boundary_count()) + " want " + SizeText(fixture.expected_table.size()));
  bool table_exact = map.boundary_count() == fixture.expected_table.size();
  std::string got_table;
  std::string want_table;
  for (std::size_t i = 0; i < map.boundaries().size(); ++i) {
    char item[24];
    std::snprintf(item, sizeof(item), "(%u,%u) ", map.boundaries()[i].utf8_byte_offset,
                  map.boundaries()[i].utf16_code_unit_offset);
    got_table += item;
    if (i < fixture.expected_table.size()) {
      std::snprintf(item, sizeof(item), "(%u,%u) ", fixture.expected_table[i].utf8_byte_offset,
                    fixture.expected_table[i].utf16_code_unit_offset);
      want_table += item;
      if (map.boundaries()[i].utf8_byte_offset != fixture.expected_table[i].utf8_byte_offset ||
          map.boundaries()[i].utf16_code_unit_offset !=
              fixture.expected_table[i].utf16_code_unit_offset) {
        table_exact = false;
      }
    } else {
      table_exact = false;
    }
  }
  Check(table_exact, name + "the whole boundary table equals the hand-written table",
        got_table + " want " + want_table);
  std::printf("[text]   evidence boundaries %-32s bytes=%s table=%s\n", fixture.label,
              HexUtf8(fixture.utf8_bytes).c_str(), got_table.c_str());
}

void CheckBoundaryCase(const BoundaryCase& fixture) {
  const Utf8BoundaryMapBuildResult built = BuildUtf8BoundaryMap(ToUtf8Bytes(fixture.utf8_bytes));
  const std::string name = std::string(fixture.label) + ": ";
  if (!built.ok()) {
    Check(false, name + "boundary map build accepted the legal text",
          StatusText(built.status) + ", " + built.diagnostic);
    return;
  }
  Check(true, name + "boundary map build accepted the legal text", StatusText(built.status));
  const Utf8BoundaryMap& map = built.boundary_map;
  CheckTable(fixture, map);

  Check(map.utf8_byte_length() == fixture.utf8_bytes.size(),
        name + "utf8_byte_length() is in UTF-8 code units",
        SizeText(map.utf8_byte_length()) + " want " + SizeText(fixture.utf8_bytes.size()));
  Check(map.utf16_code_unit_length() == fixture.expected_table.back().utf16_code_unit_offset,
        name + "utf16_code_unit_length() is in UTF-16 code units",
        SizeText(map.utf16_code_unit_length()));

  // Every legal boundary, both directions, and the composition of the two.
  bool round_trips = true;
  for (const Utf8Utf16Boundary& boundary : map.boundaries()) {
    std::size_t units = kSentinel;
    const BoundaryLookupStatus forth =
        map.MapUtf8ByteOffsetToUtf16CodeUnitOffset(boundary.utf8_byte_offset, &units);
    if (forth != BoundaryLookupStatus::kMapped || units != boundary.utf16_code_unit_offset) {
      round_trips = false;
    }
    std::size_t bytes = kSentinel;
    const BoundaryLookupStatus back =
        map.MapUtf16CodeUnitOffsetToUtf8ByteOffset(boundary.utf16_code_unit_offset, &bytes);
    if (back != BoundaryLookupStatus::kMapped || bytes != boundary.utf8_byte_offset) {
      round_trips = false;
    }
    // utf8 -> utf16 -> utf8 must land exactly where it started.
    std::size_t bytes_again = kSentinel;
    if (units != kSentinel) {
      const BoundaryLookupStatus again =
          map.MapUtf16CodeUnitOffsetToUtf8ByteOffset(units, &bytes_again);
      if (again != BoundaryLookupStatus::kMapped || bytes_again != boundary.utf8_byte_offset) {
        round_trips = false;
      }
    }
  }
  Check(round_trips, name + "every legal boundary round-trips in both directions",
        round_trips ? "all mapped" : "see table above");

  // The end offset is a legal boundary: the zero-length position at the end of the text.
  {
    std::size_t units = kSentinel;
    const BoundaryLookupStatus at_end =
        map.MapUtf8ByteOffsetToUtf16CodeUnitOffset(map.utf8_byte_length(), &units);
    Check(at_end == BoundaryLookupStatus::kMapped &&
              units == map.utf16_code_unit_length(),
          name + "the end offset maps (no clamp, no rounding needed to reach it)",
          LookupStatusText(at_end) + " -> " + SizeText(units));
  }

  // Interior offsets: refused, with the reason named, and the out-parameter untouched.
  bool interiors_refused = true;
  std::string interior_detail;
  for (const std::size_t offset : fixture.interior_utf8_byte_offsets) {
    std::size_t sink = kSentinel;
    const BoundaryLookupStatus status = map.MapUtf8ByteOffsetToUtf16CodeUnitOffset(offset, &sink);
    if (status != BoundaryLookupStatus::kInsideUtf8Sequence || sink != kSentinel) {
      interiors_refused = false;
      char item[48];
      std::snprintf(item, sizeof(item), "byte %zu -> %s out=%s; ", offset,
                    LookupStatusText(status).c_str(),
                    sink == kSentinel ? "untouched" : "OVERWRITTEN");
      interior_detail += item;
    }
  }
  for (const std::size_t offset : fixture.interior_utf16_code_unit_offsets) {
    std::size_t sink = kSentinel;
    const BoundaryLookupStatus status =
        map.MapUtf16CodeUnitOffsetToUtf8ByteOffset(offset, &sink);
    if (status != BoundaryLookupStatus::kInsideUtf16SurrogatePair || sink != kSentinel) {
      interiors_refused = false;
      char item[48];
      std::snprintf(item, sizeof(item), "unit %zu -> %s out=%s; ", offset,
                    LookupStatusText(status).c_str(),
                    sink == kSentinel ? "untouched" : "OVERWRITTEN");
      interior_detail += item;
    }
  }
  Check(interiors_refused,
        name + "interior offsets refused, with the out-parameter left untouched",
        interior_detail.empty() ? "all interior offsets refused as expected"
                                : interior_detail.c_str());

  // Out of range starts one past the end offset, in both units.
  bool out_of_range_refused = true;
  for (std::size_t extra = 1; extra <= 3; ++extra) {
    std::size_t sink = kSentinel;
    if (map.MapUtf8ByteOffsetToUtf16CodeUnitOffset(map.utf8_byte_length() + extra, &sink) !=
        BoundaryLookupStatus::kUtf8ByteOffsetOutOfRange) {
      out_of_range_refused = false;
    }
    sink = kSentinel;
    if (map.MapUtf16CodeUnitOffsetToUtf8ByteOffset(map.utf16_code_unit_length() + extra, &sink) !=
        BoundaryLookupStatus::kUtf16CodeUnitOffsetOutOfRange) {
      out_of_range_refused = false;
    }
  }
  Check(out_of_range_refused, name + "offsets past the end are refused as out of range",
        out_of_range_refused ? "refused" : "accepted something out of range");

  // A null out-parameter is refused rather than written through.
  {
    const bool null_byte =
        map.MapUtf8ByteOffsetToUtf16CodeUnitOffset(0, nullptr) ==
        BoundaryLookupStatus::kNullOutParameter;
    const bool null_unit =
        map.MapUtf16CodeUnitOffsetToUtf8ByteOffset(0, nullptr) ==
        BoundaryLookupStatus::kNullOutParameter;
    Check(null_byte && null_unit, name + "a null out-parameter is refused in both directions",
          null_byte && null_unit ? "refused" : "written through");
  }
}

int RunBoundary() {
  std::printf("[text] mode boundary: the offset boundary table\n");

  for (const BoundaryCase& fixture : kBoundaryCases) {
    CheckBoundaryCase(fixture);
  }

  // The contract the brief names, as its own judgement: for "A U+1F600 e-acute" UTF-16
  // offset 2 is the low half of the surrogate pair and must be refused, while 0,1,3,4 map.
  {
    const Utf8BoundaryMapBuildResult built =
        BuildUtf8BoundaryMap(ToUtf8Bytes(Bytes{0x41, 0xF0, 0x9F, 0x98, 0x80, 0xC3, 0xA9}));
    const Utf8BoundaryMap& map = built.boundary_map;
    std::size_t sink = kSentinel;
    Check(map.MapUtf16CodeUnitOffsetToUtf8ByteOffset(2u, &sink) ==
              BoundaryLookupStatus::kInsideUtf16SurrogatePair &&
              sink == kSentinel,
          "A U+1F600 e-acute: UTF-16 offset 2 refused as inside a surrogate pair",
          LookupStatusText(map.MapUtf16CodeUnitOffsetToUtf8ByteOffset(2u, &sink)));
    sink = kSentinel;
    Check(map.MapUtf8ByteOffsetToUtf16CodeUnitOffset(4u, &sink) ==
              BoundaryLookupStatus::kInsideUtf8Sequence &&
              sink == kSentinel,
          "A U+1F600 e-acute: UTF-8 offset 4 refused as inside the emoji's sequence",
          LookupStatusText(map.MapUtf8ByteOffsetToUtf16CodeUnitOffset(4u, &sink)));
    sink = kSentinel;
    Check(map.MapUtf8ByteOffsetToUtf16CodeUnitOffset(6u, &sink) ==
              BoundaryLookupStatus::kInsideUtf8Sequence &&
              sink == kSentinel,
          "A U+1F600 e-acute: UTF-8 offset 6 refused as inside e-acute's sequence",
          LookupStatusText(map.MapUtf8ByteOffsetToUtf16CodeUnitOffset(6u, &sink)));
    std::size_t units = 0;
    Check(map.MapUtf8ByteOffsetToUtf16CodeUnitOffset(5u, &units) ==
              BoundaryLookupStatus::kMapped &&
              units == 3u,
          "A U+1F600 e-acute: UTF-8 offset 5 maps to UTF-16 offset 3",
          SizeText(units));
  }

  // A default-constructed map IS the empty text's map: 0 <-> 0 legal, 1 refused.
  {
    const Utf8BoundaryMap empty;
    std::size_t units = kSentinel;
    std::size_t bytes = kSentinel;
    Check(empty.boundary_count() == 1 && empty.utf8_byte_length() == 0 &&
              empty.utf16_code_unit_length() == 0,
          "an empty map holds exactly the 0<->0 boundary", SizeText(empty.boundary_count()));
    Check(empty.MapUtf8ByteOffsetToUtf16CodeUnitOffset(0, &units) ==
              BoundaryLookupStatus::kMapped &&
          units == 0, "empty map: 0 UTF-8 bytes maps to 0 UTF-16 code units",
          SizeText(units));
    units = kSentinel;
    Check(empty.MapUtf16CodeUnitOffsetToUtf8ByteOffset(0, &bytes) ==
              BoundaryLookupStatus::kMapped &&
          bytes == 0, "empty map: 0 UTF-16 code units maps to 0 UTF-8 bytes",
          SizeText(bytes));
    Check(empty.MapUtf8ByteOffsetToUtf16CodeUnitOffset(1, &units) ==
              BoundaryLookupStatus::kUtf8ByteOffsetOutOfRange &&
          units == kSentinel,
          "empty map: offset 1 refused as out of range, out-parameter untouched",
          SizeText(units));
  }

  // An illegal text produces NO map: the build refuses and the map it returns is the empty
  // text's, so a caller cannot read a partial table built from half a rejected sequence.
  {
    const Utf8BoundaryMapBuildResult built = BuildUtf8BoundaryMap(ToUtf8Bytes(
        Bytes{0x41, 0xE4, 0xB8}));  // "A" then a truncated three-byte sequence
    Check(!built.ok() && built.status == ConversionStatus::kInvalidInput,
          "a boundary map build over illegal UTF-8 is refused as invalid-input",
          StatusText(built.status) + ", " + GleText(built.os_error));
    Check(built.boundary_map.utf8_byte_length() == 0 &&
              built.boundary_map.boundary_count() == 1,
          "the refused build returns no partial table",
          SizeText(built.boundary_map.boundary_count()) + " boundaries, " +
              SizeText(built.boundary_map.utf8_byte_length()) + " bytes");
    Check(!built.diagnostic.empty(), "the refused build carries a diagnostic",
          built.diagnostic);
    std::printf("[text]   evidence build-refusal bytes=%s status=%s (%s) diagnostic=%s\n",
                HexUtf8(Bytes{0x41, 0xE4, 0xB8}).c_str(), StatusText(built.status).c_str(),
                GleText(built.os_error).c_str(), built.diagnostic.c_str());
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

    const Utf8BoundaryMapBuildResult built =
        BuildUtf8BoundaryMap(std::string_view(nullptr, kSizeMax));
    Check(!built.ok() && built.status == ConversionStatus::kInputTooLong &&
              built.boundary_map.boundary_count() == 1,
          "the boundary map builder refuses an out-of-range length with no table",
          StatusText(built.status));

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
      std::printf("usage: text_bridge --mode encoding|boundary|guards|self-check-fail\n");
      return kExitUsage;
    }
  }
  if (mode == L"encoding") {
    return RunEncoding();
  }
  if (mode == L"boundary") {
    return RunBoundary();
  }
  if (mode == L"guards") {
    return RunGuards();
  }
  if (mode == L"self-check-fail") {
    return RunSelfCheckFail();
  }
  std::printf("usage: text_bridge --mode encoding|boundary|guards|self-check-fail\n");
  return kExitUsage;
}
