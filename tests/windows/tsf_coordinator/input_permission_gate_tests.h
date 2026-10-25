// EpoCanva P3.9 -- coordinator admission cases driven by REAL TSF metadata.
//
// Every case here builds the production permission source with
// CreateRealPermissionSource and lets it read the real document's GetStatus, context
// compartments and GUID_PROP_INPUTSCOPE through real edit sessions. No case hands the
// coordinator an Allowed value. The engine is the controlled scheduling backend (not
// rime), so engine-call counts are direct evidence rather than inferred from text.
#ifndef EPOCANVA_TESTS_WINDOWS_TSF_COORDINATOR_INPUT_PERMISSION_GATE_TESTS_H_
#define EPOCANVA_TESTS_WINDOWS_TSF_COORDINATOR_INPUT_PERMISSION_GATE_TESTS_H_

#include <string>

namespace epocanva {
namespace tsf {
namespace coordinator {
namespace test {

// Returns the case's exit code, or -1 when `case_name` is not a permission-gate case.
int RunInputPermissionGateCase(const std::string& case_name);

}  // namespace test
}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TESTS_WINDOWS_TSF_COORDINATOR_INPUT_PERMISSION_GATE_TESTS_H_
