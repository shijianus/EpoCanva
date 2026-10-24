// EpoCanva P3.7 -- composition-layer verification cases.
#ifndef EPOCANVA_TESTS_WINDOWS_TSF_EDIT_COMPOSITION_TESTS_H_
#define EPOCANVA_TESTS_WINDOWS_TSF_EDIT_COMPOSITION_TESTS_H_

#include <string>

namespace epocanva {
namespace tsf {
namespace edit {
namespace test {

// Returns the case's exit code, or -1 when the name is not a composition case.
int RunCompositionCase(const std::string& case_name);

}  // namespace test
}  // namespace edit
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TESTS_WINDOWS_TSF_EDIT_COMPOSITION_TESTS_H_
