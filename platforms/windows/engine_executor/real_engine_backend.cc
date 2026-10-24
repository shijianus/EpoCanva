// EpoCanva P3.5 -- the real engine backend: RimeRuntime, sessions and controllers owned by the
// executor's worker thread.
//
// Every method here is expected on the worker. RimeRuntime itself enforces that rule: calling
// Start on the wrong thread, or using a session from another thread, is refused by the bridge.
// This adapter adds no thread of its own and performs no locking; EngineExecutor serializes it.
//
// The adapter keeps a client id -> (RimeSession, InputController) table. Controller is destroyed
// before its session, because the controller holds a raw RimeSession* and must not outlive it.
// All engine text is copied by P3.4; nothing here retains a pointer into rime.dll.
#include <windows.h>

#include <map>
#include <memory>
#include <utility>

#include "engine_backend.h"
#include "engine_executor_internal.h"
#include "input_controller.h"
#include "rime_runtime.h"
#include "rime_session.h"

namespace epocanva {
namespace windows {
namespace engine {
namespace {

struct RealClient {
  std::unique_ptr<RimeSession> session;
  std::unique_ptr<InputController> controller;
};

class RealEngineBackend final : public EngineBackend {
 public:
  BackendStartResult StartRuntime(const RuntimeOptions& options) override {
    BackendStartResult result;
    result.status = runtime_.Start(options);
    result.info = runtime_.Info();
    result.diagnostic = result.info.last_start_diagnostic;
    return result;
  }

  BackendCreateResult CreateClient(std::uint64_t client_id,
                                   const std::string& schema_id,
                                   const ControllerOptions& options) override {
    BackendCreateResult result;
    SessionCreateResult created = runtime_.CreateSession(schema_id);
    result.status = created.status;
    result.diagnostic = created.diagnostic;
    if (!created.ok()) {
      return result;
    }

    auto client = std::unique_ptr<RealClient>(new RealClient);
    client->session = std::move(created.session);
    client->controller = std::unique_ptr<InputController>(
        new InputController(runtime_, client->session.get(), options));

    const auto inserted = clients_.emplace(client_id, std::move(client));
    if (!inserted.second) {
      // Duplicate ids are impossible because the executor assigns them monotonically. If one
      // appears, the new client is destroyed here on the worker (its session closes) and the
      // command reports a failure instead of silently replacing an existing client.
      result.status = SessionCreateStatus::kCreateFailed;
      result.diagnostic = "duplicate client id";
      result.client_ready = false;
      return result;
    }
    result.client_ready = true;
    return result;
  }

  BackendEvaluateResult Evaluate(std::uint64_t client_id,
                                 const input::KeyEventSnapshot& event,
                                 const input::KeyText& text) override {
    BackendEvaluateResult result;
    RealClient* client = Find(client_id);
    if (client == nullptr) {
      return result;
    }
    result.client_found = true;
    result.evaluation = client->controller->Evaluate(event, text);
    return result;
  }

  BackendDispatchResult Dispatch(std::uint64_t client_id,
                                 const input::KeyEventSnapshot& event,
                                 const input::KeyText& text) override {
    BackendDispatchResult result;
    RealClient* client = Find(client_id);
    if (client == nullptr) {
      return result;
    }
    result.client_found = true;
    result.outcome = client->controller->Dispatch(event, text);
    return result;
  }

  BackendCandidateResult AcceptCandidate(std::uint64_t client_id,
                                         std::size_t candidate_index) override {
    BackendCandidateResult result;
    RealClient* client = Find(client_id);
    if (client == nullptr) {
      return result;
    }
    result.client_found = true;
    result.outcome = client->controller->AcceptCandidate(candidate_index);
    return result;
  }

  BackendRefreshResult Refresh(std::uint64_t client_id) override {
    BackendRefreshResult result;
    RealClient* client = Find(client_id);
    if (client == nullptr) {
      return result;
    }
    result.client_found = true;
    result.status = client->controller->RefreshState();
    if (result.status == ControllerStatus::kOk) {
      result.state = client->controller->cached_state();
    }
    return result;
  }

  BackendCloseResult CloseClient(std::uint64_t client_id) override {
    BackendCloseResult result;
    const auto found = clients_.find(client_id);
    if (found == clients_.end()) {
      return result;
    }
    result.client_found = true;
    RealClient& client = *found->second;
    client.controller.reset();
    if (client.session) {
      result.status = client.session->Close();
      result.diagnostic = "session close did not succeed";
      if (result.status == SessionCloseStatus::kOk ||
          result.status == SessionCloseStatus::kAlreadyClosed) {
        result.diagnostic.clear();
        clients_.erase(found);  // destroys the closed session on this thread
      }
    } else {
      result.status = SessionCloseStatus::kAlreadyClosed;
      clients_.erase(found);
    }
    return result;
  }

  BackendCleanupReport CleanupAllClients() override {
    BackendCleanupReport report;
    report.shutdown_thread_id = ::GetCurrentThreadId();
    for (auto& entry : clients_) {
      RealClient& client = *entry.second;
      client.controller.reset();
      if (client.session && client.session->IsOpen()) {
        const SessionCloseStatus status = client.session->Close();
        if (status == SessionCloseStatus::kOk ||
            status == SessionCloseStatus::kAlreadyClosed) {
          ++report.clients_closed;
        } else {
          ++report.close_failures;
          report.diagnostic = "a session could not be closed during cleanup";
        }
      } else {
        ++report.clients_closed;
      }
    }
    // Destroying the remaining Client objects here, on the worker, lets an already-failed
    // session destructor retry its close on the only thread allowed to do so.
    clients_.clear();
    report.live_sessions_after = runtime_.live_session_count();
    report.shutdown_status = runtime_.Shutdown();
    return report;
  }

  RuntimeInfo RuntimeInfoCopy() const override { return runtime_.Info(); }
  ApiCallCounts ApiCountsCopy() const override { return runtime_.ApiCounts(); }

 private:
  RealClient* Find(std::uint64_t client_id) {
    const auto found = clients_.find(client_id);
    return found == clients_.end() ? nullptr : found->second.get();
  }

  RimeRuntime runtime_;
  std::map<std::uint64_t, std::unique_ptr<RealClient>> clients_;
};

}  // namespace

std::unique_ptr<EngineBackend> MakeRealEngineBackend() {
  return std::unique_ptr<EngineBackend>(new RealEngineBackend);
}

}  // namespace engine
}  // namespace windows
}  // namespace epocanva
