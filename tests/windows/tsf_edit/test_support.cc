// EpoCanva P3.7 -- shared check helpers.
#include "test_support.h"

#include <cstdio>

namespace epocanva {
namespace tsf {
namespace edit {
namespace test {

int g_total = 0;
int g_failed = 0;

std::string HrText(HRESULT hr) {
  char buffer[24] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
  return buffer;
}

void Check(bool condition, const std::string& what, const std::string& got) {
  ++g_total;
  if (condition) {
    std::printf("[tsf-edit]   ok   %s\n", what.c_str());
  } else {
    ++g_failed;
    std::printf("[tsf-edit]   FAIL %s  (got: %s)\n", what.c_str(), got.c_str());
  }
}

void CheckHr(HRESULT hr, HRESULT want, const std::string& what) {
  Check(hr == want, what, "hr=" + HrText(hr) + " want=" + HrText(want));
}

void CheckU64(std::uint64_t value, std::uint64_t minimum, const std::string& what) {
  Check(value >= minimum, what,
        std::to_string(value) + " (want >= " + std::to_string(minimum) + ")");
}

void CheckText(const std::wstring& actual, const std::wstring& expected,
               const std::string& what) {
  if (actual == expected) {
    Check(true, what, "");
    return;
  }
  std::size_t first_difference = 0;
  const std::size_t shorter =
      actual.size() < expected.size() ? actual.size() : expected.size();
  while (first_difference < shorter &&
         actual[first_difference] == expected[first_difference]) {
    ++first_difference;
  }
  Check(false, what,
        "length_utf16=" + std::to_string(actual.size()) + " expected=" +
            std::to_string(expected.size()) + " first_diff_utf16=" +
            std::to_string(first_difference));
}

int Finish() {
  if (g_failed != 0) {
    std::printf("[tsf-edit] RESULT FAIL (%d checks, %d failed)\n", g_total, g_failed);
    return kExitCheckFailed;
  }
  std::printf("[tsf-edit] RESULT PASS (%d checks, %d failed)\n", g_total, g_failed);
  return kExitOk;
}

}  // namespace test
}  // namespace edit
}  // namespace tsf
}  // namespace epocanva
