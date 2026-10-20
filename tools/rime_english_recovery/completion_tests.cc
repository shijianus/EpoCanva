// EpoCanva P2.6-recovery -- the rebuilt English prefix-completion regression.
// See completion_tests.h for where the expectations come from and why they are not
// read back out of the code under test.
#include "completion_tests.h"

#include <cstdio>
#include <string>
#include <vector>

namespace epocanva {
namespace recovery {
namespace {

// The menus below were written by hand from the fixture's rows, not produced by a run.
// Derivation, for each prefix: the rows of lexicon.tsv that start with it, weighted;
// candidate 0 is the typed text; a row equal to the typed text is dropped from the
// completion tail; the tail is sorted by weight descending with ties broken by
// ascending ASCII; at most 7 completions survive.
//
//   "h"   -> hello(10) help(10) hen(6) hel(5) helmet(4) he(2) herb(1)          = 7
//   "he"  -> hello(10) help(10) hen(6) hel(5) helmet(4) herb(1)    ["he" deduped]
//   "hel" -> hello(10) help(10) helmet(4)                           ["hel" deduped]
//           (at weight 10 "hello" precedes "help": 'l' 0x6C < 'p' 0x70)
//   "app" -> apple(7) application(7) apply(4)   ("apple" < "application": they agree
//           through "appl", then 'e' 0x65 < 'i' 0x69)
//   "en"  -> enable(9) enjoy(9) | enforce(8) engine(8) enlarge(8) enough(8) envelope(8)
//           -- the weight-8 rows ordered by their third byte f<g<l<o<v; those are
//           already 7 completions, so enemy, english, enlist, enormous, ensure, entire,
//           entry and envy fall off the end of the page.
//   "zzz" -> nothing but the typed text: the row zzz(1) equals it and is deduped.
constexpr char kMenuH[] = "h|hello|help|hen|hel|helmet|he|herb";
constexpr char kMenuHe[] = "he|hello|help|hen|hel|helmet|herb";
constexpr char kMenuHel[] = "hel|hello|help|helmet";
constexpr char kMenuApp[] = "app|apple|application|apply";
constexpr char kMenuEn[] = "en|enable|enjoy|enforce|engine|enlarge|enough|envelope";
constexpr char kMenuZzz[] = "zzz";

// The fixture's schema turns no suggestion feature on, so every entry in the comment
// column must be empty: a label here would mean this run deployed a different schema
// than the one it staged.
std::string EmptyCommentColumn(std::size_t items) {
  return items == 0 ? std::string() : std::string(items - 1, '|');
}

std::string U64(unsigned long long v) { return std::to_string(v); }

std::string OrAbsent(bool present, const std::string& text) {
  return present ? text : std::string("absent");
}

}  // namespace

ExitCode RunCompletionMode(Harness& h) {
  if (!h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "completion.surface",
         "this run needs the english engine; the loaded DLL exposes no "
         "epocanva_english module through find_module()/get_api()");
    return kEnglishSurfaceUnavailable;
  }
  // Ties the behaviour about to be observed to the exact component that produced it.
  h.checks.CheckStr(std::string(h.surface.api->build_id()), kExpectedBuildId,
                    "component build id");

  EpoCanvaEnglishCounters c;
  if (!ReadCounters(h.surface, &c)) {
    Fail(kCounterReadFailed, "completion.counters", "get_counters() returned False");
    return kCounterReadFailed;
  }
  std::printf("[english] counters.after_init        %s\n",
              DescribeCounters(c).c_str());
  // The DLL-load-time initializer only REGISTERS the module (src/rime_api.h:528-533);
  // module_initialized moves only when ModuleManager::LoadModule calls initialize()
  // (src/rime/module.cc:25-37), so this is the acceptance that the engine's own loading
  // mechanism ran our component.
  h.checks.Check(c.module_initialized >= 1, "the module's initialize() ran",
                 U64(c.module_initialized));
  h.checks.CheckNum(static_cast<long long>(c.module_finalized), 0,
                    "module not finalized while the surface is still held");
  h.checks.CheckNum(static_cast<long long>(c.translator_constructed), 0,
                    "no translator object before the first session");
  h.checks.CheckNum(static_cast<long long>(c.lexicon_load_attempts), 0,
                    "no word list read before the first session");
  h.checks.CheckNum(static_cast<long long>(c.queries), 0,
                    "no query before the first session");

  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }

  long long letter_keys = 0;  // keystrokes injected here, for the query floor
  std::vector<KeyEvent> events;

  auto type = [&](RimeSessionId sid, const std::string& text) -> ExitCode {
    const ExitCode rc = TypeText(h.api, sid, text, &h.balance, &events);
    if (rc == kOk) {
      letter_keys += static_cast<long long>(text.size());
    }
    return rc;
  };
  auto key = [&](RimeSessionId sid, int keycode, KeyEvent* out) -> ExitCode {
    return SendKey(h.api, sid, keycode, 0, &h.balance, out);
  };
  auto check_frame = [&](const KeyEvent& e, const std::string& want_raw,
                         const std::string& want_menu, const std::string& label) {
    const std::vector<std::string> want = SplitPipeList(want_menu);
    h.checks.Check(e.handled, label + ": process_key() handled the key",
                   e.handled ? "True" : "False");
    h.checks.CheckStr(e.state.raw_input, want_raw,
                      label + ": get_input() raw composition");
    h.checks.Check(e.state.has_preedit, label + ": preedit present",
                   OrAbsent(e.state.has_preedit, e.state.preedit));
    h.checks.CheckStr(e.state.preedit, want_raw, label + ": preedit text");
    h.checks.CheckNum(e.state.preedit_length,
                      static_cast<long long>(want_raw.size()),
                      label + ": preedit length (UTF-8 code units)");
    h.checks.CheckNum(e.state.num_candidates, static_cast<long long>(want.size()),
                      label + ": candidate count");
    h.checks.CheckNum(e.state.page_size, 8, label + ": menu page_size");
    h.checks.CheckMenu(e.state, want_menu, label + ": menu");
    h.checks.CheckStr(JoinComments(e.state), EmptyCommentColumn(want.size()),
                      label + ": empty comment column");
    if (!want.empty()) {
      // Asserted apart from the tail: a component that dropped the original text and
      // still sorted the rest correctly would otherwise pass.
      h.checks.CheckStr(e.state.candidates[0], want_raw,
                        label + ": candidate 0 is the typed text");
    }
  };

  const RimeSessionId a = CreateSessionOrZero(h.api, "A");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  if (!ReadCounters(h.surface, &c)) {
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  h.checks.Check(c.translator_constructed >= 1,
                 "creating a session constructed the translator",
                 U64(c.translator_constructed));
  h.checks.Check(c.translator_live >= 1, "and the instance is alive",
                 U64(c.translator_live));

  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "A",
                                       kCompletionSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }

  // The schema resolved and the word list it names is readable: attempts/ok/fail/
  // entries/errline plus the status NAME, because the candidate list alone cannot tell
  // "loaded 54 rows" from "loaded nothing and fell back to the typed text".
  if (!ReadCounters(h.surface, &c)) {
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  std::printf("[english] counters.after_select      %s\n",
              DescribeCounters(c).c_str());
  h.checks.CheckNum(static_cast<long long>(c.lexicon_load_attempts), 1,
                    "the word list was read exactly once");
  h.checks.CheckNum(static_cast<long long>(c.lexicon_load_successes), 1,
                    "and it loaded");
  h.checks.CheckNum(static_cast<long long>(c.lexicon_load_failures), 0,
                    "with no failure recorded");
  h.checks.CheckNum(static_cast<long long>(c.loaded_entry_count),
                    kExpectedLexiconRows, "every row of the fixture arrived");
  h.checks.CheckNum(static_cast<long long>(c.last_load_error_line), 0,
                    "no load error line");
  h.checks.CheckStr(LastStatusName(h.surface), "ok", "word list status name");

  // ---- 1. three growing prefixes, one real keystroke at a time ----------------
  rc = type(a, "h");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  check_frame(events[0], "h", kMenuH, "'h'");
  rc = type(a, "e");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  check_frame(events[1], "he", kMenuHe, "'he'");
  rc = type(a, "l");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  check_frame(events[2], "hel", kMenuHel, "'hel'");

  // ---- 2. Return with no selection commits the original text, exactly once ----
  KeyEvent committed;
  rc = key(a, kXkReturn, &committed);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.Check(committed.handled, "'Return' handled",
                 committed.handled ? "True" : "False");
  h.checks.Check(committed.state.commit_present, "'Return' produced a commit",
                 OrAbsent(committed.state.commit_present,
                          committed.state.commit_text));
  h.checks.CheckStr(committed.state.commit_text, "hel",
                    "Return with no selection commits the typed text");
  h.checks.Check(!committed.state.status_composing, "the composition ended",
                 "still composing");
  h.checks.Check(!committed.state.has_preedit, "preedit gone after the commit",
                 OrAbsent(committed.state.has_preedit, committed.state.preedit));
  h.checks.CheckStr(committed.state.raw_input, "",
                    "Return cleared the raw composition");
  h.checks.CheckNum(committed.state.num_candidates, 0, "no candidates left");
  // get_commit() consumes. Reading it again must find nothing, or the engine is handing
  // the same text out twice and any host would type the word twice.
  bool again = false;
  std::string again_text;
  rc = ReadCommitOnce(h.api, a, &h.balance, &again, &again_text);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.Check(!again, "the same commit is not offered a second time",
                 OrAbsent(again, again_text));

  // ---- 3. an explicit selection commits the completion, not the prefix --------
  rc = type(a, "hel");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  check_frame(events.back(), "hel", kMenuHel, "'hel' recomposed");
  bool accepted = false;
  rc = SelectCandidate(h.api, a, 1, &accepted);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.Check(accepted, "select_candidate(1) was accepted",
                 accepted ? "True" : "False");
  // Selection is not acceptance-by-commit: the queue must still be empty. This is the
  // observation that lets a later mode claim one space adds exactly one character.
  bool queued = false;
  std::string queued_text;
  rc = ReadCommitOnce(h.api, a, &h.balance, &queued, &queued_text);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.Check(!queued, "select_candidate() committed nothing by itself",
                 OrAbsent(queued, queued_text));
  rc = key(a, kXkReturn, &committed);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.CheckStr(committed.state.commit_text, "hello",
                    "Return after select_candidate(1) commits the whole word");
  h.checks.Check(committed.state.commit_text != "hel",
                 "and not the prefix again", committed.state.commit_text);
  rc = ReadCommitOnce(h.api, a, &h.balance, &again, &again_text);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.Check(!again, "that commit is consumed once", OrAbsent(again, again_text));

  // ---- 4. BackSpace recomputes the menu; it does not replay a stale one -------
  rc = type(a, "hel");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  check_frame(events.back(), "hel", kMenuHel, "'hel' once more");
  KeyEvent back;
  rc = key(a, kXkBackSpace, &back);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.Check(back.handled, "'BackSpace' handled", back.handled ? "True" : "False");
  check_frame(back, "he", kMenuHe, "'hel' -> BackSpace -> 'he'");

  // ---- 5. Escape cancels and leaves nothing behind ---------------------------
  KeyEvent esc;
  rc = key(a, kXkEscape, &esc);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.Check(!esc.state.status_composing, "Escape ended the composition",
                 "still composing");
  h.checks.Check(!esc.state.has_preedit, "Escape removed the preedit",
                 OrAbsent(esc.state.has_preedit, esc.state.preedit));
  h.checks.CheckStr(esc.state.raw_input, "", "Escape cleared the raw composition");
  h.checks.CheckNum(esc.state.num_candidates, 0, "Escape cleared the menu");
  h.checks.Check(!esc.state.commit_present, "cancelling committed nothing",
                 OrAbsent(esc.state.commit_present, esc.state.commit_text));

  // ---- 6. the candidate budget: a full page, then a deduped single row -------
  rc = type(a, "en");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  check_frame(events.back(), "en", kMenuEn, "'en' fills the page");
  rc = key(a, kXkEscape, &esc);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  rc = type(a, "zzz");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  check_frame(events.back(), "zzz", kMenuZzz, "'zzz' dedupes its own row");
  // A has to be released from that composition before the isolation claim is made,
  // otherwise "session A is idle" would simply be a stale expectation: A is still
  // legitimately composing here.
  rc = key(a, kXkEscape, &esc);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.Check(!esc.state.status_composing,
                 "session A is idle before session B starts typing", "composing");

  // ---- 7. two sessions, two states ------------------------------------------
  const RimeSessionId b = CreateSessionOrZero(h.api, "B");
  if (b == kNoSession) {
    h.api->destroy_session(a);
    return kCreateSessionFailed;
  }
  rc = SelectAndConfirmSchema(h.api, b, &h.balance, &h.checks, "B",
                              kCompletionSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return rc;
  }
  rc = type(b, "app");
  if (rc != kOk) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return rc;
  }
  check_frame(events.back(), "app", kMenuApp, "'app' on session B");
  // Session A must still read as idle while B is mid-composition: this direction is
  // what proves the two sessions do not share a composition.
  StateSnapshot a_now;
  ExitCode failure = kOk;
  if (!ReadState(h.api, a, &h.balance, &a_now, &failure)) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return failure;
  }
  h.checks.Check(!a_now.status_composing, "session A is still idle", "composing");
  h.checks.Check(!a_now.has_preedit, "session A has no preedit",
                 OrAbsent(a_now.has_preedit, a_now.preedit));
  h.checks.CheckStr(a_now.raw_input, "", "session A has no raw composition");
  h.checks.CheckNum(a_now.num_candidates, 0, "session A offers no candidates");
  rc = key(b, kXkReturn, &committed);
  if (rc != kOk) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return rc;
  }
  // B commits its own text, with no trace of A's history in it.
  h.checks.CheckStr(committed.state.commit_text, "app",
                    "session B commits its own text");

  // ---- 8. final accounting ----------------------------------------------------
  if (!ReadCounters(h.surface, &c)) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  std::printf("[english] counters.before_destroy    %s\n",
              DescribeCounters(c).c_str());
  // The word list is read once per process, not once per key: everything above injected
  // seventeen letter keys against a single load. That is the claim this phase exists to
  // make observable (docs/testing/rime-english-completion.md section 7).
  h.checks.CheckNum(static_cast<long long>(c.lexicon_load_attempts), 1,
                    "still exactly one word list read after every keystroke");
  h.checks.CheckNum(static_cast<long long>(c.loaded_entry_count),
                    kExpectedLexiconRows, "and still the same 54 rows");
  h.checks.Check(c.queries >= static_cast<unsigned long long>(letter_keys),
                 "every injected letter produced a query", U64(c.queries));
  h.checks.CheckNum(static_cast<long long>(c.lexicon_load_attempts), 1,
                    "the load count did not grow with the queries");
  // Bookkeeping identities: a query that produced neither an original-text candidate nor
  // a refusal, or a completion whose case shape was never recorded, shows up here
  // instead of hiding behind a plausible-looking menu.
  h.checks.CheckNum(static_cast<long long>(c.queries),
                    static_cast<long long>(c.original_candidates),
                    "queries == original_candidates");
  h.checks.CheckNum(static_cast<long long>(c.queries_with_index),
                    static_cast<long long>(c.queries) -
                        static_cast<long long>(c.queries_rejected),
                    "queries_with_index == queries - rejected");
  h.checks.CheckNum(static_cast<long long>(c.queries_rejected), 0,
                    "every input in this mode was a completable lowercase prefix");
  h.checks.CheckNum(static_cast<long long>(c.completion_candidates),
                    static_cast<long long>(c.completions_lowercase) +
                        static_cast<long long>(c.completions_initial_capital) +
                        static_cast<long long>(c.completions_uppercase),
                    "completions == the three case shapes");
  h.checks.CheckNum(static_cast<long long>(c.completions_initial_capital), 0,
                    "this mode typed no initial-capital input");
  h.checks.CheckNum(static_cast<long long>(c.completions_uppercase), 0,
                    "this mode typed no uppercase input");
  h.checks.CheckNum(static_cast<long long>(c.queries_covering_part_of_input), 0,
                    "no query was ever handed part of a composition");
  // Scope evidence, one of four together: get_input() carried the whole token, candidate 0
  // equalled it, the menu held nothing else, and the commit was byte-identical -- each of
  // those is asserted per frame above, so this counter corroborates rather than carries the
  // claim on its own.
  h.checks.CheckNum(static_cast<long long>(c.queries_covering_whole_input),
                    static_cast<long long>(c.queries),
                    "queries_covering_whole_input == queries (scope evidence 4 of 4)");
  h.checks.CheckNum(static_cast<long long>(c.module_finalized), 0,
                    "module still not finalized");

  ExitCode done = DestroySessionAndCheck(h.api, b, &h.checks, "session B");
  const ExitCode done_a = DestroySessionAndCheck(h.api, a, &h.checks, "session A");
  if (done_a != kOk && done == kOk) {
    done = done_a;
  }
  if (!ReadCounters(h.surface, &c)) {
    return kCounterReadFailed;
  }
  std::printf("[english] counters.final             %s\n", DescribeCounters(c).c_str());
  h.checks.CheckNum(static_cast<long long>(c.translator_live), 0,
                    "no translator instance outlives its session");
  h.checks.CheckNum(static_cast<long long>(c.translator_constructed),
                    static_cast<long long>(c.translator_destroyed),
                    "constructed == destroyed");
  return done;
}

ExitCode RunExpectModuleAbsentMode(Harness& h) {
  // A mode that asserts an absence reports its own code, so "the engine turned out to
  // be more capable than this run claimed" is never mistaken for an assertion slip.
  h.assertion_failure_code = kNegativeExpectationNotMet;

  if (h.surface.module_found || h.surface.api_available) {
    Fail(kNegativeExpectationNotMet, "absent.surface",
         "this run expects an engine with no epocanva_english module, but "
         "find_module() located it and its diagnostics table is reachable");
    return kNegativeExpectationNotMet;
  }
  Log("absent.surface", "find_module(\"epocanva_english\") returned NULL");

  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }

  const RimeSessionId a = CreateSessionOrZero(h.api, "absent-A");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "absent A",
                                       kCompletionSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  // The engine itself must be healthy: an absent component and a broken engine look
  // identical unless the composition and the commit are asserted as well.
  std::vector<KeyEvent> events;
  rc = TypeText(h.api, a, "hel", &h.balance, &events);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  const StateSnapshot& last = events.back().state;
  h.checks.CheckStr(last.raw_input, "hel", "the composition still formed");
  h.checks.CheckStr(last.preedit, "hel", "and the preedit still renders");
  h.checks.CheckNum(last.num_candidates, 0,
                    "no candidate at all: the fixture schema lists no other "
                    "translator, so nothing can offer even the typed text");
  h.checks.CheckNum(static_cast<long long>(last.candidates.size()), 0,
                    "and the menu array is empty");
  KeyEvent committed;
  rc = SendKey(h.api, a, kXkReturn, 0, &h.balance, &committed);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.CheckStr(committed.state.commit_text, "hel",
                    "Return still commits what was typed");
  return DestroySessionAndCheck(h.api, a, &h.checks, "absent session A");
}

ExitCode RunSelfCheckFailMode(Harness& h) {
  if (!h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "self-check.surface",
         "this mode needs the english engine to have something real to get wrong");
    return kEnglishSurfaceUnavailable;
  }
  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    // A deployment failure stops the run before its deliberate mistake and reports 8
    // rather than 14, which the registered case then catches as a mismatch.
    return deployed;
  }
  const RimeSessionId a = CreateSessionOrZero(h.api, "self-check");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "self-check",
                                       kCompletionSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  std::vector<KeyEvent> events;
  rc = TypeText(h.api, a, "hel", &h.balance, &events);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  // The mode's one assertion is wrong on purpose, and wrong about a fact this file
  // derives independently: no row of the fixture begins "hel" other than hel, hello,
  // help and helmet, so a fifth candidate cannot exist. Everything else in the run is
  // left correct, which is what makes the resulting failure specific to the assertion
  // rather than to the setup.
  h.checks.CheckMenu(events.back().state, "hel|hello|help|helmet|helicopter",
                     "deliberately wrong menu");
  const ExitCode done = DestroySessionAndCheck(h.api, a, &h.checks, "self-check A");
  if (done != kOk) {
    return done;
  }
  return kOk;  // the harness turns the recorded failure into exit 14
}

}  // namespace recovery
}  // namespace epocanva
