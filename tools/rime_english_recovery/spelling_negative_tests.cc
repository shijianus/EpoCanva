// EpoCanva P2.6-recovery -- the spelling group's unavailable-word-list case and its two
// falsification controls. See spelling_negative_tests.h for what each one is for.
#include "spelling_negative_tests.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "english_diagnostics.h"

namespace epocanva {
namespace recovery {
namespace {

long long LL(unsigned long long v) { return static_cast<long long>(v); }

// The comment column is joined by the harness's own JoinComments, whose separator rule is
// index-based: a "have I written anything yet" rule would drop the first pipe whenever
// candidate 0 carries no comment -- which is every candidate 0 in this project.

long long LabelledRows(const StateSnapshot& s) {
  long long n = 0;
  for (const std::string& c : s.comments) {
    if (c == EPOCANVA_ENGLISH_SPELLING_COMMENT) {
      ++n;
    }
  }
  return n;
}

// The byte sizes the fixture documentation records for the two word lists this run stages:
// the surviving 16-row file, and the three rows this program generated. Naming them is what
// makes "the engine looked at exactly these bytes" a checked fact rather than a hope -- the
// failure this case exists for depends on WHICH list the schema resolved.
constexpr long long kGoodSpellingLexiconBytes = 157;
constexpr long long kBrokenSpellingLexiconBytes = 23;

// Reports one file's size, or -1 when it is not there.
long long FileBytes(const std::filesystem::path& p) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(p, ec) || ec) {
    return -1;
  }
  const std::uintmax_t n = std::filesystem::file_size(p, ec);
  if (ec) {
    return -1;
  }
  return static_cast<long long>(n);
}

}  // namespace

ExitCode RunSpellingLexiconUnavailableMode(Harness& h) {
  if (!h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "spellbroken.surface",
         "this run needs the english engine: an absent module must not be able to "
         "impersonate a word-list error");
    return kEnglishSurfaceUnavailable;
  }
  h.checks.CheckStr(std::string(h.surface.api->build_id()), kExpectedBuildId,
                    "component build id");

  // Both files were produced by the staging step; saying so here is what stops a run that
  // silently deployed the GOOD schema from reading as a load-failure test that passed.
  const std::filesystem::path broken = h.shared_dir / kSpellingBrokenSchemaYaml;
  const std::filesystem::path good = h.shared_dir / kSpellingFixtureSchemaYaml;
  h.checks.CheckNum(FileBytes(broken) > 0 ? 1 : 0, 1,
                    "the generated broken schema is in this run's shared directory");
  h.checks.CheckNum(FileBytes(good) > 0 ? 1 : 0, 1,
                    "and so is the original it was derived from");
  h.checks.CheckNum(FileBytes(h.shared_dir / kSpellingStagedLexiconName),
                    kGoodSpellingLexiconBytes,
                    "the readable 16-row list is untouched at its own path");
  h.checks.CheckNum(FileBytes(h.shared_dir / kSpellingBrokenStagedLexiconName),
                    kBrokenSpellingLexiconBytes,
                    "the generated three-row list is exactly what this run wrote");

  h.schema_source = broken;
  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }

  const RimeSessionId a = CreateSessionOrZero(h.api, "SPELLX-A");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "spellbroken A",
                                       kSpellingBrokenSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }

  EpoCanvaEnglishCounters before;
  if (!ReadCounters(h.surface, &before)) {
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  std::printf("[english] counters.after_select      %s\n", DescribeCounters(before).c_str());
  // The triple that makes this distinguishable from every other empty answer: the candidate
  // list, the load counters, and the status NAME.
  h.checks.CheckNum(LL(before.lexicon_load_attempts), 1,
                    "the list this schema names was read once");
  h.checks.CheckNum(LL(before.lexicon_load_successes), 0, "and it did not load");
  h.checks.CheckNum(LL(before.lexicon_load_failures), 1, "with one failure recorded");
  h.checks.CheckNum(LL(before.loaded_entry_count), 0,
                    "a failed load leaves no partial index");
  h.checks.CheckNum(LL(before.last_load_error_line), 3,
                    "the duplicate row is line 3 of the generated list");
  h.checks.CheckNum(LL(before.config_missing_path), 0,
                    "the schema DID name a word list: only the file is bad");
  h.checks.CheckStr(LastStatusName(h.surface), "duplicate-word",
                    "named load status, not silence");

  // "helo" is the input whose suggestions the readable list offers; here it must produce the
  // load failure once per query and nothing else.
  KeyEvent ev;
  const std::string text = "helo";
  long long frames = 0;
  for (std::size_t i = 0; i < text.size() && rc == kOk; ++i) {
    rc = SendKey(h.api, a, static_cast<int>(static_cast<unsigned char>(text[i])), 0,
                 &h.balance, &ev);
    if (rc != kOk) {
      break;
    }
    EpoCanvaEnglishCounters cur;
    if (!ReadCounters(h.surface, &cur)) {
      rc = kCounterReadFailed;
      break;
    }
    ++frames;
    const std::string frame = "spelling 'helo' frame " + std::to_string(i + 1);
    const long long dq = CounterDelta(before, cur, &EpoCanvaEnglishCounters::queries);
    h.checks.CheckNum(dq >= 1 ? 1 : 0, 1, frame + " [the keystroke ran a Query]");
    h.checks.CheckNum(
        CounterDelta(before, cur, &EpoCanvaEnglishCounters::spelling_lexicon_unavailable), dq,
        frame + " [every query reported lexicon-unavailable]");
    // The disguise this phase forbids, asserted absent rather than inferred: a broken word
    // list must never arrive as "nothing you typed is misspelled".
    h.checks.CheckNum(
        CounterDelta(before, cur, &EpoCanvaEnglishCounters::spelling_no_candidate), 0,
        frame + " [never reported as no-candidate]");
    h.checks.CheckNum(
        CounterDelta(before, cur, &EpoCanvaEnglishCounters::spelling_suggested), 0,
        frame + " [and never as suggested]");
    h.checks.CheckNum(
        CounterDelta(before, cur, &EpoCanvaEnglishCounters::spelling_candidates_appended), 0,
        frame + " [no candidate out of an index that is not there]");
    h.checks.CheckNum(
        CounterDelta(before, cur, &EpoCanvaEnglishCounters::spelling_variants_generated), 0,
        frame + " [no variant generated: there is nothing to look a variant up in]");
    h.checks.CheckNum(
        CounterDelta(before, cur, &EpoCanvaEnglishCounters::spelling_lexicon_lookups), 0,
        frame + " [and no index lookup]");
    h.checks.CheckNum(
        CounterDelta(before, cur, &EpoCanvaEnglishCounters::lexicon_load_attempts), 0,
        frame + " [the failed load is cached, not retried per keystroke]");
    before = cur;
  }
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  // The user is never stuck: the text they typed is still there, alone, and unlabelled.
  h.checks.CheckMenu(ev.state, "helo", "spelling broken list: only the original text");
  h.checks.CheckStr(JoinComments(ev.state), "", "spelling broken list: nothing was labelled");
  h.checks.CheckNum(LabelledRows(ev.state), 0, "spelling broken list: no spell label");
  h.checks.CheckStr(ev.state.raw_input, "helo", "spelling broken list: get_input() intact");
  std::printf("[english] spell %-34s menu=%s cmts=%s (lexicon-unavailable x%lld frames)\n",
              "helo/broken", JoinMenu(ev.state).c_str(), JoinComments(ev.state).c_str(), frames);

  // And the default key still commits exactly what was typed, byte for byte.
  rc = SendKey(h.api, a, kXkReturn, 0, &h.balance, &ev);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.Check(ev.state.commit_present, "Return produced a commit", "none");
  h.checks.CheckStr(ev.state.commit_text, "helo",
                    "Return still commits the typed text, unimproved");
  bool again = false;
  std::string again_text;
  rc = ReadCommitOnce(h.api, a, &h.balance, &again, &again_text);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.Check(!again, "that commit is consumed once",
                 again ? again_text : std::string("absent"));

  if (!ReadCounters(h.surface, &before)) {
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  std::printf("[english] counters.before_destroy    %s\n", DescribeCounters(before).c_str());
  h.checks.CheckNum(LL(before.spelling_lexicon_unavailable),
                    LL(before.queries),
                    "every query this run made reported the load failure, and only that");
  h.checks.CheckNum(LL(before.spelling_no_candidate), 0,
                    "and not one of them was allowed to read as 'nothing is misspelled'");
  h.checks.CheckNum(LL(before.lexicon_load_attempts), 1,
                    "one read for the whole session, failure included");
  h.checks.CheckNum(LL(before.queries_covering_part_of_input), 0,
                    "the token still reached the translator whole");

  const ExitCode done = DestroySessionAndCheck(h.api, a, &h.checks, "spellbroken session A");
  if (!ReadCounters(h.surface, &before)) {
    return kCounterReadFailed;
  }
  h.checks.CheckNum(LL(before.translator_live), 0, "no translator outlives its session");
  return done;
}

ExitCode RunSpellingSuggestionRankingSelfCheckFailMode(Harness& h) {
  if (!h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "spellrank-self.surface",
         "this control needs the english engine to have a real ranking to get wrong");
    return kEnglishSurfaceUnavailable;
  }
  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }
  const RimeSessionId a = CreateSessionOrZero(h.api, "SPELLRANK-SELF");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "SPELLRANK-SELF",
                                        kSpellingSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  std::vector<KeyEvent> events;
  rc = TypeText(h.api, a, "helo", &h.balance, &events);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  // The claim is wrong on purpose, about a fact this file derives independently: "hello"
  // carries weight 9 and "help" carries 8, so a list ordered by weight descending cannot
  // start with help. Everything else in the run stayed correct, which is what makes the
  // resulting failure specific to the ordering rather than to the setup.
  h.checks.CheckMenu(events.back().state, "helo|help|hello|hell",
                     "deliberately mis-ranked suggestion menu");
  const ExitCode done = DestroySessionAndCheck(h.api, a, &h.checks, "SPELLRANK-SELF");
  if (done != kOk) {
    return done;
  }
  return kOk;  // the harness turns the recorded failure into exit 14
}

ExitCode RunSpellingLabelSelfCheckFailMode(Harness& h) {
  if (!h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "spelllabel-self.surface",
         "this control needs the english engine to have a real label column to get wrong");
    return kEnglishSurfaceUnavailable;
  }
  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }
  const RimeSessionId a = CreateSessionOrZero(h.api, "SPELLLABEL-SELF");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "SPELLLABEL-SELF",
                                        kSpellingSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  std::vector<KeyEvent> events;
  rc = TypeText(h.api, a, "heli", &h.balance, &events);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  // "heli" is completed by "helium", and a completion is exactly the thing that must NOT
  // carry the suggestion label. Asserting the label here claims the one distinction a front
  // end has is gone; the run must say so by failing.
  h.checks.CheckStr(JoinComments(events.back().state), "|spell",
                    "deliberately claimed that a completion carries the spell label");
  const ExitCode done = DestroySessionAndCheck(h.api, a, &h.checks, "SPELLLABEL-SELF");
  if (done != kOk) {
    return done;
  }
  return kOk;  // the harness turns the recorded failure into exit 14
}

}  // namespace recovery
}  // namespace epocanva
