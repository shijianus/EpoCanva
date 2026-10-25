// EpoCanva P3.9 -- entry point of the input-permission verification program.
//
// EXIT CODES: 0 pass; 2 usage; 5 a check failed; 14 the falsifiability control.
#include <cstring>
#include <string>

#include "permission_tests.h"

namespace {

const struct {
  const char* name;
  int (*runner)();
} kCases[] = {
    {"pure-classification", epocanva::tsf::context::test::RunPureClassification},
    {"real-path-probe", epocanva::tsf::context::test::RunRealPathProbe},
    {"plain-editable-allowed", epocanva::tsf::context::test::RunPlainEditableAllowed},
    {"read-only-denied", epocanva::tsf::context::test::RunReadOnlyDenied},
    {"keyboard-disabled-values", epocanva::tsf::context::test::RunKeyboardDisabledValues},
    {"empty-context-denied", epocanva::tsf::context::test::RunEmptyContextDenied},
    {"sensitive-scopes-denied", epocanva::tsf::context::test::RunSensitiveScopesDenied},
    {"scope-absent-wrongtype-fail", epocanva::tsf::context::test::RunScopeAbsentWrongTypeFail},
    {"cleared-compartment-unknown", epocanva::tsf::context::test::RunClearedCompartmentRecreated},
    {"repeated-queries-consistent", epocanva::tsf::context::test::RunRepeatedQueriesConsistent},
    {"async-query-pending", epocanva::tsf::context::test::RunAsyncQueryPending},
    {"abandon-pending-query", epocanva::tsf::context::test::RunAbandonPendingQuery},
    {"resource-balance", epocanva::tsf::context::test::RunResourceBalance},
    {"can-fail", epocanva::tsf::context::test::RunCanFail},
};

}  // namespace

int main(int argc, char** argv) {
  std::string case_name;
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--case") == 0 && index + 1 < argc) {
      case_name = argv[++index];
    }
  }
  if (case_name.empty()) {
    std::fprintf(stderr, "usage: %s --case <name>\n", argv[0]);
    return epocanva::tsf::context::test::kExitUsage;
  }
  for (const auto& entry : kCases) {
    if (case_name == entry.name) {
      return entry.runner();
    }
  }
  std::fprintf(stderr, "unknown case: %s\n", case_name.c_str());
  return epocanva::tsf::context::test::kExitUsage;
}
