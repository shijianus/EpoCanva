// EpoCanva P2.6 -- one-edit spelling suggestions. See spelling_suggester.h for the
// gates, the four operations, the ordering and the lookup discipline.
//
// This file stores nothing between calls, allocates only locals, opens no file,
// performs no network I/O and emits no log statement (AGENTS.md section 2).
#include "spelling_suggester.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "prefix_completer.h"

namespace epocanva {
namespace en {
namespace {

// One generated variant: the candidate string plus the operation that produced it.
struct Variant {
  std::string word;
  SpellingEdit edit;
};

// Deletions: one fewer code unit, every position. A run of identical letters yields
// the same string several times -- that is allowed here on purpose and removed after
// the lookup, so the duplicate is a counted fact rather than an invisible trick.
void AddDeletions(const std::string& key, std::vector<Variant>* out) {
  for (std::size_t i = 0; i < key.size(); ++i) {
    std::string variant = key;
    variant.erase(variant.begin() + static_cast<std::ptrdiff_t>(i),
                  variant.begin() + static_cast<std::ptrdiff_t>(i) + 1);
    // An empty result cannot be a word, and the length gate below would drop it
    // anyway; skipping keeps variants_generated meaning "candidate strings".
    if (!variant.empty()) {
      out->push_back({std::move(variant), SpellingEdit::kDeletion});
    }
  }
}

void AddInsertions(const std::string& key, std::vector<Variant>* out) {
  for (std::size_t i = 0; i <= key.size(); ++i) {
    for (char c = 'a'; c <= 'z'; ++c) {
      std::string variant = key;
      variant.insert(variant.begin() + static_cast<std::ptrdiff_t>(i), c);
      out->push_back({std::move(variant), SpellingEdit::kInsertion});
    }
  }
}

void AddSubstitutions(const std::string& key, std::vector<Variant>* out) {
  for (std::size_t i = 0; i < key.size(); ++i) {
    for (char c = 'a'; c <= 'z'; ++c) {
      if (c == key[i]) {
        // Substituting a letter with itself returns the input: excluded at
        // generation, exactly as the brief requires.
        continue;
      }
      std::string variant = key;
      variant[i] = c;
      out->push_back({std::move(variant), SpellingEdit::kSubstitution});
    }
  }
}

void AddTranspositions(const std::string& key, std::vector<Variant>* out) {
  for (std::size_t i = 0; i + 1 < key.size(); ++i) {
    if (key[i] == key[i + 1]) {
      // Swapping two equal adjacent letters yields the input. Not a correction.
      continue;
    }
    std::string variant = key;
    std::swap(variant[i], variant[i + 1]);
    out->push_back({std::move(variant), SpellingEdit::kTransposition});
  }
}

// Gate 6's mapping from the token layer's decision to this layer's named outcome.
// Kept separate from the run so the table is readable and testable in one place.
SpellingOutcome OutcomeForDecision(QueryDecision decision) {
  switch (decision) {
    case QueryDecision::kProtected:
      return SpellingOutcome::kProtectedInput;
    case QueryDecision::kNotAscii:
      return SpellingOutcome::kNonAsciiInput;
    case QueryDecision::kUnsupportedByte:
      return SpellingOutcome::kUnsupportedByte;
    case QueryDecision::kIllegalForm:
      return SpellingOutcome::kDelimitedForm;
    case QueryDecision::kEmpty:
      return SpellingOutcome::kEmptyInput;
    case QueryDecision::kTooLong:
      return SpellingOutcome::kTooLong;
    case QueryDecision::kQuery:
      break;
  }
  return SpellingOutcome::kNoCandidate;  // unreachable; see the caller
}

}  // namespace

SpellingResult SuggestSpelling(const EnglishLexicon* lexicon,
                               std::string_view raw_input,
                               std::size_t completion_candidates,
                               std::size_t k,
                               bool enabled) {
  SpellingResult result;
  result.length_bytes = raw_input.size();
  // Gate 1 and 2 first: an off switch and a missing index are properties of the
  // configuration, not of the input, and must be reportable even for an input that
  // would otherwise be refused (so "the feature silently does nothing" and "the
  // word list failed to load" cannot be confused with "no corrections exist").
  if (!enabled) {
    result.outcome = SpellingOutcome::kDisabled;
    return result;
  }
  if (lexicon == nullptr) {
    result.outcome = SpellingOutcome::kLexiconUnavailable;
    return result;
  }
  if (raw_input.empty()) {
    result.outcome = SpellingOutcome::kEmptyInput;
    return result;
  }
  if (raw_input.size() > kMaxSuggestionWordBytes) {
    result.outcome = SpellingOutcome::kTooLong;
    return result;
  }
  if (raw_input.size() < kMinSuggestionWordBytes) {
    result.outcome = SpellingOutcome::kTooShort;
    return result;
  }

  // Gates 6 to 8 reuse the two existing analysers rather than restating their rules,
  // so "don't", "he2llo", "user@x" and "Co-Operate" are refused here for exactly the
  // reasons the completion layer refuses them -- one definition, no drift.
  const TokenAnalysis token = AnalyzeEnglishToken(raw_input);
  if (token.decision != QueryDecision::kQuery) {
    result.outcome = OutcomeForDecision(token.decision);
    return result;
  }
  if (token.delimiter != DelimiterForm::kNone) {
    // A queryable form this far, but this round corrects plain letter words only:
    // an apostrophe or a hyphen in the input means no suggestion at all.
    result.outcome = SpellingOutcome::kDelimitedForm;
    return result;
  }
  result.case_style = ClassifyAsciiCase(raw_input);
  if (!IsCompletableCase(result.case_style)) {
    result.outcome = SpellingOutcome::kMixedCase;
    return result;
  }

  std::string key;
  if (!ToLookupKey(raw_input, &key)) {
    // Unreachable after the gates above; refused loudly rather than queried with a
    // key the index could never hold.
    result.outcome = SpellingOutcome::kUnsupportedByte;
    return result;
  }

  // Gate 8: the word is already in the list. Note that this fires even when a
  // completion list exists, so "help" and "hell" are never offered corrections --
  // and it is checked BEFORE gate 9, because "already correct" is the more specific
  // and more user-visible fact.
  if (lexicon->Find(key) != nullptr) {
    result.outcome = SpellingOutcome::kWordInLexicon;
    return result;
  }
  // Gate 9. The completer already dropped any row whose display text would repeat
  // the input, so completion_candidates counts real completions only -- the
  // always-present original candidate can never make this gate misfire.
  if (completion_candidates > 0) {
    result.outcome = SpellingOutcome::kCompletionAvailable;
    return result;
  }

  std::vector<Variant> variants;
  AddDeletions(key, &variants);
  AddInsertions(key, &variants);
  AddSubstitutions(key, &variants);
  AddTranspositions(key, &variants);
  result.variants_generated = variants.size();

  // Look each variant up in the index the caller already loaded. Find() is the only
  // search primitive used here, so the work per keystroke is
  // variants * O(log entries) with no scan and no second structure.
  std::vector<std::pair<std::size_t, SpellingEdit>> hits;
  hits.reserve(variants.size());
  for (const Variant& variant : variants) {
    ++result.lexicon_lookups;
    const LexiconEntry* entry = lexicon->Find(variant.word);
    if (entry == nullptr) {
      continue;
    }
    // Pointer arithmetic into entries() is stable: the vector is immutable after
    // construction and this is the identity used to drop duplicate hits below.
    const std::size_t index = static_cast<std::size_t>(
        entry - lexicon->entries().data());
    hits.emplace_back(index, variant.edit);
  }
  result.words_in_lexicon = hits.size();

  // Group by entry, keeping the first hit in generation order for each word: that
  // is the operation reported for the candidate. Sorting by index (i.e. by
  // canonical byte order) makes the grouping exact without hashing.
  std::stable_sort(hits.begin(), hits.end(),
                   [](const std::pair<std::size_t, SpellingEdit>& a,
                      const std::pair<std::size_t, SpellingEdit>& b) {
                     return a.first < b.first;
                   });
  std::vector<const LexiconEntry*> unique;
  unique.reserve(hits.size());
  std::vector<SpellingEdit> unique_edit;
  unique_edit.reserve(hits.size());
  for (std::size_t i = 0; i < hits.size(); ++i) {
    if (i > 0 && hits[i].first == hits[i - 1].first) {
      ++result.duplicates_dropped;
      continue;
    }
    unique.push_back(&lexicon->entries()[hits[i].first]);
    unique_edit.push_back(hits[i].second);
  }

  // Rank with the ordering every completion list in this project already uses.
  std::vector<std::size_t> order(unique.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  const std::size_t limit =
      std::min(k, std::min(kMaxSuggestions, unique.size()));
  std::partial_sort(
      order.begin(), order.begin() + static_cast<std::ptrdiff_t>(limit),
      order.end(), [&unique](std::size_t a, std::size_t b) {
        return CompletionBefore(*unique[a], *unique[b]);
      });

  if (unique.empty()) {
    // The run happened and found nothing. Distinct from every gate above, and
    // distinct from kLexiconUnavailable, which is the one answer this layer must
    // never disguise as this one.
    result.search_ran = true;
    result.outcome = SpellingOutcome::kNoCandidate;
    return result;
  }
  result.search_ran = true;
  result.outcome = SpellingOutcome::kSuggested;
  result.suggestions.reserve(limit);
  for (std::size_t i = 0; i < limit; ++i) {
    const LexiconEntry& entry = *unique[order[i]];
    SpellingSuggestion suggestion;
    suggestion.canonical = entry.word;
    // The raw input was never touched; only the dictionary's own spelling is
    // rendered, through the same mapping completions use.
    suggestion.display = ApplyCaseStyle(entry.word, result.case_style);
    suggestion.weight = entry.weight;
    suggestion.edit = unique_edit[order[i]];
    result.suggestions.push_back(std::move(suggestion));
  }
  return result;
}

const char* SpellingEditName(SpellingEdit edit) {
  switch (edit) {
    case SpellingEdit::kNone:
      return "none";
    case SpellingEdit::kDeletion:
      return "deletion";
    case SpellingEdit::kInsertion:
      return "insertion";
    case SpellingEdit::kSubstitution:
      return "substitution";
    case SpellingEdit::kTransposition:
      return "transposition";
  }
  return "out-of-range";
}

const char* SpellingOutcomeName(SpellingOutcome outcome) {
  switch (outcome) {
    case SpellingOutcome::kSuggested:
      return "suggested";
    case SpellingOutcome::kNoCandidate:
      return "no-candidate";
    case SpellingOutcome::kDisabled:
      return "disabled";
    case SpellingOutcome::kLexiconUnavailable:
      return "lexicon-unavailable";
    case SpellingOutcome::kEmptyInput:
      return "empty-input";
    case SpellingOutcome::kTooShort:
      return "too-short";
    case SpellingOutcome::kTooLong:
      return "too-long";
    case SpellingOutcome::kProtectedInput:
      return "protected-input";
    case SpellingOutcome::kNonAsciiInput:
      return "non-ascii-input";
    case SpellingOutcome::kUnsupportedByte:
      return "unsupported-byte";
    case SpellingOutcome::kDelimitedForm:
      return "delimited-form";
    case SpellingOutcome::kMixedCase:
      return "mixed-case";
    case SpellingOutcome::kWordInLexicon:
      return "word-in-lexicon";
    case SpellingOutcome::kCompletionAvailable:
      return "completion-available";
  }
  return "out-of-range";
}

}  // namespace en
}  // namespace epocanva
