// EpoCanva P2.6-recovery -- the spelling feature switch, measured as a comparison.
// See spelling_toggle_tests.h for what this proves and what it deliberately does not.
#include "spelling_toggle_tests.h"

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

long long LabelledRows(const StateSnapshot& s) {
  long long n = 0;
  for (const std::string& c : s.comments) {
    if (c == EPOCANVA_ENGLISH_SPELLING_COMMENT) {
      ++n;
    }
  }
  return n;
}

// --- the 19 spelling counters, each with what a movement would have meant -----------------
// The disabled side is judged field by field rather than as one sum, because "the sum is zero"
// alone cannot say WHY it is zero. Every one of these is written only inside
// CountSpellingOutcome(), which the translator reaches from two places and both of them are
// under `if (spelling_enabled_)` (engine/rime_components/english/english_translator.cc:368 and
// :482). A non-zero value on the disabled side therefore means the suggestion path was entered
// by a schema that did not ask for it -- and each name below says which entry it was.
struct SpellField {
  const char* name;
  unsigned long long EpoCanvaEnglishCounters::*field;
  const char* meaning_if_not_zero;
};

constexpr SpellField kSpellFields[] = {
    {"spelling_suggested", &EpoCanvaEnglishCounters::spelling_suggested,
     "a query found at least one row one edit away"},
    {"spelling_no_candidate", &EpoCanvaEnglishCounters::spelling_no_candidate,
     "a query searched the index and found nothing one edit away"},
    {"spelling_candidates_appended", &EpoCanvaEnglishCounters::spelling_candidates_appended,
     "a suggestion candidate was appended to a menu"},
    {"spelling_variants_generated", &EpoCanvaEnglishCounters::spelling_variants_generated,
     "the suggester generated edit variants at all"},
    {"spelling_lexicon_lookups", &EpoCanvaEnglishCounters::spelling_lexicon_lookups,
     "the suggester looked a variant up in the word list"},
    {"spelling_skipped_word_in_lexicon",
     &EpoCanvaEnglishCounters::spelling_skipped_word_in_lexicon,
     "the suggester reached its word-is-already-a-row gate"},
    {"spelling_skipped_completion", &EpoCanvaEnglishCounters::spelling_skipped_completion,
     "the suggester reached its completion-suppression gate"},
    {"spelling_skipped_protected", &EpoCanvaEnglishCounters::spelling_skipped_protected,
     "the suggester reached its protected-text gate"},
    {"spelling_skipped_delimited", &EpoCanvaEnglishCounters::spelling_skipped_delimited,
     "the suggester reached its delimited-word-form gate"},
    {"spelling_skipped_mixed_case", &EpoCanvaEnglishCounters::spelling_skipped_mixed_case,
     "the suggester reached its mixed-case gate"},
    {"spelling_skipped_too_short", &EpoCanvaEnglishCounters::spelling_skipped_too_short,
     "the suggester reached its length-window floor"},
    {"spelling_skipped_too_long", &EpoCanvaEnglishCounters::spelling_skipped_too_long,
     "the suggester reached its length-window ceiling"},
    {"spelling_skipped_unsupported_byte",
     &EpoCanvaEnglishCounters::spelling_skipped_unsupported_byte,
     "the suggester classified an unsupported byte"},
    {"spelling_skipped_non_ascii", &EpoCanvaEnglishCounters::spelling_skipped_non_ascii,
     "the suggester classified a non-ASCII byte"},
    {"spelling_skipped_empty", &EpoCanvaEnglishCounters::spelling_skipped_empty,
     "the suggester was asked about an empty input"},
    {"spelling_lexicon_unavailable",
     &EpoCanvaEnglishCounters::spelling_lexicon_unavailable,
     "the suggester reported an unreadable word list (english_translator.cc:368 is inside "
     "the same guard, so even this state must stay zero)"},
    {"spelling_suggestions_lowercase",
     &EpoCanvaEnglishCounters::spelling_suggestions_lowercase,
     "a suggestion was rendered lowercase"},
    {"spelling_suggestions_initial_capital",
     &EpoCanvaEnglishCounters::spelling_suggestions_initial_capital,
     "a suggestion was rendered initial-capital"},
    {"spelling_suggestions_uppercase",
     &EpoCanvaEnglishCounters::spelling_suggestions_uppercase,
     "a suggestion was rendered uppercase"},
};
constexpr std::size_t kSpellFieldCount = sizeof(kSpellFields) / sizeof(kSpellFields[0]);

// --- everything that is NOT spelling, and must move identically on both sides -------------
// Same input, same 16 rows, same budgets: if the switch only switches the switch, these
// event counts must agree between the enabled phase and the disabled phase to the unit. This
// is what stops "no suggestions appeared" from being earned by a side that did nothing at all.
struct PlainField {
  const char* name;
  unsigned long long EpoCanvaEnglishCounters::*field;
};

constexpr PlainField kPlainFields[] = {
    {"queries", &EpoCanvaEnglishCounters::queries},
    {"queries_with_index", &EpoCanvaEnglishCounters::queries_with_index},
    {"queries_rejected", &EpoCanvaEnglishCounters::queries_rejected},
    {"original_candidates", &EpoCanvaEnglishCounters::original_candidates},
    {"completion_candidates", &EpoCanvaEnglishCounters::completion_candidates},
    {"queries_no_match", &EpoCanvaEnglishCounters::queries_no_match},
    {"queries_mixed_case", &EpoCanvaEnglishCounters::queries_mixed_case},
    {"queries_not_alphabetic", &EpoCanvaEnglishCounters::queries_not_alphabetic},
    {"queries_too_long", &EpoCanvaEnglishCounters::queries_too_long},
    {"queries_illegal_word_form", &EpoCanvaEnglishCounters::queries_illegal_word_form},
    {"queries_protected", &EpoCanvaEnglishCounters::queries_protected},
    {"queries_non_ascii", &EpoCanvaEnglishCounters::queries_non_ascii},
    {"queries_unsupported_byte", &EpoCanvaEnglishCounters::queries_unsupported_byte},
    {"completions_lowercase", &EpoCanvaEnglishCounters::completions_lowercase},
    {"completions_initial_capital", &EpoCanvaEnglishCounters::completions_initial_capital},
    {"completions_uppercase", &EpoCanvaEnglishCounters::completions_uppercase},
    {"completions_with_delimiter", &EpoCanvaEnglishCounters::completions_with_delimiter},
    {"queries_covering_whole_input", &EpoCanvaEnglishCounters::queries_covering_whole_input},
    {"queries_covering_part_of_input", &EpoCanvaEnglishCounters::queries_covering_part_of_input},
    {"processor_key_events", &EpoCanvaEnglishCounters::processor_key_events},
    {"space_events_seen", &EpoCanvaEnglishCounters::space_events_seen},
    {"space_events_refused", &EpoCanvaEnglishCounters::space_events_refused},
    {"space_case_a_raw_token", &EpoCanvaEnglishCounters::space_case_a_raw_token},
    {"space_case_b_selection", &EpoCanvaEnglishCounters::space_case_b_selection},
    {"space_case_c_empty", &EpoCanvaEnglishCounters::space_case_c_empty},
    {"composition_commits_issued", &EpoCanvaEnglishCounters::composition_commits_issued},
    {"space_characters_emitted", &EpoCanvaEnglishCounters::space_characters_emitted},
    {"translator_constructed", &EpoCanvaEnglishCounters::translator_constructed},
    {"translator_destroyed", &EpoCanvaEnglishCounters::translator_destroyed},
    {"space_processor_constructed", &EpoCanvaEnglishCounters::space_processor_constructed},
    {"space_processor_destroyed", &EpoCanvaEnglishCounters::space_processor_destroyed},
};
constexpr std::size_t kPlainFieldCount = sizeof(kPlainFields) / sizeof(kPlainFields[0]);

// --- the probes --------------------------------------------------------------------------
// `on_menu` and `on_comments` are transcribed from tests/fixtures/english_spelling/README.md
// section 4, the same documented table spelling_tests.cc checks its reference against. The
// disabled expectation needs no third source: it is section 7's sentence, and it is either
// "the original text alone" (where the enabled side showed suggestions) or "the enabled side's
// menu, unchanged" (where a completion or a suppression was in charge).
struct Probe {
  const char* typed;
  const char* on_menu;
  const char* on_comments;
  int commit_key;
  const char* committed;
  bool suggestion_probe;
};

constexpr Probe kProbes[] = {
    {"helo", "helo|hello|help|hell", "|spell|spell|spell", kXkReturn, "helo", true},
    {"heli", "heli|helium", "|", kXkReturn, "heli", false},
    {"wrld", "wrld|world", "|spell", kXkReturn, "wrld", true},
    {"Helo", "Helo|Hello|Help|Hell", "|spell|spell|spell", kXkReturn, "Helo", true},
    {"hello", "hello", "", kXkReturn, "hello", false},
    // The same misspelling again, closed with the space bar rather than Return, so the
    // "space commits the raw token plus exactly one U+0020" rule is read on both sides.
    {"helo", "helo|hello|help|hell", "|spell|spell|spell", ' ', "helo ", true},
};
constexpr std::size_t kProbeCount = sizeof(kProbes) / sizeof(kProbes[0]);

// The 25 keystrokes of that table (4+4+4+4+5+4), counted from kProbes rather than typed in
// twice, so the two sides cannot be told to have done different amounts of work by accident.
constexpr int kTypedKeysPerSide = 4 + 4 + 4 + 4 + 5 + 4;

// What the enabled side must have recorded over the sequence, derived from the table above:
// three suggestion rows for `helo`, one for `wrld`, three for `Helo`, three again for the
// space-run of `helo`; nothing for `heli` (a completion is available) or `hello` (the row
// already exists). 3+1+3+3 = 10 rows over 4 queries that had at least one.
constexpr long long kExpectedOnSuggested = 4;
constexpr long long kExpectedOnAppended = 10;
constexpr long long kExpectedOnLowercase = 7;   // 3 + 1 + 3
constexpr long long kExpectedOnInitialCapital = 3;  // the `Helo` run only
constexpr long long kExpectedOnUppercase = 0;

const char* OffMenuFor(const Probe& p, std::string* scratch) {
  if (!p.suggestion_probe) {
    return p.on_menu;
  }
  *scratch = p.typed;
  return scratch->c_str();
}

const char* OffCommentsFor(const Probe& p, std::string* scratch) {
  if (!p.suggestion_probe) {
    return p.on_comments;
  }
  // One candidate, therefore one empty comment field and no separator at all.
  scratch->clear();
  return scratch->c_str();
}

struct ProbeOutcome {
  std::string menu;
  std::string comments;
  long long labelled = -1;
  long long candidates = -1;
};

// Types `p.typed` one byte at a time, asserting every frame, then presses the probe's commit
// key and asserts what came out of the queue. `judge_spelling` is the disabled side's
// per-frame rule; the enabled side's spelling accounting is done once, over the whole phase,
// because the per-frame gate table belongs to spelling_tests.cc and this file must not give
// two documents room to disagree about it.
ExitCode TypeProbe(Harness& h, RimeSessionId sid, const Probe& p, const char* side,
                   bool judge_spelling, ProbeOutcome* out) {
  const std::string text(p.typed);
  EpoCanvaEnglishCounters frame_before;
  EpoCanvaEnglishCounters frame_now;
  if (!ReadCounters(h.surface, &frame_before)) {
    return kCounterReadFailed;
  }
  KeyEvent ev;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const ExitCode rc = SendKey(h.api, sid, static_cast<int>(static_cast<unsigned char>(text[i])),
                                0, &h.balance, &ev);
    if (rc != kOk) {
      return rc;
    }
    const std::string prefix = text.substr(0, i + 1);
    const std::string label =
        std::string(side) + " '" + text + "' frame " + std::to_string(i + 1);
    // The frame's own structure, on both sides and with no linguistic claim in it.
    h.checks.Check(ev.handled, label + ": process_key() handled the key",
                  ev.handled ? "True" : "False");
    h.checks.CheckStr(ev.state.raw_input, prefix, label + ": get_input() raw composition");
    h.checks.Check(ev.state.has_preedit, label + ": preedit present",
                  ev.state.has_preedit ? ev.state.preedit : std::string("absent"));
    h.checks.CheckStr(ev.state.preedit, prefix, label + ": preedit text");
    h.checks.CheckNum(ev.state.preedit_length, static_cast<long long>(prefix.size()),
                      label + ": preedit length (UTF-8 code units)");
    h.checks.CheckNum(ev.state.page_size, 8, label + ": menu page_size");
    if (!ev.state.candidates.empty()) {
      h.checks.CheckStr(ev.state.candidates[0], prefix,
                        label + ": candidate 0 is the typed text");
    } else {
      h.checks.Check(false, label + ": candidate 0 is the typed text", "menu empty");
    }
    h.checks.CheckNum(static_cast<long long>(ev.state.comments.size()),
                      static_cast<long long>(ev.state.candidates.size()),
                      label + ": one comment field per candidate");
    if (!ReadCounters(h.surface, &frame_now)) {
      return kCounterReadFailed;
    }
    h.checks.CheckNum(CounterDelta(frame_before, frame_now,
                                   &EpoCanvaEnglishCounters::queries),
                      1, label + ": exactly one Query() for one keystroke");
    h.checks.CheckNum(CounterDelta(frame_before, frame_now,
                                   &EpoCanvaEnglishCounters::queries_with_index),
                      1, label + ": and that Query consulted a loaded index");
    h.checks.CheckNum(CounterDelta(frame_before, frame_now,
                                   &EpoCanvaEnglishCounters::queries_covering_part_of_input),
                      0, label + ": the token still reached the translator whole");
    if (judge_spelling) {
      for (std::size_t f = 0; f < kSpellFieldCount; ++f) {
        h.checks.CheckNum(
            CounterDelta(frame_before, frame_now, kSpellFields[f].field), 0,
            label + ": " + kSpellFields[f].name + " stayed 0 -- a value here would mean " +
                kSpellFields[f].meaning_if_not_zero + " on a schema that never asked");
      }
      h.checks.CheckNum(LabelledRows(ev.state), 0,
                        label + ": nothing in this menu carries the spell label");
    }
    frame_before = frame_now;
  }

  out->menu = JoinMenu(ev.state);
  out->comments = JoinComments(ev.state);
  out->labelled = LabelledRows(ev.state);
  out->candidates = static_cast<long long>(ev.state.candidates.size());

  // The commit boundary, asserted identically on both sides: what arrives without an explicit
  // selection is the user's own bytes. Requirement 5 of the brief, and the reason a suggestion
  // feature can be described as advisory rather than automatic.
  const ExitCode rc = SendKey(h.api, sid, p.commit_key, 0, &h.balance, &ev);
  if (rc != kOk) {
    return rc;
  }
  const std::string commit_label = std::string(side) + " '" + text + "' commit";
  h.checks.Check(ev.state.commit_present, commit_label + ": a commit was produced",
                 ev.state.commit_present ? "present" : "absent");
  h.checks.CheckStr(ev.state.commit_text, p.committed,
                    commit_label + ": the committed text is what was typed, unimproved");
  bool again = false;
  std::string again_text;
  const ExitCode rc2 = ReadCommitOnce(h.api, sid, &h.balance, &again, &again_text);
  if (rc2 != kOk) {
    return rc2;
  }
  h.checks.Check(!again, commit_label + ": that commit is consumed once",
                 again ? again_text : std::string("absent"));
  return kOk;
}

// Deploys the two controlled schemas and returns the engine's own view of each. Every step
// here is a precondition of the comparison rather than an expectation about English.
ExitCode SetupBothSides(Harness& h, RimeSessionId* sid_on, RimeSessionId* sid_off,
                        DeployedEnglishConfig* on_cfg, DeployedEnglishConfig* off_cfg) {
  // 1. Both schema files are in this run's shared directory, the word list is the one 157-byte
  //    fixture, and the broken pair the other spelling plan derives is NOT here -- the toggle
  //    plan staged exactly what it compares and nothing else.
  h.checks.CheckNum(FileBytes(h.shared_dir / kSpellingFixtureSchemaYaml) > 0 ? 1 : 0, 1,
                    "the suggestions-enabled schema is in this run's shared directory");
  h.checks.CheckNum(FileBytes(h.shared_dir / kSpellingOffSchemaYaml) > 0 ? 1 : 0, 1,
                    "and so is the derived suggestions-disabled twin");
  h.checks.CheckNum(FileBytes(h.shared_dir / kSpellingStagedLexiconName), 157,
                    "the staged word list is the 157-byte 16-row fixture both schemas name");
  h.checks.CheckNum(FileBytes(h.shared_dir / kSpellingBrokenSchemaYaml), -1,
                    "the broken schema is NOT staged by this plan");
  h.checks.CheckNum(FileBytes(h.shared_dir / kSpellingBrokenStagedLexiconName), -1,
                    "nor is the three-row duplicate word list");

  // 2. Deploy both. DeployFixture() proves each compiled artefact landed in the engine's own
  //    staging directory, deriving the expected file name from what this run staged.
  h.schema_source = h.shared_dir / kSpellingFixtureSchemaYaml;
  ExitCode rc = DeployFixture(&h);
  if (rc != kOk) {
    return rc;
  }
  h.schema_source = h.shared_dir / kSpellingOffSchemaYaml;
  rc = DeployFixture(&h);
  const std::filesystem::path primary = h.shared_dir / kSpellingFixtureSchemaYaml;
  h.schema_source = primary;
  if (rc != kOk) {
    return rc;
  }

  // 3. Read both back through the engine's config component -- the lookup
  //    Engine::ApplySchema() performs, not a re-read of the fixture text.
  rc = ReadDeployedEnglishConfig(h.api, &h.balance, kSpellingSchemaId, on_cfg);
  if (rc != kOk) {
    return rc;
  }
  rc = ReadDeployedEnglishConfig(h.api, &h.balance, kSpellingOffSchemaId, off_cfg);
  if (rc != kOk) {
    return rc;
  }
  h.checks.Check(on_cfg->opened, "schema_open() readable for the enabled id",
                 on_cfg->opened ? "True" : "False");
  h.checks.Check(off_cfg->opened, "schema_open() readable for the disabled id",
                 off_cfg->opened ? "True" : "False");

  // The switch itself. `present` is the return value of the read, so an absent key and a key
  // reading false are kept apart: the fixture's own comment says the component defaults to
  // off when a schema names nothing, and this run must not be able to satisfy its disabled
  // side by having lost the line entirely.
  h.checks.Check(on_cfg->enable_suggestions_present,
                 "enabled schema: english/enable_suggestions is readable",
                 on_cfg->enable_suggestions_present ? "read" : "ABSENT");
  h.checks.Check(on_cfg->enable_suggestions_value,
                 "enabled schema: english/enable_suggestions reads true",
                 on_cfg->enable_suggestions_value ? "true" : "false");
  h.checks.Check(off_cfg->enable_suggestions_present,
                 "disabled schema: english/enable_suggestions is readable, not merely absent",
                 off_cfg->enable_suggestions_present ? "read" : "ABSENT");
  h.checks.Check(!off_cfg->enable_suggestions_value,
                 "disabled schema: english/enable_suggestions reads false",
                 off_cfg->enable_suggestions_value ? "true" : "false");

  // Everything else, the same on both sides, checked rather than assumed to have been copied.
  const char* keys[] = {"english/max_suggestions", "english/max_completions",
                        "menu/page_size"};
  for (std::size_t i = 0; i < 3; ++i) {
    const std::string which(keys[i]);
    const bool pa = i == 0 ? on_cfg->max_suggestions_present
                           : (i == 1 ? on_cfg->max_completions_present
                                     : on_cfg->page_size_present);
    const bool pb = i == 0 ? off_cfg->max_suggestions_present
                           : (i == 1 ? off_cfg->max_completions_present
                                     : off_cfg->page_size_present);
    const long long va = i == 0 ? on_cfg->max_suggestions
                                : (i == 1 ? on_cfg->max_completions : on_cfg->page_size);
    const long long vb = i == 0 ? off_cfg->max_suggestions
                                : (i == 1 ? off_cfg->max_completions : off_cfg->page_size);
    h.checks.Check(pa && pb, "both schemas name " + which,
                   std::string(pa ? "read" : "ABSENT") + " / " + (pb ? "read" : "ABSENT"));
    h.checks.CheckNum(va, vb, which + ": identical on both sides");
  }
  h.checks.CheckNum(on_cfg->max_suggestions, 3, "english/max_suggestions is 3");
  h.checks.CheckNum(on_cfg->max_completions, 7, "english/max_completions is 7");
  h.checks.CheckNum(on_cfg->page_size, 8, "menu/page_size is 8 on both sides");
  h.checks.CheckStr(on_cfg->lexicon_path, kSpellingStagedLexiconName,
                    "the enabled schema resolves this lexicon path");
  h.checks.CheckStr(off_cfg->lexicon_path, kSpellingStagedLexiconName,
                    "the disabled schema resolves the SAME lexicon path");
  h.checks.CheckStr(off_cfg->lexicon_path, on_cfg->lexicon_path,
                    "so both sides read one and the same word list");

  // 4. Two sessions, each on its own schema, each confirmed by reading
  //    get_status().schema_id back rather than by trusting select_schema()'s return value.
  *sid_on = CreateSessionOrZero(h.api, "TOGGLE-ON");
  if (*sid_on == kNoSession) {
    return kCreateSessionFailed;
  }
  rc = SelectAndConfirmSchema(h.api, *sid_on, &h.balance, &h.checks, "toggle ON",
                              kSpellingSchemaId);
  if (rc != kOk) {
    return rc;
  }
  *sid_off = CreateSessionOrZero(h.api, "TOGGLE-OFF");
  if (*sid_off == kNoSession) {
    return kCreateSessionFailed;
  }
  rc = SelectAndConfirmSchema(h.api, *sid_off, &h.balance, &h.checks, "toggle OFF",
                              kSpellingOffSchemaId);
  return rc;
}

void DestroyBoth(Harness& h, RimeSessionId sid_on, RimeSessionId sid_off) {
  // A refused destroy is recorded as an assertion by DestroySessionAndCheck itself; the run
  // turns that into exit 14, which is the outcome that must not be silently swallowed here.
  DestroySessionAndCheck(h.api, sid_on, &h.checks, "toggle ON session");
  DestroySessionAndCheck(h.api, sid_off, &h.checks, "toggle OFF session");
}

}  // namespace

ExitCode RunSpellingToggleAbMode(Harness& h) {
  if (!h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "toggle.surface",
         "this run needs the english engine: without the module there is no suggestion path "
         "to switch on, and an absent module must not be able to impersonate a disabled one");
    return kEnglishSurfaceUnavailable;
  }
  h.checks.CheckStr(std::string(h.surface.api->build_id()), kExpectedBuildId,
                    "component build id");

  RimeSessionId sid_on = kNoSession;
  RimeSessionId sid_off = kNoSession;
  DeployedEnglishConfig on_cfg;
  DeployedEnglishConfig off_cfg;
  ExitCode rc = SetupBothSides(h, &sid_on, &sid_off, &on_cfg, &off_cfg);
  if (rc != kOk) {
    DestroyBoth(h, sid_on, sid_off);
    return rc;
  }

  EpoCanvaEnglishCounters before;
  EpoCanvaEnglishCounters after_on;
  EpoCanvaEnglishCounters after_off;
  if (!ReadCounters(h.surface, &before)) {
    DestroyBoth(h, sid_on, sid_off);
    return kCounterReadFailed;
  }
  std::printf("[english] counters.before_phases     %s\n", DescribeCounters(before).c_str());
  // Both sessions and both translators already exist at this point, and neither has been
  // asked about a word: the load happened once for the path, through the cache
  // (english_translator.cc:276-279 keeps one entry per normalised path), and the second
  // schema resolving the SAME path is a cache hit rather than a second read.
  h.checks.CheckNum(LL(before.lexicon_load_attempts), 1,
                    "the shared word list was read once for both schemas");
  h.checks.CheckNum(LL(before.lexicon_load_successes), 1, "that one read succeeded");
  h.checks.CheckNum(LL(before.lexicon_load_failures), 0, "and no read failed");
  h.checks.CheckNum(LL(before.loaded_entry_count), kExpectedSpellingLexiconRows,
                    "and the engine holds all 16 controlled rows");
  h.checks.CheckNum(LL(before.last_load_error_line), 0, "with no error line to report");
  h.checks.CheckNum(LL(before.config_missing_path), 0,
                    "both schemas named a word list; nothing fell back to a missing path");
  h.checks.CheckStr(LastStatusName(h.surface), "ok", "named load status");
  h.checks.CheckNum(SpellingTotal(before), 0,
                    "no keystroke has been typed yet, so the suggestion path has not run");

  // --- phase 1: every probe on the enabled side -------------------------------------------
  ProbeOutcome on_out[kProbeCount];
  for (std::size_t i = 0; i < kProbeCount; ++i) {
    rc = TypeProbe(h, sid_on, kProbes[i], "ON", /*judge_spelling=*/false, &on_out[i]);
    if (rc != kOk) {
      DestroyBoth(h, sid_on, sid_off);
      return rc;
    }
  }
  if (!ReadCounters(h.surface, &after_on)) {
    DestroyBoth(h, sid_on, sid_off);
    return kCounterReadFailed;
  }

  // --- phase 2: the identical probes on the disabled side ---------------------------------
  ProbeOutcome off_out[kProbeCount];
  for (std::size_t i = 0; i < kProbeCount; ++i) {
    rc = TypeProbe(h, sid_off, kProbes[i], "OFF", /*judge_spelling=*/true, &off_out[i]);
    if (rc != kOk) {
      DestroyBoth(h, sid_on, sid_off);
      return rc;
    }
  }
  if (!ReadCounters(h.surface, &after_off)) {
    DestroyBoth(h, sid_on, sid_off);
    return kCounterReadFailed;
  }

  // --- the comparison, probe by probe ----------------------------------------------------
  std::printf("[english] toggle comparison          %zu probes, %d keystrokes per side\n",
              kProbeCount, kTypedKeysPerSide);
  for (std::size_t i = 0; i < kProbeCount; ++i) {
    const Probe& p = kProbes[i];
    std::string scratch_off_menu;
    std::string scratch_off_comments;
    const char* want_off_menu = OffMenuFor(p, &scratch_off_menu);
    const char* want_off_comments = OffCommentsFor(p, &scratch_off_comments);
    const std::string probe_label =
        std::string("probe '") + p.typed + "' (suggestion probe=" +
        (p.suggestion_probe ? "yes" : "no") + ")";

    // The enabled side's menu is the documented one, labels and all.
    h.checks.CheckStr(on_out[i].menu, p.on_menu, probe_label + ": enabled menu as documented");
    h.checks.CheckStr(on_out[i].comments, p.on_comments,
                      probe_label + ": enabled comment column as documented");
    h.checks.CheckNum(on_out[i].candidates,
                      static_cast<long long>(SplitPipeList(p.on_menu).size()),
                      probe_label + ": enabled candidate count");

    // The disabled side: the original text alone where a suggestion would have appeared, and
    // the enabled menu byte-for-byte where a completion or a suppression was in charge.
    h.checks.CheckStr(off_out[i].menu, std::string(want_off_menu),
                      probe_label + ": disabled menu");
    h.checks.CheckStr(off_out[i].comments, std::string(want_off_comments),
                      probe_label + ": disabled comment column");
    h.checks.CheckNum(off_out[i].candidates,
                      static_cast<long long>(SplitPipeList(want_off_menu).size()),
                      probe_label + ": disabled candidate count");
    h.checks.CheckNum(off_out[i].labelled, 0,
                      probe_label + ": no disabled row carries the spell label");

    if (p.suggestion_probe) {
      h.checks.Check(on_out[i].menu != off_out[i].menu,
                     probe_label + ": the two sides differ, so the switch did something",
                     on_out[i].menu + " vs " + off_out[i].menu);
      h.checks.CheckNum(on_out[i].labelled,
                        static_cast<long long>(SplitPipeList(p.on_menu).size() - 1),
                        probe_label + ": the enabled side labelled every suggestion");
    } else {
      h.checks.CheckStr(on_out[i].menu, off_out[i].menu,
                        probe_label + ": identical menus, because this input was never a "
                                       "suggestion on either side");
      h.checks.CheckStr(on_out[i].comments, off_out[i].comments,
                        probe_label + ": identical comment columns");
    }
  }

  // --- the counter comparison, phase against phase ---------------------------------------
  // Same 25 keystrokes per side, and identical amounts of work everywhere except the
  // suggestion path.
  for (std::size_t i = 0; i < kPlainFieldCount; ++i) {
    const long long da =
        CounterDelta(before, after_on, kPlainFields[i].field);
    const long long db = CounterDelta(after_on, after_off, kPlainFields[i].field);
    h.checks.CheckNum(da, db,
                      std::string("both sides moved ") + kPlainFields[i].name +
                          " identically -- the disabled side did the same work, it just "
                          "never entered the suggestion path");
  }
  h.checks.CheckNum(CounterDelta(before, after_on, &EpoCanvaEnglishCounters::queries),
                    kTypedKeysPerSide, "the enabled side ran one Query per keystroke");
  h.checks.CheckNum(CounterDelta(after_on, after_off, &EpoCanvaEnglishCounters::queries),
                    kTypedKeysPerSide, "and so did the disabled side");
  h.checks.CheckNum(
      CounterDelta(before, after_on, &EpoCanvaEnglishCounters::original_candidates),
      kTypedKeysPerSide, "every query offered the typed text as candidate 0");
  // The word list is not re-read by the second schema or by the second session.
  h.checks.CheckNum(CounterDelta(before, after_off, &EpoCanvaEnglishCounters::lexicon_load_attempts),
                    0, "no further word-list read across both phases");

  // Enabled side: the suggestion path ran, and recorded exactly what the documented menus
  // contain.
  h.checks.CheckNum(
      CounterDelta(before, after_on, &EpoCanvaEnglishCounters::spelling_suggested),
      kExpectedOnSuggested, "enabled: queries that found at least one suggestion");
  h.checks.CheckNum(
      CounterDelta(before, after_on, &EpoCanvaEnglishCounters::spelling_candidates_appended),
      kExpectedOnAppended, "enabled: suggestion rows that reached a menu");
  h.checks.CheckNum(
      CounterDelta(before, after_on, &EpoCanvaEnglishCounters::spelling_suggestions_lowercase),
      kExpectedOnLowercase, "enabled: rows rendered lowercase");
  h.checks.CheckNum(
      CounterDelta(before, after_on,
                   &EpoCanvaEnglishCounters::spelling_suggestions_initial_capital),
      kExpectedOnInitialCapital, "enabled: rows rendered initial-capital");
  h.checks.CheckNum(
      CounterDelta(before, after_on, &EpoCanvaEnglishCounters::spelling_suggestions_uppercase),
      kExpectedOnUppercase, "enabled: rows rendered uppercase (none were typed uppercase)");
  h.checks.CheckNum(
      CounterDelta(before, after_on, &EpoCanvaEnglishCounters::spelling_lexicon_unavailable),
      0, "enabled: a readable word list never reported the unavailable state");
  h.checks.Check(SpellingTotal(after_on) - SpellingTotal(before) > 0,
                 "enabled: the suggestion path contributed something",
                 std::to_string(SpellingTotal(after_on) - SpellingTotal(before)));
  h.checks.CheckNum(LL(after_on.spelling_variants_generated) > 0 ? 1 : 0, 1,
                    "enabled: edit variants were generated, so the path really ran");
  h.checks.CheckNum(LL(after_on.spelling_lexicon_lookups) > 0 ? 1 : 0, 1,
                    "enabled: and those variants were looked up in the word list");

  // Disabled side: the sum, and then each field with the reason its zero is the defined one.
  const long long off_total =
      SpellingTotal(after_off) - SpellingTotal(after_on);
  h.checks.CheckNum(off_total, 0,
                    "disabled: every spelling counter together moved by nothing");
  for (std::size_t f = 0; f < kSpellFieldCount; ++f) {
    h.checks.CheckNum(
        CounterDelta(after_on, after_off, kSpellFields[f].field), 0,
        std::string("disabled: ") + kSpellFields[f].name + " == 0 because " +
            kSpellFields[f].meaning_if_not_zero + " cannot happen with the switch off");
  }

  // --- tear down ------------------------------------------------------------------------
  // The commit boundary this case owns is the DEFAULT one, and it is asserted on both sides
  // for every probe above: nothing the user did beyond typing, and nothing the switch turned
  // on, replaced what was typed.
  // What this case deliberately does not test is the explicit-acceptance boundary --
  // spelling_commit_tests.cc owns that. The distinction is real and measured, so it is written
  // down rather than left to be rediscovered: highlight a row and press Return, and upstream
  // commits the highlighted row (Context::Commit() -> ConcreteEngine::OnCommit() ->
  // Context::GetCommitText() -> Composition::GetCommitText(), which reads the segment's
  // selected_index that Context::Highlight() set -- src/rime/context.cc:19-32, :132-137,
  // src/rime/composition.cc:102-109, src/rime/engine.cc:251-257). "A highlight is not an
  // acceptance" is a rule about EpoCanva's space processor (english_space_policy.h case A
  // commits the raw token), not about Return; no assertion here claims otherwise.
  DestroyBoth(h, sid_on, sid_off);

  EpoCanvaEnglishCounters final_counters;
  if (!ReadCounters(h.surface, &final_counters)) {
    return kCounterReadFailed;
  }
  std::printf("[english] counters.after_destroy     %s\n",
              DescribeCounters(final_counters).c_str());
  h.checks.CheckNum(LL(final_counters.translator_live), 0,
                    "no translator outlives its session");
  h.checks.CheckNum(LL(final_counters.space_processor_live), 0,
                    "no space processor outlives its session");
  h.checks.CheckNum(LL(final_counters.translator_constructed),
                    LL(final_counters.translator_destroyed),
                    "every translator constructed was destroyed");
  h.checks.CheckNum(LL(final_counters.space_processor_constructed),
                    LL(final_counters.space_processor_destroyed),
                    "every space processor constructed was destroyed");
  h.checks.CheckNum(LL(final_counters.lexicon_load_attempts), 1,
                    "and the word list was still read exactly once for the whole run");
  return kOk;
}

ExitCode RunSpellingToggleSelfCheckFailMode(Harness& h) {
  if (!h.surface.api_available) {
    Fail(kEnglishSurfaceUnavailable, "toggle-self.surface",
         "this control needs the english engine to have a switch that actually does something");
    return kEnglishSurfaceUnavailable;
  }
  RimeSessionId sid_on = kNoSession;
  RimeSessionId sid_off = kNoSession;
  DeployedEnglishConfig on_cfg;
  DeployedEnglishConfig off_cfg;
  ExitCode rc = SetupBothSides(h, &sid_on, &sid_off, &on_cfg, &off_cfg);
  if (rc != kOk) {
    DestroyBoth(h, sid_on, sid_off);
    return rc;
  }
  const std::string text = "helo";
  KeyEvent on_ev;
  KeyEvent off_ev;
  for (std::size_t i = 0; i < text.size(); ++i) {
    rc = SendKey(h.api, sid_on, static_cast<int>(static_cast<unsigned char>(text[i])), 0,
                 &h.balance, &on_ev);
    if (rc != kOk) {
      DestroyBoth(h, sid_on, sid_off);
      return rc;
    }
    rc = SendKey(h.api, sid_off, static_cast<int>(static_cast<unsigned char>(text[i])), 0,
                 &h.balance, &off_ev);
    if (rc != kOk) {
      DestroyBoth(h, sid_on, sid_off);
      return rc;
    }
  }
  std::printf("[english] toggle-self menus          ON=%s OFF=%s\n",
              JoinMenu(on_ev.state).c_str(), JoinMenu(off_ev.state).c_str());
  // The claim is false on purpose, and it is the exact degradation this group exists to catch:
  // a toggle comparison whose two sides are asserted but never actually different. Real
  // behaviour puts hello, help and hell behind the typed text on the enabled side and nothing
  // behind it on the disabled side, so this assertion must fail and the run must exit 14.
  h.checks.CheckStr(JoinMenu(on_ev.state), JoinMenu(off_ev.state),
                    "deliberately claimed that toggling the switch changed nothing");
  DestroyBoth(h, sid_on, sid_off);
  return kOk;  // the harness turns the recorded failure into exit 14
}

}  // namespace recovery
}  // namespace epocanva
