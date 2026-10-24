// EpoCanva P3.5 -- controlled backend for the scheduling-layer tests.
//
// This is NOT the engine. It does not load rime.dll, does not implement the English policy and
// does not claim that a key reached an input method. It exists so the executor's state machine,
// FIFO, capacity accounting, close admission rule, timeout and stop-drain behavior can be
// exercised deterministically, including a command deliberately held inside the backend. The
// real-engine cases live in real_engine_tests.cc and use the executor's real backend; the two
// suites are separate CTest cases and print different labels.
#ifndef EPOCANVA_TESTS_WINDOWS_ENGINE_EXECUTOR_CONTROLLED_BACKEND_H_
#define EPOCANVA_TESTS_WINDOWS_ENGINE_EXECUTOR_CONTROLLED_BACKEND_H_

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "engine_backend.h"
#include "engine_executor.h"

namespace epocanva {
namespace tests {
namespace engine_executor {

class ControlledBackend final : public epocanva::windows::engine::EngineBackend {
 public:
  struct Configuration {
    epocanva::windows::engine::RuntimeStartStatus start_status =
        epocanva::windows::engine::RuntimeStartStatus::kOk;
    epocanva::windows::engine::RuntimeShutdownStatus shutdown_status =
        epocanva::windows::engine::RuntimeShutdownStatus::kOk;
    bool fail_create = false;
    bool throw_on_dispatch_once = false;
  };

  struct Record {
    epocanva::windows::engine::CommandKind kind =
        epocanva::windows::engine::CommandKind::kCreateClient;
    std::uint64_t client = 0;
    std::uint32_t virtual_key = 0;
    std::string text_ascii;
  };

  explicit ControlledBackend(Configuration config = Configuration());

  // Test controls. HoldDispatches only affects dispatches that have not yet passed the gate.
  void HoldDispatches();
  void ReleaseDispatches();
  void HoldStart();
  void ReleaseStart();
  void SetDispatchHook(std::function<void()> hook);

  std::vector<Record> Records() const;
  std::uint64_t dispatch_calls() const;
  std::uint64_t evaluate_calls() const;
  std::uint64_t selection_calls() const;
  std::uint64_t create_calls() const;
  std::uint64_t close_calls() const;
  std::uint32_t runtime_thread_id() const;
  std::uint32_t shutdown_thread_id() const;
  std::string Raw(std::uint64_t client) const;
  std::size_t client_count() const;

  // EngineBackend.
  epocanva::windows::engine::BackendStartResult StartRuntime(
      const epocanva::windows::engine::RuntimeOptions& options) override;
  epocanva::windows::engine::BackendCreateResult CreateClient(
      std::uint64_t client_id, const std::string& schema_id,
      const epocanva::windows::engine::ControllerOptions& options) override;
  epocanva::windows::engine::BackendEvaluateResult Evaluate(
      std::uint64_t client_id,
      const epocanva::windows::input::KeyEventSnapshot& event,
      const epocanva::windows::input::KeyText& text) override;
  epocanva::windows::engine::BackendDispatchResult Dispatch(
      std::uint64_t client_id,
      const epocanva::windows::input::KeyEventSnapshot& event,
      const epocanva::windows::input::KeyText& text) override;
  epocanva::windows::engine::BackendCandidateResult AcceptCandidate(
      std::uint64_t client_id, std::size_t candidate_index) override;
  epocanva::windows::engine::BackendRefreshResult Refresh(
      std::uint64_t client_id) override;
  epocanva::windows::engine::BackendCloseResult CloseClient(
      std::uint64_t client_id) override;
  epocanva::windows::engine::BackendCleanupReport CleanupAllClients() override;
  epocanva::windows::engine::RuntimeInfo RuntimeInfoCopy() const override;
  epocanva::windows::engine::ApiCallCounts ApiCountsCopy() const override;

 private:
  struct FakeClient {
    epocanva::windows::engine::ControllerOptions options;
    std::string raw_ascii;
  };

  epocanva::windows::engine::EngineState MakeState(
      const FakeClient& client) const;
  void RecordLocked(epocanva::windows::engine::CommandKind kind, std::uint64_t client,
                    const epocanva::windows::input::KeyEventSnapshot& event,
                    const epocanva::windows::input::KeyText& text);

  Configuration config_;
  mutable std::mutex mutex_;
  std::condition_variable gate_cv_;
  std::condition_variable start_gate_cv_;
  bool block_dispatches_ = false;
  bool gate_open_ = true;
  bool block_start_ = false;
  bool start_gate_open_ = true;
  std::function<void()> dispatch_hook_;
  std::map<std::uint64_t, FakeClient> clients_;
  std::vector<Record> records_;
  epocanva::windows::engine::ApiCallCounts counts_;
  std::uint64_t dispatch_calls_ = 0;
  std::uint64_t evaluate_calls_ = 0;
  std::uint64_t selection_calls_ = 0;
  std::uint64_t create_calls_ = 0;
  std::uint64_t close_calls_ = 0;
  std::uint32_t runtime_thread_id_ = 0;
  std::uint32_t shutdown_thread_id_ = 0;
  bool started_ = false;
  bool stopped_ = false;
  bool throw_on_dispatch_ = false;
};

}  // namespace engine_executor
}  // namespace tests
}  // namespace epocanva

#endif  // EPOCANVA_TESTS_WINDOWS_ENGINE_EXECUTOR_CONTROLLED_BACKEND_H_
