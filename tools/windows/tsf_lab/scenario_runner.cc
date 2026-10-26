// EpoCanva P3.11 -- implementation of the fixed lab scenarios. See scenario_runner.h.
#include "scenario_runner.h"

#include <cstring>
#include <utility>

namespace epocanva {
namespace tsf {
namespace lab {

namespace {

std::string TextMismatchDetail(const char* label, const std::wstring& actual,
                               const std::wstring& expected, bool suffix_only) {
  std::size_t difference = 0;
  if (!suffix_only) {
    while (difference < actual.size() && difference < expected.size() &&
           actual[difference] == expected[difference]) {
      ++difference;
    }
  } else if (actual.size() >= expected.size()) {
    const std::size_t base = actual.size() - expected.size();
    while (difference < expected.size() &&
           actual[base + difference] == expected[difference]) {
      ++difference;
    }
  }
  std::string detail = std::string(label) + ": document length=" +
                       std::to_string(actual.size()) + " utf16, expected " +
                       (suffix_only ? "suffix" : "value") + " length=" +
                       std::to_string(expected.size()) +
                       ", first " + (suffix_only ? "suffix " : "") +
                       "difference at utf16 offset=" +
                       std::to_string(difference);
  return detail;
}

}  // namespace

ScenarioRunner::ScenarioRunner(LabSession& session, bool use_window_focus)
    : session_(session), use_window_focus_(use_window_focus) {}

ScenarioRunner::~ScenarioRunner() = default;

const char* ScenarioRunner::Name(ScenarioId id) {
  switch (id) {
    case ScenarioId::kComposeHel:
      return "1 compose hel";
    case ScenarioId::kReturnCommit:
      return "2 Return commit";
    case ScenarioId::kHelloSpace:
      return "3 hello + Space";
    case ScenarioId::kBackspaceEscape:
      return "4 BackSpace/Escape";
    case ScenarioId::kAbIsolation:
      return "5 A/B isolation";
    case ScenarioId::kLateAEdit:
      return "6 late A edit";
    case ScenarioId::kReadOnly:
      return "7 read-only";
    case ScenarioId::kExternalEdit:
      return "8 external edit";
    case ScenarioId::kUncertain:
      return "9 uncertain";
  }
  return "unknown";
}

DWORD ScenarioRunner::StepTimeout() const {
  return session_.config().step_timeout_ms == 0
             ? 30000
             : session_.config().step_timeout_ms;
}

void ScenarioRunner::Progress(const std::string& line) {
  if (progress_) {
    progress_(line);
  }
}

ScenarioResult ScenarioRunner::Run(ScenarioId id) {
  if (busy_) {
    return ScenarioResult{ScenarioResult::Status::kBusy,
                          "another scenario is already running"};
  }
  if (cancel_ && cancel_()) {
    return ScenarioResult{ScenarioResult::Status::kCancelled,
                          "cancelled before the scenario started"};
  }
  LabSnapshot snapshot = session_.Snapshot();
  if (!snapshot.ready) {
    return ScenarioResult{ScenarioResult::Status::kRefused,
                          "the lab session is not ready: " +
                              snapshot.session_error};
  }
  if (snapshot.phase != coordinator::WorkPhase::kIdle) {
    return ScenarioResult{
        ScenarioResult::Status::kBusy,
        std::string("coordinator already has work: phase=") +
            coordinator::DescribeWorkPhase(snapshot.phase)};
  }

  busy_ = true;
  struct BusyGuard {
    bool& flag;
    ~BusyGuard() { flag = false; }
  } guard{busy_};

  std::string detail;
  bool ok = false;
  switch (id) {
    case ScenarioId::kComposeHel:
      ok = RunComposeHel(&detail);
      break;
    case ScenarioId::kReturnCommit:
      ok = RunReturnCommit(&detail);
      break;
    case ScenarioId::kHelloSpace:
      ok = RunHelloSpace(&detail);
      break;
    case ScenarioId::kBackspaceEscape:
      ok = RunBackspaceEscape(&detail);
      break;
    case ScenarioId::kAbIsolation:
      ok = RunAbIsolation(&detail);
      break;
    case ScenarioId::kLateAEdit:
      ok = RunLateAEdit(&detail);
      break;
    case ScenarioId::kReadOnly:
      ok = RunReadOnly(&detail);
      break;
    case ScenarioId::kExternalEdit:
      ok = RunExternalEdit(&detail);
      break;
    case ScenarioId::kUncertain:
      ok = RunUncertain(&detail);
      break;
  }
  if (ok) {
    return ScenarioResult{ScenarioResult::Status::kOk, detail};
  }
  return ScenarioResult{Cancelled() ? ScenarioResult::Status::kCancelled
                                    : ScenarioResult::Status::kFailed,
                        detail};
}

bool ScenarioRunner::FocusDocument(int index, std::string* detail) {
  std::string error;
  const bool focused =
      use_window_focus_ ? session_.EnsureDocumentWindowFocus(index, &error)
                        : session_.FocusDocumentProgrammatically(index, &error);
  if (!focused) {
    *detail = std::string("focus/binding failed for document ") +
              (index == 0 ? "A" : "B") + ": " + error;
    return false;
  }
  const LabSnapshot snapshot = session_.Snapshot();
  if (!snapshot.bound) {
    *detail = "the coordinator is not bound after the focus wait";
    return false;
  }
  Progress(std::string("focus: document ") + (index == 0 ? "A" : "B") +
           " ctx=" + std::to_string(snapshot.identity.context_id) +
           " gen=" + std::to_string(snapshot.identity.generation) +
           " path=" + snapshot.last_bind_path +
           " win32=" + std::to_string(snapshot.win32_focus) +
           " tsf=" + std::to_string(snapshot.tsf_focus));
  return true;
}

bool ScenarioRunner::CleanCurrentComposition(std::string* detail) {
  LabSnapshot snapshot = session_.Snapshot();
  if (!snapshot.bound || !snapshot.composition_active) {
    return true;
  }
  if (snapshot.phase != coordinator::WorkPhase::kIdle) {
    *detail = std::string("cannot cancel the current composition: phase=") +
              coordinator::DescribeWorkPhase(snapshot.phase);
    return false;
  }
  int current = -1;
  for (int index = 0; index < 2; ++index) {
    if (snapshot.documents[index].context_id ==
        snapshot.identity.context_id) {
      current = index;
      break;
    }
  }
  if (current < 0) {
    *detail = "the bound context does not match either lab document";
    return false;
  }
  std::wstring expected = snapshot.documents[current].text_utf16;
  const LONG start = snapshot.composition_start_utf16;
  const LONG length = snapshot.composition_length_utf16;
  if (start < 0 || length < 0 ||
      static_cast<std::size_t>(start) + static_cast<std::size_t>(length) >
          expected.size()) {
    *detail = "the coordinator's composition range is outside the document";
    return false;
  }
  expected.erase(static_cast<std::size_t>(start),
                 static_cast<std::size_t>(length));

  coordinator::WorkId id;
  const coordinator::SubmitStatus submitted = session_.SubmitFixedKey(
      current, input::kVirtualKeyEscape, &id);
  if (submitted != coordinator::SubmitStatus::kAccepted) {
    *detail = std::string("cancel Escape refused: ") +
              coordinator::DescribeSubmitStatus(submitted);
    return false;
  }
  coordinator::CoordinatorSnapshot terminal;
  if (!DriveToTerminal("cancel composition", &terminal, detail)) {
    return false;
  }
  if (terminal.outcome != coordinator::WorkOutcome::kApplied &&
      terminal.outcome != coordinator::WorkOutcome::kNoEdit) {
    *detail = std::string("cancel outcome=") +
              coordinator::DescribeWorkOutcome(terminal.outcome);
    return false;
  }
  if (!Refresh(detail)) {
    return false;
  }
  if (!CheckTextEquals(current, expected, "cancel composition", detail)) {
    return false;
  }
  const LabSnapshot after = session_.Snapshot();
  if (after.composition_active) {
    *detail = "the composition is still active after Escape";
    return false;
  }
  Progress("cancelled the previous composition before the scenario");
  return true;
}

bool ScenarioRunner::PrepareDocument(int index, std::string* detail) {
  if (!CleanCurrentComposition(detail)) {
    return false;
  }
  return FocusDocument(index, detail);
}

bool ScenarioRunner::DriveToTerminal(const char* label,
                                     coordinator::CoordinatorSnapshot* terminal,
                                     std::string* detail) {
  const LabSession::DriveResult driven =
      session_.DriveUntilTerminal(StepTimeout());
  if (driven.cancelled) {
    *detail = std::string(label) + ": cancelled";
    return false;
  }
  if (!driven.terminal) {
    *detail = std::string(label) + ": no terminal state (phase=" +
              coordinator::DescribeWorkPhase(driven.final_state.phase) +
              ", timed_out=" + (driven.timed_out ? "1" : "0") + ")";
    return false;
  }
  if (terminal != nullptr) {
    *terminal = driven.final_state;
  }
  if (driven.final_state.outcome != coordinator::WorkOutcome::kApplied) {
    *detail = std::string(label) + ": outcome=" +
              coordinator::DescribeWorkOutcome(driven.final_state.outcome) +
              " cause=" +
              coordinator::DescribeRejectCause(driven.final_state.cause) +
              " edit=" +
              edit::DescribeEditStatus(driven.final_state.last_edit_status) +
              "/" +
              edit::DescribeEditRejectReason(
                  driven.final_state.last_edit_reason);
    return false;
  }
  return true;
}

bool ScenarioRunner::Refresh(std::string* detail) {
  std::string error;
  if (!session_.RefreshEngineState(10000, &error)) {
    *detail = "engine state refresh failed: " + error;
    return false;
  }
  return true;
}

bool ScenarioRunner::TypeText(int index, const char* text, std::string* detail) {
  if (text == nullptr) {
    *detail = "null fixture text";
    return false;
  }
  int count = 0;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (Cancelled()) {
      *detail = "cancelled";
      return false;
    }
    ++count;
    coordinator::WorkId id;
    const coordinator::SubmitStatus submitted = session_.SubmitFixedText(
        index, static_cast<wchar_t>(*cursor), &id);
    if (submitted != coordinator::SubmitStatus::kAccepted) {
      *detail = "letter " + std::to_string(count) + " refused: " +
                coordinator::DescribeSubmitStatus(submitted);
      return false;
    }
    coordinator::CoordinatorSnapshot terminal;
    if (!DriveToTerminal("letter", &terminal, detail)) {
      return false;
    }
    if (!Refresh(detail)) {
      return false;
    }
    const LabSnapshot snapshot = session_.Snapshot();
    Progress("letter " + std::to_string(count) +
             " applied; composition=" +
             (snapshot.composition_active ? "1" : "0") +
             " candidates=" + std::to_string(
                 snapshot.engine_state_valid
                     ? snapshot.engine_state.candidates.size()
                     : 0));
  }
  return true;
}

bool ScenarioRunner::TypeKey(int index, std::uint32_t virtual_key,
                             const char* label, std::string* detail) {
  if (Cancelled()) {
    *detail = "cancelled";
    return false;
  }
  coordinator::WorkId id;
  const coordinator::SubmitStatus submitted =
      session_.SubmitFixedKey(index, virtual_key, &id);
  if (submitted != coordinator::SubmitStatus::kAccepted) {
    *detail = std::string(label) + " refused: " +
              coordinator::DescribeSubmitStatus(submitted);
    return false;
  }
  coordinator::CoordinatorSnapshot terminal;
  if (!DriveToTerminal(label, &terminal, detail)) {
    return false;
  }
  if (!Refresh(detail)) {
    return false;
  }
  Progress(std::string(label) + " applied");
  return true;
}

bool ScenarioRunner::SelectHello(int index, std::string* detail) {
  std::size_t candidate = 0;
  if (!session_.FindCandidate("hello", &candidate)) {
    *detail = "the engine menu does not contain the fixed hello candidate";
    return false;
  }
  coordinator::WorkId id;
  const coordinator::SubmitStatus submitted =
      session_.SubmitCandidate(index, candidate, &id);
  if (submitted != coordinator::SubmitStatus::kAccepted) {
    *detail = std::string("candidate submission refused: ") +
              coordinator::DescribeSubmitStatus(submitted);
    return false;
  }
  coordinator::CoordinatorSnapshot terminal;
  if (!DriveToTerminal("candidate", &terminal, detail)) {
    return false;
  }
  if (!Refresh(detail)) {
    return false;
  }
  Progress("explicit candidate selected: index=" + std::to_string(candidate));
  return true;
}

bool ScenarioRunner::CheckTextEquals(int index, const std::wstring& expected,
                                     const char* label, std::string* detail) {
  const LabSnapshot snapshot = session_.Snapshot();
  if (snapshot.documents[index].text_utf16 == expected) {
    return true;
  }
  *detail = TextMismatchDetail(label, snapshot.documents[index].text_utf16,
                               expected, false);
  return false;
}

bool ScenarioRunner::CheckTextSuffix(int index, const std::wstring& suffix,
                                     const char* label, std::string* detail) {
  const LabSnapshot snapshot = session_.Snapshot();
  const std::wstring& actual = snapshot.documents[index].text_utf16;
  if (actual.size() >= suffix.size() &&
      actual.compare(actual.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return true;
  }
  *detail = TextMismatchDetail(label, actual, suffix, true);
  return false;
}

bool ScenarioRunner::CheckTextUnchanged(int index, const std::wstring& before,
                                        const char* label,
                                        std::string* detail) {
  return CheckTextEquals(index, before, label, detail);
}

// 1. Fixed "hel": the composition grows and the engine menu is visible.
bool ScenarioRunner::RunComposeHel(std::string* detail) {
  if (!PrepareDocument(0, detail)) {
    return false;
  }
  if (!TypeText(0, "hel", detail)) {
    return false;
  }
  if (!CheckTextSuffix(0, L"hel", "compose", detail)) {
    return false;
  }
  const LabSnapshot snapshot = session_.Snapshot();
  if (!snapshot.composition_active) {
    *detail = "compose: the coordinator reports no active composition";
    return false;
  }
  if (!snapshot.engine_state_valid || !snapshot.engine_state.is_composing) {
    *detail = "compose: the engine state is not a live composition";
    return false;
  }
  if (snapshot.engine_state.preedit.utf16_code_units != u"hel") {
    *detail = "compose: preedit length=" +
              std::to_string(snapshot.engine_state.preedit.utf16_code_units.size()) +
              " utf16, expected 3";
    return false;
  }
  if (snapshot.engine_state.candidates.empty()) {
    *detail = "compose: the engine menu is empty";
    return false;
  }
  std::size_t hello_index = 0;
  if (!session_.FindCandidate("hello", &hello_index)) {
    *detail = "compose: the fixed hello candidate is absent";
    return false;
  }
  *detail = "compose complete: composition active, preedit=3 utf16, candidates=" +
            std::to_string(snapshot.engine_state.candidates.size());
  return true;
}

// 2. Default Return commits the original text exactly once.
bool ScenarioRunner::RunReturnCommit(std::string* detail) {
  if (!PrepareDocument(0, detail)) {
    return false;
  }
  const std::wstring before = session_.Snapshot().documents[0].text_utf16;
  if (!TypeText(0, "hel", detail)) {
    return false;
  }
  if (!TypeKey(0, input::kVirtualKeyReturn, "Return", detail)) {
    return false;
  }
  if (!CheckTextEquals(0, before + L"hel", "return", detail)) {
    return false;
  }
  const LabSnapshot snapshot = session_.Snapshot();
  if (snapshot.composition_active) {
    *detail = "return: a composition remains after the commit";
    return false;
  }
  if (snapshot.engine_state_valid && snapshot.engine_state.is_composing) {
    *detail = "return: the engine still reports a composition";
    return false;
  }
  *detail = "return complete: one committed copy, no composition";
  return true;
}

// 3. Explicit hello candidate, then Space: exactly "hello ".
bool ScenarioRunner::RunHelloSpace(std::string* detail) {
  if (!PrepareDocument(0, detail)) {
    return false;
  }
  const std::wstring before = session_.Snapshot().documents[0].text_utf16;
  if (!TypeText(0, "hel", detail)) {
    return false;
  }
  if (!SelectHello(0, detail)) {
    return false;
  }
  if (!CheckTextEquals(0, before + L"hello", "candidate", detail)) {
    return false;
  }
  if (!TypeKey(0, input::kVirtualKeySpace, "Space", detail)) {
    return false;
  }
  if (!CheckTextEquals(0, before + L"hello ", "candidate + Space", detail)) {
    return false;
  }
  const LabSnapshot snapshot = session_.Snapshot();
  if (snapshot.composition_active) {
    *detail = "candidate + Space: a composition remains";
    return false;
  }
  if (snapshot.engine_state_valid && snapshot.engine_state.is_composing) {
    *detail = "candidate + Space: the engine still reports a composition";
    return false;
  }
  *detail = "hello + Space complete: exactly one committed word and one space";
  return true;
}

// 4. BackSpace shrinks the composition; Escape cancels only the composition.
bool ScenarioRunner::RunBackspaceEscape(std::string* detail) {
  if (!PrepareDocument(0, detail)) {
    return false;
  }
  const std::wstring before = session_.Snapshot().documents[0].text_utf16;
  if (!TypeText(0, "he", detail)) {
    return false;
  }
  if (!TypeKey(0, input::kVirtualKeyBack, "BackSpace", detail)) {
    return false;
  }
  if (!CheckTextEquals(0, before + L"h", "BackSpace", detail)) {
    return false;
  }
  if (!session_.Snapshot().composition_active) {
    *detail = "BackSpace: the composition ended instead of shrinking";
    return false;
  }
  if (!TypeKey(0, input::kVirtualKeyEscape, "Escape", detail)) {
    return false;
  }
  if (!CheckTextEquals(0, before, "Escape", detail)) {
    return false;
  }
  const LabSnapshot after_escape = session_.Snapshot();
  if (after_escape.composition_active) {
    *detail = "Escape: the composition is still active";
    return false;
  }
  if (after_escape.engine_state_valid && after_escape.engine_state.is_composing) {
    *detail = "Escape: the engine still reports a composition";
    return false;
  }
  const std::uint64_t works_before = after_escape.works_accepted;
  coordinator::WorkId id;
  const coordinator::SubmitStatus back_without_composition =
      session_.SubmitFixedKey(0, input::kVirtualKeyBack, &id);
  if (back_without_composition != coordinator::SubmitStatus::kNotEngineCandidate) {
    *detail = std::string("BackSpace without a composition was not refused locally: ") +
              coordinator::DescribeSubmitStatus(back_without_composition);
    return false;
  }
  if (session_.Snapshot().works_accepted != works_before) {
    *detail = "BackSpace without a composition created a work item";
    return false;
  }
  *detail = "BackSpace/Escape complete: shrunk, cancelled, no engine work after cancel";
  return true;
}

// 5. A and B have independent bindings and independent documents.
bool ScenarioRunner::RunAbIsolation(std::string* detail) {
  if (!PrepareDocument(0, detail)) {
    return false;
  }
  const std::wstring before_a = session_.Snapshot().documents[0].text_utf16;
  if (!TypeText(0, "ab", detail)) {
    return false;
  }
  // Leave A's composition UNCOMMITTED. The focus switch must end it while preserving the
  // displayed text; it must not commit, cancel or duplicate "ab".
  const LabSnapshot composed = session_.Snapshot();
  if (!composed.owned_composition_active ||
      composed.documents[0].active_compositions != 1) {
    *detail = "A first pass: the live composition is not owned by the session "
              "(owned=" + std::to_string(composed.owned_composition_active ? 1 : 0) +
              " enum=" + std::to_string(composed.documents[0].active_compositions) + ")";
    return false;
  }
  if (!CheckTextEquals(0, before_a + L"ab", "A first pass", detail)) {
    return false;
  }

  if (!FocusDocument(1, detail)) {
    return false;
  }
  const LabSnapshot after_switch = session_.Snapshot();
  if (!CheckTextEquals(0, before_a + L"ab", "A after the focus switch", detail)) {
    return false;
  }
  if (after_switch.documents[0].active_compositions != 0) {
    *detail = "A first pass: the real composition enumeration is still " +
              std::to_string(after_switch.documents[0].active_compositions);
    return false;
  }
  const coordinator::RetirementSnapshot& retired = after_switch.retirement;
  const coordinator::RetirementSnapshot& before_switch = composed.retirement;
  const bool finished_now = retired.finished == before_switch.finished + 1;
  const bool external_now =
      retired.externally_terminated == before_switch.externally_terminated + 1;
  if (finished_now == external_now) {
    *detail = "A first pass: exactly one of Finished/External must increase "
              "(finished=" + std::to_string(before_switch.finished) + "->" +
              std::to_string(retired.finished) + " external=" +
              std::to_string(before_switch.externally_terminated) + "->" +
              std::to_string(retired.externally_terminated) + ")";
    return false;
  }
  if (finished_now) {
    if (retired.last_cleanup_state != coordinator::CleanupState::kFinished ||
        !retired.last_cleanup_plan_submitted || !retired.last_cleanup_end_issued ||
        FAILED(retired.last_cleanup_end_hr) ||
        !retired.last_cleanup_owned_at_start || !retired.last_cleanup_live_at_start ||
        retired.last_cleanup_ended_before_at_start) {
      *detail = "A first pass: Finished is not backed by this cleanup's own end call "
                "(state=" +
                std::string(coordinator::DescribeCleanupState(
                    retired.last_cleanup_state)) +
                " plan=" + std::to_string(retired.last_cleanup_plan_submitted ? 1 : 0) +
                " end=" + std::to_string(retired.last_cleanup_end_issued ? 1 : 0) +
                ")";
      return false;
    }
  } else {
    if (retired.last_cleanup_state !=
            coordinator::CleanupState::kTerminatedExternally ||
        retired.last_cleanup_end_issued || !retired.last_cleanup_owned_at_start ||
        retired.last_cleanup_ended_before_at_start) {
      *detail = "A first pass: external end was not attributed to the external owner "
                "(state=" +
                std::string(coordinator::DescribeCleanupState(
                    retired.last_cleanup_state)) +
                " end=" + std::to_string(retired.last_cleanup_end_issued ? 1 : 0) + ")";
      return false;
    }
  }
  if (retired.settled != before_switch.settled + 1 ||
      retired.blocked != before_switch.blocked ||
      !coordinator::RetirementAccountingBalanced(retired) ||
      !coordinator::RetirementCleanupEvidenceConsistent(retired)) {
    *detail = "A first pass: the cleanup ledger is not one evidence-backed settlement "
              "(settled=" + std::to_string(before_switch.settled) + "->" +
              std::to_string(retired.settled) + " blocked=" +
              std::to_string(before_switch.blocked) + "->" +
              std::to_string(retired.blocked) + ")";
    return false;
  }
  const std::wstring before_b = after_switch.documents[1].text_utf16;
  if (!TypeText(1, "cd", detail)) {
    return false;
  }
  if (!TypeKey(1, input::kVirtualKeyReturn, "B Return", detail)) {
    return false;
  }
  if (!CheckTextEquals(1, before_b + L"cd", "B first pass", detail)) {
    return false;
  }
  if (!CheckTextEquals(0, before_a + L"ab", "A untouched by B", detail)) {
    return false;
  }

  if (!PrepareDocument(0, detail)) {
    return false;
  }
  if (!TypeText(0, "e", detail)) {
    return false;
  }
  if (!TypeKey(0, input::kVirtualKeyReturn, "A second Return", detail)) {
    return false;
  }
  if (!CheckTextEquals(0, before_a + L"abe", "A second pass", detail)) {
    return false;
  }
  if (!CheckTextEquals(1, before_b + L"cd", "B unchanged by A", detail)) {
    return false;
  }
  *detail = "A/B isolation complete: no cross-document write; A's uncommitted "
            "composition ended with its text preserved";
  return true;
}

// 6. A's edit is held before its grant; focus moves to B; the late A callback must not
//    write A or B. This also asserts the runner's and the coordinator's busy refusal.
bool ScenarioRunner::RunLateAEdit(std::string* detail) {
  if (!PrepareDocument(0, detail)) {
    return false;
  }
  const LabSnapshot initial = session_.Snapshot();
  const std::wstring before_a = initial.documents[0].text_utf16;
  const std::wstring before_b = initial.documents[1].text_utf16;

  if (!session_.SetAsyncLocks(0, true)) {
    *detail = "could not enable asynchronous locks on A";
    return false;
  }
  session_.SetHoldGrantMessages(true);

  coordinator::WorkId held_id;
  const coordinator::SubmitStatus held = session_.SubmitFixedText(0, L'q', &held_id);
  if (held != coordinator::SubmitStatus::kAccepted) {
    session_.ReleaseHeldGrantMessages();
    session_.SetHoldGrantMessages(false);
    session_.SetAsyncLocks(0, false);
    *detail = std::string("late A: first submit refused: ") +
              coordinator::DescribeSubmitStatus(held);
    return false;
  }
  const LabSession::DriveResult pending =
      session_.DriveUntilPhase(coordinator::WorkPhase::kEditPending,
                               StepTimeout());
  if (pending.cancelled) {
    session_.ReleaseHeldGrantMessages();
    session_.SetHoldGrantMessages(false);
    session_.SetAsyncLocks(0, false);
    *detail = "late A: cancelled while reaching EditPending";
    return false;
  }
  if (pending.final_state.phase != coordinator::WorkPhase::kEditPending) {
    session_.ReleaseHeldGrantMessages();
    session_.SetHoldGrantMessages(false);
    session_.SetAsyncLocks(0, false);
    *detail = std::string("late A: did not reach EditPending (phase=") +
              coordinator::DescribeWorkPhase(pending.final_state.phase) +
              " outcome=" +
              coordinator::DescribeWorkOutcome(pending.final_state.outcome) + ")";
    return false;
  }
  Progress("late A: edit pending behind a held grant; single-flight check next");

  coordinator::WorkId second;
  const coordinator::SubmitStatus direct_busy =
      session_.SubmitFixedText(0, L'w', &second);
  if (direct_busy != coordinator::SubmitStatus::kBusy) {
    session_.ReleaseHeldGrantMessages();
    session_.SetHoldGrantMessages(false);
    session_.SetAsyncLocks(0, false);
    *detail = std::string("late A: a second direct submit was not Busy: ") +
              coordinator::DescribeSubmitStatus(direct_busy);
    return false;
  }
  const ScenarioResult reentrant = Run(ScenarioId::kReturnCommit);
  if (reentrant.status != ScenarioResult::Status::kBusy) {
    session_.ReleaseHeldGrantMessages();
    session_.SetHoldGrantMessages(false);
    session_.SetAsyncLocks(0, false);
    *detail = "late A: a second scenario was not refused as busy";
    return false;
  }

  if (!FocusDocument(1, detail)) {
    session_.ReleaseHeldGrantMessages();
    session_.SetHoldGrantMessages(false);
    session_.SetAsyncLocks(0, false);
    return false;
  }
  if (!TypeText(1, "z", detail)) {
    session_.ReleaseHeldGrantMessages();
    session_.SetHoldGrantMessages(false);
    session_.SetAsyncLocks(0, false);
    return false;
  }
  if (!TypeKey(1, input::kVirtualKeyReturn, "B Return", detail)) {
    session_.ReleaseHeldGrantMessages();
    session_.SetHoldGrantMessages(false);
    session_.SetAsyncLocks(0, false);
    return false;
  }
  if (!CheckTextEquals(1, before_b + L"z", "late A: B result", detail)) {
    session_.ReleaseHeldGrantMessages();
    session_.SetHoldGrantMessages(false);
    session_.SetAsyncLocks(0, false);
    return false;
  }
  if (!CheckTextEquals(0, before_a, "late A: A before release", detail)) {
    session_.ReleaseHeldGrantMessages();
    session_.SetHoldGrantMessages(false);
    session_.SetAsyncLocks(0, false);
    return false;
  }

  session_.SetHoldGrantMessages(false);
  const std::size_t released = session_.ReleaseHeldGrantMessages();
  for (int pump = 0; pump < 8; ++pump) {
    session_.PumpOnce(0);
  }
  session_.SetAsyncLocks(0, false);
  if (released == 0) {
    *detail = "late A: the held grant was not released";
    return false;
  }
  if (!CheckTextEquals(0, before_a, "late A: A after release", detail)) {
    return false;
  }
  if (!CheckTextEquals(1, before_b + L"z", "late A: B after release", detail)) {
    return false;
  }
  const LabSnapshot after = session_.Snapshot();
  if (!after.bound || after.identity.context_id !=
                          after.documents[1].context_id) {
    *detail = "late A: the coordinator is no longer bound to B";
    return false;
  }
  if (after.binding.target_losses == 0) {
    *detail = "late A: no observer target loss was recorded";
    return false;
  }
  if (after.observer.last_target_lost_reason ==
      context::ContextDropReason::kNone) {
    *detail = "late A: the observer did not name a target-loss reason";
    return false;
  }
  Progress("late A: released grant; A unchanged; B unchanged; target_losses=" +
           std::to_string(after.binding.target_losses));
  *detail = "late A complete: old callback refused, B unaffected, single flight held";
  return true;
}

// 7. A read-only document is denied by real metadata before any engine dispatch.
bool ScenarioRunner::RunReadOnly(std::string* detail) {
  if (!PrepareDocument(0, detail)) {
    return false;
  }
  const LabSnapshot before = session_.Snapshot();
  const std::wstring before_text = before.documents[0].text_utf16;
  if (!session_.SetReadOnly(0, true)) {
    *detail = "could not set the real read-only store state";
    return false;
  }
  coordinator::WorkId id;
  const coordinator::SubmitStatus submitted =
      session_.SubmitFixedText(0, L'r', &id);
  bool denied = false;
  if (submitted == coordinator::SubmitStatus::kPermissionDenied) {
    denied = true;
  } else if (submitted == coordinator::SubmitStatus::kAccepted) {
    coordinator::CoordinatorSnapshot terminal;
    const LabSession::DriveResult driven =
        session_.DriveUntilTerminal(StepTimeout());
    terminal = driven.final_state;
    if (!driven.terminal) {
      session_.SetReadOnly(0, false);
      *detail = "read-only: the accepted work did not terminate";
      return false;
    }
    if (terminal.outcome == coordinator::WorkOutcome::kRejected &&
        terminal.cause == coordinator::RejectCause::kPermissionDenied) {
      denied = true;
    } else {
      session_.SetReadOnly(0, false);
      *detail = std::string("read-only: outcome=") +
                coordinator::DescribeWorkOutcome(terminal.outcome) +
                " cause=" + coordinator::DescribeRejectCause(terminal.cause);
      return false;
    }
  } else {
    session_.SetReadOnly(0, false);
    *detail = std::string("read-only: submit status=") +
              coordinator::DescribeSubmitStatus(submitted);
    return false;
  }

  const LabSnapshot after = session_.Snapshot();
  session_.SetReadOnly(0, false);
  if (!denied) {
    *detail = "read-only: the work was not denied";
    return false;
  }
  if (after.permission != edit::InputPermission::kDenied) {
    *detail = std::string("read-only: cached permission=") +
              edit::DescribeInputPermission(after.permission);
    return false;
  }
  if (!CheckTextUnchanged(0, before_text, "read-only", detail)) {
    return false;
  }
  if (after.documents[0].text_mutations !=
      before.documents[0].text_mutations) {
    *detail = "read-only: the store mutated " +
              std::to_string(after.documents[0].text_mutations -
                             before.documents[0].text_mutations) +
              " times";
    return false;
  }
  if (after.engine_results_taken != before.engine_results_taken) {
    *detail = "read-only: an engine result was consumed";
    return false;
  }
  if (submitted == coordinator::SubmitStatus::kPermissionDenied &&
      after.works_accepted != before.works_accepted) {
    *detail = "read-only: a refused key created a work item";
    return false;
  }
  *detail = "read-only complete: permission=Denied, no engine result, no mutation";
  return true;
}

// 8. A real external edit (different TfClientId) invalidates the old binding; the lab
//    re-binds with a new generation and subsequent input lands normally.
bool ScenarioRunner::RunExternalEdit(std::string* detail) {
  if (!PrepareDocument(1, detail)) {
    return false;
  }
  const LabSnapshot before = session_.Snapshot();
  const std::wstring before_text = before.documents[1].text_utf16;
  const std::uint64_t before_generation = before.identity.generation;
  const std::uint64_t before_external = before.observer.external_edits;
  std::string error;
  if (!session_.InsertExternalText(1, L"X", &error)) {
    *detail = "external edit failed: " + error;
    return false;
  }
  if (!session_.WaitForRebind(1, before_generation + 1, StepTimeout(), &error)) {
    *detail = "external edit: no rebind: " + error;
    return false;
  }
  LabSnapshot after = session_.Snapshot();
  if (after.identity.context_id != before.identity.context_id) {
    *detail = "external edit: the context identity changed";
    return false;
  }
  if (after.identity.generation <= before_generation) {
    *detail = "external edit: generation did not advance";
    return false;
  }
  if (after.observer.external_edits <= before_external) {
    *detail = "external edit: the observer did not count an external edit";
    return false;
  }
  if (after.observer.last_target_lost_reason !=
      context::ContextDropReason::kExternalTextEdit) {
    *detail = std::string("external edit: loss reason=") +
              context::DescribeContextDropReason(
                  after.observer.last_target_lost_reason);
    return false;
  }
  if (after.binding.invalidations == 0) {
    *detail = "external edit: the controller did not invalidate the old binding";
    return false;
  }
  if (after.documents[1].text_utf16.size() != before_text.size() + 1) {
    *detail = "external edit: document length=" +
              std::to_string(after.documents[1].text_utf16.size()) +
              " utf16, expected " + std::to_string(before_text.size() + 1);
    return false;
  }
  Progress("external edit: generation " + std::to_string(before_generation) +
           " -> " + std::to_string(after.identity.generation) +
           "; external_edits=" + std::to_string(after.observer.external_edits) +
           "; loss=ExternalTextEdit");

  if (!TypeText(1, "m", detail)) {
    return false;
  }
  if (!TypeKey(1, input::kVirtualKeyReturn, "B Return after external edit",
               detail)) {
    return false;
  }
  const LabSnapshot final = session_.Snapshot();
  if (final.identity.generation != after.identity.generation) {
    *detail = "external edit: subsequent input rebound the context";
    return false;
  }
  if (!CheckTextEquals(1, before_text + L"Xm", "external edit: subsequent input",
                       detail)) {
    return false;
  }
  *detail = "external edit complete: old generation invalidated, new one typed";
  return true;
}

// 9. An unconfirmed write latches Uncertain and explicitly blocks further input.
bool ScenarioRunner::RunUncertain(std::string* detail) {
  if (!PrepareDocument(1, detail)) {
    return false;
  }
  if (!session_.SetMutationFault(1,
                                 edit::test::MutationFault::kEFailAfterMutation)) {
    *detail = "could not install the host mutation fault";
    return false;
  }
  coordinator::WorkId id;
  const coordinator::SubmitStatus submitted = session_.SubmitFixedText(1, L'u', &id);
  if (submitted != coordinator::SubmitStatus::kAccepted) {
    session_.SetMutationFault(1, edit::test::MutationFault::kNone);
    *detail = std::string("uncertain: submit refused: ") +
              coordinator::DescribeSubmitStatus(submitted);
    return false;
  }
  const LabSession::DriveResult driven =
      session_.DriveUntilTerminal(StepTimeout());
  if (driven.cancelled) {
    session_.SetMutationFault(1, edit::test::MutationFault::kNone);
    *detail = "uncertain: cancelled";
    return false;
  }
  if (!driven.terminal) {
    session_.SetMutationFault(1, edit::test::MutationFault::kNone);
    *detail = "uncertain: the work did not terminate";
    return false;
  }
  if (driven.final_state.outcome != coordinator::WorkOutcome::kUncertain) {
    session_.SetMutationFault(1, edit::test::MutationFault::kNone);
    *detail = std::string("uncertain: outcome=") +
              coordinator::DescribeWorkOutcome(driven.final_state.outcome) +
              " cause=" +
              coordinator::DescribeRejectCause(driven.final_state.cause);
    return false;
  }
  if (driven.final_state.gate !=
      coordinator::CoordinatorGate::kBlockedUncertain) {
    session_.SetMutationFault(1, edit::test::MutationFault::kNone);
    *detail = std::string("uncertain: gate=") +
              coordinator::DescribeCoordinatorGate(driven.final_state.gate);
    return false;
  }

  const LabSnapshot after_uncertain = session_.Snapshot();
  const std::uint64_t works_after = after_uncertain.works_accepted;
  const std::uint64_t mutations_after =
      after_uncertain.documents[1].text_mutations;
  const std::uint64_t length_after =
      after_uncertain.documents[1].text_length_utf16;
  coordinator::WorkId second;
  const coordinator::SubmitStatus blocked =
      session_.SubmitFixedText(1, L'v', &second);
  session_.SetMutationFault(1, edit::test::MutationFault::kNone);
  if (blocked != coordinator::SubmitStatus::kBlockedUncertain) {
    *detail = std::string("uncertain: the next submit was not blocked: ") +
              coordinator::DescribeSubmitStatus(blocked);
    return false;
  }
  const LabSnapshot after_block = session_.Snapshot();
  if (after_block.works_accepted != works_after) {
    *detail = "uncertain: the blocked submit created a work item";
    return false;
  }
  if (after_block.documents[1].text_mutations != mutations_after ||
      after_block.documents[1].text_length_utf16 != length_after) {
    *detail = "uncertain: the blocked submit changed the document";
    return false;
  }
  Progress("uncertain: outcome=Uncertain gate=BlockedUncertain; next submit "
           "blocked; no further mutation");
  *detail = "uncertain complete: explicit blocked state, no further input";
  return true;
}

}  // namespace lab
}  // namespace tsf
}  // namespace epocanva
