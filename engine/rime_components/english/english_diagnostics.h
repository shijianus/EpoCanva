/* EpoCanva P2.1 -- diagnostics surface of the compiled-in English component.
 *
 * WHY THIS EXISTS. A prefix-completion feature can fail in three ways that all
 * look identical from the candidate list: the module never loaded, the lexicon
 * file was missing or malformed, or the query genuinely had no matches. In every
 * one of those cases the component still offers the original input, so a test
 * that only sees "the text I typed came back" would pass while the feature was
 * dead. The P2.1 brief states this as a hard requirement: a missing or broken
 * word list must be identifiable, and an original-text fallback must never be
 * reported as a working completion module.
 *
 * It is therefore an observability surface, not a product API. Like the P1.4
 * probe surface it is reached through the module's own get_api() slot, so no
 * exported symbol is added and no permanent EpoCanva ABI is created
 * (docs/adr/0004-compiled-in-rime-components.md).
 *
 * BOUNDARY RULES (AGENTS.md section 2 and 5):
 *   - no STL type crosses the boundary in either direction;
 *   - counters are copied into a struct the CALLER allocated;
 *   - the only pointers handed out address static storage inside rime.dll
 *     (string literals) and must not be freed by the caller;
 *   - nothing here records a prefix, a word, a candidate or a commit: the fields
 *     are aggregate event counts plus one status name;
 *   - every count is an event count, not an offset or a length.
 *
 * LIFETIME: do not use this surface after RimeApi::finalize(). finalize() runs
 * ModuleManager::UnloadModules() and Registry::Clear()
 * (third_party/librime/src/rime_api_impl.h:58-63).
 */
#ifndef EPOCANVA_ENGLISH_DIAGNOSTICS_H_
#define EPOCANVA_ENGLISH_DIAGNOSTICS_H_

#include <rime_api.h> /* Bool, False, True, RimeCustomApi, RIME_STRUCT* */

#ifdef __cplusplus
extern "C" {
#endif

/* Module name for RimeTraits.modules, and component name for
 * engine/translators. Both are ASCII; they cross the C ABI as UTF-8 bytes. */
#define EPOCANVA_ENGLISH_MODULE_NAME "epocanva_english"
#define EPOCANVA_ENGLISH_TRANSLATOR_NAME "epocanva_english_translator"
/* P2.5: the space/commit boundary Processor, listed in engine/processors. Named
 * here because a schema that spells it differently silently gets no space policy
 * at all -- the integration test reads this exact string back from the engine. */
#define EPOCANVA_ENGLISH_SPACE_PROCESSOR_NAME "epocanva_english_space"
/* P2.6 candidate types. The C API exposes only a candidate's text and comment, so
 * the type strings below are internal; what a front end and the integration test can
 * distinguish is EPOCANVA_ENGLISH_SPELLING_COMMENT. "raw" is candidate 0, the text
 * the user typed. */
#define EPOCANVA_ENGLISH_COMPLETION_TYPE "epocanva_english_completion"
#define EPOCANVA_ENGLISH_SPELLING_TYPE "epocanva_english_spelling"
#define EPOCANVA_ENGLISH_SPELLING_COMMENT "spell"

/* Config keys, all read under the translator's own name space. */
#define EPOCANVA_ENGLISH_ENABLE_SUGGESTIONS_KEY "enable_suggestions"
#define EPOCANVA_ENGLISH_MAX_SUGGESTIONS_KEY "max_suggestions"

/* The lexicon path key, read from the schema under the translator's name space
 * (default "translator", or whatever "klass@name_space" selects -- see
 * third_party/librime/src/rime/ticket.cc:13-22). */
#define EPOCANVA_ENGLISH_LEXICON_PATH_KEY "lexicon_path"
#define EPOCANVA_ENGLISH_MAX_COMPLETIONS_KEY "max_completions"

/* Candidate budget, fixed for this phase: index 0 is always the original input
 * and at most EPOCANVA_ENGLISH_MAX_COMPLETIONS_DEFAULT completions follow, so a
 * segment never yields more than EPOCANVA_ENGLISH_MAX_TOTAL_CANDIDATES items.
 * The numbers are duplicated deliberately: a test that re-derived its expectation
 * from the same expression could not detect the budget changing. */
#define EPOCANVA_ENGLISH_MAX_COMPLETIONS_DEFAULT 7
#define EPOCANVA_ENGLISH_MAX_TOTAL_CANDIDATES 8

/* P2.6: the same discipline for suggestions. Three is the number this phase ships
 * with; a schema may lower it, never raise it, so "one original plus at most seven
 * further candidates" cannot be widened by configuration however many rows are one
 * edit away. Suggestions and completions are mutually exclusive per query (a query
 * that produced any completion produces no suggestion), so the two ceilings never
 * add up. */
#define EPOCANVA_ENGLISH_MAX_SUGGESTIONS_DEFAULT 3

typedef struct epocanva_english_counters_t {
  int data_size;

  /* Module lifecycle, moved by ModuleManager::LoadModule / UnloadModules. */
  unsigned long long module_initialized;
  unsigned long long module_finalized;

  /* Translator object lifecycle. live_instances is maintained directly by the
   * constructor and destructor rather than derived from two other counters. */
  unsigned long long translator_constructed;
  unsigned long long translator_destroyed;
  unsigned long long translator_live;

  /* Lexicon loading. attempts moves ONLY on a cache miss, which is what makes
   * "the word list is not re-read for every keypress" an observable fact:
   * attempts stays 1 while queries climbs. */
  unsigned long long lexicon_load_attempts;
  unsigned long long lexicon_load_successes;
  unsigned long long lexicon_load_failures;
  unsigned long long loaded_entry_count;
  /* 1-based line the most recent load stopped parsing on; 0 when the failure is
   * not line-attributable. Lets a test name WHICH row of a bad word list was
   * rejected instead of only noticing "something was wrong". */
  unsigned long long last_load_error_line;

  /* Query accounting. */
  unsigned long long queries;              /* Query() entered with non-empty input */
  unsigned long long queries_with_index;   /* Query() that consulted a loaded index */
  unsigned long long queries_rejected;     /* prefix refused by the input contract */
  unsigned long long original_candidates;  /* candidates carrying the raw input */
  unsigned long long completion_candidates; /* candidates that came from the index */

  /* 1 when the schema asked for a lexicon path and none was configured. */
  unsigned long long config_missing_path;

  /* --- P2.2: why a query produced no completions. All four of these stay
   * distinct on purpose. "Looked up and nothing matched", "recognised letters but
   * an unrecognised case shape", "not even a letter-only word" and "the word list
   * failed to load" are different facts, and collapsing them into one empty-candidate
   * result is precisely how a dead completion feature keeps looking green.
   * queries_rejected above is kept and equals mixed + not_alphabetic + too_long. */
  unsigned long long queries_no_match;        /* completable, looked up, zero rows */
  unsigned long long queries_mixed_case;      /* "HeL", "hEL", "iPh": policy refusal */
  unsigned long long queries_not_alphabetic;  /* a non A-Z / a-z byte is present */
  unsigned long long queries_too_long;        /* over kMaxCaseInputBytes, not truncated */

  /* Completions per case shape, so "the mapping ran" is observable separately from
   * "a completion appeared". For a lowercase input the display text equals the
   * canonical row, so lowercase mappings are counted too. */
  unsigned long long completions_lowercase;
  unsigned long long completions_initial_capital;
  unsigned long long completions_uppercase;

  /* --- P2.3: why a query stopped, at the level of the WORD FORM and of the
   * protecting byte class. The brief's requirement is that "user@example.com",
   * "he2", "'hello" and "rock'n'roll" all come back as the untouched original text
   * -- and that a test can still tell those four apart instead of seeing four
   * identical empty completion lists.
   *
   * queries_not_alphabetic above keeps its P2.2 role ("a byte outside this
   * layer's alphabet is present"), but P2.3 widened that alphabet with the two
   * word delimiters, so the bucket now excludes them and the three fields below
   * say WHICH class of byte fired.
   *
   * Identities, asserted by the integration test rather than narrated:
   *   queries_rejected    = mixed_case + too_long + not_alphabetic + illegal_word_form
   *   queries_not_alphabetic = non_ascii + protected + unsupported_byte
   *   completion_candidates  = lowercase + initial_capital + uppercase
   *   completions_with_delimiter <= completion_candidates                  */
  unsigned long long queries_illegal_word_form;  /* apostrophe/hyphen misplaced,
                                                    doubled, or both kinds at once */
  unsigned long long queries_protected;          /* a digit or one of @ . _ : / \ ? & = # % + */
  unsigned long long queries_non_ascii;          /* a byte >= 0x80; not decoded */
  unsigned long long queries_unsupported_byte;   /* printable ASCII with no rule here */

  /* Completions whose display text contains a word delimiter, i.e. the row that
   * came out of the index really was "don't" / "co-operate" rather than a plain
   * letter word. Separated from the case-shape counters because a delimited word
   * also has a case shape, and the two facts are tested independently. */
  unsigned long long completions_with_delimiter;

  /* Segment scope, expressed as event counts rather than offsets (AGENTS.md
   * section 2 keeps composition content and positions off this surface). Query()
   * is called once per segment, so
   *   queries_covering_whole_input + queries_covering_part_of_input == queries
   * "part of input" means the engine handed this translator a segment that is NOT
   * the whole composition. For P2.3 that is the difference between protection and
   * corruption: if "user@example.com" were cut into an "abc" segment "user" plus a
   * raw tail, the letters would be completed while the user was typing an address,
   * and from the public C API the only way to SEE that is this counter moving. A
   * test asserts it stays 0 while protected text is typed and the whole token is
   * offered back unchanged. */
  unsigned long long queries_covering_whole_input;
  unsigned long long queries_covering_part_of_input;

  /* --- P2.5: the space bar and the commit boundary. Appended at the end for the
   * same reason P2.3 appended its fields: every field above keeps its byte offset,
   * so a recorded counter name still means the same thing.
   *
   * These exist because "the word was committed once and then one space" is not
   * observable from the concatenated commit string alone: a component that
   * committed "hello", "hello" and " " in a row would hand the caller exactly the
   * same text as the correct one. The counts say how many commits were ISSUED, from
   * which case, which is the fact the P2.5 brief asks to be checked per event.
   *
   * Identities, asserted by the integration test rather than narrated:
   *   space_events_seen = case_a + case_b + case_c + space_events_refused
   *   composition_commits_issued = case_a + case_b
   *   space_characters_emitted   = case_a + case_b + case_c
   *   processor_key_events >= space_events_seen                       */
  unsigned long long space_processor_constructed;
  unsigned long long space_processor_destroyed;
  unsigned long long space_processor_live;
  /* Every ProcessKeyEvent this processor entered -- letters included, because
   * "the processor is first in the chain but releases everything that is not a
   * space" has to be visible as a count, not asserted in prose. */
  unsigned long long processor_key_events;
  /* Of those, the ones whose keysym was XK_space (press, release or modified). */
  unsigned long long space_events_seen;
  /* Space events this processor refused: releases and ctrl/alt/super combinations.
   * Refusing means returning kNoop, i.e. the event continues down the chain. */
  unsigned long long space_events_refused;
  /* A / B / C of engine/languages/en/english_space_policy.h. */
  unsigned long long space_case_a_raw_token;
  unsigned long long space_case_b_selection;
  unsigned long long space_case_c_empty;
  /* Commits of composing text issued (case A: the raw input string; case B: the
   * engine's own commit text for the confirmed composition). */
  unsigned long long composition_commits_issued;
  /* U+0020 commits issued. Deliberately counted apart from the text commits so a
   * missing space, or a doubled one, cannot hide inside a single total. */
  unsigned long long space_characters_emitted;

  /* --- P2.6: one-edit spelling suggestions, and the reason a query did not get
   * any. Appended at the end again, so no offset above changes.
   *
   * These exist for the same reason as the P2.2/P2.3 buckets: "no suggestions" has
   * at least eight distinguishable causes, and a menu that looks the same for all of
   * them (one original candidate) tells a user nothing and tells a test less. The
   * engine counts the outcome the CORE reported (SpellingOutcome), so the names here
   * are not a second opinion the engine formed about its own input.
   *
   * Identities, asserted by the integration test:
   *   spelling_suggested + spelling_no_candidate == every query that searched
   *   spelling_lexicon_lookups >= spelling_variants_generated - dedup, and both are
   *     reported by the core rather than recomputed here
   *   spelling_candidates_appended = lowercase + initial_capital + uppercase
   *   spelling_candidates_appended <= 7, and the menu stays <= 8 because a query
   *     that completes never suggests                                       */
  unsigned long long spelling_suggested;          /* >=1 suggestion existed */
  unsigned long long spelling_no_candidate;       /* searched, nothing one edit away */
  unsigned long long spelling_candidates_appended; /* candidates that reached a menu */
  unsigned long long spelling_variants_generated;  /* cumulative, core-reported */
  unsigned long long spelling_lexicon_lookups;     /* cumulative Find() calls */
  unsigned long long spelling_skipped_word_in_lexicon;
  unsigned long long spelling_skipped_completion;
  unsigned long long spelling_skipped_protected;
  unsigned long long spelling_skipped_delimited;
  unsigned long long spelling_skipped_mixed_case;
  unsigned long long spelling_skipped_too_short;
  unsigned long long spelling_skipped_too_long;
  unsigned long long spelling_skipped_unsupported_byte;
  unsigned long long spelling_skipped_non_ascii;
  unsigned long long spelling_skipped_empty;
  /* The one spelling state that must never be folded into "no suggestions": the
   * schema asked and there is no usable word list. */
  unsigned long long spelling_lexicon_unavailable;
  unsigned long long spelling_suggestions_lowercase;
  unsigned long long spelling_suggestions_initial_capital;
  unsigned long long spelling_suggestions_uppercase;
} EpoCanvaEnglishCounters;

typedef struct epocanva_english_api_t {
  int data_size; /* must stay first: it is RimeCustomApi's only member */

  /* Copies the counters into *out. False when out is NULL or too small for this
   * build's layout; on False nothing is written. Never allocates. */
  Bool (*get_counters)(EpoCanvaEnglishCounters* out);

  /* Name of the most recent lexicon load outcome, e.g. "ok", "file-not-found",
   * "duplicate-word". Points at a string literal inside rime.dll; read-only,
   * never freed by the caller. Returns "never-attempted" before any load. */
  const char* (*last_load_status_name)(void);

  /* Ties the observed behaviour to the exact source that produced it. */
  const char* (*build_id)(void);
} EpoCanvaEnglishApi;

#ifdef __cplusplus
}
#endif

#endif /* EPOCANVA_ENGLISH_DIAGNOSTICS_H_ */
