// EpoCanva P3.8 -- shared scaffolding for the coordinator verification program.
//
// TEST CODE ONLY. It reuses two existing test seams and adds no product decision:
//   * the P3.7 controlled TSF host (tests/windows/tsf_edit), compiled from its one set of
//     sources -- a real ITfThreadMgr/document/context/store with the documented fault
//     knobs, NOT a new host;
//   * the P3.5 engine-executor test support (tests/windows/engine_executor), compiled
//     from its one set of sources -- fixed-fixture staging, deployment preparation, the
//     real ToUnicodeEx key helpers and the controlled backend.
//
// The drive helper below owns bounded waiting on this STA: it never sleeps to guess a
// result, it waits on the two real completion facts (the executor result for a pending
// engine command; the P3.7 completion event for a pending edit) while dispatching the
// STA's messages, and it judges completion from the coordinator's state alone.
#ifndef EPOCANVA_TESTS_WINDOWS_TSF_COORDINATOR_COORDINATOR_TEST_SUPPORT_H_
#define EPOCANVA_TESTS_WINDOWS_TSF_COORDINATOR_COORDINATOR_TEST_SUPPORT_H_

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>

#include "coordinator.h"
#include "engine_executor.h"
#include "engine_executor/controlled_backend.h"
#include "engine_executor/test_support.h"
#include "key_event.h"
#include "test_host.h"

namespace epocanva {
namespace tsf {
namespace coordinator {
namespace test {

using epocanva::tsf::edit::test::TestDocument;
using epocanva::tsf::edit::test::TestDocumentOptions;
using epocanva::tsf::edit::test::TestHost;
using EngineExecutor = epocanva::windows::engine::EngineExecutor;
using Options = epocanva::tests::engine_executor::Options;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitCheckFailed = 5;
constexpr int kExitSelfCheckFailed = 14;

extern int g_total;
extern int g_failed;

void Check(bool condition, const std::string& what, const std::string& got = "");
void CheckOutcome(const CoordinatorSnapshot& snapshot, WorkOutcome want,
                  const std::string& what);
std::string DescribeSnapshot(const CoordinatorSnapshot& snapshot);  // text-free
// Compares document text without ever echoing it: a failure reports the two lengths and
// the first differing UTF-16 code-unit offset only.
void CheckDocText(const std::wstring& actual, const std::wstring& expected,
                  const std::string& what);

// Parses --rime-dll/--fixture-dir/--spelling-fixture-dir/--run-root the way the P3.5
// programs do. Returns false on a usage error.
bool ParseEngineArgs(int argc, char** argv, Options* options, std::string* case_name);

// --- controlled-backend harness (stage 1: the existing scheduling test seam) ------------

struct ControlledHarness {
  // Raw alias into the executor-owned backend; valid while `executor` lives.
  epocanva::tests::engine_executor::ControlledBackend* backend = nullptr;
  std::unique_ptr<EngineExecutor> executor;
  epocanva::windows::engine::ClientId client_a;
  epocanva::windows::engine::ClientId client_b;
};

// Creates a started executor over a controlled backend and two ready engine clients.
// No rime.dll is loaded. Returns false (with a printed reason) on failure.
bool CreateControlledHarness(ControlledHarness* harness);

// --- real-engine harness (stage 2: the pinned English engine) ---------------------------

struct RealHarness {
  std::unique_ptr<EngineExecutor> executor;
  epocanva::windows::engine::ClientId client_a;
  epocanva::windows::engine::ClientId client_b;
};

// Stages the fixed English fixtures, prepares the deployment, starts a real-engine
// executor and creates two ready clients against the epocanva_test_english schema.
bool CreateRealHarness(const Options& options, const char* leaf, RealHarness* harness);

// --- driving -----------------------------------------------------------------------------

struct DriveResult {
  bool completed = false;               // phase reached Idle before the deadline
  CoordinatorSnapshot final_state;
};

// Advances one work item to a terminal outcome without blocking the STA: Pump() is called
// here, bounded waits happen on the real completion facts only, and messages are
// dispatched by the P3.7 host while an edit is pending.
DriveResult DriveToCompletion(ContextCoordinator& coordinator, TestHost& host,
                              EngineExecutor& executor, DWORD timeout_ms);

// Convenience: submit one key and drive it. `expect_accept` names the check.
DriveResult TypeKeyAndDrive(ContextCoordinator& coordinator, TestHost& host,
                            EngineExecutor& executor,
                            const epocanva::windows::input::KeyEventSnapshot& event,
                            const epocanva::windows::input::KeyText& text,
                            const std::string& label, DWORD timeout_ms = 20000);

int Finish();

}  // namespace test
}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_TESTS_WINDOWS_TSF_COORDINATOR_COORDINATOR_TEST_SUPPORT_H_
