// EpoCanva P2.5-recovery -- space and continuous multi-word commit.
// See space_tests.h for the three rules, why counters are needed at all, and what this
// mode does NOT claim.
#include "space_tests.h"

#include <cstdio>
#include <string>
#include <vector>

#include "english_diagnostics.h"

namespace epocanva {
namespace recovery {
namespace {

// XK_space. The keysym and the emitted byte have the same value for printable ASCII
// (include/X11/keysymdef.h:528-601) but they are different things -- an event identifier
// and a text payload -- so they are named separately, as the policy header does.
constexpr int kXkSpace = 0x0020;

// Modifier bits, transcribed from the pinned upstream header
// (third_party/librime/src/rime/key_table.h:14-38) rather than read from the product's
// own copy of them. Deliberately duplicated: if the product's constants drifted, this
// test has to disagree with it instead of agreeing with whatever the code currently says.
constexpr int kShiftMaskBit = 1 << 0;
constexpr int kControlMaskBit = 1 << 2;
constexpr int kAltMaskBit = 1 << 3;
constexpr int kSuperMaskBit = 1 << 26;
constexpr int kReleaseMaskBit = 1 << 30;

constexpr char kMenuHel[] = "hel|hello|help|helmet";
constexpr char kMenuHe[] = "he|hello|help|hen|hel|helmet|herb";
constexpr char kMenuDonApos[] = "don'|don't";
constexpr char kMenuHello[] = "Hello";  // the deduped single candidate for "Hello"
constexpr char kMenuWorld[] = "world";
constexpr char kMenuApp[] = "app|apple|application|apply";
// Protected text: the whole token, candidate 0 only.
constexpr char kMenuEmail[] = "user@example.com";

long long LL(unsigned long long v) { return static_cast<long long>(v); }

std::string SpaceCounterLine(const EpoCanvaEnglishCounters& c) {
  char buf[320];
  std::snprintf(buf, sizeof(buf),
                "sp_ctor=%llu sp_dstr=%llu sp_live=%llu pk_events=%llu "
                "space_seen=%llu space_refused=%llu case_a=%llu case_b=%llu "
                "case_c=%llu commits_issued=%llu spaces_emitted=%llu",
                static_cast<unsigned long long>(c.space_processor_constructed),
                static_cast<unsigned long long>(c.space_processor_destroyed),
                static_cast<unsigned long long>(c.space_processor_live),
                static_cast<unsigned long long>(c.processor_key_events),
                static_cast<unsigned long long>(c.space_events_seen),
                static_cast<unsigned long long>(c.space_events_refused),
                static_cast<unsigned long long>(c.space_case_a_raw_token),
                static_cast<unsigned long long>(c.space_case_b_selection),
                static_cast<unsigned long long>(c.space_case_c_empty),
                static_cast<unsigned long long>(c.composition_commits_issued),
                static_cast<unsigned long long>(c.space_characters_emitted));
  return std::string(buf);
}

}  // namespace

ExitCode RunSpaceCommitMode(Harness& h) {
  if (!h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "space.surface",
         "this run needs the english engine; the loaded DLL exposes no "
         "epocanva_english module through find_module()/get_api()");
    return kEnglishSurfaceUnavailable;
  }
  h.checks.CheckStr(std::string(h.surface.api->build_id()), kExpectedBuildId,
                    "component build id");

  EpoCanvaEnglishCounters c;
  if (!ReadCounters(h.surface, &c)) {
    Fail(kCounterReadFailed, "space.counters", "get_counters() returned False");
    return kCounterReadFailed;
  }
  std::printf("[english] counters.after_init        %s\n", SpaceCounterLine(c).c_str());
  h.checks.CheckNum(LL(c.space_processor_constructed), 0,
                    "no space processor before the first session");
  h.checks.CheckNum(LL(c.processor_key_events), 0, "no key event has reached it yet");

  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }

  long long keys_sent = 0;
  long long exp_a = 0, exp_b = 0, exp_c = 0, exp_refused = 0;
  std::string transcript;  // only bytes get_commit() actually returned
  KeyEvent ev;

  auto send = [&](RimeSessionId sid, int keysym, int mask,
                  const std::string& label) -> ExitCode {
    ++keys_sent;
    const ExitCode prc = SendKey(h.api, sid, keysym, mask, &h.balance, &ev);
    if (prc != kOk) {
      return prc;
    }
    std::printf("[english] ev=%03lld key=0x%04x mask=%d %-34s handled=%d %s\n",
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
  // The commit a key event produced is read out of that event's own observation:
  // get_commit() CONSUMES, and SendKey already calls it after process_key(), so a second
  // call would find nothing and would prove nothing. The re-read below is deliberately the
  // other half of the claim: after the value has been handed out once, the queue is empty.
  auto read_commit = [&](RimeSessionId sid, const std::string& want,
                         const std::string& label) -> ExitCode {
    h.checks.Check(ev.state.commit_present,
                   label + ": the event produced a commit", "none");
    h.checks.CheckStr(ev.state.commit_text, want, label + ": committed bytes");
    transcript += ev.state.commit_text;  // appended verbatim, never synthesised
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
  auto expect_no_commit = [&](RimeSessionId sid,
                              const std::string& label) -> ExitCode {
    h.checks.Check(!ev.state.commit_present,
                   label + ": the event produced no commit",
                   ev.state.commit_present ? ev.state.commit_text
                                           : std::string("absent"));
    bool present = false;
    std::string text;
    const ExitCode crc = ReadCommitOnce(h.api, sid, &h.balance, &present, &text);
    h.checks.Check(!present, label + ": and the queue is still empty",
                   present ? text : std::string("absent"));
    return crc;
  };

  // A plain space that must resolve to case A or B or C, with the exact concatenated
  // commit the C API hands back, and the post-event state.
  auto space = [&](RimeSessionId sid, const std::string& want_commit, long long* case_tally,
                   const std::string& label) -> ExitCode {
    const ExitCode prc = send(sid, kXkSpace, 0, label);
    if (prc != kOk) {
      return prc;
    }
    h.checks.Check(ev.handled, label + ": the plain space was accepted",
                   ev.handled ? "True" : "False");
    h.checks.Check(!ev.state.status_composing, label + ": composition ended",
                   "still composing");
    h.checks.Check(!ev.state.has_preedit, label + ": no preedit left",
                   ev.state.has_preedit ? ev.state.preedit : std::string("absent"));
    h.checks.CheckStr(ev.state.raw_input, "", label + ": raw composition cleared");
    h.checks.CheckNum(ev.state.num_candidates, 0, label + ": menu cleared");
    ++*case_tally;
    return read_commit(sid, want_commit, label);
  };

  const RimeSessionId a = CreateSessionOrZero(h.api, "SPACE-A");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "SPACE-A",
                                       kCompletionSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  if (!ReadCounters(h.surface, &c)) {
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  h.checks.Check(c.space_processor_constructed >= 1,
                 "the schema built the space processor",
                 std::to_string(c.space_processor_constructed));
  h.checks.Check(c.space_processor_live >= 1, "and it is alive",
                 std::to_string(c.space_processor_live));

  // ---- 1/2/3. the three shapes each commit their own text, once --------------
  rc = type(a, "hel", "hel");
  if (rc == kOk) {
    h.checks.CheckMenu(ev.state, kMenuHel, "hel menu before the space");
    rc = space(a, "hel ", &exp_a, "hel + Space (case A)");
  }
  if (rc == kOk) {
    rc = type(a, "Hel", "Hel");
  }
  if (rc == kOk) {
    rc = space(a, "Hel ", &exp_a, "Hel + Space keeps the typed shape");
  }
  if (rc == kOk) {
    rc = type(a, "HEL", "HEL");
  }
  if (rc == kOk) {
    rc = space(a, "HEL ", &exp_a, "HEL + Space keeps the typed shape");
  }

  // ---- 4. a highlight is NOT an acceptance ----------------------------------
  rc = type(a, "hel", "hel for highlight");
  if (rc == kOk) {
    bool moved = false;
    rc = HighlightCandidate(h.api, a, 1, &moved);
    if (rc == kOk) {
      h.checks.Check(moved, "highlight_candidate(1) moved the highlight",
                     moved ? "True" : "False");
      // Read the menu back: the highlight really is on the completion, so what the next
      // space commits is the raw text DESPITE a highlighted "hello".
      StateSnapshot ctx_after;
      ExitCode failure = kOk;
      if (!ReadState(h.api, a, &h.balance, &ctx_after, &failure)) {
        h.api->destroy_session(a);
        return failure;
      }
      h.checks.CheckNum(ctx_after.highlighted, 1,
                        "get_context() reports the highlight at index 1");
      h.checks.CheckMenu(ctx_after, kMenuHel, "the menu itself did not change");
      h.checks.CheckStr(ctx_after.raw_input, "hel",
                        "the raw composition is still what was typed");
    }
  }
  if (rc == kOk) {
    rc = space(a, "hel ", &exp_a, "highlight then Space still commits the original");
  }

  // ---- 5/6. an explicit selection, and the state between selection and space --
  rc = type(a, "hel", "hel for selection");
  if (rc == kOk) {
    bool accepted = false;
    rc = SelectCandidate(h.api, a, 1, &accepted);
    if (rc == kOk) {
      h.checks.Check(accepted, "select_candidate(1) accepted",
                     accepted ? "True" : "False");
      StateSnapshot mid;
      ExitCode failure = kOk;
      if (!ReadState(h.api, a, &h.balance, &mid, &failure)) {
        h.api->destroy_session(a);
        return failure;
      }
      // Selection is a step, not a commit: nothing may be in the queue yet, and the
      // composition must still be the three bytes that were typed. This is the run-side
      // evidence for "the selected word is committed by the space, exactly once".
      h.checks.Check(!mid.commit_present,
                     "select_candidate() put nothing in the commit queue",
                     mid.commit_present ? mid.commit_text : std::string("absent"));
      h.checks.Check(mid.status_composing, "the composition is still live",
                     "not composing");
      h.checks.CheckStr(mid.raw_input, "hel", "and get_input() still reads 'hel'");
      // The menu is GONE once the selection is made: ConcreteEngine::OnSelect() closes the
      // segment and advances the composition (src/rime/engine.cc:259-270, Segment::Close),
      // so there is nothing left to choose from -- the choice has been recorded in the
      // segment's status. That is the state the next space reads, and it is why case B
      // commits the confirmed text instead of candidate 0 of a live menu.
      h.checks.CheckNum(mid.num_candidates, 0,
                        "the selection closed the segment: no menu is offered any more");
      h.checks.Check(mid.has_commit_text_preview,
                     "and the engine still reports what a commit would write",
                     mid.has_commit_text_preview ? mid.commit_text_preview
                                                 : std::string("absent"));
    }
  }
  if (rc == kOk) {
    rc = space(a, "hello ", &exp_b, "select hello then Space (case B)");
  }

  // ---- 7. two empty-composition spaces are two spaces -----------------------
  rc = space(a, " ", &exp_c, "Space on an empty composition (case C)");
  if (rc == kOk) {
    rc = space(a, " ", &exp_c, "a second Space, still case C");
  }

  // ---- 8. continuous multi-word input: "Hello world " -----------------------
  if (rc == kOk) {
    const std::size_t before = transcript.size();
    rc = type(a, "Hello", "Hello");
    if (rc == kOk) {
      h.checks.CheckMenu(ev.state, kMenuHello, "Hello is an exact row: deduped, 1 item");
      rc = space(a, "Hello ", &exp_a, "Hello + Space");
    }
    if (rc == kOk) {
      rc = type(a, "world", "world");
    }
    if (rc == kOk) {
      h.checks.CheckMenu(ev.state, kMenuWorld,
                         "world is an exact row too, so it is not completed into work");
    }
    if (rc == kOk) {
      rc = space(a, "world ", &exp_a, "world + Space");
    }
    if (rc == kOk) {
      // Built only from what get_commit() returned: "Hello world " has to appear by
      // concatenation, which is what proves no space was inserted by the test.
      h.checks.CheckStr(transcript.substr(before), "Hello world ",
                        "the concatenated transcript of two words");
    }
  }

  // ---- 9. a delimited form: default commits the partial, selection the word --
  if (rc == kOk) {
    rc = type(a, "don'", "don'");
  }
  if (rc == kOk) {
    h.checks.CheckMenu(ev.state, kMenuDonApos,
                       "don' offers don't in the menu, and the Space must not take it");
    rc = space(a, "don' ", &exp_a, "don' + Space commits the partial");
  }
  if (rc == kOk) {
    rc = type(a, "don'", "don' for selection");
  }
  if (rc == kOk) {
    bool accepted = false;
    rc = SelectCandidate(h.api, a, 1, &accepted);
    if (rc == kOk) {
      h.checks.Check(accepted, "don': select_candidate(1) accepted",
                     accepted ? "True" : "False");
    }
  }
  if (rc == kOk) {
    rc = space(a, "don't ", &exp_b, "don' selected then Space commits the whole word");
  }

  // ---- 10/11. protected text, then the next ordinary word completes again ---
  if (rc == kOk) {
    rc = type(a, "user@example.com", "email");
  }
  if (rc == kOk) {
    h.checks.CheckMenu(ev.state, kMenuEmail, "the address came back whole");
    rc = space(a, "user@example.com ", &exp_a,
               "protected text + Space is byte-for-byte the text and one space");
  }
  if (rc == kOk) {
    // The space processor kept no state about the protected token: the next word is
    // judged on its own bytes.
    rc = type(a, "hel", "hel after a protected commit");
  }
  if (rc == kOk) {
    h.checks.CheckMenu(ev.state, kMenuHel,
                       "completions are back for the next ordinary word");
    rc = space(a, "hel ", &exp_a, "and it commits normally");
  }

  // ---- 12. the case shape of a committed word is not inherited -------------
  if (rc == kOk) {
    rc = type(a, "HEL", "HEL before a lowercase word");
  }
  if (rc == kOk) {
    rc = space(a, "HEL ", &exp_a, "HEL + Space");
  }
  if (rc == kOk) {
    rc = type(a, "he", "he after an uppercase commit");
  }
  if (rc == kOk) {
    h.checks.CheckStr(ev.state.raw_input, "he", "the new composition is lowercase");
    h.checks.CheckMenu(ev.state, kMenuHe,
                       "and its completions are lowercase: no shape was inherited");
    rc = space(a, "he ", &exp_a, "he + Space");
  }

  // ---- 13. Escape cancels; a following Space does not revive the text ------
  if (rc == kOk) {
    rc = type(a, "hel", "hel to cancel");
  }
  if (rc == kOk) {
    rc = send(a, kXkEscape, 0, "Escape");
  }
  if (rc == kOk) {
    h.checks.Check(!ev.state.status_composing, "Escape ended the composition",
                   "still composing");
    rc = expect_no_commit(a, "Escape committed nothing");
  }
  if (rc == kOk) {
    rc = space(a, " ", &exp_c, "Space after Escape emits one space only");
  }

  // ---- 14. refused space events -------------------------------------------
  // Each refused event must reach the processor once (space_events_seen counts it,
  // space_events_refused names it), must NOT commit, and must leave the composition and
  // the menu exactly as they were.
  if (rc == kOk) {
    rc = type(a, "hel", "hel for the refused spaces");
  }
  struct Refused {
    int mask;
    const char* label;
  };
  const Refused refused[] = {
      {kControlMaskBit, "Ctrl+Space"},
      {kAltMaskBit, "Alt+Space"},
      {kSuperMaskBit, "Super+Space"},
      {kReleaseMaskBit, "Space release"},
  };
  for (const Refused& r : refused) {
    if (rc != kOk) {
      break;
    }
    rc = send(a, kXkSpace, r.mask, std::string("refused ") + r.label);
    if (rc != kOk) {
      break;
    }
    h.checks.Check(!ev.handled, std::string(r.label) + ": not handled by this policy",
                   ev.handled ? "True" : "False");
    h.checks.Check(ev.state.status_composing,
                   std::string(r.label) + ": the composition survived it",
                   "cleared");
    h.checks.CheckStr(ev.state.raw_input, "hel",
                      std::string(r.label) + ": raw composition untouched");
    h.checks.CheckMenu(ev.state, kMenuHel, std::string(r.label) + ": menu untouched");
    ++exp_refused;
    rc = expect_no_commit(a, std::string(r.label) + " committed nothing");
  }
  if (rc == kOk) {
    // The held-down keys did not damage the policy either: the next plain space is a
    // case A commit of the same raw text.
    rc = space(a, "hel ", &exp_a, "plain Space after the refused ones");
  }

  // ---- 14b. shift is NOT a blocking modifier -------------------------------
  // Only Ctrl, Alt and Super make a space somebody else's key combination
  // (engine/languages/en/english_space_policy.h: kSpaceBlockingMask). Shift+space is
  // still the user asking for a space, and refusing it would leave the space bar dead
  // while a shift key is held.
  if (rc == kOk) {
    rc = type(a, "hel", "hel for shift+Space");
  }
  if (rc == kOk) {
    rc = send(a, kXkSpace, kShiftMaskBit, "accepted shift+Space");
  }
  if (rc == kOk) {
    h.checks.Check(ev.handled, "shift+Space was accepted",
                   ev.handled ? "True" : "False");
    h.checks.Check(!ev.state.status_composing, "shift+Space ended the composition",
                   "still composing");
    ++exp_a;  // it resolves to case A, exactly like a plain space
    rc = read_commit(a, "hel ", "shift+Space committed the raw token plus one space");
  }

  // ---- 15. BackSpace on an empty composition is left to the host -----------
  if (rc == kOk) {
    rc = send(a, kXkBackSpace, 0, "BackSpace on an empty composition");
  }
  if (rc == kOk) {
    h.checks.Check(!ev.handled,
                   "BackSpace with nothing composing is not consumed here",
                   ev.handled ? "True" : "False");
    h.checks.Check(!ev.state.status_composing, "and the composition stayed empty",
                   "composing");
    rc = expect_no_commit(a, "that BackSpace committed nothing");
  }

  // ---- 16. two sessions interleaved ---------------------------------------
  const RimeSessionId b = CreateSessionOrZero(h.api, "SPACE-B");
  if (b == kNoSession) {
    h.api->destroy_session(a);
    return kCreateSessionFailed;
  }
  if (rc == kOk) {
    rc = SelectAndConfirmSchema(h.api, b, &h.balance, &h.checks, "SPACE-B",
                                kCompletionSchemaId);
  }
  if (rc == kOk) {
    rc = type(b, "app", "B types app");
  }
  if (rc == kOk) {
    h.checks.CheckMenu(ev.state, kMenuApp, "B has its own menu");
    // A is idle while B is mid-word: read back, not assumed.
    StateSnapshot a_now;
    ExitCode failure = kOk;
    if (!ReadState(h.api, a, &h.balance, &a_now, &failure)) {
      h.api->destroy_session(b);
      h.api->destroy_session(a);
      return failure;
    }
    h.checks.Check(!a_now.status_composing, "A is idle while B composes", "composing");
    h.checks.CheckStr(a_now.raw_input, "", "A has no raw composition");
    h.checks.CheckNum(a_now.num_candidates, 0, "A has no menu");
  }
  if (rc == kOk) {
    rc = type(a, "he", "A types he while B holds app");
  }
  if (rc == kOk) {
    h.checks.CheckMenu(ev.state, kMenuHe, "A has its own menu");
    StateSnapshot b_now;
    ExitCode failure = kOk;
    if (!ReadState(h.api, b, &h.balance, &b_now, &failure)) {
      h.api->destroy_session(b);
      h.api->destroy_session(a);
      return failure;
    }
    h.checks.CheckStr(b_now.raw_input, "app", "B still holds its own composition");
    h.checks.CheckMenu(b_now, kMenuApp, "and its own menu");
  }
  if (rc == kOk) {
    rc = space(b, "app ", &exp_a, "B + Space commits B's text");
  }
  if (rc == kOk) {
    rc = space(a, "he ", &exp_a, "A + Space commits A's text, one letter shorter");
  }

  // ---- 17. the counters, as identities ------------------------------------
  if (rc == kOk && !ReadCounters(h.surface, &c)) {
    rc = kCounterReadFailed;
  }
  if (rc == kOk) {
    std::printf("[english] counters.after_matrix      %s\n", SpaceCounterLine(c).c_str());
    std::printf("[english] transcript                 \"%s\"\n", transcript.c_str());
    h.checks.CheckNum(LL(c.space_case_a_raw_token), exp_a,
                      "case A happened exactly as many times as this run pressed it");
    h.checks.CheckNum(LL(c.space_case_b_selection), exp_b,
                      "case B happened once per explicit selection");
    h.checks.CheckNum(LL(c.space_case_c_empty), exp_c,
                      "case C happened once per empty-composition space");
    h.checks.CheckNum(LL(c.space_events_refused), exp_refused,
                      "the refused space events were counted as refusals");
    h.checks.CheckNum(LL(c.space_events_seen),
                      LL(c.space_case_a_raw_token) + LL(c.space_case_b_selection) +
                          LL(c.space_case_c_empty) + LL(c.space_events_refused),
                      "space_events_seen == a + b + c + refused");
    h.checks.CheckNum(LL(c.space_characters_emitted),
                      LL(c.space_case_a_raw_token) + LL(c.space_case_b_selection) +
                          LL(c.space_case_c_empty),
                      "space_characters_emitted == a + b + c");
    h.checks.CheckNum(LL(c.composition_commits_issued),
                      LL(c.space_case_a_raw_token) + LL(c.space_case_b_selection),
                      "composition_commits_issued == a + b (case C commits no text)");
    // Every injected key reaches this processor exactly once, letters included: it is
    // first in engine/processors and returns kNoop for everything that is not a plain
    // space (src/rime/engine.cc:99-108). A mismatch would mean it is either out of order
    // or swallowing keys that are not its business.
    h.checks.CheckNum(LL(c.processor_key_events), keys_sent,
                      "processor_key_events == the keys this run injected");
    h.checks.CheckNum(LL(c.lexicon_load_attempts), 1,
                      "the spaces and commits never re-read the word list");
    h.checks.CheckNum(LL(c.loaded_entry_count), kExpectedLexiconRows,
                      "and it is still the same 54 rows");
    h.checks.CheckNum(LL(c.queries_covering_part_of_input), 0,
                      "no token was handed over as a fragment");
    h.checks.CheckNum(LL(c.queries_covering_whole_input), LL(c.queries),
                      "and every query covered the whole token");
    h.checks.CheckNum(SpellingTotal(c), 0,
                      "the deployed schema asked for no spelling suggestions");
    h.checks.CheckNum(LL(c.space_processor_live),
                      LL(c.space_processor_constructed) -
                          LL(c.space_processor_destroyed),
                      "space processor live == constructed - destroyed");
  }

  if (rc == kOk) {
    rc = DestroySessionAndCheck(h.api, b, &h.checks, "SPACE session B");
  }
  const ExitCode done_a = DestroySessionAndCheck(h.api, a, &h.checks, "SPACE session A");
  if (done_a != kOk && rc == kOk) {
    rc = done_a;
  }
  if (!ReadCounters(h.surface, &c)) {
    return kCounterReadFailed;
  }
  std::printf("[english] counters.final             %s\n", SpaceCounterLine(c).c_str());
  h.checks.CheckNum(LL(c.space_processor_live), 0,
                    "no space processor outlived its session");
  h.checks.CheckNum(LL(c.space_processor_constructed),
                    LL(c.space_processor_destroyed),
                    "space processor constructed == destroyed");
  return rc;
}

ExitCode RunSpaceSelfCheckFailMode(Harness& h) {
  if (!h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "space-self.surface",
         "this control needs the english engine to have a real space bar to get wrong");
    return kEnglishSurfaceUnavailable;
  }
  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }
  const RimeSessionId a = CreateSessionOrZero(h.api, "SPACE-SELF");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "SPACE-SELF",
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
    rc = HighlightCandidate(h.api, a, 1, &moved);
  }
  if (rc == kOk) {
    rc = SendKey(h.api, a, kXkSpace, 0, &h.balance, &events.emplace_back());
  }
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  // The claim this mode makes is the forbidden one: that a space accepts the highlighted
  // completion. Case A commits the raw token, so this must fail.
  h.checks.CheckStr(events.back().state.commit_text, "hello ",
                    "deliberately asserted that a highlight is an acceptance");
  const ExitCode done = DestroySessionAndCheck(h.api, a, &h.checks, "SPACE-SELF");
  if (done != kOk) {
    return done;
  }
  return kOk;  // the harness turns the recorded failure into exit 14
}

}  // namespace recovery
}  // namespace epocanva
