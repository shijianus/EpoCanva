#include "engine/prediction/model.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace p = epocanva::prediction;
namespace {

std::size_t checks = 0;
void Check(bool value, const char* expression, int line) {
  ++checks;
  if (!value) throw std::runtime_error(std::string("check failed line ") +
                                     std::to_string(line) + ": " + expression);
}
#define CHECK(value) Check((value), #value, __LINE__)
bool Near(double a, double b, double tolerance = 3e-13) {
  return std::fabs(a - b) <= tolerance * (1.0 + std::fabs(a) + std::fabs(b));
}

p::LexemeId Id(const std::string& text, const std::string& language = "en") {
  return p::StableLexemeId(language, text);
}
p::ModelRecords Blank() {
  p::ModelRecords records;
  records.metadata.source = p::SourceKind::kTestFixture;
  records.metadata.source_fingerprint.assign(64, 'a');
  return records;
}
void Totals(p::ModelRecords* records) {
  records->metadata.lexeme_count = records->lexicon.size();
  records->metadata.unigram_total = 0;
  records->metadata.bigram_total = 0;
  records->metadata.trigram_total = 0;
  for (const auto& row : records->unigrams) records->metadata.unigram_total += row.count;
  for (const auto& row : records->bigrams) records->metadata.bigram_total += row.count;
  for (const auto& row : records->trigrams) records->metadata.trigram_total += row.count;
}
void Lexeme(p::ModelRecords* records, std::string text, std::string language = "en",
            p::TokenKind kind = p::TokenKind::kWord) {
  records->lexicon.push_back({Id(text, language), std::move(language), std::move(text), kind});
}
struct Corpus {
  p::ModelRecords records = Blank();
  std::map<p::LexemeId, std::uint64_t> unigrams;
  std::map<std::pair<p::LexemeId, p::LexemeId>, std::uint64_t> bigrams;
  std::map<std::tuple<p::LexemeId, p::LexemeId, p::LexemeId>, std::uint64_t> trigrams;
  void Sentence(std::vector<p::LexemeId> tokens, std::uint64_t repetitions, bool eos = true) {
    if (eos) tokens.push_back(0);
    for (std::size_t token = 0; token < tokens.size(); ++token) {
      unigrams[tokens[token]] += repetitions;
      if (token >= 1) bigrams[{tokens[token - 1], tokens[token]}] += repetitions;
      if (token >= 2) trigrams[{tokens[token - 2], tokens[token - 1], tokens[token]}] += repetitions;
    }
  }
  p::ModelRecords Finish() {
    for (const auto& row : unigrams) records.unigrams.push_back({row.first, row.second});
    for (const auto& row : bigrams) records.bigrams.push_back({row.first.first, row.first.second, row.second});
    for (const auto& row : trigrams)
      records.trigrams.push_back({std::get<0>(row.first), std::get<1>(row.first), std::get<2>(row.first), row.second});
    Totals(&records);
    return records;
  }
};
p::ModelRecords Fixture() {
  Corpus corpus;
  for (const auto& text : {"i", "you", "like", "love", "tea", "coffee", "unseen"})
    Lexeme(&corpus.records, text);
  Lexeme(&corpus.records, ".", "en", p::TokenKind::kPunctuation);
  Lexeme(&corpus.records, u8"café", "fr");
  Lexeme(&corpus.records, u8"你好", "zh");
  Lexeme(&corpus.records, u8"世界", "ja");
  Lexeme(&corpus.records, u8"안녕", "ko");
  corpus.Sentence({Id("i"), Id("like"), Id("tea"), Id(".")}, 30);
  corpus.Sentence({Id("you"), Id("like"), Id("coffee"), Id(".")}, 20);
  corpus.Sentence({Id("i"), Id("love"), Id("coffee"), Id(".")}, 5);
  corpus.Sentence({Id("you"), Id("love"), Id("tea"), Id(".")}, 7);
  corpus.Sentence({Id(u8"café", "fr"), Id(u8"你好", "zh"), Id(u8"世界", "ja"), Id(u8"안녕", "ko")}, 3);
  return corpus.Finish();
}
std::shared_ptr<const p::Model> Build(const p::ModelRecords& records) {
  p::BuildInfo info;
  auto model = p::Model::FromRecords(records, &info);
  if (!model) throw std::runtime_error(std::string("fixture invalid: ") + p::StatusName(info.status));
  CHECK(info.status == p::Status::kOk);
  CHECK(model->available());
  return model;
}

// Independent oracle: direct linear scans of the input records and literal
// probability equations, no model Score(), index data, or production helper.
double Oracle(const p::ModelRecords& data, p::Context context, p::LexemeId id,
              const p::PersonalCounts* personal = nullptr) {
  const auto known = [&](p::LexemeId wanted) {
    return std::any_of(data.lexicon.begin(), data.lexicon.end(),
                       [&](const auto& row) { return row.id == wanted; });
  };
  p::Context suffix;
  for (const auto token : context) {
    if (token == 0 || !known(token)) suffix.clear();
    else suffix.push_back(token);
  }
  std::uint64_t count = 0;
  for (const auto& row : data.unigrams) if (row.id == id) count = row.count;
  double value = (static_cast<double>(count) + 1.0) /
      (static_cast<double>(data.metadata.unigram_total) + data.lexicon.size() + 1.0);
  const auto bigram = [&](const std::vector<p::BigramRecord>& rows, double prior, double lower) {
    std::uint64_t numerator = 0, total = 0;
    for (const auto& row : rows) {
      if (row.previous_id == suffix.back()) {
        total += row.count;
        if (row.id == id) numerator = row.count;
      }
    }
    return (static_cast<double>(numerator) + prior * lower) / (static_cast<double>(total) + prior);
  };
  const auto trigram = [&](const std::vector<p::TrigramRecord>& rows, double prior, double lower) {
    std::uint64_t numerator = 0, total = 0;
    for (const auto& row : rows) {
      if (row.previous2_id == suffix[0] && row.previous1_id == suffix[1]) {
        total += row.count;
        if (row.id == id) numerator = row.count;
      }
    }
    return (static_cast<double>(numerator) + prior * lower) / (static_cast<double>(total) + prior);
  };
  if (!suffix.empty()) value = bigram(data.bigrams, 8.0, value);
  if (suffix.size() == 2) value = trigram(data.trigrams, 4.0, value);
  if (personal) {
    std::uint64_t numerator = 0, total = 0;
    for (const auto& row : personal->unigrams) {
      total += row.count;
      if (row.id == id) numerator = row.count;
    }
    value = (static_cast<double>(numerator) + 16.0 * value) / (static_cast<double>(total) + 16.0);
    if (!suffix.empty()) value = bigram(personal->bigrams, 16.0, value);
    if (suffix.size() == 2) value = trigram(personal->trigrams, 16.0, value);
  }
  return value;
}
std::vector<std::pair<p::LexemeId, double>> OracleOrder(
    const p::ModelRecords& data, const p::Context& context,
    const p::PersonalCounts* personal = nullptr, std::string_view prefix = {},
    std::string_view language = {}) {
  std::vector<std::pair<p::LexemeId, double>> results;
  for (const auto& row : data.lexicon) {
    if (!language.empty() && language != row.language_id) continue;
    if (row.normalized_utf8.compare(0, prefix.size(), prefix) != 0) continue;
    results.push_back({row.id, Oracle(data, context, row.id, personal)});
  }
  std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) {
    return a.second != b.second ? a.second > b.second : a.first < b.first;
  });
  return results;
}
void CheckOrder(const p::ModelRecords& data, const p::Model& model,
                const p::Context& context, const p::PersonalCounts* personal,
                std::string_view prefix, std::string language, std::size_t wanted) {
  p::PredictOptions options;
  options.max_suggestions = wanted;
  options.language_id = language;
  auto expected = OracleOrder(data, context, personal, prefix, language);
  if (expected.size() > wanted) expected.resize(wanted);
  const auto result = model.Predict(prefix, context, options, personal);
  CHECK(result.status == p::Status::kOk);
  CHECK(result.complete);
  CHECK(!result.budget_exhausted && !result.beam_pruned);
  CHECK(result.personal_preparations == (personal ? 1u : 0u));
  CHECK(result.suggestions.size() == expected.size());
  for (std::size_t candidate = 0; candidate < expected.size(); ++candidate) {
    CHECK(result.suggestions[candidate].lexeme_ids == p::Context{expected[candidate].first});
    CHECK(Near(result.suggestions[candidate].log_probability, std::log(expected[candidate].second)));
    CHECK(result.suggestions[candidate].tokens_utf8.size() == 1);
  }
}

void TestIdentityUtf8(const std::filesystem::path& fixtures) {
  std::ifstream answers(fixtures / "identity-answers.txt", std::ios::binary);
  CHECK(static_cast<bool>(answers));
  std::string line;
  std::getline(answers, line);
  std::getline(answers, line);
  while (std::getline(answers, line)) {
    const auto first_byte = line.find('\t'), last_byte = line.rfind('\t');
    CHECK(first_byte != last_byte);
    CHECK(p::StableLexemeId(line.substr(0, first_byte),
                           line.substr(first_byte + 1, last_byte - first_byte - 1)) ==
          static_cast<p::LexemeId>(std::stoull(line.substr(last_byte + 1))));
  }
  CHECK(Id("same", "en") != Id("same", "fr"));
  CHECK(Id("HELLO") == 0);
  CHECK(Id("") == 0);
  CHECK(Id("hello", "EN") == 0);
  CHECK(p::IsValidLanguageId("zh-hant-tw"));
  CHECK(p::IsValidLanguageId("en-us-001"));
  CHECK(!p::IsValidLanguageId("e"));
  CHECK(!p::IsValidLanguageId("en-"));
  CHECK(!p::IsValidLanguageId("en--us"));
  CHECK(!p::IsValidLanguageId("englishxx"));
  CHECK(!p::IsValidLanguageId("en/evil"));
  std::string normalized;
  CHECK(p::NormalizeLexemeUtf8(u8"CAFÉ", &normalized));
  CHECK(normalized == u8"cafÉ");  // Explicitly NOT Unicode casefold.
  CHECK(p::NormalizeLexemeUtf8(normalized, &normalized));
  CHECK(normalized == u8"cafÉ");
  CHECK(Id(u8"é", "fr") != Id(u8"e\u0301", "fr"));  // Explicitly NOT NFC.
  CHECK(p::NormalizeLexemeUtf8("l'homme", &normalized));
  CHECK(p::NormalizeLexemeUtf8("well-known", &normalized));
  CHECK(p::IsValidUtf8(u8"你好é세계😀"));
  CHECK(p::IsValidUtf8(""));
  const std::vector<std::string> malformed = {
      "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80",
      "\xf5\x80\x80\x80", "\x80", "\xc2", "\xe2\x82", "\xf0\x9f\x98",
      "\xe2(\xa1", "\xfe", "\xff"};
  for (const auto& text : malformed) {
    CHECK(!p::IsValidUtf8(text));
    CHECK(!p::NormalizeLexemeUtf8(text, &normalized));
    CHECK(normalized.empty());
    CHECK(Id(text) == 0);
  }
  for (const std::string text : {"a b", "a\tb", "a\nb", u8"a\u00a0b", u8"a\u1680b",
                                 u8"a\u2000b", u8"a\u2028b", u8"a\u202fb", u8"a\u205fb",
                                 u8"a\u3000b", "\x7f", "\xc2\x85"})
    CHECK(!p::NormalizeLexemeUtf8(text, &normalized));
  CHECK(!p::NormalizeLexemeUtf8(std::string("a\0b", 3), &normalized));
  CHECK(p::NormalizeLexemeUtf8(std::string(256, 'a'), &normalized));
  CHECK(!p::NormalizeLexemeUtf8(std::string(257, 'a'), &normalized));
  CHECK(!p::NormalizeLexemeUtf8("x", nullptr));
  std::cout << "identity/UTF8 strict scalar and normalization contract verified\n";
}

void TestStaticAndOracle() {
  auto records = Fixture();
  const auto model = Build(records);
  CHECK(model->lexemes().size() == records.lexicon.size());
  CHECK(std::is_sorted(model->lexemes().begin(), model->lexemes().end(),
                      [](const auto& a, const auto& b) { return a.id < b.id; }));
  CHECK(model->Find("en", "tea") && model->Find("en", "tea")->id == Id("tea"));
  CHECK(!model->Find("fr", "tea"));
  CHECK(!model->Find("en", "TEA"));
  CHECK(!model->Find(0));
  CHECK(model->UnigramCount(Id("tea")) == 37);
  CHECK(model->UnigramCount(Id("unseen")) == 0);
  CHECK(model->UnigramCount(1) == 0);
  const std::vector<p::Context> contexts = {{}, {Id("like")}, {Id("i"), Id("like")},
      {Id("you"), Id("like")}, {Id("tea")}, {Id("unseen")}, {1},
      {1, Id("like")}, {Id("like"), 1}, {0, Id("like")}, {Id("i"), 0}};
  for (const auto& context : contexts) {
    double probability_sum = 0;
    for (const auto& row : records.lexicon) {
      const auto score = model->Score(context, row.id);
      CHECK(score.status == p::Status::kOk);
      CHECK(Near(score.static_probability, Oracle(records, context, row.id)));
      CHECK(Near(score.probability, score.static_probability));
      CHECK(!score.personal_applied);
      CHECK(Near(score.log_probability, std::log(score.probability)));
      probability_sum += score.probability;
    }
    probability_sum += model->Score(context, 0).probability;
    CHECK(Near(probability_sum, 1.0));
    for (const auto wanted : {1u, 3u, 64u}) CheckOrder(records, *model, context, nullptr, {}, {}, wanted);
    CheckOrder(records, *model, context, nullptr, "co", "en", 8);
    CheckOrder(records, *model, context, nullptr, u8"你", "zh", 8);
  }
  const auto base = model->Score({}, Id("tea"));
  const auto contextual = model->Score({Id("i"), Id("like")}, Id("tea"), nullptr);
  CHECK(contextual.probability > base.probability);
  CHECK(!contextual.personal_applied);
  CHECK(model->Predict(std::nullopt, {Id("i"), Id("like")}).suggestions.front().lexeme_ids.front() == Id("tea"));
  CHECK(model->Predict(std::nullopt, {Id("you"), Id("like")}).suggestions.front().lexeme_ids.front() == Id("coffee"));
  CHECK(Near(model->Score({1, Id("like")}, Id("tea")).probability,
             model->Score({Id("like")}, Id("tea")).probability));
  CHECK(Near(model->Score({Id("like"), 1}, Id("tea")).probability, base.probability));
  CHECK(model->Score({1}, Id("tea")).context_backed_off);
  CHECK(model->Score({Id("unseen")}, Id("tea")).context_backed_off);
  CHECK(model->Score({}, Id("unseen")).probability > 0);
  CHECK(model->Score({}, 1).status == p::Status::kUnknownId);
  CHECK(model->Score({Id("i"), Id("like"), Id("tea")}, Id("tea")).status == p::Status::kInvalidContext);
  const auto before = model->Predict(std::nullopt, {});
  std::mt19937 random(113);
  std::shuffle(records.lexicon.begin(), records.lexicon.end(), random);
  std::shuffle(records.unigrams.begin(), records.unigrams.end(), random);
  std::shuffle(records.bigrams.begin(), records.bigrams.end(), random);
  std::shuffle(records.trigrams.begin(), records.trigrams.end(), random);
  const auto reordered = Build(records);
  const auto after = reordered->Predict(std::nullopt, {});
  CHECK(before.suggestions.size() == after.suggestions.size());
  for (std::size_t entry = 0; entry < before.suggestions.size(); ++entry) {
    CHECK(before.suggestions[entry].lexeme_ids == after.suggestions[entry].lexeme_ids);
    CHECK(before.suggestions[entry].log_probability == after.suggestions[entry].log_probability);
  }
  records.lexicon.front().normalized_utf8 = "mutated-caller-storage";
  CHECK(model->Find("en", "tea") != nullptr);
  CHECK(model->Predict("CO", {}).suggestions.front().tokens_utf8.front() == "coffee");
  CHECK(model->Predict("notfound", {}).complete);
  CHECK(model->Predict("notfound", {}).suggestions.empty());
  CHECK(model->Predict("\xe4\xbd", {}).status == p::Status::kInvalidUtf8);
  CHECK(model->Predict("contains space", {}).status == p::Status::kInvalidLexeme);
  CHECK(model->Predict(std::nullopt, {1, 2, 3}).status == p::Status::kInvalidContext);
  auto options = p::PredictOptions{};
  options.max_suggestions = 0;
  CHECK(model->Predict(std::nullopt, {}, options).complete);
  CHECK(model->Predict(std::nullopt, {}, options).index_entries_visited == 0);
  options.max_suggestions = 65;
  CHECK(model->Predict(std::nullopt, {}, options).status == p::Status::kInvalidOptions);
  options = {}; options.max_words = 5;
  CHECK(model->Predict(std::nullopt, {}, options).status == p::Status::kInvalidOptions);
  options.max_words = 0;
  CHECK(model->Predict(std::nullopt, {}, options).status == p::Status::kInvalidOptions);
  options = {}; options.beam_width = 65;
  CHECK(model->Predict(std::nullopt, {}, options).status == p::Status::kInvalidOptions);
  options.beam_width = 0;
  CHECK(model->Predict(std::nullopt, {}, options).status == p::Status::kInvalidOptions);
  options = {}; options.language_id = "EN";
  CHECK(model->Predict(std::nullopt, {}, options).status == p::Status::kInvalidOptions);
  auto unavailable = p::Model::Unavailable();
  CHECK(!unavailable->available());
  CHECK(unavailable->lexemes().empty());
  CHECK(unavailable->Score({}, 0).status == p::Status::kModelUnavailable);
  CHECK(unavailable->Predict(std::nullopt, {}).status == p::Status::kModelUnavailable);
  CHECK(!unavailable->Predict(std::nullopt, {}).complete);
  std::cout << "static context, sparse/unseen fallback, deterministic oracle order verified\n";
}

void TestPersonalAndRandomOracle() {
  const auto records = Fixture();
  const auto model = Build(records);
  p::PersonalCounts personal;
  personal.unigrams = {{Id("coffee"), 100}, {Id("tea"), 1}};
  personal.bigrams = {{Id("like"), Id("coffee"), 50}, {Id("like"), Id("tea"), 2}};
  personal.trigrams = {{Id("i"), Id("like"), Id("coffee"), 25}};
  const p::Context context{Id("i"), Id("like")};
  CheckOrder(records, *model, context, &personal, {}, {}, 64);
  for (const auto& row : records.lexicon) {
    const auto score = model->Score(context, row.id, &personal);
    CHECK(score.status == p::Status::kOk);
    CHECK(score.personal_applied);
    CHECK(Near(score.probability, Oracle(records, context, row.id, &personal)));
    CHECK(Near(score.static_probability, Oracle(records, context, row.id)));
  }
  const auto baseline = model->Predict(std::nullopt, context);
  CHECK(baseline.suggestions.front().lexeme_ids.front() == Id("tea"));
  CHECK(model->Predict(std::nullopt, context, {}, &personal).suggestions.front().lexeme_ids.front() == Id("coffee"));
  CHECK(model->Predict(std::nullopt, context).suggestions.front().lexeme_ids == baseline.suggestions.front().lexeme_ids);
  auto invalid = personal;
  invalid.unigrams.push_back(invalid.unigrams.front());
  CHECK(model->Predict(std::nullopt, context, {}, &invalid).status == p::Status::kInvalidPersonalCounts);
  CHECK(model->Score(context, Id("tea"), &invalid).status == p::Status::kInvalidPersonalCounts);
  CHECK(model->Predict(std::nullopt, context, {}, &invalid).suggestions.empty());
  invalid = {}; invalid.unigrams = {{1, 1}};
  CHECK(model->Predict(std::nullopt, context, {}, &invalid).status == p::Status::kInvalidPersonalCounts);
  invalid = {}; invalid.unigrams = {{Id("tea"), 0}};
  CHECK(model->Score({}, Id("tea"), &invalid).status == p::Status::kInvalidPersonalCounts);
  invalid = {}; invalid.bigrams = {{0, Id("tea"), 1}};
  CHECK(model->Predict(std::nullopt, context, {}, &invalid).status == p::Status::kInvalidPersonalCounts);
  invalid = {}; invalid.unigrams = {{Id("tea"), (std::numeric_limits<std::uint64_t>::max)()}, {Id("coffee"), 1}};
  CHECK(model->Score({}, Id("tea"), &invalid).status == p::Status::kInvalidPersonalCounts);
  invalid = {}; invalid.unigrams.resize(p::kMaxPersonalRecords + 1, {Id("tea"), 1});
  CHECK(model->Predict(std::nullopt, context, {}, &invalid).status == p::Status::kInvalidPersonalCounts);
  p::PersonalCounts empty;
  CHECK(Near(model->Score(context, Id("tea"), &empty).probability,
             model->Score(context, Id("tea")).probability));
  CHECK(!model->Score(context, Id("tea"), &empty).personal_applied);

  std::mt19937 random(90127);
  for (unsigned trial = 0; trial < 12; ++trial) {
    Corpus corpus;
    std::vector<p::LexemeId> ids;
    for (unsigned word = 0; word < 35; ++word) {
      const auto token = std::string(word % 2 ? "alpha" : "beta") + std::to_string(word);
      Lexeme(&corpus.records, token, word % 3 ? "en" : "fr");
      ids.push_back(corpus.records.lexicon.back().id);
    }
    for (unsigned sentence = 0; sentence < 150; ++sentence) {
      std::vector<p::LexemeId> words;
      for (unsigned word = 0; word < 3 + sentence % 3; ++word) words.push_back(ids[random() % ids.size()]);
      corpus.Sentence(words, 1 + random() % 30);
    }
    auto data = corpus.Finish();
    const auto generated = Build(data);
    p::PersonalCounts counts;
    counts.unigrams = {{ids[3], 31}, {ids[17], 99}, {ids[28], 13}};
    const p::Context ctx{ids[trial], ids[trial + 1]};
    counts.bigrams = {{ctx.back(), ids[2], 25}, {ctx.back(), ids[3], 32}};
    counts.trigrams = {{ctx.front(), ctx.back(), ids[4], 56}};
    for (const auto* learned : {static_cast<const p::PersonalCounts*>(nullptr),
                                static_cast<const p::PersonalCounts*>(&counts)}) {
      for (const auto wanted : {1u, 7u, 35u}) {
        CheckOrder(data, *generated, ctx, learned, {}, {}, wanted);
        CheckOrder(data, *generated, ctx, learned, "alpha", "en", wanted);
      }
    }
  }
  std::cout << "personal separation and seeded exhaustive differential ranking verified\n";
}

void TestRankedBeyond4096AndBudget() {
  auto records = Blank();
  for (unsigned ordinal = 0; ordinal < 6000; ++ordinal) {
    std::ostringstream name;
    name << 'a'; name.width(5); name.fill('0'); name << ordinal;
    Lexeme(&records, name.str());
    records.unigrams.push_back({records.lexicon.back().id, 1});
  }
  Lexeme(&records, "zzzz-global-winner");
  const auto global = records.lexicon.back().id;
  records.unigrams.push_back({global, 1000000});
  Lexeme(&records, "zzzz-context-winner");
  const auto successor = records.lexicon.back().id;
  records.unigrams.push_back({successor, 20000});
  const auto history = records.lexicon.front().id;
  records.unigrams.front().count = 20000;
  records.bigrams.push_back({history, successor, 20000});
  records.metadata.source_fingerprint.assign(64, 'b');
  Totals(&records);
  const auto model = Build(records);
  p::PredictOptions options;
  options.max_suggestions = 1;
  const auto base = model->Predict(std::nullopt, {}, options);
  CHECK(base.complete);
  CHECK(base.suggestions.front().lexeme_ids.front() == global);
  CHECK(base.index_entries_visited < 4096);
  const auto next = model->Predict(std::nullopt, {history}, options);
  CHECK(next.complete);
  CHECK(next.suggestions.front().lexeme_ids.front() == successor);
  CHECK(next.index_entries_visited < 4096);
  CheckOrder(records, *model, {history}, nullptr, "zzzz", {}, 2);
  p::PersonalCounts personal;
  personal.unigrams = {{records.lexicon[5999].id, 10000000}};
  const auto personalized = model->Predict(std::nullopt, {}, options, &personal);
  CHECK(personalized.complete);
  CHECK(personalized.suggestions.front().lexeme_ids.front() == records.lexicon[5999].id);
  CHECK(personalized.index_entries_visited < 4096);
  options.max_suggestions = 64;
  options.max_index_entries = 1;
  const auto limited = model->Predict(std::nullopt, {}, options);
  CHECK(limited.status == p::Status::kBudgetExhausted);
  CHECK(limited.budget_exhausted && !limited.complete);
  CHECK(limited.index_entries_visited == 1);
  CHECK(limited.suggestions.size() == 1);
  options.max_suggestions = 1;
  const auto exact_with_budget = model->Predict(std::nullopt, {}, options);
  CHECK(exact_with_budget.complete);  // Bound proves the winner after one entry.
  CHECK(!exact_with_budget.budget_exhausted);
  options.max_suggestions = 64;
  const auto filtered_limited = model->Predict("no-match", {}, options);
  CHECK(filtered_limited.budget_exhausted);
  CHECK(filtered_limited.suggestions.empty());
  options.max_index_entries = 0;
  const auto exhaustive = model->Predict(std::nullopt, {}, options);
  CHECK(exhaustive.suggestions.size() == 64 && exhaustive.complete);
  std::cout << "6002-row ranked successor/global search and exact/incomplete budgets verified\n";
}

struct OraclePath { p::Context ids; double score; bool terminal; bool eos; };
void EnumeratePaths(const p::ModelRecords& records, p::Context context, p::Context ids,
                    double score, std::size_t max_words, std::vector<OraclePath>* out) {
  if (ids.size() == max_words) { out->push_back({ids, score, false, false}); return; }
  if (!ids.empty()) out->push_back({ids, score + std::log(Oracle(records, context, 0)), true, true});
  for (const auto& row : records.lexicon) {
    auto next_ids = ids;
    next_ids.push_back(row.id);
    const auto next_score = score + std::log(Oracle(records, context, row.id));
    if (row.kind == p::TokenKind::kPunctuation) {
      out->push_back({next_ids, next_score, true, false});
    } else {
      auto next_context = context;
      if (next_context.size() == 2) next_context.erase(next_context.begin());
      next_context.push_back(row.id);
      EnumeratePaths(records, next_context, next_ids, next_score, max_words, out);
    }
  }
}
void TestContinuation() {
  Corpus corpus;
  Lexeme(&corpus.records, "a"); Lexeme(&corpus.records, "b");
  Lexeme(&corpus.records, ".", "en", p::TokenKind::kPunctuation);
  corpus.Sentence({Id("a"), Id("b"), Id(".")}, 20);
  corpus.Sentence({Id("b"), Id("a")}, 10);
  const auto records = corpus.Finish();
  const auto model = Build(records);
  p::PredictOptions options;
  options.max_suggestions = 64; options.max_words = 3; options.beam_width = 64;
  const auto result = model->Predict(std::nullopt, {}, options);
  CHECK(result.complete && result.status == p::Status::kOk);
  std::vector<OraclePath> paths;
  EnumeratePaths(records, {}, {}, 0.0, 3, &paths);
  std::sort(paths.begin(), paths.end(), [](const auto& a, const auto& b) {
    if (a.score != b.score) return a.score > b.score;
    if (a.ids != b.ids) return a.ids < b.ids;
    return a.eos < b.eos;
  });
  CHECK(paths.size() == result.suggestions.size());
  bool saw_eos = false, saw_punctuation = false;
  for (std::size_t ordinal = 0; ordinal < paths.size(); ++ordinal) {
    const auto& actual = result.suggestions[ordinal];
    CHECK(actual.lexeme_ids == paths[ordinal].ids);
    CHECK(Near(actual.log_probability, paths[ordinal].score));
    CHECK(actual.terminal == paths[ordinal].terminal);
    CHECK(actual.ended_with_eos == paths[ordinal].eos);
    CHECK(!actual.lexeme_ids.empty() && actual.lexeme_ids.size() <= 3);
    CHECK(std::find(actual.lexeme_ids.begin(), actual.lexeme_ids.end(), 0) == actual.lexeme_ids.end());
    const auto punctuation = std::find(actual.lexeme_ids.begin(), actual.lexeme_ids.end(), Id("."));
    if (punctuation != actual.lexeme_ids.end()) {
      CHECK(punctuation + 1 == actual.lexeme_ids.end());
      CHECK(actual.terminal);
      saw_punctuation = true;
    }
    saw_eos = saw_eos || actual.ended_with_eos;
  }
  CHECK(saw_eos && saw_punctuation);
  options.max_words = 4; options.beam_width = 1;
  const auto pruned = model->Predict(std::nullopt, {}, options);
  CHECK(!pruned.complete && pruned.beam_pruned);
  CHECK(pruned.status == p::Status::kBeamPruned);
  CHECK(pruned.suggestions.size() <= 1);
  for (const auto& candidate : pruned.suggestions) CHECK(candidate.lexeme_ids.size() <= 4);
  options.beam_width = 8; options.max_index_entries = 3;
  const auto budget = model->Predict(std::nullopt, {}, options);
  CHECK(budget.index_entries_visited <= 3);
  CHECK(budget.budget_exhausted && !budget.complete);
  p::PersonalCounts counts;
  counts.unigrams = {{Id("a"), 2}};
  options.max_index_entries = 0;
  const auto prepared = model->Predict("a", {}, options, &counts);
  CHECK(prepared.personal_preparations == 1);
  for (const auto& candidate : prepared.suggestions) {
    CHECK(candidate.lexeme_ids.front() == Id("a"));
    CHECK(candidate.lexeme_ids.size() <= 4);
  }
  std::cout << "bounded continuation independent enumeration, EOS/punctuation, beam flags verified\n";
}

void ExpectBuild(p::ModelRecords records, p::Status expected) {
  p::BuildInfo info;
  CHECK(!p::Model::FromRecords(records, &info));
  CHECK(info.status == expected);
}
void TestValidation(const std::filesystem::path& fixtures) {
  const auto source = Fixture();
  auto changed = source;
  changed.metadata.version = 2; ExpectBuild(changed, p::Status::kUnsupportedVersion);
  changed = source; changed.metadata.stable_id_version = 2; ExpectBuild(changed, p::Status::kUnsupportedVersion);
  changed = source; changed.metadata.normalization_id = "nfc"; ExpectBuild(changed, p::Status::kUnsupportedVersion);
  changed = source; changed.metadata.source = p::SourceKind::kUnavailable; ExpectBuild(changed, p::Status::kModelUnavailable);
  changed = source; changed.metadata.source_fingerprint = "none"; ExpectBuild(changed, p::Status::kInvalidFormat);
  changed = source; changed.metadata.lexeme_count++; ExpectBuild(changed, p::Status::kInconsistentTotals);
  changed = source; changed.metadata.unigram_total++; ExpectBuild(changed, p::Status::kInconsistentTotals);
  changed = source; changed.lexicon.push_back(changed.lexicon.front()); Totals(&changed);
  ExpectBuild(changed, p::Status::kDuplicateRecord);
  changed = source; changed.lexicon.front().id = 42; ExpectBuild(changed, p::Status::kInvalidLexeme);
  changed = source; changed.lexicon.front().normalized_utf8 = "\xed\xa0\x80"; ExpectBuild(changed, p::Status::kInvalidUtf8);
  changed = source; changed.lexicon.front().language_id = "EN"; ExpectBuild(changed, p::Status::kInvalidLexeme);
  changed = source; changed.lexicon.front().normalized_utf8 = "UPPER"; ExpectBuild(changed, p::Status::kInvalidLexeme);
  changed = source; changed.unigrams.push_back(changed.unigrams.front()); ExpectBuild(changed, p::Status::kDuplicateRecord);
  changed = source; changed.bigrams.push_back(changed.bigrams.front()); ExpectBuild(changed, p::Status::kDuplicateRecord);
  changed = source; changed.trigrams.push_back(changed.trigrams.front()); ExpectBuild(changed, p::Status::kDuplicateRecord);
  changed = source; changed.unigrams.front().id = 1; ExpectBuild(changed, p::Status::kUnknownId);
  changed = source; changed.unigrams.front().count = 0; ExpectBuild(changed, p::Status::kInvalidFormat);
  changed = source; changed.bigrams.front().previous_id = 0; ExpectBuild(changed, p::Status::kUnknownId);
  changed = source; changed.trigrams.front().previous2_id = 0; ExpectBuild(changed, p::Status::kUnknownId);
  changed = source; changed.unigrams.front().count = (std::numeric_limits<std::uint64_t>::max)();
  ExpectBuild(changed, p::Status::kCountOverflow);
  changed = source; changed.bigrams.front().count = (std::numeric_limits<std::uint64_t>::max)();
  ExpectBuild(changed, p::Status::kCountOverflow);
  changed = source; changed.trigrams.front().count = (std::numeric_limits<std::uint64_t>::max)();
  ExpectBuild(changed, p::Status::kCountOverflow);
  changed = source; changed.bigrams.front().count = 100000; Totals(&changed);
  ExpectBuild(changed, p::Status::kInconsistentTotals);
  changed = source; changed.trigrams.front().count = 100000; Totals(&changed);
  ExpectBuild(changed, p::Status::kInconsistentTotals);
  auto extreme = Blank();
  Lexeme(&extreme, "large"); Lexeme(&extreme, "unseen");
  extreme.unigrams = {{Id("large"), (std::numeric_limits<std::uint64_t>::max)()}};
  Totals(&extreme);
  const auto extreme_model = Build(extreme);
  CHECK(std::isfinite(extreme_model->Score({}, Id("unseen")).log_probability));
  CHECK(extreme_model->Score({}, Id("unseen")).probability > 0);

  p::BuildInfo info;
  CHECK(!p::Model::Load(fixtures / "absent.tsv", {}, &info));
  CHECK(info.status == p::Status::kIoError);
  const auto path = fixtures / "valid-test-only.tsv";
  CHECK(!p::Model::Load(path, {}, &info));
  CHECK(info.status == p::Status::kTestFixtureDenied);
  p::LoadOptions load;
  load.allow_test_fixture = true;
  std::ifstream answers(fixtures / "identity-answers.txt");
  std::string line;
  std::getline(answers, line); std::getline(answers, load.expected_source_fingerprint);
  const auto loaded = p::Model::Load(path, load, &info);
  CHECK(loaded != nullptr && info.status == p::Status::kOk);
  CHECK(info.payload_fingerprint_verified && info.source_fingerprint_matched);
  CHECK(loaded->metadata().source == p::SourceKind::kTestFixture);
  CHECK(loaded->UnigramCount(Id("hello")) == 40);
  CHECK(loaded->lexemes().size() == 8);
  CHECK(loaded->Predict(std::nullopt, {Id("hello")}).suggestions.front().lexeme_ids.front() == Id("world"));
  CHECK(loaded->Predict(u8"你", {}).suggestions.front().tokens_utf8.front() == u8"你好");
  load.expected_source_fingerprint.assign(64, 'f');
  CHECK(!p::Model::Load(path, load, &info));
  CHECK(info.status == p::Status::kFingerprintMismatch);
  load.expected_source_fingerprint.clear();
  CHECK(p::Model::Load(path, load, &info) != nullptr);
  CHECK(!info.source_fingerprint_matched && info.payload_fingerprint_verified);
  load.max_file_utf8_bytes = 8;
  CHECK(!p::Model::Load(path, load, &info));
  CHECK(info.status == p::Status::kLimitExceeded);
  load.max_file_utf8_bytes = 256 * 1024 * 1024;
  const std::vector<std::pair<std::string, p::Status>> cases = {
      {"bad-version", p::Status::kUnsupportedVersion},
      {"bad-id-version", p::Status::kUnsupportedVersion},
      {"bad-normalizer", p::Status::kUnsupportedVersion},
      {"bad-source-kind", p::Status::kInvalidFormat},
      {"bad-source-fingerprint", p::Status::kInvalidFormat},
      {"bad-payload-fingerprint", p::Status::kFingerprintMismatch},
      {"bad-count-total", p::Status::kInconsistentTotals},
      {"bad-lexeme-total", p::Status::kInconsistentTotals},
      {"bad-integer-overflow", p::Status::kCountOverflow},
      {"bad-leading-zero", p::Status::kInvalidFormat},
      {"bad-bom", p::Status::kInvalidFormat}, {"bad-crlf", p::Status::kInvalidFormat},
      {"bad-final-lf", p::Status::kInvalidFormat}, {"bad-binary", p::Status::kInvalidFormat},
      {"bad-nul", p::Status::kInvalidFormat}, {"bad-utf8", p::Status::kInvalidUtf8},
      {"bad-normalized", p::Status::kInvalidLexeme}, {"bad-empty-line", p::Status::kInvalidFormat},
      {"bad-duplicate", p::Status::kDuplicateRecord}, {"bad-order", p::Status::kInvalidFormat},
      {"bad-unknown-id", p::Status::kUnknownId}, {"bad-zero-count", p::Status::kInvalidFormat},
      {"bad-zero-history", p::Status::kInvalidFormat}, {"bad-columns", p::Status::kInvalidFormat}};
  for (const auto& item : cases) {
    CHECK(!p::Model::Load(fixtures / (item.first + ".tsv"), load, &info));
    if (info.status != item.second)
      throw std::runtime_error(item.first + " returned " + p::StatusName(info.status) +
                               ", expected " + p::StatusName(item.second));
    ++checks;
  }
  // Once loaded, files are not revisited for per-key queries. Scratch stays in build.
  const auto detached_path = fixtures.parent_path() / "detached-test-only.tsv";
  std::filesystem::copy_file(path, detached_path, std::filesystem::copy_options::overwrite_existing);
  const auto detached = p::Model::Load(detached_path, load, &info);
  CHECK(detached != nullptr);
  CHECK(std::filesystem::remove(detached_path));
  CHECK(detached->Predict(std::nullopt, {Id("hello")}).suggestions.front().lexeme_ids.front() == Id("world"));
  std::cout << "atomic semantic validation, independent SHA256, 24 corrupt files and offline queries verified\n";
}

void TestConcurrentIsolation() {
  const auto records = Fixture();
  const auto model = Build(records);
  const p::Context context{Id("i"), Id("like")};
  const auto static_order = model->Predict(std::nullopt, context);
  std::atomic<bool> valid{true};
  std::vector<std::thread> workers;
  for (unsigned worker = 0; worker < 8; ++worker) {
    workers.emplace_back([&, worker] {
      p::PersonalCounts personal;
      const auto preferred = worker % 2 ? Id("coffee") : Id("tea");
      personal.trigrams = {{Id("i"), Id("like"), preferred, 1000000 + worker}};
      for (unsigned query = 0; query < 100; ++query) {
        const auto result = model->Predict(std::nullopt, context, {}, &personal);
        if (result.status != p::Status::kOk || !result.complete || result.personal_preparations != 1 ||
            result.suggestions.empty() || result.suggestions.front().lexeme_ids.front() != preferred)
          valid = false;
        const auto plain = model->Predict(std::nullopt, context);
        if (plain.suggestions.empty() || plain.personal_preparations != 0 ||
            plain.suggestions.front().lexeme_ids != static_order.suggestions.front().lexeme_ids)
          valid = false;
      }
    });
  }
  for (auto& worker : workers) worker.join();
  CHECK(valid.load());
  CHECK(model->UnigramCount(Id("tea")) == 37);
  std::cout << "8-thread shared model: 1600 alternating personal/static queries isolated\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--forcefail") {
    try { CHECK(false); } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 3;
  }
  if (argc != 3 || std::string(argv[1]) != "--unit") {
    std::cerr << "Expected --unit <build-only-fixture-directory>\n";
    return 2;
  }
  try {
    const std::filesystem::path fixtures(argv[2]);
    TestIdentityUtf8(fixtures);
    TestStaticAndOracle();
    TestPersonalAndRandomOracle();
    TestRankedBeyond4096AndBudget();
    TestContinuation();
    TestValidation(fixtures);
    TestConcurrentIsolation();
    std::cout << "RESULT PASS prediction core checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    std::cerr << "RESULT FAIL prediction core checks=" << checks << '\n';
    return 1;
  }
}
