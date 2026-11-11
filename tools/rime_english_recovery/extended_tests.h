// Actual Rime-engine English extension tests. Every menu/commit is read through
// RimeApi, never synthesized from LanguageSupport. Fixtures are test data only;
// assertion output contains fixed names, never input/candidate/commit strings.
#ifndef EPOCANVA_RECOVERY_EXTENDED_TESTS_H_
#define EPOCANVA_RECOVERY_EXTENDED_TESTS_H_

#include "recovery_test_support.h"

namespace epocanva {
namespace recovery {

ExitCode StageExtendedFixture(const Options& opt,
                              const std::filesystem::path& shared_dir,
                              std::filesystem::path* schema_source);
ExitCode RunExtendedDefaultsMode(Harness& h);
ExitCode RunExtendedMorphologyMode(Harness& h);
ExitCode RunExtendedAbbreviationMode(Harness& h);
ExitCode RunExtendedSpellingMode(Harness& h);
ExitCode RunExtendedFrequencyMode(Harness& h);
ExitCode RunExtendedProtectionMode(Harness& h);
ExitCode RunExtendedCommitMode(Harness& h);
ExitCode RunExtendedMalformedFrequencyMode(Harness& h);
ExitCode RunExtendedSelfCheckFailMode(Harness& h);

}  // namespace recovery
}  // namespace epocanva

#endif  // EPOCANVA_RECOVERY_EXTENDED_TESTS_H_
