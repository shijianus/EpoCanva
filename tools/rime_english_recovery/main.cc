// EpoCanva P2.6-recovery -- command line entry and mode dispatch. Nothing else.
//
// WHY THIS FILE IS SMALL. The integration harness it replaces was one ~5,100 line
// translation unit holding the engine lifecycle, every mode's expectations and the
// reporting together; that coupling is what made a single bad write destroy all of it.
// Here the lifecycle and observation live in recovery_test_support.{h,cc} and the
// behavioural expectations in completion_tests.{h,cc}, so adding the next phase's mode
// means adding a file, not editing this one.
//
// The program's exit code is a stage code (recovery_test_support.h) and its success
// marker is the line "RESULT PASS". CTest judges both: an exact integer exit code plus
// a marker policy, never a regex over the log. See tests/english_completion/
// assert_exit.cmake.
#include <cstdio>

#include "completion_tests.h"
#include "recovery_test_support.h"

namespace {

using epocanva::recovery::Mode;
using epocanva::recovery::ModeFn;

struct ModeEntry {
  Mode mode;
  ModeFn fn;
};

// Every mode ParseArgs accepts must appear here, and only modes that are actually
// implemented are listed -- no placeholder entry for a phase that has not been rebuilt
// yet.
constexpr ModeEntry kModes[] = {
    {Mode::kCompletion, epocanva::recovery::RunCompletionMode},
    {Mode::kExpectModuleAbsent, epocanva::recovery::RunExpectModuleAbsentMode},
    {Mode::kSelfCheckFail, epocanva::recovery::RunSelfCheckFailMode},
};

ModeFn FindMode(Mode mode) {
  for (const ModeEntry& entry : kModes) {
    if (entry.mode == mode) {
      return entry.fn;
    }
  }
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  epocanva::recovery::Options opt;
  std::string error;
  if (!epocanva::recovery::ParseArgs(argc, argv, &opt, &error)) {
    std::printf(
        "usage: rime_english_recovery --expect-dll <the rime.dll this run must have "
        "loaded, compared after GetFullPathNameW normalization> --fixture-dir <path> "
        "--run-root <a leaf under rime-english-recovery-run> [--mode completion|"
        "expect-module-absent|self-check-fail]\n");
    epocanva::recovery::Fail(epocanva::recovery::kUsage, "arguments", error);
    return epocanva::recovery::kUsage;
  }
  ModeFn fn = FindMode(opt.mode);
  if (fn == nullptr) {
    // Unreachable through the command line: ParseArgs refuses any name that is not a
    // mode. Kept because a dispatch that silently returned success would be the exact
    // failure this program exists to detect.
    epocanva::recovery::Fail(epocanva::recovery::kUsage, "dispatch",
                             "the parsed mode has no implementation in this build");
    return epocanva::recovery::kUsage;
  }
  return static_cast<int>(epocanva::recovery::RunHarness(opt, fn));
}
