// EpoCanva P3.1 -- the Return/Space acceptance matrix, on the completion fixture.
// See acceptance_tests.h for the five frozen rules and the sources they were derived from.
#include "acceptance_tests.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "english_diagnostics.h"

namespace epocanva {
namespace recovery {
namespace {

constexpr int kXkSpace = 0x0020;

constexpr char kMenuHel[] = "hel|hello|help|helmet";

long long LL(unsigned long long v) { return static_cast<long long>(v); }

std::string SpaceCounterLine(const EpoCanvaEnglishCounters& c) {
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "pk_events=%llu space_seen=%llu case_a=%llu case_b=%llu case_c=%llu "
                "commits_issued=%llu spaces_emitted=%llu attempts=%llu",
                static_cast<unsigned long long>(c.processor_key_events),
                static_cast<unsigned long long>(c.space_events_seen),
                static_cast<unsigned long long>(c.space_case_a_raw_token),
                static_cast<unsigned long long>(c.space_case_b_selection),
                static_cast<unsigned long long>(c.space_case_c_empty),
                static_cast<unsigned long long>(c.composition_commits_issued),
                static_cast<unsigned long long>(c.space_characters_emitted),
                static_cast<unsigned long long>(c.lexicon_load_attempts));
  return std::string(buf);
}

}  // namespace

ExitCode RunAcceptanceSemanticsMode(Harness& h) {
  if (!h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "accept.surface",
         "this run needs the english engine; the loaded DLL exposes no "
         "epocanva_english module through find_module()/get_api()");
    return kEnglishSurfaceUnavailable;
  }
  h.checks.CheckStr(std::string(h.surface.api->build_id()), kExpectedBuildId,
                    "component build id");

  EpoCanvaEnglishCounters c;
  if (!ReadCounters(h.surface, &c)) {
    Fail(kCounterReadFailed, "accept.counters", "get_counters() returned False");
    return kCounterReadFailed;
  }
  std::printf("[english] counters.after_init        %s\n", SpaceCounterLine(c).c_str());

  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }

  const RimeSessionId a = CreateSessionOrZero(h.api, "ACCEPT-A");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "ACCEPT-A",
                                       kCompletionSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }

  long long keys_sent = 0;
  long long exp_a = 0, exp_b = 0;
  KeyEvent ev;

  auto send = [&](RimeSessionId sid, int keysym, int mask,
                  const std::string& label) -> ExitCode {
    ++keys_sent;
    const ExitCode prc = SendKey(h.api, sid, keysym, mask, &h.balance, &ev);
    if (prc != kOk) {
      return prc;
    }
    std::printf("[english] ev=%03lld key=0x%04x mask=%d %-42s handled=%d %s\n",
                keys_sent, keysym, mask, label.c_str(), ev.handled ? 1 : 0,
                DescribeState(ev.state).c_str());
    return kOk;
  };
  auto type = [&](RimeSessionId sid, const std::string& text,
                  const std::string& label) -> ExitCode {
    for (std::size_t i = 0; i < text.size(); ++i) {
      const ExitCode prc =
          send(sid, static_cast<int>(static_cast<unsigned char>(text[i])), 0, label);
      if (prc != kOk) {
        return prc;
      }
    }
    return kOk;
  };
  auto read_commit = [&](RimeSessionId sid, const std::string& want,
                         const std::string& label) -> ExitCode {
    h.checks.Check(ev.state.commit_present,
                   label + ": the event produced a commit", "none");
    h.checks.CheckStr(ev.state.commit_text, want, label + ": committed bytes");
    bool again = false;
    std::string again_text;
    const ExitCode crc = ReadCommitOnce(h.api, sid, &h.balance, &again, &again_text);
    if (crc != kOk) {
      return crc;
    }
    h.checks.Check(!again, label + ": the commit was consumed, nothing left",
                   again ? again_text : std::string("absent"));
    return kOk;
  };
  auto expect_no_commit = [&](RimeSessionId sid, const std::string& label) -> ExitCode {
    h.checks.Check(!ev.state.commit_present, label + ": no commit came with that event",
                   ev.state.commit_present ? ev.state.commit_text : std::string("absent"));
    bool present = false;
    std::string text;
    const ExitCode crc = ReadCommitOnce(h.api, sid, &h.balance, &present, &text);
    h.checks.Check(!present, label + ": and the queue is still empty",
                   present ? text : std::string("absent"));
    return crc;
  };
  // The one acceptance state this suite may build with: RimeApi::highlight_candidate(),
  // whose contract reads "highlight a selection without committing"
  // (third_party/librime/src/rime_api.h:501-502). select_candidate() is a different state
  // and is called only by the rule-5 probes.
  auto highlight = [&](RimeSessionId sid, std::size_t index, const std::string& want_row,
                       const std::string& label) -> ExitCode {
    bool moved = false;
    rc = HighlightCandidate(h.api, sid, index, &moved);
    if (rc != kOk) {
      return rc;
    }
    h.checks.Check(moved, label + ": highlight_candidate() moved the highlight",
                   moved ? "True" : "False");
    StateSnapshot lit;
    ExitCode failure = kOk;
    if (!ReadState(h.api, sid, &h.balance, &lit, &failure)) {
      return failure;
    }
    // The highlight is real (the menu reports that index), the row under it is the one
    // this probe names, the composition is untouched, and nothing was committed by
    // moving it: rule 1, asserted inside the helper so every highlighted probe carries it.
    h.checks.CheckNum(lit.highlighted, static_cast<long long>(index),
                      label + ": get_context() reports the highlight there");
    h.checks.Check(lit.highlighted >= 0 &&
                       static_cast<std::size_t>(lit.highlighted) < lit.candidates.size() &&
                       lit.candidates[static_cast<std::size_t>(lit.highlighted)] == want_row,
                   label + ": the highlighted row really is '" + want_row + "'",
                   JoinMenu(lit));
    h.checks.CheckMenu(lit, kMenuHel, label + ": the menu itself did not change");
    h.checks.CheckStr(lit.raw_input, "hel", label + ": the raw composition is untouched");
    h.checks.Check(lit.status_composing, label + ": still composing", "not composing");
    h.checks.Check(!lit.commit_present, label + ": the highlight committed nothing",
                   lit.commit_present ? lit.commit_text : std::string("absent"));
    bool queued = false;
    std::string queued_text;
    const ExitCode crc =
        ReadCommitOnce(h.api, sid, &h.balance, &queued, &queued_text);
    if (crc != kOk) {
      return crc;
    }
    h.checks.Check(!queued, label + ": and the commit queue is still empty",
                   queued ? queued_text : std::string("absent"));
    return kOk;
  };

  // ---- rule 1. a highlight alone commits nothing ------------------------------------------
  rc = type(a, "hel", "rule1 typing hel");
  if (rc == kOk) {
    h.checks.CheckMenu(ev.state, kMenuHel, "rule1: menu before the highlight");
    rc = highlight(a, 2, "help", "rule1 highlight(2)");
    if (rc == kOk) {
      rc = expect_no_commit(a, "rule1: moving the highlight");
    }
  }

  // ---- rule 2. default highlight is the raw input; Return commits it, no space ------------
  if (rc == kOk) {
    // Rule 1 deliberately left its composition alive; every probe after the first clears
    // with Escape first, the way recovery.spelling.commit_and_selection does, so no probe
    // inherits another's composition.
    rc = send(a, kXkEscape, 0, "rule2 [clear]");
  }
  if (rc == kOk) {
    rc = type(a, "hel", "rule2 typing hel");
  }
  if (rc == kOk) {
    // The default highlight must be ON the raw input for this probe to mean anything:
    // candidate 0 is what was typed, and the menu reports the highlight at index 0.
    h.checks.CheckMenu(ev.state, kMenuHel, "rule2: menu as typed");
    h.checks.CheckNum(ev.state.highlighted, 0, "rule2: the highlight starts at index 0");
    h.checks.Check(!ev.state.candidates.empty() && ev.state.candidates[0] == "hel",
                   "rule2: candidate 0 IS the raw input", JoinMenu(ev.state));
    rc = send(a, kXkReturn, 0, "rule2 Return at the default highlight");
  }
  if (rc == kOk) {
    h.checks.Check(ev.handled, "rule2: Return was handled", "not handled");
    h.checks.Check(!ev.state.status_composing, "rule2: the composition ended",
                   "still composing");
    h.checks.CheckStr(ev.state.raw_input, "", "rule2: raw composition cleared");
    rc = read_commit(a, "hel", "rule2: Return at the default highlight commits the raw "
                               "input, with no space");
  }

  // ---- rule 3. Return after a real highlight accepts the highlighted row ------------------
  if (rc == kOk) {
    rc = send(a, kXkEscape, 0, "rule3 [clear]");
  }
  if (rc == kOk) {
    rc = type(a, "hel", "rule3 typing hel");
  }
  if (rc == kOk) {
    rc = highlight(a, 2, "help", "rule3 highlight(2)");
  }
  if (rc == kOk) {
    rc = send(a, kXkReturn, 0, "rule3 Return on the highlighted completion");
  }
  if (rc == kOk) {
    h.checks.Check(ev.handled, "rule3: Return was handled", "not handled");
    h.checks.Check(!ev.state.status_composing, "rule3: the composition ended",
                   "still composing");
    // "help" is the row the highlight moved to above, read back off the menu, not a
    // literal this test invented.
    rc = read_commit(a, "help",
                     "rule3: Return accepts the currently highlighted row, with no space");
  }

  // ---- rule 4. Space after a highlight-only still follows the raw-input policy ------------
  if (rc == kOk) {
    rc = send(a, kXkEscape, 0, "rule4 [clear]");
  }
  if (rc == kOk) {
    rc = type(a, "hel", "rule4 typing hel");
  }
  if (rc == kOk) {
    EpoCanvaEnglishCounters before;
    if (!ReadCounters(h.surface, &before)) {
      h.api->destroy_session(a);
      return kCounterReadFailed;
    }
    rc = highlight(a, 1, "hello", "rule4 highlight(1)");
    if (rc == kOk) {
      rc = send(a, kXkSpace, 0, "rule4 Space after highlight-only");
    }
    if (rc == kOk) {
      h.checks.Check(ev.handled, "rule4: the plain space was accepted",
                     ev.handled ? "True" : "False");
      h.checks.Check(!ev.state.status_composing, "rule4: the composition ended",
                     "still composing");
      // Case A despite the highlighted "hello": the space policy reads the segment
      // status, and a highlight leaves it alone.
      rc = read_commit(a, "hel ", "rule4: Space after highlight-only commits the raw "
                                 "input plus one space");
    }
    if (rc == kOk) {
      EpoCanvaEnglishCounters after;
      if (!ReadCounters(h.surface, &after)) {
        h.api->destroy_session(a);
        return kCounterReadFailed;
      }
      ++exp_a;
      h.checks.CheckNum(
          CounterDelta(before, after, &EpoCanvaEnglishCounters::space_case_a_raw_token), 1,
          "rule4: the space resolved to case A");
      h.checks.CheckNum(
          CounterDelta(before, after, &EpoCanvaEnglishCounters::space_case_b_selection), 0,
          "rule4: a highlight is not a selection (no case B)");
    }
  }

  // ---- rule 5a. an explicit selection, then Return: the selected text, no space -----------
  if (rc == kOk) {
    rc = send(a, kXkEscape, 0, "rule5a [clear]");
  }
  if (rc == kOk) {
    rc = type(a, "hel", "rule5a typing hel");
  }
  if (rc == kOk) {
    EpoCanvaEnglishCounters before;
    if (!ReadCounters(h.surface, &before)) {
      h.api->destroy_session(a);
      return kCounterReadFailed;
    }
    bool accepted = false;
    rc = SelectCandidate(h.api, a, 1, &accepted);
    if (rc == kOk) {
      h.checks.Check(accepted, "rule5a: select_candidate(1) was accepted",
                     accepted ? "True" : "False");
      StateSnapshot mid;
      ExitCode failure = kOk;
      if (!ReadState(h.api, a, &h.balance, &mid, &failure)) {
        h.api->destroy_session(a);
        return failure;
      }
      h.checks.Check(!mid.commit_present,
                     "rule5a: the selection committed nothing itself",
                     mid.commit_present ? mid.commit_text : std::string("absent"));
      h.checks.Check(mid.status_composing, "rule5a: the composition is still live",
                     "not composing");
      h.checks.CheckStr(mid.raw_input, "hel", "rule5a: get_input() unchanged");
      h.checks.CheckNum(mid.num_candidates, 0,
                        "rule5a: the selection closed the segment, so no menu is offered");
      rc = send(a, kXkReturn, 0, "rule5a Return after the explicit selection");
    }
    if (rc == kOk) {
      h.checks.Check(ev.handled, "rule5a: Return was handled", "not handled");
      h.checks.Check(!ev.state.status_composing, "rule5a: the composition ended",
                     "still composing");
      rc = read_commit(a, "hello",
                       "rule5a: Return after a selection commits the selected text, "
                       "with no space");
    }
    if (rc == kOk) {
      EpoCanvaEnglishCounters after;
      if (!ReadCounters(h.surface, &after)) {
        h.api->destroy_session(a);
        return kCounterReadFailed;
      }
      h.checks.CheckNum(
          CounterDelta(before, after, &EpoCanvaEnglishCounters::space_case_a_raw_token) +
              CounterDelta(before, after,
                           &EpoCanvaEnglishCounters::space_case_b_selection),
          0, "rule5a: Return resolved no space case at all");
    }
  }

  // ---- rule 5b. an explicit selection, then Space: the selected text plus one space -------
  if (rc == kOk) {
    rc = send(a, kXkEscape, 0, "rule5b [clear]");
  }
  if (rc == kOk) {
    rc = type(a, "hel", "rule5b typing hel");
  }
  if (rc == kOk) {
    EpoCanvaEnglishCounters before;
    if (!ReadCounters(h.surface, &before)) {
      h.api->destroy_session(a);
      return kCounterReadFailed;
    }
    bool accepted = false;
    rc = SelectCandidate(h.api, a, 1, &accepted);
    if (rc == kOk) {
      h.checks.Check(accepted, "rule5b: select_candidate(1) was accepted",
                     accepted ? "True" : "False");
      rc = send(a, kXkSpace, 0, "rule5b Space after the explicit selection");
    }
    if (rc == kOk) {
      h.checks.Check(ev.handled, "rule5b: the plain space was accepted",
                     ev.handled ? "True" : "False");
      h.checks.Check(!ev.state.status_composing, "rule5b: the composition ended",
                     "still composing");
      rc = read_commit(a, "hello ",
                       "rule5b: Space after a selection commits the selected text plus "
                       "one space (case B, unchanged)");
    }
    if (rc == kOk) {
      EpoCanvaEnglishCounters after;
      if (!ReadCounters(h.surface, &after)) {
        h.api->destroy_session(a);
        return kCounterReadFailed;
      }
      ++exp_b;
      h.checks.CheckNum(
          CounterDelta(before, after, &EpoCanvaEnglishCounters::space_case_b_selection), 1,
          "rule5b: the space resolved to case B");
    }
  }

  // ---- the counters, as identities ---------------------------------------------------------
  if (rc == kOk && !ReadCounters(h.surface, &c)) {
    rc = kCounterReadFailed;
  }
  if (rc == kOk) {
    std::printf("[english] counters.final             %s\n", SpaceCounterLine(c).c_str());
    h.checks.CheckNum(LL(c.processor_key_events), keys_sent,
                      "processor_key_events == the keys this run injected (every key, "
                      "Return included, reached the chain once)");
    h.checks.CheckNum(LL(c.space_case_a_raw_token), exp_a,
                      "case A happened exactly as often as this run pressed a space with "
                      "only a highlight");
    h.checks.CheckNum(LL(c.space_case_b_selection), exp_b,
                      "case B happened exactly once, on the explicit-selection space");
    h.checks.CheckNum(LL(c.space_case_c_empty), 0,
                      "no empty-composition space occurred in this mode");
    h.checks.CheckNum(LL(c.space_events_seen),
                      LL(c.space_case_a_raw_token) + LL(c.space_case_b_selection) +
                          LL(c.space_case_c_empty) + LL(c.space_events_refused),
                      "space_events_seen == a + b + c + refused");
    h.checks.CheckNum(LL(c.composition_commits_issued),
                      LL(c.space_case_a_raw_token) + LL(c.space_case_b_selection),
                      "composition_commits_issued == a + b (the Return commits belong to "
                      "the editor, not the space policy)");
    h.checks.CheckNum(LL(c.lexicon_load_attempts), 1,
                      "the whole matrix cost one word-list read");
    h.checks.CheckNum(LL(c.queries_covering_part_of_input), 0,
                      "no token was handed over as a fragment");
  }

  const ExitCode done = DestroySessionAndCheck(h.api, a, &h.checks, "ACCEPT session A");
  if (done != kOk && rc == kOk) {
    rc = done;
  }
  return rc;
}

ExitCode RunAcceptanceSelfCheckFailMode(Harness& h) {
  if (!h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "accept-self.surface",
         "this control needs the english engine to have a real Return key to get wrong");
    return kEnglishSurfaceUnavailable;
  }
  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }
  const RimeSessionId a = CreateSessionOrZero(h.api, "ACCEPT-SELF");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "ACCEPT-SELF",
                                       kCompletionSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  std::vector<KeyEvent> events;
  rc = TypeText(h.api, a, "hel", &h.balance, &events);
  if (rc == kOk) {
    // Highlight only -- no selection was made.
    bool moved = false;
    rc = HighlightCandidate(h.api, a, 2, &moved);
  }
  if (rc == kOk) {
    rc = SendKey(h.api, a, kXkReturn, 0, &h.balance, &events.emplace_back());
  }
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  // The claim this mode makes is the forbidden one: that Return ignores the highlight and
  // commits the raw input. Rule 3 says it accepts the highlighted row ("help"), so this
  // must fail.
  h.checks.CheckStr(events.back().state.commit_text, "hel",
                    "deliberately asserted that Return after a highlight commits the raw "
                    "input");
  const ExitCode done = DestroySessionAndCheck(h.api, a, &h.checks, "ACCEPT-SELF");
  if (done != kOk) {
    return done;
  }
  return kOk;  // the harness turns the recorded failure into exit 14
}

}  // namespace recovery
}  // namespace epocanva
