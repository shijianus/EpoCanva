// EpoCanva P2.3-recovery -- limited word forms and technical-text protection.
// See token_tests.h for the units this file asserts in and where its expectations came
// from.
#include "token_tests.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "english_diagnostics.h"

namespace epocanva {
namespace recovery {
namespace {

// Hand-derived from tests/fixtures/english_completion/lexicon.tsv. Rows in play:
// do(3) does(5) done(6) don't(8) doesn't(4) can't(7) co-operate(6) re-enter(3) cat(8)
// he(2) hel(5) hello(10) help(10) helmet(4) hen(6) herb(1) word(2).
// "doesn't" begins "doe", so it is in range for "do" and out of range for "don".
// Ordering is weight descending, ties by ascending ASCII of the canonical row; candidate 0
// is the typed text and a row that renders equal to it is dropped.
constexpr char kMenuD[] = "d|don't|done|does|doesn't|do";
constexpr char kMenuDo[] = "do|don't|done|does|doesn't";
constexpr char kMenuDon[] = "don|don't|done";
constexpr char kMenuDonApos[] = "don'|don't";
constexpr char kMenuDonAposIC[] = "Don'|Don't";
constexpr char kMenuDonAposUP[] = "DON'|DON'T";
constexpr char kMenuC[] = "c|cat|can't|co-operate";
constexpr char kMenuCo[] = "co|co-operate";
constexpr char kMenuCoDash[] = "co-|co-operate";
constexpr char kMenuCoUpDash[] = "CO-|CO-OPERATE";
constexpr char kMenuR[] = "r|re-enter";
constexpr char kMenuRe[] = "re|re-enter";
constexpr char kMenuReDash[] = "re-|re-enter";
constexpr char kMenuCanApos[] = "can'|can't";
// "word-" is a queryable trailing-delimiter prefix with no row behind it: one candidate,
// and the reason has to be no-match rather than any refusal bucket.
constexpr char kMenuWordDash[] = "word-";
constexpr char kMenuHLower[] = "h|hello|help|hen|hel|helmet|he|herb";
constexpr char kMenuHeLower[] = "he|hello|help|hen|hel|helmet|herb";
constexpr char kMenuHelLower[] = "hel|hello|help|helmet";
// Refused shapes, each offering the typed text alone.
constexpr char kMenuCoOperateMixed[] = "Co-Operate";
constexpr char kMenuDoNMixed[] = "DoN't";

constexpr long long LL(unsigned long long v) { return static_cast<long long>(v); }

// One input that must come back whole, plus the UTF-8 code-unit index of the byte that
// decided it, plus the bucket this run expects the engine to name for every frame whose
// length exceeds that index.
//
// The bucket claims below are forced by the two identities engine/rime_components/
// english/english_diagnostics.h records:
//   queries_rejected     = mixed + too_long + not_alphabetic + illegal_word_form
//   queries_not_alphabetic = non_ascii + protected + unsupported_byte
// The apostrophe and the hyphen are case-transparent delimiters, not technical marks, so
// they appear in neither sub-bucket of the second identity: a frame that a delimiter
// decided cannot be recorded as not_alphabetic without breaking it. That is what makes
// "'hello" nameable per frame even though its first frame carries no letter byte at all.
struct RefusalCase {
  const char* text;
  std::size_t stop_offset;
  const char* bucket_name;
  unsigned long long EpoCanvaEnglishCounters::*bucket;
};

const RefusalCase kProtectedCases[] = {
    // technical marks and digits: the conservative protection policy
    {"user@example.com", 4, "protected", &EpoCanvaEnglishCounters::queries_protected},
    {"https://example.com/a?x=1&y=2#z", 5, "protected",
     &EpoCanvaEnglishCounters::queries_protected},
    {"C:\\tmp\\file.txt", 1, "protected", &EpoCanvaEnglishCounters::queries_protected},
    {"foo_bar", 3, "protected", &EpoCanvaEnglishCounters::queries_protected},
    {"v1.2", 1, "protected", &EpoCanvaEnglishCounters::queries_protected},
};

const RefusalCase kIllegalFormCases[] = {
    // two apostrophes: the second one, at index 6, is what puts it outside the grammar
    {"rock'n'roll", 6, "illegal-word-form",
     &EpoCanvaEnglishCounters::queries_illegal_word_form},
    // a delimiter in first position: illegal from the very first frame
    {"'hello", 0, "illegal-word-form",
     &EpoCanvaEnglishCounters::queries_illegal_word_form},
    // the same delimiter twice: the second one, at index 3, decides it
    {"co--operate", 3, "illegal-word-form",
     &EpoCanvaEnglishCounters::queries_illegal_word_form},
};

}  // namespace

ExitCode RunTokenProtectionMode(Harness& h) {
  if (!h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "token.surface",
         "this run needs the english engine; the loaded DLL exposes no "
         "epocanva_english module through find_module()/get_api()");
    return kEnglishSurfaceUnavailable;
  }
  h.checks.CheckStr(std::string(h.surface.api->build_id()), kExpectedBuildId,
                    "component build id");

  EpoCanvaEnglishCounters c;
  if (!ReadCounters(h.surface, &c)) {
    Fail(kCounterReadFailed, "token.counters", "get_counters() returned False");
    return kCounterReadFailed;
  }
  h.checks.CheckNum(LL(c.lexicon_load_attempts), 0, "no word list read yet");

  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }

  const RimeSessionId a = CreateSessionOrZero(h.api, "TOKEN-A");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "TOKEN-A",
                                       kCompletionSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }

  // Expected movement in each refusal bucket, accumulated as the run goes: every increment
  // here corresponds to frames this file actually injected, never to a number copied from a
  // previous run.
  long long expect_protected = 0;
  long long expect_illegal = 0;
  long long expect_mixed = 0;
  long long expect_toolong = 0;
  long long injected_queries = 0;

  KeyEvent ev;
  long long frame_index = 0;
  // Every injected event is reported with what the API read back, so the log names the
  // observed menu for each frame and not merely the verdict. Nothing here is reconstructed:
  // the line prints the same values the assertions compare against.
  auto report = [&](int keycode, int mask) {
    std::printf("[english] ev=%03lld key=0x%04x mask=%d        handled=%d %s\n",
                frame_index, keycode, mask, ev.handled ? 1 : 0,
                DescribeState(ev.state).c_str());
    ++frame_index;
  };
  auto press = [&](RimeSessionId sid, int keycode, int mask) -> ExitCode {
    const ExitCode prc = SendKey(h.api, sid, keycode, mask, &h.balance, &ev);
    if (prc == kOk) {
      report(keycode, mask);
      if (keycode >= 0x20 && keycode < 0x7f && mask == 0) {
        ++injected_queries;
      }
    }
    return prc;
  };
  auto press_key = [&](RimeSessionId sid, int keycode) -> ExitCode {
    return press(sid, keycode, 0);
  };
  auto escape = [&](RimeSessionId sid) -> ExitCode {
    const ExitCode erc = press_key(sid, kXkEscape);
    if (erc != kOk) {
      return erc;
    }
    h.checks.Check(!ev.state.status_composing, "Escape left the session idle",
                   "still composing");
    h.checks.Check(!ev.state.commit_present, "Escape committed nothing",
                   ev.state.commit_present ? ev.state.commit_text
                                           : std::string("absent"));
    return kOk;
  };
  auto queue_empty = [&](RimeSessionId sid, const std::string& label) -> ExitCode {
    bool present = false;
    std::string text;
    const ExitCode crc = ReadCommitOnce(h.api, sid, &h.balance, &present, &text);
    h.checks.Check(!present, label + ": the commit queue is empty again",
                   present ? text : std::string("absent"));
    return crc;
  };
  auto commits_exactly = [&](RimeSessionId sid, const std::string& want,
                             const std::string& label) -> ExitCode {
    const ExitCode prc = press_key(sid, kXkReturn);
    if (prc != kOk) {
      return prc;
    }
    h.checks.Check(ev.state.commit_present, label + ": Return produced a commit",
                   "none");
    h.checks.CheckStr(ev.state.commit_text, want, label + ": committed text");
    h.checks.CheckNum(ev.state.preedit_length, 0, label + ": composition emptied");
    return queue_empty(sid, label);
  };

  // Types `text` one byte at a time and asserts the menu after every byte.
  auto type_all_frames = [&](RimeSessionId sid, const std::string& text,
                             const std::vector<std::string>& menus,
                             const std::string& label) -> ExitCode {
    if (menus.size() != text.size()) {
      Fail(kUsage, "token.test-shape", label + ": one menu per byte is required");
      return kUsage;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
      const std::string raw = text.substr(0, i + 1);
      const ExitCode prc =
          press(sid, static_cast<int>(static_cast<unsigned char>(text[i])), 0);
      if (prc != kOk) {
        return prc;
      }
      CheckFrame(&h.checks, ev, raw, menus[i], 8, label + " frame '" + raw + "'");
    }
    return kOk;
  };
  // Same typing, but only the final frame is asserted: used where the intermediate menus
  // are the case group's business and this file's claim is the delimited form.
  auto type_last_frame = [&](RimeSessionId sid, const std::string& text,
                             const std::string& menu,
                             const std::string& label) -> ExitCode {
    for (std::size_t i = 0; i < text.size(); ++i) {
      const std::string raw = text.substr(0, i + 1);
      const ExitCode prc =
          press(sid, static_cast<int>(static_cast<unsigned char>(text[i])), 0);
      if (prc != kOk) {
        return prc;
      }
      if (i + 1 == text.size()) {
        CheckFrame(&h.checks, ev, raw, menu, 8, label + " final frame '" + raw + "'");
      }
    }
    return kOk;
  };

  // Types without asserting anything, leaving the last frame in `ev` for the caller to
  // check with counters taken around it.
  auto type_plain = [&](RimeSessionId sid, const std::string& text) -> ExitCode {
    for (std::size_t i = 0; i < text.size(); ++i) {
      const ExitCode prc =
          press(sid, static_cast<int>(static_cast<unsigned char>(text[i])), 0);
      if (prc != kOk) {
        return prc;
      }
    }
    return kOk;
  };

  // ---- 1. a delimiter may END a queryable prefix ------------------------------
  rc = type_all_frames(a, "don'", {kMenuD, kMenuDo, kMenuDon, kMenuDonApos},
                       "lowercase don'");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  rc = commits_exactly(a, "don'", "don' with no selection");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  rc = type_last_frame(a, "don'", kMenuDonApos, "don' recomposed");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  {
    bool accepted = false;
    rc = SelectCandidate(h.api, a, 1, &accepted);
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
    h.checks.Check(accepted, "don': select_candidate(1) accepted",
                   accepted ? "True" : "False");
    rc = queue_empty(a, "don' after select");
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
    rc = commits_exactly(a, "don't", "don' after select_candidate(1)");
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
  }
  rc = escape(a);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }

  // The same claim in the other two shapes, plus the other delimiters. Each is typed,
  // committed unselected, then typed again and committed through an explicit selection.
  struct Shape {
    const char* text;
    const char* menu;
    const char* selected_word;
  };
  const Shape shapes[] = {
      {"Don'", kMenuDonAposIC, "Don't"},   {"DON'", kMenuDonAposUP, "DON'T"},
      {"co-", kMenuCoDash, "co-operate"},  {"CO-", kMenuCoUpDash, "CO-OPERATE"},
      {"can'", kMenuCanApos, "can't"},     {"re-", kMenuReDash, "re-enter"},
  };
  for (const Shape& s : shapes) {
    const std::string text = s.text;
    rc = type_last_frame(a, text, s.menu, "shape " + text);
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
    rc = commits_exactly(a, text, "shape " + text + " unselected");
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
    rc = type_last_frame(a, text, s.menu, "shape " + text + " recomposed");
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
    h.checks.Check(accepted, "shape " + text + ": index 1 exists",
                   accepted ? "True" : "False");
    rc = queue_empty(a, "shape " + text + " after select");
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
    rc = commits_exactly(a, s.selected_word, "shape " + text + " selected");
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
    rc = escape(a);
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
  }

  // "co-" from an empty composition: the hyphen is reached through three real frames and
  // the completion appears only in the last one.
  rc = type_all_frames(a, "co-", {kMenuC, kMenuCo, kMenuCoDash}, "'co-' frames");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  rc = escape(a);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  rc = type_all_frames(a, "re-", {kMenuR, kMenuRe, kMenuReDash}, "'re-' frames");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  rc = escape(a);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }

  // ---- 2. a queryable prefix with nothing behind it is a no-match -------------
  if (!ReadCounters(h.surface, &c)) {
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  rc = type_last_frame(a, "word-", kMenuWordDash, "'word-'");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  {
    EpoCanvaEnglishCounters after;
    if (!ReadCounters(h.surface, &after)) {
      h.api->destroy_session(a);
      return kCounterReadFailed;
    }
    h.checks.CheckNum(
        CounterDelta(c, after, &EpoCanvaEnglishCounters::queries_no_match), 2,
        "'word-' produced two no-match frames: 'word', whose row word(2) renders equal to "
        "the input and is deduped, and 'word-', where no row begins with the hyphen");
    h.checks.CheckNum(
        CounterDelta(c, after, &EpoCanvaEnglishCounters::queries_rejected), 0,
        "'word-' was not refused: a trailing delimiter is a legal partial form");
    c = after;
  }
  rc = escape(a);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }

  // ---- 3. protected and illegal inputs come back whole, frame by frame --------
  // For every frame whose length is past the deciding byte: the raw composition is the
  // whole text so far, the menu is that text alone, no completion was produced, exactly one
  // refusal was recorded, and the named bucket moved by exactly one.
  auto run_refusal = [&](const RefusalCase& item, long long* tally) -> ExitCode {
    const std::string text = item.text;
    // `cur` is the snapshot taken immediately before the frame being checked, so every
    // delta below is exactly one keystroke wide. A delta measured from the start of the
    // input would mix the completions the pre-stop frames legitimately produced into the
    // frame that is supposed to show none.
    EpoCanvaEnglishCounters cur;
    if (!ReadCounters(h.surface, &cur)) {
      return kCounterReadFailed;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
      const std::string raw = text.substr(0, i + 1);
      const ExitCode prc =
          press(a, static_cast<int>(static_cast<unsigned char>(text[i])), 0);
      if (prc != kOk) {
        return prc;
      }
      EpoCanvaEnglishCounters next;
      if (!ReadCounters(h.surface, &next)) {
        return kCounterReadFailed;
      }
      if (static_cast<long long>(i + 1) > static_cast<long long>(item.stop_offset)) {
        CheckFrame(&h.checks, ev, raw, raw, 8,
                   std::string(item.text) + " frame '" + raw + "'");
        h.checks.CheckNum(
            CounterDelta(cur, next, &EpoCanvaEnglishCounters::completion_candidates), 0,
            std::string(item.text) + ": no completion on this frame");
        h.checks.CheckNum(
            CounterDelta(cur, next, &EpoCanvaEnglishCounters::queries_rejected), 1,
            std::string(item.text) + ": exactly one refusal per frame");
        h.checks.CheckNum(CounterDelta(cur, next, item.bucket), 1,
                          std::string(item.text) + ": this frame moved " +
                              item.bucket_name);
        *tally += 1;
      }
      cur = next;
    }
    c = cur;
    const ExitCode crc =
        commits_exactly(a, text, std::string(item.text) + " commit");
    if (crc != kOk) {
      return crc;
    }
    return escape(a);
  };

  for (const RefusalCase& item : kProtectedCases) {
    rc = run_refusal(item, &expect_protected);
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
  }
  for (const RefusalCase& item : kIllegalFormCases) {
    rc = run_refusal(item, &expect_illegal);
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
  }
  if (!ReadCounters(h.surface, &c)) {
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  // The exact totals are asserted in section 8, against the two bucket identities.

  // ---- 4. refused word shapes are named as mixed, not as a word-list problem --
  {
    EpoCanvaEnglishCounters before;
    if (!ReadCounters(h.surface, &before)) {
      h.api->destroy_session(a);
      return kCounterReadFailed;
    }
    rc = type_last_frame(a, "DoN't", kMenuDoNMixed, "'DoN't'");
    if (rc == kOk) {
      rc = commits_exactly(a, "DoN't", "'DoN't'");
    }
    if (rc == kOk) {
      rc = escape(a);
    }
    if (rc == kOk) {
      rc = type_last_frame(a, "Co-Operate", kMenuCoOperateMixed, "'Co-Operate'");
    }
    if (rc == kOk) {
      rc = commits_exactly(a, "Co-Operate", "'Co-Operate'");
    }
    if (rc == kOk) {
      rc = escape(a);
    }
    EpoCanvaEnglishCounters after;
    if (rc == kOk && ReadCounters(h.surface, &after)) {
      // "DoN't": D, Do are initial-capital and complete; DoN, DoN', DoN't are mixed -> 3.
      // "Co-Operate": C, Co, Co- complete; the seven frames from "Co-O" on are mixed -> 7.
      // The delimiter is case-transparent, so "Co-" is still completable: the apostrophe
      // and the hyphen neither reset the shape nor end it.
      expect_mixed = 10;
      h.checks.CheckNum(
          CounterDelta(before, after, &EpoCanvaEnglishCounters::queries_mixed_case),
          expect_mixed, "the ten mixed-case frames of DoN't and Co-Operate");
      h.checks.CheckNum(
          CounterDelta(before, after, &EpoCanvaEnglishCounters::queries_protected), 0,
          "no delimiter input was counted as technical text");
      h.checks.CheckNum(
          CounterDelta(before, after,
                       &EpoCanvaEnglishCounters::queries_illegal_word_form),
          0, "'DoN't' and 'Co-Operate' are legal FORMS with an unsupported shape");
      c = after;
    } else if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
  }

  // ---- 5. the protection is not sticky ---------------------------------------
  // hel -> hel@ collapses, and BackSpace must bring the completions straight back.
  rc = type_all_frames(a, "hel", {kMenuHLower, kMenuHeLower, kMenuHelLower},
                       "hel before the mark");
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  {
    EpoCanvaEnglishCounters before;
    if (!ReadCounters(h.surface, &before)) {
      h.api->destroy_session(a);
      return kCounterReadFailed;
    }
    rc = press(a, static_cast<int>(static_cast<unsigned char>('@')), 0);
    if (rc == kOk) {
      CheckFrame(&h.checks, ev, "hel@", "hel@", 8, "hel -> hel@");
      expect_protected += 1;
      EpoCanvaEnglishCounters at_mark;
      if (ReadCounters(h.surface, &at_mark)) {
        h.checks.CheckNum(
            CounterDelta(before, at_mark,
                         &EpoCanvaEnglishCounters::completion_candidates),
            0, "the mark stopped the completions on that frame");
        h.checks.CheckNum(
            CounterDelta(before, at_mark,
                         &EpoCanvaEnglishCounters::queries_protected),
            1, "and named the reason as protected text");
        // BackSpace must restore exactly the three completions of "hel".
        const ExitCode brc = press_key(a, kXkBackSpace);
        ++injected_queries;  // a BackSpace frame re-queries; counted for the floor
        if (brc == kOk) {
          CheckFrame(&h.checks, ev, "hel", kMenuHelLower, 8,
                     "hel@ -> BackSpace -> hel");
          EpoCanvaEnglishCounters restored;
          if (ReadCounters(h.surface, &restored)) {
            h.checks.CheckNum(
                CounterDelta(at_mark, restored,
                             &EpoCanvaEnglishCounters::completion_candidates),
                3, "the recovered frame re-issued exactly three completions");
            c = restored;
          } else {
            rc = kCounterReadFailed;
          }
        } else {
          rc = brc;
        }
      } else {
        rc = kCounterReadFailed;
      }
    }
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
  }
  rc = escape(a);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }

  // he2 and he. : a digit and a mark, each removed by one BackSpace. The snapshots are
  // taken around individual frames, because "the recovered frame re-issued six
  // completions" is a claim about one keystroke, not about the whole input.
  struct Recovery {
    char mark;
    const char* label;
  };
  const Recovery recoveries[] = {{'2', "he2"}, {'.', "he."}};
  for (const Recovery& r : recoveries) {
    const std::string text = r.label;
    rc = type_last_frame(a, text.substr(0, 2), kMenuHeLower,
                         text + ": 'he' before the mark");
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
    EpoCanvaEnglishCounters before_mark;
    if (!ReadCounters(h.surface, &before_mark)) {
      h.api->destroy_session(a);
      return kCounterReadFailed;
    }
    rc = press(a, static_cast<int>(static_cast<unsigned char>(r.mark)), 0);
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
    CheckFrame(&h.checks, ev, text, text, 8, text + " protected");
    EpoCanvaEnglishCounters at_mark;
    if (!ReadCounters(h.surface, &at_mark)) {
      h.api->destroy_session(a);
      return kCounterReadFailed;
    }
    h.checks.CheckNum(
        CounterDelta(before_mark, at_mark,
                     &EpoCanvaEnglishCounters::completion_candidates),
        0, text + ": the mark produced no completion");
    h.checks.CheckNum(
        CounterDelta(before_mark, at_mark,
                     &EpoCanvaEnglishCounters::queries_protected),
        1, text + ": the mark was named as protected text");
    expect_protected += 1;

    rc = press_key(a, kXkBackSpace);
    ++injected_queries;  // the deletion re-queries, so it counts toward the floor
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
    CheckFrame(&h.checks, ev, "he", kMenuHeLower, 8,
               text + " -> BackSpace -> he");
    EpoCanvaEnglishCounters after_bs;
    if (!ReadCounters(h.surface, &after_bs)) {
      h.api->destroy_session(a);
      return kCounterReadFailed;
    }
    h.checks.CheckNum(
        CounterDelta(at_mark, after_bs,
                     &EpoCanvaEnglishCounters::completion_candidates),
        6, text + ": recovering 'he' re-issued its six completions");
    h.checks.CheckNum(
        CounterDelta(at_mark, after_bs,
                     &EpoCanvaEnglishCounters::queries_rejected),
        0, text + ": the recovered frame was not refused for any reason");
    c = after_bs;
    rc = escape(a);
    if (rc != kOk) {
      h.api->destroy_session(a);
      return rc;
    }
  }

  // ---- 6. two sessions, one protected and one not ----------------------------
  const RimeSessionId b = CreateSessionOrZero(h.api, "TOKEN-B");
  if (b == kNoSession) {
    h.api->destroy_session(a);
    return kCreateSessionFailed;
  }
  rc = SelectAndConfirmSchema(h.api, b, &h.balance, &h.checks, "TOKEN-B",
                              kCompletionSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return rc;
  }
  rc = type_last_frame(a, "hel@", "hel@", "session A hel@");
  if (rc != kOk) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return rc;
  }
  expect_protected += 1;
  rc = type_last_frame(b, "hel", kMenuHelLower, "session B hel");
  if (rc != kOk) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return rc;
  }
  // A is read back while B holds its own composition: A must still be the collapsed
  // technical text and B must still have its completions.
  {
    StateSnapshot a_now, b_now;
    ExitCode failure = kOk;
    if (!ReadState(h.api, a, &h.balance, &a_now, &failure) ||
        !ReadState(h.api, b, &h.balance, &b_now, &failure)) {
      h.api->destroy_session(b);
      h.api->destroy_session(a);
      return failure;
    }
    h.checks.CheckMenu(a_now, "hel@", "session A scope");
    h.checks.CheckNum(a_now.num_candidates, 1, "session A stayed collapsed");
    h.checks.CheckMenu(b_now, kMenuHelLower, "session B scope");
    h.checks.CheckNum(b_now.num_candidates, 4,
                      "session B kept its completions while A was protected");
  }
  rc = commits_exactly(a, "hel@", "session A hel@");
  if (rc != kOk) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return rc;
  }
  rc = commits_exactly(b, "hel", "session B hel");
  if (rc != kOk) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return rc;
  }
  rc = escape(a);
  if (rc == kOk) {
    rc = escape(b);
  }
  if (rc != kOk) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return rc;
  }

  // ---- 7. the length limit neither truncates nor loses a byte ----------------
  // 64 code units is the limit (kMaxTokenBytes = kMaxCaseInputBytes = 64), so 64 must
  // still be queried and 65 must be refused as too long -- with the composition keeping
  // every byte. "hel" plus filler is used so the recovered 3-byte frame is a menu this
  // file has already derived.
  {
    std::string at_limit = "hel";
    while (at_limit.size() < 64) {
      at_limit += 'a';
    }
    const std::string over_limit = at_limit + 'a';  // 65 code units
    h.checks.CheckNum(static_cast<long long>(at_limit.size()), 64,
                      "the boundary input is exactly 64 code units");
    h.checks.CheckNum(static_cast<long long>(over_limit.size()), 65,
                      "and the refusing input is 65");

    // 64 code units is inside the limit: it is queried, and this input matches no row, so
    // the answer is the typed text alone with no refusal recorded.
    rc = type_plain(a, at_limit);
    if (rc != kOk) {
      h.api->destroy_session(b);
      h.api->destroy_session(a);
      return rc;
    }
    CheckFrame(&h.checks, ev, at_limit, at_limit, 8, "64-byte input is still queried");
    EpoCanvaEnglishCounters at_64;
    if (!ReadCounters(h.surface, &at_64)) {
      h.api->destroy_session(b);
      h.api->destroy_session(a);
      return kCounterReadFailed;
    }

    // One byte more, and the frame is refused for length -- with every byte kept.
    rc = press(a, static_cast<int>(static_cast<unsigned char>('a')), 0);
    if (rc != kOk) {
      h.api->destroy_session(b);
      h.api->destroy_session(a);
      return rc;
    }
    expect_toolong += 1;
    CheckFrame(&h.checks, ev, over_limit, over_limit, 8, "65-byte input");
    h.checks.CheckNum(ev.state.preedit_length, 65,
                      "65: the preedit kept every byte (UTF-8 code units)");
    h.checks.CheckNum(static_cast<long long>(ev.state.raw_input.size()), 65,
                      "65: get_input() kept every byte");
    h.checks.CheckNum(ev.state.num_candidates, 1,
                      "65: only the original text is offered");
    {
      EpoCanvaEnglishCounters after;
      if (!ReadCounters(h.surface, &after)) {
        h.api->destroy_session(b);
        h.api->destroy_session(a);
        return kCounterReadFailed;
      }
      h.checks.CheckNum(
          CounterDelta(at_64, after, &EpoCanvaEnglishCounters::queries_too_long), 1,
          "65: this one frame was refused as too long");
      h.checks.CheckNum(
          CounterDelta(at_64, after,
                       &EpoCanvaEnglishCounters::queries_rejected),
          1, "65: and the refusal was counted once, not once per byte");
      h.checks.CheckNum(
          CounterDelta(at_64, after, &EpoCanvaEnglishCounters::completion_candidates), 0,
          "65: an over-long input produces no completion");
      c = after;
    }
    // The whole over-long token commits unchanged: nothing was cut, escaped or normalised.
    rc = commits_exactly(a, over_limit, "65-byte commit fidelity");
    if (rc != kOk) {
      h.api->destroy_session(b);
      h.api->destroy_session(a);
      return rc;
    }

    // Now the recovery path: back from 65 code units to "hel".
    rc = type_last_frame(a, over_limit, over_limit, "65-byte recomposed");
    if (rc != kOk) {
      h.api->destroy_session(b);
      h.api->destroy_session(a);
      return rc;
    }
    expect_toolong += 1;
    {
      EpoCanvaEnglishCounters before_bs;
      if (!ReadCounters(h.surface, &before_bs)) {
        h.api->destroy_session(b);
        h.api->destroy_session(a);
        return kCounterReadFailed;
      }
      for (std::size_t i = 0; i < 62; ++i) {
        const ExitCode brc = press_key(a, kXkBackSpace);
        ++injected_queries;
        if (brc != kOk) {
          h.api->destroy_session(b);
          h.api->destroy_session(a);
          return brc;
        }
      }
      CheckFrame(&h.checks, ev, "hel", kMenuHelLower, 8,
                 "65 bytes backspaced to 3 restores the completions");
      EpoCanvaEnglishCounters after_bs;
      if (!ReadCounters(h.surface, &after_bs)) {
        h.api->destroy_session(b);
        h.api->destroy_session(a);
        return kCounterReadFailed;
      }
      h.checks.CheckNum(
          CounterDelta(before_bs, after_bs,
                       &EpoCanvaEnglishCounters::queries_too_long),
          0, "the frames from 64 bytes down were no longer refused for length");
      h.checks.CheckNum(
          CounterDelta(before_bs, after_bs,
                       &EpoCanvaEnglishCounters::completion_candidates),
          3, "and the recovered hel frame issued three completions");
      c = after_bs;
    }
    rc = escape(a);
    if (rc != kOk) {
      h.api->destroy_session(b);
      h.api->destroy_session(a);
      return rc;
    }
  }

  // ---- 8. the identities this phase is built on ------------------------------
  if (!ReadCounters(h.surface, &c)) {
    h.api->destroy_session(b);
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  std::printf("[english] counters.after_matrix      %s\n",
              DescribeCounters(c).c_str());
  h.checks.CheckNum(LL(c.lexicon_load_attempts), 1,
                    "one word-list read for the whole token matrix");
  h.checks.CheckNum(LL(c.loaded_entry_count), kExpectedLexiconRows,
                    "and still the same 54 rows");
  h.checks.Check(c.queries >= static_cast<unsigned long long>(injected_queries),
                 "every injected key produced at least one query",
                 std::to_string(c.queries));
  // rejected = mixed + too_long + not_alphabetic + illegal_word_form
  h.checks.CheckNum(
      LL(c.queries_rejected), LL(c.queries_mixed_case) + LL(c.queries_too_long) +
                                 LL(c.queries_not_alphabetic) +
                                 LL(c.queries_illegal_word_form),
      "rejected == mixed + too_long + not_alphabetic + illegal_word_form");
  // not_alphabetic = non_ascii + protected + unsupported_byte
  h.checks.CheckNum(LL(c.queries_not_alphabetic),
                    LL(c.queries_non_ascii) + LL(c.queries_protected) +
                        LL(c.queries_unsupported_byte),
      "not_alphabetic == non_ascii + protected + unsupported_byte");
  h.checks.CheckNum(LL(c.queries_non_ascii), 0,
                    "every byte injected here was ASCII; nothing in this run is evidence "
                    "about non-ASCII text");
  h.checks.CheckNum(LL(c.queries_unsupported_byte), 0,
                    "no printable ASCII outside the letters, digits, delimiters and the "
                    "eleven technical marks was injected");
  h.checks.CheckNum(LL(c.queries_protected), expect_protected,
                    "protected frames == the frames past each deciding byte");
  h.checks.CheckNum(LL(c.queries_too_long), expect_toolong,
                    "too-long frames == the two 65-code-unit frames");
  h.checks.CheckNum(LL(c.queries_protected), expect_protected,
                    "protected frames == the frames past each deciding byte");
  h.checks.CheckNum(LL(c.queries_too_long), expect_toolong,
                    "too-long frames == the two 65-code-unit frames");
  // Every frame from "DoN't", "Co-Operate" and the refusal cases is accounted for in one
  // of the four named buckets, and none of them may be recorded as not_alphabetic: ' and -
  // are not technical marks, so the second identity below would break if they were.
  h.checks.CheckNum(LL(c.queries_illegal_word_form), expect_illegal,
                    "illegal-word-form frames == the three delimiter-pattern inputs");
  h.checks.CheckNum(LL(c.queries_mixed_case), expect_mixed,
                    "mixed-case frames == the ten of DoN't and Co-Operate");
  h.checks.CheckNum(LL(c.queries_covering_part_of_input), 0,
                    "no token was ever cut: a fragment would move this counter while the "
                    "final menu still looked correct");
  h.checks.CheckNum(LL(c.queries_covering_whole_input), LL(c.queries),
                    "queries_covering_whole_input == queries");
  h.checks.CheckNum(LL(c.original_candidates), LL(c.queries),
                    "every query offered the typed text");
  h.checks.CheckNum(LL(c.completion_candidates),
                    LL(c.completions_lowercase) +
                        LL(c.completions_initial_capital) +
                        LL(c.completions_uppercase),
                    "completions == lowercase + initial-capital + uppercase");
  h.checks.Check(c.completions_with_delimiter <= c.completion_candidates,
                 "delimited completions are a subset of completions",
                 std::to_string(c.completions_with_delimiter));
  h.checks.Check(c.completions_with_delimiter >= 6,
                 "the delimited rows really came out of the index",
                 std::to_string(c.completions_with_delimiter));
  h.checks.CheckNum(SpellingTotal(c), 0,
                    "the deployed schema asked for no spelling suggestions");
  h.checks.CheckNum(LL(c.module_finalized), 0, "module not finalized during the run");

  ExitCode done = DestroySessionAndCheck(h.api, b, &h.checks, "TOKEN session B");
  const ExitCode done_a = DestroySessionAndCheck(h.api, a, &h.checks, "TOKEN session A");
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

ExitCode RunTokenSelfCheckFailMode(Harness& h) {
  if (!h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "token-self.surface",
         "this control needs the english engine to have a real menu to get wrong");
    return kEnglishSurfaceUnavailable;
  }
  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }
  const RimeSessionId a = CreateSessionOrZero(h.api, "TOKEN-SELF");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "TOKEN-SELF",
                                       kCompletionSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  std::vector<KeyEvent> events;
  rc = TypeText(h.api, a, "hel", &h.balance, &events);
  if (rc == kOk) {
    rc = TypeText(h.api, a, "@", &h.balance, &events);
  }
  if (rc == kOk) {
    rc = TypeText(h.api, a, "example.com", &h.balance, &events);
  }
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  // The forbidden corruption, asserted as though it were the expectation: the letters of a
  // technical token completed while the user was typing an address. They must not be
  // there, so this run has to end in failure -- which is what proves the suite inspects
  // candidate content and token scope, not just the final committed string.
  h.checks.CheckMenu(events.back().state,
                     "hel@example.com|hello|help|helmet",
                     "deliberately asserted technical-text corruption");
  const ExitCode done = DestroySessionAndCheck(h.api, a, &h.checks, "TOKEN-SELF");
  if (done != kOk) {
    return done;
  }
  return kOk;  // the harness turns the recorded failure into exit 14
}

}  // namespace recovery
}  // namespace epocanva
