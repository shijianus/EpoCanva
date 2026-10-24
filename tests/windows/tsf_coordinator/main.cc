// EpoCanva P3.8 -- the coordinator verification program.
//
// STAGE 1 (this commit): the coordination cases over the existing controlled-engine seam
// and the REAL P3.7 TSF host -- binding and gates, single flight, composition progress,
// pure evaluation, generation invalidation (engine-pending and edit-pending), permission
// changes at both re-verification points, clean edit refusals, the engine-fault NeedsResync
// block, close-with-pending-work, pump idempotence, the pure conversion matrix and a
// falsifiability control. The engine is deliberately not rime here.
//
// STAGE 2 (next commit): the same coordinator against the pinned real English engine and
// the real text store -- the end-to-end typing flows.
//
// EXIT CODES: 0 pass; 2 usage; 5 a check failed; 14 the self-check's expected failure.
#include <cstdio>
#include <cstring>
#include <string>

#include "coordinator_test_support.h"
#include "coordination_tests.h"
#include "real_engine_tests.h"

int main(int argc, char** argv) {
  std::string case_name;
  epocanva::tsf::coordinator::test::Options options;
  epocanva::tsf::coordinator::test::ParseEngineArgs(argc, argv, &options, &case_name);
  if (case_name.empty()) {
    std::printf("usage: tsf_coordinator_tests --case <case> [--rime-dll <path>] "
                "[--fixture-dir <path>] [--spelling-fixture-dir <path>] "
                "[--run-root <path>]\n");
    return epocanva::tsf::coordinator::test::kExitUsage;
  }
  const int code =
      epocanva::tsf::coordinator::test::RunCoordinationCase(case_name);
  if (code >= 0) {
    return code;
  }
  const int real_code =
      epocanva::tsf::coordinator::test::RunRealEngineCase(case_name, options);
  if (real_code >= 0) {
    return real_code;
  }
  std::printf("unknown case: %s\n", case_name.c_str());
  return epocanva::tsf::coordinator::test::kExitUsage;
}
