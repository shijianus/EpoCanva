#include "rime_adapter.h"

#include <climits>
#include <filesystem>
#include <limits>
#include <string_view>
#include <utility>

#include "rime_runtime.h"
#include "rime_session.h"
#include "text_encoding.h"

namespace epocanva {
namespace engine {
namespace adapter {
namespace {
namespace legacy = ::epocanva::windows::engine;
namespace text = ::epocanva::windows::text;

epocanva_status ConversionStatus(text::ConversionStatus status) {
  if (status == text::ConversionStatus::kOk) return EPOCANVA_OK;
  if (status == text::ConversionStatus::kInvalidInput) return EPOCANVA_INVALID_UTF8;
  if (status == text::ConversionStatus::kInputTooLong ||
      status == text::ConversionStatus::kOutputTooLong) return EPOCANVA_OUT_OF_RANGE;
  return EPOCANVA_BACKEND_ERROR;
}

epocanva_status Path(epocanva_utf8_view view, std::filesystem::path* output) {
  std::string utf8;
  const auto copied = CopyUtf8Identifier(view, &utf8);
  if (copied != EPOCANVA_OK) return copied;
  const auto converted = text::ConvertUtf8BytesToUtf16CodeUnits(utf8);
  if (!converted.ok()) return ConversionStatus(converted.status);
  *output = std::filesystem::path(converted.utf16_code_units);
  return EPOCANVA_OK;
}

epocanva_status Utf8(const std::u16string& utf16_code_units, std::string* output) {
  // No wchar_t/char16_t aliasing. The existing converter owns strict validation.
  const std::wstring native(utf16_code_units.begin(), utf16_code_units.end());
  auto converted = text::ConvertUtf16CodeUnitsToUtf8Bytes(native);
  if (!converted.ok()) return ConversionStatus(converted.status);
  *output = std::move(converted.utf8_bytes);
  return EPOCANVA_OK;
}

epocanva_status Text(const legacy::EngineText& input, std::string* output) {
  const auto status = Utf8(input.utf16_code_units, output);
  if (status != EPOCANVA_OK) return status;
  return output->size() == input.utf8_byte_length ? EPOCANVA_OK : EPOCANVA_BACKEND_ERROR;
}

epocanva_status SnapshotFrom(const legacy::EngineState& input, Snapshot* output) {
  if (!input.valid) return EPOCANVA_BACKEND_ERROR;
  Snapshot snapshot;
  // schema strings were not validated by the old state copier; validate here too.
  for (const std::string* field : {&input.schema_id, &input.schema_name}) {
    const auto checked = text::ConvertUtf8BytesToUtf16CodeUnits(*field);
    if (!checked.ok()) return ConversionStatus(checked.status);
  }
  snapshot.schema_id_utf8 = input.schema_id;
  snapshot.schema_name_utf8 = input.schema_name;
  auto status = Text(input.raw_input, &snapshot.raw_input_utf8);
  if (status != EPOCANVA_OK) return status;
  status = Text(input.preedit, &snapshot.preedit_utf8);
  if (status != EPOCANVA_OK) return status;
  if (input.cursor_status == legacy::CursorOffsetStatus::kMapped) {
    std::size_t utf8_byte_offset = 0;
    if (input.preedit.boundary_map.MapUtf16CodeUnitOffsetToUtf8ByteOffset(
            input.cursor_utf16_code_unit_offset, &utf8_byte_offset) !=
        text::BoundaryLookupStatus::kMapped) return EPOCANVA_BACKEND_ERROR;
    snapshot.cursor_utf8_byte_offset = utf8_byte_offset;
  } else if (input.cursor_status != legacy::CursorOffsetStatus::kNoCursor) {
    return EPOCANVA_BACKEND_ERROR;
  }
  if (input.selection_status == legacy::SelectionOffsetStatus::kMapped) {
    std::size_t start_utf8_byte_offset = 0;
    std::size_t end_utf8_byte_offset = 0;
    if (input.preedit.boundary_map.MapUtf16CodeUnitOffsetToUtf8ByteOffset(
            input.selection_start_utf16_code_unit_offset, &start_utf8_byte_offset) !=
            text::BoundaryLookupStatus::kMapped ||
        input.preedit.boundary_map.MapUtf16CodeUnitOffsetToUtf8ByteOffset(
            input.selection_end_utf16_code_unit_offset, &end_utf8_byte_offset) !=
            text::BoundaryLookupStatus::kMapped ||
        start_utf8_byte_offset > end_utf8_byte_offset) return EPOCANVA_BACKEND_ERROR;
    snapshot.selection_start_utf8_byte_offset = start_utf8_byte_offset;
    snapshot.selection_end_utf8_byte_offset = end_utf8_byte_offset;
  } else if (input.selection_status != legacy::SelectionOffsetStatus::kNoSelection) {
    return EPOCANVA_BACKEND_ERROR;
  }
  if (input.candidates.size() > (std::numeric_limits<std::uint32_t>::max)() ||
      input.page_no < 0 || input.page_size < 0) return EPOCANVA_BACKEND_ERROR;
  snapshot.is_composing = input.is_composing;
  snapshot.service_disabled = input.service_disabled;
  snapshot.menu_page_ordinal = static_cast<std::uint32_t>(input.page_no);
  snapshot.menu_page_capacity = static_cast<std::uint32_t>(input.page_size);
  snapshot.is_last_page = input.is_last_page;
  if (input.highlighted_candidate_index_on_page >= 0) {
    snapshot.highlighted_candidate_ordinal_on_page =
        static_cast<std::uint32_t>(input.highlighted_candidate_index_on_page);
    if (snapshot.highlighted_candidate_ordinal_on_page >= input.candidates.size())
      return EPOCANVA_BACKEND_ERROR;
  }
  snapshot.candidates.reserve(input.candidates.size());
  for (const auto& source : input.candidates) {
    Candidate candidate;
    status = Utf8(source.text_utf16_code_units, &candidate.text_utf8);
    if (status != EPOCANVA_OK) return status;
    status = Utf8(source.comment_utf16_code_units, &candidate.comment_utf8);
    if (status != EPOCANVA_OK) return status;
    snapshot.candidates.push_back(std::move(candidate));
  }
  *output = std::move(snapshot);
  return EPOCANVA_OK;
}
}  // namespace

epocanva_status CopyUtf8Identifier(epocanva_utf8_view view, std::string* output) {
  if (view.utf8_byte_count == 0 || view.data == nullptr) return EPOCANVA_INVALID_ARGUMENT;
  if (view.utf8_byte_count > INT_MAX) return EPOCANVA_OUT_OF_RANGE;
  const std::string_view bytes(view.data, static_cast<std::size_t>(view.utf8_byte_count));
  const auto converted = text::ConvertUtf8BytesToUtf16CodeUnits(bytes);
  if (!converted.ok()) return ConversionStatus(converted.status);
  if (bytes.find('\0') != std::string_view::npos) return EPOCANVA_INVALID_ARGUMENT;
  output->assign(bytes.data(), bytes.size());
  return EPOCANVA_OK;
}

struct Runtime::Impl {
  std::unique_ptr<legacy::RimeRuntime> runtime = std::make_unique<legacy::RimeRuntime>();
};
struct Session::Impl { std::unique_ptr<legacy::RimeSession> session; };
Runtime::Runtime() : impl_(new Impl) {}
Runtime::~Runtime() {
  // Rollback must not invoke a legacy implicitly-noexcept destructor that retries
  // a throwing cleanup. Explicit close reports failures to the caller; on failed
  // rollback retain the original runtime/slot rather than terminate or finalize
  // beneath a live session. No second initialization path is introduced.
  try {
    const auto status = impl_->runtime->Shutdown();
    if (status != legacy::RuntimeShutdownStatus::kOk &&
        status != legacy::RuntimeShutdownStatus::kAlreadyShutdown &&
        status != legacy::RuntimeShutdownStatus::kNotStarted)
      (void)impl_->runtime.release();
  } catch (...) {
    (void)impl_->runtime.release();
  }
}
Session::Session() : impl_(new Impl) {}
Session::~Session() {
  try {
    if (Close() != EPOCANVA_OK) (void)impl_->session.release();
  } catch (...) {
    (void)impl_->session.release();
  }
}

epocanva_status Runtime::Start(const epocanva_engine_options& options) {
  legacy::RuntimeOptions native;
  auto status = Path(options.rime_dll_path, &native.rime_dll_path);
  if (status != EPOCANVA_OK) return status;
  status = Path(options.shared_data_dir, &native.shared_data_dir);
  if (status != EPOCANVA_OK) return status;
  status = Path(options.user_data_dir, &native.user_data_dir);
  if (status != EPOCANVA_OK) return status;
  status = Path(options.log_dir, &native.log_dir);
  if (status != EPOCANVA_OK) return status;
  std::string expected_version;
  status = CopyUtf8Identifier(options.expected_rime_version, &expected_version);
  if (status != EPOCANVA_OK) return status;
  if (options.deployment_schema_count != 0) {
    if (options.deployment_schema_paths == nullptr) return EPOCANVA_INVALID_ARGUMENT;
    legacy::DeploymentOptions deployment;
    for (std::uint32_t entry = 0; entry < options.deployment_schema_count; ++entry) {
      std::filesystem::path schema;
      status = Path(options.deployment_schema_paths[entry], &schema);
      if (status != EPOCANVA_OK) return status;
      deployment.schema_files.push_back(std::move(schema));
    }
    native.deployment = std::move(deployment);
  }
  switch (impl_->runtime->Start(native)) {
    case legacy::RuntimeStartStatus::kOk: break;
    case legacy::RuntimeStartStatus::kAnotherRuntimeActive:
    case legacy::RuntimeStartStatus::kAlreadyStarted: return EPOCANVA_BUSY;
    case legacy::RuntimeStartStatus::kInvalidOptions: return EPOCANVA_INVALID_ARGUMENT;
    default: return EPOCANVA_BACKEND_ERROR;
  }
  if (impl_->runtime->Info().librime_version != expected_version) {
    const auto closed = Shutdown();
    return closed == EPOCANVA_OK ? EPOCANVA_BACKEND_VERSION_MISMATCH : closed;
  }
  return EPOCANVA_OK;
}

epocanva_status Runtime::Shutdown() {
  switch (impl_->runtime->Shutdown()) {
    case legacy::RuntimeShutdownStatus::kOk:
    case legacy::RuntimeShutdownStatus::kAlreadyShutdown: return EPOCANVA_OK;
    case legacy::RuntimeShutdownStatus::kWrongThread: return EPOCANVA_WRONG_THREAD;
    case legacy::RuntimeShutdownStatus::kLiveSessionsRemain: return EPOCANVA_BUSY;
    default: return EPOCANVA_BACKEND_ERROR;
  }
}

epocanva_status Runtime::CreateSession(const std::string& schema_id, Session* output) {
  if (output->impl_->session) return EPOCANVA_BUSY;
  auto result = impl_->runtime->CreateSession(schema_id);
  if (!result.ok()) {
    if (result.status == legacy::SessionCreateStatus::kWrongThread)
      return EPOCANVA_WRONG_THREAD;
    if (result.status == legacy::SessionCreateStatus::kInvalidSchemaId)
      return EPOCANVA_INVALID_ARGUMENT;
    return EPOCANVA_BACKEND_ERROR;
  }
  output->impl_->session = std::move(result.session);
  return EPOCANVA_OK;
}

epocanva_status Runtime::ReadInfo(RuntimeInfo* output) const {
  const auto info = impl_->runtime->Info();
  const auto counts = impl_->runtime->ApiCounts();
  auto path = text::ConvertUtf16CodeUnitsToUtf8Bytes(info.loaded_dll_path);
  if (!path.ok()) return ConversionStatus(path.status);
  output->loaded_dll_path_utf8 = std::move(path.utf8_bytes);
  output->rime_version_utf8 = info.librime_version;
  auto& result = output->counters;
  result.initialized = info.initialized ? 1u : 0u;
  result.english_module_loaded = info.english_module_loaded ? 1u : 0u;
  result.initialize_calls = info.initialize_calls;
  result.finalize_calls = info.finalize_calls;
  result.live_session_count = static_cast<std::uint64_t>(info.live_session_count);
  result.create_session_calls = counts.create_session_calls;
  result.destroy_session_calls = counts.destroy_session_calls;
  result.process_key_calls = counts.process_key_calls;
  result.select_candidate_calls = counts.select_candidate_calls;
  result.status_acquired = counts.status_acquired;
  result.status_freed = counts.status_freed;
  result.context_acquired = counts.context_acquired;
  result.context_freed = counts.context_freed;
  result.commit_acquired = counts.commit_acquired;
  result.commit_freed = counts.commit_freed;
  result.schema_config_acquired = counts.schema_config_acquired;
  result.schema_config_closed = counts.schema_config_closed;
  return EPOCANVA_OK;
}

epocanva_status Session::ProcessKey(std::uint32_t keysym, std::uint32_t modifiers,
                                  bool* handled) {
  if (!impl_->session) return EPOCANVA_SESSION_FAULTED;
  const auto result = impl_->session->ProcessKeyOnce(static_cast<int>(keysym),
                                                    static_cast<int>(modifiers));
  if (!result.engine_call_made()) return EPOCANVA_BACKEND_ERROR;
  *handled = result.engine_reported_handled;
  return EPOCANVA_OK;
}

epocanva_status Session::SelectCandidate(std::uint32_t candidate_ordinal_on_page,
                                       bool* accepted) {
  if (!impl_->session) return EPOCANVA_SESSION_FAULTED;
  const auto result = impl_->session->SelectCandidateOnce(candidate_ordinal_on_page);
  if (!result.engine_call_made()) return EPOCANVA_BACKEND_ERROR;
  *accepted = result.engine_reported_accepted;
  return EPOCANVA_OK;
}

epocanva_status Session::Capture(Snapshot* output) {
  if (!impl_->session) return EPOCANVA_SESSION_FAULTED;
  const auto result = impl_->session->CaptureState();
  if (!result.ok()) {
    return result.status == legacy::SessionStateStatus::kTextConversionFailed
               ? EPOCANVA_INVALID_UTF8 : EPOCANVA_BACKEND_ERROR;
  }
  return SnapshotFrom(result.state, output);
}

epocanva_status Session::ReadCommit(Commit* output) {
  if (!impl_->session) return EPOCANVA_SESSION_FAULTED;
  const auto result = impl_->session->ReadCommit();
  if (result.status == legacy::SessionCommitStatus::kNoCommit) {
    *output = Commit{};
    return EPOCANVA_OK;
  }
  if (result.status != legacy::SessionCommitStatus::kRead || !result.commit.present)
    return result.status == legacy::SessionCommitStatus::kConversionFailed
               ? EPOCANVA_INVALID_UTF8 : EPOCANVA_BACKEND_ERROR;
  Commit commit;
  auto status = Utf8(result.commit.text_utf16_code_units, &commit.text_utf8);
  if (status != EPOCANVA_OK) return status;
  if (commit.text_utf8.size() != result.commit.utf8_byte_length)
    return EPOCANVA_BACKEND_ERROR;
  commit.present = true;
  *output = std::move(commit);
  return EPOCANVA_OK;
}

epocanva_status Session::Close() {
  if (!impl_->session) return EPOCANVA_OK;
  switch (impl_->session->Close()) {
    case legacy::SessionCloseStatus::kOk:
    case legacy::SessionCloseStatus::kAlreadyClosed:
      impl_->session.reset();
      return EPOCANVA_OK;
    case legacy::SessionCloseStatus::kWrongThread: return EPOCANVA_WRONG_THREAD;
    default: return EPOCANVA_BACKEND_ERROR;
  }
}

}  // namespace adapter
}  // namespace engine
}  // namespace epocanva
