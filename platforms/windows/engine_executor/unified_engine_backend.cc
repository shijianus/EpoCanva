// Unified backend C ABI adapter. All handles and API calls stay on executor's worker.
#include "engine_executor_internal.h"

#include <algorithm>
#include <limits>
#include <map>
#include <utility>

namespace epocanva::windows::engine {
namespace {
template<class T> T Header(std::uint32_t version = EPOCANVA_BACKEND_ABI_VERSION) {
  T result{}; result.struct_size_bytes = sizeof(T); result.abi_version = version; return result;
}
epocanva_utf8_view View(const std::string& text) { return {text.data(), text.size()}; }
bool Slice(const std::string& bytes, epocanva_utf8_span span, std::string* out) {
  if (span.utf8_byte_offset > bytes.size() ||
      span.utf8_byte_count > bytes.size() - span.utf8_byte_offset) return false;
  *out = bytes.substr(static_cast<std::size_t>(span.utf8_byte_offset),
                      static_cast<std::size_t>(span.utf8_byte_count));
  return CaptureEngineText(*out).ok();
}
struct ProfileViews {
  std::string abbreviation_path, model_path, conversion_path, auxiliary_path;
  epocanva_backend_profile_resource value{};
  void Set(const BackendResourceConfig& config) {
    abbreviation_path = config.profile.abbreviation_path.u8string();
    model_path = config.model_path.u8string();
    conversion_path = config.conversion_path.u8string();
    auxiliary_path = config.auxiliary_path.u8string();
    value.profile = Header<epocanva_backend_profile>(EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
    auto& p = value.profile;
    p.profile_id = config.profile.profile_id; p.language = config.profile.language;
    p.input_mode = config.profile.input_mode; p.output_mode = config.profile.output_mode;
    p.keyboard_layout = config.profile.keyboard_layout;
    p.variant_id = View(config.profile.variant_id);
    p.abbreviation_path = View(abbreviation_path);
    p.abbreviation_sha256 = View(config.profile.abbreviation_sha256);
    value.resource = Header<epocanva_backend_resource>();
    auto& r = value.resource;
    r.language = p.language; r.model_path = View(model_path); r.model_sha256 = View(config.model_sha256);
    r.conversion_path = View(conversion_path); r.conversion_sha256 = View(config.conversion_sha256);
    r.auxiliary_path = View(auxiliary_path); r.auxiliary_sha256 = View(config.auxiliary_sha256);
  }
};
struct UnifiedClient {
  epocanva_backend_session_handle session = 0;
  std::uint32_t profile_id = 0;
  BackendAccess access;
  bool faulted = false;
};

class UnifiedEngineBackend final : public EngineBackend {
 public:
  explicit UnifiedEngineBackend(UnifiedBackendConfig config) : config_(std::move(config)) {}
  BackendStartResult StartRuntime(const RuntimeOptions& runtime) override {
    BackendStartResult result;
    if (!config_.enabled || config_.profiles.empty() || config_.profiles.size() > 32) return result;
    std::vector<ProfileViews> views(config_.profiles.size());
    std::vector<epocanva_backend_profile_resource> resources;
    resources.reserve(views.size());
    for (std::size_t ordinal = 0; ordinal < views.size(); ++ordinal) {
      views[ordinal].Set(config_.profiles[ordinal]); resources.push_back(views[ordinal].value);
    }
    const auto dll = runtime.rime_dll_path.u8string(), shared = runtime.shared_data_dir.u8string();
    const auto user = runtime.user_data_dir.u8string(), logs = runtime.log_dir.u8string();
    const std::string version = "1.17.0";
    auto rime = Header<epocanva_engine_options>();
    rime.rime_dll_path = View(dll); rime.shared_data_dir = View(shared);
    rime.user_data_dir = View(user); rime.log_dir = View(logs); rime.expected_rime_version = View(version);
    std::vector<std::string> schema_paths;
    std::vector<epocanva_utf8_view> schemas;
    if (runtime.deployment) {
      for (const auto& path : runtime.deployment->schema_files) schema_paths.push_back(path.u8string());
      for (const auto& path : schema_paths) schemas.push_back(View(path));
      rime.deployment_schema_paths = schemas.data();
      rime.deployment_schema_count = static_cast<std::uint32_t>(schemas.size());
    }
    auto options = Header<epocanva_backend_profile_options>(EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
    options.profiles = resources.data(); options.profile_count = static_cast<std::uint32_t>(resources.size());
    options.allow_test_fixture = config_.allow_test_fixture ? 1u : 0u;
    options.rime_options = config_.use_rime ? &rime : nullptr;
    // No learning root: a revocable queue value is not the ABI's current field lock.
    const auto status = epocanva_backend_open_profiles(&options, &backend_);
    result.status = status == EPOCANVA_OK ? RuntimeStartStatus::kOk : RuntimeStartStatus::kInitializeFailed;
    result.diagnostic = status == EPOCANVA_OK ? "" : "unified backend open refused: " + std::to_string(status);
    result.info.last_start_diagnostic = result.diagnostic;
    return result;
  }
  ProfileClientOutcome CreateProfileClient(std::uint64_t id, std::uint32_t profile_id,
                                            const BackendAccess& access) override {
    ProfileClientOutcome result; result.client = ClientId{id}; result.profile_id = profile_id;
    if (!backend_ || !access.allowed()) { result.backend_status = EPOCANVA_PRIVACY_DENIED; return result; }
    const auto* profile = FindProfile(profile_id);
    if (!profile) { result.backend_status = EPOCANVA_BACKEND_INVALID_PROFILE; return result; }
    // This records a binding only. Actual session open waits for the first permission-gated
    // work item, rather than asserting Normal during observer binding creation.
    UnifiedClient client; client.profile_id = profile_id; client.access = access;
    result.backend_status = clients_.emplace(id, std::move(client)).second
        ? EPOCANVA_OK : EPOCANVA_INVALID_ARGUMENT;
    return result;
  }
  BackendWorkOutcome RunBackendWork(std::uint64_t id, const BackendWork& work) override {
    BackendWorkOutcome result;
    auto found = clients_.find(id);
    if (found == clients_.end()) { result.backend_status = EPOCANVA_INVALID_HANDLE; return result; }
    auto& client = found->second;
    if (client.access.context_target != work.access.context_target ||
        client.access.generation != work.access.generation) {
      result.backend_status = EPOCANVA_CONTEXT_MISMATCH; return result;
    }
    const bool allowed = client.access.allowed() && work.access.allowed();
    auto request = Header<epocanva_backend_request>();
    request.context = {work.access.context_target, work.access.generation,
        static_cast<std::uint32_t>(allowed ? EPOCANVA_PRIVACY_NORMAL : EPOCANVA_PRIVACY_UNKNOWN), 0};
    request.learning_enabled = 0;
    if (!allowed && work.operation != BackendOperation::kAcknowledgeCommit) {
      if (client.session) (void)epocanva_backend_cancel(client.session, &request);
      result.backend_status = EPOCANVA_PRIVACY_DENIED; return result;
    }
    if (client.faulted && work.operation != BackendOperation::kAcknowledgeCommit) {
      result.backend_status = EPOCANVA_SESSION_FAULTED; return result;
    }
    if (!client.session) {
      if (!allowed) { result.backend_status = EPOCANVA_PRIVACY_DENIED; return result; }
      auto options = Header<epocanva_backend_session_options>(); options.context = request.context;
      const auto* profile = FindProfile(client.profile_id);
      if (!profile) { result.backend_status = EPOCANVA_BACKEND_INVALID_PROFILE; return result; }
      options.language = profile->profile.language; options.input_mode = profile->profile.input_mode;
      ProfileViews views; views.Set(*profile);
      const auto status = epocanva_backend_profile_session_open(backend_, &views.value.profile, &options, &client.session);
      if (status != EPOCANVA_OK) { result.backend_status = status; return result; }
    }
    epocanva_status status = EPOCANVA_OK;
    switch (work.operation) {
      case BackendOperation::kPushUtf8:
        status = epocanva_backend_push_utf8(client.session, &request, View(work.text_utf8)); break;
      case BackendOperation::kBackspace:
        status = epocanva_backend_backspace(client.session, &request); break;
      case BackendOperation::kClearInput:
        status = epocanva_backend_set_input(client.session, &request, {nullptr, 0}); break;
      case BackendOperation::kSetContext:
        status = epocanva_backend_set_context(client.session, &request, View(work.text_utf8)); break;
      case BackendOperation::kConfirmText: {
        auto confirmation = Header<epocanva_backend_text_confirmation>(EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
        confirmation.request = request; confirmation.event_token = work.event_token;
        confirmation.text = View(work.text_utf8);
        auto ack = Header<epocanva_backend_ack_result>();
        status = epocanva_backend_confirm_text(client.session, &confirmation, &ack);
        result.acknowledgement = {status, ack.consumed, ack.learned, ack.context_advanced, ack.persistence_status};
        break;
      }
      case BackendOperation::kObserveHostText: {
        // Both strings were read from a bounded, permission-checked host range. Confirmation
        // is transient; it does not infer provenance for learning from displayed text.
        auto confirmation = Header<epocanva_backend_text_confirmation>(EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
        confirmation.request = request; confirmation.event_token = work.event_token;
        confirmation.text = View(work.preceding_utf8);
        auto ack = Header<epocanva_backend_ack_result>();
        status = epocanva_backend_confirm_text(client.session, &confirmation, &ack);
        result.acknowledgement = {status, ack.consumed, ack.learned, ack.context_advanced, ack.persistence_status};
        if (status == EPOCANVA_OK) status = epocanva_backend_set_context(client.session, &request, View(work.preceding_utf8));
        if (status == EPOCANVA_OK) status = epocanva_backend_set_input(client.session, &request, {nullptr, 0});
        if (status == EPOCANVA_OK && !work.text_utf8.empty())
          status = epocanva_backend_push_utf8(client.session, &request, View(work.text_utf8));
        break;
      }
      case BackendOperation::kSelect:
        status = epocanva_backend_select(client.session, &request, work.snapshot_token, work.candidate_token);
        if (status == EPOCANVA_OK) status = ReadCommit(client, request, &result.commit);
        result.backend_status = status;
        if (status != EPOCANVA_OK) client.faulted = true;
        return result;
      case BackendOperation::kAcknowledgeCommit: {
        auto ack = Header<epocanva_backend_ack_result>();
        status = epocanva_backend_ack_commit(client.session, &request, work.commit_token,
                                            work.host_applied && allowed ? 1u : 0u, &ack);
        result.acknowledgement = {status, ack.consumed, ack.learned, ack.context_advanced, ack.persistence_status};
        result.backend_status = status;
        return result;
      }
      case BackendOperation::kQuery: break;
    }
    if (status == EPOCANVA_OK && work.operation != BackendOperation::kClearInput) {
      auto query = Header<epocanva_backend_query>(); query.context = request.context;
      query.query_kind = work.query_kind; query.max_candidates = config_.max_candidates;
      query.max_continuation_words = 1; query.search_budget = 100000;
      status = epocanva_backend_query_candidates(client.session, &query);
      if (status == EPOCANVA_BACKEND_UNAVAILABLE &&
          (work.query_kind == EPOCANVA_BACKEND_NEXT_WORD ||
           work.query_kind == EPOCANVA_BACKEND_NEXT_UNIT)) {
        // A profile may explicitly lack a promised next-word/script mode. This is a
        // capability result, not a corrupt session; publish an empty memory-only menu.
        result.snapshot = BackendSnapshotOutcome{};
        result.snapshot.backend_status = status;
        result.backend_status = EPOCANVA_OK;
        return result;
      }
    }
    if (status == EPOCANVA_OK) status = ReadSnapshot(client, request, &result.snapshot);
    result.backend_status = status;
    if (status != EPOCANVA_OK) client.faulted = true;
    return result;
  }
  BackendCreateResult CreateClient(std::uint64_t, const std::string&, const ControllerOptions&) override {
    BackendCreateResult result; result.status = SessionCreateStatus::kSchemaUnavailable; return result;
  }
  BackendEvaluateResult Evaluate(std::uint64_t, const input::KeyEventSnapshot&, const input::KeyText&) override { return {}; }
  BackendDispatchResult Dispatch(std::uint64_t, const input::KeyEventSnapshot&, const input::KeyText&) override { return {}; }
  BackendCandidateResult AcceptCandidate(std::uint64_t, std::size_t) override { return {}; }
  BackendRefreshResult Refresh(std::uint64_t) override { return {}; }
  BackendCloseResult CloseClient(std::uint64_t id) override {
    BackendCloseResult result; auto found = clients_.find(id); if (found == clients_.end()) return result;
    result.client_found = true;
    const auto status = found->second.session ? epocanva_backend_session_close(found->second.session) : EPOCANVA_OK;
    if (status == EPOCANVA_OK) { clients_.erase(found); result.status = SessionCloseStatus::kOk; }
    else { result.status = SessionCloseStatus::kDestroyFailed; result.diagnostic = "backend session close failed"; }
    return result;
  }
  BackendCleanupReport CleanupAllClients() override {
    BackendCleanupReport result; result.shutdown_thread_id = ::GetCurrentThreadId();
    for (auto it = clients_.begin(); it != clients_.end();) {
      const auto id = it->first; ++it;
      if (CloseClient(id).status == SessionCloseStatus::kOk) ++result.clients_closed;
      else ++result.close_failures;
    }
    result.live_sessions_after = static_cast<int>(clients_.size());
    if (!backend_) { result.shutdown_status = RuntimeShutdownStatus::kAlreadyShutdown; return result; }
    const auto status = epocanva_backend_close(backend_);
    result.shutdown_status = status == EPOCANVA_OK ? RuntimeShutdownStatus::kOk : RuntimeShutdownStatus::kFinalizeFailed;
    if (status == EPOCANVA_OK) backend_ = 0;
    return result;
  }
  // The backend API exposes no Rime diagnostic counters. Leave them unclaimed, not fabricated.
  RuntimeInfo RuntimeInfoCopy() const override { return {}; }
  ApiCallCounts ApiCountsCopy() const override { return {}; }
 private:
  const BackendResourceConfig* FindProfile(std::uint32_t id) const {
    for (const auto& resource : config_.profiles) if (resource.profile.profile_id == id) return &resource;
    return nullptr;
  }
  epocanva_status ReadSnapshot(UnifiedClient& client, const epocanva_backend_request& request,
                                BackendSnapshotOutcome* out) {
    auto snapshot = Header<epocanva_backend_profile_snapshot>(EPOCANVA_BACKEND_PROFILE_ABI_VERSION);
    snapshot.snapshot = Header<epocanva_backend_snapshot>();
    auto status = epocanva_backend_read_profile_snapshot(client.session, &request, &snapshot, nullptr, 0);
    if (status != EPOCANVA_OK && status != EPOCANVA_BUFFER_TOO_SMALL) return status;
    if (snapshot.snapshot.required_utf8_bytes > 8192 || snapshot.snapshot.candidate_count > 64) return EPOCANVA_OUT_OF_RANGE;
    std::string bytes(static_cast<std::size_t>(snapshot.snapshot.required_utf8_bytes), '\0');
    status = epocanva_backend_read_profile_snapshot(client.session, &request, &snapshot, bytes.data(), bytes.size());
    if (status != EPOCANVA_OK) return status;
    const auto& base = snapshot.snapshot;
    if (base.context_target != request.context.context_target || base.generation != request.context.generation ||
        !Slice(bytes, base.raw_input, &out->raw_utf8) || !Slice(bytes, base.preedit, &out->preedit_utf8)) return EPOCANVA_BACKEND_ERROR;
    out->snapshot_token = base.snapshot_token; out->profile_id = snapshot.profile_id;
    out->output_mode = snapshot.output_mode; out->keyboard_layout = snapshot.keyboard_layout;
    out->unit_kind = snapshot.unit_kind; out->search_complete = base.search_complete;
    out->language = base.language; out->input_mode = base.input_mode;
    for (std::uint32_t ordinal = 0; ordinal < base.candidate_count; ++ordinal) {
      auto candidate = Header<epocanva_backend_candidate>();
      status = epocanva_backend_read_candidate(client.session, &request, base.snapshot_token, ordinal, &candidate, nullptr, 0);
      if (status != EPOCANVA_BUFFER_TOO_SMALL || candidate.required_utf8_bytes > 4096) return EPOCANVA_BACKEND_ERROR;
      std::string payload(static_cast<std::size_t>(candidate.required_utf8_bytes), '\0'), text;
      status = epocanva_backend_read_candidate(client.session, &request, base.snapshot_token, ordinal, &candidate, payload.data(), payload.size());
      if (status != EPOCANVA_OK || !Slice(payload, candidate.text, &text)) return EPOCANVA_BACKEND_ERROR;
      if ((!config_.spelling_enabled && candidate.kind == EPOCANVA_BACKEND_SPELLING) ||
          (!config_.morphology_enabled && candidate.kind == EPOCANVA_BACKEND_MORPHOLOGY) ||
          (!config_.abbreviation_enabled && candidate.kind == EPOCANVA_BACKEND_EXPANSION) ||
          (!config_.completion_enabled && candidate.kind == EPOCANVA_BACKEND_WORD)) continue;
      out->candidate_text_utf8.push_back(std::move(text)); out->candidate_tokens.push_back(candidate.candidate_token);
      out->candidate_kind.push_back(candidate.kind); out->candidate_lexeme_counts.push_back(candidate.lexeme_count);
      out->candidate_input_begin_utf8_byte.push_back(candidate.input_begin_utf8_byte);
      out->candidate_input_end_utf8_byte.push_back(candidate.input_end_utf8_byte);
    }
    out->backend_status = EPOCANVA_OK; return EPOCANVA_OK;
  }
  epocanva_status ReadCommit(UnifiedClient& client, const epocanva_backend_request& request,
                              BackendCommitOutcome* out) {
    auto commit = Header<epocanva_backend_commit>();
    auto status = epocanva_backend_read_commit(client.session, &request, &commit, nullptr, 0);
    if (status != EPOCANVA_BUFFER_TOO_SMALL || commit.required_utf8_bytes > 8192) return EPOCANVA_BACKEND_ERROR;
    std::string bytes(static_cast<std::size_t>(commit.required_utf8_bytes), '\0');
    status = epocanva_backend_read_commit(client.session, &request, &commit, bytes.data(), bytes.size());
    if (status != EPOCANVA_OK || !Slice(bytes, commit.text, &out->text_utf8) ||
        !Slice(bytes, commit.remaining_preedit, &out->remaining_preedit_utf8)) return EPOCANVA_BACKEND_ERROR;
    out->backend_status = status; out->commit_token = commit.commit_token;
    out->awaiting_ack = commit.awaiting_ack != 0; return status;
  }
  UnifiedBackendConfig config_;
  epocanva_backend_handle backend_ = 0;
  std::map<std::uint64_t, UnifiedClient> clients_;
};
}
std::unique_ptr<EngineBackend> MakeUnifiedEngineBackend(UnifiedBackendConfig config) {
  return std::make_unique<UnifiedEngineBackend>(std::move(config));
}
}  // namespace epocanva::windows::engine
