// EpoCanva P2.2-recovery -- the ASCII letter-case regression.
// See case_tests.h for the scope of what this proves, how the key events are really
// sent, and where the expectations came from.
#include "case_tests.h"

#include <cstdio>
#include <string>
#include <vector>

#include "english_diagnostics.h"

namespace epocanva {
namespace recovery {
namespace {

// Every list below was written out of tests/fixtures/english_completion/lexicon.tsv by
// hand. The rows in play are he(2) hel(5) hello(10) help(10) helmet(4) hen(6) herb(1);
// candidate 0 is always the typed text; a row whose rendered text equals the typed text
// is dropped; the tail is weight-descending with ties broken by ascending ASCII of the
// CANONICAL row, so case changes the rendering and never the order.
//
//   h    -> hello help hen hel helmet he herb                    (7, budget full)
//   He   -> the same 7 rows rendered initial-capital; row "he" renders as "He" which
//           equals the input and is dropped                             -> 6 + 1 = 7
//   HEL  -> rows starting hel: hello(10) help(10) helmet(4), "hel" itself renders as
//           "HEL" and is dropped                                          -> 3 + 1 = 4
//   HE   -> hello help hen hel helmet herb (row "he" dropped, rendered uppercase)
// At weight 10 "hello" precedes "help" because their 4th bytes are 'l' 0x6C and 'p'
// 0x70; "Hello" likewise precedes "Help".
constexpr char kMenuHLower[] = "h|hello|help|hen|hel|helmet|he|herb";
constexpr char kMenuHInitial[] = "H|Hello|Help|Hen|Hel|Helmet|He|Herb";
constexpr char kMenuHeLower[] = "he|hello|help|hen|hel|helmet|herb";
constexpr char kMenuHeInitial[] = "He|Hello|Help|Hen|Hel|Helmet|Herb";
constexpr char kMenuHeUpper[] = "HE|HELLO|HELP|HEN|HEL|HELMET|HERB";
constexpr char kMenuHelLower[] = "hel|hello|help|helmet";
constexpr char kMenuHelInitial[] = "Hel|Hello|Help|Helmet";
constexpr char kMenuHelUpper[] = "HEL|HELLO|HELP|HELMET";
// Mixed case is not guessed at, so the menu is the typed text alone (fixture contract:
// rules 3 to 7 in engine/languages/en/english_case.h, and rule 8 catches everything
// else). "Hello" is different in kind: it IS a completable shape whose rendered row
// equals the input, so the single candidate is a dedup and not a refusal.
constexpr char kMenuHeLMixed[] = "HeL";
constexpr char kMenuHeMixed[] = "hE";
constexpr char kMenuHeLMixed2[] = "hEL";
constexpr char kMenuHelloDedup[] = "Hello";

long long LL(unsigned long long v) { return static_cast<long long>(v); }

}  // namespace

ExitCode RunCasePreservationMode(Harness& h) {
  if (!h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "case.surface",
         "this run needs the english engine; the loaded DLL exposes no "
         "epocanva_english module through find_module()/get_api()");
    return kEnglishSurfaceUnavailable;
  }
  h.checks.CheckStr(std::string(h.surface.api->build_id()), kExpectedBuildId,
                    "component build id");

  EpoCanvaEnglishCounters c;
  if (!ReadCounters(h.surface, &c)) {
    Fail(kCounterReadFailed, "case.counters", "get_counters() returned False");
    return kCounterReadFailed;
  }
  h.checks.CheckNum(LL(c.lexicon_load_attempts), 0, "no word list read yet");

  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }

  std::vector<KeyEvent> ev;
  long long letter_keys = 0;

  const RimeSessionId a = CreateSessionOrZero(h.api, "CASE-A");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "CASE-A",
                                       kCompletionSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }

  // Types `text` and then asserts every frame it produced against the raw composition
  // and menu listed for it, in order. A shape change is only proven by the frames that
  // lead up to it: "HEL" reaching the uppercase menu means nothing if "H" silently
  // rendered as lowercase.
  auto type_frames = [&](RimeSessionId sid, const std::string& text,
                         const std::vector<std::string>& raws,
                         const std::vector<std::string>& menus,
                         const std::string& label) -> ExitCode {
    if (raws.size() != menus.size()) {
      h.checks.Check(false, label + ": test is self-inconsistent",
                     "raw/frame count mismatch");
      return kUsage;
    }
    const std::size_t base = ev.size();
    const ExitCode trc = TypeText(h.api, sid, text, &h.balance, &ev);
    if (trc != kOk) {
      return trc;
    }
    letter_keys += static_cast<long long>(text.size());
    h.checks.CheckNum(static_cast<long long>(ev.size() - base),
                      static_cast<long long>(raws.size()),
                      label + ": one frame per keystroke");
    for (std::size_t i = 0; i < raws.size(); ++i) {
      if (base + i < ev.size()) {
        CheckFrame(&h.checks, ev[base + i], raws[i], menus[i], 8,
                   label + " frame '" + raws[i] + "'");
      }
    }
    return kOk;
  };

  auto commit_is_gone = [&](RimeSessionId sid, const std::string& label) -> ExitCode {
    bool present = false;
    std::string text;
    const ExitCode crc = ReadCommitOnce(h.api, sid, &h.balance, &present, &text);
    if (crc != kOk) {
      return crc;
    }
    h.checks.Check(!present, label + ": the commit queue is empty again",
                   present ? text : std::string("absent"));
    return kOk;
  };

  auto press = [&](RimeSessionId sid, int keycode, int mask,
                   KeyEvent* out) -> ExitCode {
    return SendKey(h.api, sid, keycode, mask, &h.balance, out);
  };

  // ---- 1. lowercase baseline --------------------------------------------------
  rc = type_frames(a, "hel", {"h", "he", "hel"}, {kMenuHLower, kMenuHeLower, kMenuHelLower},
                   "lowercase");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  KeyEvent ret;
  rc = press(a, kXkReturn, 0, &ret);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.CheckStr(ret.state.commit_text, "hel", "lowercase: Return commits the text");
  rc = commit_is_gone(a, "lowercase");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }

  // ---- 2. initial-capital: same rows, rendered in the shape typed ------------
  rc = type_frames(a, "Hel", {"H", "He", "Hel"},
                   {kMenuHInitial, kMenuHeInitial, kMenuHelInitial}, "initial-capital");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  rc = press(a, kXkReturn, 0, &ret);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  // The default commit is the user's own text, never a completion: no automatic
  // capitalisation, no "helpful" replacement.
  h.checks.CheckStr(ret.state.commit_text, "Hel",
                    "initial-capital: Return commits the original shape");
  h.checks.Check(ret.state.commit_text != "Hello",
                 "initial-capital: the commit is not the top completion",
                 ret.state.commit_text);
  rc = commit_is_gone(a, "initial-capital");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }

  // ---- 3. uppercase, then an explicit selection of the mapped completion ----
  rc = type_frames(a, "HEL", {"H", "HE", "HEL"},
                   {kMenuHInitial, kMenuHeUpper, kMenuHelUpper}, "uppercase");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  bool accepted = false;
  rc = SelectCandidate(h.api, a, 1, &accepted);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.Check(accepted, "uppercase: select_candidate(1) accepted",
                 accepted ? "True" : "False");
  rc = commit_is_gone(a, "uppercase after select");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  rc = press(a, kXkReturn, 0, &ret);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.CheckStr(ret.state.commit_text, "HELLO",
                    "uppercase: the selected completion commits in the typed shape");
  h.checks.Check(ret.state.commit_text != "HEL", "and it is not the prefix",
                 ret.state.commit_text);
  rc = commit_is_gone(a, "uppercase committed");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }

  // ---- 4. BackSpace recomputes the SHAPE as well as the menu -----------------
  rc = type_frames(a, "HEL", {"H", "HE", "HEL"},
                   {kMenuHInitial, kMenuHeUpper, kMenuHelUpper}, "uppercase again");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  KeyEvent back;
  rc = press(a, kXkBackSpace, 0, &back);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  CheckFrame(&h.checks, back, "HE", kMenuHeUpper, 8, "HEL -> BackSpace -> HE");

  KeyEvent esc;
  rc = press(a, kXkEscape, 0, &esc);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.Check(!esc.state.status_composing, "Escape cleared session A",
                 "still composing");

  // ---- 5. mixed case is refused, and there is nothing to select --------------
  rc = type_frames(a, "HeL", {"H", "He", "HeL"},
                   {kMenuHInitial, kMenuHeInitial, kMenuHeLMixed}, "mixed HeL");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  bool refused = true;
  rc = SelectCandidate(h.api, a, 1, &refused);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.Check(!refused, "mixed case: there is no index 1 to select",
                 refused ? "accepted" : "refused");
  rc = press(a, kXkReturn, 0, &ret);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.CheckStr(ret.state.commit_text, "HeL",
                    "mixed case: the untouched original is what commits");
  rc = commit_is_gone(a, "mixed HeL");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }

  rc = type_frames(a, "hEL", {"h", "hE", "hEL"},
                   {kMenuHLower, kMenuHeMixed, kMenuHeLMixed2}, "mixed hEL");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  rc = press(a, kXkEscape, 0, &esc);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }

  // ---- 6. an exact dictionary entry in another shape is a dedup, not a refusal
  //
  // This frame needs the counters, because "one candidate" is the same menu for both
  // explanations. The distinction is which bucket moved.
  if (!ReadCounters(h.surface, &c)) {
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  const EpoCanvaEnglishCounters before_hello = c;
  const std::string hello_text = "Hello";
  for (std::size_t i = 0; i < hello_text.size(); ++i) {
    KeyEvent frame;
    rc = press(a, static_cast<int>(static_cast<unsigned char>(hello_text[i])), 0,
               &frame);
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
    ++letter_keys;
    EpoCanvaEnglishCounters after;
    if (!ReadCounters(h.surface, &after)) {
      h.api->destroy_session(a);
      return kCounterReadFailed;
    }
    if (i + 1 == hello_text.size()) {
      CheckFrame(&h.checks, frame, "Hello", kMenuHelloDedup, 8,
                 "exact entry Hello: only the original is offered");
      // Across all five frames nothing may have been refused: a refusal would move
      // queries_rejected, and the menu alone cannot tell "looked it up, the row equals
      // the input" apart from "the shape was refused".
      h.checks.CheckNum(
          CounterDelta(before_hello, after,
                       &EpoCanvaEnglishCounters::queries_rejected),
          0, "Hello's frames were not refused as mixed, non-alphabetic or too long");
      h.checks.Check(
          CounterDelta(before_hello, after,
                       &EpoCanvaEnglishCounters::completions_initial_capital) >= 1,
          "Hello's completable prefixes did produce initial-capital completions",
          std::to_string(CounterDelta(before_hello, after,
                                      &EpoCanvaEnglishCounters::
                                          completions_initial_capital)));
    }
  }
  rc = press(a, kXkEscape, 0, &esc);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }

  // ---- 7. the event shape itself is an observed fact -------------------------
  // shift + 'h' is keysym 0x0068 with mask 1 (kShiftMask is bit 0,
  // src/rime/key_table.h:14-45). Speller::ProcessKeyEvent() looks only at keycode()
  // (src/rime/gear/speller.cc:104) and reads shift() only for XK_space (:107), so this
  // arrives as a lowercase 'h'. Asserting it keeps a later edit from "adding shift" and
  // quietly testing lowercase while claiming to test uppercase.
  KeyEvent shifted;
  rc = press(a, 0x0068, 1, &shifted);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  ++letter_keys;
  CheckFrame(&h.checks, shifted, "h", kMenuHLower, 8,
             "shift + h arrives as lowercase h");
  rc = press(a, kXkEscape, 0, &esc);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }

  // ---- 8. two sessions, uppercase in one and lowercase in the other ---------
  rc = type_frames(a, "HE", {"H", "HE"}, {kMenuHInitial, kMenuHeUpper},
                   "session A uppercase");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  const RimeSessionId b = CreateSessionOrZero(h.api, "CASE-B");
  if (b == kNoSession) {
    h.api->destroy_session(a);
    return kCreateSessionFailed;
  }
  rc = SelectAndConfirmSchema(h.api, b, &h.balance, &h.checks, "CASE-B",
                              kCompletionSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return rc;
  }
  rc = type_frames(b, "he", {"h", "he"}, {kMenuHLower, kMenuHeLower},
                   "session B lowercase");
  if (rc != kOk) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return rc;
  }
  // A is read back while B holds its own composition: A must still be uppercase.
  StateSnapshot a_now;
  ExitCode failure = kOk;
  if (!ReadState(h.api, a, &h.balance, &a_now, &failure)) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return failure;
  }
  h.checks.CheckStr(a_now.raw_input, "HE", "session A kept its own raw composition");
  h.checks.CheckMenu(a_now, kMenuHeUpper, "session A kept its uppercase menu");
  h.checks.CheckStr(JoinComments(a_now), "||||||",
                    "session A's comment column stayed empty (7 candidates, "
                    "6 empty separators)");

  rc = press(a, kXkReturn, 0, &ret);
  if (rc != kOk) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.CheckStr(ret.state.commit_text, "HE", "session A commits uppercase");
  rc = commit_is_gone(a, "session A");
  if (rc != kOk) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return rc;
  }
  rc = press(b, kXkReturn, 0, &ret);
  if (rc != kOk) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.CheckStr(ret.state.commit_text, "he",
                    "session B commits lowercase, unaffected by A");
  rc = commit_is_gone(b, "session B");
  if (rc != kOk) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return rc;
  }

  // ---- 9. what the whole matrix did to the counters -------------------------
  if (!ReadCounters(h.surface, &c)) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  std::printf("[english] counters.after_matrix      %s\n",
              DescribeCounters(c).c_str());
  // Switching shape 30+ times must not cost a second word-list read: the index is
  // per-process and case is a rendering step.
  h.checks.CheckNum(LL(c.lexicon_load_attempts), 1,
                    "the case matrix never re-read or re-built the word list");
  h.checks.CheckNum(LL(c.loaded_entry_count), kExpectedLexiconRows,
                    "and still holds the same 54 rows");
  h.checks.Check(c.queries >= static_cast<unsigned long long>(letter_keys),
                 "every injected letter produced a query", std::to_string(c.queries));

  // queries_rejected = mixed + too_long + not_alphabetic + illegal_word_form. This mode
  // types only letters, all within 64 bytes, so exactly three frames may be refused and
  // they must all be named as mixed: "HeL", "hE", "hEL".
  h.checks.CheckNum(LL(c.queries_mixed_case), 3,
                    "the three mixed-case frames are the only refusals in this mode");
  h.checks.CheckNum(LL(c.queries_not_alphabetic), 0,
                    "no byte outside A-Z / a-z was typed in this mode");
  h.checks.CheckNum(LL(c.queries_too_long), 0, "no input was over 64 bytes");
  h.checks.CheckNum(LL(c.queries_illegal_word_form), 0,
                    "no delimiter was typed in this mode");
  h.checks.CheckNum(LL(c.queries_rejected),
                    LL(c.queries_mixed_case) + LL(c.queries_too_long) +
                        LL(c.queries_not_alphabetic) +
                        LL(c.queries_illegal_word_form),
                    "rejected == mixed + too_long + not_alphabetic + illegal_word_form");
  // Every completion is counted in exactly one shape bucket; an uncounted shape would
  // show up here rather than hiding behind plausible menus.
  h.checks.CheckNum(LL(c.completion_candidates),
                    LL(c.completions_lowercase) +
                        LL(c.completions_initial_capital) +
                        LL(c.completions_uppercase),
                    "completions == lowercase + initial-capital + uppercase");
  h.checks.Check(c.completions_lowercase >= 1, "lowercase completions happened",
                 std::to_string(c.completions_lowercase));
  h.checks.Check(c.completions_initial_capital >= 1,
                 "initial-capital completions happened",
                 std::to_string(c.completions_initial_capital));
  h.checks.Check(c.completions_uppercase >= 1, "uppercase completions happened",
                 std::to_string(c.completions_uppercase));
  h.checks.CheckNum(LL(c.completions_with_delimiter), 0,
                    "this mode never typed a word delimiter");
  h.checks.CheckNum(LL(c.queries_covering_part_of_input), 0,
                    "no token reached the translator as a fragment");
  h.checks.CheckNum(LL(c.queries_covering_whole_input), LL(c.queries),
                    "queries_covering_whole_input == queries");
  h.checks.CheckNum(LL(c.original_candidates), LL(c.queries),
                    "every query offered the typed text");
  // The fixture schema sets no english/enable_suggestions key. Nothing in this run may
  // touch the P2.6 feature, in either direction.
  h.checks.CheckNum(SpellingTotal(c), 0,
                    "the deployed schema asked for no spelling suggestions at all");
  h.checks.CheckNum(LL(c.module_finalized), 0, "module not finalized during the run");

  ExitCode done = DestroySessionAndCheck(h.api, b, &h.checks, "CASE session B");
  const ExitCode done_a = DestroySessionAndCheck(h.api, a, &h.checks, "CASE session A");
  if (done_a != kOk && done == kOk) {
    done = done_a;
  }
  if (!ReadCounters(h.surface, &c)) {
    return kCounterReadFailed;
  }
  std::printf("[english] counters.final             %s\n", DescribeCounters(c).c_str());
  h.checks.CheckNum(LL(c.translator_live), 0, "no instance outlived its session");
  h.checks.CheckNum(LL(c.translator_constructed), LL(c.translator_destroyed),
                    "constructed == destroyed");
  return done;
}

ExitCode RunCaseSelfCheckFailMode(Harness& h) {
  if (!h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "case-self.surface",
         "this control needs the english engine to have a real menu to get wrong");
    return kEnglishSurfaceUnavailable;
  }
  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }
  const RimeSessionId a = CreateSessionOrZero(h.api, "CASE-SELF");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "CASE-SELF",
                                       kCompletionSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  std::vector<KeyEvent> events;
  rc = TypeText(h.api, a, "Hel", &h.balance, &events);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  // Same four words, wrong order: Help before Hello. The set is right, the ranking is
  // not, which is exactly the mistake an order-blind menu check would let through.
  h.checks.CheckMenu(events.back().state, "Hel|Help|Hello|Helmet",
                     "deliberately mis-ranked initial-capital menu");
  const ExitCode done = DestroySessionAndCheck(h.api, a, &h.checks, "CASE-SELF");
  if (done != kOk) {
    return done;
  }
  return kOk;  // the harness turns the recorded failure into exit 14
}

}  // namespace recovery
}  // namespace epocanva
