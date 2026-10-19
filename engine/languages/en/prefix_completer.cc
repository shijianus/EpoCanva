// EpoCanva P2.3 -- prefix completion implementation: structural gate, then
// casing gate, then the index.
#include "prefix_completer.h"

#include <algorithm>
#include <utility>

namespace epocanva {
namespace en {
namespace {

// The prefix successor: the smallest byte string greater than every string that
// starts with `key`.
//
//   succ(key) = key[0 .. n-2] + (key[n-1] + 1)
//
// Proof, written out because P2.1's bound had been correct only by accident:
//   (a) any entry E with prefix `key` is >= key; at position n-1 it holds
//       key[n-1], strictly less than key[n-1]+1, so E < succ(key) whether E is
//       exactly key or key followed by more bytes.
//   (b) any E in [key, succ(key)) has to agree with key on the first n-1 bytes --
//       a smaller byte there puts E below key, a larger one puts it above
//       succ(key) -- and at position n-1 it must be >= key[n-1] and <
//       key[n-1]+1, i.e. equal. So `key` is a prefix of E.
// Neither half mentions 'z', '{' or the entry alphabet, which is why this replaced
// `key + '{'` the moment entries could contain U+0027 / U+002D: the old bound only
// held while every byte AFTER the prefix sat below '{', a property of one word
// list rather than of the algorithm.
//
// Precondition, guaranteed by AnalyzeEnglishToken + ToLookupKey and therefore not
// branched on: `key` is non-empty and its last byte is one of a-z, ' or -, all
// below 0xFF.
std::string PrefixSuccessor(const std::string& key) {
  std::string bound = key;
  bound.back() =
      static_cast<char>(static_cast<unsigned char>(bound.back()) + 1u);
  return bound;
}

// First index whose word is not less than `key`. std::string orders by unsigned
// byte comparison (char_traits<char>::compare is memcmp-ordered), which is the
// same order the loader sorted the entries with: no locale, no collation, and no
// sign trap for bytes above 0x7F -- those never reach here anyway.
std::size_t LowerBoundByWord(const std::vector<LexiconEntry>& entries,
                             const std::string& key) {
  std::size_t low = 0;
  std::size_t high = entries.size();
  while (low < high) {
    const std::size_t middle = low + (high - low) / 2;
    if (entries[middle].word < key) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return low;
}

CompletionOutcome OutcomeOfDecision(QueryDecision decision) {
  switch (decision) {
    case QueryDecision::kQuery:
      return CompletionOutcome::kCompleted;  // replaced by the index result below
    case QueryDecision::kEmpty:
      return CompletionOutcome::kEmptyInput;
    case QueryDecision::kTooLong:
      return CompletionOutcome::kInputTooLong;
    case QueryDecision::kNotAscii:
      return CompletionOutcome::kNonAscii;
    case QueryDecision::kProtected:
      return CompletionOutcome::kProtectedText;
    case QueryDecision::kUnsupportedByte:
      return CompletionOutcome::kUnsupportedByte;
    case QueryDecision::kIllegalForm:
      return CompletionOutcome::kIllegalWordForm;
  }
  return CompletionOutcome::kIllegalWordForm;
}

}  // namespace

const char* CompletionOutcomeName(CompletionOutcome outcome) {
  switch (outcome) {
    case CompletionOutcome::kCompleted:
      return "completed";
    case CompletionOutcome::kNoMatch:
      return "no-match";
    case CompletionOutcome::kEmptyInput:
      return "empty-input";
    case CompletionOutcome::kInputTooLong:
      return "input-too-long";
    case CompletionOutcome::kNonAscii:
      return "non-ascii";
    case CompletionOutcome::kProtectedText:
      return "protected-text";
    case CompletionOutcome::kUnsupportedByte:
      return "unsupported-byte";
    case CompletionOutcome::kIllegalWordForm:
      return "illegal-word-form";
    case CompletionOutcome::kMixedCase:
      return "mixed-case";
  }
  return "unknown-outcome";
}

bool CompletionBefore(const LexiconEntry& a, const LexiconEntry& b) {
  if (a.weight != b.weight) {
    return a.weight > b.weight;  // heavier first
  }
  return a.word < b.word;  // ties broken by ascending bytes, never by locale
}

std::size_t PrefixRangeStart(const std::vector<LexiconEntry>& entries,
                             const std::string& key) {
  return LowerBoundByWord(entries, key);
}

std::size_t PrefixRangeEnd(const std::vector<LexiconEntry>& entries,
                           const std::string& key) {
  return LowerBoundByWord(entries, PrefixSuccessor(key));
}

CompletionResult CompletePrefix(const EnglishLexicon& lexicon,
                                std::string_view raw_input,
                                std::size_t k) {
  CompletionResult result;
  // `raw_input` is a view into the caller's buffer: it is read here and never
  // written, so "the original input is not modified" is structural rather than a
  // convention. An over-long input is refused at its full length, which is also
  // why nothing in this function can drop trailing bytes.
  result.token = AnalyzeEnglishToken(raw_input);
  // Casing is recorded for every input, including the ones refused on structural
  // grounds, so a reader of the diagnostics sees both axes of the same query.
  result.case_style = ClassifyAsciiCase(raw_input);

  if (result.token.decision != QueryDecision::kQuery) {
    // Gate 1: kEmpty / kTooLong / kNotAscii / kProtected / kUnsupportedByte /
    // kIllegalForm. No lookup, no guessing and no cleaning: "he2llo" is not turned
    // into "hello", "user@example.com" is not cut at the '@', and "'hello" is not
    // repaired into "hello".
    result.outcome = OutcomeOfDecision(result.token.decision);
    return result;
  }

  // Gate 2, casing. Now that every byte is an ASCII letter or the single allowed
  // delimiter and at least one byte is a letter, ClassifyAsciiCase can only return
  // a completable style or kMixed; kMixed keeps the original text and nothing else.
  if (!IsCompletableCase(result.case_style)) {
    result.outcome = CompletionOutcome::kMixedCase;
    return result;
  }

  // The LOOKUP KEY: a lowercase copy with the delimiter kept in the same code-unit
  // position. The index itself is never duplicated per case, and the lexicon's rows
  // are untouched.
  std::string lookup_key;
  if (!ToLookupKey(raw_input, &lookup_key)) {
    // Unreachable for an input that passed both gates; reported as a refusal
    // rather than queried, so a future edit cannot make the index see a key with
    // an unaccepted byte and silently match nothing.
    result.outcome = CompletionOutcome::kIllegalWordForm;
    return result;
  }

  const std::vector<LexiconEntry>& entries = lexicon.entries();
  const std::size_t first = PrefixRangeStart(entries, lookup_key);
  const std::size_t last = PrefixRangeEnd(entries, lookup_key);
  result.lookup_performed = true;

  const std::size_t limit = std::min(k, kMaxCompletions);
  // Candidates are (weight, canonical) pairs taken from the range, with the row
  // whose DISPLAY text would repeat the raw input removed.
  //
  // Dedup happens BEFORE the k cut, not after it. Cutting first and filtering
  // afterwards would let a duplicate silently eat one slot and hand back fewer
  // completions than the caller paid for.
  std::vector<const LexiconEntry*> pool;
  pool.reserve(last - first);
  for (std::size_t i = first; i < last; ++i) {
    const std::string display =
        ApplyCaseStyle(entries[i].word, result.case_style);
    if (display == raw_input) {
      ++result.deduplicated;
      continue;
    }
    pool.push_back(&entries[i]);
  }
  result.match_count = pool.size();

  const std::size_t wanted = std::min(limit, pool.size());
  if (wanted == 0) {
    // Either the range was empty, or the caller asked for k == 0. Only the first
    // of those is "no match": an index that does hold the word must not be
    // reported as a word-list gap just because the caller wanted no output.
    result.outcome = (result.match_count == 0) ? CompletionOutcome::kNoMatch
                                               : CompletionOutcome::kCompleted;
    return result;
  }
  // Words are unique and the comparator is a total order on (weight, word), so
  // partial_sort cannot be perturbed by equal-element instability. Re-running a
  // query on an unchanged index therefore returns a byte-identical list, and
  // because ranking uses canonical text only, the same query in a different case
  // produces the same canonical order.
  std::partial_sort(
      pool.begin(), pool.begin() + static_cast<std::ptrdiff_t>(wanted),
      pool.end(),
      [](const LexiconEntry* a, const LexiconEntry* b) {
        return CompletionBefore(*a, *b);
      });
  result.completions.reserve(wanted);
  for (std::size_t i = 0; i < wanted; ++i) {
    Completion c;
    c.canonical = pool[i]->word;
    c.display = ApplyCaseStyle(pool[i]->word, result.case_style);
    c.weight = pool[i]->weight;
    result.completions.push_back(std::move(c));
  }
  result.outcome = CompletionOutcome::kCompleted;
  return result;
}

}  // namespace en
}  // namespace epocanva
