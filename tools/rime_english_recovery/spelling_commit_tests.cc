// EpoCanva P2.6-recovery -- the commit boundary with spelling suggestions on screen.
// See spelling_commit_tests.h for the rule being re-pinned and why a transcript built only
// from get_commit() is the point.
#include "spelling_commit_tests.h"

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "english_diagnostics.h"

namespace epocanva {
namespace recovery {
namespace {

// XK_space. The keysym and the byte it emits have the same value for printable ASCII, but
// they are different things -- an event identifier and a text payload -- so they are named
// separately, as the space policy header does.
constexpr int kXkSpace = 0x0020;

// The menus, hand-derived from the fixture's 16 rows exactly as
// tests/fixtures/english_spelling/README.md section 4 and section 6 document them.
constexpr char kMenuHelo[] = "helo|hello|help|hell";
constexpr char kLabels3[] = "|spell|spell|spell";
constexpr char kMenuWrld[] = "wrld|world";
constexpr char kLabel1[] = "|spell";
constexpr char kMenuHel[] = "hel|hello|help|hella|hell|helium";
constexpr char kNoLabels6[] = "|||||";
constexpr char kMenuHeloInitial[] = "Helo|Hello|Help|Hell";
constexpr char kMenuHELOUpper[] = "HELO|HELLO|HELP|HELL";
constexpr char kMenuEmail[] = "user@example.com";

long long LL(unsigned long long v) { return static_cast<long long>(v); }

std::string U64(unsigned long long v) { return std::to_string(v); }

// (the comment column is joined by the harness's own JoinComments: its separator rule is
// index-based, which is what keeps candidate 0's empty comment from eating the first pipe)

long long LabelledRows(const StateSnapshot& s) {
  long long n = 0;
  for (const std::string& c : s.comments) {
    if (c == EPOCANVA_ENGLISH_SPELLING_COMMENT) {
      ++n;
    }
  }
  return n;
}

std::string SpaceCounterLine(const EpoCanvaEnglishCounters& c) {
  char buf[320];
  std::snprintf(buf, sizeof(buf),
                "pk_events=%llu space_seen=%llu case_a=%llu case_b=%llu case_c=%llu "
                "commits_issued=%llu spaces_emitted=%llu sp_app=%llu sp_l=%llu sp_i=%llu "
                "sp_u=%llu attempts=%llu",
                static_cast<unsigned long long>(c.processor_key_events),
                static_cast<unsigned long long>(c.space_events_seen),
                static_cast<unsigned long long>(c.space_case_a_raw_token),
                static_cast<unsigned long long>(c.space_case_b_selection),
                static_cast<unsigned long long>(c.space_case_c_empty),
                static_cast<unsigned long long>(c.composition_commits_issued),
                static_cast<unsigned long long>(c.space_characters_emitted),
                static_cast<unsigned long long>(c.spelling_candidates_appended),
                static_cast<unsigned long long>(c.spelling_suggestions_lowercase),
                static_cast<unsigned long long>(c.spelling_suggestions_initial_capital),
                static_cast<unsigned long long>(c.spelling_suggestions_uppercase),
                static_cast<unsigned long long>(c.lexicon_load_attempts));
  return std::string(buf);
}

// How the composition is put on screen before the key under test.
enum class Accept { kNone, kHighlight, kSelect };

struct CommitProbe {
  const char* label;
  const char* text;
  const char* wanted_menu;
  const char* wanted_comments;
  Accept accept;
  std::size_t index;
  // Which space rule the event must resolve to.
  bool via_space;
  // What the commit must be, byte for byte, from the fixture documentation. The loop does
  // not trust this alone: it re-derives the same string from the row it read off the menu
  // and requires the two to agree before comparing either with the engine.
  const char* wanted_commit;
};

const CommitProbe kCommitProbes[] = {
    // Suggestions are on screen the whole time; the default keys must still choose the text
    // that was typed.
    {"default Return with 3 suggestions", "helo", kMenuHelo, kLabels3, Accept::kNone, 0,
     false, "helo"},
    {"default Space with 3 suggestions", "helo", kMenuHelo, kLabels3, Accept::kNone, 0, true,
     "helo "},
    // Moving the highlight is not accepting anything: the next space still commits the raw
    // token, even though index 1 is visibly "hello".
    {"highlight a suggestion then Space", "helo", kMenuHelo, kLabels3, Accept::kHighlight,
     1, true, "helo "},
    // An explicit selection is a different state, and it is what the key commits -- once.
    {"select a suggestion then Return", "helo", kMenuHelo, kLabels3, Accept::kSelect, 1,
     false, "hello"},
    {"select a suggestion then Space", "helo", kMenuHelo, kLabels3, Accept::kSelect, 1, true,
     "hello "},
    // The selected suggestion keeps the shape that was typed, in both accepted non-lowercase
    // shapes.
    {"select in uppercase", "HELO", kMenuHELOUpper, kLabels3, Accept::kSelect, 1, true,
     "HELLO "},
    {"select in initial capital", "Helo", kMenuHeloInitial, kLabels3, Accept::kSelect, 1,
     true, "Hello "},
    // A suppressed input commits exactly the way it did before P2.6.
    {"Space after protected text", "user@example.com", kMenuEmail, "", Accept::kNone, 0,
     true, "user@example.com "},
};

}  // namespace

ExitCode RunSpellingCommitMode(Harness& h) {
  if (!h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "spellcommit.surface",
         "this run needs the english engine; the loaded DLL exposes no "
         "epocanva_english module through find_module()/get_api()");
    return kEnglishSurfaceUnavailable;
  }
  h.checks.CheckStr(std::string(h.surface.api->build_id()), kExpectedBuildId,
                    "component build id");

  EpoCanvaEnglishCounters c;
  if (!ReadCounters(h.surface, &c)) {
    Fail(kCounterReadFailed, "spellcommit.counters", "get_counters() returned False");
    return kCounterReadFailed;
  }
  h.checks.CheckNum(SpellingTotal(c), 0, "no spelling counter before the first query");

  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }

  const RimeSessionId a = CreateSessionOrZero(h.api, "SPELLC-A");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "spellcommit A",
                                       kSpellingSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  if (!ReadCounters(h.surface, &c)) {
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  h.checks.CheckNum(LL(c.lexicon_load_attempts), 1, "the 16-row list was read once");
  h.checks.CheckNum(LL(c.loaded_entry_count), kExpectedSpellingLexiconRows,
                    "every controlled row reached the index");
  h.checks.CheckStr(LastStatusName(h.surface), "ok", "spelling word list status");

  long long keys_sent = 0;
  long long exp_a = 0, exp_b = 0, exp_c = 0;
  std::string transcript;  // only bytes get_commit() actually returned
  KeyEvent ev;

  auto send = [&](RimeSessionId sid, int keysym, int mask,
                  const std::string& label) -> ExitCode {
    ++keys_sent;
    const ExitCode prc = SendKey(h.api, sid, keysym, mask, &h.balance, &ev);
    if (prc != kOk) {
      return prc;
    }
    std::printf("[english] ev=%03lld key=0x%04x mask=%d %-40s handled=%d %s\n", keys_sent,
                keysym, mask, label.c_str(), ev.handled ? 1 : 0,
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
  // The commit belongs to the event that produced it: SendKey already called get_commit(),
  // which CONSUMES, so a second read can only ever be the "not offered twice" half of the
  // claim. Asserting the event's own observation is what keeps this from reporting the
  // harness's second read as the engine's output.
  auto read_commit = [&](RimeSessionId sid, const std::string& want,
                         const std::string& label) -> ExitCode {
    h.checks.Check(ev.state.commit_present, label + ": the event produced a commit",
                   "none");
    h.checks.CheckStr(ev.state.commit_text, want, label + ": committed bytes");
    transcript += ev.state.commit_text;
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

  // ---- 1. the eight commit probes --------------------------------------------------------
  for (const CommitProbe& probe : kCommitProbes) {
    const std::string label(probe.label);
    EpoCanvaEnglishCounters before;
    if (!ReadCounters(h.surface, &before)) {
      h.api->destroy_session(a);
      return kCounterReadFailed;
    }
    rc = send(a, kXkEscape, 0, label + " [clear]");
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
    rc = type(a, probe.text, label + " [typing]");
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
    // Read the menu back BEFORE any acceptance action, so "what was on offer" is observed
    // rather than taken from this table, and so a selection or a highlight is provably not
    // yet a commit.
    StateSnapshot pre;
    ExitCode failure = kOk;
    if (!ReadState(h.api, a, &h.balance, &pre, &failure)) {
      h.api->destroy_session(a);
      return failure;
    }
    h.checks.CheckMenu(pre, probe.wanted_menu, label + " [menu when the key arrives]");
    h.checks.CheckStr(JoinComments(pre), probe.wanted_comments,
                      label + " [label column when the key arrives]");
    h.checks.CheckStr(pre.raw_input, probe.text, label + " [get_input() still the typed text]");
    if (!pre.candidates.empty()) {
      h.checks.CheckStr(pre.candidates[0], probe.text, label + " [candidate 0 is the raw text]");
    }
    h.checks.Check(!pre.commit_present, label + " [nothing committed before the key]",
                   pre.commit_present ? pre.commit_text : std::string("none"));

    const std::string picked =
        probe.index < pre.candidates.size() ? pre.candidates[probe.index]
                                             : std::string("<out of range>");
    if (probe.accept == Accept::kHighlight) {
      bool moved = false;
      rc = HighlightCandidate(h.api, a, probe.index, &moved);
      if (rc != kOk) {
        h.api->destroy_session(a);
        return rc;
      }
      h.checks.Check(moved, label + ": highlight_candidate() moved the highlight",
                     moved ? "True" : "False");
      StateSnapshot lit;
      if (!ReadState(h.api, a, &h.balance, &lit, &failure)) {
        h.api->destroy_session(a);
        return failure;
      }
      h.checks.CheckNum(lit.highlighted, static_cast<long long>(probe.index),
                        label + ": the highlight really is on that row");
      h.checks.CheckMenu(lit, probe.wanted_menu, label + ": the menu itself did not change");
      h.checks.CheckStr(JoinComments(lit), probe.wanted_comments,
                        label + ": the labels did not change");
      h.checks.Check(!lit.commit_present, label + ": a highlight committed nothing",
                     lit.commit_present ? lit.commit_text : std::string("absent"));
    }
    if (probe.accept == Accept::kSelect) {
      bool accepted = false;
      rc = SelectCandidate(h.api, a, probe.index, &accepted);
      if (rc != kOk) {
        h.api->destroy_session(a);
        return rc;
      }
      h.checks.Check(accepted, label + ": select_candidate() was accepted",
                     accepted ? "True" : "False");
      StateSnapshot mid;
      if (!ReadState(h.api, a, &h.balance, &mid, &failure)) {
        h.api->destroy_session(a);
        return failure;
      }
      // Selection is a step, not a commit: nothing in the queue, the composition still live,
      // and get_input() still the typed bytes. The menu is gone because ConcreteEngine::
      // OnSelect() closes the segment (src/rime/engine.cc:259-270) -- observed in the P2.5
      // group and re-observed here with a suggestion.
      h.checks.Check(!mid.commit_present, label + ": the selection committed nothing itself",
                     mid.commit_present ? mid.commit_text : std::string("absent"));
      h.checks.Check(mid.status_composing, label + ": the composition is still live",
                     "not composing");
      h.checks.CheckStr(mid.raw_input, probe.text, label + ": get_input() unchanged");
      h.checks.CheckNum(mid.num_candidates, 0,
                        label + ": the selection closed the segment, so no menu is offered");
    }
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }

    // The commit this event must produce, derived from what the menu actually showed: the
    // selected row when a selection was made, the typed text otherwise, plus one U+0020 when
    // the key was a space. Requiring this to equal the documented literal is what stops the
    // assertion from being a restatement of the table, and requiring the engine to equal it
    // is the assertion proper -- so a wrong expectation in this file cannot quietly become
    // the definition of correct.
    const std::string derived_commit =
        (probe.accept == Accept::kSelect ? picked : std::string(probe.text)) +
        (probe.via_space ? std::string(" ") : std::string());
    h.checks.CheckStr(derived_commit, probe.wanted_commit,
                      label + ": [commit = the observed row + one space when a space was "
                              "pressed]");

    const int commit_key = probe.via_space ? kXkSpace : kXkReturn;
    rc = send(a, commit_key, 0, label + " [commit key]");
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
    h.checks.Check(ev.handled, label + ": the commit key was handled",
                   ev.handled ? "True" : "False");
    h.checks.Check(!ev.state.status_composing, label + ": the composition ended",
                   "still composing");
    h.checks.Check(!ev.state.has_preedit, label + ": no preedit left",
                   ev.state.has_preedit ? ev.state.preedit : std::string("absent"));
    h.checks.CheckStr(ev.state.raw_input, "", label + ": raw composition cleared");
    h.checks.CheckNum(ev.state.num_candidates, 0, label + ": menu cleared");
    h.checks.CheckNum(LabelledRows(ev.state), 0,
                      label + ": no suggestion survived the commit");
    rc = read_commit(a, probe.wanted_commit, label);
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }

    EpoCanvaEnglishCounters after;
    if (!ReadCounters(h.surface, &after)) {
      h.api->destroy_session(a);
      return kCounterReadFailed;
    }
    if (probe.via_space) {
      const bool by_selection = (probe.accept == Accept::kSelect);
      if (by_selection) {
        ++exp_b;
      } else {
        ++exp_a;
      }
      h.checks.CheckNum(
          CounterDelta(before, after, &EpoCanvaEnglishCounters::space_case_a_raw_token),
          by_selection ? 0 : 1, label + ": space case A");
      h.checks.CheckNum(
          CounterDelta(before, after, &EpoCanvaEnglishCounters::space_case_b_selection),
          by_selection ? 1 : 0, label + ": space case B");
      h.checks.CheckNum(
          CounterDelta(before, after, &EpoCanvaEnglishCounters::space_case_c_empty), 0,
          label + ": no empty-composition space");
      h.checks.CheckNum(
          CounterDelta(before, after,
                       &EpoCanvaEnglishCounters::composition_commits_issued),
          1, label + ": the composing text was committed exactly once");
      h.checks.CheckNum(
          CounterDelta(before, after, &EpoCanvaEnglishCounters::space_characters_emitted), 1,
          label + ": exactly one U+0020 was emitted");
    } else {
      // Return is the editor's, not the space policy's: no case counter may move.
      h.checks.CheckNum(
          CounterDelta(before, after, &EpoCanvaEnglishCounters::space_case_a_raw_token) +
              CounterDelta(before, after, &EpoCanvaEnglishCounters::space_case_b_selection) +
              CounterDelta(before, after, &EpoCanvaEnglishCounters::space_case_c_empty),
          0, label + ": Return resolved no space case at all");
      h.checks.CheckNum(
          CounterDelta(before, after, &EpoCanvaEnglishCounters::space_characters_emitted), 0,
          label + ": Return emitted no space character");
    }
    h.checks.CheckNum(
        CounterDelta(before, after, &EpoCanvaEnglishCounters::lexicon_load_attempts), 0,
        label + ": no word list re-read");
    h.checks.CheckNum(
        CounterDelta(before, after, &EpoCanvaEnglishCounters::queries_covering_part_of_input),
        0, label + ": the token reached the translator whole");
  }

  // ---- 2. Escape leaves no suggestion and no commit ------------------------------------
  rc = type(a, "helo", "escape probe [typing]");
  if (rc == kOk) {
    h.checks.CheckMenu(ev.state, kMenuHelo, "escape probe: suggestions on screen");
    rc = send(a, kXkEscape, 0, "escape probe [Escape]");
  }
  if (rc == kOk) {
    h.checks.Check(!ev.state.status_composing, "Escape ended the composition",
                   "still composing");
    h.checks.CheckNum(ev.state.num_candidates, 0, "Escape cleared the menu");
    h.checks.CheckNum(LabelledRows(ev.state), 0, "Escape left no labelled row behind");
    rc = expect_no_commit(a, "Escape committed nothing");
  }
  if (rc == kOk) {
    // A space after a cancelled composition must not revive the cancelled text.
    ++exp_c;
    rc = send(a, kXkSpace, 0, "Space on the empty composition");
    if (rc == kOk) {
      rc = read_commit(a, " ", "Space after Escape emits one space only");
    }
  }

  // ---- 3. after a suggestion commit, the next ordinary word behaves normally --------------
  if (rc == kOk) {
    const std::size_t before_len = transcript.size();
    rc = type(a, "helo", "next word probe [typing helo]");
    if (rc == kOk) {
      bool accepted = false;
      rc = SelectCandidate(h.api, a, 1, &accepted);
      h.checks.Check(accepted, "next word probe: the suggestion was selected",
                     accepted ? "True" : "False");
    }
    if (rc == kOk) {
      ++exp_b;
      rc = send(a, kXkSpace, 0, "next word probe [Space]");
    }
    if (rc == kOk) {
      rc = read_commit(a, "hello ", "a selected suggestion commits once, then one space");
    }
    if (rc == kOk) {
      rc = type(a, "hel", "next word probe [typing hel]");
    }
    if (rc == kOk) {
      // Completions are back, with no labels: the committed suggestion left no state behind
      // that could make the next token's menu look like a correction.
      h.checks.CheckMenu(ev.state, kMenuHel, "the next ordinary word still completes");
      h.checks.CheckStr(JoinComments(ev.state), kNoLabels6, "and nothing in it is labelled");
      ++exp_a;
      rc = send(a, kXkSpace, 0, "next word probe [Space 2]");
    }
    if (rc == kOk) {
      rc = read_commit(a, "hel ", "and it commits the text that was typed");
    }
    if (rc == kOk && transcript.size() >= before_len) {
      h.checks.CheckStr(transcript.substr(before_len), "hello hel ",
                        "the concatenated transcript of a suggestion then a plain word");
    }
  }

  // ---- 4. two sessions, two menus ----------------------------------------------------------
  const RimeSessionId b = CreateSessionOrZero(h.api, "SPELLC-B");
  if (b == kNoSession) {
    h.api->destroy_session(a);
    return kCreateSessionFailed;
  }
  if (rc == kOk) {
    rc = SelectAndConfirmSchema(h.api, b, &h.balance, &h.checks, "spellcommit B",
                               kSpellingSchemaId);
  }
  if (rc == kOk) {
    rc = type(b, "wrld", "session B [typing wrld]");
  }
  if (rc == kOk) {
    h.checks.CheckMenu(ev.state, kMenuWrld, "session B offers its own suggestion");
    h.checks.CheckStr(JoinComments(ev.state), kLabel1, "and labels exactly one row");
    StateSnapshot a_now;
    ExitCode failure = kOk;
    if (!ReadState(h.api, a, &h.balance, &a_now, &failure)) {
      h.api->destroy_session(b);
      h.api->destroy_session(a);
      return failure;
    }
    h.checks.Check(!a_now.status_composing, "session A is idle while B composes",
                   "composing");
    h.checks.CheckStr(a_now.raw_input, "", "session A holds no composition");
    h.checks.CheckNum(a_now.num_candidates, 0, "session A offers no menu");
  }
  if (rc == kOk) {
    rc = type(a, "helo", "session A [typing helo while B holds wrld]");
  }
  if (rc == kOk) {
    h.checks.CheckMenu(ev.state, kMenuHelo, "session A has its own menu");
    StateSnapshot b_now;
    ExitCode failure = kOk;
    if (!ReadState(h.api, b, &h.balance, &b_now, &failure)) {
      h.api->destroy_session(b);
      h.api->destroy_session(a);
      return failure;
    }
    h.checks.CheckStr(b_now.raw_input, "wrld", "session B still holds its own composition");
    h.checks.CheckMenu(b_now, kMenuWrld, "and its own menu");
  }
  if (rc == kOk) {
    ++exp_a;
    rc = send(b, kXkSpace, 0, "session B [Space]");
  }
  if (rc == kOk) {
    rc = read_commit(b, "wrld ", "session B commits its own suggestion's target text");
  }
  if (rc == kOk) {
    ++exp_a;
    rc = send(a, kXkSpace, 0, "session A [Space]");
  }
  if (rc == kOk) {
    rc = read_commit(a, "helo ", "session A commits its own raw text, one key shorter");
  }

  // ---- 5. the counters, as identities --------------------------------------------------------
  if (rc == kOk && !ReadCounters(h.surface, &c)) {
    rc = kCounterReadFailed;
  }
  if (rc == kOk) {
    std::printf("[english] counters.after_matrix      %s\n", SpaceCounterLine(c).c_str());
    std::printf("[english] transcript                 \"%s\"\n", transcript.c_str());
    h.checks.CheckNum(LL(c.space_case_a_raw_token), exp_a,
                      "case A happened exactly as often as this run pressed a plain space "
                      "with nothing selected");
    h.checks.CheckNum(LL(c.space_case_b_selection), exp_b,
                      "case B happened exactly once per explicit selection");
    h.checks.CheckNum(LL(c.space_case_c_empty), exp_c,
                      "case C happened exactly once per empty-composition space");
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
                      "composition_commits_issued == a + b");
    h.checks.CheckNum(LL(c.processor_key_events), keys_sent,
                      "processor_key_events == the keys this run injected");
    h.checks.CheckNum(LL(c.spelling_suggestions_lowercase +
                         c.spelling_suggestions_initial_capital +
                         c.spelling_suggestions_uppercase),
                      LL(c.spelling_candidates_appended),
                      "suggestions appended = the three case shapes");
    h.checks.CheckNum(LL(c.spelling_lexicon_lookups), LL(c.spelling_variants_generated),
                      "one index lookup per generated variant, cumulatively");
    h.checks.Check(c.spelling_suggestions_initial_capital >= 1,
                   "the initial-capital shape was really rendered",
                   U64(c.spelling_suggestions_initial_capital));
    h.checks.Check(c.spelling_suggestions_uppercase >= 1, "the uppercase shape was really "
                   "rendered",
                   U64(c.spelling_suggestions_uppercase));
    h.checks.CheckNum(LL(c.lexicon_load_attempts), 1,
                      "two sessions, one word list read");
    h.checks.CheckNum(LL(c.queries_covering_part_of_input), 0,
                      "no token was handed over as a fragment");
    h.checks.CheckNum(LL(c.queries_covering_whole_input), LL(c.queries),
                      "and every query covered the whole token");
    h.checks.CheckNum(LL(c.spelling_lexicon_unavailable), 0,
                      "this mode deployed a readable list, so nothing reported it missing");
  }

  if (rc == kOk) {
    rc = DestroySessionAndCheck(h.api, b, &h.checks, "spellcommit session B");
  }
  const ExitCode done_a = DestroySessionAndCheck(h.api, a, &h.checks, "spellcommit session A");
  if (done_a != kOk && rc == kOk) {
    rc = done_a;
  }
  if (!ReadCounters(h.surface, &c)) {
    return kCounterReadFailed;
  }
  h.checks.CheckNum(LL(c.translator_live), 0, "no translator outlives its session");
  return rc;
}

}  // namespace recovery
}  // namespace epocanva
