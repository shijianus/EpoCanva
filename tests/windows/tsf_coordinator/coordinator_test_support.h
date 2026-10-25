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

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "coordinator.h"
#include "engine_executor.h"
#include "engine_executor/controlled_backend.h"
#include "engine_executor/test_support.h"
#include "key_event.h"
#include "permission_query.h"
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

// P3.13 TEST SEAM, EXPLICITLY NOT A SYSTEM NOTIFICATION. The coordinator refuses ordinary
// input without a TargetRevalidator. Cases that use the old direct Bind() unit seam attach
// this implementation so their state-machine expectations stay focused on binding/gates;
// the real observer-backed cases (observer_binding_tests and focus_guard_tests) attach the
// production ContextObserver instead. `SetStatus` is a deliberate test input, never an
// observation of TSF; the write guard shares a small state block so a late callback cannot
// dereference this object after the test case has destroyed it.
class TestTargetRevalidator final : public context::TargetRevalidator {
 public:
  struct WriteState {
    std::atomic<bool> allowed{true};
    std::atomic<std::uint64_t> checks{0};
  };

  TestTargetRevalidator() : write_state_(std::make_shared<WriteState>()) {
    // The controlled seam starts permissive; cases that exercise a refusal call SetStatus.
    result_.status = context::TargetRevalidationStatus::kOk;
  }

  void SetStatus(context::TargetRevalidationStatus status, HRESULT query_hr = S_OK,
                 context::TargetFailureStage stage =
                     context::TargetFailureStage::kNone) {
    result_.status = status;
    result_.query_hr = query_hr;
    result_.failure_stage = stage;
    result_.evidence = context::TargetEvidence{};
  }
  void SetResult(const context::TargetRevalidation& result) { result_ = result; }
  void SetWriteAllowed(bool allowed) { write_state_->allowed.store(allowed); }
  std::uint64_t calls() const { return calls_; }
  std::uint64_t write_checks() const { return write_state_->checks.load(); }

  context::TargetRevalidation Revalidate(ITfContext* context,
                                         std::uint64_t context_id,
                                         std::uint64_t generation) override {
    (void)context;
    (void)context_id;
    (void)generation;
    ++calls_;
    return result_;
  }

  std::shared_ptr<context::TargetWriteGuard> CreateWriteGuard() override {
    const std::shared_ptr<WriteState> state = write_state_;
    return context::MakeTargetWriteGuard(
        [state](ITfContext*, std::uint64_t, std::uint64_t) {
          state->checks.fetch_add(1);
          return state->allowed.load();
        });
  }

 private:
  context::TargetRevalidation result_{};
  std::shared_ptr<WriteState> write_state_;
  std::uint64_t calls_ = 0;
};

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitCheckFailed = 5;
constexpr int kExitSelfCheckFailed = 14;

// THE MANUAL PERMISSION GATE, AS A TEST-OWNED PERMISSION SOURCE. The product coordinator
// has no permission setter of its own; real verdicts always come from the real source
// (CreateRealPermissionSource over the P3.7 host's metadata). This class exists so the
// P3.8 unit cases can drive the coordinator's permission-dependent state machine
// deterministically without a document, and it is compiled only into test targets. The
// verdicts it hands out are explicit test inputs, never observations.
class ManualPermissionSource final
    : public epocanva::tsf::context::PermissionSource {
 public:
  ManualPermissionSource() {
    completion_event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }
  ~ManualPermissionSource() override {
    if (completion_event_ != nullptr) {
      ::CloseHandle(completion_event_);
      completion_event_ = nullptr;
    }
  }

  void Set(epocanva::tsf::context::PermissionState state) { state_ = state; }
  // When set, Begin() parks the query in kPending until Release() is called, which is
  // the deterministic stand-in for a read session that must be pumped.
  void SetHoldPending(bool hold) { hold_pending_ = hold; }
  void Release() {
    complete_ = true;
    if (completion_event_ != nullptr) {
      ::SetEvent(completion_event_);
    }
  }

  bool Begin(ITfContext* context, std::uint64_t context_id,
             std::uint64_t generation) override {
    if (context == nullptr || context_id == 0 || generation == 0) {
      return false;
    }
    if (begun_ && !complete_) {
      return false;
    }
    context_ = context;
    context_id_ = context_id;
    generation_ = generation;
    begun_ = true;
    complete_ = false;
    if (completion_event_ != nullptr) {
      ::ResetEvent(completion_event_);
    }
    ++diagnostics_.queries_begun;
    if (!hold_pending_) {
      complete_ = true;
      ++diagnostics_.queries_completed;
      if (completion_event_ != nullptr) {
        ::SetEvent(completion_event_);
      }
    }
    return true;
  }

  epocanva::tsf::context::PermissionQueryStatus Status() const override {
    using Q = epocanva::tsf::context::PermissionQueryStatus;
    if (!begun_) {
      return Q::kIdle;
    }
    return complete_ ? Q::kReady : Q::kPending;
  }

  epocanva::tsf::context::PermissionQueryResult Result() const override {
    epocanva::tsf::context::PermissionQueryResult result;
    result.context_id = context_id_;
    result.generation = generation_;
    if (begun_ && complete_) {
      result.complete = true;
      result.decision.state = state_;
    }
    return result;
  }

  HANDLE CompletionEvent() const override {
    if (begun_ && !complete_) {
      return completion_event_;
    }
    return nullptr;
  }

  void Abandon() override {
    if (!begun_ || complete_) {
      return;
    }
    ++diagnostics_.queries_abandoned;
    begun_ = false;
    complete_ = false;
    context_ = nullptr;
    if (completion_event_ != nullptr) {
      ::SetEvent(completion_event_);
    }
  }

  epocanva::tsf::context::PermissionQueryDiagnostics Diagnostics() const override {
    return diagnostics_;
  }

  std::uint64_t began() const { return diagnostics_.queries_begun; }
  std::uint64_t completed() const { return diagnostics_.queries_completed; }

 private:
  epocanva::tsf::context::PermissionState state_ =
      epocanva::tsf::context::PermissionState::kUnknown;
  bool hold_pending_ = false;
  bool complete_ = false;
  bool begun_ = false;
  HANDLE completion_event_ = nullptr;
  ITfContext* context_ = nullptr;  // not AddRef'd: the coordinator owns the binding
  std::uint64_t context_id_ = 0;
  std::uint64_t generation_ = 0;
  epocanva::tsf::context::PermissionQueryDiagnostics diagnostics_;
};

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
};

// Stages the fixed English fixtures, prepares the deployment, starts a real-engine
// executor and creates one ready client against the epocanva_test_english schema.
bool CreateRealHarness(const Options& options, const char* leaf, RealHarness* harness);

// Creates one more engine client on demand (same schema). A second session starts with an
// empty composition, which is what the re-binding cases need.
epocanva::windows::engine::ClientId AddRealClient(RealHarness* harness);

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
