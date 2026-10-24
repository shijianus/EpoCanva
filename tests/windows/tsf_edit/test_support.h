// EpoCanva P3.7 -- shared check helpers for the TSF edit verification programs.
//
// The checks judge behaviour, not log text: an exact status/reason, an exact UTF-16 code-unit
// expectation, an exact HRESULT, or an exact counter. Failure messages never echo document
// text; they report lengths and the first differing UTF-16 offset instead.
#ifndef EPOCANVA_TESTS_WINDOWS_TSF_EDIT_TEST_SUPPORT_H_
#define EPOCANVA_TESTS_WINDOWS_TSF_EDIT_TEST_SUPPORT_H_

#include <windows.h>

#include <cstdint>
#include <string>

namespace epocanva {
namespace tsf {
namespace edit {
namespace test {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitCheckFailed = 5;
constexpr int kExitSelfCheckFailed = 14;

extern int g_total;
extern int g_failed;

std::string HrText(HRESULT hr);

void Check(bool condition, const std::string& what, const std::string& got);
void CheckHr(HRESULT hr, HRESULT want, const std::string& what);
void CheckU64(std::uint64_t value, std::uint64_t minimum, const std::string& what);
void CheckText(const std::wstring& actual, const std::wstring& expected,
               const std::string& what);

int Finish();

}  // namespace test
}  // namespace edit
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TESTS_WINDOWS_TSF_EDIT_TEST_SUPPORT_H_
