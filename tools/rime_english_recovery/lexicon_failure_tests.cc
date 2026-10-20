// EpoCanva P2.1-recovery -- word-list load failure paths.
// See lexicon_failure_tests.h for what is injected, how, and where the status strings and
// line numbers come from.
#include "lexicon_failure_tests.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "english_diagnostics.h"

namespace epocanva {
namespace recovery {
namespace {

// The probe input. "hel" is a completable lowercase prefix of the good fixture, so on a
// healthy list it would show four candidates; on a failed load it shows one. That contrast
// is what the control mode exploits.
constexpr char kProbe[] = "hel";
constexpr char kGoodProbeMenu[] = "hel|hello|help|helmet";

long long LL(unsigned long long v) { return static_cast<long long>(v); }

std::string JoinLines(const std::vector<long long>& lines) {
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i != 0) {
      out += " or ";
    }
    out += std::to_string(lines[i]);
  }
  return out;
}

// Size of a fixture file, or -1 when it is not there. Used to prove that the bytes staged
// into the run directory are the bytes of the fixture named, and that the fixture directory
// itself was never written to.
long long FixtureSize(const Harness& h, const char* name) {
  std::error_code ec;
  const std::filesystem::path p = h.fixture_dir / name;
  if (!std::filesystem::is_regular_file(p, ec) || ec) {
    return -1;
  }
  const std::uintmax_t size = std::filesystem::file_size(p, ec);
  if (ec) {
    return -1;
  }
  return static_cast<long long>(size);
}

ExitCode RunLoadFailure(Harness& h, const LoadFailureExpectation& want) {
  // 0. The component has to be in the engine for a word-list failure to mean anything.
  // Without this the baseline engine -- where nothing produces candidates at all -- would
  // satisfy "one candidate equal to the input" and the run would report a missing module as
  // a load error.
  if (!h.surface.module_found || !h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, want.label,
         "this run needs the english engine: a word-list failure can only be told apart "
         "from an absent module through the module's own diagnostics");
    return kEnglishSurfaceUnavailable;
  }
  h.checks.CheckStr(std::string(h.surface.api->build_id()), kExpectedBuildId,
                    "component build id");

  // 1. Inject the condition: stage the named word list, or remove the staged one.
  const long long staged_bytes = RestageLexicon(&h, want.fixture_lexicon);
  if (staged_bytes == -2) {
    return kRunDirectoryFailed;
  }
  const std::filesystem::path staged = h.shared_dir / kStagedLexiconName;
  std::error_code ec;
  const bool staged_present = std::filesystem::exists(staged, ec) && !ec;
  if (want.fixture_lexicon == nullptr) {
    h.checks.Check(!staged_present, "no word list is in place at the path the schema names",
                   staged_present ? staged.string() : std::string("absent"));
    h.checks.CheckNum(staged_bytes, -1, "the removal was reported as a removal");
    // The good fixture is still where it belongs, untouched: proof that this run deleted
    // only its own staged copy.
    h.checks.CheckNum(FixtureSize(h, "lexicon.tsv"), 429,
                      "tests/fixtures/english_completion/lexicon.tsv is still 429 bytes");
  } else {
    h.checks.Check(staged_present, "the intended word list is staged",
                   staged_present ? staged.string() : std::string("absent"));
    // The path the schema names holds exactly the bytes of the fixture named, so the
    // diagnostics below can only describe THAT file.
    h.checks.CheckNum(staged_bytes, FixtureSize(h, want.fixture_lexicon),
                      std::string("staged bytes == fixture '") +
                          want.fixture_lexicon + "' bytes");
    h.checks.Check(staged_bytes != FixtureSize(h, "lexicon.tsv"),
                   "and that is not the good 54-row list",
                   std::to_string(staged_bytes) + " vs " +
                       std::to_string(FixtureSize(h, "lexicon.tsv")));
  }

  // 2. Nothing may have been read yet.
  EpoCanvaEnglishCounters c;
  if (!ReadCounters(h.surface, &c)) {
    Fail(kCounterReadFailed, want.label, "get_counters() returned False");
    return kCounterReadFailed;
  }
  h.checks.CheckNum(LL(c.lexicon_load_attempts), 0, "no load attempt before the session");
  h.checks.CheckNum(LL(c.lexicon_load_failures), 0, "no failure recorded yet");

  // 3. Deploy and open a session, then prove WHICH schema is in use by reading its id back.
  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }
  const RimeSessionId a = CreateSessionOrZero(h.api, want.label);
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, want.label,
                                        kCompletionSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }

  // 4. The load diagnostics: this is the half a candidate list cannot express.
  if (!ReadCounters(h.surface, &c)) {
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  std::printf("[english] %s counters             %s\n", want.label,
              DescribeCounters(c).c_str());
  std::printf("[english] %s status                 \"%s\" at line %llu\n", want.label,
              LastStatusName(h.surface).c_str(),
              static_cast<unsigned long long>(c.last_load_error_line));
  h.checks.CheckNum(LL(c.lexicon_load_attempts), 1,
                    "the schema made exactly one load attempt");
  h.checks.CheckNum(LL(c.lexicon_load_successes), 0, "and it did not succeed");
  h.checks.CheckNum(LL(c.lexicon_load_failures), 1,
                    "the failure is recorded as a failure");
  h.checks.CheckNum(LL(c.loaded_entry_count), 0,
                    "no rows were loaded -- a failed load is never an empty list");
  const long long observed_line = LL(c.last_load_error_line);
  h.checks.Check(
      std::find(want.acceptable_error_lines.begin(),
                want.acceptable_error_lines.end(), observed_line) !=
              want.acceptable_error_lines.end(),
      std::string(want.label) + ": the error line is " +
          JoinLines(want.acceptable_error_lines),
      std::to_string(observed_line));
  h.checks.CheckStr(LastStatusName(h.surface), want.status_name,
                    std::string(want.label) + ": load status name");
  // The schema DID name a path; only the file behind it is bad or absent. Without this the
  // "no lexicon_path configured" condition would be indistinguishable from the injected one.
  h.checks.CheckNum(LL(c.config_missing_path), 0,
                    "the schema configured a word-list path; the file behind it is what "
                    "failed");

  // 5. The fallback is visible but never mistaken for completion.
  const std::string probe(kProbe);
  // Only the printable probe keys are counted: a Return on a composition that has just been
  // committed, and an Escape on an empty one, reach the engine but start no query.
  long long probe_letters = 0;
  for (std::size_t i = 0; i < probe.size(); ++i) {
    KeyEvent ev;
    const ExitCode prc =
        SendKey(h.api, a, static_cast<int>(static_cast<unsigned char>(probe[i])), 0,
                &h.balance, &ev);
    if (prc != kOk) {
      h.api->destroy_session(a);
      return prc;
    }
    ++probe_letters;
    const std::string raw = probe.substr(0, i + 1);
    CheckFrame(&h.checks, ev, raw, raw, 8,
               std::string(want.label) + " frame '" + raw + "'");
    EpoCanvaEnglishCounters after;
    if (!ReadCounters(h.surface, &after)) {
      h.api->destroy_session(a);
      return kCounterReadFailed;
    }
    h.checks.CheckNum(CounterDelta(c, after,
                                   &EpoCanvaEnglishCounters::queries_with_index),
                      0, std::string(want.label) + ": no frame consulted an index");
    h.checks.CheckNum(CounterDelta(c, after,
                                   &EpoCanvaEnglishCounters::completion_candidates),
                      0, std::string(want.label) + ": no frame produced a completion");
    h.checks.CheckNum(
        CounterDelta(c, after, &EpoCanvaEnglishCounters::queries_rejected), 0,
        std::string(want.label) +
            ": the probe is a legal word form, so a load failure is not a refusal");
    h.checks.CheckNum(
        CounterDelta(c, after, &EpoCanvaEnglishCounters::queries_no_match), 0,
        std::string(want.label) +
            ": 'no match' means an index was searched and found nothing");
    c = after;
  }

  // 6. The original text is still committed, byte-for-byte, and consumed once.
  KeyEvent committed;
  rc = SendKey(h.api, a, kXkReturn, 0, &h.balance, &committed);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.Check(committed.handled, "Return was handled",
                 committed.handled ? "True" : "False");
  h.checks.CheckStr(committed.state.commit_text, kProbe,
                    std::string(want.label) +
                        ": the typed text still commits unchanged");
  bool again = false;
  std::string again_text;
  rc = ReadCommitOnce(h.api, a, &h.balance, &again, &again_text);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.Check(!again, "the commit was consumed once",
                 again ? again_text : std::string("absent"));

  // 7. The failure is cached: typing the same probe again must not re-open the file. This
  // is the "repeated queries do not retry per keystroke" claim, and it is only meaningful
  // because the file behind the path is left exactly as it was -- nothing here rewrites it.
  for (const char ch : std::string(kProbe)) {
    KeyEvent ev;
    const ExitCode prc =
        SendKey(h.api, a, static_cast<int>(static_cast<unsigned char>(ch)), 0,
                &h.balance, &ev);
    if (prc != kOk) {
      h.api->destroy_session(a);
      return prc;
    }
    ++probe_letters;
  }
  if (!ReadCounters(h.surface, &c)) {
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  h.checks.CheckNum(LL(c.lexicon_load_attempts), 1,
                    "a failed load is cached: six probe keys, one load attempt");
  h.checks.CheckNum(LL(c.lexicon_load_failures), 1, "and one failure, not one per key");
  h.checks.Check(c.queries >= static_cast<unsigned long long>(probe_letters),
                 "every probe key still reached the translator",
                 std::to_string(c.queries));

  // 8. Cancel, clean up, and the accounting that must close either way.
  KeyEvent esc;
  rc = SendKey(h.api, a, kXkEscape, 0, &h.balance, &esc);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.Check(!esc.state.status_composing, "Escape ended the composition",
                 "still composing");
  h.checks.Check(!esc.state.commit_present, "Escape committed nothing",
                 esc.state.commit_present ? esc.state.commit_text
                                          : std::string("absent"));

  std::printf("[english] %s final counters       %s\n", want.label,
              DescribeCounters(c).c_str());
  h.checks.CheckNum(LL(c.original_candidates), LL(c.queries),
                    "every query offered the typed text");
  h.checks.CheckNum(LL(c.queries_covering_whole_input), LL(c.queries),
                    "and each was about the whole composition");
  h.checks.CheckNum(LL(c.queries_covering_part_of_input), 0,
                    "no fragment reached the translator");
  h.checks.CheckNum(SpellingTotal(c), 0,
                    "the schema asked for no spelling suggestions, failed list or not");
  h.checks.CheckNum(LL(c.module_finalized), 0, "module not finalized during the run");

  rc = DestroySessionAndCheck(h.api, a, &h.checks, want.label);
  if (!ReadCounters(h.surface, &c)) {
    return kCounterReadFailed;
  }
  h.checks.CheckNum(LL(c.translator_live), 0, "no instance outlived the session");
  h.checks.CheckNum(LL(c.translator_constructed), LL(c.translator_destroyed),
                    "constructed == destroyed");
  return rc;
}

}  // namespace

ExitCode RunMissingLexiconMode(Harness& h) {
  const LoadFailureExpectation want{"missing-lexicon", nullptr, "file-not-found", {0}};
  return RunLoadFailure(h, want);
}

ExitCode RunBadLexiconDuplicateMode(Harness& h) {
  // lexicon_bad_duplicate.tsv holds "hello" on lines 1 and 3. The parser finds duplicates
  // only after sorting the rows (english_lexicon.cc:203-216) and reports the line of one
  // member of the pair; std::sort is not required to keep equal elements in file order, so
  // either 1 or 3 is the correct answer and anything else is a bug. The SET comes from the
  // fixture, which is what makes this a real claim rather than a shrug.
  const LoadFailureExpectation want{"bad-lexicon-duplicate",
                                    "lexicon_bad_duplicate.tsv",
                                    "duplicate-word",
                                    {1, 3}};
  return RunLoadFailure(h, want);
}

ExitCode RunBadLexiconIllegalMode(Harness& h) {
  // lexicon_bad_illegal.tsv line 3 is "Help": an uppercase byte the word alphabet never
  // contains. Sequential check, so the line number is exact.
  const LoadFailureExpectation want{"bad-lexicon-illegal",
                                    "lexicon_bad_illegal.tsv",
                                    "illegal-word",
                                    {3}};
  return RunLoadFailure(h, want);
}

ExitCode RunBadLexiconIllegalFormMode(Harness& h) {
  // lexicon_bad_illegal_form.tsv line 3 is "word-": every byte is in the alphabet, the
  // SHAPE is not a dictionary entry. This must be its own status name, never the
  // byte-class failure above -- the two failures have different fixes.
  const LoadFailureExpectation want{"bad-lexicon-illegal-form",
                                    "lexicon_bad_illegal_form.tsv",
                                    "illegal-word-form",
                                    {3}};
  return RunLoadFailure(h, want);
}

ExitCode RunLexiconFailureSelfCheckFailMode(Harness& h) {
  if (!h.surface.module_found || !h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "lexicon-self.surface",
         "this control needs the english engine, or its failure would say nothing about "
         "the load diagnostics");
    return kEnglishSurfaceUnavailable;
  }
  // Stage the GOOD 54-row list and then claim a duplicate-word failure. The load succeeds,
  // so only the load assertions can fail here.
  const long long staged_bytes = RestageLexicon(&h, "lexicon.tsv");
  if (staged_bytes == -2) {
    return kRunDirectoryFailed;
  }
  h.checks.CheckNum(staged_bytes, FixtureSize(h, "lexicon.tsv"),
                    "the good 54-row fixture is what is staged");
  const ExitCode deployed = DeployFixture(&h);
  if (deployed != kOk) {
    return deployed;
  }
  const RimeSessionId a = CreateSessionOrZero(h.api, "LEXICON-SELF");
  if (a == kNoSession) {
    return kCreateSessionFailed;
  }
  ExitCode rc = SelectAndConfirmSchema(h.api, a, &h.balance, &h.checks, "LEXICON-SELF",
                                        kCompletionSchemaId);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  // The healthy half: with this word list the probe really does complete, so a failure of
  // this run can only be the load claim below, not a broken engine or a bad argument.
  std::vector<KeyEvent> events;
  rc = TypeText(h.api, a, kProbe, &h.balance, &events);
  if (rc != kOk) {
    h.api->destroy_session(a);
    return rc;
  }
  h.checks.CheckMenu(events.back().state, kGoodProbeMenu,
                     "the good list still produces completions");
  EpoCanvaEnglishCounters c;
  if (!ReadCounters(h.surface, &c)) {
    h.api->destroy_session(a);
    return kCounterReadFailed;
  }
  std::printf("[english] lexicon-self counters      %s\n", DescribeCounters(c).c_str());
  // The deliberately wrong claims: this run asserts a failed load that did not happen.
  h.checks.CheckNum(LL(c.lexicon_load_failures), 1,
                    "deliberately claimed a load failure on a valid word list");
  h.checks.CheckNum(LL(c.loaded_entry_count), 0,
                    "deliberately claimed an empty entry count on a 54-row list");
  h.checks.CheckStr(LastStatusName(h.surface), "duplicate-word",
                    "deliberately claimed the duplicate status on the good fixture");
  h.checks.CheckNum(LL(c.config_missing_path), 0,
                    "correct on purpose: only the load claims may fail");
  const ExitCode done = DestroySessionAndCheck(h.api, a, &h.checks, "LEXICON-SELF");
  if (done != kOk) {
    return done;
  }
  return kOk;  // the harness turns the recorded failures into exit 14
}

}  // namespace recovery
}  // namespace epocanva
