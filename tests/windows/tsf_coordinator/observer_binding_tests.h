// EpoCanva P3.10 -- coordinator integration cases over the real context observer.
#ifndef EPOCANVA_TESTS_WINDOWS_TSF_COORDINATOR_OBSERVER_BINDING_TESTS_H_
#define EPOCANVA_TESTS_WINDOWS_TSF_COORDINATOR_OBSERVER_BINDING_TESTS_H_

#include <string>

#include "engine_executor/test_support.h"

namespace epocanva {
namespace tsf {
namespace coordinator {
namespace test {

// Returns the case's exit code, or -1 when `case_name` is not an observed-binding case.
int RunObservedBindingCase(const std::string& case_name,
                           const epocanva::tests::engine_executor::Options& options);

}  // namespace test
}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TESTS_WINDOWS_TSF_COORDINATOR_OBSERVER_BINDING_TESTS_H_
