// EpoCanva P2.1 -- English Translator implementation and its counter storage.
//
// Compiled INTO rime.dll (tools/cmake/epocanva_rime_compiled_in_components.cmake),
// so the objects the constructor `new`s and the objects the engine's an<> holder
// deletes live in the same PE module and the same CRT heap
// (docs/adr/0004-compiled-in-rime-components.md).
//
// This translation unit emits no LOG()/DLOG() of any kind. Query() receives the
// user's composing text; one log line would put it on disk (AGENTS.md section 2).
#include "english_translator.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>

#include <rime/candidate.h>
#include <rime/common.h>
#include <rime/config.h>
#include <rime/deployer.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime/translation.h>

#include "english_space_processor.h"
#include "spelling_suggester.h"

namespace epocanva {
namespace english {
namespace {

using epocanva::en::EnglishLexicon;
using epocanva::en::LexiconLoadInfo;
using epocanva::en::LexiconStatus;

std::atomic<unsigned long long> g_module_initialized{0};
std::atomic<unsigned long long> g_module_finalized{0};
std::atomic<unsigned long long> g_translator_constructed{0};
std::atomic<unsigned long long> g_translator_destroyed{0};
std::atomic<unsigned long long> g_translator_live{0};
std::atomic<unsigned long long> g_load_attempts{0};
std::atomic<unsigned long long> g_load_successes{0};
std::atomic<unsigned long long> g_load_failures{0};
std::atomic<unsigned long long> g_loaded_entry_count{0};
std::atomic<unsigned long long> g_last_load_error_line{0};
std::atomic<unsigned long long> g_queries{0};
std::atomic<unsigned long long> g_queries_with_index{0};
std::atomic<unsigned long long> g_queries_rejected{0};
std::atomic<unsigned long long> g_original_candidates{0};
std::atomic<unsigned long long> g_completion_candidates{0};
std::atomic<unsigned long long> g_config_missing_path{0};
// P2.2: the four "no completions" reasons, kept apart rather than folded into one
// empty-candidate outcome.
std::atomic<unsigned long long> g_queries_no_match{0};
std::atomic<unsigned long long> g_queries_mixed_case{0};
std::atomic<unsigned long long> g_queries_not_alphabetic{0};
std::atomic<unsigned long long> g_queries_too_long{0};
std::atomic<unsigned long long> g_completions_lowercase{0};
std::atomic<unsigned long long> g_completions_initial_capital{0};
std::atomic<unsigned long long> g_completions_uppercase{0};
// P2.3: why a query stopped, at word-form and byte-class level. See
// english_diagnostics.h for the identities these are asserted against.
std::atomic<unsigned long long> g_queries_illegal_word_form{0};
std::atomic<unsigned long long> g_queries_protected{0};
std::atomic<unsigned long long> g_queries_non_ascii{0};
std::atomic<unsigned long long> g_queries_unsupported_byte{0};
std::atomic<unsigned long long> g_completions_with_delimiter{0};
// Segment-scope accounting. The whole point of P2.3's protection rule is that a
// technical token must reach this Translator as ONE segment: if the engine had
// split "user@example.com" into "user" plus a raw tail, the letter part would
// still be completed and the rule would look implemented while corrupting input.
// Both fields are event counts (AGENTS.md section 2 forbids putting composition
// content or offsets on this surface), and whole + part == queries.
std::atomic<unsigned long long> g_queries_covering_whole_input{0};
std::atomic<unsigned long long> g_queries_covering_part_of_input{0};

// --- P2.6: spelling suggestion accounting --------------------------------------
// Every field mirrors one SpellingOutcome, so the engine reports what the CORE
// decided instead of forming its own opinion about why nothing was offered. See
// english_diagnostics.h for the identities the integration test asserts.
std::atomic<unsigned long long> g_spelling_suggested{0};
std::atomic<unsigned long long> g_spelling_no_candidate{0};
std::atomic<unsigned long long> g_spelling_candidates_appended{0};
std::atomic<unsigned long long> g_spelling_variants_generated{0};
std::atomic<unsigned long long> g_spelling_lexicon_lookups{0};
std::atomic<unsigned long long> g_spelling_skipped_word_in_lexicon{0};
std::atomic<unsigned long long> g_spelling_skipped_completion{0};
std::atomic<unsigned long long> g_spelling_skipped_protected{0};
std::atomic<unsigned long long> g_spelling_skipped_delimited{0};
std::atomic<unsigned long long> g_spelling_skipped_mixed_case{0};
std::atomic<unsigned long long> g_spelling_skipped_too_short{0};
std::atomic<unsigned long long> g_spelling_skipped_too_long{0};
std::atomic<unsigned long long> g_spelling_skipped_unsupported_byte{0};
std::atomic<unsigned long long> g_spelling_skipped_non_ascii{0};
std::atomic<unsigned long long> g_spelling_skipped_empty{0};
std::atomic<unsigned long long> g_spelling_lexicon_unavailable{0};
std::atomic<unsigned long long> g_spelling_lowercase{0};
std::atomic<unsigned long long> g_spelling_initial_capital{0};
std::atomic<unsigned long long> g_spelling_uppercase{0};

// One place turns a SpellingResult into counters, so no branch of Query() can count
// the same state differently from another branch. The variant and lookup totals are
// cumulative across the process, which is what makes "the work per keystroke is
// bounded and observed" checkable in the performance run.
void CountSpellingOutcome(const epocanva::en::SpellingResult& spelling) {
  using epocanva::en::SpellingOutcome;
  switch (spelling.outcome) {
    case SpellingOutcome::kSuggested:
      g_spelling_suggested.fetch_add(1, std::memory_order_relaxed);
      break;
    case SpellingOutcome::kNoCandidate:
      g_spelling_no_candidate.fetch_add(1, std::memory_order_relaxed);
      break;
    case SpellingOutcome::kWordInLexicon:
      g_spelling_skipped_word_in_lexicon.fetch_add(1, std::memory_order_relaxed);
      break;
    case SpellingOutcome::kCompletionAvailable:
      g_spelling_skipped_completion.fetch_add(1, std::memory_order_relaxed);
      break;
    case SpellingOutcome::kProtectedInput:
      g_spelling_skipped_protected.fetch_add(1, std::memory_order_relaxed);
      break;
    case SpellingOutcome::kDelimitedForm:
      g_spelling_skipped_delimited.fetch_add(1, std::memory_order_relaxed);
      break;
    case SpellingOutcome::kMixedCase:
      g_spelling_skipped_mixed_case.fetch_add(1, std::memory_order_relaxed);
      break;
    case SpellingOutcome::kTooShort:
      g_spelling_skipped_too_short.fetch_add(1, std::memory_order_relaxed);
      break;
    case SpellingOutcome::kTooLong:
      g_spelling_skipped_too_long.fetch_add(1, std::memory_order_relaxed);
      break;
    case SpellingOutcome::kUnsupportedByte:
      g_spelling_skipped_unsupported_byte.fetch_add(1, std::memory_order_relaxed);
      break;
    case SpellingOutcome::kNonAsciiInput:
      g_spelling_skipped_non_ascii.fetch_add(1, std::memory_order_relaxed);
      break;
    case SpellingOutcome::kEmptyInput:
      g_spelling_skipped_empty.fetch_add(1, std::memory_order_relaxed);
      break;
    case SpellingOutcome::kLexiconUnavailable:
      g_spelling_lexicon_unavailable.fetch_add(1, std::memory_order_relaxed);
      break;
    case SpellingOutcome::kDisabled:
      // Unreachable from here: the engine asks the core only when its own switch is
      // on. Counted as nothing rather than as a skip, so a future caller bug shows up
      // as a missing count.
      break;
  }
  g_spelling_variants_generated.fetch_add(spelling.variants_generated,
                                          std::memory_order_relaxed);
  g_spelling_lexicon_lookups.fetch_add(spelling.lexicon_lookups,
                                       std::memory_order_relaxed);
}

// Always a pointer into the core's static string table (english_lexicon.cc's
// LexiconStatusName), so the value cannot dangle when a cache entry is dropped.
std::atomic<const char*> g_last_status_name{"never-attempted"};

std::mutex g_cache_mutex;
std::map<std::string, std::shared_ptr<const LexiconRecord>> g_cache;

}  // namespace

std::shared_ptr<const LexiconRecord> LexiconRecord::FromSuccessfulLoad(
    const std::shared_ptr<const EnglishLexicon>& loaded) {
  auto record = std::make_shared<LexiconRecord>();
  record->status = LexiconStatus::kOk;
  record->status_literal = epocanva::en::LexiconStatusName(LexiconStatus::kOk);
  record->error_line = 0;
  record->entry_count = loaded ? loaded->size() : 0;
  record->lexicon = loaded;
  return record;
}

std::shared_ptr<const LexiconRecord> LexiconRecord::FromFailedLoad(
    const LexiconLoadInfo& info) {
  auto record = std::make_shared<LexiconRecord>();
  // kOk with no object would mean "successfully loaded nothing", which is exactly
  // the confusion this surface exists to prevent. If a caller ever produces that
  // combination, report it as unreadable rather than as success.
  record->status =
      (info.status == LexiconStatus::kOk) ? LexiconStatus::kFileUnreadable
                                          : info.status;
  record->status_literal = epocanva::en::LexiconStatusName(record->status);
  record->error_line = info.line;
  record->entry_count = 0;
  record->lexicon = nullptr;
  return record;
}

std::shared_ptr<const LexiconRecord> EnglishLexiconCache::Acquire(
    const std::filesystem::path& resolved_path) {
  // The map key is the normalised UTF-8 form of the path; the load itself always
  // receives a path object.
  const std::string key = resolved_path.u8string();
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  const auto found = g_cache.find(key);
  if (found != g_cache.end()) {
    // Cache hit: no file is opened and no index is rebuilt. This is the branch a
    // keystroke takes, and it is why lexicon_load_attempts stays at 1 while
    // queries climbs.
    return found->second;
  }
  g_load_attempts.fetch_add(1, std::memory_order_relaxed);
  LexiconLoadInfo info;
  std::unique_ptr<EnglishLexicon> parsed =
      EnglishLexicon::FromFile(resolved_path, &info);
  std::shared_ptr<const EnglishLexicon> loaded;
  if (parsed) {
    loaded = std::shared_ptr<const EnglishLexicon>(std::move(parsed));
  }
  std::shared_ptr<const LexiconRecord> record;
  if (loaded) {
    record = LexiconRecord::FromSuccessfulLoad(loaded);
    g_load_successes.fetch_add(1, std::memory_order_relaxed);
    g_loaded_entry_count.store(record->entry_count, std::memory_order_relaxed);
  } else {
    record = LexiconRecord::FromFailedLoad(info);
    g_load_failures.fetch_add(1, std::memory_order_relaxed);
    g_loaded_entry_count.store(0, std::memory_order_relaxed);
  }
  g_last_status_name.store(record->status_literal, std::memory_order_relaxed);
  g_last_load_error_line.store(record->error_line, std::memory_order_relaxed);
  g_cache.emplace(key, record);
  return record;
}

void EnglishLexiconCache::Clear() {
  std::lock_guard<std::mutex> lock(g_cache_mutex);
  g_cache.clear();
}

EpoCanvaEnglishTranslator::EpoCanvaEnglishTranslator(
    const rime::Ticket& ticket)
    : rime::Translator(ticket) {
  g_translator_constructed.fetch_add(1, std::memory_order_relaxed);
  g_translator_live.fetch_add(1, std::memory_order_relaxed);

  rime::Config* config = ticket.schema ? ticket.schema->config() : nullptr;
  if (!config) {
    g_config_missing_path.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const std::string path_key =
      name_space() + "/" EPOCANVA_ENGLISH_LEXICON_PATH_KEY;
  std::string configured;
  if (!config->GetString(path_key, &configured) || configured.empty()) {
    // A schema that lists the component but names no word list is a
    // configuration error, and it must be countable: otherwise the engine looks
    // like it is completing and is only echoing.
    g_config_missing_path.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // Resolution base is the engine's shared data directory, which the front end
  // sets through RimeTraits.shared_data_dir (third_party/librime/src/rime/
  // setup.cc:46-47 -> deployer.h:35). It is NOT the process working directory:
  // a relative path is meaningless there, and Deployer's own default is "."
  // (deployer.cc:16), which is exactly why the test program always sets
  // shared_data_dir explicitly.
  rime::path resolved(configured);
  if (!resolved.is_absolute()) {
    resolved = rime::Service::instance().deployer().shared_data_dir / resolved;
  }
  resolved = resolved.lexically_normal();
  // One cache entry per normalised path: the load happens on the first Translator
  // built for it and every later session and every keystroke take the cached
  // branch.
  record_ = EnglishLexiconCache::Acquire(std::filesystem::path(resolved));

  // The candidate budget is a contract, so the schema may only lower it.
  const std::string max_key =
      name_space() + "/" EPOCANVA_ENGLISH_MAX_COMPLETIONS_KEY;
  int requested = EPOCANVA_ENGLISH_MAX_COMPLETIONS_DEFAULT;
  if (config->GetInt(max_key, &requested)) {
    const int clamped = std::max(
        0, std::min(requested, static_cast<int>(
                                  EPOCANVA_ENGLISH_MAX_COMPLETIONS_DEFAULT)));
    max_completions_ = static_cast<std::size_t>(clamped);
  }
  // P2.6: the same one-way clamp for suggestions, and an opt-in switch. A schema
  // that names neither key gets exactly the pre-P2.6 behaviour, which is what makes
  // the 26 pre-existing CTest cases a regression proof rather than a re-derivation.
  const std::string enable_key =
      name_space() + "/" EPOCANVA_ENGLISH_ENABLE_SUGGESTIONS_KEY;
  // Config::GetBool takes a bool* (src/rime/config/config_component.h:43), which is
  // not the API's Bool, so the value is read into its own type and only then applied.
  bool enabled = false;
  if (config->GetBool(enable_key, &enabled)) {
    spelling_enabled_ = enabled;
  }
  const std::string suggestion_key =
      name_space() + "/" EPOCANVA_ENGLISH_MAX_SUGGESTIONS_KEY;
  int suggested = EPOCANVA_ENGLISH_MAX_SUGGESTIONS_DEFAULT;
  if (config->GetInt(suggestion_key, &suggested)) {
    max_suggestions_ = static_cast<std::size_t>(std::max(
        0, std::min(suggested,
                    static_cast<int>(
                        EPOCANVA_ENGLISH_MAX_SUGGESTIONS_DEFAULT))));
  }
}

EpoCanvaEnglishTranslator::~EpoCanvaEnglishTranslator() {
  // Runs when the last an<Translator> in the engine's translators_ vector goes
  // away, i.e. when the engine and with it the session is released.
  g_translator_live.fetch_sub(1, std::memory_order_relaxed);
  g_translator_destroyed.fetch_add(1, std::memory_order_relaxed);
}

rime::an<rime::Translation> EpoCanvaEnglishTranslator::Query(
    const std::string& input, const rime::Segment& segment) {
  if (input.empty()) {
    // Nothing to translate; the engine skips a null translation.
    return nullptr;
  }
  g_queries.fetch_add(1, std::memory_order_relaxed);
  // Was this Query() about the whole composition, or about one segment of a longer
  // one? A protected token must arrive whole: if the engine had cut
  // "user@example.com" into an "abc" segment "user" plus a raw tail, the letters in
  // front of the @ would still be completed and the protection rule would be a
  // decoration. `input` is this segment's own slice (src/rime/engine.cc:211), so
  // comparing its span with the composition length is the only way to see a split
  // from here, and it puts no composition content on the diagnostics surface.
  {
    rime::Context* context = engine_ ? engine_->context() : nullptr;
    const std::size_t composition_bytes = context ? context->input().size() : 0;
    const bool whole =
        (segment.start == 0 && input.size() == composition_bytes &&
         segment.end == composition_bytes);
    (whole ? g_queries_covering_whole_input : g_queries_covering_part_of_input)
        .fetch_add(1, std::memory_order_relaxed);
  }

  auto translation = rime::New<rime::FifoTranslation>();
  if (!translation) {
    return nullptr;
  }
  // Position 0 is ALWAYS the original input. Because this schema lists exactly
  // one translator, the menu holds one translation and FifoTranslation emits
  // candidates in append order -- so the merge order is set by this function,
  // not inherited from echo_translator's quality trick
  // (third_party/librime/src/rime/engine.cc:203-231 -> menu.cc:15-22 ->
  // translation.cc:127-152 with a single element in translations_).
  auto original = rime::New<rime::SimpleCandidate>(
      "raw", segment.start, segment.end, input);
  if (!original) {
    return nullptr;
  }
  translation->Append(original);
  g_original_candidates.fetch_add(1, std::memory_order_relaxed);

  const bool has_index = record_ && record_->lexicon;
  if (!has_index) {
    // Missing word list, unreadable word list or no lexicon_path configured. The
    // original candidate above is a deliberate fallback so the user is never
    // stuck; the counters and last_load_status_name are what a test reads to
    // avoid calling that fallback "completion working".
    if (spelling_enabled_) {
      // P2.6's contribution to the same rule: a schema that asked for spelling
      // suggestions must be able to tell "there is no word list" from "nothing you
      // typed is misspelled". Asked the same way as any other query -- with the null
      // index -- so the state comes from the core's gate rather than from an
      // assumption made here.
      const epocanva::en::SpellingResult spelling =
          epocanva::en::SuggestSpelling(nullptr, input, /*completion_candidates=*/0,
                                        max_suggestions_, /*enabled=*/true);
      CountSpellingOutcome(spelling);
    }
    return translation;
  }
  g_queries_with_index.fetch_add(1, std::memory_order_relaxed);
  // The raw composing text is passed by value into the core and only ever read
  // there: it is never lowercased in place, so candidate 0 and the eventual
  // default commit keep the user's own keys.
  const epocanva::en::CompletionResult result =
      epocanva::en::CompletePrefix(*record_->lexicon, input, max_completions_);
  // One outcome per query, computed by the core from BOTH gates (token shape,
  // then casing). Counting result.outcome here rather than re-deriving it from
  // case_style is what keeps the engine side and the core side from growing
  // different opinions about why a candidate list came back empty.
  switch (result.outcome) {
    case epocanva::en::CompletionOutcome::kEmptyInput:
      // Unreachable: Query() returned early above for an empty input. Counted as
      // nothing rather than silently attributed to another bucket.
      break;
    case epocanva::en::CompletionOutcome::kMixedCase:
      // Recognised letters, unrecognised pattern. Not guessed, and explicitly NOT
      // recorded as a word-list problem. `break`, not `return`: the suggestion step
      // below has its own gate for this input and reports its own named reason.
      g_queries_rejected.fetch_add(1, std::memory_order_relaxed);
      g_queries_mixed_case.fetch_add(1, std::memory_order_relaxed);
      break;
    case epocanva::en::CompletionOutcome::kInputTooLong:
      g_queries_rejected.fetch_add(1, std::memory_order_relaxed);
      g_queries_too_long.fetch_add(1, std::memory_order_relaxed);
      break;
    case epocanva::en::CompletionOutcome::kIllegalWordForm:
      // A misplaced, doubled or mixed pair of delimiters: the offending bytes ARE
      // word delimiters, so this is a word-form refusal, not a technical-text one.
      g_queries_rejected.fetch_add(1, std::memory_order_relaxed);
      g_queries_illegal_word_form.fetch_add(1, std::memory_order_relaxed);
      break;
    case epocanva::en::CompletionOutcome::kNonAscii:
    case epocanva::en::CompletionOutcome::kProtectedText:
    case epocanva::en::CompletionOutcome::kUnsupportedByte:
      // All three mean "a byte outside this layer's alphabet", which is what
      // queries_not_alphabetic has counted since P2.2; the specialised field says
      // which class fired. Nothing was stripped or normalised to get here.
      g_queries_rejected.fetch_add(1, std::memory_order_relaxed);
      g_queries_not_alphabetic.fetch_add(1, std::memory_order_relaxed);
      if (result.outcome == epocanva::en::CompletionOutcome::kProtectedText) {
        g_queries_protected.fetch_add(1, std::memory_order_relaxed);
      } else if (result.outcome == epocanva::en::CompletionOutcome::kNonAscii) {
        g_queries_non_ascii.fetch_add(1, std::memory_order_relaxed);
      } else {
        g_queries_unsupported_byte.fetch_add(1, std::memory_order_relaxed);
      }
      break;
    case epocanva::en::CompletionOutcome::kNoMatch:
      // A lookup really ran and the index holds nothing with that prefix -- the one
      // remaining zero-candidate answer for a healthy word list. This is also the
      // only completion outcome that can be a MISSPELLING, so it must fall through
      // to the suggestion step rather than return here.
      g_queries_no_match.fetch_add(1, std::memory_order_relaxed);
      break;
    case epocanva::en::CompletionOutcome::kCompleted:
      break;
  }
  // Every refusal above leaves result.completions empty by construction, so the
  // loop is a no-op for them and no early return is needed to protect it.
  for (const epocanva::en::Completion& completion : result.completions) {
    // The DISPLAY text is what a selected candidate commits, and it is the row the
    // core already de-duplicated against the raw input, so "hello" mapped to
    // "Hello" for the input "Hello" cannot appear twice.
    auto candidate = rime::New<rime::SimpleCandidate>(
        EPOCANVA_ENGLISH_COMPLETION_TYPE, segment.start, segment.end,
        completion.display);
    if (!candidate) {
      break;
    }
    translation->Append(candidate);
    g_completion_candidates.fetch_add(1, std::memory_order_relaxed);
    // A delimited completion is a different fact from a case-mapped one: the row
    // came out of the index as don't / co-operate, not as a plain letter word that
    // happened to be re-cased.
    if (completion.display.find(epocanva::en::kApostrophe) != std::string::npos ||
        completion.display.find(epocanva::en::kHyphenMinus) != std::string::npos) {
      g_completions_with_delimiter.fetch_add(1, std::memory_order_relaxed);
    }
    switch (result.case_style) {
      case epocanva::en::CaseStyle::kLowercase:
        g_completions_lowercase.fetch_add(1, std::memory_order_relaxed);
        break;
      case epocanva::en::CaseStyle::kInitialCapital:
        g_completions_initial_capital.fetch_add(1, std::memory_order_relaxed);
        break;
      case epocanva::en::CaseStyle::kUppercase:
        g_completions_uppercase.fetch_add(1, std::memory_order_relaxed);
        break;
      default:
        break;
    }
  }

  // --- P2.6: one-edit spelling suggestions, offered only when completions were not.
  //
  // Gate 9 is fed result.match_count -- the number of completion rows the index held
  // AFTER the "would repeat the input" removal and BEFORE the budget cut -- not
  // result.completions.size(). A schema that lowered max_completions to 0 has an
  // empty list and still has completions, and treating that as "no completions" would
  // make the suppression rule depend on a budget number.
  if (spelling_enabled_) {
    const epocanva::en::SpellingResult spelling =
        epocanva::en::SuggestSpelling(record_->lexicon.get(), input,
                                      result.match_count, max_suggestions_,
                                      /*enabled=*/true);
    CountSpellingOutcome(spelling);
    for (const epocanva::en::SpellingSuggestion& suggestion : spelling.suggestions) {
      // A distinct candidate type, and the fixed comment a front end can actually see
      // through the C API (RimeCandidate carries text and comment only). The comment
      // is a constant label: it never carries the input, the edit or a distance.
      auto candidate = rime::New<rime::SimpleCandidate>(
          EPOCANVA_ENGLISH_SPELLING_TYPE, segment.start, segment.end,
          suggestion.display, EPOCANVA_ENGLISH_SPELLING_COMMENT);
      if (!candidate) {
        break;
      }
      translation->Append(candidate);
      g_spelling_candidates_appended.fetch_add(1, std::memory_order_relaxed);
      switch (spelling.case_style) {
        case epocanva::en::CaseStyle::kLowercase:
          g_spelling_lowercase.fetch_add(1, std::memory_order_relaxed);
          break;
        case epocanva::en::CaseStyle::kInitialCapital:
          g_spelling_initial_capital.fetch_add(1, std::memory_order_relaxed);
          break;
        case epocanva::en::CaseStyle::kUppercase:
          g_spelling_uppercase.fetch_add(1, std::memory_order_relaxed);
          break;
        default:
          break;
      }
    }
  }
  return translation;
}

// --- diagnostics storage -------------------------------------------------------

void NoteModuleInitialized() {
  g_module_initialized.fetch_add(1, std::memory_order_relaxed);
}

void NoteModuleFinalized() {
  g_module_finalized.fetch_add(1, std::memory_order_relaxed);
}

Bool CopyEnglishCounters(EpoCanvaEnglishCounters* out) {
  if (!out) {
    return False;
  }
  // Same versioning rule as the P1.4 surface: the payload size is everything
  // after data_size, matching RIME_STRUCT_INIT (third_party/librime/src/
  // rime_api.h:60-61). Refuse a smaller caller struct instead of overrunning it.
  const int payload =
      static_cast<int>(sizeof(EpoCanvaEnglishCounters) - sizeof(int));
  if (out->data_size < payload) {
    return False;
  }
  out->module_initialized =
      g_module_initialized.load(std::memory_order_relaxed);
  out->module_finalized = g_module_finalized.load(std::memory_order_relaxed);
  out->translator_constructed =
      g_translator_constructed.load(std::memory_order_relaxed);
  out->translator_destroyed =
      g_translator_destroyed.load(std::memory_order_relaxed);
  out->translator_live = g_translator_live.load(std::memory_order_relaxed);
  out->lexicon_load_attempts = g_load_attempts.load(std::memory_order_relaxed);
  out->lexicon_load_successes = g_load_successes.load(std::memory_order_relaxed);
  out->lexicon_load_failures = g_load_failures.load(std::memory_order_relaxed);
  out->loaded_entry_count = g_loaded_entry_count.load(std::memory_order_relaxed);
  out->last_load_error_line = g_last_load_error_line.load(std::memory_order_relaxed);
  out->queries = g_queries.load(std::memory_order_relaxed);
  out->queries_with_index = g_queries_with_index.load(std::memory_order_relaxed);
  out->queries_rejected = g_queries_rejected.load(std::memory_order_relaxed);
  out->original_candidates = g_original_candidates.load(std::memory_order_relaxed);
  out->completion_candidates =
      g_completion_candidates.load(std::memory_order_relaxed);
  out->config_missing_path = g_config_missing_path.load(std::memory_order_relaxed);
  out->queries_no_match = g_queries_no_match.load(std::memory_order_relaxed);
  out->queries_mixed_case = g_queries_mixed_case.load(std::memory_order_relaxed);
  out->queries_not_alphabetic =
      g_queries_not_alphabetic.load(std::memory_order_relaxed);
  out->queries_too_long = g_queries_too_long.load(std::memory_order_relaxed);
  out->completions_lowercase =
      g_completions_lowercase.load(std::memory_order_relaxed);
  out->completions_initial_capital =
      g_completions_initial_capital.load(std::memory_order_relaxed);
  out->completions_uppercase =
      g_completions_uppercase.load(std::memory_order_relaxed);
  out->queries_illegal_word_form =
      g_queries_illegal_word_form.load(std::memory_order_relaxed);
  out->queries_protected = g_queries_protected.load(std::memory_order_relaxed);
  out->queries_non_ascii = g_queries_non_ascii.load(std::memory_order_relaxed);
  out->queries_unsupported_byte =
      g_queries_unsupported_byte.load(std::memory_order_relaxed);
  out->completions_with_delimiter =
      g_completions_with_delimiter.load(std::memory_order_relaxed);
  out->queries_covering_whole_input =
      g_queries_covering_whole_input.load(std::memory_order_relaxed);
  out->queries_covering_part_of_input =
      g_queries_covering_part_of_input.load(std::memory_order_relaxed);
  // P2.5: the space processor keeps its own storage (english_space_processor.cc);
  // this single C struct is still the only thing that crosses the boundary.
  const EnglishSpaceCounters space = SnapshotSpaceCounters();
  out->space_processor_constructed = space.constructed;
  out->space_processor_destroyed = space.destroyed;
  out->space_processor_live = space.live;
  out->processor_key_events = space.key_events;
  out->space_events_seen = space.space_seen;
  out->space_events_refused = space.space_refused;
  out->space_case_a_raw_token = space.case_a;
  out->space_case_b_selection = space.case_b;
  out->space_case_c_empty = space.case_c;
  out->composition_commits_issued = space.composition_commits;
  out->space_characters_emitted = space.space_characters;
  out->spelling_suggested = g_spelling_suggested.load(std::memory_order_relaxed);
  out->spelling_no_candidate = g_spelling_no_candidate.load(std::memory_order_relaxed);
  out->spelling_candidates_appended =
      g_spelling_candidates_appended.load(std::memory_order_relaxed);
  out->spelling_variants_generated =
      g_spelling_variants_generated.load(std::memory_order_relaxed);
  out->spelling_lexicon_lookups =
      g_spelling_lexicon_lookups.load(std::memory_order_relaxed);
  out->spelling_skipped_word_in_lexicon =
      g_spelling_skipped_word_in_lexicon.load(std::memory_order_relaxed);
  out->spelling_skipped_completion =
      g_spelling_skipped_completion.load(std::memory_order_relaxed);
  out->spelling_skipped_protected =
      g_spelling_skipped_protected.load(std::memory_order_relaxed);
  out->spelling_skipped_delimited =
      g_spelling_skipped_delimited.load(std::memory_order_relaxed);
  out->spelling_skipped_mixed_case =
      g_spelling_skipped_mixed_case.load(std::memory_order_relaxed);
  out->spelling_skipped_too_short =
      g_spelling_skipped_too_short.load(std::memory_order_relaxed);
  out->spelling_skipped_too_long =
      g_spelling_skipped_too_long.load(std::memory_order_relaxed);
  out->spelling_skipped_unsupported_byte =
      g_spelling_skipped_unsupported_byte.load(std::memory_order_relaxed);
  out->spelling_skipped_non_ascii =
      g_spelling_skipped_non_ascii.load(std::memory_order_relaxed);
  out->spelling_skipped_empty =
      g_spelling_skipped_empty.load(std::memory_order_relaxed);
  out->spelling_lexicon_unavailable =
      g_spelling_lexicon_unavailable.load(std::memory_order_relaxed);
  out->spelling_suggestions_lowercase =
      g_spelling_lowercase.load(std::memory_order_relaxed);
  out->spelling_suggestions_initial_capital =
      g_spelling_initial_capital.load(std::memory_order_relaxed);
  out->spelling_suggestions_uppercase =
      g_spelling_uppercase.load(std::memory_order_relaxed);
  return True;
}

const char* LastLoadStatusName() {
  const char* name = g_last_status_name.load(std::memory_order_relaxed);
  return name ? name : "never-attempted";
}

const char* EnglishBuildId() { return "epocanva-english/0.0.0-p2.6"; }

}  // namespace english
}  // namespace epocanva
