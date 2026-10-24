// EpoCanva P3.5 -- shared scaffolding for the engine executor verification programs.
//
// TEST CODE ONLY. It stages the repository's fixed fixtures, prepares a controlled run root,
// prints check results, and translates fixed ASCII characters through the real P3.3 adapter.
// It contains no product decision and no live user text. The scheduler cases use a controlled
// backend and never load rime.dll; the real-engine cases use the executor's real backend. The
// two modes are separate command lines and separate CTest cases on purpose.
#ifndef EPOCANVA_TESTS_WINDOWS_ENGINE_EXECUTOR_TEST_SUPPORT_H_
#define EPOCANVA_TESTS_WINDOWS_ENGINE_EXECUTOR_TEST_SUPPORT_H_

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include "engine_executor.h"
#include "key_event.h"
#include "rime_runtime.h"

namespace epocanva {
namespace tests {
namespace engine_executor {

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

bool PrepareRunRoot(const Options& options, const char* leaf,
                    std::filesystem::path* shared_dir,
                    std::filesystem::path* user_dir,
                    std::filesystem::path* log_dir);
bool StageFixtures(const Options& options, const std::filesystem::path& shared_dir);
epocanva::windows::engine::RuntimeOptions BaseOptions(
    const Options& options, const std::filesystem::path& shared_dir,
    const std::filesystem::path& user_dir, const std::filesystem::path& log_dir);
bool PrepareDeployment(const Options& options, const std::filesystem::path& shared_dir,
                       const std::filesystem::path& user_dir,
                       const std::filesystem::path& log_dir);
void CheckBalance(const epocanva::windows::engine::ApiCallCounts& counts,
                  const std::string& label);

// --- fixed-fixture key helpers ---------------------------------------------------------
struct TextKey {
  bool ok = false;
  epocanva::windows::input::KeyEventSnapshot event;
  epocanva::windows::input::KeyText text;
  std::string diagnostic;
};

epocanva::windows::input::KeyEventSnapshot MakeKeyEvent(
    std::uint32_t virtual_key, bool shift, bool control, bool alt, bool windows,
    epocanva::windows::input::KeyTransition transition =
        epocanva::windows::input::KeyTransition::kKeyDown);
TextKey MakeTextKey(wchar_t character, HKL layout);
bool RequireTextKey(const TextKey& key, char expected, const std::string& label);

// --- executor command helpers ----------------------------------------------------------
epocanva::windows::engine::CommandResult TakeOrFail(
    epocanva::windows::engine::EngineExecutor& executor,
    epocanva::windows::engine::RequestId request, int timeout_ms,
    const std::string& label);

epocanva::windows::engine::RequestId SubmitCreateOrFail(
    epocanva::windows::engine::EngineExecutor& executor, const std::string& schema_id,
    const epocanva::windows::engine::ControllerOptions& options,
    epocanva::windows::engine::ClientId* client, const std::string& label);
epocanva::windows::engine::RequestId SubmitEvaluateOrFail(
    epocanva::windows::engine::EngineExecutor& executor,
    epocanva::windows::engine::ClientId client,
    const epocanva::windows::input::KeyEventSnapshot& event,
    const epocanva::windows::input::KeyText& text, const std::string& label);
epocanva::windows::engine::RequestId SubmitDispatchOrFail(
    epocanva::windows::engine::EngineExecutor& executor,
    epocanva::windows::engine::ClientId client,
    const epocanva::windows::input::KeyEventSnapshot& event,
    const epocanva::windows::input::KeyText& text, const std::string& label);
epocanva::windows::engine::RequestId SubmitAcceptOrFail(
    epocanva::windows::engine::EngineExecutor& executor,
    epocanva::windows::engine::ClientId client, std::size_t candidate_index,
    const std::string& label);
epocanva::windows::engine::RequestId SubmitRefreshOrFail(
    epocanva::windows::engine::EngineExecutor& executor,
    epocanva::windows::engine::ClientId client, const std::string& label);

// --- modes -----------------------------------------------------------------------------
void RunSchedulerMode();
void RunEngineMode(const Options& options);
void RunRaceMode(const Options& options);
void RunStartupFailureMode(const Options& options);
void RunSelfCheckFail();

}  // namespace engine_executor
}  // namespace tests
}  // namespace epocanva

#endif  // EPOCANVA_TESTS_WINDOWS_ENGINE_EXECUTOR_TEST_SUPPORT_H_
