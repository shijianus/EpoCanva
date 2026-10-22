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

#include "acceptance_tests.h"
#include "case_tests.h"
#include "completion_tests.h"
#include "lexicon_failure_tests.h"
#include "pilot_tests.h"
#include "recovery_test_support.h"
#include "space_tests.h"
#include "spelling_commit_tests.h"
#include "spelling_negative_tests.h"
#include "spelling_tests.h"
#include "spelling_toggle_tests.h"
#include "token_tests.h"

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
    {Mode::kCasePreservation, epocanva::recovery::RunCasePreservationMode},
    {Mode::kCaseSelfCheckFail, epocanva::recovery::RunCaseSelfCheckFailMode},
    {Mode::kTokenProtection, epocanva::recovery::RunTokenProtectionMode},
    {Mode::kTokenSelfCheckFail, epocanva::recovery::RunTokenSelfCheckFailMode},
    {Mode::kSpaceCommit, epocanva::recovery::RunSpaceCommitMode},
    {Mode::kSpaceSelfCheckFail, epocanva::recovery::RunSpaceSelfCheckFailMode},
    {Mode::kMissingLexicon, epocanva::recovery::RunMissingLexiconMode},
    {Mode::kBadLexiconDuplicate, epocanva::recovery::RunBadLexiconDuplicateMode},
    {Mode::kBadLexiconIllegal, epocanva::recovery::RunBadLexiconIllegalMode},
    {Mode::kBadLexiconIllegalForm, epocanva::recovery::RunBadLexiconIllegalFormMode},
    {Mode::kLexiconFailureSelfCheckFail,
     epocanva::recovery::RunLexiconFailureSelfCheckFailMode},
    {Mode::kPilotLexicon, epocanva::recovery::RunPilotLexiconMode},
    {Mode::kPilotAssertionSelfCheckFail,
     epocanva::recovery::RunPilotAssertionSelfCheckFailMode},
    {Mode::kPilotTruncatedRejected, epocanva::recovery::RunPilotTruncatedRejectedMode},
    {Mode::kPilotMissingRejected, epocanva::recovery::RunPilotMissingRejectedMode},
    {Mode::kSpelling, epocanva::recovery::RunSpellingMode},
    {Mode::kSpellingCommit, epocanva::recovery::RunSpellingCommitMode},
    {Mode::kSpellingLexiconUnavailable,
     epocanva::recovery::RunSpellingLexiconUnavailableMode},
    {Mode::kSpellingSuggestionRankingSelfCheckFail,
     epocanva::recovery::RunSpellingSuggestionRankingSelfCheckFailMode},
    {Mode::kSpellingLabelSelfCheckFail,
     epocanva::recovery::RunSpellingLabelSelfCheckFailMode},
    {Mode::kSpellingToggleAb, epocanva::recovery::RunSpellingToggleAbMode},
    {Mode::kSpellingToggleSelfCheckFail,
     epocanva::recovery::RunSpellingToggleSelfCheckFailMode},
    {Mode::kAcceptanceSemantics, epocanva::recovery::RunAcceptanceSemanticsMode},
    {Mode::kAcceptanceSelfCheckFail,
     epocanva::recovery::RunAcceptanceSelfCheckFailMode},
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
        "expect-module-absent|self-check-fail|case-preservation|case-self-check-fail|"
        "token-protection|token-self-check-fail|space-commit|space-self-check-fail|"
        "missing-lexicon|bad-lexicon-duplicate|bad-lexicon-illegal|"
        "bad-lexicon-illegal-form|lexicon-failure-self-check-fail|"
        "pilot-lexicon|pilot-assertion-self-check-fail|pilot-truncated-rejected|"
        "pilot-missing-rejected|spelling-suggestions|spelling-commit|"
        "spelling-lexicon-unavailable|spelling-ranking-self-check-fail|"
        "spelling-label-self-check-fail|spelling-toggle-ab|"
        "spelling-toggle-self-check-fail|acceptance-semantics|"
        "acceptance-self-check-fail]\n"
        "       the four pilot modes additionally require --pilot-lexicon <absolute path "
        "to the generated word list> and accept it and nothing else\n");
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
