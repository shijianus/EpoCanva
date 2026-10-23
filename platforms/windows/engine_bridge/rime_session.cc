// EpoCanva P3.4 -- session implementation. See rime_session.h for the contract and units.
#include "rime_session.h"

#include <cstring>
#include <utility>

#include "rime_runtime_internal.h"

namespace epocanva {
namespace windows {
namespace engine {

struct RimeSession::Impl {
  std::shared_ptr<RuntimeCore> core;
  std::uintptr_t session_id = 0;
  std::string schema_id;
  bool open = false;
};

RimeSession::RimeSession(std::shared_ptr<RuntimeCore> core,
                         std::uintptr_t session_id, std::string schema_id)
    : impl_(new Impl) {
  impl_->core = std::move(core);
  impl_->session_id = session_id;
  impl_->schema_id = std::move(schema_id);
  impl_->open = true;
  impl_->core->live_sessions += 1;
}

RimeSession::~RimeSession() {
  if (!impl_ || !impl_->open) {
    return;
  }
  // The destructor is subject to the same ownership rule as every method. On the wrong
  // thread it deliberately does NOT call destroy_session: making an engine call there is
  // exactly the failure this round refuses to hide. The engine session and the live-session
  // count are left in place, so Runtime::Shutdown will refuse to finalize instead of
  // unloading the DLL underneath a session it cannot see.
  if (impl_->core && impl_->core->IsOwnerThread() && impl_->core->initialized) {
    (void)Close();
  }
}

bool RimeSession::IsOpen() const { return impl_ && impl_->open; }

std::uintptr_t RimeSession::session_id() const {
  return impl_ ? impl_->session_id : 0;
}

const std::string& RimeSession::schema_id() const {
  static const std::string kEmpty;
  return impl_ ? impl_->schema_id : kEmpty;
}

SessionKeyResult RimeSession::ProcessKeyOnce(int keysym, int modifiers) {
  SessionKeyResult result;
  if (!impl_->core->IsOwnerThread()) {
    result.status = SessionKeyStatus::kWrongThread;
    result.diagnostic = "process_key refused: caller is not the runtime owner thread";
    return result;
  }
  if (!impl_->open) {
    result.status = SessionKeyStatus::kClosed;
    result.diagnostic = "process_key refused: session is closed";
    return result;
  }
  if (!impl_->core->initialized) {
    result.status = SessionKeyStatus::kRuntimeNotInitialized;
    result.diagnostic = "process_key refused: runtime is not initialized";
    return result;
  }

  impl_->core->counts.process_key_calls += 1;
  const Bool handled = impl_->core->api->process_key(impl_->session_id, keysym, modifiers);
  result.status = SessionKeyStatus::kCallMade;
  result.engine_reported_handled = handled != False;
  return result;
}

SessionSelectionResult RimeSession::SelectCandidateOnce(std::size_t candidate_index) {
  SessionSelectionResult result;
  if (!impl_->core->IsOwnerThread()) {
    result.status = SessionSelectionStatus::kWrongThread;
    result.diagnostic = "select_candidate refused: caller is not the runtime owner thread";
    return result;
  }
  if (!impl_->open) {
    result.status = SessionSelectionStatus::kClosed;
    result.diagnostic = "select_candidate refused: session is closed";
    return result;
  }
  if (!impl_->core->initialized) {
    result.status = SessionSelectionStatus::kRuntimeNotInitialized;
    result.diagnostic = "select_candidate refused: runtime is not initialized";
    return result;
  }

  impl_->core->counts.select_candidate_calls += 1;
  const Bool accepted =
      impl_->core->api->select_candidate(impl_->session_id, candidate_index);
  result.status = SessionSelectionStatus::kCallMade;
  result.engine_reported_accepted = accepted != False;
  return result;
}

SessionStateResult RimeSession::CaptureState() {
  SessionStateResult result;
  RuntimeCore& core = *impl_->core;
  if (!core.IsOwnerThread()) {
    result.status = SessionStateStatus::kWrongThread;
    result.diagnostic = "state read refused: caller is not the runtime owner thread";
    return result;
  }
  if (!impl_->open) {
    result.status = SessionStateStatus::kClosed;
    result.diagnostic = "state read refused: session is closed";
    return result;
  }
  if (!core.initialized) {
    result.status = SessionStateStatus::kRuntimeNotInitialized;
    result.diagnostic = "state read refused: runtime is not initialized";
    return result;
  }

  // --- RimeStatus ----------------------------------------------------------------
  RIME_STRUCT(RimeStatus, status);
  const Bool got_status = core.api->get_status(impl_->session_id, &status);
  if (got_status != False) {
    core.counts.status_acquired += 1;
  }
  std::string schema_id;
  std::string schema_name;
  bool service_disabled = false;
  bool is_composing = false;
  bool status_copy_ok = got_status != False;
  if (got_status != False) {
    if (status.schema_id == nullptr || status.schema_name == nullptr) {
      status_copy_ok = false;
    } else {
      schema_id = status.schema_id;
      schema_name = status.schema_name;
      service_disabled = status.is_disabled != False;
      is_composing = status.is_composing != False;
    }
    const Bool freed = core.api->free_status(&status);
    if (freed != False) {
      core.counts.status_freed += 1;
    } else {
      result.status = SessionStateStatus::kFreeFailed;
      result.diagnostic = "free_status refused";
      return result;
    }
  }
  if (got_status == False) {
    result.status = SessionStateStatus::kStatusFailed;
    result.diagnostic = "get_status refused";
    return result;
  }
  if (!status_copy_ok) {
    result.status = SessionStateStatus::kStatusFailed;
    result.diagnostic = "get_status returned a null schema field";
    return result;
  }

  // --- RimeContext ---------------------------------------------------------------
  RIME_STRUCT(RimeContext, context);
  const Bool got_context = core.api->get_context(impl_->session_id, &context);
  if (got_context != False) {
    core.counts.context_acquired += 1;
  }
  std::string preedit_utf8;
  int cursor_pos_utf8_bytes = 0;
  int sel_start_utf8_bytes = 0;
  int sel_end_utf8_bytes = 0;
  int page_size = 0;
  int page_no = 0;
  bool is_last_page = false;
  int highlighted = -1;
  std::vector<std::pair<std::string, std::string>> raw_candidates;
  bool context_copy_ok = got_context != False;
  std::string context_copy_error;

  if (got_context != False) {
    const int preedit_length = context.composition.length;
    if (preedit_length < 0) {
      context_copy_ok = false;
      context_copy_error = "RimeComposition.length is negative";
    } else if (preedit_length == 0) {
      if (context.composition.preedit != nullptr &&
          context.composition.preedit[0] != '\0') {
        context_copy_ok = false;
        context_copy_error = "RimeComposition.length is 0 but preedit is not empty";
      }
    } else if (context.composition.preedit == nullptr) {
      context_copy_ok = false;
      context_copy_error = "RimeComposition.length > 0 but preedit is null";
    } else {
      // The API allocates length + 1 and strcpy()s (src/rime_api_impl.h:217-223). The
      // returned length is nevertheless checked against the actual terminator instead of
      // being trusted on its own.
      const std::size_t actual = std::strlen(context.composition.preedit);
      if (actual != static_cast<std::size_t>(preedit_length)) {
        context_copy_ok = false;
        context_copy_error = "RimeComposition.length disagrees with the preedit string";
      } else {
        preedit_utf8.assign(context.composition.preedit, actual);
      }
    }
    cursor_pos_utf8_bytes = context.composition.cursor_pos;
    sel_start_utf8_bytes = context.composition.sel_start;
    sel_end_utf8_bytes = context.composition.sel_end;

    if (context_copy_ok) {
      page_size = context.menu.page_size;
      page_no = context.menu.page_no;
      is_last_page = context.menu.is_last_page != False;
      const int count = context.menu.num_candidates;
      if (count < 0) {
        context_copy_ok = false;
        context_copy_error = "RimeMenu.num_candidates is negative";
      } else if (count > 0) {
        if (context.menu.candidates == nullptr) {
          // RimeFreeContext would iterate a null array (src/rime_api_impl.h:285-307). The
          // count is cleared before the free so the API's own loop is not entered; the
          // observation is then refused.
          context.menu.num_candidates = 0;
          context_copy_ok = false;
          context_copy_error = "RimeMenu.num_candidates > 0 but candidates is null";
        } else if (page_size <= 0 || page_no < 0) {
          context_copy_ok = false;
          context_copy_error = "RimeMenu page_size/page_no is not usable";
        } else {
          highlighted = context.menu.highlighted_candidate_index;
          if (highlighted < 0) {
            highlighted = -1;
          } else if (highlighted >= count) {
            context_copy_ok = false;
            context_copy_error = "highlighted candidate index is outside this page";
          }
          raw_candidates.reserve(static_cast<std::size_t>(count));
          for (int i = 0; context_copy_ok && i < count; ++i) {
            const RimeCandidate& candidate = context.menu.candidates[i];
            if (candidate.text == nullptr) {
              context_copy_ok = false;
              context_copy_error = "a candidate text is null";
              break;
            }
            raw_candidates.emplace_back(
                std::string(candidate.text),
                candidate.comment == nullptr ? std::string() : std::string(candidate.comment));
          }
        }
      }
    }

    const Bool freed = core.api->free_context(&context);
    if (freed != False) {
      core.counts.context_freed += 1;
    } else {
      result.status = SessionStateStatus::kFreeFailed;
      result.diagnostic = "free_context refused";
      return result;
    }
  }
  if (got_context == False) {
    result.status = SessionStateStatus::kContextFailed;
    result.diagnostic = "get_context refused";
    return result;
  }
  if (!context_copy_ok) {
    result.status = SessionStateStatus::kContextFailed;
    result.diagnostic = context_copy_error;
    return result;
  }

  // --- get_input: borrowed, copied before anything else can run -------------------
  const char* borrowed_input = core.api->get_input(impl_->session_id);
  if (borrowed_input == nullptr) {
    result.status = SessionStateStatus::kInputFailed;
    result.diagnostic = "get_input returned null while the session is open";
    return result;
  }
  const std::string raw_utf8(borrowed_input);

  // --- strict conversions ---------------------------------------------------------
  EngineState state;
  state.schema_id = std::move(schema_id);
  state.schema_name = std::move(schema_name);
  state.service_disabled = service_disabled;
  state.is_composing = is_composing;

  EngineTextCapture raw = CaptureEngineText(raw_utf8);
  if (!raw.ok()) {
    result.status = SessionStateStatus::kTextConversionFailed;
    result.diagnostic = "raw input: " + raw.diagnostic;
    return result;
  }
  state.raw_input = std::move(raw.text);

  EngineTextCapture preedit = CaptureEngineText(preedit_utf8);
  if (!preedit.ok()) {
    result.status = SessionStateStatus::kTextConversionFailed;
    result.diagnostic = "preedit: " + preedit.diagnostic;
    return result;
  }
  state.preedit = std::move(preedit.text);

  if (is_composing) {
    const PreeditOffsetMapping mapping = MapPreeditOffsets(
        preedit_utf8, cursor_pos_utf8_bytes, sel_start_utf8_bytes, sel_end_utf8_bytes);
    state.cursor_status = mapping.cursor_status;
    state.cursor_utf16_code_unit_offset = mapping.cursor_utf16_code_unit_offset;
    state.selection_status = mapping.selection_status;
    state.selection_start_utf16_code_unit_offset =
        mapping.selection_start_utf16_code_unit_offset;
    state.selection_end_utf16_code_unit_offset = mapping.selection_end_utf16_code_unit_offset;
  } else {
    // Not composing: RimeGetContext leaves the composition fields zeroed, and "zero" there is
    // not a cursor at offset 0. Report the legal no-value states instead.
    state.cursor_status = CursorOffsetStatus::kNoCursor;
    state.selection_status = SelectionOffsetStatus::kNoSelection;
  }

  state.has_menu = !raw_candidates.empty();
  if (state.has_menu) {
    state.page_size = page_size;
    state.page_no = page_no;
    state.is_last_page = is_last_page;
    state.highlighted_candidate_index_on_page = highlighted;
    state.candidates.reserve(raw_candidates.size());
    for (const auto& candidate : raw_candidates) {
      EngineTextCapture text = CaptureEngineText(candidate.first);
      if (!text.ok()) {
        result.status = SessionStateStatus::kTextConversionFailed;
        result.diagnostic = "candidate text: " + text.diagnostic;
        return result;
      }
      EngineTextCapture comment = CaptureEngineText(candidate.second);
      if (!comment.ok()) {
        result.status = SessionStateStatus::kTextConversionFailed;
        result.diagnostic = "candidate comment: " + comment.diagnostic;
        return result;
      }
      EngineCandidate copied;
      copied.text_utf16_code_units = std::move(text.text.utf16_code_units);
      copied.comment_utf16_code_units = std::move(comment.text.utf16_code_units);
      state.candidates.push_back(std::move(copied));
    }
  }

  state.valid = true;
  result.status = SessionStateStatus::kOk;
  result.state = std::move(state);
  return result;
}

SessionCommitResult RimeSession::ReadCommit() {
  SessionCommitResult result;
  RuntimeCore& core = *impl_->core;
  if (!core.IsOwnerThread()) {
    result.status = SessionCommitStatus::kWrongThread;
    result.diagnostic = "commit read refused: caller is not the runtime owner thread";
    return result;
  }
  if (!impl_->open) {
    result.status = SessionCommitStatus::kClosed;
    result.diagnostic = "commit read refused: session is closed";
    return result;
  }
  if (!core.initialized) {
    result.status = SessionCommitStatus::kRuntimeNotInitialized;
    result.diagnostic = "commit read refused: runtime is not initialized";
    return result;
  }

  RIME_STRUCT(RimeCommit, commit);
  const Bool got_commit = core.api->get_commit(impl_->session_id, &commit);
  if (got_commit == False) {
    result.status = SessionCommitStatus::kNoCommit;
    return result;
  }
  core.counts.commit_acquired += 1;

  if (commit.text == nullptr) {
    const Bool freed = core.api->free_commit(&commit);
    if (freed != False) {
      core.counts.commit_freed += 1;
    }
    result.status = SessionCommitStatus::kConversionFailed;
    result.diagnostic = "get_commit returned True with a null text";
    return result;
  }
  const std::string text_utf8(commit.text);
  const Bool freed = core.api->free_commit(&commit);
  if (freed != False) {
    core.counts.commit_freed += 1;
  } else {
    // RimeGetCommit already advanced the engine queue; the bytes were copied, but the
    // allocation was not released. Report both facts rather than a clean success.
    result.status = SessionCommitStatus::kFreeFailed;
    result.diagnostic = "free_commit refused after the queue was advanced";
  }

  EngineTextCapture text = CaptureEngineText(text_utf8);
  if (!text.ok()) {
    result.status = SessionCommitStatus::kConversionFailed;
    result.diagnostic = "commit conversion: " + text.diagnostic;
    return result;
  }
  result.commit.present = true;
  result.commit.text_utf16_code_units = std::move(text.text.utf16_code_units);
  result.commit.utf8_byte_length = text_utf8.size();
  if (result.status != SessionCommitStatus::kFreeFailed) {
    result.status = SessionCommitStatus::kRead;
  }
  return result;
}

SessionCloseStatus RimeSession::Close() {
  if (!impl_->open) {
    return SessionCloseStatus::kAlreadyClosed;
  }
  if (!impl_->core->IsOwnerThread()) {
    return SessionCloseStatus::kWrongThread;
  }
  if (!impl_->core->initialized) {
    return SessionCloseStatus::kRuntimeNotInitialized;
  }

  impl_->core->counts.destroy_session_calls += 1;
  const Bool destroyed = impl_->core->api->destroy_session(impl_->session_id);
  if (destroyed == False) {
    return SessionCloseStatus::kDestroyFailed;
  }
  impl_->open = false;
  impl_->session_id = 0;
  impl_->core->live_sessions -= 1;
  return SessionCloseStatus::kOk;
}

std::unique_ptr<RimeSession> CreateSessionObject(std::shared_ptr<RuntimeCore> core,
                                                 std::uintptr_t session_id,
                                                 std::string schema_id) {
  return std::unique_ptr<RimeSession>(
      new RimeSession(std::move(core), session_id, std::move(schema_id)));
}

const char* DescribeSessionKeyStatus(SessionKeyStatus status) {
  switch (status) {
    case SessionKeyStatus::kCallMade:
      return "call-made";
    case SessionKeyStatus::kWrongThread:
      return "wrong-thread";
    case SessionKeyStatus::kClosed:
      return "closed";
    case SessionKeyStatus::kRuntimeNotInitialized:
      return "runtime-not-initialized";
  }
  return "unknown-status";
}

const char* DescribeSessionSelectionStatus(SessionSelectionStatus status) {
  switch (status) {
    case SessionSelectionStatus::kCallMade:
      return "call-made";
    case SessionSelectionStatus::kWrongThread:
      return "wrong-thread";
    case SessionSelectionStatus::kClosed:
      return "closed";
    case SessionSelectionStatus::kRuntimeNotInitialized:
      return "runtime-not-initialized";
  }
  return "unknown-status";
}

const char* DescribeSessionStateStatus(SessionStateStatus status) {
  switch (status) {
    case SessionStateStatus::kOk:
      return "ok";
    case SessionStateStatus::kWrongThread:
      return "wrong-thread";
    case SessionStateStatus::kClosed:
      return "closed";
    case SessionStateStatus::kRuntimeNotInitialized:
      return "runtime-not-initialized";
    case SessionStateStatus::kStatusFailed:
      return "status-failed";
    case SessionStateStatus::kContextFailed:
      return "context-failed";
    case SessionStateStatus::kInputFailed:
      return "input-failed";
    case SessionStateStatus::kFreeFailed:
      return "free-failed";
    case SessionStateStatus::kTextConversionFailed:
      return "text-conversion-failed";
  }
  return "unknown-status";
}

const char* DescribeSessionCommitStatus(SessionCommitStatus status) {
  switch (status) {
    case SessionCommitStatus::kNoCommit:
      return "no-commit";
    case SessionCommitStatus::kRead:
      return "read";
    case SessionCommitStatus::kWrongThread:
      return "wrong-thread";
    case SessionCommitStatus::kClosed:
      return "closed";
    case SessionCommitStatus::kRuntimeNotInitialized:
      return "runtime-not-initialized";
    case SessionCommitStatus::kFreeFailed:
      return "free-failed";
    case SessionCommitStatus::kConversionFailed:
      return "conversion-failed";
  }
  return "unknown-status";
}

const char* DescribeSessionCloseStatus(SessionCloseStatus status) {
  switch (status) {
    case SessionCloseStatus::kOk:
      return "ok";
    case SessionCloseStatus::kAlreadyClosed:
      return "already-closed";
    case SessionCloseStatus::kWrongThread:
      return "wrong-thread";
    case SessionCloseStatus::kRuntimeNotInitialized:
      return "runtime-not-initialized";
    case SessionCloseStatus::kDestroyFailed:
      return "destroy-failed";
  }
  return "unknown-status";
}

}  // namespace engine
}  // namespace windows
}  // namespace epocanva
