#include "lexical_service.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "english_token.h"
#include "language_support.h"
#include "model.h"

namespace {
namespace lx = epocanva::lexical;
namespace prediction = epocanva::prediction;
int checks = 0;
int failures = 0;
void Check(bool condition, const char* label) {
  ++checks;
  if (!condition) {
    ++failures;
    std::cerr << "ASSERT FAIL " << label << '\n';
  }
}

using Row = std::tuple<std::string, std::string, std::uint64_t>;
prediction::LexemeId Id(std::string_view language, std::string_view text) {
  std::string normalized;
  if (!prediction::NormalizeLexemeUtf8(text, &normalized)) return 0;
  return prediction::StableLexemeId(language, normalized);
}
prediction::ModelRecords Records(const std::vector<Row>& rows) {
  prediction::ModelRecords records;
  records.metadata.source = prediction::SourceKind::kTestFixture;
  records.metadata.source_fingerprint.assign(64, 'a');
  for (const auto& row : rows) {
    std::string normalized;
    if (!prediction::NormalizeLexemeUtf8(std::get<1>(row), &normalized)) {
      throw std::runtime_error("invalid project-authored fixture");
    }
    const auto id = prediction::StableLexemeId(std::get<0>(row), normalized);
    records.lexicon.push_back({id, std::get<0>(row), normalized, prediction::TokenKind::kWord});
    if (std::get<2>(row) != 0) records.unigrams.push_back({id, std::get<2>(row)});
    records.metadata.unigram_total += std::get<2>(row);
  }
  records.metadata.lexeme_count = records.lexicon.size();
  return records;
}
std::shared_ptr<const prediction::Model> Model(const std::vector<Row>& rows) {
  prediction::BuildInfo info;
  auto model = prediction::Model::FromRecords(Records(rows), &info);
  Check(model != nullptr && info.status == prediction::Status::kOk, "fixture model is valid");
  if (!model) throw std::runtime_error("fixture model construction failed");
  return model;
}
std::unique_ptr<lx::LexicalService> Service(const std::vector<Row>& rows, lx::BuildInfo* info = nullptr) {
  const auto model = Model(rows);
  auto service = lx::LexicalService::Create(*model, info);
  Check(service != nullptr, "provider builds from real immutable model");
  if (!service) throw std::runtime_error("fixture provider construction failed");
  return service;  // model deliberately dies: provider must own its copied index
}
lx::QueryOptions OnlyPrefix() {
  lx::QueryOptions options;
  options.enable_morphology = false;
  options.enable_spelling = false;
  options.enable_segmentation = false;
  options.enable_abbreviation = false;
  options.enable_accents = false;
  options.max_candidates = lx::kMaxCandidates;
  return options;
}
const lx::Candidate* Find(const lx::QueryResult& result, std::string_view text,
                           lx::CandidateType type) {
  const auto found = std::find_if(result.candidates.begin(), result.candidates.end(),
      [&](const lx::Candidate& candidate) { return candidate.text_utf8 == text && candidate.type == type; });
  return found == result.candidates.end() ? nullptr : &*found;
}
void CheckRaw(const lx::QueryResult& result, std::string_view raw) {
  Check(!result.candidates.empty(), "raw is present");
  if (result.candidates.empty()) return;
  const auto& candidate = result.candidates.front();
  Check(candidate.type == lx::CandidateType::kOriginal, "raw is explicitly original");
  Check(candidate.text_utf8 == raw, "raw bytes preserved exactly");
  Check(candidate.lexeme_ids.empty(), "raw never has a learnable ID");
  Check(candidate.input_utf8_byte_spans.size() == 1 &&
        candidate.input_utf8_byte_spans.front().begin_utf8_byte == 0 &&
        candidate.input_utf8_byte_spans.front().end_utf8_byte == raw.size(),
        "raw span uses original UTF8 bytes");
}
void CheckKnown(const lx::LexicalService& service, const lx::QueryResult& result,
                std::string_view raw) {
  Check(result.candidates.size() <= lx::kMaxCandidates, "hard candidate limit");
  for (const auto& candidate : result.candidates) {
    if (candidate.type == lx::CandidateType::kOriginal) continue;
    Check(!candidate.lexeme_ids.empty(), "non-original has known ID vector");
    for (const auto id : candidate.lexeme_ids) Check(id != 0 && service.HasLexeme(id), "every ID is admitted");
    Check(lx::ValidateUtf8(candidate.text_utf8) == lx::TextStatus::kOk, "candidate output is valid UTF8");
    Check(std::isfinite(candidate.score), "candidate score finite");
    for (const auto span : candidate.input_utf8_byte_spans) {
      Check(span.begin_utf8_byte <= span.end_utf8_byte && span.end_utf8_byte <= raw.size() &&
            lx::IsUtf8ByteBoundary(raw, span.begin_utf8_byte) &&
            lx::IsUtf8ByteBoundary(raw, span.end_utf8_byte), "candidate spans bound original UTF8 bytes");
    }
  }
}

void Text() {
  Check(lx::NormalizeNfcUtf8("Plain").text_utf8 == "Plain", "ASCII normalization preserves text");
  Check(lx::FoldUtf8("DON'T").text_utf8 == "don't", "ASCII lower preserves punctuation");
  Check(lx::UpperUtf8("co-operate").text_utf8 == "CO-OPERATE", "ASCII uppercase");
  Check(lx::NormalizeNfcUtf8(std::string("a\0b", 3)).text_utf8 == std::string("a\0b", 3),
        "helper explicit counts preserve embedded NUL");
  const std::vector<std::string> invalid = {
      std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
      std::string("\xf4\x90\x80\x80", 4), std::string("\xe2\x82", 2),
      std::string("\x80", 1), std::string("\xff", 1)};
  for (const auto& text : invalid) {
    std::size_t error = lx::kNoUtf8Byte;
    Check(lx::ValidateUtf8("a" + text, &error) == lx::TextStatus::kInvalidUtf8 && error == 1,
          "strict UTF8 rejects invalid scalar at lead byte");
    const auto normalized = lx::NormalizeNfcUtf8(text);
    Check(normalized.status == lx::TextStatus::kInvalidUtf8 && normalized.text_utf8.empty(),
          "normalization never repairs invalid UTF8");
  }
  Check(lx::ValidateUtf8(std::string(lx::kMaxTextUtf8Bytes + 1, 'a')) == lx::TextStatus::kInputTooLong,
        "text helper byte bound");
  Check(lx::AnalyzeWordUtf8("iPhone").status == lx::TextStatus::kMixedCase, "ASCII brand case protected");
  Check(lx::AnalyzeWordUtf8("co-").status == lx::TextStatus::kInvalidWord &&
        lx::AnalyzeWordUtf8("co-", true).status == lx::TextStatus::kOk, "trailing joiner only interactive");
  for (const auto text : {"user@host", "a.b", "x_foo", "http://site", "word2", "x/y", "f+g"}) {
    Check(lx::AnalyzeWordUtf8(text).status == lx::TextStatus::kProtectedText,
          "technical ASCII token refused whole");
  }
  for (const auto text : {"a b", "-word", "a--b", "a\tb", "a!b"}) {
    Check(lx::AnalyzeWordUtf8(text).status == lx::TextStatus::kInvalidWord, "word grammar rejects noise");
  }
  Check(lx::AnalyzeWordUtf8(std::string("a\0b", 3)).status == lx::TextStatus::kInvalidWord,
        "NUL not a lexical word");
  if (!lx::HasNativeUnicodeSupport()) {
    Check(lx::NormalizeNfcUtf8(u8"é").status == lx::TextStatus::kUnicodeUnavailable,
          "portable path explicitly has no Unicode normalization");
    return;
  }
  Check(lx::NormalizeNfcUtf8(u8"e\u0301").text_utf8 == u8"é", "native NFC composes accent");
  Check(lx::NormalizeNfcUtf8(u8"a\u0301\u0327").text_utf8 ==
        lx::NormalizeNfcUtf8(u8"a\u0327\u0301").text_utf8, "native NFC reorders canonical marks");
  Check(lx::NormalizeNfcUtf8(u8"\u1100\u1161").text_utf8 == u8"가", "native NFC composes Hangul Jamo");
  Check(lx::NormalizeNfcUtf8(u8"\ufb01").text_utf8 == u8"\ufb01", "NFC is not compatibility fold");
  Check(lx::FoldUtf8(u8"ÉCOLE").text_utf8 == u8"école", "native accented uppercase lower");
  Check(lx::FoldUtf8(u8"Straße").text_utf8 != "strasse", "lower mapping not falsely full casefold");
  Check(lx::NormalizeNfcUtf8(u8"\U0001f600").text_utf8 == u8"\U0001f600", "supplementary scalar helper support");
  Check(lx::AnalyzeWordUtf8(u8"cAfé").status == lx::TextStatus::kMixedCase,
        "Unicode mixed-case raw-only policy");
  Check(lx::AnalyzeWordUtf8(u8"CAFÉ").case_style == lx::CaseStyle::kUpper, "native uppercase shape");
  Check(lx::AnalyzeWordUtf8(u8"Cafe\u0301").case_style == lx::CaseStyle::kInitialCapital,
        "decomposed title case recognized");
  Check(lx::AnalyzeWordUtf8(u8"한국어").case_style == lx::CaseStyle::kUncased, "Korean uncased shape");
  Check(lx::AnalyzeWordUtf8(u8"hello\U0001f600").status == lx::TextStatus::kInvalidWord,
        "emoji not treated as a dictionary letter");
  Check(lx::AnalyzeWordUtf8(u8"\u0301a").status == lx::TextStatus::kInvalidWord,
        "leading combining mark not a word letter");
  Check(lx::AnalyzeWordUtf8(u8"a\u00b4b").status == lx::TextStatus::kInvalidWord,
        "spacing diacritic not misclassified as combining mark");
  Check(lx::AnalyzeWordUtf8(u8"ab\u0301").status == lx::TextStatus::kOk,
        "noncomposing combining mark after letter accepted");
  Check(lx::IsUtf8ByteBoundary(u8"é한", 0) && !lx::IsUtf8ByteBoundary(u8"é한", 1) &&
        lx::IsUtf8ByteBoundary(u8"é한", 2) && !lx::IsUtf8ByteBoundary(u8"é한", 3) &&
        !lx::IsUtf8ByteBoundary(u8"é한", 4) && lx::IsUtf8ByteBoundary(u8"é한", 5),
        "UTF8 byte boundaries not scalar/grapheme ordinals");
}

void Aliases() {
  Check(Id("en", "hello") == UINT64_C(5840500155624144132), "independent frozen ID test vector");
  const auto unavailable = prediction::Model::Unavailable();
  lx::BuildInfo unavailable_info;
  Check(!lx::LexicalService::Create(*unavailable, &unavailable_info) &&
        unavailable_info.status == lx::BuildStatus::kEmptyCatalog, "unavailable model cannot fake dictionary");
  auto english = Service({{"en", "hello", 10}, {"ja", u8"日本", 1}, {"en", "name@example", 1}});
  Check(english->size() == 1, "unsupported and technical catalog rows excluded");
  Check(english->FindExact("en", "Hello") == Id("en", "hello"), "ASCII lookup resolves shared ID");
  Check(english->FindExact("en", "unknown") == 0 && !english->HasLexeme(0), "unknown never receives ID");
  if (!lx::HasNativeUnicodeSupport()) return;
  lx::BuildInfo info;
  auto service = Service({{"fr", u8"école", 10}, {"fr", u8"École", 50},
                          {"fr", u8"e\u0301cole", 25}, {"en", u8"café", 9}}, &info);
  Check(info.alias_collisions == 2 && info.catalog_entries == 4, "alias collisions keep all original IDs");
  Check(service->FindExact("fr", u8"école") == Id("fr", u8"école"), "exact model lowercase wins over frequency");
  Check(service->FindExact("fr", u8"École") == Id("fr", u8"École"), "exact model accented uppercase ID preserved");
  Check(service->FindExact("fr", u8"e\u0301cole") == Id("fr", u8"e\u0301cole"), "exact decomposed row ID preserved");
  Check(service->FindExact("fr", u8"éCOLE") == 0, "mixed case does not become a learnable raw lookup");
  const auto result = service->Suggest("fr", u8"E\u0301COL", OnlyPrefix());
  CheckRaw(result, u8"E\u0301COL");
  const auto* candidate = Find(result, u8"ÉCOLE", lx::CandidateType::kCompletion);
  Check(candidate && candidate->lexeme_ids == std::vector<lx::LexemeId>{Id("fr", u8"École")},
        "completion alias uses highest-count known ID without rehash");
  CheckKnown(*service, result, u8"E\u0301COL");
  auto alias_only = Service({{"fr", u8"École", 3}, {"en", u8"e\u0301lan", 2}});
  Check(alias_only->FindExact("fr", u8"école") == Id("fr", u8"École"), "alias can resolve known uppercase catalog");
  Check(alias_only->FindExact("en", u8"élan") == Id("en", u8"e\u0301lan"), "NFC query resolves decomposed catalog ID");
  auto tied = Service({{"fr", u8"école", 7}, {"fr", u8"e\u0301cole", 7}});
  const auto tie = tied->Suggest("fr", u8"éco", OnlyPrefix());
  const auto* tied_candidate = Find(tie, u8"école", lx::CandidateType::kCompletion);
  Check(tied_candidate && tied_candidate->lexeme_ids.front() ==
        std::min(Id("fr", u8"école"), Id("fr", u8"e\u0301cole")), "alias tie broken by stable ID");
  const auto exact = service->Suggest("fr", u8"école", OnlyPrefix());
  CheckRaw(exact, u8"école");
  const auto* known = Find(exact, u8"école", lx::CandidateType::kExact);
  Check(known && known->lexeme_ids == std::vector<lx::LexemeId>{Id("fr", u8"école")},
        "exact candidate identity separate from identical raw display");
}

void English() {
  auto service = Service({{"en", "hello", 100}, {"en", "help", 20}, {"en", "world", 100},
      {"en", "word", 90}, {"en", "walk", 100}, {"en", "walks", 10}, {"en", "walked", 10},
      {"en", "walking", 10}, {"en", "child", 50}, {"en", "children", 10},
      {"en", "cat", 40}, {"en", "cats", 30}, {"en", "walkaway", 5},
      {"en", "don't", 60}, {"en", "by", 40}, {"en", "the", 200}, {"en", "way", 80}});
  const auto prefix = service->Suggest("en", "Hel", OnlyPrefix());
  CheckRaw(prefix, "Hel");
  Check(Find(prefix, "Hello", lx::CandidateType::kCompletion) != nullptr, "English case-preserving real completion");
  CheckKnown(*service, prefix, "Hel");
  auto options = OnlyPrefix();
  options.enable_morphology = true;
  const auto morph = service->Suggest("en", "walk", options);
  for (const auto word : {"walks", "walked", "walking", "walk's"}) {
    const auto* candidate = Find(morph, word, lx::CandidateType::kMorphology);
    Check(candidate && candidate->lexeme_ids == std::vector<lx::LexemeId>{Id("en", "walk")},
          "existing English forms map admitted lemma to shared ID");
    if (candidate) Check(candidate->lexeme_ids.front() != epocanva::en::StableLexemeId("walk"),
                         "legacy IDs never leaked to backend catalog");
  }
  const auto irregular = service->Suggest("en", "child", options);
  Check(Find(irregular, "children", lx::CandidateType::kMorphology) != nullptr, "existing irregular English morphology");
  const auto cats = service->Suggest("en", "cat", options);
  Check(Find(cats, "cats", lx::CandidateType::kMorphology) != nullptr &&
        Find(cats, "cat's", lx::CandidateType::kMorphology) != nullptr &&
        Find(cats, "cats'", lx::CandidateType::kMorphology) != nullptr,
        "known plural and actual-base possessives preserved");
  const auto plural_input = service->Suggest("en", "cats", options);
  Check(Find(plural_input, "catses'", lx::CandidateType::kMorphology) == nullptr &&
        Find(plural_input, "catsed", lx::CandidateType::kMorphology) == nullptr,
        "plural catalog row never justifies unverified reinflection");
  Check(Find(service->Suggest("en", "hello", options), "helloing", lx::CandidateType::kMorphology) == nullptr,
        "known nonlemma never justifies unattested form");
  Check(Find(service->Suggest("en", "walkaway", options), "walkawayed", lx::CandidateType::kMorphology) == nullptr,
        "unattested rule-generated output filtered");
  options = OnlyPrefix();
  options.enable_spelling = true;
  const auto spelling = service->Suggest("en", "wrod", options);
  const auto* corrected = Find(spelling, "word", lx::CandidateType::kSpelling);
  Check(corrected && corrected->edit_distance == 1 &&
        corrected->lexeme_ids == std::vector<lx::LexemeId>{Id("en", "word")}, "existing English OSA spelling maps shared ID");
  options = OnlyPrefix();
  options.enable_segmentation = true;
  const auto segmented = service->Suggest("en", "helloworld", options);
  const auto* split = Find(segmented, "hello world", lx::CandidateType::kSegmentation);
  Check(split && split->lexeme_ids == std::vector<lx::LexemeId>{Id("en", "hello"), Id("en", "world")},
        "existing segmentation uses real frequency-covered words");
  Check(split && split->input_utf8_byte_spans.size() == 2 &&
        split->input_utf8_byte_spans[0].begin_utf8_byte == 0 &&
        split->input_utf8_byte_spans[0].end_utf8_byte == 5 &&
        split->input_utf8_byte_spans[1].begin_utf8_byte == 5 &&
        split->input_utf8_byte_spans[1].end_utf8_byte == 10, "English segmentation original byte spans");
  CheckKnown(*service, segmented, "helloworld");
  options = OnlyPrefix();
  options.enable_abbreviation = true;
  const auto expanded = service->Suggest("en", "btw", options);
  const auto* abbreviation = Find(expanded, "by the way", lx::CandidateType::kAbbreviation);
  Check(abbreviation && abbreviation->lexeme_ids.size() == 3, "existing abbreviation with all known output IDs");
  const auto contraction = service->Suggest("en", "dont", options);
  Check(Find(contraction, "don't", lx::CandidateType::kContraction) != nullptr, "existing contraction known lexicon ID");
  auto missing_expansion = Service({{"en", "by", 5}, {"en", "the", 10}});
  Check(Find(missing_expansion->Suggest("en", "btw", options), "by the way", lx::CandidateType::kAbbreviation) == nullptr,
        "unattested abbreviation output not fabricated");
  for (const auto raw : {"iPhone", "HeL", "user@host", "foo_thing", "hello2", "http://hello"}) {
    const auto result = service->Suggest("en", raw);
    CheckRaw(result, raw);
    Check(result.candidates.size() == 1 && service->FindExact("en", raw) == 0,
          "technical and mixed case get no rewrites or learning identity");
  }
  if (!lx::HasNativeUnicodeSupport()) return;
  auto unicode = Service({{"en", u8"café", 50}, {"en", u8"cafétéria", 1},
                          {"en", u8"naïve", 20}, {"en", "hello", 1}});
  options = OnlyPrefix();
  options.enable_accents = true;
  const auto accented = unicode->Suggest("en", "Cafe", options);
  CheckRaw(accented, "Cafe");
  Check(Find(accented, u8"Café", lx::CandidateType::kAccent) != nullptr, "Unicode English accent lexical suggestion");
  const auto normalized = unicode->Suggest("en", u8"CAFE\u0301", options);
  CheckRaw(normalized, u8"CAFE\u0301");
  Check(Find(normalized, u8"CAFÉTÉRIA", lx::CandidateType::kCompletion) != nullptr,
        "Unicode English completion native case/NFC");
  CheckKnown(*unicode, normalized, u8"CAFE\u0301");
}

void French() {
  if (!lx::HasNativeUnicodeSupport()) return;
  auto service = Service({{"fr", u8"école", 100}, {"fr", u8"écologie", 50}, {"fr", u8"élève", 80},
      {"fr", "cheval", 20}, {"fr", "chevaux", 30}, {"fr", "bal", 10},
      {"fr", "chapeau", 20}, {"fr", "chapeaux", 40}, {"fr", "chat", 10}, {"fr", "chats", 20},
      {"fr", u8"l'école", 5}});
  const auto prefix = service->Suggest("fr", u8"Éco", OnlyPrefix());
  CheckRaw(prefix, u8"Éco");
  Check(Find(prefix, u8"École", lx::CandidateType::kCompletion) != nullptr, "French real accented prefix");
  auto options = OnlyPrefix();
  options.enable_accents = true;
  const auto accent = service->Suggest("fr", "Eleve", options);
  Check(Find(accent, u8"Élève", lx::CandidateType::kAccent) != nullptr, "existing French multi-accent suggestion");
  const auto keep_accent = service->Suggest("fr", u8"éleve", options);
  Check(Find(keep_accent, u8"élève", lx::CandidateType::kAccent) != nullptr, "existing accent preserved while adding another");
  const auto wrong_accent = service->Suggest("fr", u8"élevé", options);
  Check(Find(wrong_accent, u8"élève", lx::CandidateType::kAccent) == nullptr, "accent matcher never silently replaces existing accent");
  options = OnlyPrefix();
  options.enable_morphology = true;
  const auto plural = service->Suggest("fr", "cheval", options);
  Check(Find(plural, "chevaux", lx::CandidateType::kMorphology) != nullptr,
        "French -al plural requires real admitted alternative");
  const auto unbacked = service->Suggest("fr", "bal", options);
  Check(Find(unbacked, "baux", lx::CandidateType::kMorphology) == nullptr,
        "French lexical exception not guessed when output absent");
  CheckKnown(*service, plural, "cheval");
  for (const auto raw : {u8"écoLe", u8"école@example", u8"école2", u8"Éco_le"}) {
    const auto result = service->Suggest("fr", raw);
    CheckRaw(result, raw);
    Check(result.candidates.size() == 1, "French Unicode mixed/technical raw preserved");
  }
}

void Korean() {
  if (!lx::HasNativeUnicodeSupport()) return;
  auto service = Service({{"ko", u8"한국어", 100}, {"ko", u8"한국", 50},
                          {"ko", u8"한글", 70}, {"ko", u8"가방", 5}, {"en", "hello", 20}});
  const auto result = service->Suggest("ko", u8"한", OnlyPrefix());
  CheckRaw(result, u8"한");
  const auto* candidate = Find(result, u8"한국어", lx::CandidateType::kCompletion);
  Check(candidate && candidate->lexeme_ids == std::vector<lx::LexemeId>{Id("ko", u8"한국어")},
        "Korean completion needs actual catalog row");
  CheckKnown(*service, result, u8"한");
  // Independent exhaustive oracle over this small multilingual catalog checks
  // UTF8 prefix bounds and range-max extraction for different prefixes/limits.
  const std::vector<std::pair<std::string, std::uint64_t>> known_words = {
      {u8"한국어", 100}, {u8"한국", 50}, {u8"한글", 70}, {u8"가방", 5}};
  for (const auto prefix : {u8"한", u8"한국", u8"가", u8"없는"}) {
    std::vector<std::pair<std::string, std::uint64_t>> expected;
    for (const auto& word : known_words) {
      if (word.first != prefix && word.first.compare(0, std::string(prefix).size(), prefix) == 0) {
        expected.push_back(word);
      }
    }
    std::sort(expected.begin(), expected.end(), [](const auto& left, const auto& right) {
      return left.second != right.second ? left.second > right.second : left.first < right.first;
    });
    const auto actual = service->Suggest("ko", prefix, OnlyPrefix());
    std::vector<std::string> actual_words;
    for (const auto& item : actual.candidates) {
      if (item.type == lx::CandidateType::kCompletion) actual_words.push_back(item.text_utf8);
    }
    std::vector<std::string> expected_words;
    for (const auto& word : expected) expected_words.push_back(word.first);
    Check(actual_words == expected_words && !actual.prefix_budget_exhausted,
          "Unicode prefix index matches independent exhaustive oracle");
  }
  const auto decomposed = service->Suggest("ko", u8"\u1100\u1161", OnlyPrefix());
  CheckRaw(decomposed, u8"\u1100\u1161");
  const auto* normalized = Find(decomposed, u8"가방", lx::CandidateType::kCompletion);
  Check(normalized && normalized->input_utf8_byte_spans.front().end_utf8_byte == 6,
        "Korean NFC query still spans original six UTF8 bytes");
  Check(service->Suggest("ko", u8"없는", OnlyPrefix()).candidates.size() == 1,
        "absent Korean prefix has no fictional completions");
  auto no_korean = Service({{"en", "hello", 10}});
  const auto absent = no_korean->Suggest("ko", u8"한");
  Check(absent.status == lx::QueryStatus::kDictionaryUnavailable && absent.candidates.size() == 1,
        "missing Korean dictionary status explicit");
}

void Ranking() {
  auto service = Service({{"en", "alpha", 100}, {"en", "alpine", 1}, {"en", "before", 10}});
  const auto options = OnlyPrefix();
  auto base = service->Suggest("en", "al", options);
  Check(base.candidates.size() == 3 && base.candidates[1].text_utf8 == "alpha", "base corpus counts rank prefix");
  std::vector<lx::LearnedCount> learned{{Id("en", "alpine"), 1000000}};
  std::vector<lx::ContextCount> context{{Id("en", "before"), Id("en", "alpine"), 1000000}};
  lx::RankingInputs input;
  input.allow_learning = true;
  input.previous_id = Id("en", "before");
  input.learned = &learned;
  input.context = &context;
  const auto personalized = service->Suggest("en", "al", options, input);
  Check(personalized.candidates[1].text_utf8 == "alpine", "ID snapshot learned/context changes ranking");
  auto bounded_options = options;
  bounded_options.prefix_entry_budget = 1;
  const auto personal_bounded = service->Suggest("en", "al", bounded_options, input);
  Check(personal_bounded.candidates.size() == 2 && personal_bounded.candidates[1].text_utf8 == "alpine" &&
        personal_bounded.prefix_entries_examined == 1 && personal_bounded.prefix_budget_exhausted,
        "known personal IDs reach prefix pool under small hard budget");
  input.allow_learning = false;
  // These deliberately invalid addresses must never be dereferenced while
  // learning is gated off. Any accidental read crashes this negative guard.
  input.learned = reinterpret_cast<const std::vector<lx::LearnedCount>*>(std::uintptr_t{1});
  input.context = reinterpret_cast<const std::vector<lx::ContextCount>*>(std::uintptr_t{1});
  const auto private_field = service->Suggest("en", "al", options, input);
  Check(private_field.ranking_inputs_valid && private_field.candidates[1].text_utf8 == "alpha",
        "privacy-off snapshot pointers never touched");
  input.allow_learning = true;
  learned.push_back(learned.front());
  input.learned = &learned;
  input.context = &context;
  const auto duplicate = service->Suggest("en", "al", options, input);
  Check(!duplicate.ranking_inputs_valid && duplicate.candidates[1].text_utf8 == "alpha",
        "duplicate learned IDs reject whole snapshot including context");
  learned.assign(lx::kMaxRankingRecords + 1, {Id("en", "alpine"), 1});
  const auto oversized = service->Suggest("en", "al", options, input);
  Check(!oversized.ranking_inputs_valid && oversized.candidates[1].text_utf8 == "alpha",
        "oversized snapshot cannot affect ranking");
  learned = {{UINT64_C(12345678), 1000000}};
  input.context = nullptr;
  const auto unknown = service->Suggest("en", "al", options, input);
  Check(unknown.ranking_inputs_valid && unknown.candidates[1].text_utf8 == "alpha",
        "unknown personal IDs ignored without enrollment");
  Check(service->FindExact("en", "private-unknown") == 0, "raw unknown not hashed into learned ID");
  Check(service->Suggest("en", "al", options).candidates[1].text_utf8 == "alpha",
        "query snapshot never retained");
}

std::string SyntheticWord(std::size_t ordinal) {
  std::string word = "prefixaaaa";
  for (std::size_t letter_byte = word.size(); letter_byte > 6; --letter_byte) {
    word[letter_byte - 1] = static_cast<char>('a' + ordinal % 26);
    ordinal /= 26;
  }
  return word;
}
void Budgets() {
  std::vector<Row> rows;
  constexpr std::size_t record_count = 12000;
  rows.reserve(record_count);
  for (std::size_t ordinal = 0; ordinal < record_count; ++ordinal) {
    // This synthetic dataset proves bounds, not production vocabulary coverage.
    rows.emplace_back("fr", SyntheticWord(ordinal), ordinal + 1);
  }
  auto service = Service(rows);
  auto options = OnlyPrefix();
  options.prefix_entry_budget = 5;
  const auto bounded = service->Suggest("fr", "prefix", options);
  Check(bounded.prefix_entries_examined == 5 && bounded.prefix_budget_exhausted,
        "12000-record prefix extracts only bounded rows");
  Check(bounded.candidates.size() == 6 && bounded.candidates[1].text_utf8 == SyntheticWord(record_count - 1),
        "bounded extraction still finds highest-count distant row");
  options.max_candidates = 1000000;
  options.prefix_entry_budget = 1000000;
  const auto clamped = service->Suggest("fr", "prefix", options);
  Check(clamped.candidates.size() == 64 && clamped.prefix_entries_examined == lx::kMaxPrefixEntriesExamined &&
        clamped.prefix_budget_exhausted, "candidate and traversal hard ceilings");
  options.max_candidates = 0;
  const auto zero = service->Suggest("fr", "prefix", options);
  Check(zero.candidates.empty() && zero.prefix_entries_examined == 0, "zero candidates does no search");
  options.max_candidates = 1;
  const auto original = service->Suggest("fr", "prefix", options);
  CheckRaw(original, "prefix");
  Check(original.candidates.size() == 1 && original.prefix_entries_examined == 0, "single slot reserved for raw");
  options = OnlyPrefix();
  options.prefix_entry_budget = 0;
  const auto no_work = service->Suggest("fr", "prefix", options);
  Check(no_work.candidates.size() == 1 && no_work.prefix_entries_examined == 0 && no_work.prefix_budget_exhausted,
        "zero prefix budget reports withheld matches");
  Check(service->Suggest("fr", "").status == lx::QueryStatus::kEmptyInput, "empty query status");
  const auto too_long = service->Suggest("fr", std::string(lx::kMaxWordUtf8Bytes + 1, 'a'));
  Check(too_long.status == lx::QueryStatus::kInputTooLong && too_long.candidates.empty(), "no truncated oversized raw");
  const auto invalid = service->Suggest("fr", std::string("\xc0\xaf", 2));
  Check(invalid.status == lx::QueryStatus::kInvalidUtf8 && invalid.candidates.empty(), "no malformed candidate text");
  const auto unknown_language = service->Suggest("xx", "word");
  CheckRaw(unknown_language, "word");
  Check(unknown_language.status == lx::QueryStatus::kUnsupportedLanguage, "unsupported language explicit");
  auto english = Service({{"en", "hello", 20}, {"en", "world", 20}, {"en", "walk", 20}, {"en", "word", 20}});
  options = {};
  options.morphology_match_budget = 0;
  options.spelling_cell_budget = 0;
  options.segmentation_transition_budget = 0;
  const auto morphology = english->Suggest("en", "walk", options);
  Check(morphology.morphology_matches_examined == 0 && morphology.morphology_budget_exhausted,
        "English morphology budget passed through");
  const auto spelling = english->Suggest("en", "wrod", options);
  Check(spelling.spelling_cells == 0 && spelling.spelling_budget_exhausted,
        "English spelling budget passed through");
  const auto segment = english->Suggest("en", "helloworld", options);
  Check(segment.segmentation_transitions == 0 && segment.segmentation_budget_exhausted,
        "English segmentation budget passed through");
}

void Loaders(const std::filesystem::path& fixture_root) {
  lx::BuildInfo info;
  Check(!lx::LexicalService::FromFile(fixture_root / "missing.tsv", &info) &&
        info.status == lx::BuildStatus::kModelLoadFailure && info.dependency_status == "io-error",
        "missing model file explicit failure");
  Check(!lx::LexicalService::FromFile(fixture_root / "valid.tsv", &info) &&
        info.dependency_status == "test-fixture-denied", "production loader rejects fixture by default");
  prediction::LoadOptions options;
  options.allow_test_fixture = true;
  options.expected_source_fingerprint.assign(64, 'a');
  auto service = lx::LexicalService::FromFile(fixture_root / "valid.tsv", options, &info);
  Check(service && info.status == lx::BuildStatus::kOk && service->FindExact("en", "hello") == Id("en", "hello"),
        "versioned checksummed local text model becomes dictionary");
  if (service) Check(Find(service->Suggest("en", "hel", OnlyPrefix()), "hello", lx::CandidateType::kCompletion),
                     "loaded model drives real completion");
  Check(!lx::LexicalService::FromFile(fixture_root / "corrupt.tsv", options, &info) &&
        info.dependency_status == "fingerprint-mismatch", "corrupt payload rejected atomically");
  Check(!lx::LexicalService::FromFile(fixture_root / "version.tsv", options, &info) &&
        info.dependency_status == "unsupported-version", "unknown format version rejected");
  Check(!lx::LexicalService::FromFile(fixture_root / "binary.dat", options, &info) &&
        info.dependency_status == "invalid-format", "v1 binary explicitly unsupported not claimed parsed");
  options.expected_source_fingerprint.assign(64, 'b');
  Check(!lx::LexicalService::FromFile(fixture_root / "valid.tsv", options, &info) &&
        info.dependency_status == "fingerprint-mismatch", "trusted source fingerprint enforced");
  options.expected_source_fingerprint.clear();
  options.max_file_utf8_bytes = 4;
  Check(!lx::LexicalService::FromFile(fixture_root / "valid.tsv", options, &info) &&
        info.dependency_status == "limit-exceeded", "loader byte bound passed through");
}

void Concurrent() {
  auto service = Service({{"en", "hello", 20}, {"en", "help", 10}, {"en", "word", 15},
                          {"fr", "bonjour", 10}});
  std::atomic<bool> valid{true};
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 8; ++worker) {
    workers.emplace_back([&] {
      for (int iteration = 0; iteration < 100; ++iteration) {
        const auto result = service->Suggest("en", "Hel", OnlyPrefix());
        if (result.status != lx::QueryStatus::kOk || result.candidates.size() != 3 ||
            result.candidates[0].text_utf8 != "Hel" || !result.candidates[0].lexeme_ids.empty() ||
            result.candidates[1].text_utf8 != "Hello" ||
            result.candidates[1].lexeme_ids != std::vector<lx::LexemeId>{Id("en", "hello")}) {
          valid.store(false);
        }
      }
    });
  }
  for (auto& worker : workers) worker.join();
  Check(valid.load(), "immutable concurrent query results consistent");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 3) { std::cerr << "INVALID MODE\n"; return 2; }
  const std::string mode = argv[1];
  try {
    if (mode == "--text") Text();
    else if (mode == "--aliases") Aliases();
    else if (mode == "--english") English();
    else if (mode == "--french") French();
    else if (mode == "--korean") Korean();
    else if (mode == "--ranking") Ranking();
    else if (mode == "--budgets") Budgets();
    else if (mode == "--loaders" && argc == 3) Loaders(std::filesystem::u8path(argv[2]));
    else if (mode == "--concurrent") Concurrent();
    else if (mode == "--forcefail") Check(false, "intentional failure channel");
    else { std::cerr << "INVALID MODE\n"; return 2; }
  } catch (const std::exception&) {
    Check(false, "test could not complete");
  }
  if (failures != 0) {
    std::cout << "RESULT FAIL checks=" << checks << " failures=" << failures << '\n';
    return 1;
  }
  if (checks == 0) { std::cerr << "NO CHECKS EXECUTED\n"; return 3; }
  std::cout << "RESULT PASS checks=" << checks << '\n';
  return 0;
}
