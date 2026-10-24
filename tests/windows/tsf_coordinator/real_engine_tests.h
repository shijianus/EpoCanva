// EpoCanva P3.8 -- stage-2 real-engine cases (pinned English engine + real TSF store).
#ifndef EPOCANVA_TESTS_WINDOWS_TSF_COORDINATOR_REAL_ENGINE_TESTS_H_
#define EPOCANVA_TESTS_WINDOWS_TSF_COORDINATOR_REAL_ENGINE_TESTS_H_

#include <string>

#include "coordinator_test_support.h"

namespace epocanva {
namespace tsf {
namespace coordinator {
namespace test {

// Returns the case's exit code, or -1 when `case_name` is not a real-engine case.
// `options` carries the already-parsed --rime-dll/--fixture-dir/--spelling-fixture-dir/
// --run-root arguments.
int RunRealEngineCase(const std::string& case_name, const Options& options);

}  // namespace test
}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TESTS_WINDOWS_TSF_COORDINATOR_REAL_ENGINE_TESTS_H_
