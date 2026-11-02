#include "engine/prediction/abbreviation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace p = epocanva::prediction;
namespace a = epocanva::prediction::abbreviation;
namespace {

std::size_t checks = 0;
void Check(bool value, const char* expression, int line) {
  ++checks;
  if (!value) throw std::runtime_error(std::string("check failed line ") +
                                     std::to_string(line) + ": " + expression);
}
#define CHECK(value) Check((value), #value, __LINE__)

bool Near(double left, double right) {
  return std::fabs(left - right) <= 3e-12 * (1.0 + std::fabs(left) + std::fabs(right));
}
p::LexemeId Id(std::string_view text, std::string_view language_id = "en") {
  return p::StableLexemeId(language_id, text);
}

// Explicitly SYNTHETIC TEST FIXTURE. Counts are accumulated from sentences to
// satisfy the real Model::FromRecords marginal checks, never a decoder stub.
struct Corpus {
  p::ModelRecords records;
  std::map<p::LexemeId, std::uint64_t> unigrams;
  std::map<std::pair<p::LexemeId, p::LexemeId>, std::uint64_t> bigrams;
  std::map<std::tuple<p::LexemeId, p::LexemeId, p::LexemeId>, std::uint64_t> trigrams;
  Corpus() {
    records.metadata.source = p::SourceKind::kTestFixture;
    records.metadata.source_fingerprint.assign(64, 'a');
  }
  p::LexemeId Lexeme(std::string text, std::string language_id = "en",
                     p::TokenKind kind = p::TokenKind::kWord) {
    const auto id = Id(text, language_id);
    if (std::none_of(records.lexicon.begin(), records.lexicon.end(),
                     [&](const auto& row) { return row.id == id; }))
      records.lexicon.push_back({id, std::move(language_id), std::move(text), kind});
    return id;
  }
  void Sentence(const std::vector<std::string>& words, std::uint64_t repetitions,
                const std::string& language_id = "en") {
    std::vector<p::LexemeId> ids;
    for (const auto& word : words) ids.push_back(Lexeme(word, language_id));
    ids.push_back(p::kEosId);
    for (std::size_t token_ordinal = 0; token_ordinal < ids.size(); ++token_ordinal) {
      unigrams[ids[token_ordinal]] += repetitions;
      if (token_ordinal >= 1)
        bigrams[{ids[token_ordinal - 1], ids[token_ordinal]}] += repetitions;
      if (token_ordinal >= 2)
        trigrams[{ids[token_ordinal - 2], ids[token_ordinal - 1], ids[token_ordinal]}] += repetitions;
    }
  }
  p::ModelRecords Finish() {
    records.metadata.lexeme_count = records.lexicon.size();
    for (const auto& row : unigrams) {
      records.unigrams.push_back({row.first, row.second});
      records.metadata.unigram_total += row.second;
    }
    for (const auto& row : bigrams) {
      records.bigrams.push_back({row.first.first, row.first.second, row.second});
      records.metadata.bigram_total += row.second;
    }
    for (const auto& row : trigrams) {
      records.trigrams.push_back({std::get<0>(row.first), std::get<1>(row.first),
                                  std::get<2>(row.first), row.second});
      records.metadata.trigram_total += row.second;
    }
    return records;
  }
};

p::ModelRecords Fixture() {
  Corpus corpus;
  corpus.Sentence({"hi", "say", "good", "morning"}, 200);
  corpus.Sentence({"we", "say", "great", "meeting"}, 200);
  corpus.Sentence({"hi", "say", "great", "meeting"}, 2);
  corpus.Sentence({"we", "say", "good", "morning"}, 2);
  corpus.Sentence({"green", "maple"}, 3);
  corpus.Sentence({"be", "right", "back"}, 40);
  corpus.Sentence({"alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "hotel", "india"}, 20);
  corpus.Lexeme("g!", "en", p::TokenKind::kPunctuation);
  corpus.Lexeme("m!", "en", p::TokenKind::kPunctuation);
  corpus.Sentence({"ami", "dit", "bon", "matin"}, 70, "fr");
  corpus.Sentence({"architecte", "dit", "belle", "maison"}, 70, "fr");
  corpus.Sentence({u8"école", "bilingue"}, 25, "fr");
  corpus.Sentence({u8"étude", "brillante"}, 12, "fr");
  corpus.Sentence({u8"àcole", "bilingue"}, 100, "fr");
  corpus.Sentence({u8"Éclair", "bilingue"}, 3, "fr");
  corpus.Sentence({u8"𐐀cole", "bilingue"}, 2, "fr");
  corpus.Sentence({u8"你好", u8"世界"}, 30, "zh");
  corpus.Sentence({u8"今日は", u8"世界"}, 30, "ja");
  corpus.Sentence({u8"좋은", u8"아침"}, 30, "ko");
  return corpus.Finish();
}

std::shared_ptr<const p::Model> BuildModel(const p::ModelRecords& records) {
  p::BuildInfo info;
  const auto model = p::Model::FromRecords(records, &info);
  if (!model) throw std::runtime_error(std::string("invalid test fixture: ") +
                                      p::StatusName(info.status));
  CHECK(info.status == p::Status::kOk);
  CHECK(model->metadata().source == p::SourceKind::kTestFixture);
  return model;
}
std::shared_ptr<const a::Decoder> BuildDecoder(const std::shared_ptr<const p::Model>& model,
                                              std::string_view source = {}) {
  a::BuildInfo info;
  const auto decoder = a::Decoder::Build(model, source, {}, &info);
  if (!decoder) throw std::runtime_error(std::string("invalid test resource: ") +
                                        p::StatusName(info.status));
  CHECK(info.status == p::Status::kOk && info.line_ordinal == 0);
  CHECK(info.explicit_expansions == decoder->explicit_expansion_count());
  return decoder;
}
std::string Header() { return "EPOCANVA_ABBREVIATIONS_TSV\t1\n"; }
std::string Row(const std::string& key, const std::vector<std::string>& words,
                const std::string& language_id = "en") {
  std::string text = language_id + '\t' + key + '\t';
  for (std::size_t token_ordinal = 0; token_ordinal < words.size(); ++token_ordinal) {
    if (token_ordinal != 0) text.push_back(' ');
    text += std::to_string(Id(words[token_ordinal], language_id));
  }
  return text + '\n';
}
const a::Suggestion* FindText(const a::Result& result, std::string_view text) {
  const auto found = std::find_if(result.suggestions.begin(), result.suggestions.end(),
                                 [&](const auto& suggestion) { return suggestion.text_utf8 == text; });
  return found == result.suggestions.end() ? nullptr : &*found;
}
void CheckAttested(const a::Result& result, const p::Model& model,
                   const std::string& language_id, const a::Options& options = {}) {
  CHECK(result.suggestions.size() <= options.max_suggestions);
  CHECK(result.model_queries <= options.max_model_queries);
  CHECK(result.index_entries_visited <= options.max_index_entries);
  CHECK(result.trie_nodes_visited <= options.max_trie_nodes);
  CHECK(result.explicit_expansions_visited <= options.max_explicit_expansions);
  CHECK(result.personal_preparations <= result.model_queries);
  for (const auto& suggestion : result.suggestions) {
    CHECK(!suggestion.lexeme_ids.empty());
    CHECK(suggestion.lexeme_ids.size() <= options.max_output_lexemes);
    CHECK(suggestion.lexeme_ids.size() == suggestion.tokens_utf8.size());
    CHECK(suggestion.text_utf8.size() <= options.max_output_utf8_bytes);
    CHECK(p::IsValidUtf8(suggestion.text_utf8));
    CHECK(std::isfinite(suggestion.log_probability));
    CHECK(suggestion.log_probability <= 0.0 && suggestion.match_log_score <= 0.0);
    CHECK(!suggestion.exact_key || suggestion.explicit_key);
    std::string joined;
    for (std::size_t token_ordinal = 0; token_ordinal < suggestion.lexeme_ids.size(); ++token_ordinal) {
      const auto* row = model.Find(suggestion.lexeme_ids[token_ordinal]);
      CHECK(row && row->kind == p::TokenKind::kWord && row->language_id == language_id);
      CHECK(row->normalized_utf8 == suggestion.tokens_utf8[token_ordinal]);
      if (token_ordinal != 0 && language_id != "zh" && language_id != "ja") joined.push_back(' ');
      joined += row->normalized_utf8;
    }
    CHECK(joined == suggestion.text_utf8);
  }
  if (result.complete) CHECK(!result.budget_exhausted && !result.beam_pruned);
}

// Independent equation oracle. Does not call Model::Score/Predict or decoder
// helpers. All context IDs in these cases are known, and no backoff resets occur.
double OracleProbability(const p::ModelRecords& records, const p::Context& context,
                          p::LexemeId target, const p::PersonalCounts* personal = nullptr) {
  std::uint64_t unigram = 0;
  for (const auto& row : records.unigrams) if (row.id == target) unigram = row.count;
  double probability = (static_cast<double>(unigram) + 1.0) /
      (static_cast<double>(records.metadata.unigram_total) +
       static_cast<double>(records.lexicon.size()) + 1.0);
  const auto bigram = [&](const std::vector<p::BigramRecord>& rows, double prior, double lower) {
    std::uint64_t numerator = 0, total = 0;
    for (const auto& row : rows) {
      if (row.previous_id != context.back()) continue;
      total += row.count;
      if (row.id == target) numerator += row.count;
    }
    return (static_cast<double>(numerator) + prior * lower) /
        (static_cast<double>(total) + prior);
  };
  const auto trigram = [&](const std::vector<p::TrigramRecord>& rows, double prior, double lower) {
    std::uint64_t numerator = 0, total = 0;
    for (const auto& row : rows) {
      if (row.previous2_id != context[0] || row.previous1_id != context[1]) continue;
      total += row.count;
      if (row.id == target) numerator += row.count;
    }
    return (static_cast<double>(numerator) + prior * lower) /
        (static_cast<double>(total) + prior);
  };
  if (!context.empty()) probability = bigram(records.bigrams, 8.0, probability);
  if (context.size() == 2) probability = trigram(records.trigrams, 4.0, probability);
  if (personal) {
    std::uint64_t numerator = 0, total = 0;
    for (const auto& row : personal->unigrams) {
      total += row.count;
      if (row.id == target) numerator += row.count;
    }
    probability = (static_cast<double>(numerator) + 16.0 * probability) /
        (static_cast<double>(total) + 16.0);
    if (!context.empty()) probability = bigram(personal->bigrams, 16.0, probability);
    if (context.size() == 2) probability = trigram(personal->trigrams, 16.0, probability);
  }
  return probability;
}
double OraclePhrase(const p::ModelRecords& records, p::Context context,
                     const std::vector<p::LexemeId>& ids, const p::PersonalCounts* personal = nullptr) {
  double log_probability = 0.0;
  for (const auto id : ids) {
    log_probability += std::log(OracleProbability(records, context, id, personal));
    if (context.size() == 2) context.erase(context.begin());
    context.push_back(id);
  }
  return log_probability;
}
struct OracleSuggestion { std::vector<p::LexemeId> ids; double score; };
std::vector<OracleSuggestion> OracleExpansions(const p::ModelRecords& records,
                                              const std::vector<std::string>& prefixes_utf8,
                                              const p::Context& previous_ids,
                                              const std::string& language_id,
                                              const p::PersonalCounts* personal = nullptr) {
  std::vector<OracleSuggestion> suggestions;
  std::vector<p::LexemeId> ids;
  std::function<void(std::size_t)> enumerate = [&](std::size_t token_ordinal) {
    if (token_ordinal == prefixes_utf8.size()) {
      suggestions.push_back({ids, OraclePhrase(records, previous_ids, ids, personal)});
      return;
    }
    for (const auto& row : records.lexicon) {
      if (row.language_id != language_id || row.kind != p::TokenKind::kWord ||
          row.normalized_utf8.compare(0, prefixes_utf8[token_ordinal].size(),
                                       prefixes_utf8[token_ordinal]) != 0) continue;
      ids.push_back(row.id);
      enumerate(token_ordinal + 1);
      ids.pop_back();
    }
  };
  enumerate(0);
  std::sort(suggestions.begin(), suggestions.end(), [](const auto& left, const auto& right) {
    if (left.score != right.score) return left.score > right.score;
    return left.ids < right.ids;
  });
  if (suggestions.size() > a::kMaxResults) suggestions.resize(a::kMaxResults);
  return suggestions;
}
void CheckOracle(const p::ModelRecords& records, const a::Decoder& decoder,
                  std::string_view shorthand_utf8, const std::vector<std::string>& prefixes_utf8,
                  const p::Context& previous_ids, const std::string& language_id,
                  const p::PersonalCounts* personal = nullptr) {
  a::Options options;
  options.beam_width = 63;
  const auto actual = decoder.Decode(shorthand_utf8, previous_ids, language_id, options, personal);
  const auto expected = OracleExpansions(records, prefixes_utf8, previous_ids, language_id, personal);
  CHECK(actual.status == p::Status::kOk && actual.complete);
  CHECK(actual.suggestions.size() == expected.size());
  for (std::size_t suggestion_ordinal = 0; suggestion_ordinal < expected.size(); ++suggestion_ordinal) {
    CHECK(actual.suggestions[suggestion_ordinal].lexeme_ids == expected[suggestion_ordinal].ids);
    CHECK(Near(actual.suggestions[suggestion_ordinal].log_probability,
               expected[suggestion_ordinal].score));
    CHECK(actual.suggestions[suggestion_ordinal].match_log_score == 0.0);
  }
}

bool SameResult(const a::Result& left, const a::Result& right) {
  if (left.status != right.status || left.complete != right.complete ||
      left.budget_exhausted != right.budget_exhausted || left.beam_pruned != right.beam_pruned ||
      left.model_queries != right.model_queries ||
      left.index_entries_visited != right.index_entries_visited ||
      left.trie_nodes_visited != right.trie_nodes_visited ||
      left.explicit_expansions_visited != right.explicit_expansions_visited ||
      left.personal_preparations != right.personal_preparations ||
      left.suggestions.size() != right.suggestions.size()) return false;
  for (std::size_t ordinal = 0; ordinal < left.suggestions.size(); ++ordinal) {
    const auto& first = left.suggestions[ordinal];
    const auto& second = right.suggestions[ordinal];
    if (first.text_utf8 != second.text_utf8 || first.tokens_utf8 != second.tokens_utf8 ||
        first.lexeme_ids != second.lexeme_ids || first.log_probability != second.log_probability ||
        first.match_log_score != second.match_log_score || first.explicit_key != second.explicit_key ||
        first.exact_key != second.exact_key) return false;
  }
  return true;
}

void TestContextAndPrefixes() {
  const auto records = Fixture();
  const auto model = BuildModel(records);
  const auto decoder = BuildDecoder(model);
  CHECK(decoder->explicit_expansion_count() == 0);  // gm was never in ANY lookup table.
  const p::Context greeting{Id("hi"), Id("say")};
  const p::Context business{Id("we"), Id("say")};
  const auto morning = decoder->Decode("gm", greeting, "en");
  const auto meeting = decoder->Decode("gm", business, "en");
  CHECK(morning.complete && meeting.complete);
  CHECK(!morning.suggestions.empty() && !meeting.suggestions.empty());
  CHECK(morning.suggestions.front().text_utf8 == "good morning");
  CHECK(meeting.suggestions.front().text_utf8 == "great meeting");
  CHECK(morning.suggestions.front().lexeme_ids != meeting.suggestions.front().lexeme_ids);
  CHECK(morning.personal_preparations == 0 && meeting.personal_preparations == 0);
  CHECK(!morning.suggestions.front().explicit_key && !morning.suggestions.front().exact_key);
  CHECK(!meeting.suggestions.front().explicit_key && !meeting.suggestions.front().exact_key);
  CHECK(FindText(morning, "great meeting") != nullptr);
  CHECK(FindText(meeting, "good morning") != nullptr);
  CheckAttested(morning, *model, "en");
  CheckAttested(meeting, *model, "en");
  CHECK(SameResult(morning, decoder->Decode("G M", greeting, "en")));
  CHECK(SameResult(morning, decoder->Decode("GM", greeting, "en")));
  const auto morning_prefix = decoder->Decode("g mor", business, "en");
  CHECK(morning_prefix.complete && !morning_prefix.suggestions.empty());
  CHECK(FindText(morning_prefix, "great morning") != nullptr);
  for (const auto& candidate : morning_prefix.suggestions)
    CHECK(candidate.tokens_utf8.back() == "morning");
  const auto compact = decoder->Decode("gmor", greeting, "en");
  CHECK(compact.complete && compact.suggestions.front().text_utf8 == "good morning");
  for (const auto& candidate : compact.suggestions) {
    CHECK(candidate.lexeme_ids.size() == 2);
    CHECK(candidate.tokens_utf8.back().compare(0, 3, "mor") == 0);
  }
  CHECK(decoder->Decode("g mee", business, "en").suggestions.front().text_utf8 == "great meeting");
  CHECK(decoder->Decode("gxyz", business, "en").suggestions.empty());
  CHECK(decoder->Decode("good morning", greeting, "en").suggestions.empty());
  CHECK(decoder->Decode("zq", greeting, "en").complete);
  CHECK(decoder->Decode("zq", greeting, "en").suggestions.empty());
  for (const auto& context : std::vector<p::Context>{{}, {Id("say")}, greeting, business}) {
    CheckOracle(records, *decoder, "gm", {"g", "m"}, context, "en");
    CheckOracle(records, *decoder, "g m", {"g", "m"}, context, "en");
    CheckOracle(records, *decoder, "g mor", {"g", "mor"}, context, "en");
    CheckOracle(records, *decoder, "gmb", {"g", "m", "b"}, context, "en");
  }
  const auto eight = decoder->Decode("abcdefhi", {}, "en");
  CHECK(eight.complete);
  CHECK(FindText(eight, "alpha bravo charlie delta echo foxtrot hotel india") != nullptr);
  CHECK(eight.suggestions.front().lexeme_ids.size() == 8);
  CheckAttested(eight, *model, "en");
  std::cout << "model-only unseen initials, two-ID context reversal and scalar-prefix grammar verified\n";
}

void TestMultilingualScalars() {
  const auto records = Fixture();
  const auto model = BuildModel(records);
  const auto decoder = BuildDecoder(model);
  const auto friendly = decoder->Decode("bm", {Id("ami", "fr"), Id("dit", "fr")}, "fr");
  const auto house = decoder->Decode("bm", {Id("architecte", "fr"), Id("dit", "fr")}, "fr");
  CHECK(friendly.complete && house.complete);
  CHECK(friendly.suggestions.front().text_utf8 == "bon matin");
  CHECK(house.suggestions.front().text_utf8 == "belle maison");
  CheckOracle(records, *decoder, "bm", {"b", "m"}, {Id("ami", "fr"), Id("dit", "fr")}, "fr");
  const auto accented = decoder->Decode(u8"éb", {}, "fr");
  CHECK(accented.complete && !accented.suggestions.empty());
  for (const auto& suggestion : accented.suggestions) {
    CHECK(suggestion.tokens_utf8.front().compare(0, std::string(u8"é").size(), u8"é") == 0);
    CHECK(suggestion.tokens_utf8.front() != u8"àcole");  // Same leading UTF-8 byte, WRONG scalar.
  }
  CHECK(SameResult(accented, decoder->Decode(u8"é b", {}, "fr")));
  CheckOracle(records, *decoder, u8"éb", {u8"é", "b"}, {}, "fr");
  CHECK(decoder->Decode(u8"Éb", {}, "fr").suggestions.front().tokens_utf8.front() == u8"Éclair");
  CHECK(decoder->Decode(u8"e\u0301b", {}, "fr").suggestions.empty());  // Not NFC.
  const auto supplementary = decoder->Decode(u8"𐐀b", {}, "fr");
  CHECK(supplementary.complete && supplementary.suggestions.front().text_utf8 == u8"𐐀cole bilingue");
  const std::vector<std::tuple<std::string, std::string, std::string>> examples = {
      {"zh", u8"你世", u8"你好世界"}, {"ja", u8"今世", u8"今日は世界"},
      {"ko", u8"좋아", u8"좋은 아침"}};
  for (const auto& example : examples) {
    const auto result = decoder->Decode(std::get<1>(example), {}, std::get<0>(example));
    CHECK(result.complete && !result.suggestions.empty());
    CHECK(result.suggestions.front().text_utf8 == std::get<2>(example));
    CHECK(result.suggestions.front().lexeme_ids.size() == 2);
    CheckAttested(result, *model, std::get<0>(example));
  }
  a::Options bytes;
  bytes.max_output_utf8_bytes = 11;
  const auto too_short = decoder->Decode(u8"你世", {}, "zh", bytes);
  CHECK(too_short.complete && too_short.suggestions.empty());  // 12 UTF-8 bytes, never sliced.
  CHECK(decoder->Decode("gm", {}, "fr").suggestions.empty());
  std::cout << "French contextual ambiguity and 2/3/4-byte scalar initials across five languages verified\n";
}

void TestExplicitTrie() {
  const auto records = Fixture();
  const auto model = BuildModel(records);
  const std::vector<std::string> rows = {
      Row("zz", {"good", "morning"}), Row("zz", {"great", "meeting"}),
      Row("zzz", {"great", "meeting"}), Row("brb", {"be", "right", "back"}),
      Row(u8"éb", {u8"école", "bilingue"}, "fr"),
      Row("full key", {"good", "morning"}), Row("gm", {"good", "morning"})};
  std::string source = Header();
  for (const auto& row : rows) source += row;
  const auto decoder = BuildDecoder(model, source);
  CHECK(decoder->explicit_expansion_count() == rows.size());
  source.assign(source.size(), '!');  // No borrowed source lifetime.
  const p::Context greeting{Id("hi"), Id("say")};
  const p::Context business{Id("we"), Id("say")};
  const auto morning = decoder->Decode("zz", greeting, "en");
  const auto meeting = decoder->Decode("zz", business, "en");
  CHECK(morning.complete && meeting.complete);
  CHECK(morning.suggestions.front().text_utf8 == "good morning");
  CHECK(meeting.suggestions.front().text_utf8 == "great meeting");
  CHECK(meeting.suggestions.size() == 2);  // Same output via zz and zzz is merged.
  CHECK(meeting.suggestions.front().match_log_score == 0.0);
  CHECK(meeting.suggestions.front().explicit_key && meeting.suggestions.front().exact_key);
  for (const auto& result : {morning, meeting}) CheckAttested(result, *model, "en");
  const auto partial = decoder->Decode("z", business, "en");
  CHECK(partial.complete && partial.suggestions.front().text_utf8 == "great meeting");
  CHECK(partial.suggestions.size() == 2);
  CHECK(partial.suggestions.front().explicit_key && !partial.suggestions.front().exact_key);
  CHECK(Near(partial.suggestions.front().match_log_score, -std::log(2.0)));
  CHECK(Near(partial.suggestions.front().log_probability,
             OraclePhrase(records, business, partial.suggestions.front().lexeme_ids)));
  const auto irregular_prefix = decoder->Decode("BR", {}, "en");
  CHECK(irregular_prefix.complete && FindText(irregular_prefix, "be right back") != nullptr);
  CHECK(Near(FindText(irregular_prefix, "be right back")->match_log_score, -std::log(2.0)));
  CHECK(decoder->Decode("brb", {}, "en").suggestions.front().text_utf8 == "be right back");
  CHECK(decoder->Decode("full k", greeting, "en").suggestions.front().text_utf8 == "good morning");
  CHECK(decoder->Decode("brbeyond", {}, "en").suggestions.empty());
  CHECK(decoder->Decode("zz", {}, "fr").suggestions.empty());
  a::Options only_explicit;
  only_explicit.enable_initialism = false;
  const auto accented = decoder->Decode(u8"é", {}, "fr", only_explicit);
  CHECK(accented.complete && accented.suggestions.front().text_utf8 == u8"école bilingue");
  CHECK(accented.trie_nodes_visited == 3);  // root + é scalar edge + b scalar edge.
  CHECK(Near(accented.suggestions.front().match_log_score, -std::log(2.0)));
  CHECK(decoder->Decode(u8"É", {}, "fr", only_explicit).suggestions.empty());
  CHECK(decoder->Decode(u8"e\u0301", {}, "fr", only_explicit).suggestions.empty());
  const auto merged = decoder->Decode("gm", greeting, "en");
  CHECK(std::count_if(merged.suggestions.begin(), merged.suggestions.end(),
                      [](const auto& row) { return row.text_utf8 == "good morning"; }) == 1);
  CHECK(merged.suggestions.front().match_log_score == 0.0);
  CHECK(merged.suggestions.front().explicit_key && merged.suggestions.front().exact_key);
  CHECK(!decoder->Decode("G M", greeting, "en").suggestions.front().exact_key);
  std::string reversed = Header();
  for (auto row = rows.rbegin(); row != rows.rend(); ++row) reversed += *row;
  const auto reordered = BuildDecoder(model, reversed);
  CHECK(SameResult(morning, reordered->Decode("zz", greeting, "en")));
  a::Options limited = only_explicit;
  limited.max_explicit_expansions = 1;
  CHECK(SameResult(decoder->Decode("z", business, "en", limited),
                   reordered->Decode("z", business, "en", limited)));
  a::Options only_initial;
  only_initial.enable_explicit = false;
  CHECK(decoder->Decode("zz", business, "en", only_initial).suggestions.empty());
  only_initial.enable_initialism = false;
  CHECK(decoder->Decode("zz", business, "en", only_initial).complete);
  CHECK(decoder->Decode("zz", business, "en", only_initial).model_queries == 0);
  CHECK(decoder->Decode("zz", business, "en", only_initial).trie_nodes_visited == 0);
  // Partial explicit provenance survives temporary top-k eviction and later
  // model rediscovery of the same IDs with a better (zero) match penalty.
  a::Options one;
  one.max_suggestions = 1;
  const auto rediscovered = BuildDecoder(model, Header() +
      Row("gm", {"great", "meeting"}) + Row("gmaaaaaaaa", {"good", "morning"}));
  const auto evidence = rediscovered->Decode("gm", greeting, "en", one);
  CHECK(evidence.suggestions.size() == 1 && evidence.suggestions.front().text_utf8 == "good morning");
  CHECK(evidence.suggestions.front().match_log_score == 0.0);
  CHECK(evidence.suggestions.front().explicit_key && !evidence.suggestions.front().exact_key);
  std::cout << "immutable parsed expansion trie, partial keys, attested outputs and context rerank verified\n";
}

void ExpectBuildFailure(const std::shared_ptr<const p::Model>& model, const std::string& source,
                        p::Status expected, const a::BuildOptions& options = {}) {
  a::BuildInfo info;
  CHECK(!a::Decoder::Build(model, source, options, &info));
  CHECK(info.status == expected);
  CHECK(info.explicit_expansions == 0 && info.trie_nodes == 0);
}
void TestParserAndInvalidInputs() {
  const auto model = BuildModel(Fixture());
  const auto decoder = BuildDecoder(model);
  const auto valid_row = Row("brb", {"be", "right", "back"});
  const auto valid_source = Header() + valid_row;
  CHECK(BuildDecoder(model, Header())->explicit_expansion_count() == 0);
  ExpectBuildFailure(nullptr, {}, p::Status::kModelUnavailable);
  ExpectBuildFailure(p::Model::Unavailable(), {}, p::Status::kModelUnavailable);
  ExpectBuildFailure(model, "EPOCANVA_ABBREVIATIONS_TSV\t2\n", p::Status::kUnsupportedVersion);
  ExpectBuildFailure(model, "\xef\xbb\xbf" + valid_source, p::Status::kInvalidFormat);
  ExpectBuildFailure(model, valid_source.substr(0, valid_source.size() - 1), p::Status::kInvalidFormat);
  ExpectBuildFailure(model, Header() + "\n", p::Status::kInvalidFormat);
  ExpectBuildFailure(model, Header() + "# comment\n", p::Status::kInvalidFormat);
  ExpectBuildFailure(model, Header() + "en\tbrb\t1\textra\n", p::Status::kInvalidFormat);
  ExpectBuildFailure(model, Header() + "en\tbrb\tgood morning\n", p::Status::kInvalidFormat);
  ExpectBuildFailure(model, Header() + "en\tbrb\t0\n", p::Status::kUnknownId);
  ExpectBuildFailure(model, Header() + "en\tbrb\t1\n", p::Status::kUnknownId);
  ExpectBuildFailure(model, Header() + "en\tbrb\t" + std::to_string(Id("not-in-model")) + "\n",
                      p::Status::kUnknownId);
  ExpectBuildFailure(model, Header() + "en\tbrb\t18446744073709551616\n", p::Status::kCountOverflow);
  for (const std::string invalid_id : {"01", "+1", "-1", "1.0", "", " 1", "1 ", "1  2"})
    ExpectBuildFailure(model, Header() + "en\tbrb\t" + invalid_id + "\n",
                        invalid_id == "1 " || invalid_id == "1  2" ?
                        p::Status::kUnknownId : p::Status::kInvalidFormat);
  const auto good_id = std::to_string(Id("good"));
  for (const std::string ids : {good_id + " ", good_id + "  " + good_id})
    ExpectBuildFailure(model, Header() + "en\tbrb\t" + ids + "\n", p::Status::kInvalidFormat);
  ExpectBuildFailure(model, Header() + "fr\tbrb\t" + good_id + "\n", p::Status::kInvalidLexeme);
  ExpectBuildFailure(model, Header() + Row("brb", {"g!"}), p::Status::kInvalidLexeme);
  for (const std::string language_id : {"EN", "de", "en-us", "zh-hant", "en/evil", ""})
    ExpectBuildFailure(model, Header() + language_id + "\tbrb\t" + good_id + "\n",
                        p::Status::kInvalidFormat);
  for (const std::string key : {"BRB", "", " brb", "brb ", "b  rb", "b\tb", u8"b\u00a0b"}) {
    const auto expected = key == "b\tb" ? p::Status::kInvalidFormat : p::Status::kInvalidLexeme;
    ExpectBuildFailure(model, Header() + "en\t" + key + "\t" + good_id + "\n", expected);
  }
  auto carriage = valid_source;
  carriage.insert(carriage.size() - 1, 1, '\r');
  ExpectBuildFailure(model, carriage, p::Status::kInvalidFormat);
  auto nul = valid_source;
  nul.insert(nul.size() - 1, 1, '\0');
  ExpectBuildFailure(model, nul, p::Status::kInvalidFormat);
  a::BuildInfo duplicate_info;
  CHECK(!a::Decoder::Build(model, valid_source + valid_row, {}, &duplicate_info));
  CHECK(duplicate_info.status == p::Status::kDuplicateRecord && duplicate_info.line_ordinal == 3);
  ExpectBuildFailure(model, Header() + Row("x", std::vector<std::string>(9, "good")),
                      p::Status::kLimitExceeded);
  ExpectBuildFailure(model, Header() + Row(std::string(257, 'x'), {"good"}), p::Status::kLimitExceeded);
  ExpectBuildFailure(model, Header() + Row("a b c d e f g h i", {"good"}), p::Status::kLimitExceeded);
  ExpectBuildFailure(model, Header() + std::string(a::kMaxRecordUtf8Bytes + 1, 'x') + '\n',
                      p::Status::kLimitExceeded);
  a::BuildOptions limits;
  limits.max_source_utf8_bytes = valid_source.size() - 1;
  ExpectBuildFailure(model, valid_source, p::Status::kLimitExceeded, limits);
  limits = {}; limits.max_record_utf8_bytes = valid_row.size() - 2;
  ExpectBuildFailure(model, valid_source, p::Status::kLimitExceeded, limits);
  limits = {}; limits.max_records = 0;
  ExpectBuildFailure(model, valid_source, p::Status::kLimitExceeded, limits);
  limits = {}; limits.max_trie_nodes = 3;
  ExpectBuildFailure(model, valid_source, p::Status::kLimitExceeded, limits);
  limits.max_trie_nodes = 4;
  CHECK(a::Decoder::Build(model, valid_source, limits) != nullptr);
  limits = {}; limits.max_source_utf8_bytes = a::kMaxSourceUtf8Bytes + 1;
  ExpectBuildFailure(model, {}, p::Status::kInvalidOptions, limits);
  limits = {}; limits.max_record_utf8_bytes = a::kMaxRecordUtf8Bytes + 1;
  ExpectBuildFailure(model, {}, p::Status::kInvalidOptions, limits);
  limits = {}; limits.max_records = a::kMaxResourceRecords + 1;
  ExpectBuildFailure(model, {}, p::Status::kInvalidOptions, limits);
  limits = {}; limits.max_trie_nodes = a::kMaxResourceTrieNodes + 1;
  ExpectBuildFailure(model, {}, p::Status::kInvalidOptions, limits);

  const std::vector<std::string> malformed = {
      "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80",
      "\x80", "\xc2", "\xe2\x82", "\xf0\x9f\x98", "\xe2(\xa1", "\xfe", "\xff"};
  for (const auto& invalid : malformed) {
    const auto result = decoder->Decode(invalid, {}, "en");
    CHECK(result.status == p::Status::kInvalidUtf8 && !result.complete && result.suggestions.empty());
    CHECK(result.model_queries == 0 && result.index_entries_visited == 0);
    ExpectBuildFailure(model, Header() + "en\t" + invalid + "\t" + good_id + "\n",
                        p::Status::kInvalidUtf8);
  }
  for (const std::string invalid : {"", " gm", "gm ", "g  m", "g\tm", "g\nm", u8"g\u00a0m"})
    CHECK(decoder->Decode(invalid, {}, "en").status == p::Status::kInvalidLexeme);
  CHECK(decoder->Decode(std::string("g\0m", 3), {}, "en").status == p::Status::kInvalidLexeme);
  CHECK(decoder->Decode(std::string(513, 'x'), {}, "en").status == p::Status::kLimitExceeded);
  CHECK(decoder->Decode(std::string(257, 'x'), {}, "en").status == p::Status::kLimitExceeded);
  CHECK(decoder->Decode("a b c d e f g h i", {}, "en").status == p::Status::kLimitExceeded);
  for (const auto& context : std::vector<p::Context>{{0}, {1}, {Id("hi"), 1},
       {Id("good"), Id("great"), Id("morning")}, {Id("bon", "fr")}}) {
    const auto result = decoder->Decode("gm", context, "en");
    CHECK(result.status == p::Status::kInvalidContext && !result.complete && result.suggestions.empty());
    CHECK(result.model_queries == 0);
  }
  for (const std::string language_id : {"", "EN", "de", "en-us", "zh-hant", "en/evil"})
    CHECK(decoder->Decode("gm", {}, language_id).status == p::Status::kInvalidOptions);
  std::cout << "atomic resource validation, invalid IDs/languages/UTF8 and parser bounds verified\n";
}

void TestBudgetsAndOutputLimits() {
  const auto model = BuildModel(Fixture());
  const auto decoder = BuildDecoder(model, Header() + Row("zz", {"good", "morning"}) +
                                    Row("zz", {"great", "meeting"}));
  const p::Context context{Id("hi"), Id("say")};
  a::Options options;
  options.max_model_queries = 0;
  auto result = decoder->Decode("gm", context, "en", options);
  CHECK(result.status == p::Status::kBudgetExhausted && result.budget_exhausted && !result.complete);
  CHECK(result.suggestions.empty() && result.model_queries == 0 && result.index_entries_visited == 0);
  options = {}; options.max_index_entries = 0;
  result = decoder->Decode("gm", context, "en", options);
  CHECK(result.budget_exhausted && !result.complete && result.model_queries == 0);
  options.max_index_entries = 1;
  result = decoder->Decode("gm", context, "en", options);
  CHECK(result.budget_exhausted && !result.complete && result.index_entries_visited == 1);
  CheckAttested(result, *model, "en", options);
  options = {}; options.max_model_queries = 1;
  result = decoder->Decode("gm", context, "en", options);
  CHECK(result.budget_exhausted && result.model_queries == 1 && result.suggestions.empty());
  options.max_model_queries = 2;
  result = decoder->Decode("gm", context, "en", options);
  CHECK(result.budget_exhausted && result.model_queries == 2 && !result.suggestions.empty());
  for (const auto& row : result.suggestions) CHECK(row.lexeme_ids.size() == 2);
  options = {}; options.beam_width = 1;
  result = decoder->Decode("gm", context, "en", options);
  CHECK(result.status == p::Status::kBeamPruned && result.beam_pruned && !result.complete);
  CHECK(!result.budget_exhausted && result.suggestions.front().text_utf8 == "good morning");
  options = {}; options.max_suggestions = 1;
  result = decoder->Decode("gm", context, "en", options);
  CHECK(result.complete && !result.beam_pruned && result.suggestions.size() == 1);
  options.max_suggestions = 0;
  result = decoder->Decode("gm", context, "en", options);
  CHECK(result.complete && result.model_queries == 0 && result.suggestions.empty());
  options = {}; options.max_output_lexemes = 1;
  result = decoder->Decode("gm", context, "en", options);
  CHECK(result.complete && result.suggestions.empty());
  options = {}; options.max_output_utf8_bytes = 11;
  result = decoder->Decode("gm", context, "en", options);
  CHECK(result.complete && !FindText(result, "good morning"));
  CheckAttested(result, *model, "en", options);
  options.max_output_utf8_bytes = 1;
  result = decoder->Decode("gm", context, "en", options);
  CHECK(result.complete && result.suggestions.empty());

  a::Options explicit_only;
  explicit_only.enable_initialism = false;
  explicit_only.max_trie_nodes = 0;
  result = decoder->Decode("zz", context, "en", explicit_only);
  CHECK(result.budget_exhausted && result.trie_nodes_visited == 0 && result.suggestions.empty());
  explicit_only.max_trie_nodes = 2;
  result = decoder->Decode("zz", context, "en", explicit_only);
  CHECK(result.budget_exhausted && result.trie_nodes_visited == 2 && result.suggestions.empty());
  explicit_only.max_trie_nodes = 3;
  explicit_only.max_explicit_expansions = 2;
  explicit_only.max_model_queries = 4;
  result = decoder->Decode("zz", context, "en", explicit_only);
  CHECK(result.complete && result.trie_nodes_visited == 3 && result.model_queries == 4);
  CHECK(result.explicit_expansions_visited == 2 && result.suggestions.size() == 2);
  explicit_only.max_explicit_expansions = 0;
  result = decoder->Decode("zz", context, "en", explicit_only);
  CHECK(result.budget_exhausted && result.explicit_expansions_visited == 0);
  explicit_only.max_explicit_expansions = 1;
  result = decoder->Decode("zz", context, "en", explicit_only);
  CHECK(result.budget_exhausted && result.explicit_expansions_visited == 1);
  CHECK(result.suggestions.size() == 1);
  explicit_only.max_explicit_expansions = 2;
  explicit_only.max_model_queries = 1;
  result = decoder->Decode("zz", context, "en", explicit_only);
  CHECK(result.budget_exhausted && result.suggestions.empty());  // No half expansion.
  explicit_only.max_model_queries = 4;
  explicit_only.max_index_entries = 0;
  CHECK(decoder->Decode("zz", context, "en", explicit_only).complete);  // No indexed queries needed.

  for (unsigned trial = 0; trial < 12; ++trial) {
    a::Options invalid;
    switch (trial) {
      case 0: invalid.max_suggestions = 9; break;
      case 1: invalid.max_output_lexemes = 0; break;
      case 2: invalid.max_output_lexemes = 9; break;
      case 3: invalid.max_output_utf8_bytes = 0; break;
      case 4: invalid.max_output_utf8_bytes = 4097; break;
      case 5: invalid.beam_width = 0; break;
      case 6: invalid.beam_width = 64; break;
      case 7: invalid.max_model_queries = a::kMaxModelQueries + 1; break;
      case 8: invalid.max_index_entries = a::kMaxIndexEntries + 1; break;
      case 9: invalid.max_trie_nodes = a::kMaxResourceTrieNodes + 1; break;
      case 10: invalid.max_explicit_expansions = a::kMaxResourceRecords + 1; break;
      case 11: invalid.max_suggestions = (std::numeric_limits<std::size_t>::max)(); break;
    }
    result = decoder->Decode("gm", context, "en", invalid);
    CHECK(result.status == p::Status::kInvalidOptions && !result.complete && result.suggestions.empty());
  }

  Corpus long_corpus;
  std::string long_word;
  for (unsigned scalar_ordinal = 0; scalar_ordinal < 64; ++scalar_ordinal) long_word += u8"𐐀";
  CHECK(long_word.size() == p::kMaxLexemeUtf8Bytes);
  long_corpus.Sentence(std::vector<std::string>(8, long_word), 2);
  const auto long_model = BuildModel(long_corpus.Finish());
  const auto long_decoder = BuildDecoder(long_model, Header() + Row("x", std::vector<std::string>(8, long_word)));
  a::Options long_options;
  long_options.enable_initialism = false;
  result = long_decoder->Decode("x", {}, "en", long_options);
  CHECK(result.complete && result.suggestions.size() == 1);
  CHECK(result.suggestions.front().lexeme_ids.size() == 8);
  CHECK(result.suggestions.front().text_utf8.size() == 2055);
  CheckAttested(result, *long_model, "en", long_options);
  long_options.max_output_utf8_bytes = 2054;
  result = long_decoder->Decode("x", {}, "en", long_options);
  CHECK(result.complete && result.suggestions.empty());
  long_options.max_output_utf8_bytes = 2055;
  CHECK(long_decoder->Decode("x", {}, "en", long_options).suggestions.size() == 1);

  std::string wide_source = Header();
  for (unsigned ordinal = 0; ordinal < 200; ++ordinal)
    wide_source += Row("q" + std::to_string(ordinal), {"good", "morning"});
  const auto wide = BuildDecoder(model, wide_source);
  a::Options wide_options;
  wide_options.enable_initialism = false;
  wide_options.max_trie_nodes = 7;
  result = wide->Decode("q", context, "en", wide_options);
  CHECK(result.budget_exhausted && result.trie_nodes_visited == 7);
  CheckAttested(result, *model, "en", wide_options);
  std::cout << "shared query/index/trie budgets, honest beam flags and whole-output limits verified\n";
}

void TestExplicitScheduling() {
  const auto model = BuildModel(Fixture());
  const auto decoder = BuildDecoder(model, Header() + Row("brb", {"be", "right", "back"}));
  a::Options bounded;
  bounded.max_model_queries = 3;
  const auto exact = decoder->Decode("brb", {}, "en", bounded);
  CHECK(exact.budget_exhausted && !exact.complete);
  CHECK(exact.model_queries == 3 && exact.index_entries_visited == 0);
  CHECK(exact.suggestions.size() == 1 && exact.suggestions.front().text_utf8 == "be right back");
  const auto partial = decoder->Decode("br", {}, "en", bounded);
  CHECK(partial.budget_exhausted && partial.suggestions.size() == 1);
  CHECK(partial.suggestions.front().text_utf8 == "be right back");
  CHECK(Near(partial.suggestions.front().match_log_score, -std::log(2.0)));
  bounded.max_model_queries = 4;
  p::PersonalCounts empty;
  const auto with_snapshot = decoder->Decode("brb", {}, "en", bounded, &empty);
  CHECK(with_snapshot.budget_exhausted && with_snapshot.suggestions.size() == 1);
  CHECK(with_snapshot.personal_preparations == 4 && with_snapshot.model_queries == 4);
  bounded.max_model_queries = 2;
  CHECK(decoder->Decode("brb", {}, "en", bounded).suggestions.empty());  // Cannot score 3 words.

  // Conversely, a wide explicit subtree cannot take every model call. Its
  // attested but irrelevant outputs remain scored rather than forcibly promoted.
  std::string source = Header();
  for (unsigned ordinal = 0; ordinal < 100; ++ordinal)
    source += Row("gm" + std::to_string(ordinal), {"be", "right", "back"});
  const auto many = BuildDecoder(model, source);
  a::Options fair;
  fair.max_model_queries = 32;
  const auto mixed = many->Decode("gm", {Id("hi"), Id("say")}, "en", fair);
  CHECK(mixed.budget_exhausted && !mixed.complete);
  CHECK(mixed.explicit_expansions_visited == 6);  // 16-call share scores 5 full paths.
  CHECK(mixed.index_entries_visited > 0 && mixed.model_queries <= 32);
  CHECK(!mixed.suggestions.empty() && mixed.suggestions.front().text_utf8 == "good morning");
  CheckAttested(mixed, *model, "en", fair);
  std::cout << "explicit-first bounded work share prevents both initialism and resource starvation\n";
}

p::PersonalCounts PersonalFlip() {
  p::PersonalCounts personal;
  personal.unigrams = {{Id("great"), 10000}, {Id("meeting"), 10000}};
  personal.bigrams = {{Id("say"), Id("great"), 10000}, {Id("great"), Id("meeting"), 10000}};
  personal.trigrams = {{Id("hi"), Id("say"), Id("great"), 10000},
                        {Id("say"), Id("great"), Id("meeting"), 10000}};
  return personal;
}
void TestPersonalAndConcurrency() {
  const auto records = Fixture();
  const auto model = BuildModel(records);
  const auto decoder = BuildDecoder(model, Header() + Row("zz", {"good", "morning"}) +
                                    Row("zz", {"great", "meeting"}));
  const p::Context context{Id("hi"), Id("say")};
  const auto original = decoder->Decode("gm", context, "en");
  auto personal = PersonalFlip();
  const auto learned = decoder->Decode("gm", context, "en", {}, &personal);
  CHECK(learned.complete && learned.suggestions.front().text_utf8 == "great meeting");
  CHECK(learned.personal_preparations == learned.model_queries);
  CHECK(learned.personal_preparations > 1);  // Honest public Model API call accounting.
  CHECK(original.suggestions.front().text_utf8 == "good morning");
  CHECK(SameResult(original, decoder->Decode("gm", context, "en")));
  CheckOracle(records, *decoder, "gm", {"g", "m"}, context, "en", &personal);
  const auto explicit_learned = decoder->Decode("zz", context, "en", {}, &personal);
  CHECK(explicit_learned.suggestions.front().text_utf8 == "great meeting");
  CHECK(Near(explicit_learned.suggestions.front().log_probability,
             OraclePhrase(records, context, explicit_learned.suggestions.front().lexeme_ids, &personal)));
  auto bad = personal;
  bad.unigrams.push_back(bad.unigrams.front());
  CHECK(decoder->Decode("gm", context, "en", {}, &bad).status == p::Status::kInvalidPersonalCounts);
  CHECK(decoder->Decode("no match", context, "en", {}, &bad).status == p::Status::kInvalidPersonalCounts);
  bad = {}; bad.unigrams = {{1, 5}};
  CHECK(decoder->Decode("gm", context, "en", {}, &bad).status == p::Status::kInvalidPersonalCounts);
  bad = {}; bad.unigrams = {{Id("good"), 0}};
  CHECK(decoder->Decode("gm", context, "en", {}, &bad).status == p::Status::kInvalidPersonalCounts);
  bad = {}; bad.bigrams = {{0, Id("good"), 5}};
  CHECK(decoder->Decode("gm", context, "en", {}, &bad).status == p::Status::kInvalidPersonalCounts);
  bad = {}; bad.unigrams.resize(p::kMaxPersonalRecords + 1, {Id("good"), 1});
  CHECK(decoder->Decode("gm", context, "en", {}, &bad).status == p::Status::kInvalidPersonalCounts);
  bad = {}; bad.unigrams = {{Id("good"), (std::numeric_limits<std::uint64_t>::max)()}, {Id("great"), 1}};
  CHECK(decoder->Decode("gm", context, "en", {}, &bad).status == p::Status::kInvalidPersonalCounts);
  a::Options zero;
  zero.max_model_queries = 0;
  const auto no_validation_work = decoder->Decode("gm", context, "en", zero, &personal);
  CHECK(no_validation_work.budget_exhausted && !no_validation_work.complete);
  CHECK(no_validation_work.personal_preparations == 0 && no_validation_work.suggestions.empty());
  zero = {}; zero.max_suggestions = 0;
  CHECK(decoder->Decode("gm", context, "en", zero, &personal).personal_preparations == 1);
  CHECK(decoder->Decode("gm", context, "en", zero, &bad).status == p::Status::kInvalidPersonalCounts);
  p::PersonalCounts empty;
  const auto empty_counts = decoder->Decode("gm", context, "en", {}, &empty);
  CHECK(empty_counts.suggestions.front().text_utf8 == original.suggestions.front().text_utf8);
  CHECK(empty_counts.suggestions.front().log_probability == original.suggestions.front().log_probability);
  personal.unigrams.clear();
  CHECK(SameResult(original, decoder->Decode("gm", context, "en")));

  std::atomic<bool> valid{true};
  std::vector<std::thread> workers;
  for (unsigned worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&, worker] {
      const auto local_personal = PersonalFlip();
      for (unsigned query = 0; query < 30; ++query) {
        const auto result = decoder->Decode("gm", context, "en", {}, worker % 2 ? &local_personal : nullptr);
        const auto& expected = worker % 2 ? learned : original;
        if (!SameResult(result, expected) || !SameResult(original, decoder->Decode("gm", context, "en")))
          valid = false;
      }
    });
  }
  for (auto& worker : workers) worker.join();
  CHECK(valid.load());
  std::cout << "personal conditional rerank, rejection, no shared learning state and concurrent isolation verified\n";
}

void TestDifferentialAndDeterminism() {
  std::mt19937 random(716031);
  for (unsigned trial = 0; trial < 10; ++trial) {
    Corpus corpus;
    for (unsigned older = 0; older < 2; ++older) {
      for (unsigned first = 0; first < 3; ++first) {
        for (unsigned second = 0; second < 3; ++second) {
          corpus.Sentence({older ? "x" : "y", "say", "g" + std::to_string(first),
                           "m" + std::to_string(second)}, 1 + random() % 100);
        }
      }
    }
    auto records = corpus.Finish();
    const auto model = BuildModel(records);
    const auto decoder = BuildDecoder(model);
    p::PersonalCounts personal;
    personal.unigrams = {{Id("g2"), 30 + trial}, {Id("m2"), 80 + trial}};
    personal.bigrams = {{Id("g2"), Id("m2"), 20 + trial}};
    personal.trigrams = {{Id("x"), Id("say"), Id("g2"), 50 + trial}};
    for (const auto& context : std::vector<p::Context>{{}, {Id("say")}, {Id("x"), Id("say")},
                                                      {Id("y"), Id("say")}}) {
      CheckOracle(records, *decoder, "gm", {"g", "m"}, context, "en");
      CheckOracle(records, *decoder, "gm", {"g", "m"}, context, "en", &personal);
    }
    const auto expected = decoder->Decode("gm", {Id("x"), Id("say")}, "en");
    std::shuffle(records.lexicon.begin(), records.lexicon.end(), random);
    std::shuffle(records.unigrams.begin(), records.unigrams.end(), random);
    std::shuffle(records.bigrams.begin(), records.bigrams.end(), random);
    std::shuffle(records.trigrams.begin(), records.trigrams.end(), random);
    const auto shuffled = BuildDecoder(BuildModel(records));
    CHECK(SameResult(expected, shuffled->Decode("gm", {Id("x"), Id("say")}, "en")));
    a::Options budget;
    budget.max_model_queries = 2;
    CHECK(SameResult(decoder->Decode("gm", {Id("x"), Id("say")}, "en", budget),
                     shuffled->Decode("gm", {Id("x"), Id("say")}, "en", budget)));
  }
  Corpus tied;
  for (const auto& word : {"g1", "g2", "m1", "m2"}) tied.Sentence({word}, 5);
  const auto records = tied.Finish();
  const auto decoder = BuildDecoder(BuildModel(records));
  CheckOracle(records, *decoder, "gm", {"g", "m"}, {}, "en");
  const auto result = decoder->Decode("gm", {}, "en");
  CHECK(result.complete && result.suggestions.size() == 4);
  for (std::size_t ordinal = 1; ordinal < result.suggestions.size(); ++ordinal) {
    CHECK(result.suggestions[ordinal - 1].log_probability == result.suggestions[ordinal].log_probability);
    CHECK(result.suggestions[ordinal - 1].lexeme_ids < result.suggestions[ordinal].lexeme_ids);
  }
  std::cout << "seeded independent exhaustive probability oracle and stable input-order/tie sorting verified\n";
}

int ProbeProduction(int argc, char** argv) {
  // Developer-only PUBLIC TEXT probe, not a runtime logging API. No query/file
  // writes here; callers may redirect this public-fixture report only into build.
  // This mode is deliberately separate from --unit and rejects test models.
  if (argc < 8 || argc > 10 || std::string(argv[2]) != "--public-fixture") {
    std::cerr << "Expected --probe --public-fixture <production-model> <en|fr|zh|ja|ko> "
                 "<shorthand> <model|combined|explicit> <default|tight|calls|wide> [older] [newer]\n";
    return 2;
  }
  const auto load_begin = std::chrono::steady_clock::now();
  p::LoadOptions load;
  p::BuildInfo model_info;
  const auto model = p::Model::Load(std::filesystem::u8path(argv[3]), load, &model_info);
  const auto load_end = std::chrono::steady_clock::now();
  if (!model || model->metadata().source != p::SourceKind::kProductionCorpus) {
    std::cerr << "Production model required; status=" << p::StatusName(model_info.status) << '\n';
    return 1;
  }
  const std::string language_id = argv[4];
  const std::string shorthand_utf8 = argv[5];
  const std::string mode = argv[6];
  const std::string budget = argv[7];
  if (mode != "model" && mode != "combined" && mode != "explicit") return 2;
  a::Options options;
  if (budget == "tight") {
    options.max_model_queries = 8;
    options.max_index_entries = 256;
  } else if (budget == "calls") {
    options.max_model_queries = 3;
  } else if (budget == "wide") {
    // Predetermined upper-bound diagnostic, not tuning against heldout outcomes.
    options.max_model_queries = a::kMaxModelQueries;
    options.max_index_entries = a::kMaxIndexEntries;
    options.beam_width = 63;
    options.max_explicit_expansions = a::kMaxResourceRecords;
    options.max_trie_nodes = a::kMaxResourceTrieNodes;
  } else if (budget != "default") return 2;
  options.enable_initialism = mode != "explicit";
  p::Context context;
  for (int argument_ordinal = 8; argument_ordinal < argc; ++argument_ordinal) {
    std::string normalized_utf8;
    if (!p::NormalizeLexemeUtf8(argv[argument_ordinal], &normalized_utf8)) return 2;
    const auto* row = model->Find(language_id, normalized_utf8);
    if (!row) { std::cerr << "Public context token is OOV\n"; return 1; }
    context.push_back(row->id);
  }
  std::string source;
  std::size_t source_fixture_oov = 0;
  if (mode != "model") {
    source = Header();
    const auto append = [&](const std::string& key, const std::vector<std::string>& words) {
      for (const auto& word : words) {
        const auto* row = model->Find(language_id, word);
        if (!row || row->kind != p::TokenKind::kWord) { ++source_fixture_oov; return; }
      }
      source += Row(key, words, language_id);
    };
    // Authored PUBLIC diagnostic fixture. These are resource INPUT rows, not
    // handcoded decoder results, and no gm mapping is present in this fixture.
    if (language_id == "en") {
      append("brb", {"be", "right", "back"});
      append("zz", {"good", "morning"});
      append("zz", {"great", "meeting"});
    } else if (language_id == "fr") {
      append("stp", {"s'il", "te", u8"plaît"});
      append("mrc", {"merci", "beaucoup"});
    } else if (language_id == "ko") {
      append("thx", {u8"정말", u8"감사합니다"});
      append("hi", {u8"안녕하세요"});
    } else if (language_id != "zh" && language_id != "ja") return 2;
  }
  a::BuildInfo decoder_info;
  const auto decoder = a::Decoder::Build(model, source, {}, &decoder_info);
  if (!decoder) {
    std::cerr << "Public resource rejected status=" << p::StatusName(decoder_info.status) << '\n';
    return 1;
  }
  const auto first_begin = std::chrono::steady_clock::now();
  const auto first = decoder->Decode(shorthand_utf8, context, language_id, options);
  const auto first_end = std::chrono::steady_clock::now();
  if (first.status != p::Status::kOk && first.status != p::Status::kBeamPruned &&
      first.status != p::Status::kBudgetExhausted) {
    std::cerr << "Public query rejected status=" << p::StatusName(first.status) << '\n';
    return 1;
  }
  std::vector<double> elapsed_ms;
  for (unsigned iteration = 0; iteration < 21; ++iteration) {
    const auto begin = std::chrono::steady_clock::now();
    const auto current = decoder->Decode(shorthand_utf8, context, language_id, options);
    const auto end = std::chrono::steady_clock::now();
    if (!SameResult(first, current)) { std::cerr << "Nondeterministic public probe\n"; return 1; }
    elapsed_ms.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
  }
  std::sort(elapsed_ms.begin(), elapsed_ms.end());
  CheckAttested(first, *model, language_id, options);
  std::cout << std::setprecision(9) << "PUBLIC_FIXTURE_PROBE language=" << language_id
            << " mode=" << mode << " budget=" << budget << " source=production-corpus"
            << " lexemes=" << model->lexemes().size() << " resource_rows="
            << decoder->explicit_expansion_count() << " resource_oov=" << source_fixture_oov << '\n';
  std::cout << "model_load_ms=" << std::chrono::duration<double, std::milli>(load_end - load_begin).count()
            << " first_decode_ms=" << std::chrono::duration<double, std::milli>(first_end - first_begin).count()
            << " repeated=21 p50_ms=" << elapsed_ms[10] << " p95_ms=" << elapsed_ms[19]
            << " max_ms=" << elapsed_ms.back() << '\n';
  std::cout << "status=" << p::StatusName(first.status) << " complete=" << first.complete
            << " budget_exhausted=" << first.budget_exhausted << " beam_pruned=" << first.beam_pruned
            << " model_queries=" << first.model_queries << " index_entries=" << first.index_entries_visited
            << " trie_nodes=" << first.trie_nodes_visited
            << " explicit_expansions=" << first.explicit_expansions_visited
            << " candidates=" << first.suggestions.size() << '\n';
  for (std::size_t ordinal = 0; ordinal < first.suggestions.size(); ++ordinal) {
    const auto& suggestion = first.suggestions[ordinal];
    std::cout << ordinal + 1 << '\t' << suggestion.text_utf8 << '\t' << suggestion.log_probability
              << '\t' << suggestion.match_log_score << '\t' << suggestion.explicit_key
              << '\t' << suggestion.exact_key << '\n';
  }
  return 0;  // Success means probe executed, NOT language-quality acceptance.
}

}  // namespace

int MainUtf8(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--probe") {
    try { return ProbeProduction(argc, argv); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
  }
  if (argc == 2 && std::string(argv[1]) == "--forcefail") {
    try { CHECK(false); } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 3;
  }
  if (argc != 2 || std::string(argv[1]) != "--unit") {
    std::cerr << "Expected --unit or --forcefail\n";
    return 2;
  }
  try {
    TestContextAndPrefixes();
    TestMultilingualScalars();
    TestExplicitTrie();
    TestParserAndInvalidInputs();
    TestBudgetsAndOutputLimits();
    TestExplicitScheduling();
    TestPersonalAndConcurrency();
    TestDifferentialAndDeterminism();
    std::cout << "RESULT PASS prediction abbreviation SYNTHETIC FIXTURE checks=" << checks << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    std::cerr << "RESULT FAIL prediction abbreviation checks=" << checks << '\n';
    return 1;
  }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv_utf16) {
  // Native Windows argv is UTF-16, NOT UTF-8. Convert with strict UTF-8 output;
  // no active-codepage assumptions or system changes (test executable only).
  try {
    std::vector<std::string> args_utf8;
    std::vector<char*> argv_utf8;
    args_utf8.reserve(static_cast<std::size_t>(argc));
    argv_utf8.reserve(static_cast<std::size_t>(argc));
    for (int argument_ordinal = 0; argument_ordinal < argc; ++argument_ordinal) {
      const auto required_utf8_bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
          argv_utf16[argument_ordinal], -1, nullptr, 0, nullptr, nullptr);
      if (required_utf8_bytes <= 0) return 2;
      std::string argument_utf8(static_cast<std::size_t>(required_utf8_bytes), '\0');
      if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv_utf16[argument_ordinal], -1,
            argument_utf8.data(), required_utf8_bytes, nullptr, nullptr) != required_utf8_bytes)
        return 2;
      argument_utf8.pop_back();  // Remove only the conversion's NUL terminator.
      args_utf8.push_back(std::move(argument_utf8));
    }
    for (auto& argument_utf8 : args_utf8) argv_utf8.push_back(argument_utf8.data());
    return MainUtf8(argc, argv_utf8.data());
  } catch (const std::exception& error) {
    std::cerr << "Invalid UTF-16 command argument: " << error.what() << '\n';
    return 2;
  }
}
#else
int main(int argc, char** argv) { return MainUtf8(argc, argv); }
#endif
