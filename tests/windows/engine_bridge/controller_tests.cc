// EpoCanva P3.4 -- controller verification: real Windows translation, P3.3 decisions, one
// engine call per allowed event, and the result copies/statuses they produce.
//
// All text printed here comes from the fixed fixtures under tests/fixtures/. The program never
// types into a host application, never registers a TSF sink and never edits a document.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "input_controller.h"
#include "key_decision.h"
#include "key_event.h"
#include "key_text_translation.h"
#include "keyboard_state.h"
#include "test_support.h"

namespace epocanva {
namespace tests {
namespace engine_bridge {

using epocanva::windows::engine::CandidateOutcome;
using epocanva::windows::engine::CandidateStatus;
using epocanva::windows::engine::ControllerOptions;
using epocanva::windows::engine::ControllerStatus;
using epocanva::windows::engine::DescribeCandidateStatus;
using epocanva::windows::engine::DescribeControllerStatus;
using epocanva::windows::engine::DescribeDispatchStatus;
using epocanva::windows::engine::DispatchOutcome;
using epocanva::windows::engine::DispatchStatus;
using epocanva::windows::engine::EngineKey;
using epocanva::windows::engine::EngineState;
using epocanva::windows::engine::InputController;
using epocanva::windows::engine::kEngineKeysymBackSpace;
using epocanva::windows::engine::kEngineKeysymEscape;
using epocanva::windows::engine::kEngineKeysymReturn;
using epocanva::windows::engine::kEngineKeysymSpace;
using epocanva::windows::engine::KeyEvaluation;
using epocanva::windows::engine::MapDecisionToEngineKey;
using epocanva::windows::engine::RimeRuntime;
using epocanva::windows::engine::RuntimeShutdownStatus;
using epocanva::windows::engine::RuntimeStartStatus;
using epocanva::windows::engine::SessionCloseStatus;
using epocanva::windows::engine::SessionCommitResult;
using epocanva::windows::engine::SessionCommitStatus;
using epocanva::windows::engine::SessionCreateResult;
using epocanva::windows::engine::SessionCreateStatus;
using epocanva::windows::engine::SessionStateResult;
using epocanva::windows::engine::SessionStateStatus;
using epocanva::windows::input::DescribeKeyDecisionReason;
using epocanva::windows::input::DescribeKeyDisposition;
using epocanva::windows::input::InputPermission;
using epocanva::windows::input::kKeyboardStateByteCount;
using epocanva::windows::input::kVirtualKeyBack;
using epocanva::windows::input::kVirtualKeyEscape;
using epocanva::windows::input::kVirtualKeyReturn;
using epocanva::windows::input::kVirtualKeySpace;
using epocanva::windows::input::KeyDecision;
using epocanva::windows::input::KeyDecisionReason;
using epocanva::windows::input::KeyDisposition;
using epocanva::windows::input::KeyEventSnapshot;
using epocanva::windows::input::KeyText;
using epocanva::windows::input::KeyTransition;
using epocanva::windows::input::KeyboardStateSnapshot;
using epocanva::windows::input::KeyTranslationResult;
using epocanva::windows::input::TranslateKeyEvent;

namespace {

struct RunEnvironment {
  std::filesystem::path shared_dir;
  std::filesystem::path user_dir;
  std::filesystem::path log_dir;
  std::filesystem::path empty_user_dir;
  std::filesystem::path empty_log_dir;
  RimeRuntime runtime;
};

bool BringUpRuntime(const Options& options, RunEnvironment* env) {
  if (!PrepareRunRoot(options, "controller", &env->shared_dir, &env->user_dir,
                      &env->log_dir, &env->empty_user_dir, &env->empty_log_dir) ||
      !StageFixtures(options, env->shared_dir)) {
    Check(false, "fixture preparation");
    return false;
  }
  if (!PrepareDeployment(options, env->shared_dir, env->user_dir, env->log_dir)) {
    Check(false, "deployment preparation");
    return false;
  }
  const RuntimeStartStatus started =
      env->runtime.Start(BaseOptions(options, env->shared_dir, env->user_dir,
                                    env->log_dir));
  Check(started == RuntimeStartStatus::kOk, "runtime starts",
        epocanva::windows::engine::DescribeRuntimeStartStatus(started));
  if (started != RuntimeStartStatus::kOk) {
    std::printf("[engine-win] runtime diagnostic: %s\n",
                env->runtime.Info().last_start_diagnostic.c_str());
    return false;
  }
  return true;
}

void ShutDownRuntime(RunEnvironment* env, const std::string& label) {
  const RuntimeShutdownStatus stopped = env->runtime.Shutdown();
  Check(stopped == RuntimeShutdownStatus::kOk, label + ": runtime shuts down",
        epocanva::windows::engine::DescribeRuntimeShutdownStatus(stopped));
  CheckBalance(env->runtime.ApiCounts(), label);
}

struct ControllerFixture {
  SessionCreateResult session;
  std::unique_ptr<InputController> controller;

  bool Start(RimeRuntime& runtime, const std::string& schema) {
    session = runtime.CreateSession(schema);
    Check(session.ok(), "session created with " + schema,
          std::string(DescribeSessionCreateStatus(session.status)) + ": " +
              session.diagnostic);
    if (!session.ok()) {
      return false;
    }
    controller.reset(new InputController(
        runtime, session.session.get(),
        ControllerOptions{true, InputPermission::kAllowed}));
    return true;
  }

  void Close() {
    controller.reset();
    if (session.ok() && session.session->IsOpen()) {
      const SessionCloseStatus status = session.session->Close();
      Check(status == SessionCloseStatus::kOk, "session closes",
            DescribeSessionCloseStatus(status));
    }
  }
};

HKL CurrentLayout() { return ::GetKeyboardLayout(0); }

std::string CurrentLayoutKlid() {
  wchar_t name[KL_NAMELENGTH] = {};
  if (::GetKeyboardLayoutNameW(name) == FALSE) {
    return "unknown";
  }
  return AsciiOf(std::wstring(name));
}

struct TextKey {
  bool ok = false;
  KeyEventSnapshot event;
  KeyText text;
  std::string diagnostic;
};

// Build a controlled 256-byte state from the layout's own VkKeyScanExW answer, then translate
// through P3.3's real ToUnicodeEx adapter. Nothing sends a key or changes a layout.
TextKey MakeTextKey(wchar_t character, HKL layout) {
  TextKey out;
  const SHORT scan = ::VkKeyScanExW(character, layout);
  if (scan == -1) {
    out.diagnostic = "the current layout cannot type this fixture character";
    return out;
  }
  const BYTE virtual_key = static_cast<BYTE>(scan & 0xFF);
  const BYTE shift_state = static_cast<BYTE>((scan >> 8) & 0xFF);
  if ((shift_state & 0x06u) != 0) {
    out.diagnostic = "the fixture character needs Ctrl/Alt on this layout";
    return out;
  }
  std::array<std::uint8_t, kKeyboardStateByteCount> bytes{};
  if ((shift_state & 0x01u) != 0) {
    bytes[VK_SHIFT] = 0x80;
  }
  const KeyboardStateSnapshot snapshot =
      KeyboardStateSnapshot::FromExplicitState(bytes, layout);

  out.event.virtual_key = virtual_key;
  out.event.scan_code = ::MapVirtualKeyExW(virtual_key, MAPVK_VK_TO_VSC, layout);
  out.event.is_extended = false;
  out.event.transition = KeyTransition::kKeyDown;
  out.event.repeat_count = 1;
  out.event.previously_down = false;
  out.event.modifiers = snapshot.modifiers();

  const KeyTranslationResult result = TranslateKeyEvent(out.event, snapshot);
  out.text = result.text;
  out.ok = result.ok_for_text();
  out.diagnostic = result.diagnostic;
  return out;
}

bool RequireTextKey(const TextKey& key, char expected, const std::string& label) {
  Check(key.ok, label + ": real ToUnicodeEx produced text", key.diagnostic);
  if (!key.ok) {
    return false;
  }
  CheckText(key.text.utf16_code_units, std::string(1, expected),
            label + ": translated character is exact");
  return true;
}

KeyEventSnapshot MakeKeyEvent(std::uint32_t virtual_key, bool shift, bool control,
                              bool alt, bool windows,
                              KeyTransition transition = KeyTransition::kKeyDown) {
  KeyEventSnapshot event;
  event.virtual_key = virtual_key;
  event.scan_code = ::MapVirtualKeyW(virtual_key, MAPVK_VK_TO_VSC);
  event.transition = transition;
  event.repeat_count = 1;
  event.previously_down = false;
  event.modifiers.shift = shift;
  event.modifiers.control = control;
  event.modifiers.alt = alt;
  event.modifiers.windows = windows;
  return event;
}

void CheckMappedKey(const KeyDecision& decision, int expected_keysym,
                    const std::string& label) {
  EngineKey key;
  const bool mapped = MapDecisionToEngineKey(decision, &key);
  Check(mapped, label + ": decision maps to an engine key");
  if (!mapped) {
    return;
  }
  Check(key.keysym == expected_keysym, label + ": keysym is exact",
        std::to_string(key.keysym));
  Check(key.modifiers == 0, label + ": modifier mask is zero",
        std::to_string(key.modifiers));
}

void CheckCandidateList(const EngineState& state,
                        const std::vector<std::string>& expected,
                        const std::string& label) {
  Check(state.candidates.size() == expected.size(),
        label + ": candidate count is exact",
        CountText(state.candidates.size()) + " vs " + CountText(expected.size()));
  const std::size_t common = (std::min)(state.candidates.size(), expected.size());
  for (std::size_t i = 0; i < common; ++i) {
    CheckText(state.candidates[i].text_utf16_code_units, expected[i],
              label + ": candidate " + CountText(i) + " text");
  }
}

KeyEventSnapshot SpaceEvent() {
  return MakeKeyEvent(kVirtualKeySpace, false, false, false, false);
}

KeyEventSnapshot ReturnEvent() {
  return MakeKeyEvent(kVirtualKeyReturn, false, false, false, false);
}

KeyEventSnapshot EscapeEvent() {
  return MakeKeyEvent(kVirtualKeyEscape, false, false, false, false);
}

KeyEventSnapshot BackSpaceEvent() {
  return MakeKeyEvent(kVirtualKeyBack, false, false, false, false);
}

}  // namespace

void RunDispatchMode(const Options& options) {
  RunEnvironment env;
  if (!BringUpRuntime(options, &env)) {
    return;
  }
  const HKL layout = CurrentLayout();
  Check(layout != nullptr, "the thread has a keyboard layout");
  std::printf("[engine-win] layout %s\n", CurrentLayoutKlid().c_str());
  if (layout == nullptr) {
    ShutDownRuntime(&env, "dispatch");
    return;
  }

  const TextKey h = MakeTextKey(L'h', layout);
  const TextKey e = MakeTextKey(L'e', layout);
  const TextKey l = MakeTextKey(L'l', layout);
  const TextKey H = MakeTextKey(L'H', layout);
  const TextKey a = MakeTextKey(L'a', layout);
  const TextKey p = MakeTextKey(L'p', layout);
  if (!RequireTextKey(h, 'h', "h") || !RequireTextKey(e, 'e', "e") ||
      !RequireTextKey(l, 'l', "l") || !RequireTextKey(H, 'H', "H") ||
      !RequireTextKey(a, 'a', "a") || !RequireTextKey(p, 'p', "p")) {
    ShutDownRuntime(&env, "dispatch");
    return;
  }

  std::uint64_t controller_key_calls = 0;

  // 3. Repeated Evaluate leaves the engine alone; one Dispatch moves it once.
  {
    ControllerFixture f;
    if (!f.Start(env.runtime, "epocanva_test_english")) {
      ShutDownRuntime(&env, "dispatch");
      return;
    }
    const std::uint64_t calls_before = env.runtime.ApiCounts().process_key_calls;
    const std::uint64_t refreshes_before = f.controller->stats().state_refreshes;
    const KeyEvaluation first = f.controller->Evaluate(h.event, h.text);
    const KeyEvaluation second = f.controller->Evaluate(h.event, h.text);
    const KeyEvaluation third = f.controller->Evaluate(h.event, h.text);
    Check(first.status == ControllerStatus::kOk, "Evaluate h succeeds",
          DescribeControllerStatus(first.status));
    Check(first.decision == second.decision && second.decision == third.decision,
          "three evaluations of h return the same decision");
    Check(first.decision.disposition == KeyDisposition::kEngineTextCandidate,
          "h is a text candidate", DescribeKeyDisposition(first.decision.disposition));
    Check(f.controller->CanDispatch(first), "h can be dispatched");
    Check(f.controller->stats().engine_key_calls == 0,
          "Evaluate made no engine key call",
          CountText(f.controller->stats().engine_key_calls));
    Check(env.runtime.ApiCounts().process_key_calls == calls_before,
          "Evaluate made no process_key call in the API witness",
          CountText(env.runtime.ApiCounts().process_key_calls));
    Check(f.controller->stats().state_refreshes == refreshes_before,
          "Evaluate made no state query either",
          CountText(f.controller->stats().state_refreshes));
    const SessionStateResult before_dispatch = f.session.session->CaptureState();
    Check(before_dispatch.ok() && before_dispatch.state.raw_input.utf16_code_units.empty(),
          "three evaluations left the engine text empty");

    const DispatchOutcome dispatched = f.controller->Dispatch(h.event, h.text);
    Check(dispatched.status == DispatchStatus::kDispatchedHandled,
          "one Dispatch of h is dispatched-handled",
          DescribeDispatchStatus(dispatched.status));
    Check(dispatched.engine_key_call_made, "h dispatch made the engine call");
    Check(dispatched.engine_reported_handled, "h dispatch was reported handled");
    Check(dispatched.state.valid, "h dispatch returned a valid state");
    CheckText(dispatched.state.raw_input.utf16_code_units, "h",
              "h dispatch left raw input h");
    CheckText(dispatched.state.preedit.utf16_code_units, "h",
              "h dispatch left preedit h");
    Check(dispatched.state.candidates.size() >= 1 &&
              dispatched.state.candidates[0].text_utf16_code_units == U16("h"),
          "h menu candidate 0 is the raw input");
    Check(!dispatched.pending_commit.present, "h dispatch produced no commit");
    Check(f.controller->stats().engine_key_calls == 1,
          "exactly one engine key call for one Dispatch",
          CountText(f.controller->stats().engine_key_calls));
    controller_key_calls += f.controller->stats().engine_key_calls;
    f.Close();
  }

  // 4. Real Windows-translated h/e/l build the composition and the fixture menu.
  {
    ControllerFixture f;
    if (!f.Start(env.runtime, "epocanva_test_english")) {
      ShutDownRuntime(&env, "dispatch");
      return;
    }
    for (const TextKey* key : {&h, &e, &l}) {
      const DispatchOutcome outcome = f.controller->Dispatch(key->event, key->text);
      Check(outcome.status == DispatchStatus::kDispatchedHandled,
            "h/e/l dispatch is handled", DescribeDispatchStatus(outcome.status));
    }
    const SessionStateResult state = f.session.session->CaptureState();
    Check(state.ok(), "state after h/e/l", DescribeSessionStateStatus(state.status));
    if (state.ok()) {
      CheckText(state.state.raw_input.utf16_code_units, "hel", "raw input after h/e/l");
      CheckText(state.state.preedit.utf16_code_units, "hel", "preedit after h/e/l");
      Check(state.state.is_composing, "h/e/l left a composition");
      Check(state.state.cursor_status ==
                    epocanva::windows::engine::CursorOffsetStatus::kMapped &&
                state.state.cursor_utf16_code_unit_offset == 3,
            "preedit cursor maps to 3 UTF-16 units");
      Check(state.state.selection_status ==
                    epocanva::windows::engine::SelectionOffsetStatus::kMapped &&
                state.state.selection_start_utf16_code_unit_offset == 0 &&
                state.state.selection_end_utf16_code_unit_offset == 3,
            "preedit selection maps to [0,3)");
      Check(state.state.page_size == 8 && state.state.page_no == 0 &&
                state.state.is_last_page &&
                state.state.highlighted_candidate_index_on_page == 0,
            "menu page reports page 0 of 1 with index 0 highlighted");
      CheckCandidateList(state.state, {"hel", "hello", "help", "helmet"}, "hel");
      for (const auto& candidate : state.state.candidates) {
        Check(candidate.comment_utf16_code_units.empty(),
              "completion candidate carries no comment");
      }
    }
    Check(f.controller->stats().engine_key_calls == 3,
          "three letters made three engine key calls",
          CountText(f.controller->stats().engine_key_calls));
    controller_key_calls += f.controller->stats().engine_key_calls;
    f.Close();
  }

  // 5. Uppercase arrives already translated; it is not shifted a second time.
  {
    ControllerFixture f;
    if (!f.Start(env.runtime, "epocanva_test_english")) {
      ShutDownRuntime(&env, "dispatch");
      return;
    }
    const KeyEvaluation upper = f.controller->Evaluate(H.event, H.text);
    Check(upper.status == ControllerStatus::kOk, "Evaluate H succeeds",
          DescribeControllerStatus(upper.status));
    Check(upper.decision.ascii_code_point == 0x48,
          "H decision carries U+0048", std::to_string(upper.decision.ascii_code_point));
    CheckMappedKey(upper.decision, 0x48, "H");
    const DispatchOutcome first = f.controller->Dispatch(H.event, H.text);
    Check(first.status == DispatchStatus::kDispatchedHandled, "H dispatch is handled",
          DescribeDispatchStatus(first.status));
    CheckText(first.state.raw_input.utf16_code_units, "H", "raw input is exactly H");
    CheckText(first.state.preedit.utf16_code_units, "H", "preedit is exactly H");
    const DispatchOutcome second = f.controller->Dispatch(e.event, e.text);
    Check(second.status == DispatchStatus::kDispatchedHandled, "e after H is handled",
          DescribeDispatchStatus(second.status));
    CheckText(second.state.raw_input.utf16_code_units, "He", "raw input is He, not he");
    Check(second.state.candidates.size() >= 2, "He has a completion");
    if (second.state.candidates.size() >= 2) {
      CheckText(second.state.candidates[1].text_utf16_code_units, "Hello",
                "initial-capital completion is Hello");
    }
    Check(f.controller->stats().engine_key_calls == 2,
          "uppercase and lowercase made two calls",
          CountText(f.controller->stats().engine_key_calls));
    controller_key_calls += f.controller->stats().engine_key_calls;
    f.Close();
  }

  // 6. BackSpace / Escape / Enter / Space keep the engine semantics P2.5 and fluid_editor own.
  {
    ControllerFixture f;
    if (!f.Start(env.runtime, "epocanva_test_english")) {
      ShutDownRuntime(&env, "dispatch");
      return;
    }
    for (const TextKey* key : {&h, &e, &l}) {
      (void)f.controller->Dispatch(key->event, key->text);
    }
    const KeyEventSnapshot backspace = BackSpaceEvent();
    const KeyEventSnapshot escape = EscapeEvent();
    const KeyEventSnapshot enter = ReturnEvent();
    const KeyEventSnapshot space = SpaceEvent();
    CheckMappedKey(f.controller->Evaluate(backspace, KeyText{}).decision,
                   kEngineKeysymBackSpace, "BackSpace");
    CheckMappedKey(f.controller->Evaluate(escape, KeyText{}).decision,
                   kEngineKeysymEscape, "Escape");
    CheckMappedKey(f.controller->Evaluate(enter, KeyText{}).decision,
                   kEngineKeysymReturn, "Enter");
    CheckMappedKey(f.controller->Evaluate(space, KeyText{}).decision,
                   kEngineKeysymSpace, "Space");

    const DispatchOutcome back = f.controller->Dispatch(backspace, KeyText{});
    Check(back.status == DispatchStatus::kDispatchedHandled, "BackSpace is handled",
          DescribeDispatchStatus(back.status));
    CheckText(back.state.raw_input.utf16_code_units, "he", "BackSpace removed l");
    Check(!back.pending_commit.present, "BackSpace produced no commit");

    (void)f.controller->Dispatch(l.event, l.text);
    const DispatchOutcome esc = f.controller->Dispatch(escape, KeyText{});
    Check(esc.status == DispatchStatus::kDispatchedHandled, "Escape is handled",
          DescribeDispatchStatus(esc.status));
    Check(esc.state.raw_input.utf16_code_units.empty(), "Escape cleared the raw input");
    Check(esc.state.preedit.utf16_code_units.empty(), "Escape cleared the preedit");
    Check(!esc.state.is_composing, "Escape ended the composition");
    Check(!esc.pending_commit.present, "Escape produced no commit");

    for (const TextKey* key : {&h, &e, &l}) {
      (void)f.controller->Dispatch(key->event, key->text);
    }
    const DispatchOutcome committed = f.controller->Dispatch(enter, KeyText{});
    Check(committed.status == DispatchStatus::kDispatchedHandled, "Enter is handled",
          DescribeDispatchStatus(committed.status));
    Check(committed.pending_commit.present, "Enter produced a commit");
    CheckText(committed.pending_commit.text_utf16_code_units, "hel",
              "Enter committed the raw input hel");
    Check(!committed.state.is_composing, "Enter ended the composition");

    for (const TextKey* key : {&h, &e, &l}) {
      (void)f.controller->Dispatch(key->event, key->text);
    }
    const DispatchOutcome spaced = f.controller->Dispatch(space, KeyText{});
    Check(spaced.status == DispatchStatus::kDispatchedHandled, "Space is handled",
          DescribeDispatchStatus(spaced.status));
    Check(spaced.pending_commit.present, "Space produced a commit");
    CheckText(spaced.pending_commit.text_utf16_code_units, "hel ",
              "Space committed raw input plus exactly one space");
    const DispatchOutcome empty_space = f.controller->Dispatch(space, KeyText{});
    Check(empty_space.status == DispatchStatus::kDispatchedHandled,
          "Space with no composition is handled",
          DescribeDispatchStatus(empty_space.status));
    CheckText(empty_space.pending_commit.text_utf16_code_units, " ",
              "Space with no composition committed exactly one space");

    const std::uint64_t calls_before_refusals =
        env.runtime.ApiCounts().process_key_calls;
    const DispatchOutcome enter_no_composition =
        f.controller->Dispatch(enter, KeyText{});
    Check(enter_no_composition.status == DispatchStatus::kNotDispatched,
          "Enter without a composition is not dispatched",
          DescribeDispatchStatus(enter_no_composition.status));
    Check(enter_no_composition.decision.reason == KeyDecisionReason::kNoComposition,
          "Enter without a composition reports no-composition",
          DescribeKeyDecisionReason(enter_no_composition.decision.reason));
    const DispatchOutcome backspace_no_composition =
        f.controller->Dispatch(backspace, KeyText{});
    Check(backspace_no_composition.status == DispatchStatus::kNotDispatched,
          "BackSpace without a composition is not dispatched",
          DescribeDispatchStatus(backspace_no_composition.status));
    Check(backspace_no_composition.decision.reason ==
              KeyDecisionReason::kBackSpaceWithoutComposition,
          "BackSpace without a composition reports its own reason",
          DescribeKeyDecisionReason(backspace_no_composition.decision.reason));
    const DispatchOutcome escape_no_composition =
        f.controller->Dispatch(escape, KeyText{});
    Check(escape_no_composition.status == DispatchStatus::kNotDispatched,
          "Escape without a composition is not dispatched",
          DescribeDispatchStatus(escape_no_composition.status));
    const KeyEventSnapshot shifted_space =
        MakeKeyEvent(kVirtualKeySpace, true, false, false, false);
    const DispatchOutcome shifted = f.controller->Dispatch(shifted_space, KeyText{});
    Check(shifted.status == DispatchStatus::kNotDispatched,
          "Shift+Space is not dispatched",
          DescribeDispatchStatus(shifted.status));
    Check(shifted.decision.reason ==
              KeyDecisionReason::kUnsupportedModifierCombination,
          "Shift+Space reports unsupported-modifier-combination",
          DescribeKeyDecisionReason(shifted.decision.reason));
    Check(env.runtime.ApiCounts().process_key_calls == calls_before_refusals,
          "refused special keys made no process_key call",
          CountText(env.runtime.ApiCounts().process_key_calls));
    controller_key_calls += f.controller->stats().engine_key_calls;
    f.Close();
  }

  // 7. Candidate/commit UTF-16 copies and a non-empty comment, plus explicit selection.
  {
    ControllerFixture f;
    if (!f.Start(env.runtime, "epocanva_spell_english")) {
      ShutDownRuntime(&env, "dispatch");
      return;
    }
    for (const TextKey* key : {&h, &e, &l, &l, &p}) {
      const DispatchOutcome outcome = f.controller->Dispatch(key->event, key->text);
      Check(outcome.status == DispatchStatus::kDispatchedHandled,
            "spelling input key is handled", DescribeDispatchStatus(outcome.status));
    }
    const SessionStateResult state = f.session.session->CaptureState();
    Check(state.ok(), "state after hellp", DescribeSessionStateStatus(state.status));
    if (state.ok()) {
      CheckText(state.state.raw_input.utf16_code_units, "hellp", "raw input is hellp");
      CheckCandidateList(state.state, {"hellp", "hello", "help", "hella"}, "hellp");
      Check(state.state.candidates.size() >= 2 &&
                state.state.candidates[1].comment_utf16_code_units == U16("spell"),
            "the spelling suggestion carries the UTF-16 comment spell",
            state.state.candidates.size() >= 2
                ? AsciiOf(state.state.candidates[1].comment_utf16_code_units)
                : "missing");
    }
    const CandidateOutcome selected = f.controller->AcceptCandidate(1);
    Check(selected.status == CandidateStatus::kAccepted,
          "candidate 1 is explicitly accepted",
          DescribeCandidateStatus(selected.status));
    Check(selected.engine_selection_call_made, "selection made one engine call");
    Check(selected.engine_reported_accepted, "the engine accepted the selection");
    CheckText(selected.state.raw_input.utf16_code_units, "hellp",
              "raw input stays hellp after selection");
    CheckText(selected.state.preedit.utf16_code_units, "hello",
              "preedit shows the selected completion");
    Check(!selected.pending_commit.present, "selection alone produced no commit");
    const DispatchOutcome spaced = f.controller->Dispatch(SpaceEvent(), KeyText{});
    Check(spaced.status == DispatchStatus::kDispatchedHandled,
          "Space after selection is handled",
          DescribeDispatchStatus(spaced.status));
    CheckText(spaced.pending_commit.text_utf16_code_units, "hello ",
              "explicit selection committed hello plus one space");
    controller_key_calls += f.controller->stats().engine_key_calls;
    f.Close();
  }

  // 8. Default raw input and explicit candidate acceptance are not confused.
  {
    std::u16string default_commit;
    std::u16string selected_commit;
    {
      ControllerFixture f;
      if (!f.Start(env.runtime, "epocanva_test_english")) {
        ShutDownRuntime(&env, "dispatch");
        return;
      }
      for (const TextKey* key : {&h, &e, &l}) {
        (void)f.controller->Dispatch(key->event, key->text);
      }
      const DispatchOutcome spaced = f.controller->Dispatch(SpaceEvent(), KeyText{});
      default_commit = spaced.pending_commit.text_utf16_code_units;
      controller_key_calls += f.controller->stats().engine_key_calls;
      f.Close();
    }
    {
      ControllerFixture f;
      if (!f.Start(env.runtime, "epocanva_test_english")) {
        ShutDownRuntime(&env, "dispatch");
        return;
      }
      for (const TextKey* key : {&h, &e, &l}) {
        (void)f.controller->Dispatch(key->event, key->text);
      }
      const CandidateOutcome selected = f.controller->AcceptCandidate(1);
      Check(selected.status == CandidateStatus::kAccepted,
            "completion candidate 1 accepted", DescribeCandidateStatus(selected.status));
      CheckText(selected.state.raw_input.utf16_code_units, "hel",
                "selected completion leaves raw input at hel");
      CheckText(selected.state.preedit.utf16_code_units, "hello",
                "selected completion preedit is hello");
      // The preedit is 5 UTF-16 units while the raw input is 3 bytes. A position taken from
      // the raw input's boundary table would be 3 here, so the mapped values are evidence
      // that the preedit's own text supplied the table.
      Check(selected.state.cursor_status ==
                    epocanva::windows::engine::CursorOffsetStatus::kMapped &&
                selected.state.cursor_utf16_code_unit_offset == 5,
            "selected preedit cursor maps against the preedit text",
            CountText(selected.state.cursor_utf16_code_unit_offset));
      Check(selected.state.selection_status ==
                    epocanva::windows::engine::SelectionOffsetStatus::kMapped &&
                selected.state.selection_start_utf16_code_unit_offset == 5 &&
                selected.state.selection_end_utf16_code_unit_offset == 5,
            "selected preedit selection maps against the preedit text");
      const DispatchOutcome spaced = f.controller->Dispatch(SpaceEvent(), KeyText{});
      selected_commit = spaced.pending_commit.text_utf16_code_units;
      controller_key_calls += f.controller->stats().engine_key_calls;
      f.Close();
    }
    CheckText(default_commit, "hel ", "default Space commits the raw input");
    CheckText(selected_commit, "hello ", "explicit acceptance commits the completion");
    Check(default_commit != selected_commit,
          "raw input and explicit candidate acceptance are different commits");
  }

  // 9. A commit is consumed exactly once, and a read-only refresh carries no old commit.
  {
    ControllerFixture f;
    if (!f.Start(env.runtime, "epocanva_test_english")) {
      ShutDownRuntime(&env, "dispatch");
      return;
    }
    for (const TextKey* key : {&h, &e, &l}) {
      (void)f.controller->Dispatch(key->event, key->text);
    }
    const DispatchOutcome committed =
        f.controller->Dispatch(ReturnEvent(), KeyText{});
    Check(committed.status == DispatchStatus::kDispatchedHandled,
          "Enter commit is handled", DescribeDispatchStatus(committed.status));
    CheckText(committed.pending_commit.text_utf16_code_units, "hel",
              "first read returns hel");
    const SessionCommitResult second_read = f.session.session->ReadCommit();
    Check(second_read.status == SessionCommitStatus::kNoCommit,
          "the same commit cannot be read twice",
          DescribeSessionCommitStatus(second_read.status));
    const std::uint64_t commits_before = env.runtime.ApiCounts().commit_acquired;
    Check(f.controller->RefreshState() == ControllerStatus::kOk,
          "read-only RefreshState succeeds");
    Check(env.runtime.ApiCounts().commit_acquired == commits_before,
          "RefreshState did not read the commit queue",
          CountText(env.runtime.ApiCounts().commit_acquired));
    const DispatchOutcome next = f.controller->Dispatch(a.event, a.text);
    Check(next.status == DispatchStatus::kDispatchedHandled, "a after commit is handled",
          DescribeDispatchStatus(next.status));
    Check(!next.pending_commit.present, "the next dispatch carries no old commit");
    CheckText(next.state.raw_input.utf16_code_units, "a", "next raw input is a");
    controller_key_calls += f.controller->stats().engine_key_calls;
    f.Close();
  }

  // Repeat metadata is not expanded into extra engine calls.
  {
    ControllerFixture f;
    if (!f.Start(env.runtime, "epocanva_test_english")) {
      ShutDownRuntime(&env, "dispatch");
      return;
    }
    KeyEventSnapshot repeated = h.event;
    repeated.repeat_count = 5;
    repeated.previously_down = true;
    const DispatchOutcome outcome = f.controller->Dispatch(repeated, h.text);
    Check(outcome.status == DispatchStatus::kDispatchedHandled,
          "repeat_count=5 is one handled dispatch",
          DescribeDispatchStatus(outcome.status));
    CheckText(outcome.state.raw_input.utf16_code_units, "h",
              "repeat_count=5 produced exactly one h");
    Check(f.controller->stats().engine_key_calls == 1,
          "repeat_count=5 made exactly one engine call",
          CountText(f.controller->stats().engine_key_calls));
    controller_key_calls += f.controller->stats().engine_key_calls;
    f.Close();
  }

  const auto counts = env.runtime.ApiCounts();
  Check(counts.process_key_calls == controller_key_calls,
        "controller engine calls match the process_key witness",
        CountText(counts.process_key_calls) + " vs " + CountText(controller_key_calls));
  ShutDownRuntime(&env, "dispatch");
}

void RunGuardsMode(const Options& options) {
  RunEnvironment env;
  if (!BringUpRuntime(options, &env)) {
    return;
  }
  const HKL layout = CurrentLayout();
  Check(layout != nullptr, "the thread has a keyboard layout");
  std::printf("[engine-win] layout %s\n", CurrentLayoutKlid().c_str());
  if (layout == nullptr) {
    ShutDownRuntime(&env, "guards");
    return;
  }
  const TextKey h = MakeTextKey(L'h', layout);
  const TextKey e = MakeTextKey(L'e', layout);
  const TextKey l = MakeTextKey(L'l', layout);
  const TextKey a = MakeTextKey(L'a', layout);
  const TextKey p = MakeTextKey(L'p', layout);
  const TextKey comma = MakeTextKey(L',', layout);
  if (!RequireTextKey(h, 'h', "h") || !RequireTextKey(e, 'e', "e") ||
      !RequireTextKey(l, 'l', "l") || !RequireTextKey(a, 'a', "a") ||
      !RequireTextKey(p, 'p', "p") || !RequireTextKey(comma, ',', "comma")) {
    ShutDownRuntime(&env, "guards");
    return;
  }

  std::uint64_t controller_key_calls = 0;

  // 11. Boundary coordination is deferred and does not discard the composition.
  {
    ControllerFixture f;
    if (!f.Start(env.runtime, "epocanva_test_english")) {
      ShutDownRuntime(&env, "guards");
      return;
    }
    for (const TextKey* key : {&h, &e, &l}) {
      (void)f.controller->Dispatch(key->event, key->text);
    }
    const SessionStateResult before = f.session.session->CaptureState();
    Check(before.ok(), "state before comma", DescribeSessionStateStatus(before.status));
    const KeyEvaluation boundary = f.controller->Evaluate(comma.event, comma.text);
    Check(boundary.status == ControllerStatus::kOk, "Evaluate comma succeeds",
          DescribeControllerStatus(boundary.status));
    Check(boundary.decision.requires_composition_boundary(),
          "comma while composing requires a boundary",
          DescribeKeyDisposition(boundary.decision.disposition));
    Check(!f.controller->CanDispatch(boundary), "boundary decision cannot be dispatched");
    const std::uint64_t calls_before = env.runtime.ApiCounts().process_key_calls;
    const DispatchOutcome deferred = f.controller->Dispatch(comma.event, comma.text);
    Check(deferred.status == DispatchStatus::kDeferredCompositionBoundary,
          "comma dispatch is deferred, not dropped and not handled",
          DescribeDispatchStatus(deferred.status));
    Check(!deferred.engine_key_call_made, "deferred boundary made no engine call");
    Check(env.runtime.ApiCounts().process_key_calls == calls_before,
          "deferred boundary did not call process_key",
          CountText(env.runtime.ApiCounts().process_key_calls));
    const SessionStateResult after = f.session.session->CaptureState();
    Check(after.ok(), "state after deferred comma",
          DescribeSessionStateStatus(after.status));
    if (before.ok() && after.ok()) {
      CheckText(after.state.raw_input.utf16_code_units, "hel",
                "deferred boundary left raw input at hel");
      CheckText(after.state.preedit.utf16_code_units, "hel",
                "deferred boundary left preedit at hel");
      Check(after.state.is_composing, "deferred boundary left the composition open");
      Check(after.state.candidates.size() == before.state.candidates.size(),
            "deferred boundary left the menu size unchanged");
      for (std::size_t i = 0;
           i < after.state.candidates.size() && i < before.state.candidates.size(); ++i) {
        Check(after.state.candidates[i].text_utf16_code_units ==
                  before.state.candidates[i].text_utf16_code_units,
              "deferred boundary left candidate " + CountText(i) + " unchanged");
      }
    }
    const DispatchOutcome resumed = f.controller->Dispatch(l.event, l.text);
    Check(resumed.status == DispatchStatus::kDispatchedHandled,
          "typing after a deferred boundary is still handled",
          DescribeDispatchStatus(resumed.status));
    CheckText(resumed.state.raw_input.utf16_code_units, "hell",
              "composition continued to hell after the deferred comma");
    controller_key_calls += f.controller->stats().engine_key_calls;
    f.Close();
  }

  // 10. Shortcuts, release, permission, disabled input and engine-not-ready never reach the
  // engine. The real raw input is checked after every refusal.
  {
    ControllerFixture f;
    if (!f.Start(env.runtime, "epocanva_test_english")) {
      ShutDownRuntime(&env, "guards");
      return;
    }
    for (const TextKey* key : {&h, &e, &l}) {
      (void)f.controller->Dispatch(key->event, key->text);
    }
    const std::uint64_t calls_before = env.runtime.ApiCounts().process_key_calls;

    // Dispatch must judge against the state that exists when it runs, not against a decision
    // object a caller obtained earlier.
    const KeyEvaluation allowed_evaluation = f.controller->Evaluate(h.event, h.text);
    Check(f.controller->CanDispatch(allowed_evaluation),
          "h is dispatchable while the field is allowed");
    Check(f.controller->SetPermission(InputPermission::kForbidden),
          "permission changes to forbidden");
    const DispatchOutcome rechecked = f.controller->Dispatch(h.event, h.text);
    Check(rechecked.status == DispatchStatus::kNotDispatched,
          "Dispatch re-judges with the current permission and refuses",
          DescribeDispatchStatus(rechecked.status));
    Check(env.runtime.ApiCounts().process_key_calls == calls_before,
          "the re-judged refusal made no process_key call",
          CountText(env.runtime.ApiCounts().process_key_calls));
    Check(f.controller->SetPermission(InputPermission::kAllowed),
          "permission returns to allowed");

    const KeyEventSnapshot ctrl_a =
        MakeKeyEvent(static_cast<std::uint32_t>('A'), false, true, false, false);
    const KeyEventSnapshot alt_a =
        MakeKeyEvent(static_cast<std::uint32_t>('A'), false, false, true, false);
    const KeyEventSnapshot win_a =
        MakeKeyEvent(static_cast<std::uint32_t>('A'), false, false, false, true);
    const KeyEventSnapshot ctrl_alt_a =
        MakeKeyEvent(static_cast<std::uint32_t>('A'), false, true, true, false);
    for (const KeyEventSnapshot* event : {&ctrl_a, &alt_a, &win_a, &ctrl_alt_a}) {
      const KeyEvaluation evaluation = f.controller->Evaluate(*event, KeyText{});
      Check(evaluation.decision.disposition == KeyDisposition::kShortcutPassThrough,
            "shortcut is passed through",
            DescribeKeyDisposition(evaluation.decision.disposition));
      const DispatchOutcome outcome = f.controller->Dispatch(*event, KeyText{});
      Check(outcome.status == DispatchStatus::kNotDispatched,
            "shortcut is not dispatched", DescribeDispatchStatus(outcome.status));
      Check(env.runtime.ApiCounts().process_key_calls == calls_before,
            "shortcut made no process_key call",
            CountText(env.runtime.ApiCounts().process_key_calls));
    }

    KeyEventSnapshot release = h.event;
    release.transition = KeyTransition::kKeyUp;
    const KeyEvaluation released = f.controller->Evaluate(release, KeyText{});
    Check(released.decision.reason == KeyDecisionReason::kKeyRelease,
          "key-up reports key-release",
          DescribeKeyDecisionReason(released.decision.reason));
    const DispatchOutcome release_out = f.controller->Dispatch(release, KeyText{});
    Check(release_out.status == DispatchStatus::kNotDispatched,
          "key-up is not dispatched", DescribeDispatchStatus(release_out.status));

    Check(f.controller->SetPermission(InputPermission::kForbidden),
          "SetPermission(forbidden) succeeds");
    const KeyEvaluation forbidden = f.controller->Evaluate(h.event, h.text);
    Check(forbidden.decision.reason == KeyDecisionReason::kPermissionForbidden,
          "forbidden reports permission-forbidden",
          DescribeKeyDecisionReason(forbidden.decision.reason));
    Check(f.controller->Dispatch(h.event, h.text).status ==
              DispatchStatus::kNotDispatched,
          "forbidden field is not dispatched");
    Check(f.controller->SetPermission(InputPermission::kUnknown),
          "SetPermission(unknown) succeeds");
    const KeyEvaluation unknown = f.controller->Evaluate(h.event, h.text);
    Check(unknown.decision.reason == KeyDecisionReason::kPermissionUnknown,
          "unknown reports permission-unknown",
          DescribeKeyDecisionReason(unknown.decision.reason));
    Check(f.controller->Dispatch(h.event, h.text).status ==
              DispatchStatus::kNotDispatched,
          "unknown permission is not dispatched");
    Check(f.controller->SetPermission(InputPermission::kAllowed),
          "SetPermission(allowed) succeeds");

    Check(f.controller->SetInputEnabled(false), "SetInputEnabled(false) succeeds");
    const KeyEvaluation disabled = f.controller->Evaluate(h.event, h.text);
    Check(disabled.decision.reason == KeyDecisionReason::kInputDisabled,
          "disabled input reports input-disabled",
          DescribeKeyDecisionReason(disabled.decision.reason));
    Check(f.controller->Dispatch(h.event, h.text).status ==
              DispatchStatus::kNotDispatched,
          "disabled input is not dispatched");
    Check(f.controller->SetInputEnabled(true), "SetInputEnabled(true) succeeds");

    Check(env.runtime.ApiCounts().process_key_calls == calls_before,
          "all refused events together made no process_key call",
          CountText(env.runtime.ApiCounts().process_key_calls));
    const SessionStateResult state = f.session.session->CaptureState();
    Check(state.ok() && state.state.raw_input.utf16_code_units == U16("hel"),
          "refusals left the real raw input unchanged",
          state.ok() ? AsciiOf(state.state.raw_input.utf16_code_units) : "state failed");
    controller_key_calls += f.controller->stats().engine_key_calls;
    f.Close();
  }

  {
    InputController not_ready(env.runtime, nullptr,
                              ControllerOptions{true, InputPermission::kAllowed});
    const KeyEvaluation evaluation = not_ready.Evaluate(h.event, h.text);
    Check(evaluation.status == ControllerStatus::kNoSession,
          "a controller without a session reports no-session",
          DescribeControllerStatus(evaluation.status));
    Check(!not_ready.CanDispatch(evaluation), "no-session decision cannot be dispatched");
    const DispatchOutcome outcome = not_ready.Dispatch(h.event, h.text);
    Check(outcome.status == DispatchStatus::kNotDispatched,
          "engine-not-ready dispatch is refused",
          DescribeDispatchStatus(outcome.status));
    Check(!outcome.engine_key_call_made, "engine-not-ready made no engine call");
    Check(not_ready.stats().engine_key_calls == 0,
          "engine-not-ready controller made no engine key call",
          CountText(not_ready.stats().engine_key_calls));
  }

  // 12. Two controllers/sessions interleaved; plus 13. cross-thread refusals.
  {
    ControllerFixture fa;
    ControllerFixture fb;
    if (!fa.Start(env.runtime, "epocanva_test_english") ||
        !fb.Start(env.runtime, "epocanva_test_english")) {
      fa.Close();
      fb.Close();
      ShutDownRuntime(&env, "guards");
      return;
    }
    const std::vector<std::pair<InputController*, const TextKey*>> sequence = {
        {fa.controller.get(), &h}, {fb.controller.get(), &a},
        {fa.controller.get(), &e}, {fb.controller.get(), &p},
        {fb.controller.get(), &p}, {fa.controller.get(), &l}};
    for (const auto& step : sequence) {
      const DispatchOutcome outcome =
          step.first->Dispatch(step.second->event, step.second->text);
      Check(outcome.status == DispatchStatus::kDispatchedHandled,
            "interleaved dispatch is handled", DescribeDispatchStatus(outcome.status));
    }
    const SessionStateResult state_a = fa.session.session->CaptureState();
    const SessionStateResult state_b = fb.session.session->CaptureState();
    Check(state_a.ok() && state_a.state.raw_input.utf16_code_units == U16("hel"),
          "controller A raw input is hel",
          state_a.ok() ? AsciiOf(state_a.state.raw_input.utf16_code_units) : "failed");
    Check(state_b.ok() && state_b.state.raw_input.utf16_code_units == U16("app"),
          "controller B raw input is app",
          state_b.ok() ? AsciiOf(state_b.state.raw_input.utf16_code_units) : "failed");
    if (state_a.ok()) {
      Check(state_a.state.candidates.size() >= 2 &&
                state_a.state.candidates[1].text_utf16_code_units == U16("hello"),
            "A candidate 1 is hello");
    }
    if (state_b.ok()) {
      CheckCandidateList(state_b.state, {"app", "apple", "application", "apply"}, "app");
    }

    ControllerStatus thread_eval = ControllerStatus::kOk;
    ControllerStatus thread_refresh = ControllerStatus::kOk;
    DispatchStatus thread_dispatch = DispatchStatus::kNotDispatched;
    CandidateStatus thread_candidate = CandidateStatus::kAccepted;
    bool thread_set_permission = true;
    bool thread_set_enabled = true;
    SessionCloseStatus thread_close = SessionCloseStatus::kOk;
    SessionStateStatus thread_state = SessionStateStatus::kOk;
    SessionCommitStatus thread_commit = SessionCommitStatus::kRead;
    RuntimeShutdownStatus thread_shutdown = RuntimeShutdownStatus::kOk;
    SessionCreateStatus thread_create = SessionCreateStatus::kOk;
    const std::uint64_t thread_calls_before = env.runtime.ApiCounts().process_key_calls;
    std::thread wrong_thread([&]() {
      thread_eval = fb.controller->Evaluate(h.event, h.text).status;
      thread_refresh = fb.controller->RefreshState();
      thread_dispatch = fb.controller->Dispatch(h.event, h.text).status;
      thread_candidate = fb.controller->AcceptCandidate(0).status;
      thread_set_permission = fb.controller->SetPermission(InputPermission::kForbidden);
      thread_set_enabled = fb.controller->SetInputEnabled(false);
      thread_close = fb.session.session->Close();
      thread_state = fb.session.session->CaptureState().status;
      thread_commit = fb.session.session->ReadCommit().status;
      thread_shutdown = env.runtime.Shutdown();
      thread_create = env.runtime.CreateSession("epocanva_test_english").status;
    });
    wrong_thread.join();
    Check(thread_eval == ControllerStatus::kWrongThread,
          "Evaluate from a non-owner thread is refused",
          DescribeControllerStatus(thread_eval));
    Check(thread_refresh == ControllerStatus::kWrongThread,
          "RefreshState from a non-owner thread is refused",
          DescribeControllerStatus(thread_refresh));
    Check(thread_dispatch == DispatchStatus::kNotDispatched,
          "Dispatch from a non-owner thread is refused",
          DescribeDispatchStatus(thread_dispatch));
    Check(thread_candidate == CandidateStatus::kWrongThread,
          "AcceptCandidate from a non-owner thread is refused",
          DescribeCandidateStatus(thread_candidate));
    Check(!thread_set_permission && !thread_set_enabled,
          "controller setters from a non-owner thread are refused");
    Check(thread_close == SessionCloseStatus::kWrongThread,
          "session Close from a non-owner thread is refused",
          DescribeSessionCloseStatus(thread_close));
    Check(thread_state == SessionStateStatus::kWrongThread,
          "session CaptureState from a non-owner thread is refused",
          DescribeSessionStateStatus(thread_state));
    Check(thread_commit == SessionCommitStatus::kWrongThread,
          "session ReadCommit from a non-owner thread is refused",
          DescribeSessionCommitStatus(thread_commit));
    Check(thread_shutdown == RuntimeShutdownStatus::kWrongThread,
          "runtime Shutdown from a non-owner thread is refused",
          epocanva::windows::engine::DescribeRuntimeShutdownStatus(thread_shutdown));
    Check(thread_create == SessionCreateStatus::kWrongThread,
          "runtime CreateSession from a non-owner thread is refused",
          DescribeSessionCreateStatus(thread_create));
    Check(env.runtime.IsInitialized() && env.runtime.live_session_count() == 2,
          "cross-thread refusals left the runtime and sessions intact",
          CountText(env.runtime.live_session_count()));
    Check(env.runtime.ApiCounts().process_key_calls == thread_calls_before,
          "cross-thread refusals made no process_key call",
          CountText(env.runtime.ApiCounts().process_key_calls));
    const SessionStateResult b_after = fb.session.session->CaptureState();
    Check(b_after.ok() && b_after.state.raw_input.utf16_code_units == U16("app"),
          "B raw input is still app after cross-thread refusals",
          b_after.ok() ? AsciiOf(b_after.state.raw_input.utf16_code_units) : "failed");

    const DispatchOutcome commit_a = fa.controller->Dispatch(ReturnEvent(), KeyText{});
    Check(commit_a.status == DispatchStatus::kDispatchedHandled,
          "A Enter after the cross-thread test is handled",
          DescribeDispatchStatus(commit_a.status));
    CheckText(commit_a.pending_commit.text_utf16_code_units, "hel",
              "A committed hel without B interference");
    Check(commit_a.state.raw_input.utf16_code_units.empty(), "A composition ended");
    const SessionStateResult b_still = fb.session.session->CaptureState();
    Check(b_still.ok() && b_still.state.raw_input.utf16_code_units == U16("app"),
          "B composition is untouched by A commit",
          b_still.ok() ? AsciiOf(b_still.state.raw_input.utf16_code_units) : "failed");
    const DispatchOutcome space_b = fb.controller->Dispatch(SpaceEvent(), KeyText{});
    Check(space_b.status == DispatchStatus::kDispatchedHandled,
          "B Space after A commit is handled",
          DescribeDispatchStatus(space_b.status));
    CheckText(space_b.pending_commit.text_utf16_code_units, "app ",
              "B committed app plus one space");

    controller_key_calls += fa.controller->stats().engine_key_calls;
    controller_key_calls += fb.controller->stats().engine_key_calls;
    fa.Close();
    fb.Close();
  }

  const auto counts = env.runtime.ApiCounts();
  Check(counts.process_key_calls == controller_key_calls,
        "controller engine calls match the process_key witness",
        CountText(counts.process_key_calls) + " vs " + CountText(controller_key_calls));
  ShutDownRuntime(&env, "guards");
}

void RunSelfCheckFailDispatch() {
  KeyDecision pass_through;
  EngineKey key;
  Check(MapDecisionToEngineKey(pass_through, &key),
        "deliberately false: a pass-through decision maps to an engine key");
}

}  // namespace engine_bridge
}  // namespace tests
}  // namespace epocanva
