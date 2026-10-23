// EpoCanva P3.4 -- shared helpers for the engine bridge verification programs.
//
// This is test scaffolding, not product code. It contains no engine decision: it stages fixed
// fixtures, creates controlled directories, prints check results, and compares paths. The
// runtime/session cases live in main.cc; the controller cases live in controller_tests.cc.
#ifndef EPOCANVA_TESTS_WINDOWS_ENGINE_BRIDGE_TEST_SUPPORT_H_
#define EPOCANVA_TESTS_WINDOWS_ENGINE_BRIDGE_TEST_SUPPORT_H_

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include "engine_update.h"
#include "rime_runtime.h"

namespace epocanva {
namespace tests {
namespace engine_bridge {

struct Options {
  std::string mode;
  std::filesystem::path rime_dll;
  std::filesystem::path baseline_dll;
  std::filesystem::path fixture_dir;
  std::filesystem::path spelling_fixture_dir;
  std::filesystem::path run_root;
};

extern int g_total;
extern int g_failed;

void Check(bool condition, const std::string& what,
           const std::string& got = std::string());
std::string AsciiOf(const std::u16string& text);
std::string AsciiOf(const std::wstring& text);
std::u16string U16(const std::string& ascii);
void CheckText(const std::u16string& actual, const std::string& expected,
               const std::string& what);
std::string CountText(std::uint64_t value);
std::string DescribeCounts(const epocanva::windows::engine::ApiCallCounts& counts);
std::wstring NormalizePath(const std::filesystem::path& path);
bool SamePath(const std::filesystem::path& left, const std::filesystem::path& right);
int CountFilesBeneath(const std::filesystem::path& dir);

bool PrepareRunRoot(const Options& options, const char* leaf,
                    std::filesystem::path* shared_dir,
                    std::filesystem::path* user_dir,
                    std::filesystem::path* log_dir,
                    std::filesystem::path* empty_user_dir,
                    std::filesystem::path* empty_log_dir);
bool StageFixtures(const Options& options, const std::filesystem::path& shared_dir);
epocanva::windows::engine::RuntimeOptions BaseOptions(
    const Options& options, const std::filesystem::path& shared_dir,
    const std::filesystem::path& user_dir, const std::filesystem::path& log_dir);
bool PrepareDeployment(const Options& options, const std::filesystem::path& shared_dir,
                       const std::filesystem::path& user_dir,
                       const std::filesystem::path& log_dir);
void CheckBalance(const epocanva::windows::engine::ApiCallCounts& counts,
                  const std::string& label);

// Controller cases (controller_tests.cc).
void RunDispatchMode(const Options& options);
void RunGuardsMode(const Options& options);
void RunSelfCheckFailDispatch();

}  // namespace engine_bridge
}  // namespace tests
}  // namespace epocanva

#endif  // EPOCANVA_TESTS_WINDOWS_ENGINE_BRIDGE_TEST_SUPPORT_H_
