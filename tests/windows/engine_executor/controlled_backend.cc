// EpoCanva P3.5 -- controlled backend implementation. Test code only. See the header.
#include "controlled_backend.h"

#include <windows.h>

#include <stdexcept>
#include <utility>

#include "engine_update.h"
#include "key_decision.h"

namespace epocanva {
namespace tests {
namespace engine_executor {

namespace engine = epocanva::windows::engine;
namespace input = epocanva::windows::input;

ControlledBackend::ControlledBackend(Configuration config)
    : config_(std::move(config)), throw_on_dispatch_(config_.throw_on_dispatch_once) {}

void ControlledBackend::HoldDispatches() {
  std::lock_guard<std::mutex> lock(mutex_);
  block_dispatches_ = true;
  gate_open_ = false;
}

void ControlledBackend::ReleaseDispatches() {
  std::lock_guard<std::mutex> lock(mutex_);
  gate_open_ = true;
  gate_cv_.notify_all();
}

void ControlledBackend::HoldStart() {
  std::lock_guard<std::mutex> lock(mutex_);
  block_start_ = true;
  start_gate_open_ = false;
}

void ControlledBackend::ReleaseStart() {
  std::lock_guard<std::mutex> lock(mutex_);
  start_gate_open_ = true;
  start_gate_cv_.notify_all();
}

void ControlledBackend::SetDispatchHook(std::function<void()> hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  dispatch_hook_ = std::move(hook);
}

std::vector<ControlledBackend::Record> ControlledBackend::Records() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return records_;
}

std::uint64_t ControlledBackend::dispatch_calls() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return dispatch_calls_;
}

std::uint64_t ControlledBackend::evaluate_calls() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return evaluate_calls_;
}

std::uint64_t ControlledBackend::selection_calls() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return selection_calls_;
}

std::uint64_t ControlledBackend::create_calls() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return create_calls_;
}

std::uint64_t ControlledBackend::close_calls() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return close_calls_;
}

std::uint32_t ControlledBackend::runtime_thread_id() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return runtime_thread_id_;
}

std::uint32_t ControlledBackend::shutdown_thread_id() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return shutdown_thread_id_;
}

std::string ControlledBackend::Raw(std::uint64_t client) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = clients_.find(client);
  return found == clients_.end() ? std::string() : found->second.raw_ascii;
}

std::size_t ControlledBackend::client_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return clients_.size();
}

engine::BackendStartResult ControlledBackend::StartRuntime(
    const engine::RuntimeOptions&) {
  std::unique_lock<std::mutex> lock(mutex_);
  start_gate_cv_.wait(lock, [this] { return !block_start_ || start_gate_open_; });
  engine::BackendStartResult result;
  result.status = config_.start_status;
  result.thread_id = ::GetCurrentThreadId();
  runtime_thread_id_ = result.thread_id;
  if (result.status == engine::RuntimeStartStatus::kOk) {
    started_ = true;
    result.info.initialized = true;
    result.info.initialize_calls = 1;
  } else {
    result.diagnostic = "controlled backend refused to start";
    result.info.last_start_diagnostic = result.diagnostic;
  }
  return result;
}

engine::BackendCreateResult ControlledBackend::CreateClient(
    std::uint64_t client_id, const std::string& schema_id,
    const engine::ControllerOptions& options) {
  std::lock_guard<std::mutex> lock(mutex_);
  engine::BackendCreateResult result;
  if (config_.fail_create || schema_id.empty()) {
    result.status = engine::SessionCreateStatus::kSchemaUnavailable;
    result.diagnostic = "controlled backend refused the schema";
    return result;
  }
  FakeClient client;
  client.options = options;
  const auto inserted = clients_.emplace(client_id, std::move(client));
  if (!inserted.second) {
    result.status = engine::SessionCreateStatus::kCreateFailed;
    result.diagnostic = "duplicate client id";
    return result;
  }
  result.status = engine::SessionCreateStatus::kOk;
  result.client_ready = true;
  ++create_calls_;
  ++counts_.create_session_calls;
  return result;
}

engine::BackendEvaluateResult ControlledBackend::Evaluate(
    std::uint64_t client_id, const input::KeyEventSnapshot& event,
    const input::KeyText& text) {
  std::lock_guard<std::mutex> lock(mutex_);
  engine::BackendEvaluateResult result;
  const auto found = clients_.find(client_id);
  if (found == clients_.end()) {
    return result;
  }
  result.client_found = true;
  ++evaluate_calls_;
  RecordLocked(engine::CommandKind::kEvaluate, client_id, event, text);

  input::InputContext context;
  context.input_enabled = found->second.options.input_enabled;
  context.engine_ready = true;
  context.composition_active = !found->second.raw_ascii.empty();
  context.permission = found->second.options.permission;
  result.evaluation.status = engine::ControllerStatus::kOk;
  result.evaluation.decision = input::DecideKey(event, text, context);
  result.evaluation.can_dispatch = result.evaluation.decision.is_engine_candidate();
  result.evaluation.composition_active = context.composition_active;
  return result;
}

engine::BackendDispatchResult ControlledBackend::Dispatch(
    std::uint64_t client_id, const input::KeyEventSnapshot& event,
    const input::KeyText& text) {
  engine::BackendDispatchResult result;
  std::function<void()> hook;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    gate_cv_.wait(lock, [this] { return !block_dispatches_ || gate_open_; });
    const auto found = clients_.find(client_id);
    if (found == clients_.end()) {
      return result;
    }
    result.client_found = true;
    RecordLocked(engine::CommandKind::kDispatch, client_id, event, text);

    if (throw_on_dispatch_) {
      throw_on_dispatch_ = false;
      throw std::runtime_error("controlled dispatch exception");
    }

    ++dispatch_calls_;
    ++counts_.process_key_calls;
    if (text.status == input::KeyTextStatus::kText &&
        text.utf16_code_units.size() == 1 &&
        text.utf16_code_units[0] >= 0x20 &&
        text.utf16_code_units[0] < 0x7F) {
      found->second.raw_ascii.push_back(
          static_cast<char>(text.utf16_code_units[0]));
    }

    result.outcome.status = engine::DispatchStatus::kDispatchedHandled;
    result.outcome.engine_key_call_made = true;
    result.outcome.engine_reported_handled = true;
    result.outcome.state_status = engine::SessionStateStatus::kOk;
    result.outcome.commit_status = engine::SessionCommitStatus::kNoCommit;
    result.outcome.state = MakeState(found->second);
    hook = dispatch_hook_;
  }
  // The hook runs outside this backend's lock; the self-stop test calls back into the
  // executor, which must return immediately instead of waiting for the worker to join itself.
  if (hook) {
    hook();
  }
  return result;
}

engine::BackendCandidateResult ControlledBackend::AcceptCandidate(
    std::uint64_t client_id, std::size_t) {
  std::lock_guard<std::mutex> lock(mutex_);
  engine::BackendCandidateResult result;
  const auto found = clients_.find(client_id);
  if (found == clients_.end()) {
    return result;
  }
  result.client_found = true;
  ++selection_calls_;
  ++counts_.select_candidate_calls;
  RecordLocked(engine::CommandKind::kAcceptCandidate, client_id,
               input::KeyEventSnapshot{}, input::KeyText{});
  result.outcome.status = engine::CandidateStatus::kAccepted;
  result.outcome.engine_selection_call_made = true;
  result.outcome.engine_reported_accepted = true;
  result.outcome.state_status = engine::SessionStateStatus::kOk;
  result.outcome.commit_status = engine::SessionCommitStatus::kNoCommit;
  result.outcome.state = MakeState(found->second);
  return result;
}

engine::BackendRefreshResult ControlledBackend::Refresh(std::uint64_t client_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  engine::BackendRefreshResult result;
  const auto found = clients_.find(client_id);
  if (found == clients_.end()) {
    return result;
  }
  result.client_found = true;
  result.status = engine::ControllerStatus::kOk;
  result.state = MakeState(found->second);
  return result;
}

engine::BackendCloseResult ControlledBackend::CloseClient(std::uint64_t client_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  engine::BackendCloseResult result;
  const auto found = clients_.find(client_id);
  if (found == clients_.end()) {
    return result;
  }
  result.client_found = true;
  clients_.erase(found);
  ++close_calls_;
  ++counts_.destroy_session_calls;
  result.status = engine::SessionCloseStatus::kOk;
  return result;
}

engine::BackendCleanupReport ControlledBackend::CleanupAllClients() {
  std::lock_guard<std::mutex> lock(mutex_);
  engine::BackendCleanupReport report;
  report.shutdown_thread_id = ::GetCurrentThreadId();
  shutdown_thread_id_ = report.shutdown_thread_id;
  report.clients_closed = clients_.size();
  counts_.destroy_session_calls += clients_.size();
  clients_.clear();
  if (!started_) {
    // Mirrors RimeRuntime: Shutdown after a failed Start reports already-shutdown and does
    // not pretend finalize ran.
    report.shutdown_status = engine::RuntimeShutdownStatus::kAlreadyShutdown;
    return report;
  }
  report.shutdown_status = config_.shutdown_status;
  if (report.shutdown_status == engine::RuntimeShutdownStatus::kOk) {
    stopped_ = true;
  }
  return report;
}

engine::RuntimeInfo ControlledBackend::RuntimeInfoCopy() const {
  std::lock_guard<std::mutex> lock(mutex_);
  engine::RuntimeInfo info;
  info.librime_version = "controlled";
  info.initialized = started_ && !stopped_;
  info.api_complete = true;
  info.initialize_calls = started_ ? 1 : 0;
  info.finalize_calls = stopped_ ? 1 : 0;
  info.live_session_count = static_cast<int>(clients_.size());
  return info;
}

engine::ApiCallCounts ControlledBackend::ApiCountsCopy() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return counts_;
}

engine::EngineState ControlledBackend::MakeState(const FakeClient& client) const {
  engine::EngineState state;
  state.valid = true;
  state.schema_id = "controlled";
  state.schema_name = "controlled";
  state.is_composing = !client.raw_ascii.empty();
  state.raw_input.utf16_code_units =
      std::u16string(client.raw_ascii.begin(), client.raw_ascii.end());
  state.raw_input.utf8_byte_length = client.raw_ascii.size();
  state.preedit = state.raw_input;
  state.cursor_status = engine::CursorOffsetStatus::kMapped;
  state.cursor_utf16_code_unit_offset = client.raw_ascii.size();
  state.selection_status = engine::SelectionOffsetStatus::kMapped;
  state.selection_start_utf16_code_unit_offset = 0;
  state.selection_end_utf16_code_unit_offset = client.raw_ascii.size();
  return state;
}

void ControlledBackend::RecordLocked(
    engine::CommandKind kind, std::uint64_t client,
    const input::KeyEventSnapshot& event, const input::KeyText& text) {
  Record record;
  record.kind = kind;
  record.client = client;
  record.virtual_key = event.virtual_key;
  if (text.status == input::KeyTextStatus::kText &&
      text.utf16_code_units.size() == 1 &&
      text.utf16_code_units[0] >= 0x20 &&
      text.utf16_code_units[0] < 0x7F) {
    record.text_ascii.push_back(static_cast<char>(text.utf16_code_units[0]));
  }
  records_.push_back(std::move(record));
}

}  // namespace engine_executor
}  // namespace tests
}  // namespace epocanva
