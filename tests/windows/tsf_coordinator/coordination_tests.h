// EpoCanva P3.8 -- stage-1 coordination cases (controlled engine seam, real TSF store).
#ifndef EPOCANVA_TESTS_WINDOWS_TSF_COORDINATOR_COORDINATION_TESTS_H_
#define EPOCANVA_TESTS_WINDOWS_TSF_COORDINATOR_COORDINATION_TESTS_H_

#include <string>

namespace epocanva {
namespace tsf {
namespace coordinator {
namespace test {

// Returns the case's exit code, or -1 when `case_name` is not a coordination case.
int RunCoordinationCase(const std::string& case_name);

}  // namespace test
}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TESTS_WINDOWS_TSF_COORDINATOR_COORDINATION_TESTS_H_
