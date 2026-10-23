// EpoCanva P3.4 -- one librime input session, owned by RimeRuntime and thread-affine.
//
// WHAT THIS IS. A thin, strict wrapper around one `RimeSessionId`: create/select schema,
// exactly one `process_key` call per request, exactly one `select_candidate` call per request,
// and read-only snapshots copied out of `get_status` / `get_context` / `get_input`. The commit
// queue is read only by ReadCommit(), which the controller calls once after an engine call.
//
// WHAT THIS IS NOT. No TSF sink, no host text edit, no candidate UI, no learning, no logging of
// text. A session does not initialize or finalize librime: the service behind all sessions is
// process-global (src/rime/service.cc:196-203), which is exactly why RimeRuntime owns the one
// setup/initialize/finalize and a session cannot.
//
// THREAD RULE. Every method that touches the engine checks the runtime's owner thread and
// returns kWrongThread without making the call. The destructor does the same: destroying a
// session on the wrong thread does NOT call destroy_session. This round is a single-threaded
// bridge, not a claim about TSF's eventual multi-threaded hosting.
//
// OWNERSHIP. `CaptureState()` copies every string before free_status/free_context runs, and
// `ReadCommit()` copies before free_commit. After a call returns, this object holds no pointer
// into rime.dll. `get_input()` is the one returned pointer with no free slot
// (rime_api.h:426-430); it is copied immediately and never retained.
#ifndef EPOCANVA_WINDOWS_ENGINE_BRIDGE_RIME_SESSION_H_
#define EPOCANVA_WINDOWS_ENGINE_BRIDGE_RIME_SESSION_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "engine_update.h"

namespace epocanva {
namespace windows {
namespace engine {

struct RuntimeCore;  // internal
class RimeSession;

// One call to RimeApi::process_key(). `kCallMade` means the API was entered exactly once; the
// Bool it returned is `engine_reported_handled`, not "the key was applied to a document".
enum class SessionKeyStatus {
  kCallMade = 0,
  kWrongThread,
  kClosed,
  kRuntimeNotInitialized,
};

struct SessionKeyResult {
  SessionKeyStatus status = SessionKeyStatus::kRuntimeNotInitialized;
  bool engine_reported_handled = false;
  std::string diagnostic;
  bool engine_call_made() const { return status == SessionKeyStatus::kCallMade; }
};

enum class SessionSelectionStatus {
  kCallMade = 0,
  kWrongThread,
  kClosed,
  kRuntimeNotInitialized,
};

struct SessionSelectionResult {
  SessionSelectionStatus status = SessionSelectionStatus::kRuntimeNotInitialized;
  bool engine_reported_accepted = false;
  std::string diagnostic;
  bool engine_call_made() const { return status == SessionSelectionStatus::kCallMade; }
};

// Why a state snapshot could not be produced. kStatusFailed / kContextFailed / kInputFailed
// name the API slot; kFreeFailed means the matching free_* refused, so the copy is not trusted
// and the API balance is no longer clean. kTextConversionFailed means the engine text was not
// well-formed UTF-8 and P3.2 refused it.
enum class SessionStateStatus {
  kOk = 0,
  kWrongThread,
  kClosed,
  kRuntimeNotInitialized,
  kStatusFailed,
  kContextFailed,
  kInputFailed,
  kFreeFailed,
  kTextConversionFailed,
};

struct SessionStateResult {
  SessionStateStatus status = SessionStateStatus::kRuntimeNotInitialized;
  EngineState state;
  std::string diagnostic;
  bool ok() const { return status == SessionStateStatus::kOk && state.valid; }
};

enum class SessionCommitStatus {
  kNoCommit = 0,  // get_commit returned False while the session was open
  kRead,          // one commit was copied out; the engine queue was advanced
  kWrongThread,
  kClosed,
  kRuntimeNotInitialized,
  kFreeFailed,
  kConversionFailed,  // the bytes were consumed but could not be copied strictly
};

struct SessionCommitResult {
  SessionCommitStatus status = SessionCommitStatus::kRuntimeNotInitialized;
  EngineCommit commit;
  std::string diagnostic;
};

enum class SessionCloseStatus {
  kOk = 0,
  kAlreadyClosed,
  kWrongThread,
  kRuntimeNotInitialized,
  kDestroyFailed,
};

class RimeSession {
 public:
  ~RimeSession();

  RimeSession(const RimeSession&) = delete;
  RimeSession& operator=(const RimeSession&) = delete;

  bool IsOpen() const;
  // The opaque librime id. It is not text and is never logged as content.
  std::uintptr_t session_id() const;
  const std::string& schema_id() const;

  // Exactly one RimeApi::process_key call when the status is kCallMade. `keysym` is an X11
  // keysym (printable ASCII equals its code point) and `modifiers` is librime's own bit set.
  SessionKeyResult ProcessKeyOnce(int keysym, int modifiers);

  // Exactly one RimeApi::select_candidate call. The controller checks the index against the
  // last cached menu first; this method does not add a second policy layer.
  SessionSelectionResult SelectCandidateOnce(std::size_t candidate_index);

  // Read-only: get_status + get_context + get_input. It never calls get_commit, so evaluating
  // or refreshing a snapshot cannot consume a pending commit. It also never calls process_key.
  SessionStateResult CaptureState();

  // Consumes the commit queue exactly once. The engine's own RimeGetCommit clears the queue on
  // success (src/rime_api_impl.h:309-324), and this wrapper calls it only here.
  SessionCommitResult ReadCommit();

  // Destroys the engine session. Refused on the wrong thread; the object then remains open.
  SessionCloseStatus Close();

 private:
  friend class RimeRuntime;
  friend std::unique_ptr<RimeSession> CreateSessionObject(
      std::shared_ptr<RuntimeCore> core, std::uintptr_t session_id,
      std::string schema_id);

  explicit RimeSession(std::shared_ptr<RuntimeCore> core,
                       std::uintptr_t session_id, std::string schema_id);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

const char* DescribeSessionKeyStatus(SessionKeyStatus status);
const char* DescribeSessionSelectionStatus(SessionSelectionStatus status);
const char* DescribeSessionStateStatus(SessionStateStatus status);
const char* DescribeSessionCommitStatus(SessionCommitStatus status);
const char* DescribeSessionCloseStatus(SessionCloseStatus status);

}  // namespace engine
}  // namespace windows
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_ENGINE_BRIDGE_RIME_SESSION_H_
