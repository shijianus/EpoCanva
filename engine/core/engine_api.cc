#include "epocanva_engine.h"

#include <climits>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "rime_adapter.h"

namespace {
namespace adapter = ::epocanva::engine::adapter;

template <class T> epocanva_status CheckStruct(const T* value) noexcept {
  if (value == nullptr) return EPOCANVA_INVALID_ARGUMENT;
  // Check size before touching any later member, including abi_version.
  if (value->struct_size_bytes < sizeof(T)) return EPOCANVA_STRUCT_TOO_SMALL;
  return value->abi_version == EPOCANVA_ENGINE_ABI_VERSION
             ? EPOCANVA_OK : EPOCANVA_ABI_VERSION_MISMATCH;
}
template <class T> epocanva_status ClearOutput(T* value) noexcept {
  const auto status = CheckStruct(value);
  if (status != EPOCANVA_OK) return status;
  const auto caller_size_bytes = value->struct_size_bytes;
  *value = T{}; // Only the v1 prefix; a future caller's tail is untouched.
  value->struct_size_bytes = caller_size_bytes;
  value->abi_version = EPOCANVA_ENGINE_ABI_VERSION;
  return EPOCANVA_OK;
}

template <class Function> epocanva_status Boundary(Function&& function) noexcept {
  try { return function(); }
  catch (const std::bad_alloc&) { return EPOCANVA_OUT_OF_MEMORY; }
  catch (...) { return EPOCANVA_INTERNAL_ERROR; }
}

epocanva_status CheckContext(const epocanva_context& context) noexcept {
  // Rechecked at the operation, not inferred from an earlier admission.
  if (context.privacy_class != EPOCANVA_PRIVACY_NORMAL) return EPOCANVA_PRIVACY_DENIED;
  if (context.context_target == 0 || context.generation == 0 || context.reserved != 0)
    return EPOCANVA_INVALID_ARGUMENT;
  return EPOCANVA_OK;
}

struct Engine {
  adapter::Runtime runtime;
  const std::thread::id owner = std::this_thread::get_id();
  std::uint64_t session_count = 0;
};
struct Session {
  Engine* engine = nullptr; // Engine cannot close while session_count != 0.
  adapter::Session backend;
  std::string schema_id;
  epocanva_context context{};
  std::uint64_t snapshot_version = 0;
  adapter::Snapshot snapshot;
  adapter::Commit commit;
  bool faulted = false;
};
struct Registry {
  std::mutex mutex;
  std::map<epocanva_engine_handle, std::unique_ptr<Engine>> engines;
  std::map<epocanva_session_handle, std::unique_ptr<Session>> sessions;
  std::uint64_t next_token = 1;
  std::uint64_t AllocateToken() noexcept {
    if (next_token == 0) return 0; // Exhaustion never wraps to a reused live/closed token.
    return next_token++;
  }
};
Registry& Objects() {
  // An intentionally process-lifetime registry prevents static destruction from
  // closing a forgotten thread-affine Rime session on an arbitrary exit thread.
  // Explicit close releases every engine/session; never finalize under live ones.
  static Registry* const objects = new Registry;
  return *objects;
}

epocanva_status Owner(const Engine& engine) noexcept {
  return engine.owner == std::this_thread::get_id() ? EPOCANVA_OK : EPOCANVA_WRONG_THREAD;
}

epocanva_status Access(Session& session, const epocanva_context& context) noexcept {
  auto status = Owner(*session.engine);
  if (status != EPOCANVA_OK) return status;
  status = CheckContext(context);
  if (status != EPOCANVA_OK) return status;
  if (context.context_target != session.context.context_target) return EPOCANVA_CONTEXT_MISMATCH;
  if (context.generation != session.context.generation) return EPOCANVA_GENERATION_MISMATCH;
  if (session.faulted) return EPOCANVA_SESSION_FAULTED;
  return EPOCANVA_OK;
}

template <class Function>
epocanva_status WithSession(epocanva_session_handle handle, const epocanva_context& context,
                            Function&& function) {
  auto& objects = Objects();
  std::lock_guard<std::mutex> lock(objects.mutex);
  const auto found = objects.sessions.find(handle);
  if (found == objects.sessions.end()) return EPOCANVA_INVALID_HANDLE;
  auto status = Access(*found->second, context);
  if (status != EPOCANVA_OK) return status;
  return function(objects, *found->second);
}

epocanva_utf8_span Span(std::uint64_t* cursor_utf8_byte_offset, const std::string& text) noexcept {
  const epocanva_utf8_span result{*cursor_utf8_byte_offset, text.size()};
  *cursor_utf8_byte_offset += text.size(); // At most four fields, each limited to INT_MAX bytes.
  return result;
}

epocanva_status CopyPayload(std::initializer_list<std::string_view> fields,
                            char* buffer, std::uint64_t capacity_utf8_bytes) noexcept {
  if (buffer == nullptr && capacity_utf8_bytes != 0) return EPOCANVA_INVALID_ARGUMENT;
  if (capacity_utf8_bytes > (std::numeric_limits<std::size_t>::max)())
    return EPOCANVA_OUT_OF_RANGE;
  std::uint64_t required_utf8_bytes = 0;
  for (const auto field : fields) {
    if (field.size() > (std::numeric_limits<std::uint64_t>::max)() - required_utf8_bytes)
      return EPOCANVA_OUT_OF_RANGE;
    required_utf8_bytes += field.size();
  }
  if (capacity_utf8_bytes < required_utf8_bytes) return EPOCANVA_BUFFER_TOO_SMALL;
  std::size_t output_utf8_byte_offset = 0;
  for (const auto field : fields) {
    if (!field.empty()) {
      std::memcpy(buffer + output_utf8_byte_offset, field.data(), field.size());
      output_utf8_byte_offset += field.size();
    }
  }
  return EPOCANVA_OK;
}

void ResultIdentity(const Session& session, epocanva_operation_result* output) noexcept {
  output->snapshot_version = session.snapshot_version;
  output->context_target = session.context.context_target;
  output->generation = session.context.generation;
  output->commit_pending = session.commit.present ? 1u : 0u;
  output->state_available = session.faulted ? 0u : 1u;
}

epocanva_status CheckCandidate(const Session& session,
                               const epocanva_candidate_request& request) noexcept {
  if (request.reserved != 0) return EPOCANVA_INVALID_ARGUMENT;
  if (request.snapshot_version != session.snapshot_version) return EPOCANVA_STALE_SNAPSHOT;
  if (request.candidate_ordinal_on_page >= session.snapshot.candidates.size())
    return EPOCANVA_OUT_OF_RANGE;
  return EPOCANVA_OK;
}

template <class Function>
epocanva_status Mutate(Registry& objects, Session& session,
                       epocanva_operation_result* output, Function&& backend_operation) {
  ResultIdentity(session, output);
  if (session.commit.present) return EPOCANVA_BUSY;
  if (session.snapshot.service_disabled) return EPOCANVA_BACKEND_ERROR;
  const auto version = objects.AllocateToken();
  if (version == 0) return EPOCANVA_OUT_OF_RANGE;
  // Invalidate FIRST. Any thrown exception leaves the session faulted and the old
  // menu unusable; a post-call error must never invite replay of the user's key.
  session.snapshot_version = version;
  session.faulted = true;
  output->backend_call_made = 1;
  ResultIdentity(session, output);
  bool handled = false;
  auto status = backend_operation(&handled);
  output->backend_reported_handled = handled ? 1u : 0u;
  if (status != EPOCANVA_OK) return status;
  adapter::Snapshot snapshot;
  adapter::Commit commit;
  const auto state_status = session.backend.Capture(&snapshot);
  const auto commit_status = session.backend.ReadCommit(&commit);
  // Exactly one backend commit read per entered, completed mutation, never per
  // size query. A failed read/conversion faults the session instead of guessing.
  if (state_status != EPOCANVA_OK) return state_status;
  if (commit_status != EPOCANVA_OK) return commit_status;
  session.snapshot = std::move(snapshot);
  session.commit = std::move(commit);
  session.faulted = false;
  ResultIdentity(session, output);
  return EPOCANVA_OK;
}
}  // namespace

extern "C" uint32_t EPOCANVA_ENGINE_CALL epocanva_engine_abi_version(void) noexcept {
  return EPOCANVA_ENGINE_ABI_VERSION;
}
extern "C" const char* EPOCANVA_ENGINE_CALL epocanva_status_name(epocanva_status status) noexcept {
  switch (status) {
    case EPOCANVA_OK: return "ok";
    case EPOCANVA_NO_COMMIT: return "no-commit";
    case EPOCANVA_INVALID_ARGUMENT: return "invalid-argument";
    case EPOCANVA_STRUCT_TOO_SMALL: return "struct-too-small";
    case EPOCANVA_ABI_VERSION_MISMATCH: return "abi-version-mismatch";
    case EPOCANVA_INVALID_UTF8: return "invalid-utf8";
    case EPOCANVA_PRIVACY_DENIED: return "privacy-denied";
    case EPOCANVA_INVALID_HANDLE: return "invalid-handle";
    case EPOCANVA_WRONG_THREAD: return "wrong-thread";
    case EPOCANVA_BUFFER_TOO_SMALL: return "buffer-too-small";
    case EPOCANVA_CONTEXT_MISMATCH: return "context-mismatch";
    case EPOCANVA_GENERATION_MISMATCH: return "generation-mismatch";
    case EPOCANVA_STALE_SNAPSHOT: return "stale-snapshot";
    case EPOCANVA_OUT_OF_RANGE: return "out-of-range";
    case EPOCANVA_BUSY: return "busy";
    case EPOCANVA_BACKEND_ERROR: return "backend-error";
    case EPOCANVA_SESSION_FAULTED: return "session-faulted";
    case EPOCANVA_OUT_OF_MEMORY: return "out-of-memory";
    case EPOCANVA_INTERNAL_ERROR: return "internal-error";
    case EPOCANVA_BACKEND_VERSION_MISMATCH: return "backend-version-mismatch";
    default: return "unknown-status";
  }
}

extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_engine_open(
    const epocanva_engine_options* options, epocanva_engine_handle* out_engine) noexcept {
  return Boundary([&]() -> epocanva_status {
    if (out_engine == nullptr) return EPOCANVA_INVALID_ARGUMENT;
    *out_engine = 0;
    auto status = CheckStruct(options);
    if (status != EPOCANVA_OK) return status;
    if (options->reserved != 0) return EPOCANVA_INVALID_ARGUMENT;
    auto& objects = Objects();
    std::lock_guard<std::mutex> lock(objects.mutex);
    auto engine = std::make_unique<Engine>();
    status = engine->runtime.Start(*options);
    if (status != EPOCANVA_OK) return status;
    const auto handle = objects.AllocateToken();
    if (handle == 0) return EPOCANVA_OUT_OF_RANGE;
    objects.engines.emplace(handle, std::move(engine));
    *out_engine = handle;
    return EPOCANVA_OK;
  });
}

extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_engine_close(
    epocanva_engine_handle handle) noexcept {
  return Boundary([&]() -> epocanva_status {
    auto& objects = Objects();
    std::lock_guard<std::mutex> lock(objects.mutex);
    const auto found = objects.engines.find(handle);
    if (found == objects.engines.end()) return EPOCANVA_INVALID_HANDLE;
    auto status = Owner(*found->second);
    if (status != EPOCANVA_OK) return status;
    if (found->second->session_count != 0) return EPOCANVA_BUSY;
    status = found->second->runtime.Shutdown();
    if (status != EPOCANVA_OK) return status;
    objects.engines.erase(found);
    return EPOCANVA_OK;
  });
}

extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_engine_read_info(
    epocanva_engine_handle handle, epocanva_engine_info* output,
    char* buffer, uint64_t capacity_utf8_bytes) noexcept {
  return Boundary([&]() -> epocanva_status {
    auto status = ClearOutput(output);
    if (status != EPOCANVA_OK) return status;
    auto& objects = Objects();
    std::lock_guard<std::mutex> lock(objects.mutex);
    const auto found = objects.engines.find(handle);
    if (found == objects.engines.end()) return EPOCANVA_INVALID_HANDLE;
    status = Owner(*found->second);
    if (status != EPOCANVA_OK) return status;
    adapter::RuntimeInfo info;
    status = found->second->runtime.ReadInfo(&info);
    if (status != EPOCANVA_OK) return status;
    info.counters.struct_size_bytes = output->struct_size_bytes;
    info.counters.abi_version = output->abi_version;
    *output = info.counters;
    std::uint64_t cursor_utf8_byte_offset = 0;
    output->loaded_dll_path = Span(&cursor_utf8_byte_offset, info.loaded_dll_path_utf8);
    output->rime_version = Span(&cursor_utf8_byte_offset, info.rime_version_utf8);
    output->required_utf8_bytes = cursor_utf8_byte_offset;
    return CopyPayload({info.loaded_dll_path_utf8, info.rime_version_utf8}, buffer, capacity_utf8_bytes);
  });
}

extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_session_open(
    epocanva_engine_handle engine_handle, const epocanva_session_options* options,
    epocanva_session_handle* out_session) noexcept {
  return Boundary([&]() -> epocanva_status {
    if (out_session == nullptr) return EPOCANVA_INVALID_ARGUMENT;
    *out_session = 0;
    auto status = CheckStruct(options);
    if (status != EPOCANVA_OK) return status;
    status = CheckContext(options->context);
    if (status != EPOCANVA_OK) return status; // No schema bytes touched on privacy denial.
    auto& objects = Objects();
    std::lock_guard<std::mutex> lock(objects.mutex);
    const auto found = objects.engines.find(engine_handle);
    if (found == objects.engines.end()) return EPOCANVA_INVALID_HANDLE;
    Engine& engine = *found->second;
    status = Owner(engine);
    if (status != EPOCANVA_OK) return status;
    auto session = std::make_unique<Session>();
    status = adapter::CopyUtf8Identifier(options->schema_id, &session->schema_id);
    if (status != EPOCANVA_OK) return status;
    session->engine = &engine;
    session->context = options->context;
    status = engine.runtime.CreateSession(session->schema_id, &session->backend);
    if (status != EPOCANVA_OK) return status;
    status = session->backend.Capture(&session->snapshot);
    if (status != EPOCANVA_OK) return status;
    const auto handle = objects.AllocateToken();
    const auto version = objects.AllocateToken();
    if (handle == 0 || version == 0) return EPOCANVA_OUT_OF_RANGE;
    session->snapshot_version = version;
    objects.sessions.emplace(handle, std::move(session));
    ++engine.session_count;
    *out_session = handle;
    return EPOCANVA_OK;
  });
}

extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_session_close(
    epocanva_session_handle handle) noexcept {
  return Boundary([&]() -> epocanva_status {
    auto& objects = Objects();
    std::lock_guard<std::mutex> lock(objects.mutex);
    const auto found = objects.sessions.find(handle);
    if (found == objects.sessions.end()) return EPOCANVA_INVALID_HANDLE;
    Session& session = *found->second;
    auto status = Owner(*session.engine);
    if (status != EPOCANVA_OK) return status;
    session.faulted = true;
    status = session.backend.Close();
    if (status != EPOCANVA_OK) return status;
    --session.engine->session_count;
    objects.sessions.erase(found);
    return EPOCANVA_OK;
  });
}

extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_session_process_key(
    epocanva_session_handle handle, const epocanva_key_request* request,
    epocanva_operation_result* output) noexcept {
  return Boundary([&]() -> epocanva_status {
    auto status = ClearOutput(output);
    if (status != EPOCANVA_OK) return status;
    status = CheckStruct(request);
    if (status != EPOCANVA_OK) return status;
    return WithSession(handle, request->context, [&](Registry& objects, Session& session) {
      if (request->keysym == 0 || request->keysym > INT_MAX ||
          (request->modifiers & ~0x5f001fffu) != 0) return epocanva_status(EPOCANVA_INVALID_ARGUMENT);
      // Unicode keysyms must identify one Unicode scalar, never a surrogate.
      if ((request->keysym & 0xff000000u) == 0x01000000u) {
        const auto scalar = request->keysym & 0x00ffffffu;
        if (scalar > 0x10ffffu || (scalar >= 0xd800u && scalar <= 0xdfffu))
          return epocanva_status(EPOCANVA_INVALID_ARGUMENT);
      }
      return Mutate(objects, session, output, [&](bool* handled) {
        return session.backend.ProcessKey(request->keysym, request->modifiers, handled);
      });
    });
  });
}

extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_session_select_candidate(
    epocanva_session_handle handle, const epocanva_candidate_request* request,
    epocanva_operation_result* output) noexcept {
  return Boundary([&]() -> epocanva_status {
    auto status = ClearOutput(output);
    if (status != EPOCANVA_OK) return status;
    status = CheckStruct(request);
    if (status != EPOCANVA_OK) return status;
    return WithSession(handle, request->context, [&](Registry& objects, Session& session) {
      const auto candidate_status = CheckCandidate(session, *request);
      if (candidate_status != EPOCANVA_OK) return candidate_status;
      return Mutate(objects, session, output, [&](bool* accepted) {
        return session.backend.SelectCandidate(request->candidate_ordinal_on_page, accepted);
      });
    });
  });
}

extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_session_cancel(
    epocanva_session_handle handle, const epocanva_session_request* request,
    epocanva_operation_result* output) noexcept {
  return Boundary([&]() -> epocanva_status {
    auto status = ClearOutput(output);
    if (status != EPOCANVA_OK) return status;
    status = CheckStruct(request);
    if (status != EPOCANVA_OK) return status;
    return WithSession(handle, request->context, [&](Registry& objects, Session& session) {
      if (session.context.generation == (std::numeric_limits<std::uint64_t>::max)())
        return epocanva_status(EPOCANVA_OUT_OF_RANGE);
      const auto version = objects.AllocateToken();
      if (version == 0) return epocanva_status(EPOCANVA_OUT_OF_RANGE);
      session.faulted = true;
      session.snapshot_version = version;
      ++session.context.generation;
      session.snapshot = adapter::Snapshot{};
      session.commit = adapter::Commit{};
      output->backend_call_made = 1;
      ResultIdentity(session, output);
      auto cancelled = session.backend.Close();
      if (cancelled != EPOCANVA_OK) return cancelled;
      cancelled = session.engine->runtime.CreateSession(session.schema_id, &session.backend);
      if (cancelled != EPOCANVA_OK) return cancelled;
      cancelled = session.backend.Capture(&session.snapshot);
      if (cancelled != EPOCANVA_OK) return cancelled;
      session.faulted = false;
      output->backend_reported_handled = 1;
      ResultIdentity(session, output);
      return epocanva_status(EPOCANVA_OK);
    });
  });
}

extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_session_read_snapshot(
    epocanva_session_handle handle, const epocanva_session_request* request,
    epocanva_snapshot* output, char* buffer, uint64_t capacity_utf8_bytes) noexcept {
  return Boundary([&]() -> epocanva_status {
    auto status = ClearOutput(output);
    if (status != EPOCANVA_OK) return status;
    status = CheckStruct(request);
    if (status != EPOCANVA_OK) return status;
    return WithSession(handle, request->context, [&](Registry&, Session& session) {
      const auto& snapshot = session.snapshot;
      output->snapshot_version = session.snapshot_version;
      output->context_target = session.context.context_target;
      output->generation = session.context.generation;
      std::uint64_t cursor_utf8_byte_offset = 0;
      output->schema_id = Span(&cursor_utf8_byte_offset, snapshot.schema_id_utf8);
      output->schema_name = Span(&cursor_utf8_byte_offset, snapshot.schema_name_utf8);
      output->raw_input = Span(&cursor_utf8_byte_offset, snapshot.raw_input_utf8);
      output->preedit = Span(&cursor_utf8_byte_offset, snapshot.preedit_utf8);
      output->required_utf8_bytes = cursor_utf8_byte_offset;
      output->cursor_utf8_byte_offset = snapshot.cursor_utf8_byte_offset;
      output->selection_start_utf8_byte_offset = snapshot.selection_start_utf8_byte_offset;
      output->selection_end_utf8_byte_offset = snapshot.selection_end_utf8_byte_offset;
      output->is_composing = snapshot.is_composing ? 1u : 0u;
      output->service_disabled = snapshot.service_disabled ? 1u : 0u;
      output->candidate_count_on_page = static_cast<std::uint32_t>(snapshot.candidates.size());
      output->highlighted_candidate_ordinal_on_page = snapshot.highlighted_candidate_ordinal_on_page;
      output->menu_page_ordinal = snapshot.menu_page_ordinal;
      output->menu_page_capacity = snapshot.menu_page_capacity;
      output->is_last_page = snapshot.is_last_page ? 1u : 0u;
      output->commit_pending = session.commit.present ? 1u : 0u;
      return CopyPayload({snapshot.schema_id_utf8, snapshot.schema_name_utf8,
                          snapshot.raw_input_utf8, snapshot.preedit_utf8}, buffer, capacity_utf8_bytes);
    });
  });
}

extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_session_read_candidate(
    epocanva_session_handle handle, const epocanva_candidate_request* request,
    epocanva_candidate* output, char* buffer, uint64_t capacity_utf8_bytes) noexcept {
  return Boundary([&]() -> epocanva_status {
    auto status = ClearOutput(output);
    if (status != EPOCANVA_OK) return status;
    status = CheckStruct(request);
    if (status != EPOCANVA_OK) return status;
    return WithSession(handle, request->context, [&](Registry&, Session& session) {
      const auto candidate_status = CheckCandidate(session, *request);
      if (candidate_status != EPOCANVA_OK) return candidate_status;
      const auto& candidate = session.snapshot.candidates[request->candidate_ordinal_on_page];
      output->snapshot_version = session.snapshot_version;
      std::uint64_t cursor_utf8_byte_offset = 0;
      output->text = Span(&cursor_utf8_byte_offset, candidate.text_utf8);
      output->comment = Span(&cursor_utf8_byte_offset, candidate.comment_utf8);
      output->required_utf8_bytes = cursor_utf8_byte_offset;
      return CopyPayload({candidate.text_utf8, candidate.comment_utf8}, buffer, capacity_utf8_bytes);
    });
  });
}

extern "C" epocanva_status EPOCANVA_ENGINE_CALL epocanva_session_read_commit(
    epocanva_session_handle handle, const epocanva_session_request* request,
    epocanva_commit* output, char* buffer, uint64_t capacity_utf8_bytes) noexcept {
  return Boundary([&]() -> epocanva_status {
    auto status = ClearOutput(output);
    if (status != EPOCANVA_OK) return status;
    status = CheckStruct(request);
    if (status != EPOCANVA_OK) return status;
    return WithSession(handle, request->context, [&](Registry&, Session& session) {
      output->context_target = session.context.context_target;
      output->generation = session.context.generation;
      output->snapshot_version = session.snapshot_version;
      if (!session.commit.present) return epocanva_status(EPOCANVA_NO_COMMIT);
      output->required_utf8_bytes = session.commit.text_utf8.size();
      output->text = {0, session.commit.text_utf8.size()};
      // A sizing query never consumes even an empty, but present, commit.
      if (buffer == nullptr && capacity_utf8_bytes == 0)
        return epocanva_status(EPOCANVA_BUFFER_TOO_SMALL);
      const auto copied = CopyPayload({session.commit.text_utf8}, buffer, capacity_utf8_bytes);
      if (copied == EPOCANVA_OK) session.commit = adapter::Commit{};
      return copied;
    });
  });
}
