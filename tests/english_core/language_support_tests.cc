// Standalone algorithm contracts. Literal morphology/ID examples, exhaustive
// segmentation path oracle and full-matrix spelling oracle do not call the
// production generators, trie, scorer or DP to derive their expected answers.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "language_support.h"
#include "morphology_rules.h"
#include "prefix_completer.h"
#include "spelling_suggester.h"

namespace {
using namespace epocanva::en;
int checked = 0;
int failed = 0;

void Check(bool condition, const char* label) {
  ++checked;
  if (!condition) {
    ++failed;
    std::cerr << "ASSERT FAIL " << label << '\n';
  }
}
void Near(double actual, double expected, const char* label) {
  Check(std::isfinite(actual) && std::abs(actual - expected) < 1e-10, label);
}
std::unique_ptr<EnglishLexicon> Lexicon(const std::string& text) {
  LexiconLoadInfo info;
  auto result = EnglishLexicon::FromText(text, &info);
  Check(result != nullptr && info.status == LexiconStatus::kOk, "fixture lexicon parsed");
  return result;
}
FrequencyData Frequencies(std::initializer_list<std::pair<std::string, std::uint64_t>> rows) {
  FrequencyData data;
  data.source = FrequencySource::kTestFixture;
  data.provenance = "project-authored independent frequency fixture; NOT corpus data";
  for (const auto& row : rows) {
    data.entries.push_back({StableLexemeId(row.first), row.second});
  }
  return data;
}
std::unique_ptr<LanguageSupport> Support(const EnglishLexicon& lexicon,
                                       const FrequencyData& data = {}) {
  LanguageSupportBuildInfo info;
  auto result = LanguageSupport::Create(lexicon, data, &info);
  Check(result != nullptr && info.status == LanguageSupportBuildStatus::kOk,
        "language support created");
  return result;
}
LanguageSupportOptions Only(CandidateType type) {
  LanguageSupportOptions options;
  options.enable_morphology = type == CandidateType::kMorphology;
  options.enable_segmentation = type == CandidateType::kSegmentation;
  options.enable_abbreviation = type == CandidateType::kAbbreviation ||
                                type == CandidateType::kContraction;
  options.enable_spelling = type == CandidateType::kSpelling;
  options.max_candidates = kMaxLanguageCandidates;
  options.morphology_match_budget = kMaxMorphologyMatches;
  options.spelling_cell_budget = kMaxSpellingCells;
  options.segmentation_transition_budget = kMaxSegmentationTransitions;
  options.prediction_entry_budget = 65536;
  return options;
}
const Candidate* Find(const LanguageSupportResult& result, std::string_view text) {
  const auto found = std::find_if(result.candidates.begin(), result.candidates.end(),
      [&](const Candidate& candidate) { return candidate.text == text; });
  return found == result.candidates.end() ? nullptr : &*found;
}
std::vector<std::string> Texts(const LanguageSupportResult& result) {
  std::vector<std::string> texts;
  for (const auto& candidate : result.candidates) {
    texts.push_back(candidate.text);
  }
  return texts;
}
std::set<std::string> FormTexts(std::string_view lemma, std::uint32_t mask = kAllMorphologyForms) {
  std::set<std::string> forms;
  for (const auto& form : GenerateWordForms(lemma, mask)) {
    forms.insert(form.canonical);
  }
  return forms;
}

void StableIdsAndFrequencyValidation() {
  // Known-answer values calculated independently with a byte-wise Python
  // FNV-1a-64 reference, not with any C++ production routine at test runtime.
  Check(StableLexemeId("hello") == 0xb96da22a2f71ad36ull, "stable hello ID v1");
  Check(StableLexemeId("don't") == 0x15795106325e2cd4ull, "stable apostrophe ID v1");
  Check(StableLexemeId("co-operate") == 0x1437855839606c0dull, "stable hyphen ID v1");
  Check(StableLexemeId("world") == 0x395e32b4cb3f36eaull, "stable world ID v1");
  for (const auto* invalid : {"", "Hello", "HELLO", "cat'", "two words", "x2", "rock'n'roll"}) {
    Check(StableLexemeId(invalid) == 0, "invalid canonical ID is zero");
  }
  Check(StableLexemeId("caf\xc3\xa9") == 0, "non-ASCII ID is zero");
  auto lexicon = Lexicon("world\t4294967295\nhello\t0\nhelp\t19\n");
  auto reordered = Lexicon("help\t1\nhello\t4294967295\nworld\t0\n");
  if (!lexicon || !reordered) return;
  const auto data = Frequencies({{"hello", 1000}, {"help", 100}, {"world", 1}});
  auto support = Support(*lexicon, data);
  auto support2 = Support(*reordered, data);
  if (!support || !support2) return;
  const auto prediction = support->Predict();
  Check(Texts(prediction) == std::vector<std::string>({"hello", "help", "world"}),
        "independent frequency beats inverted legacy weight");
  Check(Texts(prediction) == Texts(support2->Predict()), "order and legacy weights do not change results");
  Check(!prediction.production_frequency_supplied, "fixture is never production frequency");
  for (const auto& candidate : prediction.candidates) {
    Check(candidate.type == CandidateType::kPrediction && candidate.segments_utf8_byte.empty(),
          "predictions have no fictitious input byte spans");
    Check(candidate.lexeme_ids.size() == 1 && support->HasLexeme(candidate.lexeme_ids[0]),
          "prediction IDs are known lexical IDs");
    Near(candidate.match_log_score, 0.0, "prediction has no input match penalty");
  }
  const auto score = support->ScoreLexeme(StableLexemeId("hello"));
  Check(score.known_lexeme && score.has_base_frequency && score.base_count == 1000,
        "independent base count exposed separately");
  Near(score.base, 6.9087547793152206, "log(1001) literal oracle");
  Near(score.learned, 0, "no implicit learned counts");
  Near(score.context, 0, "no implicit context counts");
  Check(!support->HasLexeme(0) && !support->ScoreLexeme(0).known_lexeme, "zero ID cannot become known");

  auto missing = Support(*lexicon);
  if (missing) {
    const auto result = missing->Suggest("helloworld");
    Check(result.frequency_source == FrequencySource::kUnavailable &&
              result.segmentation == LanguageFeatureStatus::kFrequencyUnavailable,
          "no frequency is explicit, not false success");
    Check(missing->Predict().prediction == LanguageFeatureStatus::kFrequencyUnavailable,
          "prediction has no attestation-weight fallback");
    Near(missing->ScoreLexeme(StableLexemeId("world")).total, 0,
         "largest legacy weight is NOT a frequency");
  }
  const auto expect_bad = [&](FrequencyData bad, LanguageSupportBuildStatus status) {
    LanguageSupportBuildInfo info;
    const auto rejected = LanguageSupport::Create(*lexicon, bad, &info);
    Check(!rejected && info.status == status, "malformed frequency fails atomically with exact reason");
  };
  FrequencyData bad = data;
  bad.source = FrequencySource::kUnavailable;
  expect_bad(bad, LanguageSupportBuildStatus::kInvalidFrequencySource);
  bad = data; bad.source = static_cast<FrequencySource>(999);
  expect_bad(bad, LanguageSupportBuildStatus::kInvalidFrequencySource);
  bad = data; bad.provenance.clear();
  expect_bad(bad, LanguageSupportBuildStatus::kMissingFrequencyProvenance);
  bad = data; bad.entries.clear();
  expect_bad(bad, LanguageSupportBuildStatus::kEmptyFrequencyData);
  bad = data; bad.entries[0].count = 0;
  expect_bad(bad, LanguageSupportBuildStatus::kInvalidFrequencyRecord);
  bad = data; bad.entries[0].id = 0;
  expect_bad(bad, LanguageSupportBuildStatus::kInvalidFrequencyRecord);
  bad = data; bad.entries[0].id = StableLexemeId("absent");
  expect_bad(bad, LanguageSupportBuildStatus::kUnknownFrequencyLexeme);
  bad = data; bad.entries.push_back(bad.entries[0]);
  expect_bad(bad, LanguageSupportBuildStatus::kDuplicateFrequencyRecord);
  bad = data; bad.entries[0].count = std::numeric_limits<std::uint64_t>::max();
  expect_bad(bad, LanguageSupportBuildStatus::kFrequencyCountOverflow);
  bad = Frequencies({{"hello", std::numeric_limits<std::uint64_t>::max()}});
  auto largest = Support(*lexicon, bad);
  Check(largest && std::isfinite(largest->ScoreLexeme(StableLexemeId("hello")).total),
        "single maximum count remains finite");
  bad = data; bad.source = FrequencySource::kProductionCorpus;
  auto declared = Support(*lexicon, bad);
  Check(declared && declared->Predict().production_frequency_supplied,
        "caller declared source reported distinctly, not treated as certification");
  // The immutable support owns the dictionary bytes. No lexicon lifetime trap.
  lexicon.reset();
  Check(Texts(support->Predict()) == std::vector<std::string>({"hello", "help", "world"}),
        "support outlives source lexicon");
}

void MorphologyLiteralOracle() {
  struct Example { const char* lemma; std::uint32_t mask; std::set<std::string> expected; };
  const std::vector<Example> examples = {
      {"cat", kPlural, {"cats"}}, {"box", kPlural, {"boxes"}},
      {"church", kPlural, {"churches"}}, {"brush", kThirdPersonSingular, {"brushes"}},
      {"city", kPlural, {"cities"}}, {"play", kThirdPersonSingular, {"plays"}},
      {"study", kPast | kProgressive, {"studied", "studying"}},
      {"carry", kThirdPersonSingular | kPast | kProgressive, {"carries", "carried", "carrying"}},
      {"walk", kPast | kProgressive, {"walked", "walking"}},
      {"love", kPast | kProgressive, {"loved", "loving"}},
      {"make", kPast | kProgressive, {"made", "making"}},
      {"tie", kPast | kProgressive, {"tied", "tying"}},
      {"see", kPast | kPastParticiple | kProgressive, {"saw", "seen", "seeing"}},
      {"dye", kProgressive, {"dyeing"}}, {"singe", kProgressive, {"singeing"}},
      {"stop", kPast | kProgressive, {"stopped", "stopping"}},
      {"plan", kPast | kProgressive, {"planned", "planning"}},
      {"fix", kPast | kProgressive, {"fixed", "fixing"}},
      {"snow", kPast | kProgressive, {"snowed", "snowing"}},
      {"open", kPast | kProgressive, {"opened", "opening"}},
      {"visit", kPast | kProgressive, {"visited", "visiting"}},
      {"prefer", kPast | kProgressive, {"preferred", "preferring"}},
      {"panic", kPast | kProgressive, {"panicked", "panicking"}},
      {"child", kPlural | kSingularPossessive | kPluralPossessive,
          {"children", "child's", "children's"}},
      {"cat", kSingularPossessive | kPluralPossessive, {"cat's", "cats'"}},
      {"knife", kPlural, {"knives"}}, {"mouse", kPlural, {"mice"}},
      {"person", kPlural, {"people"}}, {"foot", kPlural, {"feet"}},
      {"goose", kPlural, {"geese"}}, {"sheep", kPlural, {"sheep"}},
      {"be", kThirdPersonSingular | kPast | kPastParticiple | kProgressive | kIrregularPresent,
          {"is", "was", "were", "been", "being", "am", "are"}},
      {"go", kThirdPersonSingular | kPast | kPastParticiple | kProgressive,
          {"goes", "went", "gone", "going"}},
      {"run", kPast | kPastParticiple | kProgressive, {"ran", "run", "running"}},
      {"write", kPast | kPastParticiple | kProgressive, {"wrote", "written", "writing"}},
      {"have", kThirdPersonSingular | kPast | kProgressive, {"has", "had", "having"}},
      {"get", kPast | kPastParticiple, {"got", "gotten"}},
      {"buy", kPast, {"bought"}}, {"teach", kPast, {"taught"}},
      {"read", kPast | kPastParticiple, {"read"}},
  };
  for (const auto& example : examples) {
    Check(FormTexts(example.lemma, example.mask) == example.expected,
          "literal word-form oracle exactly matches selected rule categories");
  }
  for (const auto* rejected : {"", "Cat", "CAT", "a", "the", "is", "am", "are",
                               "went", "children", "don't", "co-operate", "x1"}) {
    Check(GenerateWordForms(rejected).empty(), "unsupported stems and protected forms produce no inflections");
  }
  Check(GenerateWordForms("caf\xc3\xa9").empty(), "morphology never broadens ASCII grammar");
  Check(GenerateWordForms(std::string(64, 'a')).empty(), "overlong generated forms not truncated");
  Check(FormTexts("go").count("goed") == 0 && FormTexts("child").count("childs") == 0,
        "irregulars suppress wrong regular alternatives");
  Check(std::string(MorphologyRuleSetVersion()) == "epocanva.en.morphology/v1", "rule version declared");
  const auto walks = GenerateWordForms("walk");
  const auto walking = std::find_if(walks.begin(), walks.end(),
      [](const GeneratedWordForm& form) { return form.canonical == "walks"; });
  Check(walking != walks.end() && (walking->forms & (kPlural | kThirdPersonSingular)) ==
            (kPlural | kThirdPersonSingular), "same surface merges grammatical form bits");

  // Lemmas ONLY: every expected form below must be absent from the dictionary.
  auto lexicon = Lexicon("cat\t1\nchild\t2\ngo\t3\nrun\t4\nmake\t5\nstudy\t6\n");
  if (!lexicon) return;
  auto support = Support(*lexicon);
  if (!support) return;
  const auto options = Only(CandidateType::kMorphology);
  struct Query { const char* input; const char* form; const char* lemma; };
  const Query queries[] = {{"cat", "cats", "cat"}, {"cat", "cat's", "cat"},
      {"cats", "cats'", "cat"}, {"child", "children", "child"},
      {"child", "children's", "child"}, {"go", "went", "go"},
      {"go", "gone", "go"}, {"run", "ran", "run"},
      {"runn", "running", "run"}, {"stud", "studied", "study"},
      {"mak", "making", "make"}};
  for (const auto& query : queries) {
    Check(lexicon->Find(query.form) == nullptr, "wordform really absent from source dictionary");
    const auto result = support->Suggest(query.input, {}, options);
    const auto* candidate = Find(result, query.form);
    Check(candidate && candidate->type == CandidateType::kMorphology,
          "absent wordform generated algorithmically");
    if (!candidate) continue;
    Check(candidate->lexeme_ids == std::vector<LexemeId>{StableLexemeId(query.lemma)},
          "generated forms use stable admitted lemma ID, not raw hash");
    Check(candidate->segments_utf8_byte.size() == 1 &&
              candidate->segments_utf8_byte[0].begin_utf8_byte == 0 &&
              candidate->segments_utf8_byte[0].end_utf8_byte == std::string_view(query.input).size(),
          "wordform span covers original query UTF-8 bytes");
  }
  Check(Find(support->Suggest("Run", {}, options), "Running") != nullptr,
        "initial capital morphology preserved");
  Check(Find(support->Suggest("RUN", {}, options), "RUNNING") != nullptr,
        "uppercase morphology preserved");
  const auto known = support->Suggest("running");
  Check(known.recognized_word_form && known.spelling == LanguageFeatureStatus::kNotApplicable,
        "valid generated form is not spell-corrected");
  Check(!Find(known, "running"), "raw original never duplicated");
  LexiconLoadInfo info;
  Check(!EnglishLexicon::FromText("cats'\t1\n", &info) && info.status == LexiconStatus::kIllegalWordForm,
        "generated plural possessive does not change legacy dictionary grammar");
}

void ExactLemmaBeatsCompetingPrefixes() {
  // The real product has walkaway/walker/walking prefixes. With all frequency
  // absent, lexical sorting alone displaces walking from a small menu. Extra
  // synthetic competing prefixes make that failure independent of product data.
  auto lexicon = Lexicon("walk\t1\nwalkability\t99\nwalkable\t98\nwalkaway\t97\nwalker\t96\nwalking\t95\n");
  if (!lexicon) return;
  auto support = Support(*lexicon);
  if (!support) return;
  LanguageSupportOptions options;
  options.max_candidates = 5;
  const auto result = support->Suggest("walk", {}, options);
  Check(Texts(result) == std::vector<std::string>({"walk's", "walked", "walking", "walks", "walks'"}),
        "exact lemma morphology survives competing prefixes and small menu");
  for (const auto& candidate : result.candidates) {
    Check(candidate.lexeme_ids == std::vector<LexemeId>{StableLexemeId(
              candidate.text == "walking" ? "walking" : "walk")},
          "exact lemma form uses attested surface or documented lemma fallback");
    Near(candidate.score, 0.75, "explicit exact-lemma preference, not incidental insertion order");
    Near(candidate.match_log_score, 0.75, "exact-lemma match feature is count independent");
  }
  options.max_candidates = 64;
  const auto complete = support->Suggest("walk", {}, options);
  const auto* prefix = Find(complete, "walkaway's");
  Check(prefix != nullptr, "prefix forms still available, not discarded for exact lemma");
  if (prefix) Near(prefix->score, -0.25, "nonexact prefix form has no exact-lemma bonus");
}

void AttestedMorphologyIdentities() {
  auto lexicon = Lexicon("walk\t1\nwalked\t1\nwalking\t1\nwalk's\t1\n"
                         "child\t1\nchildren\t1\ngo\t1\nwent\t1\nbefore\t1\n");
  if (!lexicon) return;
  auto support = Support(*lexicon, Frequencies({{"walk", 9000}, {"walked", 9},
      {"walking", 19}, {"walk's", 29}, {"child", 7000}, {"children", 39},
      {"go", 5000}, {"went", 49}, {"before", 1}}));
  if (!support) return;
  std::vector<LearnedFrequency> learned{{StableLexemeId("walk"), 900000},
      {StableLexemeId("walked"), 99}, {StableLexemeId("children"), 199}};
  std::vector<ContextFrequency> context{
      {StableLexemeId("before"), StableLexemeId("walk"), 900000},
      {StableLexemeId("before"), StableLexemeId("walked"), 999},
      {StableLexemeId("before"), StableLexemeId("went"), 1999}};
  RankingInputs ranking{StableLexemeId("before"), true, &learned, &context};
  const auto options = Only(CandidateType::kMorphology);
  struct Example {
    const char* query;
    const char* surface;
    std::uint64_t base;
    std::uint64_t learned;
    std::uint64_t context;
    double match;
  };
  const Example examples[] = {
      {"walk", "walked", 9, 99, 999, 0.75},
      {"walk", "walking", 19, 0, 0, 0.75},
      {"Walk", "Walked", 9, 99, 999, 0.75},
      {"walk", "walk's", 29, 0, 0, 0.75},
      {"wal", "walked", 9, 99, 999, -0.25},
      {"child", "children", 39, 199, 0, 0.75},
      {"go", "went", 49, 0, 1999, 0.75}};
  for (const auto& example : examples) {
    const auto result = support->Suggest(example.query, ranking, options);
    const auto* candidate = Find(result, example.surface);
    Check(candidate != nullptr, "attested morphology reaches suggestions");
    if (!candidate) continue;
    std::string canonical;
    Check(ToLookupKey(example.surface, &canonical), "surface fixture canonicalized");
    Check(candidate->lexeme_ids == std::vector<LexemeId>{StableLexemeId(canonical)},
          "regular irregular and attested possessive forms use SURFACE identity");
    Near(candidate->match_log_score, example.match,
         "morphology match channel excludes lemma and surface counts");
    Near(candidate->score, std::log1p(static_cast<double>(example.base)) +
         0.75 * std::log1p(static_cast<double>(example.learned)) +
         1.25 * std::log1p(static_cast<double>(example.context)) + example.match,
         "morphology total uses surface unigram learned and context only");
  }
  const auto fallback = support->Suggest("child", ranking, options);
  const auto* possessive = Find(fallback, "children's");
  Check(possessive && possessive->lexeme_ids ==
            std::vector<LexemeId>{StableLexemeId("child")},
        "unattested possessive keeps documented admitted lemma fallback");
  if (possessive) Near(possessive->match_log_score, 0.75,
                      "possessive fallback retains morphology input feature");
}

void LearningAndPrediction() {
  auto lexicon = Lexicon("hello\t1\nhelp\t3\nhero\t4\nworld\t2\n");
  if (!lexicon) return;
  auto support = Support(*lexicon, Frequencies({{"hello", 100}, {"help", 50}, {"hero", 10}, {"world", 20}}));
  if (!support) return;
  const auto hello = StableLexemeId("hello");
  const auto help = StableLexemeId("help");
  const auto world = StableLexemeId("world");
  std::vector<LearnedFrequency> learned{{help, 1000}};
  std::vector<ContextFrequency> context{{world, hello, 10000}, {hello, help, 20}};
  RankingInputs ranking{world, true, &learned, &context};
  auto score = support->ScoreLexeme(hello, ranking);
  Check(score.base_count == 100 && score.learned_count == 0 && score.context_count == 10000,
        "base user and context counts are distinct channels");
  Near(score.total, std::log(101.0) + 1.25 * std::log(10001.0), "ranking independent equation oracle");
  Near(support->ScoreLexeme(help, ranking).learned, 0.75 * std::log(1001.0), "user count contribution");
  Check(support->Predict(ranking).candidates.front().text == "hello", "matching previous ID changes next-word ranking");
  ranking.previous_id = 0;
  Check(support->Predict(ranking).candidates.front().text == "help", "no previous ID still uses admitted learned counts");
  ranking.allow_learning = false;
  score = support->ScoreLexeme(help, ranking);
  Check(score.learned_count == 0 && score.context_count == 0, "private mode ignores both statistics inputs");
  Check(Texts(support->Predict(ranking)) == Texts(support->Predict()), "private prediction equals base-only prediction");
  ranking.allow_learning = true;
  ranking.previous_id = StableLexemeId("absent");
  Check(support->ScoreLexeme(hello, ranking).context_count == 0, "unknown previous ID cannot supply context");
  learned.push_back({StableLexemeId("absent"), std::numeric_limits<std::uint64_t>::max()});
  Check(!support->HasLexeme(StableLexemeId("absent")), "statistics cannot enroll raw/OOV IDs");
  Check(support->ScoreLexeme(hello, ranking).ranking_inputs_valid, "unknown unique IDs ignored without poisoning known data");
  learned.push_back({help, 3});
  score = support->ScoreLexeme(help, ranking);
  Check(!score.ranking_inputs_valid && score.learned_count == 0 && score.context_count == 0,
        "duplicate learned ID rejects whole learned/context snapshot");
  learned.pop_back();
  context.push_back(context.front());
  score = support->ScoreLexeme(help, ranking);
  Check(!score.ranking_inputs_valid && score.learned_count == 0,
        "duplicate context pair also rejects user-count channel");
  ranking.allow_learning = false;
  Check(support->ScoreLexeme(help, ranking).ranking_inputs_valid,
        "private mode does not validate or dereference supplied bad statistics");
  context.pop_back();
  learned.assign(kMaxRankingRecords + 1, LearnedFrequency{help, 1});
  ranking.allow_learning = true;
  Check(!support->Predict(ranking).ranking_inputs_valid, "oversized statistics rejected, not silently truncated");
  learned = {{help, 1000}};
  const auto before = support->Predict(ranking);
  for (int repetition = 0; repetition < 5; ++repetition) {
    Check(Texts(support->Predict(ranking)) == Texts(before), "queries do not mutate learning counts or retain inputs");
  }
  Check(learned[0].count == 1000 && context[0].count == 10000, "caller statistics are read-only");
  auto missing = Support(*lexicon);
  Check(missing && missing->Predict(ranking).prediction == LanguageFeatureStatus::kFrequencyUnavailable,
        "learning cannot disguise absent base frequency as production prediction");
  auto budget = Only(CandidateType::kPrediction);
  budget.prediction_entry_budget = 1;
  const auto partial = support->Predict({}, budget);
  Check(partial.prediction_entries_examined == 1 &&
            partial.prediction == LanguageFeatureStatus::kBudgetExhausted &&
            partial.candidates.size() == 1, "prediction budget is observable and bounded");
}

struct OraclePath { std::string text; double score; std::vector<std::size_t> boundaries_utf8_byte; };
// Exponential, deliberately NOT DP: enumerate every literal dictionary path of a
// small input, compute its probability feature separately and sort all of them.
std::vector<OraclePath> ExhaustiveSegments(
    const std::string& text, const std::map<std::string, std::uint64_t>& counts,
    const std::map<std::pair<std::string, std::string>, std::uint64_t>& context = {},
    std::string previous = {}, std::size_t max_words = 8) {
  std::uint64_t total = 0;
  for (const auto& row : counts) total += row.second;
  const double denominator = static_cast<double>(total) + static_cast<double>(counts.size());
  std::vector<OraclePath> answers;
  std::function<void(std::size_t, std::string, OraclePath, std::size_t)> enumerate;
  enumerate = [&](std::size_t begin_utf8_byte, std::string prior, OraclePath path, std::size_t words) {
    if (begin_utf8_byte == text.size()) {
      if (words >= 2) answers.push_back(std::move(path));
      return;
    }
    if (words >= max_words) return;
    for (const auto& row : counts) {
      if (row.first.size() == 1 && row.first != "a" && row.first != "i") continue;
      if (text.compare(begin_utf8_byte, row.first.size(), row.first) != 0) continue;
      OraclePath next = path;
      if (words != 0) { next.text += " "; next.score -= 0.75; }
      next.text += row.first;
      next.score += std::log((static_cast<double>(row.second) + 1.0) / denominator);
      const auto bigram = context.find({prior, row.first});
      if (bigram != context.end()) next.score += 1.25 * std::log(static_cast<double>(bigram->second) + 1.0);
      const auto end_utf8_byte = begin_utf8_byte + row.first.size();
      next.boundaries_utf8_byte.push_back(end_utf8_byte);
      enumerate(end_utf8_byte, row.first, std::move(next), words + 1);
    }
  };
  enumerate(0, std::move(previous), {}, 0);
  std::sort(answers.begin(), answers.end(), [](const OraclePath& a, const OraclePath& b) {
    return a.score != b.score ? a.score > b.score : a.text < b.text;
  });
  if (answers.size() > kMaxSegmentationCandidates) answers.resize(kMaxSegmentationCandidates);
  return answers;
}

void SegmentationOracleAndBoundaries() {
  const std::map<std::string, std::uint64_t> counts{
      {"a", 40}, {"an", 20}, {"ant", 2}, {"art", 90}, {"go", 50},
      {"i", 100}, {"in", 20}, {"into", 20}, {"t", 999}, {"tart", 30}, {"to", 10}};
  std::string rows;
  FrequencyData data;
  data.source = FrequencySource::kTestFixture;
  data.provenance = "synthetic ambiguous segmentation fixture; not corpus frequency";
  for (const auto& row : counts) {
    rows += row.first + "\t1\n";
    data.entries.push_back({StableLexemeId(row.first), row.second});
  }
  auto lexicon = Lexicon(rows);
  if (!lexicon) return;
  auto support = Support(*lexicon, data);
  if (!support) return;
  auto options = Only(CandidateType::kSegmentation);
  for (const auto* text : {"antart", "igointo", "anan", "antartgo", "zzzzz", "gointogo"}) {
    const auto expected = ExhaustiveSegments(text, counts);
    const auto result = support->Suggest(text, {}, options);
    Check(result.candidates.size() == expected.size(), "all top-k exhaustive segmentation paths recovered");
    for (std::size_t index = 0; index < std::min(result.candidates.size(), expected.size()); ++index) {
      const auto& candidate = result.candidates[index];
      Check(candidate.text == expected[index].text, "DP agrees with independent exhaustive path ordering");
      Near(candidate.score, expected[index].score, "DP score agrees with independent path equation");
      Near(candidate.match_log_score,
           -0.75 * static_cast<double>(expected[index].boundaries_utf8_byte.size() - 1),
           "segmentation match term omits unigram normalizer and counts");
      Check(candidate.segments_utf8_byte.size() == expected[index].boundaries_utf8_byte.size(), "one span per oracle word");
      std::size_t begin_utf8_byte = 0;
      for (std::size_t word = 0; word < candidate.segments_utf8_byte.size(); ++word) {
        const auto& span = candidate.segments_utf8_byte[word];
        Check(span.begin_utf8_byte == begin_utf8_byte &&
                  span.end_utf8_byte == expected[index].boundaries_utf8_byte[word],
              "segments use contiguous original UTF-8 byte offsets, not output spaces");
        const std::string canonical = std::string(text).substr(begin_utf8_byte, span.end_utf8_byte - begin_utf8_byte);
        Check(candidate.lexeme_ids[word] == StableLexemeId(canonical), "segment identity maps to corresponding dictionary word");
        begin_utf8_byte = span.end_utf8_byte;
      }
      Check(begin_utf8_byte == std::string_view(text).size(), "segmentation must cover all original bytes");
    }
  }
  const auto base = support->Suggest("antart", {}, options);
  Check(!base.candidates.empty() && base.candidates[0].text == "an tart", "literal best segmentation uses frequency not longest-match greediness");
  Check(Find(support->Suggest("Antart", {}, options), "An tart") != nullptr,
        "initial capital applies to whole output, not every segment");
  Check(Find(support->Suggest("ANTART", {}, options), "AN TART") != nullptr,
        "segmentation uppercase rendering");
  const auto foreign = support->Suggest("antartx", {}, options);
  Check(foreign.candidates.empty(), "no truncated or OOV tail segmentation");
  Check(support->Suggest("tart", {}, options).segmentation == LanguageFeatureStatus::kNotApplicable,
        "existing dictionary word is not split");
  Check(support->Suggest("going", {}, options).segmentation == LanguageFeatureStatus::kNotApplicable,
        "recognized generated wordform is not split");

  // The first prefix prefers an+tart, but ant+art+go wins after its bigram.
  // A single-best path at each byte boundary loses this alternative.
  std::vector<ContextFrequency> context{{StableLexemeId("art"), StableLexemeId("go"), 10000}};
  RankingInputs ranking{0, true, nullptr, &context};
  const auto expected = ExhaustiveSegments("antartgo", counts, {{{"art", "go"}, 10000}});
  const auto contextual = support->Suggest("antartgo", ranking, options);
  Check(!contextual.candidates.empty() && contextual.candidates[0].text == "ant art go",
        "last-ID DP state preserves globally winning bigram alternative");
  Check(contextual.candidates.size() == expected.size(), "context oracle path count");
  for (std::size_t index = 0; index < std::min(contextual.candidates.size(), expected.size()); ++index) {
    Check(contextual.candidates[index].text == expected[index].text, "context DP ordering matches exhaustive oracle");
    Near(contextual.candidates[index].score, expected[index].score, "context path score oracle");
    Near(contextual.candidates[index].match_log_score,
         -0.75 * static_cast<double>(expected[index].boundaries_utf8_byte.size() - 1),
         "learned internal bigrams never enter segmentation match penalty");
  }
  ranking.allow_learning = false;
  Check(Texts(support->Suggest("antartgo", ranking, options)) == Texts(support->Suggest("antartgo", {}, options)),
        "private segmentation ignores internal bigrams too");
  options.max_segmentation_words = 2;
  Check(support->Suggest("antartgo", ranking, options).candidates.empty(), "DP state respects maximum word count");
  options.max_segmentation_words = 8;
  options.segmentation_transition_budget = 1;
  const auto limited = support->Suggest("antartgo", {}, options);
  Check(limited.segmentation == LanguageFeatureStatus::kBudgetExhausted && limited.segmentation_transitions == 1,
        "segmentation real work budget stops at exact transition count");
  options.segmentation_transition_budget = 0;
  const auto zero = support->Suggest("antartgo", {}, options);
  Check(zero.segmentation_transitions == 0 && zero.candidates.empty(), "zero segmentation budget does no transitions");
  auto partial_frequency = Support(*lexicon, Frequencies({{"an", 2}, {"art", 3}}));
  Check(partial_frequency && partial_frequency->Suggest("antart", {}, Only(CandidateType::kSegmentation)).candidates.empty(),
        "uncovered word cannot silently get a made-up base frequency");
}

// Full O(m*n) matrix over EACH dictionary word; no trie, shared prefixes,
// keyboard helper, candidate generator or production distance routine used.
unsigned MatrixOsa(std::string_view a, std::string_view b) {
  std::vector<std::vector<unsigned>> distance(a.size() + 1,
                                             std::vector<unsigned>(b.size() + 1));
  for (std::size_t i_byte = 0; i_byte <= a.size(); ++i_byte) distance[i_byte][0] = static_cast<unsigned>(i_byte);
  for (std::size_t j_byte = 0; j_byte <= b.size(); ++j_byte) distance[0][j_byte] = static_cast<unsigned>(j_byte);
  for (std::size_t i_byte = 1; i_byte <= a.size(); ++i_byte) {
    for (std::size_t j_byte = 1; j_byte <= b.size(); ++j_byte) {
      distance[i_byte][j_byte] = std::min({distance[i_byte - 1][j_byte] + 1,
          distance[i_byte][j_byte - 1] + 1,
          distance[i_byte - 1][j_byte - 1] + (a[i_byte - 1] == b[j_byte - 1] ? 0u : 1u)});
      if (i_byte > 1 && j_byte > 1 && a[i_byte - 1] == b[j_byte - 2] && a[i_byte - 2] == b[j_byte - 1]) {
        distance[i_byte][j_byte] = std::min(distance[i_byte][j_byte], distance[i_byte - 2][j_byte - 2] + 1);
      }
    }
  }
  return distance.back().back();
}
void VerifySpellingOracle(const EnglishLexicon& lexicon, const LanguageSupport& support,
                          const std::string& query) {
  const auto result = support.Suggest(query, {}, Only(CandidateType::kSpelling));
  std::set<std::string> wanted;
  for (const auto& word : lexicon.entries()) {
    const auto distance = MatrixOsa(query, word.word);
    if (distance > 0 && distance <= 2) wanted.insert(word.word);
  }
  Check(wanted.size() <= kMaxLanguageCandidates, "oracle fixture fits candidate cap");
  std::set<std::string> got;
  for (const auto& candidate : result.candidates) {
    got.insert(candidate.text);
    Check(candidate.edit_distance == MatrixOsa(query, candidate.text), "spelling distance matches independent full matrix");
    Check(candidate.type == CandidateType::kSpelling && candidate.lexeme_ids.size() == 1 &&
              candidate.lexeme_ids[0] == StableLexemeId(candidate.text), "spelling IDs always point to real dictionary entries");
  }
  Check(got == wanted, "trie suggestions equal complete two-edit oracle set");
  Check(result.spelling != LanguageFeatureStatus::kBudgetExhausted, "oracle fixture search genuinely completed");
}

void TwoEditSpellingAndKeyboard() {
  auto lexicon = Lexicon("hello\t1\nworld\t2\nhelp\t3\nheld\t4\nshell\t5\nyellow\t6\nhouse\t7\nmouse\t8\nkeyboard\t9\n");
  if (!lexicon) return;
  auto support = Support(*lexicon);
  if (!support) return;
  for (const auto* query : {"hxlo", "hxlol", "ehllp", "wrold", "hellxq", "mousx", "shlel", "zzzz", "qellp"}) {
    VerifySpellingOracle(*lexicon, *support, query);
  }
  const auto two = support->Suggest("hxlo", {}, Only(CandidateType::kSpelling));
  const auto* hello = Find(two, "hello");
  Check(hello && hello->edit_distance == 2, "genuine two-edit correction beyond legacy one-edit pass");
  Check(SuggestSpelling(lexicon.get(), "hxlo", 0, 16, true).suggestions.empty(),
        "two-edit example is absent from legacy one-edit implementation");
  Check(Find(support->Suggest("HXLO", {}, Only(CandidateType::kSpelling)), "HELLO") != nullptr,
        "extended spelling preserves uppercase");
  Check(Find(support->Suggest("Hxlo", {}, Only(CandidateType::kSpelling)), "Hello") != nullptr,
        "extended spelling preserves initial capital");
  auto options = Only(CandidateType::kSpelling);
  options.spelling_cell_budget = 7;
  const auto limited = support->Suggest("hxlo", {}, options);
  Check(limited.spelling == LanguageFeatureStatus::kBudgetExhausted && limited.spelling_cells == 7,
        "DP cell budget exhaustion is exact and observable");
  options.spelling_cell_budget = 0;
  const auto zero = support->Suggest("hxlo", {}, options);
  Check(zero.spelling_cells == 0 && zero.candidates.empty() &&
            zero.spelling == LanguageFeatureStatus::kBudgetExhausted,
        "zero spelling budget cannot run a hidden search");
  options.spelling_cell_budget = std::numeric_limits<std::size_t>::max();
  Check(support->Suggest("hxlo", {}, options).spelling_cells <= kMaxSpellingCells,
        "caller cannot lift hard spelling ceiling");
  for (const auto* query : {"hel", "hello", "don't", "co-operate"}) {
    Check(support->Suggest(query, {}, options).spelling == LanguageFeatureStatus::kNotApplicable,
          "short known or delimited input not corrected");
  }
  Check(support->Suggest(std::string(33, 'q'), {}, options).spelling == LanguageFeatureStatus::kNotApplicable,
        "extended spelling window is still 4..32 UTF-8 bytes");
  auto keys = Lexicon("test\t0\ntess\t4294967295\n");
  if (keys) {
    auto keyboard = Support(*keys);
    if (keyboard) {
      const auto result = keyboard->Suggest("tesr", {}, Only(CandidateType::kSpelling));
      Check(Texts(result) == std::vector<std::string>({"test", "tess"}), "near-key substitution beats far-key regardless of attestation rank");
      const auto* near = Find(result, "test");
      const auto* far = Find(result, "tess");
      Check(near && far && near->keyboard_cost == 60 && far->keyboard_cost == 100,
            "literal QWERTY r-t/r-s cost oracle");
      const auto swapped = keyboard->Suggest("tset", {}, Only(CandidateType::kSpelling));
      Check(Find(swapped, "test") && Find(swapped, "test")->edit_distance == 1 &&
                Find(swapped, "test")->keyboard_cost == 80, "adjacent swap has explicit 1-edit 80-cost contract");
    }
  }
  // Dozens of held-out synthetic dictionary rows prevent a canned English list
  // from satisfying this test. Distances come from the full matrix, not variants.
  std::string generated_rows;
  for (char a = 'a'; a <= 'c'; ++a) for (char b = 'a'; b <= 'c'; ++b)
    for (char c = 'a'; c <= 'c'; ++c) for (char d = 'a'; d <= 'c'; ++d)
      generated_rows += std::string{a, b, c, d} + "\t1\n";
  auto synthetic = Lexicon(generated_rows);
  if (synthetic) {
    auto generated = Support(*synthetic);
    if (generated) {
      for (const auto* query : {"abdd", "cdbd", "dddd", "dabc", "aadb", "bdbd"}) {
        VerifySpellingOracle(*synthetic, *generated, query);
      }
    }
  }
}

void LayoutAwareSpelling() {
  using Layout = epocanva::keyboard::Layout;
  Check(static_cast<std::uint32_t>(Layout::kUnknown) == 0 &&
            static_cast<std::uint32_t>(Layout::kUsQwerty) == 1 &&
            static_cast<std::uint32_t>(Layout::kFrenchAzerty) == 2 &&
            static_cast<std::uint32_t>(Layout::kGermanQwertz) == 3 &&
            static_cast<std::uint32_t>(Layout::kDvorak) == 4,
        "keyboard enum values are explicit adapter contract");
  Check(LanguageSupportOptions{}.keyboard_layout == Layout::kUsQwerty,
        "unspecified legacy option keeps US QWERTY");
  const std::array<Layout, 7> layouts{Layout::kUsQwerty, Layout::kFrenchAzerty,
      Layout::kGermanQwertz, Layout::kDvorak, Layout::kUnknown, Layout::kNeutral,
      static_cast<Layout>(999)};
  struct Neighbours {
    char expected;
    char typed;
    std::array<unsigned, 7> costs;
  };
  // Literal physical-key relationships, NOT generated from the production
  // geometry. z-e distinguishes AZERTY; z-t and y-x distinguish QWERTZ;
  // a-o and p-e distinguish Dvorak, whose punctuation columns must stay intact.
  const Neighbours examples[] = {
      {'r', 't', {{60, 60, 60, 60, 100, 100, 100}}},
      {'q', 'w', {{60, 60, 60, 100, 100, 100, 100}}},
      {'z', 'e', {{100, 60, 100, 100, 100, 100, 100}}},
      {'z', 't', {{100, 100, 60, 100, 100, 100, 100}}},
      {'y', 'x', {{100, 100, 60, 100, 100, 100, 100}}},
      {'a', 'o', {{100, 100, 100, 60, 100, 100, 100}}},
      {'p', 'e', {{100, 100, 100, 60, 100, 100, 100}}},
      {'g', 'h', {{60, 60, 60, 60, 100, 100, 100}}},
      {'m', 'l', {{100, 60, 100, 100, 100, 100, 100}}}};
  for (const auto& example : examples) {
    // A synthetic admitted dictionary word isolates one substitution. Prefix
    // and morphology features are disabled, and the middle byte cannot align
    // as insertion/deletion at the one-edit minimum.
    std::string surface = "xxxx";
    surface[1] = example.expected;
    std::string raw = "xxxx";
    raw[1] = example.typed;
    auto lexicon = Lexicon(surface + "\t1\n");
    if (!lexicon) continue;
    auto support = Support(*lexicon);
    if (!support) continue;
    for (std::size_t layout_ordinal = 0; layout_ordinal < layouts.size(); ++layout_ordinal) {
      auto options = Only(CandidateType::kSpelling);
      options.keyboard_layout = layouts[layout_ordinal];
      const auto result = support->Suggest(raw, {}, options);
      const auto* candidate = Find(result, surface);
      Check(candidate && candidate->edit_distance == 1,
            "layout change preserves one-edit admission and distance");
      if (!candidate) continue;
      const auto expected_cost = example.costs[layout_ordinal];
      Check(candidate->keyboard_cost == expected_cost,
            "selected layout matches literal physical-key neighbour oracle");
      Near(candidate->match_log_score, -4.0 - 0.005 * expected_cost,
           "layout spelling penalty exposed without count features");
      Near(candidate->score, candidate->match_log_score,
           "frequency-unavailable spelling has only input penalty");
      if (layout_ordinal == 0) {
        const auto legacy = support->Suggest(raw, {}, Only(CandidateType::kSpelling));
        Check(Find(legacy, surface) && Find(legacy, surface)->keyboard_cost == expected_cost,
              "default and explicit QWERTY costs agree");
      }
    }
  }

  auto keys = Lexicon("test\t1\ntess\t1\n");
  if (!keys) return;
  auto keyboard = Support(*keys);
  if (!keyboard) return;
  auto options = Only(CandidateType::kSpelling);
  const auto qwerty = keyboard->Suggest("tesr", {}, options);
  Check(!qwerty.candidates.empty() && qwerty.candidates.front().text == "test",
        "known-layout neighbour changes equal-frequency ordering");
  for (const auto layout : {Layout::kUnknown, Layout::kNeutral, static_cast<Layout>(999)}) {
    options.keyboard_layout = layout;
    const auto neutral = keyboard->Suggest("tesr", {}, options);
    Check(Texts(neutral) == std::vector<std::string>({"tess", "test"}),
          "unknown layout gives neutral tie order rather than QWERTY preference");
    for (const auto& candidate : neutral.candidates) {
      Check(candidate.keyboard_cost == 100, "unknown substitution has no neighbour discount");
      Near(candidate.match_log_score, -4.5, "neutral one-edit penalty literal oracle");
    }
    const auto swapped = keyboard->Suggest("tset", {}, options);
    const auto* test = Find(swapped, "test");
    Check(test && test->keyboard_cost == 80 && test->edit_distance == 1,
          "neutral keeps layout-independent adjacent-byte transposition");
  }
}

void ExpansionProtectionCompatibilityAndBudgets() {
  auto lexicon = Lexicon("be\t1\nright\t1\nback\t1\nhello\t10\nhelp\t8\ncat\t6\ndon't\t1\nplease\t1\n");
  if (!lexicon) return;
  auto support = Support(*lexicon);
  if (!support) return;
  const auto options = Only(CandidateType::kAbbreviation);
  struct Rule { const char* key; const char* output; };
  const Rule rules[] = {{"brb", "be right back"}, {"BRB", "BE RIGHT BACK"},
      {"Brb", "Be right back"}, {"asap", "as soon as possible"},
      {"pls", "please"}, {"dont", "don't"}, {"Dont", "Don't"},
      {"DONT", "DON'T"}, {"ive", "i've"}};
  for (const auto& rule : rules) {
    const auto result = support->Suggest(rule.key, {}, options);
    const auto* candidate = Find(result, rule.output);
    Check(candidate != nullptr, "literal project-authored expansion mapping");
    if (!candidate) continue;
    Check(candidate->segments_utf8_byte.size() == 1 &&
              candidate->segments_utf8_byte[0].end_utf8_byte == std::string_view(rule.key).size(),
          "expansion span is shorthand input bytes, never expanded output bytes");
    for (const auto id : candidate->lexeme_ids) {
      Check(id == 0 || support->HasLexeme(id), "unknown expansion words cannot mint persistent IDs");
    }
  }
  const auto brb = support->Suggest("brb", {}, options);
  Check(Find(brb, "be right back") && Find(brb, "be right back")->lexeme_ids ==
            std::vector<LexemeId>({StableLexemeId("be"), StableLexemeId("right"), StableLexemeId("back")}),
        "phrase expansion exposes each known output lexical ID");
  Check(Find(support->Suggest("dont", {}, options), "don't")->type == CandidateType::kContraction,
        "contraction distinguished from generic shorthand");
  for (const auto* text : {"br", "brbb", "unknown", "don't"}) {
    Check(support->Suggest(text, {}, options).candidates.empty(), "expansion must match whole key, not prefix or suffix");
  }
  const std::vector<std::pair<std::string, LanguageSupportOutcome>> gates = {
      {"", LanguageSupportOutcome::kEmptyInput},
      {std::string(65, 'a'), LanguageSupportOutcome::kInputTooLong},
      {"caf\xc3\xa9", LanguageSupportOutcome::kNonAscii},
      {"don\xe2\x80\x99t", LanguageSupportOutcome::kNonAscii},
      {"user@example.com", LanguageSupportOutcome::kProtectedText},
      {"C:\\temp\\file", LanguageSupportOutcome::kProtectedText},
      {"b2b", LanguageSupportOutcome::kProtectedText},
      {"hello_world", LanguageSupportOutcome::kProtectedText},
      {"hello world", LanguageSupportOutcome::kUnsupportedByte},
      {"hello!", LanguageSupportOutcome::kUnsupportedByte},
      {"'hello", LanguageSupportOutcome::kIllegalWordForm},
      {"rock'n'roll", LanguageSupportOutcome::kIllegalWordForm},
      {"don't-x", LanguageSupportOutcome::kIllegalWordForm},
      {"bRb", LanguageSupportOutcome::kMixedCase},
      {"HeL", LanguageSupportOutcome::kMixedCase},
  };
  for (const auto& gate : gates) {
    std::string raw = gate.first;
    const auto result = support->Suggest(raw);
    Check(raw == gate.first && result.outcome == gate.second && result.candidates.empty(),
          "full original input preserved on every legacy protection gate");
    Check(result.spelling_cells == 0 && result.segmentation_transitions == 0 && result.morphology_matches_examined == 0,
          "protected input never enters any algorithm");
  }
  const auto before_prefix = CompletePrefix(*lexicon, "hel", 16);
  const auto before_spelling = SuggestSpelling(lexicon.get(), "hxllo", 0, 16, true);
  const auto parallel = support->Suggest("hel");
  (void)parallel;
  const auto after_prefix = CompletePrefix(*lexicon, "hel", 16);
  const auto after_spelling = SuggestSpelling(lexicon.get(), "hxllo", 0, 16, true);
  Check(before_prefix.completions.size() == after_prefix.completions.size() &&
            before_prefix.completions[0].display == "hello" &&
            before_prefix.completions[0].weight == 10,
        "new pipeline leaves old prefix attestation ordering untouched");
  Check(before_spelling.suggestions.size() == after_spelling.suggestions.size() &&
            before_spelling.suggestions[0].display == "hello", "legacy one-edit interface remains unchanged");
  auto none = Only(CandidateType::kMorphology);
  none.max_candidates = 0;
  const auto no_candidates = support->Suggest("cat", {}, none);
  Check(no_candidates.outcome == LanguageSupportOutcome::kQueried && no_candidates.candidates.empty() &&
            no_candidates.morphology_matches_examined == 0, "zero output budget reports gate without hidden search");
  none = Only(CandidateType::kMorphology);
  none.morphology_match_budget = 1;
  const auto morph_budget = support->Suggest("cat", {}, none);
  Check(morph_budget.morphology == LanguageFeatureStatus::kBudgetExhausted &&
            morph_budget.morphology_matches_examined == 1, "morphology match budget checked before work");
  none = Only(CandidateType::kMorphology);
  none.max_candidates = 1;
  Check(support->Suggest("cat", {}, none).candidates.size() == 1, "global candidate cap applies after deduplication");
  none.enable_morphology = false;
  Check(support->Suggest("cat", {}, none).candidates.empty(), "feature flags actually turn algorithms off");
}

int Finish() {
  std::cout << "[english-language-support] " << checked << " checked, " << failed << " failed\n";
  if (failed != 0) { std::cout << "RESULT FAIL\n"; return 1; }
  std::cout << "RESULT PASS\n";
  return 0;
}
int PilotBudget(const std::filesystem::path& path) {
  LexiconLoadInfo load;
  auto lexicon = EnglishLexicon::FromFile(path, &load);
  Check(lexicon != nullptr && lexicon->size() > 40000, "real offline pilot fixture has expected scale");
  if (!lexicon) return Finish();
  const auto start = std::chrono::steady_clock::now();
  LanguageSupportBuildInfo build;
  auto support = LanguageSupport::Create(*lexicon, {}, &build);
  const auto built = std::chrono::steady_clock::now();
  Check(support != nullptr, "pilot-scale immutable index built once");
  if (!support) return Finish();
  std::size_t total_cells = 0;
  for (const auto* input : {"helo", "hxllo", "helloworld", "qwertyuiopasdfghjklzxcvbnmqwertyu", "zzzzzzzz", "brb"}) {
    const auto result = support->Suggest(input);
    Check(result.candidates.size() <= 16 && result.spelling_cells <= 60000 &&
              result.morphology_matches_examined <= 512 && result.segmentation_transitions == 0,
          "pilot query obeys every configured work cap");
    Check(result.frequency_source == FrequencySource::kUnavailable && !result.production_frequency_supplied,
          "pilot attestation data is NOT a production frequency corpus");
    std::set<std::string> texts;
    for (const auto& candidate : result.candidates) {
      Check(texts.insert(candidate.text).second && candidate.text != input && std::isfinite(candidate.score),
            "pilot candidates are unique finite suggestions, not original replacements");
    }
    total_cells += result.spelling_cells;
  }
  const auto finished = std::chrono::steady_clock::now();
  const auto build_ms = std::chrono::duration_cast<std::chrono::milliseconds>(built - start).count();
  const auto query_ms = std::chrono::duration_cast<std::chrono::milliseconds>(finished - built).count();
  std::cout << "pilot lexemes=" << build.lexeme_count << " generated_forms=" << build.generated_form_count
            << " build_ms=" << build_ms << " six_queries_ms=" << query_ms << " spelling_cells=" << total_cells << '\n';
  return Finish();
}

int RunCapability(std::string_view capability) {
  // Each mode runs only the assertions for that capability. The release gate
  // requires a separately executed case per capability, not one aggregate run
  // relabelled seven times.
  if (capability == "frequency") {
    StableIdsAndFrequencyValidation();
  } else if (capability == "context-learning") {
    LearningAndPrediction();
  } else if (capability == "morphology") {
    MorphologyLiteralOracle();
    ExactLemmaBeatsCompetingPrefixes();
    AttestedMorphologyIdentities();
  } else if (capability == "spelling") {
    TwoEditSpellingAndKeyboard();
    LayoutAwareSpelling();
  } else if (capability == "segmentation") {
    SegmentationOracleAndBoundaries();
  } else if (capability == "abbreviation") {
    ExpansionProtectionCompatibilityAndBudgets();
  } else {
    std::cerr << "unknown capability: " << capability << '\n';
    return 2;
  }
  return Finish();
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--forcefail") {
    // An intentionally wrong morphological oracle must fail the same assertion
    // path as positive tests, with EXACT exit 1 and no success marker.
    Check(FormTexts("child", kPlural) == std::set<std::string>{"childs"},
          "injected incorrect morphology oracle must fail");
    return Finish();
  }
  if (argc == 3 && std::string_view(argv[1]) == "--pilot-budget") {
    return PilotBudget(std::filesystem::u8path(argv[2]));
  }
  if (argc == 3 && std::string_view(argv[1]) == "--capability") {
    return RunCapability(argv[2]);
  }
  if (argc != 2 || std::string_view(argv[1]) != "--unit") {
    std::cerr << "usage: epocanva_english_language_support_tests "
                 "--unit|--capability <frequency|context-learning|morphology|spelling|"
                 "segmentation|abbreviation>|--forcefail|--pilot-budget <path>\n";
    return 2;
  }
  StableIdsAndFrequencyValidation();
  MorphologyLiteralOracle();
  ExactLemmaBeatsCompetingPrefixes();
  AttestedMorphologyIdentities();
  LearningAndPrediction();
  SegmentationOracleAndBoundaries();
  TwoEditSpellingAndKeyboard();
  LayoutAwareSpelling();
  ExpansionProtectionCompatibilityAndBudgets();
  return Finish();
}
