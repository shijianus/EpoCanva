// EpoCanva P3.10 -- observer verification cases.
#ifndef EPOCANVA_TESTS_WINDOWS_TSF_CONTEXT_OBSERVER_OBSERVER_TESTS_H_
#define EPOCANVA_TESTS_WINDOWS_TSF_CONTEXT_OBSERVER_OBSERVER_TESTS_H_

#include <string>

namespace epocanva {
namespace tsf {
namespace context {
namespace test {

// Returns the case's exit code, or -1 when `case_name` is not an observer case.
int RunObserverCase(const std::string& case_name);

// The independent, interactive real-focus mode. It is not registered as an automated
// CTest case and it never claims an automated pass.
int RunInteractiveFocus();

}  // namespace test
}  // namespace context
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TESTS_WINDOWS_TSF_CONTEXT_OBSERVER_OBSERVER_TESTS_H_
