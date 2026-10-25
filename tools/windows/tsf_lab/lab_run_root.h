// EpoCanva P3.13-R1 -- fresh, guarded run-directory preparation for the visible lab.
//
// The lab never deletes or reuses a run directory. Every start resolves a build-supplied
// trusted root, validates the requested parent component by component against the
// canonical root, creates one previously nonexistent child, and keeps every artefact
// there. Existing directories, including old engine-executor-run trees, are never removed.
//
// This is path validation for a test tool, not a race-proof sandbox. The other test
// programs keep their existing wipe-and-recreate helper; only the lab uses this one.
#ifndef EPOCANVA_TOOLS_WINDOWS_TSF_LAB_LAB_RUN_ROOT_H_
#define EPOCANVA_TOOLS_WINDOWS_TSF_LAB_LAB_RUN_ROOT_H_

#include <filesystem>
#include <string>

namespace epocanva {
namespace tsf {
namespace lab {

struct LabRunDirectories {
  std::filesystem::path run_dir;
  std::filesystem::path shared_dir;
  std::filesystem::path user_dir;
  std::filesystem::path log_dir;
};

// A process- and time-stamped name stem with no path separators. The first candidate is
// the stem itself; collisions use "<stem>-1", "<stem>-2", and so on.
std::string FreshLabRunStem();

// Validates |trusted_root| (supplied by the build, never by --run-root) and |run_parent|
// (the command-line parent), creates missing parent components below the root one level
// at a time, then creates one new run directory that did not exist before. |max_attempts|
// bounds the collision retries. On refusal nothing is deleted; a partially prepared
// parent may remain.
bool PrepareFreshLabRunRoot(const std::filesystem::path& trusted_root,
                            const std::filesystem::path& run_parent,
                            const std::string& run_stem,
                            unsigned max_attempts,
                            LabRunDirectories* directories,
                            std::string* error);

}  // namespace lab
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TOOLS_WINDOWS_TSF_LAB_LAB_RUN_ROOT_H_
