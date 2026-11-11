#include "language_support.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "morphology_rules.h"

namespace epocanva {
namespace en {
namespace {

constexpr std::size_t kNoRow = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kMaxPredictionRows = 65536;
constexpr double kLearnedScale = 0.75;
constexpr double kContextScale = 1.25;
constexpr double kBoundaryPenalty = 0.75;

bool PlainLower(std::string_view word) {
  return !word.empty() && std::all_of(word.begin(), word.end(), [](char c) {
    return c >= 'a' && c <= 'z';
  });
}
bool StartsWith(std::string_view word, std::string_view prefix) {
  return word.size() >= prefix.size() && word.substr(0, prefix.size()) == prefix;
}
bool CandidateBefore(const Candidate& a, const Candidate& b) {
  if (a.score != b.score) {
    return a.score > b.score;
  }
  if (a.text != b.text) {
    return a.text < b.text;
  }
  if (a.type != b.type) {
    return a.type < b.type;
  }
  return a.lexeme_ids < b.lexeme_ids;
}

// Retains only the best candidate for each display text, then only the best k.
// Memory per query is bounded even when the dictionary match range is large.
class CandidateCollector {
 public:
  explicit CandidateCollector(std::size_t limit) : limit_(limit) {}
  void Add(Candidate candidate) {
    auto same = std::find_if(values_.begin(), values_.end(),
                            [&](const Candidate& prior) {
                              return prior.text == candidate.text;
                            });
    if (same != values_.end()) {
      if (same->type == candidate.type && same->lexeme_ids == candidate.lexeme_ids) {
        candidate.morphology_forms |= same->morphology_forms;
      }
      if (!CandidateBefore(candidate, *same)) {
        same->morphology_forms |= candidate.morphology_forms;
        return;
      }
      *same = std::move(candidate);
    } else {
      values_.push_back(std::move(candidate));
    }
    std::sort(values_.begin(), values_.end(), CandidateBefore);
    if (values_.size() > limit_) {
      values_.resize(limit_);
    }
  }
  std::vector<Candidate> Take() { return std::move(values_); }

 private:
  std::size_t limit_;  // candidate count
  std::vector<Candidate> values_;
};

struct TrieNode {
  std::array<std::size_t, 26> children{};  // alphabet slots, not text offsets
  std::size_t row = kNoRow;  // dictionary row ordinal
  TrieNode() { children.fill(kNoRow); }
};
struct Word {
  std::string text;
  LexemeId id = kNoLexemeId;
  std::uint64_t base_count = 0;
  bool has_frequency = false;
};
struct IndexedForm {
  std::string text;
  std::size_t lemma_row = 0;  // dictionary row ordinal
  std::uint32_t forms = 0;
};
struct RankingSnapshot {
  bool valid = true;
  LexemeId previous_id = kNoLexemeId;
  std::unordered_map<LexemeId, std::uint64_t> learned;
  std::map<std::pair<LexemeId, LexemeId>, std::uint64_t> context;
};

// Project-authored explicit shorthand choices (en-expansion-v1). They are exact
// key mappings, NOT a natural-language abbreviation detector, and are opt-in
// suggestions even when a key is also an ordinary word (cant/ill/its).
struct ExpansionRule {
  std::string_view key;
  std::string_view text;
  CandidateType type;
};
constexpr ExpansionRule kExpansions[] = {
    {"afaik", "as far as i know", CandidateType::kAbbreviation},
    {"asap", "as soon as possible", CandidateType::kAbbreviation},
    {"brb", "be right back", CandidateType::kAbbreviation},
    {"btw", "by the way", CandidateType::kAbbreviation},
    {"fyi", "for your information", CandidateType::kAbbreviation},
    {"idk", "i do not know", CandidateType::kAbbreviation},
    {"imo", "in my opinion", CandidateType::kAbbreviation},
    {"imho", "in my humble opinion", CandidateType::kAbbreviation},
    {"lmk", "let me know", CandidateType::kAbbreviation},
    {"np", "no problem", CandidateType::kAbbreviation},
    {"omw", "on my way", CandidateType::kAbbreviation},
    {"pls", "please", CandidateType::kAbbreviation},
    {"thx", "thanks", CandidateType::kAbbreviation},
    {"ty", "thank you", CandidateType::kAbbreviation},
    {"tbh", "to be honest", CandidateType::kAbbreviation},
    {"wip", "work in progress", CandidateType::kAbbreviation},
    {"dont", "don't", CandidateType::kContraction},
    {"doesnt", "doesn't", CandidateType::kContraction},
    {"didnt", "didn't", CandidateType::kContraction},
    {"cant", "can't", CandidateType::kContraction},
    {"couldnt", "couldn't", CandidateType::kContraction},
    {"wouldnt", "wouldn't", CandidateType::kContraction},
    {"shouldnt", "shouldn't", CandidateType::kContraction},
    {"wont", "won't", CandidateType::kContraction},
    {"isnt", "isn't", CandidateType::kContraction},
    {"arent", "aren't", CandidateType::kContraction},
    {"wasnt", "wasn't", CandidateType::kContraction},
    {"werent", "weren't", CandidateType::kContraction},
    {"hasnt", "hasn't", CandidateType::kContraction},
    {"havent", "haven't", CandidateType::kContraction},
    {"hadnt", "hadn't", CandidateType::kContraction},
    {"im", "i'm", CandidateType::kContraction},
    {"ive", "i've", CandidateType::kContraction},
    {"ill", "i'll", CandidateType::kContraction},
    {"youre", "you're", CandidateType::kContraction},
    {"theyre", "they're", CandidateType::kContraction},
};

LanguageSupportOutcome GateOutcome(QueryDecision decision) {
  switch (decision) {
    case QueryDecision::kQuery: return LanguageSupportOutcome::kQueried;
    case QueryDecision::kEmpty: return LanguageSupportOutcome::kEmptyInput;
    case QueryDecision::kTooLong: return LanguageSupportOutcome::kInputTooLong;
    case QueryDecision::kNotAscii: return LanguageSupportOutcome::kNonAscii;
    case QueryDecision::kProtected: return LanguageSupportOutcome::kProtectedText;
    case QueryDecision::kUnsupportedByte:
      return LanguageSupportOutcome::kUnsupportedByte;
    case QueryDecision::kIllegalForm:
      return LanguageSupportOutcome::kIllegalWordForm;
  }
  return LanguageSupportOutcome::kUnsupportedByte;
}

struct EditCost {
  unsigned edits = 0;
  unsigned cost = 0;
};
bool CostBefore(EditCost a, EditCost b) {
  return std::tie(a.edits, a.cost) < std::tie(b.edits, b.cost);
}
EditCost Minimum(EditCost a, EditCost b) { return CostBefore(a, b) ? a : b; }
EditCost Plus(EditCost a, unsigned edits, unsigned cost) {
  return {a.edits + edits, a.cost + cost};
}

}  // namespace

struct LanguageSupport::Impl {
  std::vector<Word> words;  // canonical byte order, independent of legacy weight
  std::unordered_map<LexemeId, std::size_t> by_id;
  std::vector<IndexedForm> forms;  // form byte order, then lemma row ordinal
  std::vector<std::vector<std::size_t>> by_lemma;  // form row ordinals
  std::vector<TrieNode> trie{1};  // plain ASCII letters only
  FrequencySource source = FrequencySource::kUnavailable;
  std::uint64_t total_count = 0;
  std::size_t covered_rows = 0;
  std::vector<std::size_t> frequency_rows;  // covered dictionary row ordinals

  std::size_t Find(std::string_view canonical) const {
    const auto found = std::lower_bound(words.begin(), words.end(), canonical,
        [](const Word& word, std::string_view key) { return word.text < key; });
    return found != words.end() && found->text == canonical
               ? static_cast<std::size_t>(found - words.begin()) : kNoRow;
  }
  RankingSnapshot Snapshot(const RankingInputs& input) const {
    RankingSnapshot result;
    if (!input.allow_learning) {
      return result;  // no dereference of either supplied statistics vector
    }
    const std::size_t learned_count = input.learned ? input.learned->size() : 0;
    const std::size_t context_count = input.context ? input.context->size() : 0;
    if (learned_count > kMaxRankingRecords ||
        context_count > kMaxRankingRecords - learned_count) {
      result.valid = false;
      return result;
    }
    std::set<LexemeId> seen_learned;
    std::set<std::pair<LexemeId, LexemeId>> seen_context;
    if (input.learned) {
      for (const auto& row : *input.learned) {
        if (!seen_learned.insert(row.id).second) {
          result.valid = false;
          break;
        }
        if (by_id.count(row.id) != 0) {
          result.learned.emplace(row.id, row.count);
        }
      }
    }
    if (result.valid && input.context) {
      for (const auto& row : *input.context) {
        const auto key = std::make_pair(row.previous_id, row.id);
        if (!seen_context.insert(key).second) {
          result.valid = false;
          break;
        }
        if (by_id.count(row.previous_id) != 0 && by_id.count(row.id) != 0) {
          result.context.emplace(key, row.count);
        }
      }
    }
    if (!result.valid) {
      result.learned.clear();
      result.context.clear();
    } else if (by_id.count(input.previous_id) != 0) {
      result.previous_id = input.previous_id;
    }
    return result;
  }
  ScoreBreakdown Score(LexemeId id, LexemeId previous,
                       const RankingSnapshot& snapshot) const {
    ScoreBreakdown score;
    score.ranking_inputs_valid = snapshot.valid;
    const auto found = by_id.find(id);
    if (found == by_id.end()) {
      return score;
    }
    const auto& word = words[found->second];
    score.known_lexeme = true;
    score.has_base_frequency = word.has_frequency;
    score.base_count = word.base_count;
    const auto learned = snapshot.learned.find(id);
    if (learned != snapshot.learned.end()) {
      score.learned_count = learned->second;
    }
    const auto context = snapshot.context.find({previous, id});
    if (context != snapshot.context.end()) {
      score.context_count = context->second;
    }
    score.base = std::log1p(static_cast<double>(score.base_count));
    score.learned = kLearnedScale * std::log1p(static_cast<double>(score.learned_count));
    score.context = kContextScale * std::log1p(static_cast<double>(score.context_count));
    score.total = score.base + score.learned + score.context;
    return score;
  }

  void Morphology(std::string_view key, CaseStyle style,
                  const RankingSnapshot& snapshot,
                  const LanguageSupportOptions& options,
                  LanguageSupportResult* result, CandidateCollector* output) const {
    result->morphology = LanguageFeatureStatus::kNoMatch;
    const auto budget = std::min(options.morphology_match_budget, kMaxMorphologyMatches);
    std::set<std::size_t> visited;  // generated-form row ordinals
    bool found_candidate = false;
    const auto examine = [&](std::size_t form_row) {
      if (visited.count(form_row) != 0) {
        return true;
      }
      if (result->morphology_matches_examined >= budget) {
        result->morphology = LanguageFeatureStatus::kBudgetExhausted;
        return false;
      }
      visited.insert(form_row);
      ++result->morphology_matches_examined;
      const auto& form = forms[form_row];
      if (form.text == key) {
        return true;
      }
      Candidate candidate;
      candidate.type = CandidateType::kMorphology;
      candidate.text = ApplyCaseStyle(form.text, style);
      // Context/learning identify the committed surface, not its source lemma.
      // The standalone legacy core also offers unattested generated forms, so
      // those retain an admitted lemma fallback instead of minting an OOV ID.
      const auto surface_row = Find(form.text);
      const auto score_row = surface_row == kNoRow ? form.lemma_row : surface_row;
      candidate.lexeme_ids.push_back(words[score_row].id);
      candidate.morphology_forms = form.forms;
      candidate.match_log_score = -0.25;
      // A complete typed lemma is stronger morphological evidence than merely
      // sharing a prefix. With no frequency data, walk's/walked/walking/walks
      // must not be displaced by walkaway/walker forms in a small menu. This
      // remains a ranking feature; independent frequency/context can outweigh it.
      if (words[form.lemma_row].text == key) {
        candidate.match_log_score += 1.0;
      }
      candidate.score = Score(candidate.lexeme_ids[0], snapshot.previous_id,
                              snapshot).total + candidate.match_log_score;
      candidate.segments_utf8_byte.push_back({0, key.size()});
      output->Add(std::move(candidate));
      found_candidate = true;
      return true;
    };
    const auto lemma = Find(key);
    if (lemma != kNoRow) {
      for (const auto form_row : by_lemma[lemma]) {
        if (!examine(form_row)) {
          return;
        }
      }
    }
    auto form = std::lower_bound(forms.begin(), forms.end(), key,
        [](const IndexedForm& value, std::string_view prefix) {
          return value.text < prefix;
        });
    for (; form != forms.end() && StartsWith(form->text, key); ++form) {
      if (!examine(static_cast<std::size_t>(form - forms.begin()))) {
        return;
      }
    }
    result->morphology = found_candidate ? LanguageFeatureStatus::kCandidates
                                        : LanguageFeatureStatus::kNoMatch;
  }

  void Expand(std::string_view key, CaseStyle style,
              const RankingSnapshot& snapshot, LanguageSupportResult* result,
              CandidateCollector* output) const {
    result->abbreviation = LanguageFeatureStatus::kNoMatch;
    for (const auto& rule : kExpansions) {
      if (rule.key != key) {
        continue;
      }
      Candidate candidate;
      candidate.type = rule.type;
      candidate.text = ApplyCaseStyle(rule.text, style);
      candidate.segments_utf8_byte.push_back({0, key.size()});
      LexemeId previous = snapshot.previous_id;
      std::string_view remaining = rule.text;
      while (!remaining.empty()) {
        const auto space_utf8_byte = remaining.find(' ');
        const auto row = Find(remaining.substr(0, space_utf8_byte));
        const auto id = row == kNoRow ? kNoLexemeId : words[row].id;
        candidate.lexeme_ids.push_back(id);
        candidate.score += Score(id, previous, snapshot).total;
        previous = id;
        if (space_utf8_byte == std::string_view::npos) {
          break;
        }
        remaining.remove_prefix(space_utf8_byte + 1);
      }
      candidate.score /= static_cast<double>(candidate.lexeme_ids.size());
      output->Add(std::move(candidate));
      result->abbreviation = LanguageFeatureStatus::kCandidates;
    }
  }

  struct SegmentPath {
    double score = 0;
    std::string text;
    std::vector<SegmentUtf8Byte> segments;
    std::vector<LexemeId> ids;
  };
  static bool PathBefore(const SegmentPath& a, const SegmentPath& b) {
    return a.score != b.score ? a.score > b.score : a.text < b.text;
  }
  void Segment(std::string_view key, CaseStyle style,
               const RankingSnapshot& snapshot,
               const LanguageSupportOptions& options,
               LanguageSupportResult* result, CandidateCollector* output) const {
    if (source == FrequencySource::kUnavailable) {
      result->segmentation = LanguageFeatureStatus::kFrequencyUnavailable;
      return;
    }
    result->segmentation = LanguageFeatureStatus::kNoMatch;
    const auto word_limit = std::min(options.max_segmentation_words, kMaxSegmentationWords);
    if (word_limit < 2) {
      result->segmentation = LanguageFeatureStatus::kNotApplicable;
      return;
    }
    const auto budget = std::min(options.segmentation_transition_budget,
                                 kMaxSegmentationTransitions);
    const auto keep = std::min(options.max_candidates, kMaxSegmentationCandidates);
    // Boundary index is an original-input UTF-8 byte offset. State includes the
    // last ID AND word count, preserving exact top-k under the supplied bigrams
    // and maximum-word constraint. A one-best-per-byte algorithm is incorrect.
    using State = std::pair<LexemeId, std::size_t>;
    std::vector<std::map<State, std::vector<SegmentPath>>> dp(key.size() + 1);
    dp[0][{snapshot.previous_id, 0}].push_back({});
    const double normalizer = std::log(static_cast<double>(total_count) +
                                      static_cast<double>(covered_rows));
    bool exhausted = false;
    for (std::size_t begin_utf8_byte = 0;
         begin_utf8_byte < key.size() && !exhausted; ++begin_utf8_byte) {
      if (dp[begin_utf8_byte].empty()) {
        continue;
      }
      std::size_t node = 0;  // trie node ordinal
      for (std::size_t end_utf8_byte = begin_utf8_byte;
           end_utf8_byte < key.size() && !exhausted; ++end_utf8_byte) {
        node = trie[node].children[static_cast<std::size_t>(key[end_utf8_byte] - 'a')];
        if (node == kNoRow) {
          break;
        }
        const auto row = trie[node].row;
        if (row == kNoRow || !words[row].has_frequency) {
          continue;
        }
        const Word& word = words[row];
        if (word.text.size() == 1 && word.text != "a" && word.text != "i") {
          continue;
        }
        for (const auto& state : dp[begin_utf8_byte]) {
          if (state.first.second >= word_limit) {
            continue;
          }
          for (const auto& prior : state.second) {
            if (result->segmentation_transitions >= budget) {
              exhausted = true;
              break;
            }
            ++result->segmentation_transitions;
            SegmentPath path = prior;
            if (!path.text.empty()) {
              path.text.push_back(' ');
            }
            path.text += word.text;
            path.score += Score(word.id, state.first.first, snapshot).total - normalizer;
            if (!path.ids.empty()) {
              path.score -= kBoundaryPenalty;
            }
            path.segments.push_back({begin_utf8_byte, end_utf8_byte + 1});
            path.ids.push_back(word.id);
            auto& paths = dp[end_utf8_byte + 1][{word.id, state.first.second + 1}];
            paths.push_back(std::move(path));
            std::sort(paths.begin(), paths.end(), PathBefore);
            if (paths.size() > keep) {
              paths.resize(keep);
            }
          }
          if (exhausted) {
            break;
          }
        }
      }
    }
    CandidateCollector segmented(keep);
    bool found = false;
    for (const auto& state : dp[key.size()]) {
      if (state.first.second < 2) {
        continue;
      }
      for (const auto& path : state.second) {
        Candidate candidate;
        candidate.type = CandidateType::kSegmentation;
        candidate.text = ApplyCaseStyle(path.text, style);
        candidate.score = path.score;
        candidate.match_log_score =
            -kBoundaryPenalty * static_cast<double>(path.ids.size() - 1);
        candidate.segments_utf8_byte = path.segments;
        candidate.lexeme_ids = path.ids;
        segmented.Add(std::move(candidate));
        found = true;
      }
    }
    for (auto& candidate : segmented.Take()) {
      output->Add(std::move(candidate));
    }
    result->segmentation = exhausted ? LanguageFeatureStatus::kBudgetExhausted
        : found ? LanguageFeatureStatus::kCandidates : LanguageFeatureStatus::kNoMatch;
  }

  void Spell(std::string_view key, CaseStyle style,
             const RankingSnapshot& snapshot,
             const LanguageSupportOptions& options,
             LanguageSupportResult* result, CandidateCollector* output) const {
    const auto budget = std::min(options.spelling_cell_budget, kMaxSpellingCells);
    result->spelling = LanguageFeatureStatus::kNoMatch;
    using CostRow = std::vector<EditCost>;  // cells indexed by input UTF-8 bytes
    CostRow root(key.size() + 1);
    for (std::size_t input_utf8_byte = 0; input_utf8_byte <= key.size(); ++input_utf8_byte) {
      root[input_utf8_byte] = {static_cast<unsigned>(input_utf8_byte),
                              static_cast<unsigned>(input_utf8_byte) * 100u};
    }
    bool exhausted = false;
    bool found = false;
    // Trie DP shares all dictionary prefixes and prunes by the minimum distance
    // in a row. Work is measured in evaluated DP cells, not elapsed time. The
    // previous-previous row implements restricted Damerau (OSA), not unrestricted
    // Damerau-Levenshtein. Maximum recursion depth is key UTF-8 bytes + 2.
    std::function<void(std::size_t, char, char, std::size_t,
                       const CostRow&, const CostRow*)> visit;
    visit = [&](std::size_t node, char letter, char previous_letter,
                std::size_t depth_utf8_byte, const CostRow& previous,
                const CostRow* previous_previous) {
      if (exhausted) {
        return;
      }
      CostRow current(key.size() + 1);
      current[0] = {static_cast<unsigned>(depth_utf8_byte),
                    static_cast<unsigned>(depth_utf8_byte) * 100u};
      unsigned minimum = current[0].edits;
      for (std::size_t input_utf8_byte = 1; input_utf8_byte <= key.size(); ++input_utf8_byte) {
        if (result->spelling_cells >= budget) {
          exhausted = true;
          return;
        }
        ++result->spelling_cells;
        const bool same = letter == key[input_utf8_byte - 1];
        EditCost best = Plus(previous[input_utf8_byte - 1], same ? 0u : 1u,
                             keyboard::SubstitutionCost(options.keyboard_layout,
                                                        letter, key[input_utf8_byte - 1]));
        best = Minimum(best, Plus(previous[input_utf8_byte], 1, 100));
        best = Minimum(best, Plus(current[input_utf8_byte - 1], 1, 100));
        if (previous_previous && input_utf8_byte >= 2 &&
            letter == key[input_utf8_byte - 2] &&
            previous_letter == key[input_utf8_byte - 1] && letter != previous_letter) {
          best = Minimum(best, Plus((*previous_previous)[input_utf8_byte - 2], 1, 80));
        }
        current[input_utf8_byte] = best;
        minimum = std::min(minimum, best.edits);
      }
      if (trie[node].row != kNoRow && current.back().edits > 0 && current.back().edits <= 2) {
        const auto& word = words[trie[node].row];
        Candidate candidate;
        candidate.type = CandidateType::kSpelling;
        candidate.text = ApplyCaseStyle(word.text, style);
        candidate.lexeme_ids.push_back(word.id);
        candidate.edit_distance = current.back().edits;
        candidate.keyboard_cost = current.back().cost;
        candidate.match_log_score =
            -4.0 * candidate.edit_distance - 0.005 * candidate.keyboard_cost;
        candidate.score = Score(word.id, snapshot.previous_id, snapshot).total +
                          candidate.match_log_score;
        candidate.segments_utf8_byte.push_back({0, key.size()});
        output->Add(std::move(candidate));
        found = true;
      }
      if (minimum > 2 || depth_utf8_byte >= key.size() + 2) {
        return;
      }
      for (std::size_t alphabet_slot = 0; alphabet_slot < 26 && !exhausted; ++alphabet_slot) {
        const auto child = trie[node].children[alphabet_slot];
        if (child != kNoRow) {
          visit(child, static_cast<char>('a' + alphabet_slot), letter,
                depth_utf8_byte + 1, current, &previous);
        }
      }
    };
    for (std::size_t alphabet_slot = 0; alphabet_slot < 26 && !exhausted; ++alphabet_slot) {
      const auto child = trie[0].children[alphabet_slot];
      if (child != kNoRow) {
        visit(child, static_cast<char>('a' + alphabet_slot), 0, 1, root, nullptr);
      }
    }
    result->spelling = exhausted ? LanguageFeatureStatus::kBudgetExhausted
        : found ? LanguageFeatureStatus::kCandidates : LanguageFeatureStatus::kNoMatch;
  }
};

LexemeId StableLexemeId(std::string_view canonical) {
  if (!IsDictionaryEntryForm(canonical)) {
    return kNoLexemeId;
  }
  std::uint64_t hash = 14695981039346656037ull;
  constexpr std::string_view domain = "epocanva.en.lexeme/v1/";
  for (const char c : domain) {
    hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
  }
  for (const char c : canonical) {
    hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
  }
  return hash == 0 ? 1 : hash;
}

std::unique_ptr<LanguageSupport> LanguageSupport::Create(
    const EnglishLexicon& lexicon, const FrequencyData& frequencies,
    LanguageSupportBuildInfo* info) {
  LanguageSupportBuildInfo local;
  if (!info) {
    info = &local;
  }
  *info = {};
  auto impl = std::make_unique<Impl>();
  for (const auto& entry : lexicon.entries()) {
    const auto id = StableLexemeId(entry.word);
    if (id == 0 || !impl->by_id.emplace(id, impl->words.size()).second) {
      info->status = LanguageSupportBuildStatus::kLexemeIdCollision;
      return nullptr;
    }
    impl->words.push_back({entry.word, id, 0, false});  // NEVER entry.weight
  }
  info->lexeme_count = impl->words.size();
  if (frequencies.source != FrequencySource::kUnavailable &&
      frequencies.source != FrequencySource::kTestFixture &&
      frequencies.source != FrequencySource::kProductionCorpus) {
    info->status = LanguageSupportBuildStatus::kInvalidFrequencySource;
    return nullptr;
  }
  if (frequencies.source == FrequencySource::kUnavailable) {
    if (!frequencies.entries.empty() || !frequencies.provenance.empty()) {
      info->status = LanguageSupportBuildStatus::kInvalidFrequencySource;
      return nullptr;
    }
  } else if (frequencies.provenance.empty()) {
    info->status = LanguageSupportBuildStatus::kMissingFrequencyProvenance;
    return nullptr;
  } else if (frequencies.entries.empty()) {
    info->status = LanguageSupportBuildStatus::kEmptyFrequencyData;
    return nullptr;
  }
  for (const auto& row : frequencies.entries) {
    if (row.id == 0 || row.count == 0) {
      info->status = LanguageSupportBuildStatus::kInvalidFrequencyRecord;
      return nullptr;
    }
    const auto found = impl->by_id.find(row.id);
    if (found == impl->by_id.end()) {
      info->status = LanguageSupportBuildStatus::kUnknownFrequencyLexeme;
      return nullptr;
    }
    auto& word = impl->words[found->second];
    if (word.has_frequency) {
      info->status = LanguageSupportBuildStatus::kDuplicateFrequencyRecord;
      return nullptr;
    }
    if (row.count > std::numeric_limits<std::uint64_t>::max() - impl->total_count) {
      info->status = LanguageSupportBuildStatus::kFrequencyCountOverflow;
      return nullptr;
    }
    word.has_frequency = true;
    word.base_count = row.count;
    impl->total_count += row.count;
    ++impl->covered_rows;
  }
  impl->source = frequencies.source;
  for (std::size_t row = 0; row < impl->words.size(); ++row) {
    const auto& word = impl->words[row];
    if (word.has_frequency) {
      impl->frequency_rows.push_back(row);
    }
    if (PlainLower(word.text)) {
      std::size_t node = 0;
      for (const char c : word.text) {
        const auto slot = static_cast<std::size_t>(c - 'a');
        if (impl->trie[node].children[slot] == kNoRow) {
          impl->trie[node].children[slot] = impl->trie.size();
          impl->trie.emplace_back();
        }
        node = impl->trie[node].children[slot];
      }
      impl->trie[node].row = row;
    }
    for (auto& form : GenerateWordForms(word.text)) {
      impl->forms.push_back({std::move(form.canonical), row, form.forms});
    }
  }
  std::sort(impl->forms.begin(), impl->forms.end(),
      [](const IndexedForm& a, const IndexedForm& b) {
        return std::tie(a.text, a.lemma_row) < std::tie(b.text, b.lemma_row);
      });
  impl->by_lemma.resize(impl->words.size());
  for (std::size_t row = 0; row < impl->forms.size(); ++row) {
    impl->by_lemma[impl->forms[row].lemma_row].push_back(row);
  }
  info->frequency_count = impl->covered_rows;
  info->generated_form_count = impl->forms.size();
  return std::unique_ptr<LanguageSupport>(new LanguageSupport(std::move(impl)));
}

LanguageSupport::LanguageSupport(std::unique_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}
LanguageSupport::~LanguageSupport() = default;

LanguageSupportResult LanguageSupport::Suggest(
    std::string_view raw_input, const RankingInputs& ranking,
    const LanguageSupportOptions& options) const {
  LanguageSupportResult result;
  result.frequency_source = impl_->source;
  result.production_frequency_supplied = impl_->source == FrequencySource::kProductionCorpus;
  result.token = AnalyzeEnglishToken(raw_input);
  result.outcome = GateOutcome(result.token.decision);
  if (result.token.decision != QueryDecision::kQuery) {
    return result;
  }
  result.case_style = ClassifyAsciiCase(raw_input);
  if (!IsCompletableCase(result.case_style)) {
    result.outcome = LanguageSupportOutcome::kMixedCase;
    return result;
  }
  std::string key;
  if (!ToLookupKey(raw_input, &key)) {
    result.outcome = LanguageSupportOutcome::kUnsupportedByte;
    return result;
  }
  const auto snapshot = impl_->Snapshot(ranking);
  result.ranking_inputs_valid = snapshot.valid;
  const auto form = std::lower_bound(impl_->forms.begin(), impl_->forms.end(), key,
      [](const IndexedForm& value, const std::string& text) { return value.text < text; });
  result.recognized_word_form = impl_->Find(key) != kNoRow ||
                                (form != impl_->forms.end() && form->text == key);
  const auto limit = std::min(options.max_candidates, kMaxLanguageCandidates);
  if (limit == 0) {
    return result;
  }
  CandidateCollector output(limit);
  if (options.enable_morphology) {
    impl_->Morphology(key, result.case_style, snapshot, options, &result, &output);
  }
  if (options.enable_abbreviation) {
    impl_->Expand(key, result.case_style, snapshot, &result, &output);
  }
  if (options.enable_segmentation) {
    if (impl_->source == FrequencySource::kUnavailable) {
      result.segmentation = LanguageFeatureStatus::kFrequencyUnavailable;
    } else if (result.token.delimiter != DelimiterForm::kNone || result.recognized_word_form) {
      result.segmentation = LanguageFeatureStatus::kNotApplicable;
    } else {
      impl_->Segment(key, result.case_style, snapshot, options, &result, &output);
    }
  }
  if (options.enable_spelling) {
    if (result.token.delimiter != DelimiterForm::kNone || result.recognized_word_form ||
        key.size() < 4 || key.size() > 32) {
      result.spelling = LanguageFeatureStatus::kNotApplicable;
    } else {
      impl_->Spell(key, result.case_style, snapshot, options, &result, &output);
    }
  }
  result.candidates = output.Take();
  return result;
}

LanguageSupportResult LanguageSupport::Predict(
    const RankingInputs& ranking, const LanguageSupportOptions& options) const {
  LanguageSupportResult result;
  result.outcome = LanguageSupportOutcome::kQueried;
  result.frequency_source = impl_->source;
  result.production_frequency_supplied = impl_->source == FrequencySource::kProductionCorpus;
  if (impl_->source == FrequencySource::kUnavailable) {
    result.prediction = LanguageFeatureStatus::kFrequencyUnavailable;
    return result;
  }
  const auto snapshot = impl_->Snapshot(ranking);
  result.ranking_inputs_valid = snapshot.valid;
  const auto limit = std::min(options.max_candidates, kMaxLanguageCandidates);
  if (limit == 0) {
    return result;
  }
  const auto budget = std::min(options.prediction_entry_budget, kMaxPredictionRows);
  CandidateCollector output(limit);
  bool found = false;
  for (const auto row : impl_->frequency_rows) {
    const auto& word = impl_->words[row];
    if (result.prediction_entries_examined >= budget) {
      result.prediction = LanguageFeatureStatus::kBudgetExhausted;
      break;
    }
    ++result.prediction_entries_examined;
    Candidate candidate;
    candidate.type = CandidateType::kPrediction;
    candidate.text = word.text;
    candidate.score = impl_->Score(word.id, snapshot.previous_id, snapshot).total;
    candidate.lexeme_ids.push_back(word.id);
    output.Add(std::move(candidate));
    found = true;
  }
  if (result.prediction != LanguageFeatureStatus::kBudgetExhausted) {
    result.prediction = found ? LanguageFeatureStatus::kCandidates : LanguageFeatureStatus::kNoMatch;
  }
  result.candidates = output.Take();
  return result;
}

ScoreBreakdown LanguageSupport::ScoreLexeme(LexemeId id,
                                           const RankingInputs& ranking) const {
  const auto snapshot = impl_->Snapshot(ranking);
  return impl_->Score(id, snapshot.previous_id, snapshot);
}
bool LanguageSupport::HasLexeme(LexemeId id) const { return impl_->by_id.count(id) != 0; }
FrequencySource LanguageSupport::frequency_source() const { return impl_->source; }

const char* CandidateTypeName(CandidateType type) {
  switch (type) {
    case CandidateType::kMorphology: return "english_morphology";
    case CandidateType::kSegmentation: return "english_segmentation";
    case CandidateType::kAbbreviation: return "english_abbreviation";
    case CandidateType::kSpelling: return "english_spelling_extended";
    case CandidateType::kContraction: return "english_contraction";
    case CandidateType::kPrediction: return "english_prediction";
  }
  return "unknown";
}
const char* LanguageSupportBuildStatusName(LanguageSupportBuildStatus status) {
  switch (status) {
    case LanguageSupportBuildStatus::kOk: return "ok";
    case LanguageSupportBuildStatus::kLexemeIdCollision: return "lexeme-id-collision";
    case LanguageSupportBuildStatus::kInvalidFrequencySource: return "invalid-frequency-source";
    case LanguageSupportBuildStatus::kMissingFrequencyProvenance: return "missing-frequency-provenance";
    case LanguageSupportBuildStatus::kEmptyFrequencyData: return "empty-frequency-data";
    case LanguageSupportBuildStatus::kInvalidFrequencyRecord: return "invalid-frequency-record";
    case LanguageSupportBuildStatus::kDuplicateFrequencyRecord: return "duplicate-frequency-record";
    case LanguageSupportBuildStatus::kUnknownFrequencyLexeme: return "unknown-frequency-lexeme";
    case LanguageSupportBuildStatus::kFrequencyCountOverflow: return "frequency-count-overflow";
  }
  return "unknown";
}

}  // namespace en
}  // namespace epocanva
