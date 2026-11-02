#include "lexical_service.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>

#include "english_lexicon.h"
#include "english_token.h"
#include "french_core.h"
#include "language_support.h"
#include "model.h"
#include "morphology_rules.h"

namespace epocanva::lexical {
namespace {

constexpr std::size_t kNoRow = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kMaxAccentEntriesExamined = 4096;  // dictionary rows

bool SupportedLanguage(std::string_view language) {
  return language == "en" || language == "fr" || language == "ko";
}
bool StartsWith(std::string_view word, std::string_view prefix) {
  return word.size() >= prefix.size() && word.substr(0, prefix.size()) == prefix;
}

struct Word {
  LexemeId id = 0;
  std::string language;
  std::string model_text_utf8;  // untouched model-normalized bytes
  std::string alias_utf8;  // native NFC/invariant-lower lookup only, NOT identity
  std::uint64_t count = 0;
};
using WordKey = std::pair<std::string, std::string>;

bool BaseBefore(const Word& left, const Word& right) {
  return left.count != right.count ? left.count > right.count : left.id < right.id;
}
bool CandidateBefore(const Candidate& left, const Candidate& right) {
  if (left.type == CandidateType::kExact && right.type != CandidateType::kExact) return true;
  if (left.type != CandidateType::kExact && right.type == CandidateType::kExact) return false;
  if (left.score != right.score) return left.score > right.score;
  if (left.type != right.type) return left.type < right.type;
  if (left.text_utf8 != right.text_utf8) return left.text_utf8 < right.text_utf8;
  return left.lexeme_ids < right.lexeme_ids;
}

class Collector {
 public:
  explicit Collector(std::size_t limit) : limit_(limit) {}
  void Add(Candidate candidate) {
    if (limit_ == 0 || candidate.lexeme_ids.empty() ||
        std::any_of(candidate.lexeme_ids.begin(), candidate.lexeme_ids.end(),
                     [](LexemeId id) { return id == 0; }) ||
        !std::isfinite(candidate.score) || !std::isfinite(candidate.match_log_score)) return;
    const auto existing = std::find_if(candidates_.begin(), candidates_.end(),
        [&](const Candidate& previous) { return previous.text_utf8 == candidate.text_utf8; });
    if (existing != candidates_.end()) {
      if (!CandidateBefore(candidate, *existing)) return;
      *existing = std::move(candidate);
    } else {
      candidates_.push_back(std::move(candidate));
    }
    std::sort(candidates_.begin(), candidates_.end(), CandidateBefore);
    if (candidates_.size() > limit_) candidates_.resize(limit_);
  }
  std::vector<Candidate> Take() { return std::move(candidates_); }
 private:
  std::size_t limit_;  // candidates, excluding raw original
  std::vector<Candidate> candidates_;
};

// A sorted UTF-8-key index plus a range-maximum tree. Prefix bounds use two
// binary searches. Extracting k base-frequency-ranked rows uses O(k log N)
// tree/heap operations, not a full dictionary scan on every key. The query
// budget counts actual dictionary rows extracted; no hidden model iteration.
class RankedIndex {
 public:
  struct Entry { std::string key_utf8; std::size_t word_row = 0; };
  void Build(std::vector<Entry> entries, const std::vector<Word>* words) {
    words_ = words;
    entries_ = std::move(entries);
    std::sort(entries_.begin(), entries_.end(), [&](const Entry& left, const Entry& right) {
      if (left.key_utf8 != right.key_utf8) return left.key_utf8 < right.key_utf8;
      return BaseBefore((*words_)[left.word_row], (*words_)[right.word_row]);
    });
    leaf_count_ = 1;
    while (leaf_count_ < entries_.size()) leaf_count_ *= 2;
    tree_.assign(leaf_count_ * 2, kNoRow);
    for (std::size_t entry_row = 0; entry_row < entries_.size(); ++entry_row) {
      tree_[leaf_count_ + entry_row] = entry_row;
    }
    for (std::size_t tree_node = leaf_count_; tree_node > 1; --tree_node) {
      const auto parent = tree_node - 1;
      tree_[parent] = Better(tree_[parent * 2], tree_[parent * 2 + 1]);
    }
  }
  bool empty() const { return entries_.empty(); }
  template <typename Visit>
  void Query(std::string_view key_utf8, bool prefix, std::size_t budget,
             std::size_t* examined, bool* exhausted, Visit visit) const {
    const auto lower = std::lower_bound(entries_.begin(), entries_.end(), key_utf8,
        [](const Entry& entry, std::string_view key) { return entry.key_utf8 < key; });
    auto upper = lower;
    if (prefix) {
      // A valid UTF-8 key cannot contain byte 0xff. Appending it bounds every
      // extension while leaving the stored keys and original byte spans intact.
      std::string sentinel(key_utf8);
      sentinel.push_back(static_cast<char>(0xff));
      upper = std::lower_bound(lower, entries_.end(), std::string_view(sentinel),
          [](const Entry& entry, std::string_view key) { return entry.key_utf8 < key; });
    } else {
      upper = std::upper_bound(lower, entries_.end(), key_utf8,
          [](std::string_view key, const Entry& entry) { return key < entry.key_utf8; });
    }
    if (lower == upper) return;
    struct Range {
      std::size_t begin_row;
      std::size_t end_row;
      std::size_t best_row;
    };
    const auto less = [&](const Range& left, const Range& right) {
      return left.best_row != right.best_row &&
             Better(left.best_row, right.best_row) == right.best_row;
    };
    std::priority_queue<Range, std::vector<Range>, decltype(less)> pending(less);
    const auto push = [&](std::size_t begin_row, std::size_t end_row) {
      if (begin_row < end_row) pending.push({begin_row, end_row, Best(begin_row, end_row)});
    };
    push(static_cast<std::size_t>(lower - entries_.begin()),
         static_cast<std::size_t>(upper - entries_.begin()));
    while (!pending.empty() && *examined < budget) {
      const auto range = pending.top();
      pending.pop();
      ++*examined;
      visit(entries_[range.best_row].word_row);
      push(range.begin_row, range.best_row);
      push(range.best_row + 1, range.end_row);
    }
    *exhausted = !pending.empty();
  }
 private:
  std::size_t Better(std::size_t left, std::size_t right) const {
    if (left == kNoRow) return right;
    if (right == kNoRow) return left;
    return BaseBefore((*words_)[entries_[left].word_row],
                      (*words_)[entries_[right].word_row]) ? left : right;
  }
  std::size_t Best(std::size_t begin_row, std::size_t end_row) const {
    begin_row += leaf_count_;
    end_row += leaf_count_;
    std::size_t best = kNoRow;
    while (begin_row < end_row) {
      if ((begin_row & 1u) != 0) best = Better(best, tree_[begin_row++]);
      if ((end_row & 1u) != 0) best = Better(best, tree_[--end_row]);
      begin_row /= 2;
      end_row /= 2;
    }
    return best;
  }
  const std::vector<Word>* words_ = nullptr;
  std::vector<Entry> entries_;
  std::size_t leaf_count_ = 1;  // tree leaf nodes, not text units
  std::vector<std::size_t> tree_;  // indexed row ordinals
};

// The spelling/accent repertoire comes from fr::ComposeLetter itself. These
// encodings index its bounded linguistic rules; they are NOT NFC or a hand-made
// all-Unicode accent/case table. NFC has already been delegated to Windows.
std::string EncodeScalar(char32_t scalar) {
  std::string utf8;
  if (scalar <= 0x7f) {
    utf8.push_back(static_cast<char>(scalar));
  } else if (scalar <= 0x7ff) {
    utf8.push_back(static_cast<char>(0xc0 | (scalar >> 6)));
    utf8.push_back(static_cast<char>(0x80 | (scalar & 0x3f)));
  } else {
    utf8.push_back(static_cast<char>(0xe0 | (scalar >> 12)));
    utf8.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 0x3f)));
    utf8.push_back(static_cast<char>(0x80 | (scalar & 0x3f)));
  }
  return utf8;
}
std::string AccentKey(std::string_view folded_utf8) {
  static const auto rules = [] {
    std::vector<std::pair<std::string, char>> values;
    constexpr std::array<fr::DeadAccent, 5> accents{{fr::DeadAccent::kAcute,
        fr::DeadAccent::kGrave, fr::DeadAccent::kCircumflex,
        fr::DeadAccent::kDiaeresis, fr::DeadAccent::kCedilla}};
    for (char ascii = 'a'; ascii <= 'z'; ++ascii) {
      for (const auto accent : accents) {
        const auto composed = fr::ComposeLetter(accent, static_cast<char32_t>(ascii));
        if (composed) values.emplace_back(EncodeScalar(*composed), ascii);
      }
    }
    return values;
  }();
  std::string key(folded_utf8);
  for (const auto& rule : rules) {
    std::size_t offset_utf8_byte = 0;
    while ((offset_utf8_byte = key.find(rule.first, offset_utf8_byte)) != std::string::npos) {
      key.replace(offset_utf8_byte, rule.first.size(), 1, rule.second);
      ++offset_utf8_byte;
    }
  }
  return key;
}

struct Snapshot {
  bool valid = true;
  LexemeId previous_id = 0;
  std::unordered_map<LexemeId, std::uint64_t> learned;
  std::map<std::pair<LexemeId, LexemeId>, std::uint64_t> context;
};

QueryStatus QueryTextStatus(TextStatus status) {
  switch (status) {
    case TextStatus::kOk: return QueryStatus::kOk;
    case TextStatus::kInvalidUtf8: return QueryStatus::kInvalidUtf8;
    case TextStatus::kInputTooLong: return QueryStatus::kInputTooLong;
    case TextStatus::kUnicodeUnavailable: return QueryStatus::kUnicodeUnavailable;
    case TextStatus::kNativeFailure: return QueryStatus::kNormalizationFailure;
    case TextStatus::kInvalidWord: return QueryStatus::kInvalidWord;
    case TextStatus::kProtectedText: return QueryStatus::kProtectedText;
    case TextStatus::kMixedCase: return QueryStatus::kMixedCase;
  }
  return QueryStatus::kInvalidWord;
}
CandidateType EnglishType(en::CandidateType type) {
  switch (type) {
    case en::CandidateType::kMorphology: return CandidateType::kMorphology;
    case en::CandidateType::kSegmentation: return CandidateType::kSegmentation;
    case en::CandidateType::kAbbreviation: return CandidateType::kAbbreviation;
    case en::CandidateType::kSpelling: return CandidateType::kSpelling;
    case en::CandidateType::kContraction: return CandidateType::kContraction;
    case en::CandidateType::kPrediction: break;  // not called by this provider
  }
  return CandidateType::kOriginal;
}

}  // namespace

struct LexicalService::Impl {
  std::vector<Word> words;
  std::unordered_map<LexemeId, std::size_t> by_id;
  std::map<WordKey, std::size_t> exact_rows;
  std::map<WordKey, std::size_t> alias_rows;
  std::map<std::string, RankedIndex> prefixes;
  std::map<std::string, RankedIndex> accents;
  std::unique_ptr<en::LanguageSupport> english;
  std::unordered_map<en::LexemeId, std::size_t> legacy_to_row;
  std::unordered_map<LexemeId, en::LexemeId> row_to_legacy;
  bool english_frequency_available = false;

  std::size_t Resolve(std::string_view language, std::string_view raw_utf8,
                      std::string_view alias_utf8) const {
    std::string exact_key;
    if (prediction::NormalizeLexemeUtf8(raw_utf8, &exact_key)) {
      const auto exact = exact_rows.find({std::string(language), exact_key});
      if (exact != exact_rows.end()) return exact->second;
    }
    const auto alias = alias_rows.find({std::string(language), std::string(alias_utf8)});
    return alias == alias_rows.end() ? kNoRow : alias->second;
  }
  Snapshot Prepare(const RankingInputs& ranking) const {
    Snapshot result;
    if (!ranking.allow_learning) return result;  // no pointer dereference
    const auto learned_records = ranking.learned ? ranking.learned->size() : 0;
    const auto context_records = ranking.context ? ranking.context->size() : 0;
    if (learned_records > kMaxRankingRecords ||
        context_records > kMaxRankingRecords - learned_records) {
      result.valid = false;
      return result;
    }
    std::set<LexemeId> seen_learned;
    std::set<std::pair<LexemeId, LexemeId>> seen_context;
    if (ranking.learned) {
      for (const auto& record : *ranking.learned) {
        if (record.id == 0 || !seen_learned.insert(record.id).second) {
          result.valid = false;
          break;
        }
        if (by_id.count(record.id) != 0) result.learned.emplace(record.id, record.count);
      }
    }
    if (result.valid && ranking.context) {
      for (const auto& record : *ranking.context) {
        const auto key = std::make_pair(record.previous_id, record.id);
        if (record.previous_id == 0 || record.id == 0 || !seen_context.insert(key).second) {
          result.valid = false;
          break;
        }
        if (by_id.count(record.previous_id) != 0 && by_id.count(record.id) != 0) {
          result.context.emplace(key, record.count);
        }
      }
    }
    if (!result.valid) {
      result.learned.clear();
      result.context.clear();
    } else if (by_id.count(ranking.previous_id) != 0) {
      result.previous_id = ranking.previous_id;
    }
    return result;
  }
  double Score(std::size_t row, const Snapshot& snapshot) const {
    const auto& word = words[row];
    double score = std::log1p(static_cast<double>(word.count));
    const auto learned = snapshot.learned.find(word.id);
    if (learned != snapshot.learned.end()) {
      score += 0.75 * std::log1p(static_cast<double>(learned->second));
    }
    const auto context = snapshot.context.find({snapshot.previous_id, word.id});
    if (context != snapshot.context.end()) {
      score += 1.25 * std::log1p(static_cast<double>(context->second));
    }
    return score;
  }
  void AddWord(std::size_t row, CandidateType type, CaseStyle style,
               std::size_t input_utf8_bytes, const Snapshot& snapshot,
               Collector* output) const {
    const auto display = ApplyCaseStyleUtf8(words[row].alias_utf8, style);
    if (display.status != TextStatus::kOk || display.text_utf8.size() > kMaxWordUtf8Bytes) return;
    Candidate candidate;
    candidate.type = type;
    candidate.text_utf8 = display.text_utf8;
    candidate.lexeme_ids.push_back(words[row].id);
    candidate.input_utf8_byte_spans.push_back({0, input_utf8_bytes});
    if (type == CandidateType::kAccent) candidate.match_log_score = -0.5;
    if (type == CandidateType::kMorphology) candidate.match_log_score = -0.25;
    candidate.score = Score(row, snapshot) + candidate.match_log_score;
    output->Add(std::move(candidate));
  }

  void English(std::string_view raw_utf8, const QueryOptions& options,
               const Snapshot& snapshot, QueryResult* result, Collector* output) const {
    if (!english || !IsAscii(raw_utf8)) return;
    en::RankingInputs ranking;
    std::vector<en::LearnedFrequency> learned;
    std::vector<en::ContextFrequency> context;
    for (const auto& row : snapshot.learned) {
      const auto mapped = row_to_legacy.find(row.first);
      if (mapped != row_to_legacy.end()) learned.push_back({mapped->second, row.second});
    }
    for (const auto& row : snapshot.context) {
      const auto previous = row_to_legacy.find(row.first.first);
      const auto next = row_to_legacy.find(row.first.second);
      if (previous != row_to_legacy.end() && next != row_to_legacy.end()) {
        context.push_back({previous->second, next->second, row.second});
      }
    }
    const auto previous = row_to_legacy.find(snapshot.previous_id);
    if (previous != row_to_legacy.end()) ranking.previous_id = previous->second;
    ranking.allow_learning = snapshot.valid && (!learned.empty() || !context.empty());
    ranking.learned = &learned;
    ranking.context = &context;
    en::LanguageSupportOptions settings;
    // The core truncates before the provider's attestation/deduplication gates.
    // Request its bounded maximum so speculative forms cannot consume a small
    // menu and hide real candidates before we filter them.
    settings.max_candidates = en::kMaxLanguageCandidates;
    settings.enable_morphology = options.enable_morphology;
    settings.enable_spelling = options.enable_spelling;
    settings.enable_segmentation = options.enable_segmentation;
    settings.enable_abbreviation = options.enable_abbreviation;
    settings.morphology_match_budget = options.morphology_match_budget;
    settings.spelling_cell_budget = options.spelling_cell_budget;
    settings.segmentation_transition_budget = options.segmentation_transition_budget;
    settings.keyboard_layout = options.keyboard_layout;
    const auto supported = english->Suggest(raw_utf8, ranking, settings);
    result->morphology_matches_examined = supported.morphology_matches_examined;
    result->spelling_cells = supported.spelling_cells;
    result->segmentation_transitions = supported.segmentation_transitions;
    result->morphology_budget_exhausted = supported.morphology == en::LanguageFeatureStatus::kBudgetExhausted;
    result->spelling_budget_exhausted = supported.spelling == en::LanguageFeatureStatus::kBudgetExhausted;
    result->segmentation_budget_exhausted = supported.segmentation == en::LanguageFeatureStatus::kBudgetExhausted;
    for (const auto& existing : supported.candidates) {
      Candidate candidate;
      candidate.type = EnglishType(existing.type);
      if (candidate.type == CandidateType::kOriginal) continue;
      candidate.text_utf8 = existing.text;
      candidate.score = existing.score;
      candidate.match_log_score = existing.match_log_score;
      candidate.morphology_forms = existing.morphology_forms;
      candidate.edit_distance = existing.edit_distance;
      bool all_known = !existing.lexeme_ids.empty();
      for (const auto id : existing.lexeme_ids) {
        const auto mapped = legacy_to_row.find(id);
        if (id == 0 || mapped == legacy_to_row.end()) { all_known = false; break; }
        candidate.lexeme_ids.push_back(words[mapped->second].id);
      }
      // Explicit expansions require every output word to be admitted. The
      // English core generates possible forms without POS; mirror the Rime
      // default attestation policy (english_translator.cc:839-865), rather than
      // treating a known source lemma as proof that catses'/helloing are words.
      if (!all_known) continue;
      if (candidate.type == CandidateType::kMorphology) {
        const auto canonical = FoldUtf8(candidate.text_utf8);
        if (canonical.status != TextStatus::kOk) continue;
        const bool known_surface = alias_rows.count({"en", canonical.text_utf8}) != 0;
        const bool marked_possessive = candidate.lexeme_ids.size() == 1 &&
            (candidate.morphology_forms & (en::kSingularPossessive | en::kPluralPossessive)) != 0;
        std::string_view base_utf8(canonical.text_utf8);
        if (base_utf8.size() > 2 && base_utf8.substr(base_utf8.size() - 2) == "'s") {
          base_utf8.remove_suffix(2);  // two ASCII UTF-8 code units (bytes)
        } else if (!base_utf8.empty() && base_utf8.back() == '\'') {
          base_utf8.remove_suffix(1);  // one ASCII UTF-8 code unit (byte)
        } else {
          base_utf8 = {};
        }
        const bool attested_possessive = marked_possessive && !base_utf8.empty() &&
            alias_rows.count({"en", std::string(base_utf8)}) != 0;
        if (!known_surface && !attested_possessive) continue;
      }
      for (const auto span : existing.segments_utf8_byte) {
        candidate.input_utf8_byte_spans.push_back({span.begin_utf8_byte, span.end_utf8_byte});
      }
      output->Add(std::move(candidate));
    }
  }

  void Accents(std::string_view language, const WordAnalysis& analysis,
               std::size_t input_utf8_bytes, const QueryOptions& options,
               const Snapshot& snapshot, QueryResult* result, Collector* output) const {
    if (!options.enable_accents || (language != "en" && language != "fr") ||
        fr::ClassifyToken(analysis.normalized_utf8).kind != fr::TokenKind::kFrenchWord) return;
    const auto index = accents.find(std::string(language));
    if (index == accents.end()) return;
    fr::DictionaryInput dictionary;
    dictionary.source.status = fr::DictionarySourceStatus::kCallerSuppliedUnverified;
    dictionary.source.name = "prediction-model-known-catalog-subset";
    dictionary.source.version = "1";
    dictionary.source.licence = "caller-supplied-not-a-licence-audit";
    const auto budget = std::min(options.accent_entry_budget, kMaxAccentEntriesExamined);
    index->second.Query(AccentKey(analysis.folded_utf8), false, budget,
        &result->accent_entries_examined, &result->accent_budget_exhausted,
        [&](std::size_t row) { dictionary.entries.push_back({words[row].alias_utf8, words[row].count}); });
    if (dictionary.entries.empty()) return;
    // Only this indexed, bounded subset is validated by the existing French
    // core; never pass the whole production catalog to its per-call validator.
    const auto accented = fr::SuggestAccents(analysis.normalized_utf8, dictionary,
                                            std::min(options.max_candidates, kMaxCandidates));
    if (accented.status != fr::Status::kOk) return;
    for (const auto& suggestion : accented.suggestions) {
      const auto folded = FoldUtf8(suggestion.text_utf8);
      if (folded.status != TextStatus::kOk) continue;
      const auto row = alias_rows.find({std::string(language), folded.text_utf8});
      if (row == alias_rows.end()) continue;
      AddWord(row->second, CandidateType::kAccent, analysis.case_style,
              input_utf8_bytes, snapshot, output);
    }
  }

  void FrenchPlural(std::size_t exact_row, const WordAnalysis& analysis,
                    std::size_t input_utf8_bytes, const Snapshot& snapshot,
                    Collector* output) const {
    if (exact_row == kNoRow ||
        fr::ClassifyToken(analysis.normalized_utf8).kind != fr::TokenKind::kFrenchWord) return;
    constexpr std::array<fr::PluralRule, 6> rules{{fr::PluralRule::kRegularS,
        fr::PluralRule::kEauX, fr::PluralRule::kAuEuX, fr::PluralRule::kOuX,
        fr::PluralRule::kAlAux, fr::PluralRule::kAilAux}};
    for (const auto rule : rules) {
      const auto form = fr::InflectPlural(words[exact_row].alias_utf8, rule);
      if (form.status != fr::Status::kOk || form.text_utf8 == words[exact_row].alias_utf8) continue;
      const auto known = alias_rows.find({"fr", form.text_utf8});
      if (known == alias_rows.end()) continue;
      // No inferred POS/exception class: the output must ALSO be a real catalog
      // row. This is an attested orthographic alternative, not a grammar claim.
      AddWord(known->second, CandidateType::kMorphology, analysis.case_style,
              input_utf8_bytes, snapshot, output);
    }
  }
};

std::unique_ptr<LexicalService> LexicalService::Create(
    const prediction::Model& model, BuildInfo* info) {
  BuildInfo local;
  if (!info) info = &local;
  *info = {};
  if (!model.available() || model.lexemes().empty()) {
    info->status = BuildStatus::kEmptyCatalog;
    return nullptr;
  }
  auto impl = std::make_unique<Impl>();
  impl->words.reserve(model.lexemes().size());
  std::string english_text;
  en::FrequencyData frequencies;
  frequencies.source = model.metadata().source == prediction::SourceKind::kTestFixture
      ? en::FrequencySource::kTestFixture : en::FrequencySource::kProductionCorpus;
  frequencies.provenance = "prediction-model-source-sha256/" + model.metadata().source_fingerprint;
  for (const auto& entry : model.lexemes()) {
    if (!SupportedLanguage(entry.language_id) || entry.kind != prediction::TokenKind::kWord) {
      ++info->skipped_catalog_entries;
      continue;
    }
    if (entry.id == 0 || prediction::StableLexemeId(entry.language_id, entry.normalized_utf8) != entry.id) {
      info->status = BuildStatus::kLexemeIdMismatch;
      return nullptr;
    }
    const auto analysis = AnalyzeWordUtf8(entry.normalized_utf8);
    if (analysis.status == TextStatus::kUnicodeUnavailable) {
      ++info->unicode_unavailable_entries;
      continue;
    }
    if (analysis.status == TextStatus::kNativeFailure) {
      info->status = BuildStatus::kNormalizationFailure;
      return nullptr;
    }
    if (analysis.status != TextStatus::kOk && analysis.status != TextStatus::kMixedCase) {
      ++info->skipped_catalog_entries;
      continue;
    }
    const std::size_t row = impl->words.size();
    if (!impl->by_id.emplace(entry.id, row).second ||
        !impl->exact_rows.emplace(WordKey{entry.language_id, entry.normalized_utf8}, row).second) {
      info->status = BuildStatus::kDuplicateLexeme;
      return nullptr;
    }
    impl->words.push_back({entry.id, entry.language_id, entry.normalized_utf8,
                           analysis.folded_utf8, model.UnigramCount(entry.id)});
    const auto alias = impl->alias_rows.emplace(WordKey{entry.language_id, analysis.folded_utf8}, row);
    if (!alias.second) {
      ++info->alias_collisions;
      if (BaseBefore(impl->words[row], impl->words[alias.first->second])) alias.first->second = row;
    }
    if (entry.language_id == "en" && en::IsDictionaryEntryForm(entry.normalized_utf8)) {
      english_text += entry.normalized_utf8 + "\t0\n";
      const auto legacy_id = en::StableLexemeId(entry.normalized_utf8);
      impl->legacy_to_row.emplace(legacy_id, row);
      impl->row_to_legacy.emplace(entry.id, legacy_id);
      if (impl->words.back().count > 0) frequencies.entries.push_back({legacy_id, impl->words.back().count});
      ++info->english_ascii_entries;
    }
  }
  if (impl->words.empty()) {
    info->status = info->unicode_unavailable_entries != 0 ? BuildStatus::kUnicodeUnavailable
                                                        : BuildStatus::kEmptyCatalog;
    return nullptr;
  }
  std::map<std::string, std::vector<RankedIndex::Entry>> prefix_entries;
  std::map<std::string, std::vector<RankedIndex::Entry>> accent_entries;
  for (const auto& alias : impl->alias_rows) {
    const auto row = alias.second;
    const auto& word = impl->words[row];
    prefix_entries[word.language].push_back({word.alias_utf8, row});
    if ((word.language == "en" || word.language == "fr") &&
        fr::ClassifyToken(word.alias_utf8).kind == fr::TokenKind::kFrenchWord) {
      accent_entries[word.language].push_back({AccentKey(word.alias_utf8), row});
      if (word.language == "fr") ++info->french_accent_entries;
    }
  }
  for (auto& language : prefix_entries) {
    impl->prefixes[language.first].Build(std::move(language.second), &impl->words);
  }
  for (auto& language : accent_entries) {
    impl->accents[language.first].Build(std::move(language.second), &impl->words);
  }
  if (!english_text.empty()) {
    en::LexiconLoadInfo loaded;
    const auto lexicon = en::EnglishLexicon::FromText(english_text, &loaded);
    if (!lexicon) {
      info->status = BuildStatus::kEnglishCoreFailure;
      info->dependency_status = en::LexiconStatusName(loaded.status);
      return nullptr;
    }
    if (frequencies.entries.empty()) frequencies = {};
    en::LanguageSupportBuildInfo supported;
    impl->english = en::LanguageSupport::Create(*lexicon, frequencies, &supported);
    if (!impl->english) {
      info->status = BuildStatus::kEnglishCoreFailure;
      info->dependency_status = en::LanguageSupportBuildStatusName(supported.status);
      return nullptr;
    }
    impl->english_frequency_available = frequencies.source != en::FrequencySource::kUnavailable;
  }
  info->catalog_entries = impl->words.size();
  return std::unique_ptr<LexicalService>(new LexicalService(std::move(impl)));
}

std::unique_ptr<LexicalService> LexicalService::FromFile(
    const std::filesystem::path& path, BuildInfo* info) {
  return FromFile(path, prediction::LoadOptions{}, info);
}
std::unique_ptr<LexicalService> LexicalService::FromFile(
    const std::filesystem::path& path, const prediction::LoadOptions& options, BuildInfo* info) {
  BuildInfo local;
  if (!info) info = &local;
  *info = {};
  prediction::BuildInfo loaded;
  const auto model = prediction::Model::Load(path, options, &loaded);
  if (!model || !model->available()) {
    info->status = BuildStatus::kModelLoadFailure;
    info->dependency_status = prediction::StatusName(loaded.status);
    return nullptr;
  }
  return Create(*model, info);
}

LexicalService::LexicalService(std::unique_ptr<const Impl> impl) : impl_(std::move(impl)) {}
LexicalService::~LexicalService() = default;

QueryResult LexicalService::Suggest(std::string_view language, std::string_view raw_utf8,
                                    const QueryOptions& options, const RankingInputs& ranking) const {
  QueryResult result;
  if (raw_utf8.empty()) { result.status = QueryStatus::kEmptyInput; return result; }
  if (raw_utf8.size() > kMaxWordUtf8Bytes) { result.status = QueryStatus::kInputTooLong; return result; }
  const auto valid = ValidateUtf8(raw_utf8);
  if (valid != TextStatus::kOk) { result.status = QueryTextStatus(valid); return result; }
  const auto limit = std::min(options.max_candidates, kMaxCandidates);
  if (limit > 0) {
    Candidate original;
    original.type = CandidateType::kOriginal;
    original.text_utf8 = std::string(raw_utf8);
    original.input_utf8_byte_spans.push_back({0, raw_utf8.size()});
    result.candidates.push_back(std::move(original));
  }
  if (!SupportedLanguage(language)) { result.status = QueryStatus::kUnsupportedLanguage; return result; }
  const auto analysis = AnalyzeWordUtf8(raw_utf8, true);
  if (analysis.status != TextStatus::kOk) { result.status = QueryTextStatus(analysis.status); return result; }
  const auto index = impl_->prefixes.find(std::string(language));
  if (index == impl_->prefixes.end() || index->second.empty()) {
    result.status = QueryStatus::kDictionaryUnavailable;
    return result;
  }
  result.english_frequency_available = language == "en" && impl_->english_frequency_available;
  if (limit <= 1) return result;
  const auto snapshot = impl_->Prepare(ranking);
  result.ranking_inputs_valid = snapshot.valid;
  Collector output(limit - 1);
  const auto exact_row = impl_->Resolve(language, raw_utf8, analysis.folded_utf8);
  if (exact_row != kNoRow) {
    impl_->AddWord(exact_row, CandidateType::kExact, analysis.case_style,
                   raw_utf8.size(), snapshot, &output);
  }
  const auto budget = std::min(options.prefix_entry_budget, kMaxPrefixEntriesExamined);
  std::set<std::size_t> visited;
  const auto visit_personal = [&](LexemeId id) {
    const auto found = impl_->by_id.find(id);
    if (found == impl_->by_id.end() || visited.count(found->second) != 0) return;
    const auto row = found->second;
    const auto& word = impl_->words[row];
    if (word.language != language || !StartsWith(word.alias_utf8, analysis.folded_utf8) ||
        word.alias_utf8 == analysis.folded_utf8) return;
    if (result.prefix_entries_examined >= budget) { result.prefix_budget_exhausted = true; return; }
    visited.insert(row);
    ++result.prefix_entries_examined;
    impl_->AddWord(row, CandidateType::kCompletion, analysis.case_style,
                   raw_utf8.size(), snapshot, &output);
  };
  // Visit admitted snapshot IDs first: a low static-frequency row with strong
  // personal evidence must be allowed into a small bounded prefix pool. Sort
  // the already bounded ID set so equal-budget results are deterministic.
  std::set<LexemeId> personal_prefix_ids;
  for (const auto& learned : snapshot.learned) personal_prefix_ids.insert(learned.first);
  for (const auto& context : snapshot.context) {
    if (context.first.first == snapshot.previous_id) personal_prefix_ids.insert(context.first.second);
  }
  for (const auto id : personal_prefix_ids) visit_personal(id);
  index->second.Query(analysis.folded_utf8, true, budget, &result.prefix_entries_examined,
      &result.prefix_budget_exhausted, [&](std::size_t row) {
        if (!visited.insert(row).second ||
            impl_->words[row].alias_utf8 == analysis.folded_utf8) return;
        impl_->AddWord(row, CandidateType::kCompletion, analysis.case_style,
                       raw_utf8.size(), snapshot, &output);
      });
  if (language == "en") impl_->English(raw_utf8, options, snapshot, &result, &output);
  impl_->Accents(language, analysis, raw_utf8.size(), options, snapshot, &result, &output);
  if (language == "fr" && options.enable_morphology) {
    impl_->FrenchPlural(exact_row, analysis, raw_utf8.size(), snapshot, &output);
  }
  auto candidates = output.Take();
  for (auto& candidate : candidates) result.candidates.push_back(std::move(candidate));
  return result;
}

LexemeId LexicalService::FindExact(std::string_view language, std::string_view raw_utf8) const {
  if (!SupportedLanguage(language)) return 0;
  const auto analysis = AnalyzeWordUtf8(raw_utf8);
  if (analysis.status != TextStatus::kOk) return 0;
  const auto row = impl_->Resolve(language, raw_utf8, analysis.folded_utf8);
  return row == kNoRow ? 0 : impl_->words[row].id;
}
bool LexicalService::HasLexeme(LexemeId id) const { return id != 0 && impl_->by_id.count(id) != 0; }
std::size_t LexicalService::size() const { return impl_->words.size(); }

const char* CandidateTypeName(CandidateType type) {
  switch (type) {
    case CandidateType::kOriginal: return "original";
    case CandidateType::kExact: return "exact";
    case CandidateType::kCompletion: return "completion";
    case CandidateType::kSpelling: return "spelling";
    case CandidateType::kAccent: return "accent";
    case CandidateType::kMorphology: return "morphology";
    case CandidateType::kSegmentation: return "segmentation";
    case CandidateType::kAbbreviation: return "abbreviation";
    case CandidateType::kContraction: return "contraction";
  }
  return "unknown";
}
const char* QueryStatusName(QueryStatus status) {
  switch (status) {
    case QueryStatus::kOk: return "ok";
    case QueryStatus::kEmptyInput: return "empty-input";
    case QueryStatus::kInputTooLong: return "input-too-long";
    case QueryStatus::kInvalidUtf8: return "invalid-utf8";
    case QueryStatus::kUnsupportedLanguage: return "unsupported-language";
    case QueryStatus::kDictionaryUnavailable: return "dictionary-unavailable";
    case QueryStatus::kUnicodeUnavailable: return "unicode-unavailable";
    case QueryStatus::kNormalizationFailure: return "normalization-failure";
    case QueryStatus::kProtectedText: return "protected-text";
    case QueryStatus::kMixedCase: return "mixed-case";
    case QueryStatus::kInvalidWord: return "invalid-word";
  }
  return "unknown";
}
const char* BuildStatusName(BuildStatus status) {
  switch (status) {
    case BuildStatus::kOk: return "ok";
    case BuildStatus::kEmptyCatalog: return "empty-catalog";
    case BuildStatus::kUnsupportedLanguage: return "unsupported-language";
    case BuildStatus::kInvalidCatalogWord: return "invalid-catalog-word";
    case BuildStatus::kLexemeIdMismatch: return "lexeme-id-mismatch";
    case BuildStatus::kDuplicateLexeme: return "duplicate-lexeme";
    case BuildStatus::kUnicodeUnavailable: return "unicode-unavailable";
    case BuildStatus::kNormalizationFailure: return "normalization-failure";
    case BuildStatus::kEnglishCoreFailure: return "english-core-failure";
    case BuildStatus::kModelLoadFailure: return "model-load-failure";
  }
  return "unknown";
}

}  // namespace epocanva::lexical
