// EpoCanva P3.4 -- the controller: P3.3's decision, executed exactly once.
//
// DIVISION OF LABOUR.
//   Evaluate/CanDispatch  read the cached EngineState and call the pure P3.3 DecideKey. They
//                         make NO engine call: no process_key, no get_context, no get_commit.
//                         Repeating an evaluation cannot create preedit or commit text.
//   Dispatch              re-runs DecideKey against the CURRENT cache, then calls
//                         Session::ProcessKeyOnce exactly once for an allowed event. The
//                         keysym is taken from P3.3's already-translated result, never
//                         re-derived from a virtual key, and no Shift/Caps conversion is
//                         applied a second time. `repeat_count` is metadata and is never
//                         expanded.
//   AcceptCandidate       one explicit select_candidate call, used to distinguish "the raw
//                         input was committed" from "an explicitly accepted completion was
//                         committed". It is not candidate UI.
//
// KEY MAPPING. Printable ASCII keysyms equal their code points
// (third_party/librime/include/X11/keysymdef.h:528-601 and the file's ASCII table). The four
// special keys use the pinned constants below, transcribed from that header:
//   XK_BackSpace 0xff08 (:124), XK_Return 0xff0d (:128), XK_Escape 0xff1b (:132),
//   XK_space 0x0020 (:527).
// Every mapped key carries modifier mask 0: P3.3 only produces engine candidates for
// unmodified text/special keys, and a shifted letter has already become the uppercase scalar.
//
// RESULTS. DispatchStatus separates:
//   kNotDispatched              no engine key call was made (policy refusal or not ready)
//   kDeferredCompositionBoundary a composition exists and this round refuses to decide the
//                                order; no engine call, the composition is untouched
//   kDispatchedUnhandled        exactly one process_key call returned False; state was read
//   kDispatchedHandled          exactly one process_key call returned True; state was read
//   kDispatchedStateFailed      the call was made, but get_status/get_context/get_input or a
//                                commit read/conversion failed afterwards
// The last value is deliberately not a bool: after the engine has changed, returning "not
// handled" would invite a host to replay the same key.
//
// COMMIT. EngineCommit is a pending event for the host, not proof that a document changed.
// Dispatch calls Session::ReadCommit once, after the state read. Read-only refreshes never
// call get_commit, so an old commit cannot be attached to a later observation.
//
// THREADING. A controller is owned by the runtime's owner thread. Evaluate, Dispatch,
// AcceptCandidate, RefreshState and both setters refuse any other thread; none of them
// silently performs an engine operation there. This is the single-threaded bridge for the
// first integration round, not the final TSF multi-threaded hosting contract.
#ifndef EPOCANVA_WINDOWS_ENGINE_BRIDGE_INPUT_CONTROLLER_H_
#define EPOCANVA_WINDOWS_ENGINE_BRIDGE_INPUT_CONTROLLER_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "engine_update.h"
#include "key_decision.h"
#include "key_event.h"
#include "rime_runtime.h"
#include "rime_session.h"

namespace epocanva {
namespace windows {
namespace engine {

// Pinned keysyms for the four special keys. See the file comment for the header lines.
constexpr int kEngineKeysymBackSpace = 0xff08;
constexpr int kEngineKeysymReturn = 0xff0d;
constexpr int kEngineKeysymEscape = 0xff1b;
constexpr int kEngineKeysymSpace = 0x0020;

struct EngineKey {
  int keysym = 0;     // X11 keysym, not a virtual-key code
  int modifiers = 0;  // librime mask (src/rime/key_table.h); 0 for every candidate here
};

// The one mapping from a P3.3 decision to arguments for RimeApi::process_key. Returns false
// for non-candidates and for an out-of-range text scalar. `decision.ascii_code_point` is
// used as-is; nothing here consults a VK, Shift state or Caps Lock.
bool MapDecisionToEngineKey(const input::KeyDecision& decision, EngineKey* out_key);

enum class ControllerStatus {
  kOk = 0,
  kWrongThread,
  kNoSession,
  kStateUnavailable,  // the cached state is missing; RefreshState may recover it
  kEngineFaulted,     // a commit was consumed but could not be copied; do not dispatch again
};

struct KeyEvaluation {
  ControllerStatus status = ControllerStatus::kNoSession;
  input::KeyDecision decision;
  bool can_dispatch = false;
  bool composition_active = false;
  std::string diagnostic;
};

enum class DispatchStatus {
  kNotDispatched = 0,
  kDeferredCompositionBoundary,
  kDispatchedUnhandled,
  kDispatchedHandled,
  kDispatchedStateFailed,
};

struct DispatchOutcome {
  DispatchStatus status = DispatchStatus::kNotDispatched;
  input::KeyDecision decision;
  bool engine_key_call_made = false;
  bool engine_reported_handled = false;
  SessionStateStatus state_status = SessionStateStatus::kRuntimeNotInitialized;
  SessionCommitStatus commit_status = SessionCommitStatus::kRuntimeNotInitialized;
  EngineState state;          // valid only when state_status == kOk
  EngineCommit pending_commit;  // pending event for the host; not evidence of application
  std::string diagnostic;
};

enum class CandidateStatus {
  kAccepted = 0,
  kRejectedByEngine,  // select_candidate returned False
  kOutOfRange,        // index is not in the cached menu; no engine call
  kNotReady,          // no session/cache, or a previous fault; no engine call
  kWrongThread,
  kStateFailed,  // the selection call happened, but the follow-up read failed
};

struct CandidateOutcome {
  CandidateStatus status = CandidateStatus::kNotReady;
  bool engine_selection_call_made = false;
  bool engine_reported_accepted = false;
  SessionStateStatus state_status = SessionStateStatus::kRuntimeNotInitialized;
  SessionCommitStatus commit_status = SessionCommitStatus::kRuntimeNotInitialized;
  EngineState state;
  EngineCommit pending_commit;
  std::string diagnostic;
};

struct ControllerStats {
  std::uint64_t evaluations = 0;
  std::uint64_t dispatch_requests = 0;
  std::uint64_t engine_key_calls = 0;
  std::uint64_t not_dispatched = 0;
  std::uint64_t deferred_boundary = 0;
  std::uint64_t dispatched_unhandled = 0;
  std::uint64_t dispatched_handled = 0;
  std::uint64_t dispatched_state_failed = 0;
  std::uint64_t selection_requests = 0;
  std::uint64_t selection_calls = 0;
  std::uint64_t selection_out_of_range = 0;
  std::uint64_t state_refreshes = 0;
  std::uint64_t state_refresh_failures = 0;
};

struct ControllerOptions {
  bool input_enabled = true;
  // Conservative default, matching InputContext's own default: an unclassified field must
  // not start a composition just because a caller forgot to set this.
  input::InputPermission permission = input::InputPermission::kUnknown;
};

class InputController {
 public:
  // `session` may be null: that is the "engine not ready" case, and every method then refuses
  // without an engine call. Construction captures one read-only state on the owner thread.
  InputController(RimeRuntime& runtime, RimeSession* session,
                  ControllerOptions options = ControllerOptions());
  ~InputController();

  InputController(const InputController&) = delete;
  InputController& operator=(const InputController&) = delete;

  // Controller-owned context. Refused on a non-owner thread.
  bool SetInputEnabled(bool enabled);
  bool SetPermission(input::InputPermission permission);

  // Pure over the cached state. No process_key, no get_commit, no state read.
  KeyEvaluation Evaluate(const input::KeyEventSnapshot& event, const input::KeyText& text);
  bool CanDispatch(const KeyEvaluation& evaluation) const;

  // Re-evaluates against the current cache, then performs at most one process_key call and
  // one commit read. Never expands repeat_count.
  DispatchOutcome Dispatch(const input::KeyEventSnapshot& event, const input::KeyText& text);

  // One explicit select_candidate call for an index in the last cached menu. The follow-up
  // state/commit read is the same one Dispatch performs.
  CandidateOutcome AcceptCandidate(std::size_t candidate_index);

  // Read-only engine query (get_status/get_context/get_input). It does NOT read the commit
  // queue and does NOT process a key. It is the only way this class replaces its cache.
  ControllerStatus RefreshState();

  const EngineState& cached_state() const { return cached_; }
  const ControllerStats& stats() const { return stats_; }
  bool HasSession() const { return session_ != nullptr; }

 private:
  bool OwnerThread() const;
  ControllerStatus Observe(SessionStateStatus state_status, SessionCommitStatus commit_status,
                           EngineState state, std::string* diagnostic);

  RimeRuntime* runtime_ = nullptr;
  RimeSession* session_ = nullptr;
  ControllerOptions options_;
  EngineState cached_;
  bool cache_valid_ = false;
  bool state_fault_ = false;
  bool commit_fault_ = false;
  ControllerStats stats_;
};

const char* DescribeControllerStatus(ControllerStatus status);
const char* DescribeDispatchStatus(DispatchStatus status);
const char* DescribeCandidateStatus(CandidateStatus status);

}  // namespace engine
}  // namespace windows
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_ENGINE_BRIDGE_INPUT_CONTROLLER_H_
