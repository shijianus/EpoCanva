// EpoCanva P3.10 -- entry point of the context-observer verification program.
//
// EXIT CODES: 0 pass; 2 usage; 5 a check failed; 14 the falsifiability control; 77 the
// real-focus case could not be exercised in this environment (CTest reports SKIPPED).
#include <cstdio>
#include <cstring>
#include <string>

#include "observer_test_support.h"
#include "observer_tests.h"

int main(int argc, char** argv) {
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--interactive-focus") == 0) {
      return epocanva::tsf::context::test::RunInteractiveFocus();
    }
  }
  std::string case_name;
  for (int index = 1; index < argc; ++index) {
    if (std::strcmp(argv[index], "--case") == 0 && index + 1 < argc) {
      case_name = argv[++index];
    }
  }
  if (case_name.empty()) {
    std::printf(
        "usage: tsf_context_observer_tests --case <case>\n"
        "       tsf_context_observer_tests --interactive-focus\n"
        "cases: registry-identity initial-binding push-pop-top external-selection\n"
        "       external-text noncollapsed-selection document-uninit own-edit real-focus\n"
        "       shutdown-late-read target-evidence target-policy resource-balance can-fail\n");
    return epocanva::tsf::context::test::kExitUsage;
  }
  const int code = epocanva::tsf::context::test::RunObserverCase(case_name);
  if (code >= 0) {
    return code;
  }
  std::printf("unknown case: %s\n", case_name.c_str());
  return epocanva::tsf::context::test::kExitUsage;
}
