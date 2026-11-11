#include "french_core.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fr = epocanva::fr;
namespace {
int failures = 0;
std::size_t checks = 0;  // assertion count, not a text offset
void Check(bool condition, int source_line) {
  ++checks;
  if (!condition) {
    ++failures;
    // Never print input, dictionary words or composition content.
    std::cerr << "assertion failed at source line " << source_line << '\n';
  }
}
#define CHECK(condition) Check((condition), __LINE__)

std::string Bytes(std::initializer_list<unsigned char> bytes) {
  std::string result;
  for (const auto byte : bytes) result += static_cast<char>(byte);
  return result;
}

void Utf8Tests() {
  CHECK(fr::ValidateUtf8("").status == fr::Status::kOk);
  CHECK(fr::ValidateUtf8(u8"français Œ 東京 \U0001f600").status == fr::Status::kOk);
  CHECK(fr::ValidateUtf8(std::string("a\0b", 3)).status == fr::Status::kOk);
  CHECK(fr::ValidateUtf8(u8"\uffff\U0010ffff").status == fr::Status::kOk);
  const std::vector<std::string> bad{
      Bytes({0x80}), Bytes({0xbf}), Bytes({0xc0, 0xaf}), Bytes({0xc1, 0xbf}),
      Bytes({0xc2}), Bytes({0xc2, 0x20}), Bytes({0xe0, 0x80, 0xaf}),
      Bytes({0xed, 0xa0, 0x80}), Bytes({0xed, 0xbf, 0xbf}),
      Bytes({0xe2, 0x82}), Bytes({0xe2, 0x28, 0xa1}),
      Bytes({0xf0, 0x80, 0x80, 0xaf}), Bytes({0xf4, 0x90, 0x80, 0x80}),
      Bytes({0xf5, 0x80, 0x80, 0x80}), Bytes({0xf0, 0x9f, 0x98}),
      Bytes({0xfe}), Bytes({0xff})};
  for (const auto& input : bad) {
    CHECK(fr::ValidateUtf8(input).status == fr::Status::kInvalidUtf8);
    CHECK(fr::ValidateUtf8("ok" + input).error_utf8_byte == 2);
    CHECK(fr::NormalizeFrench(input).status == fr::Status::kInvalidUtf8);
    CHECK(fr::NormalizeFrench(input).text_utf8.empty());
    CHECK(fr::ToUpperFrench(input).status == fr::Status::kInvalidUtf8);
    CHECK(fr::ToLowerFrench(input).status == fr::Status::kInvalidUtf8);
    CHECK(fr::SegmentCombiningClusters(input).clusters.empty());
    CHECK(fr::SegmentTokens(input).tokens.empty());
    CHECK(fr::ClassifyToken(input).status == fr::Status::kInvalidUtf8);
    CHECK(fr::InflectPlural(input, fr::PluralRule::kRegularS).status == fr::Status::kInvalidUtf8);
    CHECK(fr::SuggestAccents(input, {}).status == fr::Status::kInvalidUtf8);
  }
  CHECK(fr::ValidateUtf8(std::string(fr::kMaxTextUtf8Bytes, 'a')).status == fr::Status::kOk);
  const std::string too_long(fr::kMaxTextUtf8Bytes + 1, 'a');
  CHECK(fr::ValidateUtf8(too_long).status == fr::Status::kInputTooLong);
  CHECK(fr::NormalizeFrench(too_long).text_utf8.empty());
  CHECK(fr::SegmentTokens(too_long).status == fr::Status::kInputTooLong);
}

void AccentsCaseTests() {
  struct Mapping {
    fr::DeadAccent accent;
    char32_t base;
    char32_t lower;
    char32_t upper;
  };
  const std::array<Mapping, 16> mappings{{
      {fr::DeadAccent::kAcute, U'e', U'é', U'É'},
      {fr::DeadAccent::kGrave, U'a', U'à', U'À'},
      {fr::DeadAccent::kGrave, U'e', U'è', U'È'},
      {fr::DeadAccent::kGrave, U'u', U'ù', U'Ù'},
      {fr::DeadAccent::kCircumflex, U'a', U'â', U'Â'},
      {fr::DeadAccent::kCircumflex, U'e', U'ê', U'Ê'},
      {fr::DeadAccent::kCircumflex, U'i', U'î', U'Î'},
      {fr::DeadAccent::kCircumflex, U'o', U'ô', U'Ô'},
      {fr::DeadAccent::kCircumflex, U'u', U'û', U'Û'},
      {fr::DeadAccent::kDiaeresis, U'a', U'ä', U'Ä'},
      {fr::DeadAccent::kDiaeresis, U'e', U'ë', U'Ë'},
      {fr::DeadAccent::kDiaeresis, U'i', U'ï', U'Ï'},
      {fr::DeadAccent::kDiaeresis, U'o', U'ö', U'Ö'},
      {fr::DeadAccent::kDiaeresis, U'u', U'ü', U'Ü'},
      {fr::DeadAccent::kDiaeresis, U'y', U'ÿ', U'Ÿ'},
      {fr::DeadAccent::kCedilla, U'c', U'ç', U'Ç'},
  }};
  for (const auto& mapping : mappings) {
    CHECK(fr::ComposeLetter(mapping.accent, mapping.base) == mapping.lower);
    CHECK(fr::ComposeLetter(mapping.accent, mapping.base - (U'a' - U'A')) == mapping.upper);
  }
  CHECK(!fr::ComposeLetter(fr::DeadAccent::kAcute, U'a'));
  CHECK(!fr::ComposeLetter(fr::DeadAccent::kCedilla, U's'));
  CHECK(!fr::ComposeLetter(fr::DeadAccent::kCircumflex, U'é'));
  CHECK(!fr::ComposeLetter(fr::DeadAccent::kGrave, 0xd800));
  CHECK(!fr::ComposeLetter(fr::DeadAccent::kNone, U'e'));
  CHECK(!fr::SpacingAccent(static_cast<fr::DeadAccent>(99)));
  CHECK(fr::SpacingAccent(fr::DeadAccent::kAcute) == U'´');
  CHECK(fr::SpacingAccent(fr::DeadAccent::kGrave) == U'`');
  CHECK(fr::SpacingAccent(fr::DeadAccent::kCircumflex) == U'^');
  CHECK(fr::SpacingAccent(fr::DeadAccent::kDiaeresis) == U'¨');
  CHECK(fr::SpacingAccent(fr::DeadAccent::kCedilla) == U'¸');
  const std::string decomposed = u8"e\u0301 a\u0300 e\u0300 u\u0300 a\u0302 e\u0302 i\u0302 o\u0302 u\u0302 a\u0308 e\u0308 i\u0308 o\u0308 u\u0308 y\u0308 c\u0327";
  const std::string composed = u8"é à è ù â ê î ô û ä ë ï ö ü ÿ ç";
  CHECK(fr::NormalizeFrench(decomposed).text_utf8 == composed);
  CHECK(fr::NormalizeFrench(u8"E\u0301 C\u0327 Y\u0308").text_utf8 == u8"É Ç Ÿ");
  CHECK(fr::NormalizeFrench(u8"e\u0301\u0308").text_utf8 == u8"é\u0308");
  CHECK(fr::NormalizeFrench(u8"e\u0327\u0301").text_utf8 == u8"e\u0327\u0301");
  CHECK(fr::NormalizeFrench(u8"a\u0301 n\u0303 \u212b \u0301").text_utf8 == u8"a\u0301 n\u0303 \u212b \u0301");
  CHECK(fr::NormalizeFrench(composed).text_utf8 == composed);
  CHECK(fr::ToUpperFrench(composed + u8" œ æ").text_utf8 == u8"É À È Ù Â Ê Î Ô Û Ä Ë Ï Ö Ü Ÿ Ç Œ Æ");
  CHECK(fr::ToLowerFrench(u8"É À È Ù Â Ê Î Ô Û Ä Ë Ï Ö Ü Ÿ Ç Œ Æ").text_utf8 == composed + u8" œ æ");
  CHECK(fr::ToUpperFrench(u8"l'œuvre, Straße 東京 \U0001f600").text_utf8 == u8"L'ŒUVRE, STRAßE 東京 \U0001f600");
  CHECK(fr::ToLowerFrench(u8"E\u0301COLE").text_utf8 == u8"école");
  CHECK(!fr::kProductReady);
}

void TokenTests() {
  for (const std::string input : {u8"l'été", u8"aujourd’hui", u8"arc-en-ciel", u8"Jean‐Luc",
                                  u8"Jean‑Luc", u8"ŒUVRE", u8"cœur", u8"aïe", u8"E\u0301cole", "hello"}) {
    CHECK(fr::ClassifyToken(input).kind == fr::TokenKind::kFrenchWord);
  }
  for (const std::string input : {"'ete", "ete'", "-ete", "ete-", "arc--ciel", "l''ete", "a-'b",
                                  u8"cœur2", u8"n\u0303", u8"東京", "two words", "a/b", u8"l’été’"}) {
    CHECK(fr::ClassifyToken(input).kind == fr::TokenKind::kOther);
  }
  for (const std::string input : {"https://exemple.fr/ete?x=2", "HTTP://x", "custom+v2://x", "www.exemple.fr", "mailto:ete@x"}) {
    CHECK(fr::ClassifyToken(input).kind == fr::TokenKind::kUrl);
  }
  CHECK(fr::ClassifyToken("ete@example.fr").kind == fr::TokenKind::kEmail);
  CHECK(fr::ClassifyToken(u8"élève@exemple.fr").kind == fr::TokenKind::kEmail);
  CHECK(fr::ClassifyToken("ete@x").kind == fr::TokenKind::kEmail);
  CHECK(fr::ClassifyToken("a@@b").kind == fr::TokenKind::kOther);
  CHECK(fr::ClassifyToken("@b").kind == fr::TokenKind::kOther);
  CHECK(fr::ClassifyToken("https://two words").kind == fr::TokenKind::kOther);
  for (const std::string input : {"123", "-12,5", "+12.34", "6,02e+23", "2E-3%", u8"−20", u8"1 234,56", u8"12 345", "20%"}) {
    CHECK(fr::ClassifyToken(input).kind == fr::TokenKind::kNumber);
  }
  for (const std::string input : {"1e", "1e+", "12,", "1,,2", "--3", "12abc", "1%2", ".5", "1 234", "2e1.5"}) {
    CHECK(fr::ClassifyToken(input).kind == fr::TokenKind::kOther);
  }
  CHECK(fr::ClassifyToken(u8" \t\r\n  ").kind == fr::TokenKind::kWhitespace);
  CHECK(fr::ClassifyToken(std::string("a\0b", 3)).kind == fr::TokenKind::kOther);
  CHECK(fr::ClassifyToken(std::string("https://a\0b", 11)).kind == fr::TokenKind::kOther);
  CHECK(fr::ClassifyToken("").kind == fr::TokenKind::kOther);

  const std::string text = u8"L'été, https://x.fr/a-b e@x.fr -12,5 foo--bar e\u0301cole!";
  const auto segmented = fr::SegmentTokens(text);
  CHECK(segmented.status == fr::Status::kOk);
  const std::vector<std::pair<std::string, fr::TokenKind>> expected{
      {u8"L'été", fr::TokenKind::kFrenchWord}, {",", fr::TokenKind::kOther},
      {" ", fr::TokenKind::kWhitespace}, {"https://x.fr/a-b", fr::TokenKind::kUrl},
      {" ", fr::TokenKind::kWhitespace}, {"e@x.fr", fr::TokenKind::kEmail},
      {" ", fr::TokenKind::kWhitespace}, {"-12,5", fr::TokenKind::kNumber},
      {" ", fr::TokenKind::kWhitespace}, {"foo--bar", fr::TokenKind::kOther},
      {" ", fr::TokenKind::kWhitespace}, {u8"e\u0301cole", fr::TokenKind::kFrenchWord},
      {"!", fr::TokenKind::kOther}};
  CHECK(segmented.tokens.size() == expected.size());
  auto actual = segmented.tokens.begin();
  std::size_t next_utf8_byte = 0;
  for (const auto& token : expected) {
    if (actual == segmented.tokens.end()) { CHECK(false); break; }
    CHECK(actual->kind == token.second);
    CHECK(actual->utf8_bytes.begin_utf8_byte == next_utf8_byte);
    next_utf8_byte += token.first.size();
    CHECK(actual->utf8_bytes.end_utf8_byte == next_utf8_byte);
    CHECK(text.substr(actual->utf8_bytes.begin_utf8_byte,
                      actual->utf8_bytes.end_utf8_byte - actual->utf8_bytes.begin_utf8_byte) == token.first);
    ++actual;
  }
  CHECK(next_utf8_byte == text.size());
  const auto punctuation = fr::SegmentTokens("((ete)), !!!");
  CHECK(punctuation.tokens.size() == 5);
  CHECK(fr::SegmentTokens("").tokens.empty());
}

void CompositionTests() {
  const auto clusters = fr::SegmentCombiningClusters(u8"e\u0301\u0308x");
  CHECK(clusters.status == fr::Status::kOk);
  CHECK(clusters.clusters.size() == 2);
  CHECK(clusters.clusters.front().begin_utf8_byte == 0);
  CHECK(clusters.clusters.front().end_utf8_byte == 5);
  CHECK(clusters.clusters.back().begin_utf8_byte == 5);
  CHECK(clusters.clusters.back().end_utf8_byte == 6);
  CHECK(fr::SegmentCombiningClusters(u8"\u0301\u0308a").clusters.size() == 2);
  CHECK(fr::SegmentCombiningClusters(u8"\U0001f469\u200d\U0001f4bb").clusters.size() == 3);
  CHECK(fr::SegmentCombiningClusters(u8"a\ufe0f").clusters.size() == 2);
  fr::Composition core;
  CHECK(core.BeginDeadAccent(fr::DeadAccent::kAcute) == fr::Status::kOk);
  CHECK(core.text_utf8().empty());
  CHECK(core.InsertUtf8("") == fr::Status::kOk);
  CHECK(core.pending_accent() == fr::DeadAccent::kAcute);
  CHECK(core.InsertUtf8("e") == fr::Status::kOk);
  CHECK(core.text_utf8() == u8"é");
  CHECK(core.caret_utf8_byte() == 2);
  CHECK(core.pending_accent() == fr::DeadAccent::kNone);
  CHECK(core.SetCaret(1) == fr::Status::kInvalidCaretUtf8Byte);
  CHECK(core.SetCaret(3) == fr::Status::kInvalidCaretUtf8Byte);
  CHECK(core.BeginDeadAccent(fr::DeadAccent::kCedilla) == fr::Status::kOk);
  CHECK(core.InsertUtf8("C") == fr::Status::kOk);
  CHECK(core.text_utf8() == u8"éÇ");
  CHECK(core.caret_utf8_byte() == 4);
  CHECK(core.BeginDeadAccent(fr::DeadAccent::kGrave) == fr::Status::kOk);
  CHECK(core.Backspace() == fr::Status::kOk);
  CHECK(core.text_utf8() == u8"éÇ");
  CHECK(core.pending_accent() == fr::DeadAccent::kNone);
  CHECK(core.Backspace() == fr::Status::kOk);
  CHECK(core.text_utf8() == u8"é");
  CHECK(core.BeginDeadAccent(fr::DeadAccent::kAcute) == fr::Status::kOk);
  CHECK(core.BeginDeadAccent(fr::DeadAccent::kAcute) == fr::Status::kOk);
  CHECK(core.text_utf8() == u8"é´");
  CHECK(core.pending_accent() == fr::DeadAccent::kNone);
  core.Cancel();
  CHECK(core.text_utf8().empty());
  CHECK(core.caret_utf8_byte() == 0);
  CHECK(core.BeginDeadAccent(fr::DeadAccent::kCircumflex) == fr::Status::kOk);
  CHECK(core.BeginDeadAccent(fr::DeadAccent::kDiaeresis) == fr::Status::kOk);
  CHECK(core.text_utf8() == "^");
  CHECK(core.pending_accent() == fr::DeadAccent::kDiaeresis);
  CHECK(core.InsertUtf8(" ") == fr::Status::kOk);
  CHECK(core.text_utf8() == u8"^¨");
  CHECK(core.BeginDeadAccent(fr::DeadAccent::kCedilla) == fr::Status::kOk);
  CHECK(core.InsertUtf8("q") == fr::Status::kOk);
  CHECK(core.text_utf8() == u8"^¨¸q");
  CHECK(core.BeginDeadAccent(fr::DeadAccent::kNone) == fr::Status::kInvalidDeadAccent);
  CHECK(core.BeginDeadAccent(static_cast<fr::DeadAccent>(99)) == fr::Status::kInvalidDeadAccent);
  core.Cancel();
  CHECK(core.SetText(u8"e\u0301\u0308x", 5) == fr::Status::kOk);
  CHECK(core.SetCaret(1) == fr::Status::kInvalidCaretUtf8Byte);
  CHECK(core.SetCaret(3) == fr::Status::kInvalidCaretUtf8Byte);
  CHECK(core.Backspace() == fr::Status::kOk);
  CHECK(core.text_utf8() == "x");
  CHECK(core.caret_utf8_byte() == 0);
  CHECK(core.Backspace() == fr::Status::kOk);
  CHECK(core.text_utf8() == "x");
  CHECK(core.SetText(u8"a\u0327", 3) == fr::Status::kOk);
  CHECK(core.Backspace() == fr::Status::kOk);
  CHECK(core.text_utf8().empty());
  CHECK(core.InsertUtf8("e") == fr::Status::kOk);
  CHECK(core.InsertUtf8(u8"\u0301") == fr::Status::kOk);
  CHECK(core.text_utf8() == u8"é");
  CHECK(core.caret_utf8_byte() == 2);
  CHECK(core.InsertUtf8(u8"\u0308") == fr::Status::kOk);
  CHECK(core.caret_utf8_byte() == 4);
  CHECK(core.Backspace() == fr::Status::kOk);
  CHECK(core.text_utf8().empty());
  CHECK(core.SetText(u8"éx", 2) == fr::Status::kOk);
  CHECK(core.InsertUtf8("t") == fr::Status::kOk);
  CHECK(core.text_utf8() == u8"étx");
  CHECK(core.caret_utf8_byte() == 3);
  CHECK(core.SetText(u8"\u0301x", 0) == fr::Status::kOk);
  CHECK(core.InsertUtf8("e") == fr::Status::kOk);
  CHECK(core.text_utf8() == u8"éx");
  CHECK(core.caret_utf8_byte() == 2);
  CHECK(core.SetText(u8"e\u0301x", 0) == fr::Status::kOk);
  CHECK(core.InsertUtf8("z") == fr::Status::kOk);
  CHECK(core.text_utf8() == u8"zéx");
  CHECK(core.caret_utf8_byte() == 1);
  CHECK(core.SetText(u8"\U0001f600", 4) == fr::Status::kOk);
  CHECK(core.SetCaret(2) == fr::Status::kInvalidCaretUtf8Byte);
  CHECK(core.Backspace() == fr::Status::kOk);
  CHECK(core.text_utf8().empty());
  CHECK(core.SetText("abc", 2) == fr::Status::kOk);
  CHECK(core.InsertUtf8(core.text_utf8()) == fr::Status::kOk);
  CHECK(core.text_utf8() == "ababcc");
  CHECK(core.caret_utf8_byte() == 5);
  CHECK(core.BeginDeadAccent(fr::DeadAccent::kAcute) == fr::Status::kOk);
  const std::string previous = core.text_utf8();
  const std::size_t previous_caret_utf8_byte = core.caret_utf8_byte();
  CHECK(core.SetText(u8"e\u0301", 1) == fr::Status::kInvalidCaretUtf8Byte);
  CHECK(core.InsertUtf8(Bytes({0xc0, 0xaf})) == fr::Status::kInvalidUtf8);
  CHECK(core.SetText(Bytes({0x80}), 0) == fr::Status::kInvalidUtf8);
  CHECK(core.text_utf8() == previous);
  CHECK(core.caret_utf8_byte() == previous_caret_utf8_byte);
  CHECK(core.pending_accent() == fr::DeadAccent::kAcute);
  CHECK(core.SetText(std::string(fr::kMaxTextUtf8Bytes, 'a'), fr::kMaxTextUtf8Bytes) == fr::Status::kOk);
  CHECK(core.BeginDeadAccent(fr::DeadAccent::kAcute) == fr::Status::kOk);
  CHECK(core.InsertUtf8("e") == fr::Status::kInputTooLong);
  CHECK(core.BeginDeadAccent(fr::DeadAccent::kGrave) == fr::Status::kInputTooLong);
  CHECK(core.text_utf8().size() == fr::kMaxTextUtf8Bytes);
  CHECK(core.pending_accent() == fr::DeadAccent::kAcute);
  core.Cancel();
  CHECK(core.text_utf8().empty());
  CHECK(core.pending_accent() == fr::DeadAccent::kNone);
}

fr::DictionaryInput Fixture() {
  // Original test assertions only; not a bundled dictionary or production data.
  return {{fr::DictionarySourceStatus::kTestFixture, "original unit fixture", "1", "test-only; no production data"},
          {{u8"été", 20}, {u8"côté", 5}, {u8"côte", 30}, {u8"cote", 100},
           {u8"co\u0302te", 1}, {u8"côte", 40}, {u8"CÔTE", 15}, {u8"cœur", 5},
           {u8"où", 10}, {u8"noël", 2}, {u8"l'été", 10}, {u8"arc-en-ciel", 1}}};
}

void SuggestionTests() {
  const auto data = Fixture();
  CHECK(fr::SuggestAccents("ete", {}).status == fr::Status::kDictionaryUnavailable);
  auto result = fr::SuggestAccents("ete", data);
  CHECK(result.status == fr::Status::kOk);
  CHECK(result.source_status == fr::DictionarySourceStatus::kTestFixture);
  CHECK(result.suggestions.size() == 1);
  if (!result.suggestions.empty()) {
    CHECK(result.suggestions.front().text_utf8 == u8"été");
    CHECK(result.suggestions.front().changed_grapheme_clusters == 2);
  }
  result = fr::SuggestAccents("cote", data);
  CHECK(result.suggestions.size() == 2);
  if (result.suggestions.size() == 2) {
    CHECK(result.suggestions.front().text_utf8 == u8"côte");
    CHECK(result.suggestions.front().rank == 40);
    CHECK(result.suggestions.back().text_utf8 == u8"côté");
  }
  result = fr::SuggestAccents("COTE", data, 1);
  CHECK(result.suggestions.size() == 1);
  if (!result.suggestions.empty()) CHECK(result.suggestions.front().text_utf8 == u8"CÔTE");
  result = fr::SuggestAccents("Ete", data);
  if (!result.suggestions.empty()) CHECK(result.suggestions.front().text_utf8 == u8"Été");
  CHECK(result.suggestions.size() == 1);
  CHECK(fr::SuggestAccents("eTe", data).status == fr::Status::kUnsupportedCase);
  CHECK(fr::SuggestAccents(u8"été", data).suggestions.empty());
  CHECK(fr::SuggestAccents(u8"e\u0301te\u0301", data).suggestions.empty());
  CHECK(fr::SuggestAccents("coeur", data).suggestions.empty());
  CHECK(fr::SuggestAccents(u8"còte", data).status == fr::Status::kNotFrenchWord);
  CHECK(fr::SuggestAccents(u8"coté", data).suggestions.size() == 1);
  CHECK(fr::SuggestAccents(u8"côté", data).suggestions.empty());
  CHECK(fr::SuggestAccents("unknown", data).suggestions.empty());
  CHECK(fr::SuggestAccents("ete", data, 0).suggestions.empty());
  CHECK(fr::SuggestAccents("ete", data, 65).status == fr::Status::kInvalidLimit);
  CHECK(fr::SuggestAccents("", data).status == fr::Status::kEmptyInput);
  CHECK(fr::SuggestAccents("ete@x", data).status == fr::Status::kProtectedToken);
  CHECK(fr::SuggestAccents("https://ete.fr", data).status == fr::Status::kProtectedToken);
  CHECK(fr::SuggestAccents("12,3", data).status == fr::Status::kProtectedToken);
  CHECK(fr::SuggestAccents("ete--ete", data).status == fr::Status::kNotFrenchWord);
  CHECK(fr::SuggestAccents("l'ete", data).suggestions.size() == 1);
  CHECK(fr::SuggestAccents(u8"l’ete", data).suggestions.empty());
  auto invalid = data;
  invalid.entries.push_back({Bytes({0x80}), 1});
  CHECK(fr::SuggestAccents("ete", invalid).status == fr::Status::kInvalidDictionary);
  CHECK(fr::SuggestAccents("ete", invalid).suggestions.empty());
  CHECK(fr::SuggestAccents("ete", invalid, 0).status == fr::Status::kInvalidDictionary);
  invalid = data;
  invalid.source.version.clear();
  CHECK(fr::SuggestAccents("ete", invalid).status == fr::Status::kInvalidDictionary);
  invalid = data;
  invalid.source.licence = "   ";
  CHECK(fr::SuggestAccents("ete", invalid).status == fr::Status::kInvalidDictionary);
  invalid = data;
  invalid.entries.clear();
  CHECK(fr::SuggestAccents("ete", invalid).status == fr::Status::kInvalidDictionary);
  invalid = data;
  invalid.source.status = fr::DictionarySourceStatus::kUnavailable;
  CHECK(fr::SuggestAccents("ete", invalid).status == fr::Status::kInvalidDictionary);
  invalid = data;
  invalid.entries.push_back({"https://example.fr", 1});
  CHECK(fr::SuggestAccents("ete", invalid).status == fr::Status::kInvalidDictionary);
  invalid = data;
  invalid.entries.push_back({std::string(fr::kMaxWordUtf8Bytes + 1, 'a'), 1});
  CHECK(fr::SuggestAccents("ete", invalid).status == fr::Status::kInvalidDictionary);
  invalid = data;
  invalid.entries.resize(fr::kMaxDictionaryEntries + 1);
  CHECK(fr::SuggestAccents("ete", invalid).status == fr::Status::kInvalidDictionary);
  invalid = data;
  invalid.source.status = static_cast<fr::DictionarySourceStatus>(99);
  CHECK(fr::SuggestAccents("ete", invalid).status == fr::Status::kInvalidDictionary);
  auto caller = data;
  caller.source.status = fr::DictionarySourceStatus::kCallerSuppliedUnverified;
  CHECK(fr::SuggestAccents("ete", caller).source_status == fr::DictionarySourceStatus::kCallerSuppliedUnverified);
  auto tie = data;
  tie.entries = {{u8"côté", 10}, {u8"coté", 10}, {u8"côte", 10}};
  result = fr::SuggestAccents("cote", tie);
  CHECK(result.suggestions.size() == 3);
  if (result.suggestions.size() == 3) {
    CHECK(result.suggestions.front().text_utf8 == u8"coté");
    CHECK(result.suggestions.back().text_utf8 == u8"côté");
  }
}

void PluralTests() {
  const auto check_plural = [](std::string_view lemma, fr::PluralRule rule,
                               std::string_view expected) {
    const auto result = fr::InflectPlural(lemma, rule);
    CHECK(result.status == fr::Status::kOk);
    CHECK(result.text_utf8 == expected);
  };
  check_plural(u8"école", fr::PluralRule::kRegularS, u8"écoles");
  check_plural(u8"E\u0301cole", fr::PluralRule::kRegularS, u8"Écoles");
  check_plural(u8"ŒUVRE", fr::PluralRule::kRegularS, u8"ŒUVRES");
  check_plural("prix", fr::PluralRule::kRegularS, "prix");
  check_plural("nez", fr::PluralRule::kRegularS, "nez");
  check_plural("bras", fr::PluralRule::kRegularS, "bras");
  check_plural("bateau", fr::PluralRule::kEauX, "bateaux");
  check_plural("Jeu", fr::PluralRule::kAuEuX, "Jeux");
  check_plural("TUYAU", fr::PluralRule::kAuEuX, "TUYAUX");
  check_plural("bleu", fr::PluralRule::kRegularS, "bleus");
  check_plural("pneu", fr::PluralRule::kRegularS, "pneus");
  check_plural("chou", fr::PluralRule::kOuX, "choux");
  check_plural("trou", fr::PluralRule::kRegularS, "trous");
  check_plural("cheval", fr::PluralRule::kAlAux, "chevaux");
  check_plural("BAL", fr::PluralRule::kRegularS, "BALS");
  check_plural("Travail", fr::PluralRule::kAilAux, "Travaux");
  check_plural("chandail", fr::PluralRule::kRegularS, "chandails");
  check_plural("orange", fr::PluralRule::kInvariant, "orange");
  CHECK(fr::InflectPlural("chat", fr::PluralRule::kEauX).status == fr::Status::kRuleNotApplicable);
  CHECK(fr::InflectPlural("chat", fr::PluralRule::kAuEuX).status == fr::Status::kRuleNotApplicable);
  CHECK(fr::InflectPlural("chat", fr::PluralRule::kOuX).status == fr::Status::kRuleNotApplicable);
  CHECK(fr::InflectPlural("chat", fr::PluralRule::kAlAux).status == fr::Status::kRuleNotApplicable);
  CHECK(fr::InflectPlural("chat", fr::PluralRule::kAilAux).status == fr::Status::kRuleNotApplicable);
  CHECK(fr::InflectPlural("arc-en-ciel", fr::PluralRule::kRegularS).status == fr::Status::kUnsupportedInflection);
  CHECK(fr::InflectPlural("l'ami", fr::PluralRule::kRegularS).status == fr::Status::kUnsupportedInflection);
  CHECK(fr::InflectPlural("cHaT", fr::PluralRule::kRegularS).status == fr::Status::kUnsupportedCase);
  CHECK(fr::InflectPlural("12", fr::PluralRule::kRegularS).status == fr::Status::kProtectedToken);
  CHECK(fr::InflectPlural("a@b.fr", fr::PluralRule::kRegularS).status == fr::Status::kProtectedToken);
  CHECK(fr::InflectPlural("deux mots", fr::PluralRule::kRegularS).status == fr::Status::kNotFrenchWord);
  CHECK(fr::InflectPlural("", fr::PluralRule::kRegularS).status == fr::Status::kEmptyInput);
  CHECK(fr::InflectPlural("chat", static_cast<fr::PluralRule>(99)).status == fr::Status::kUnsupportedInflection);
  CHECK(fr::InflectPlural(std::string(fr::kMaxWordUtf8Bytes, 'a'), fr::PluralRule::kRegularS).status == fr::Status::kInputTooLong);
  CHECK(fr::InflectPlural(std::string(fr::kMaxWordUtf8Bytes + 1, 'a'), fr::PluralRule::kRegularS).status == fr::Status::kInputTooLong);
}

void PropertyTests() {
  // Deterministic adversarial byte strings. No OS randomness or external data.
  std::uint32_t seed = 0x426315u;
  for (std::size_t trial = 0; trial < 1000; ++trial) {  // trial count, not text offsets
    std::string input;
    const std::size_t input_utf8_bytes = trial % 33;
    for (std::size_t utf8_byte = 0; utf8_byte < input_utf8_bytes; ++utf8_byte) {
      seed = seed * 1664525u + 1013904223u;
      input += static_cast<char>(seed >> 24);
    }
    const auto validation = fr::ValidateUtf8(input);
    const auto normalized = fr::NormalizeFrench(input);
    CHECK(normalized.status == validation.status);
    if (validation.status != fr::Status::kOk) {
      CHECK(normalized.text_utf8.empty());
      continue;
    }
    CHECK(fr::ValidateUtf8(normalized.text_utf8).status == fr::Status::kOk);
    CHECK(fr::NormalizeFrench(normalized.text_utf8).text_utf8 == normalized.text_utf8);
    CHECK(fr::ToUpperFrench(fr::ToUpperFrench(input).text_utf8).text_utf8 == fr::ToUpperFrench(input).text_utf8);
    const auto segmented = fr::SegmentCombiningClusters(input);
    CHECK(segmented.status == fr::Status::kOk);
    std::size_t next_utf8_byte = 0;
    fr::Composition core;
    for (const auto& span : segmented.clusters) {
      CHECK(span.begin_utf8_byte == next_utf8_byte);
      CHECK(span.end_utf8_byte > span.begin_utf8_byte);
      CHECK(fr::ValidateUtf8(std::string_view(input).substr(span.begin_utf8_byte,
                     span.end_utf8_byte - span.begin_utf8_byte)).status == fr::Status::kOk);
      CHECK(core.SetText(input, span.end_utf8_byte) == fr::Status::kOk);
      CHECK(core.Backspace() == fr::Status::kOk);
      CHECK(core.text_utf8() == input.substr(0, span.begin_utf8_byte) + input.substr(span.end_utf8_byte));
      CHECK(core.caret_utf8_byte() == span.begin_utf8_byte);
      next_utf8_byte = span.end_utf8_byte;
    }
    CHECK(next_utf8_byte == input.size());
    next_utf8_byte = 0;
    for (const auto& token : fr::SegmentTokens(input).tokens) {
      CHECK(token.utf8_bytes.begin_utf8_byte == next_utf8_byte);
      CHECK(token.utf8_bytes.end_utf8_byte > token.utf8_bytes.begin_utf8_byte);
      next_utf8_byte = token.utf8_bytes.end_utf8_byte;
    }
    CHECK(next_utf8_byte == input.size());
  }
  const std::vector<std::string> accents{u8"\u0301", u8"\u0300", u8"\u0302", u8"\u0308", u8"\u0327"};
  for (char letter = 'a'; letter <= 'z'; ++letter) {
    for (const auto& accent : accents) {
      fr::Composition core;
      CHECK(core.InsertUtf8(std::string(1, letter)) == fr::Status::kOk);
      CHECK(core.InsertUtf8(accent) == fr::Status::kOk);
      CHECK(fr::ValidateUtf8(core.text_utf8()).status == fr::Status::kOk);
      CHECK(core.caret_utf8_byte() == core.text_utf8().size());
      CHECK(core.Backspace() == fr::Status::kOk);
      CHECK(core.text_utf8().empty());
    }
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cout << "INVALID MODE\n";
    return 2;
  }
  const std::string_view mode(argv[1]);
  if (mode == "utf8") Utf8Tests();
  else if (mode == "accents_case") AccentsCaseTests();
  else if (mode == "tokens") TokenTests();
  else if (mode == "composition") CompositionTests();
  else if (mode == "suggestions") SuggestionTests();
  else if (mode == "plurals") PluralTests();
  else if (mode == "properties") PropertyTests();
  else if (mode == "force_failure") CHECK(fr::NormalizeFrench(u8"e\u0301").text_utf8 == "e");
  else {
    std::cout << "INVALID MODE\n";
    return 2;
  }
  std::cout << (failures == 0 ? "RESULT PASS" : "RESULT FAIL")
            << " checks=" << checks << " failures=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
