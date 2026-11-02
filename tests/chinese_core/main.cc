#include "engine/languages/zh/phonetic_core.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace zh = epocanva::zh;

namespace {
int failures = 0;
std::size_t checks = 0;

void Check(bool condition, int source_line) {
  ++checks;
  if (!condition) {
    ++failures;
    // Diagnostics never print input, preedit, dictionary text or committed text.
    std::cerr << "CHECK FAILED line " << source_line << '\n';
  }
}
#define CHECK(condition) Check((condition), __LINE__)

bool HasPath(const zh::SegmentationResult& result,
             const std::vector<std::string>& syllables) {
  for (const auto& path : result.alternatives) {
    if (path.syllables.size() != syllables.size()) continue;
    bool match = true;
    for (std::size_t ordinal = 0; ordinal < syllables.size(); ++ordinal) {
      if (path.syllables[ordinal].canonical_pinyin != syllables[ordinal]) match = false;
    }
    if (match) return true;
  }
  return false;
}

void PinyinGrammar() {
  for (const auto text : {"a", "ai", "an", "ang", "ao", "o", "ou", "e", "ei", "en", "eng", "er",
                          "ba", "pei", "ming", "fo", "dun", "ting", "niang", "lia", "lo", "gui",
                          "kui", "huo", "jiong", "qiu", "xuan", "zhuang", "chua", "shuang",
                          "ruan", "zi", "ci", "si", "yai", "yo", "weng", "wu"}) {
    CHECK(zh::ParsePinyinSyllable(text).status == zh::Status::kOk);
  }
  for (const auto text : {"biong", "fing", "gi", "ki", "hi", "jiangn", "qang", "xong",
                          "shong", "rengg", "zhae", "zhei4x", "wong", "nue", "lue", "juae",
                          "nvan", "lvan", "nven", "dinn", "tiu", "yui", "fua", "uu", "iou", "uei"}) {
    CHECK(zh::ParsePinyinSyllable(text).status == zh::Status::kInvalidPhonetic);
  }
  for (const auto text : {"b", "ch", "zh", "sh", "j", "q", "x", "g", "w", "y", "din", "jua"}) {
    CHECK(zh::ParsePinyinSyllable(text).status == zh::Status::kIncomplete);
  }
  CHECK(zh::ParsePinyinSyllable("").status == zh::Status::kEmpty);
  const auto nv = zh::ParsePinyinSyllable(u8"Nǚ");
  CHECK(nv.status == zh::Status::kOk);
  CHECK(nv.syllable.canonical_pinyin == "nv");
  CHECK(nv.syllable.tone == zh::Tone::kThird);
  CHECK(nv.syllable.source_bytes.end_byte == 3);
  CHECK(nv.syllable.phonetic_utf8 == u8"nü3");
  CHECK(zh::ParsePinyinSyllable("nv3").syllable.phonetic_utf8 == u8"nü3");
  CHECK(zh::ParsePinyinSyllable("nu3").syllable.canonical_pinyin == "nu");
  CHECK(zh::ParsePinyinSyllable("lv4").syllable.canonical_pinyin == "lv");
  CHECK(zh::ParsePinyinSyllable("lu4").syllable.canonical_pinyin == "lu");
  CHECK(zh::ParsePinyinSyllable(u8"lüe").syllable.canonical_pinyin == "lve");
  for (const auto text : {u8"jü", "jv", "ju", u8"qǚ", "qv3", "qu3", "xve", "xue", "yv", "yu"}) {
    CHECK(zh::ParsePinyinSyllable(text).status == zh::Status::kOk);
  }
  CHECK(zh::ParsePinyinSyllable("jv").syllable.canonical_pinyin == "ju");
  CHECK(zh::ParsePinyinSyllable("xve").syllable.canonical_pinyin == "xue");
  CHECK(zh::ParsePinyinSyllable("ni").syllable.canonical_pinyin !=
        zh::ParsePinyinSyllable("li").syllable.canonical_pinyin);
  for (int tone = 1; tone <= 5; ++tone) {
    const auto parsed = zh::ParsePinyinSyllable("ma" + std::to_string(tone));
    CHECK(parsed.status == zh::Status::kOk);
    CHECK(static_cast<int>(parsed.syllable.tone) == tone);
  }
  for (const auto text : {u8"mā", u8"má", u8"mǎ", u8"mà", u8"liú", u8"guǐ", u8"shǒu",
                          u8"xuě", u8"YUÁN", u8"lüè", u8"ê"}) {
    CHECK(zh::ParsePinyinSyllable(text).status == zh::Status::kOk);
  }
  for (const auto text : {u8"mǎ3", "ma33", "3ma", "m3a", "m3", u8"lǐu", u8"gǔi", u8"shoú",
                          u8"mǎá", "'ma", "ma'", "xi'an"}) {
    CHECK(zh::ParsePinyinSyllable(text).status == zh::Status::kInvalidPhonetic);
  }
  for (const auto text : {"ma0", "ma6", "nu:", "n@", "hello.com", u8"nu\u0308"}) {
    CHECK(zh::ParsePinyinSyllable(text).status == zh::Status::kProtectedInput);
  }
}

void Segmentation() {
  auto result = zh::SegmentPinyin("xian");
  CHECK(result.status == zh::Status::kOk);
  CHECK(result.ambiguous);
  CHECK(HasPath(result, {"xian"}));
  CHECK(HasPath(result, {"xi", "an"}));
  CHECK(result.alternatives.front().syllables.front().canonical_pinyin == "xian");
  result = zh::SegmentPinyin("xi'an");
  CHECK(result.status == zh::Status::kOk);
  CHECK(!result.ambiguous);
  CHECK(HasPath(result, {"xi", "an"}));
  CHECK(!HasPath(result, {"xian"}));
  CHECK(result.alternatives.front().syllables[0].source_bytes.begin_byte == 0);
  CHECK(result.alternatives.front().syllables[0].source_bytes.end_byte == 2);
  CHECK(result.alternatives.front().syllables[1].source_bytes.begin_byte == 3);
  CHECK(result.alternatives.front().syllables[1].source_bytes.end_byte == 5);
  result = zh::SegmentPinyin("fangan");
  CHECK(result.status == zh::Status::kOk);
  CHECK(HasPath(result, {"fang", "an"}));
  CHECK(HasPath(result, {"fan", "gan"}));
  CHECK(result.ambiguous);
  result = zh::SegmentPinyin("xingan");
  CHECK(HasPath(result, {"xing", "an"}));
  CHECK(HasPath(result, {"xin", "gan"}));
  result = zh::SegmentPinyin("nali");
  CHECK(HasPath(result, {"na", "li"}));
  CHECK(!HasPath(result, {"la", "ni"}));
  result = zh::SegmentPinyin("nanan");
  CHECK(HasPath(result, {"nan", "an"}));
  CHECK(HasPath(result, {"na", "nan"}));
  result = zh::SegmentPinyin("zhongguo");
  CHECK(result.status == zh::Status::kOk);
  CHECK(HasPath(result, {"zhong", "guo"}));
  result = zh::SegmentPinyin("ni3hao3");
  CHECK(result.status == zh::Status::kOk);
  CHECK(result.alternatives.front().syllables[0].tone == zh::Tone::kThird);
  CHECK(result.alternatives.front().syllables[1].tone == zh::Tone::kThird);
  CHECK(result.alternatives.front().syllables[1].source_bytes.begin_byte == 3);
  result = zh::SegmentPinyin(u8"NǙ'ér");
  CHECK(result.status == zh::Status::kOk);
  CHECK(HasPath(result, {"nv", "er"}));
  const auto& first = result.alternatives.front().syllables[0];
  const auto& second = result.alternatives.front().syllables[1];
  CHECK(first.source_bytes.begin_byte == 0 && first.source_bytes.end_byte == 3);
  CHECK(second.source_bytes.begin_byte == 4 && second.source_bytes.end_byte == 7);
  CHECK(result.input_bytes == 7);
  CHECK(result.alternatives.front().phonetic_utf8 == u8"nü3'er2");
  result = zh::SegmentPinyin("zhongg");
  CHECK(result.status == zh::Status::kIncomplete);
  CHECK(result.alternatives.front().pending_bytes.has_value());
  CHECK(result.alternatives.front().pending_bytes->begin_byte == 5);
  CHECK(result.alternatives.front().pending_bytes->end_byte == 6);
  CHECK(result.alternatives.front().syllables.front().canonical_pinyin == "zhong");
  result = zh::SegmentPinyin("xi'");
  CHECK(result.status == zh::Status::kIncomplete);
  CHECK(result.alternatives.front().pending_bytes->begin_byte == 3);
  CHECK(result.alternatives.front().pending_bytes->end_byte == 3);
  for (const auto text : {"'xi", "xi''an", "biong", "ni33", "xi3'an5n3", "zhzh"}) {
    result = zh::SegmentPinyin(text);
    CHECK(result.status == zh::Status::kInvalidPhonetic);
    CHECK(result.alternatives.empty());
  }
  zh::Limits limits;
  limits.max_alternatives = 1;
  result = zh::SegmentPinyin("xian", limits);
  CHECK(result.status == zh::Status::kOk);
  CHECK(result.alternatives.size() == 1);
  CHECK(result.ambiguous && result.alternatives_truncated);
  limits.max_alternatives = 8;
  limits.max_syllables = 1;
  result = zh::SegmentPinyin("xian", limits);
  CHECK(result.status == zh::Status::kOk);
  CHECK(result.ambiguous && result.alternatives_truncated);
  CHECK(result.alternatives.size() == 1);
  CHECK(zh::SegmentPinyin("nihao", limits).status == zh::Status::kLimitExceeded);
  limits = {};
  limits.max_alternatives = 3;
  const std::string repeated = "xianxianxianxianxianxianxianxian";
  result = zh::SegmentPinyin(repeated, limits);
  CHECK(result.status == zh::Status::kOk);
  CHECK(result.alternatives.size() == 3);
  CHECK(result.ambiguous && result.alternatives_truncated);
  const auto again = zh::SegmentPinyin(repeated, limits);
  for (std::size_t ordinal = 0; ordinal < result.alternatives.size(); ++ordinal) {
    CHECK(result.alternatives[ordinal].phonetic_utf8 == again.alternatives[ordinal].phonetic_utf8);
  }
}

void Zhuyin() {
  struct Pair { std::string_view symbols; std::string_view pinyin; };
  const Pair pairs[] = {
      {u8"ㄅㄛ", "bo"}, {u8"ㄆㄧㄥ", "ping"}, {u8"ㄇㄧㄡ", "miu"}, {u8"ㄈㄛ", "fo"},
      {u8"ㄉㄨㄟ", "dui"}, {u8"ㄊㄨㄥ", "tong"}, {u8"ㄋㄩ", "nv"}, {u8"ㄋㄨ", "nu"},
      {u8"ㄌㄩㄝ", "lve"}, {u8"ㄌㄨ", "lu"}, {u8"ㄍㄨㄚ", "gua"}, {u8"ㄎㄨㄣ", "kun"},
      {u8"ㄏㄨㄞ", "huai"}, {u8"ㄐㄩㄣ", "jun"}, {u8"ㄑㄧㄡ", "qiu"}, {u8"ㄒㄩㄥ", "xiong"},
      {u8"ㄓ", "zhi"}, {u8"ㄔ", "chi"}, {u8"ㄕ", "shi"}, {u8"ㄖ", "ri"},
      {u8"ㄗ", "zi"}, {u8"ㄘ", "ci"}, {u8"ㄙ", "si"}, {u8"ㄦ", "er"}, {u8"ㄝ", "eh"},
      {u8"ㄧ", "yi"}, {u8"ㄨ", "wu"}, {u8"ㄩ", "yu"}, {u8"ㄩㄢ", "yuan"},
      {u8"ㄩㄥ", "yong"}, {u8"ㄨㄥ", "weng"}, {u8"ㄧㄡ", "you"}, {u8"ㄧㄣ", "yin"},
  };
  for (const auto& pair : pairs) {
    const auto parsed = zh::ParseZhuyinSyllable(pair.symbols);
    CHECK(parsed.status == zh::Status::kOk);
    CHECK(parsed.syllable.canonical_pinyin == pair.pinyin);
    CHECK(parsed.syllable.source_bytes.end_byte == pair.symbols.size());
  }
  // Explore the finite three-slot phonotactic grammar (not a corpus/fuzzer).
  // Every accepted onset/medial/rime tuple must have a real canonical reading,
  // and all of its UTF-8 scalar-boundary prefixes must be typeable.
  const std::array<std::string_view, 22> initials = {
      "", u8"ㄅ", u8"ㄆ", u8"ㄇ", u8"ㄈ", u8"ㄉ", u8"ㄊ", u8"ㄋ", u8"ㄌ", u8"ㄍ", u8"ㄎ",
      u8"ㄏ", u8"ㄐ", u8"ㄑ", u8"ㄒ", u8"ㄓ", u8"ㄔ", u8"ㄕ", u8"ㄖ", u8"ㄗ", u8"ㄘ", u8"ㄙ"};
  const std::array<std::string_view, 4> medials = {"", u8"ㄧ", u8"ㄨ", u8"ㄩ"};
  const std::array<std::string_view, 14> rimes = {
      "", u8"ㄚ", u8"ㄛ", u8"ㄜ", u8"ㄝ", u8"ㄞ", u8"ㄟ", u8"ㄠ", u8"ㄡ", u8"ㄢ", u8"ㄣ", u8"ㄤ", u8"ㄥ", u8"ㄦ"};
  std::size_t legal_tuple_count = 0;
  std::size_t rejected_tuple_count = 0;
  std::vector<std::string> canonical_readings;
  for (const auto initial : initials) {
    for (const auto medial : medials) {
      for (const auto rime : rimes) {
        const std::string text = std::string(initial) + std::string(medial) + std::string(rime);
        const auto parsed = zh::ParseZhuyinSyllable(text);
        if (parsed.status != zh::Status::kOk) {
          ++rejected_tuple_count;
          continue;
        }
        ++legal_tuple_count;
        CHECK(std::find(canonical_readings.begin(), canonical_readings.end(),
                        parsed.syllable.canonical_pinyin) == canonical_readings.end());
        canonical_readings.push_back(parsed.syllable.canonical_pinyin);
        CHECK(zh::ParsePinyinSyllable(parsed.syllable.canonical_pinyin).status == zh::Status::kOk);
        for (std::size_t end_byte = 3; end_byte <= text.size(); end_byte += 3) {
          const auto prefix = zh::ParseZhuyinSyllable(std::string_view(text).substr(0, end_byte));
          CHECK(prefix.status == zh::Status::kOk || prefix.status == zh::Status::kIncomplete);
        }
        for (const auto mark : {u8"ˉ", u8"ˊ", u8"ˇ", u8"ˋ", u8"˙"}) {
          const auto toned = zh::ParseZhuyinSyllable(text + mark);
          CHECK(toned.status == zh::Status::kOk);
          CHECK(toned.syllable.canonical_pinyin == parsed.syllable.canonical_pinyin);
          const auto roundtrip = zh::ParseZhuyinSyllable(toned.syllable.phonetic_utf8);
          CHECK(roundtrip.status == zh::Status::kOk);
          CHECK(roundtrip.syllable.tone == toned.syllable.tone);
        }
      }
    }
  }
  CHECK(legal_tuple_count > 390 && legal_tuple_count < 430);
  CHECK(rejected_tuple_count > 700);
  for (const auto text : {u8"ㄍㄧ", u8"ㄌㄌ", u8"ㄅㄦ", u8"ㄑㄨ", u8"ㄧㄋ", u8"ㄕㄧ", u8"ㄋㄩㄢ",
                          u8"ˊㄋㄧ", u8"ㄋㄧˇˊ", u8"ㄋˋ"}) {
    CHECK(zh::ParseZhuyinSyllable(text).status == zh::Status::kInvalidPhonetic);
  }
  CHECK(zh::ParseZhuyinSyllable(u8"ㄋ").status == zh::Status::kIncomplete);
  CHECK(zh::ParseZhuyinSyllable(u8"˙").status == zh::Status::kIncomplete);
  CHECK(zh::ParseZhuyinSyllable("su3").status == zh::Status::kProtectedInput);
  const std::array<std::string, 5> marks = {u8"ˉ", u8"ˊ", u8"ˇ", u8"ˋ", u8"˙"};
  for (std::size_t ordinal = 0; ordinal < marks.size(); ++ordinal) {
    const auto parsed = zh::ParseZhuyinSyllable(std::string(u8"ㄇㄚ") + marks[ordinal]);
    CHECK(parsed.status == zh::Status::kOk);
    CHECK(static_cast<std::size_t>(parsed.syllable.tone) == ordinal + 1);
  }
  CHECK(zh::ParseZhuyinSyllable(u8"ㄇㄚ˙").syllable.phonetic_utf8 == u8"˙ㄇㄚ");
  CHECK(zh::ParseZhuyinSyllable(u8"˙ㄇㄚ").syllable.tone == zh::Tone::kNeutral);
  auto result = zh::AnalyzeZhuyin(u8"ㄋㄧˇㄏㄠˇ");
  CHECK(result.status == zh::Status::kOk);
  CHECK(result.alternatives.front().syllables.size() == 2);
  CHECK(result.alternatives.front().syllables[0].source_bytes.end_byte == 8);
  CHECK(result.alternatives.front().syllables[1].source_bytes.begin_byte == 8);
  CHECK(result.alternatives.front().syllables[1].source_bytes.end_byte == 16);
  CHECK(result.alternatives.front().phonetic_utf8 == u8"ㄋㄧˇ ㄏㄠˇ");
  CHECK(!result.ambiguous);
  result = zh::AnalyzeZhuyin(u8"ㄋㄧ ㄏㄠ");
  CHECK(result.status == zh::Status::kOk);
  CHECK(result.alternatives.front().syllables[1].source_bytes.begin_byte == 7);
  result = zh::AnalyzeZhuyin(u8"ㄋㄧ ");
  CHECK(result.status == zh::Status::kIncomplete);
  CHECK(result.alternatives.front().pending_bytes->begin_byte == 7);
  result = zh::AnalyzeZhuyin(u8"ㄋㄧˇㄏ");
  CHECK(result.status == zh::Status::kIncomplete);
  CHECK(result.alternatives.front().pending_bytes->begin_byte == 8);
  for (const auto text : {u8"ㄋㄧㄏㄠ", u8" ㄋㄧ", u8"ㄋㄧ  ㄏㄠ", u8"ㄋ ㄧ", u8"ㄋㄧˇˊ"}) {
    CHECK(zh::AnalyzeZhuyin(text).status == zh::Status::kInvalidPhonetic);
  }
  zh::PhoneticComposer keyboard(zh::InputMode::kZhuyinStandardKeyboard);
  for (const char key : std::string("su3cl3")) {
    const auto status = keyboard.TypeKey(key);
    CHECK(status == zh::Status::kOk || status == zh::Status::kIncomplete);
  }
  CHECK(keyboard.preedit_utf8() == u8"ㄋㄧˇㄏㄠˇ");
  auto commit = keyboard.CommitPhonetic();
  CHECK(commit.status == zh::Status::kOk);
  CHECK(commit.phonetic_utf8 == u8"ㄋㄧˇ ㄏㄠˇ");
  CHECK(commit.consumed_input_bytes == 16);
  CHECK(keyboard.preedit_utf8().empty());
  CHECK(keyboard.TypeKey('5') == zh::Status::kOk);
  CHECK(keyboard.TypeKey(' ') == zh::Status::kOk);
  CHECK(keyboard.CommitPhonetic().phonetic_utf8 == u8"ㄓˉ");
  CHECK(keyboard.TypeKey('a') == zh::Status::kIncomplete);
  CHECK(keyboard.TypeKey('8') == zh::Status::kOk);
  CHECK(keyboard.TypeKey('7') == zh::Status::kOk);
  CHECK(keyboard.CommitPhonetic().phonetic_utf8 == u8"˙ㄇㄚ");
  CHECK(keyboard.AppendText("su3") == zh::Status::kProtectedInput);
  CHECK(keyboard.TypeKey('@') == zh::Status::kProtectedInput);
  CHECK(keyboard.preedit_utf8().empty());
  struct Mapping { char key; std::string_view symbol; };
  const Mapping map[] = {
      {'1', u8"ㄅ"}, {'q', u8"ㄆ"}, {'a', u8"ㄇ"}, {'z', u8"ㄈ"}, {'2', u8"ㄉ"},
      {'w', u8"ㄊ"}, {'s', u8"ㄋ"}, {'x', u8"ㄌ"}, {'e', u8"ㄍ"}, {'d', u8"ㄎ"},
      {'c', u8"ㄏ"}, {'r', u8"ㄐ"}, {'f', u8"ㄑ"}, {'v', u8"ㄒ"}, {'5', u8"ㄓ"},
      {'t', u8"ㄔ"}, {'g', u8"ㄕ"}, {'b', u8"ㄖ"}, {'y', u8"ㄗ"}, {'h', u8"ㄘ"},
      {'n', u8"ㄙ"}, {'u', u8"ㄧ"}, {'j', u8"ㄨ"}, {'m', u8"ㄩ"}, {'8', u8"ㄚ"},
      {'i', u8"ㄛ"}, {'k', u8"ㄜ"}, {',', u8"ㄝ"}, {'9', u8"ㄞ"}, {'o', u8"ㄟ"},
      {'l', u8"ㄠ"}, {'.', u8"ㄡ"}, {'0', u8"ㄢ"}, {'p', u8"ㄣ"}, {';', u8"ㄤ"},
      {'/', u8"ㄥ"}, {'-', u8"ㄦ"},
  };
  for (const auto& entry : map) {
    keyboard.Cancel();
    const auto status = keyboard.TypeKey(entry.key);
    CHECK(status == zh::Status::kOk || status == zh::Status::kIncomplete);
    CHECK(keyboard.preedit_utf8() == entry.symbol);
  }
}

void Composition() {
  zh::PhoneticComposer pinyin(zh::InputMode::kPinyin);
  CHECK(pinyin.CommitPhonetic().status == zh::Status::kEmpty);
  CHECK(!pinyin.Backspace());
  CHECK(pinyin.AppendText("n") == zh::Status::kIncomplete);
  CHECK(pinyin.CommitPhonetic().status == zh::Status::kIncomplete);
  CHECK(pinyin.preedit_utf8() == "n");
  CHECK(pinyin.AppendText(u8"ǚ") == zh::Status::kOk);
  CHECK(pinyin.preedit_utf8().size() == 3);
  CHECK(pinyin.AppendText("@example.com") == zh::Status::kProtectedInput);
  CHECK(pinyin.preedit_utf8() == u8"nǚ");
  CHECK(pinyin.Backspace());
  CHECK(pinyin.preedit_utf8() == "n");
  CHECK(pinyin.AppendText("v3") == zh::Status::kOk);
  auto commit = pinyin.CommitPhonetic();
  CHECK(commit.status == zh::Status::kOk);
  CHECK(commit.phonetic_utf8 == u8"nü3");
  CHECK(commit.consumed_input_bytes == 3);
  CHECK(pinyin.preedit_utf8().empty());
  CHECK(pinyin.AppendText("xi'an") == zh::Status::kOk);
  CHECK(pinyin.Backspace());
  CHECK(pinyin.preedit_utf8() == "xi'a");
  CHECK(pinyin.Backspace());
  CHECK(pinyin.Analyze().status == zh::Status::kIncomplete);
  CHECK(pinyin.Backspace());
  CHECK(pinyin.preedit_utf8() == "xi");
  pinyin.Cancel();
  CHECK(pinyin.Analyze().status == zh::Status::kEmpty);
  CHECK(pinyin.AppendText("xian") == zh::Status::kOk);
  CHECK(pinyin.CommitPhonetic().status == zh::Status::kAmbiguous);
  CHECK(pinyin.preedit_utf8() == "xian");
  CHECK(pinyin.CommitPhonetic(999).status == zh::Status::kInvalidSelection);
  commit = pinyin.CommitPhonetic(1);
  CHECK(commit.status == zh::Status::kOk);
  CHECK(commit.phonetic_utf8 == "xi'an");
  CHECK(pinyin.preedit_utf8().empty());
  zh::Limits limits;
  limits.max_alternatives = 1;
  zh::PhoneticComposer bounded(zh::InputMode::kPinyin, limits);
  CHECK(bounded.AppendText("xian") == zh::Status::kOk);
  CHECK(bounded.CommitPhonetic().status == zh::Status::kAmbiguous);
  CHECK(bounded.CommitPhonetic(0).status == zh::Status::kOk);
  CHECK(pinyin.TypeKey('s') == zh::Status::kInvalidOptions);
  zh::PhoneticComposer symbols(zh::InputMode::kZhuyinSymbols);
  CHECK(symbols.AppendText(u8"ㄋㄧˇ") == zh::Status::kOk);
  CHECK(symbols.Backspace());
  CHECK(symbols.preedit_utf8() == u8"ㄋㄧ");
  CHECK(symbols.Backspace());
  CHECK(symbols.preedit_utf8() == u8"ㄋ");
  CHECK(symbols.Analyze().status == zh::Status::kIncomplete);
  CHECK(symbols.AppendText(u8"ㄌ") == zh::Status::kInvalidPhonetic);
  CHECK(symbols.preedit_utf8() == u8"ㄋ");
  symbols.Cancel();
  CHECK(!symbols.Backspace());
  CHECK(symbols.AppendText(u8"˙ㄇㄚ") == zh::Status::kOk);
  CHECK(symbols.Backspace());
  CHECK(symbols.Analyze().status == zh::Status::kIncomplete);
  CHECK(symbols.Backspace());
  CHECK(symbols.preedit_utf8() == u8"˙");
  CHECK(symbols.CommitPhonetic().status == zh::Status::kIncomplete);
  CHECK(symbols.Backspace());
  CHECK(symbols.preedit_utf8().empty());
  // Same instance after cancel/commit behaves like a fresh instance: no learning.
  for (int count = 0; count < 3; ++count) {
    CHECK(pinyin.AppendText("ni3") == zh::Status::kOk);
    CHECK(pinyin.CommitPhonetic().phonetic_utf8 == "ni3");
    CHECK(pinyin.Analyze().status == zh::Status::kEmpty);
  }
}

void BoundsProtection() {
  for (const auto text : {"https://nihao.cn/xian", "www.xian.com", "ni@hao.cn", "ni+tag@example.com",
                          "ni_hao", "xi-an", "ni.hao", "ni/hao", "ni\\hao", "ni:hao", "ni?hao",
                          "ni#hao", "ni&hao", "ni=hao", "ni%hao", "ni+hao", "ni,hao", "ni;hao",
                          "ni!hao", "ni(hao)", "ni[hao]", "ni{hao}", "ni\"hao", "ni`hao", "ni~hao",
                          "ni\thao", "ni\nhao", "ni hao", "ma9", u8"ni，hao", u8"xi’an", u8"你好"}) {
    const auto result = zh::SegmentPinyin(text);
    CHECK(result.status == zh::Status::kProtectedInput);
    CHECK(result.alternatives.empty());
    zh::PhoneticComposer pinyin(zh::InputMode::kPinyin);
    CHECK(pinyin.AppendText(text) == zh::Status::kProtectedInput);
    CHECK(pinyin.preedit_utf8().empty());
  }
  const std::string embedded_nul("ni\0hao", 6);
  CHECK(zh::SegmentPinyin(embedded_nul).status == zh::Status::kProtectedInput);
  CHECK(zh::AnalyzeZhuyin(u8"ㄋㄧ@example.com").status == zh::Status::kProtectedInput);
  CHECK(zh::AnalyzeZhuyin(u8"ㄋㄧ/ㄏㄠ").status == zh::Status::kProtectedInput);
  // UTF-8 is validated strictly, before token protection, with original byte offsets.
  const std::vector<std::string> malformed = {
      std::string("\x80", 1), std::string("\xc0\xaf", 2), std::string("\xc1\xbf", 2),
      std::string("\xe0\x80\xaf", 3), std::string("\xed\xa0\x80", 3),
      std::string("\xf4\x90\x80\x80", 4), std::string("\xf5\x80\x80\x80", 4),
      std::string("\xf0\x9f", 2), std::string("\xc3", 1), std::string("\xe3\x41\x80", 3),
  };
  for (const auto& bytes : malformed) {
    CHECK(zh::ParsePinyinSyllable(bytes).status == zh::Status::kInvalidUtf8);
    CHECK(zh::ParseZhuyinSyllable(bytes).status == zh::Status::kInvalidUtf8);
    CHECK(zh::SegmentPinyin(bytes).status == zh::Status::kInvalidUtf8);
    CHECK(zh::AnalyzeZhuyin(bytes).status == zh::Status::kInvalidUtf8);
    zh::PhoneticComposer composer(zh::InputMode::kPinyin);
    CHECK(composer.AppendText(bytes) == zh::Status::kInvalidUtf8);
    CHECK(composer.preedit_utf8().empty());
  }
  CHECK(zh::SegmentPinyin(std::string("ni\xc3", 3)).error_byte == 2);
  CHECK(zh::SegmentPinyin(std::string("ni\xc3x", 4)).error_byte == 3);
  CHECK(zh::SegmentPinyin(u8"nü@").error_byte == 3);
  CHECK(zh::SegmentPinyin(u8"nü\u0308").status == zh::Status::kProtectedInput);
  CHECK(zh::SegmentPinyin(u8"ni😀").status == zh::Status::kProtectedInput);
  CHECK(zh::ParsePinyinSyllable(std::string(zh::kMaxSyllableBytes + 1, 'a')).status ==
        zh::Status::kLimitExceeded);
  zh::Limits limits;
  limits.max_input_bytes = 3;
  CHECK(zh::SegmentPinyin(u8"nü", limits).status == zh::Status::kOk);
  CHECK(zh::SegmentPinyin(u8"nü3", limits).status == zh::Status::kLimitExceeded);
  zh::PhoneticComposer capped(zh::InputMode::kPinyin, limits);
  CHECK(capped.AppendText(u8"nü") == zh::Status::kOk);
  CHECK(capped.AppendText("3") == zh::Status::kLimitExceeded);
  CHECK(capped.preedit_utf8() == u8"nü");
  limits = {};
  limits.max_syllables = 1;
  CHECK(zh::AnalyzeZhuyin(u8"ㄋㄧ ㄏㄠ", limits).status == zh::Status::kLimitExceeded);
  CHECK(zh::AnalyzeZhuyin(u8"ㄋㄧ ㄏ", limits).status == zh::Status::kLimitExceeded);
  CHECK(zh::SegmentPinyin("ni'", limits).status == zh::Status::kLimitExceeded);
  CHECK(zh::SegmentPinyin("nih", limits).status == zh::Status::kLimitExceeded);
  for (int setting = 0; setting < 6; ++setting) {
    limits = {};
    if (setting == 0) limits.max_input_bytes = 0;
    if (setting == 1) limits.max_input_bytes = zh::kMaxInputBytes + 1;
    if (setting == 2) limits.max_syllables = 0;
    if (setting == 3) limits.max_syllables = zh::kMaxSyllables + 1;
    if (setting == 4) limits.max_alternatives = 0;
    if (setting == 5) limits.max_alternatives = zh::kMaxAlternatives + 1;
    CHECK(zh::SegmentPinyin("ni", limits).status == zh::Status::kInvalidOptions);
    CHECK(zh::AnalyzeZhuyin(u8"ㄋㄧ", limits).status == zh::Status::kInvalidOptions);
    zh::PhoneticComposer invalid(zh::InputMode::kPinyin, limits);
    CHECK(invalid.AppendText("ni") == zh::Status::kInvalidOptions);
    CHECK(invalid.preedit_utf8().empty());
  }
  limits = {zh::kMaxInputBytes, zh::kMaxSyllables, zh::kMaxAlternatives};
  std::string exactly;
  for (std::size_t count = 0; count < zh::kMaxSyllables; ++count) exactly += "ni3";
  auto result = zh::SegmentPinyin(exactly, limits);
  CHECK(result.status == zh::Status::kOk);
  CHECK(result.alternatives.front().syllables.size() == zh::kMaxSyllables);
  CHECK(zh::SegmentPinyin(exactly + "ni3", limits).status == zh::Status::kLimitExceeded);
  CHECK(zh::SegmentPinyin(std::string(zh::kMaxInputBytes + 1, 'a'), limits).status ==
        zh::Status::kLimitExceeded);
}

zh::PhoneticPath Reading(std::string_view text) {
  auto result = zh::SegmentPinyin(text);
  CHECK(result.status == zh::Status::kOk);
  if (result.alternatives.empty()) return {};
  return result.alternatives.front();
}

void Dictionary() {
  const auto reading = Reading("ni3'hao3");
  auto result = zh::ConvertPhonetic(reading);
  CHECK(result.status == zh::Status::kUnavailable);
  CHECK(result.source_status == zh::SourceStatus::kUnavailable);
  CHECK(result.candidates.empty());
  CHECK(!result.frequency_ranking_available && !result.traditional_simplified_available);
  zh::DictionaryData data;
  data.schema.schema_id = "isolated-synthetic-schema";
  data.source_id = "unit-test-synthetic-only-not-a-language-dataset";
  // Caller-supplied labels deliberately are not a tiny hardcoded Hanzi dictionary.
  data.entries = {{{"ni3", "hao3"}, "SYNTHETIC-A"},
                  {{"ni3", "hao3"}, "SYNTHETIC-B"},
                  {{"ni3", "hao3"}, "SYNTHETIC-A"},
                  {{"li3", "hao3"}, "SYNTHETIC-L"},
                  {{"ni3", "hao4"}, "SYNTHETIC-TONE"}};
  CHECK(zh::ConvertPhonetic(reading, data).status == zh::Status::kUnavailable);
  data.source_status = zh::SourceStatus::kCallerSupplied;
  result = zh::ConvertPhonetic(reading, data);
  CHECK(result.status == zh::Status::kOk);
  CHECK(result.source_status == zh::SourceStatus::kCallerSupplied);
  CHECK(result.candidates.size() == 2);
  CHECK(result.candidates[0].text_utf8 == "SYNTHETIC-A");
  CHECK(result.candidates[1].source_entry_ordinal == 1);
  CHECK(!result.candidates_truncated);
  CHECK(!result.frequency_ranking_available && !result.traditional_simplified_available);
  result = zh::ConvertPhonetic(reading, data, 1);
  CHECK(result.candidates.size() == 1 && result.candidates_truncated);
  result = zh::ConvertPhonetic(Reading("ni'hao"), data);
  CHECK(result.status == zh::Status::kOk && result.candidates.empty());
  data.schema.tone_sensitive = false;
  result = zh::ConvertPhonetic(Reading("ni'hao"), data);
  CHECK(result.candidates.size() == 3);
  CHECK(result.candidates.back().text_utf8 == "SYNTHETIC-TONE");
  data.schema.tone_sensitive = true;
  data.schema.system = zh::PhoneticSystem::kZhuyin;
  data.entries = {{{u8"ㄋㄧˇ", u8"ㄏㄠˇ"}, "SYNTHETIC-ZHUYIN"}};
  result = zh::ConvertPhonetic(reading, data);
  CHECK(result.status == zh::Status::kOk);
  CHECK(result.candidates.size() == 1);
  CHECK(result.candidates.front().text_utf8 == "SYNTHETIC-ZHUYIN");
  const auto zhuyin = zh::AnalyzeZhuyin(u8"ㄋㄧˇㄏㄠˇ").alternatives.front();
  data.schema.system = zh::PhoneticSystem::kPinyin;
  data.entries = {{{"ni3", "hao3"}, "SYNTHETIC-PINYIN"}};
  CHECK(zh::ConvertPhonetic(zhuyin, data).candidates.size() == 1);
  auto malformed = data;
  malformed.entries.push_back({{"biong"}, "SYNTHETIC-INVALID"});
  result = zh::ConvertPhonetic(reading, malformed, 1);
  CHECK(result.status == zh::Status::kInvalidData);
  CHECK(result.candidates.empty());  // Invalid late rows cannot leak a successful prefix.
  malformed = data;
  malformed.entries.back().text_utf8 = std::string("\xed\xa0\x80", 3);
  CHECK(zh::ConvertPhonetic(reading, malformed).status == zh::Status::kInvalidData);
  malformed = data;
  malformed.entries.back().text_utf8 = "SYNTHETIC\nCONTROL";
  CHECK(zh::ConvertPhonetic(reading, malformed).status == zh::Status::kInvalidData);
  malformed = data;
  malformed.source_id.clear();
  CHECK(zh::ConvertPhonetic(reading, malformed).status == zh::Status::kInvalidData);
  malformed = data;
  malformed.schema.schema_id.clear();
  CHECK(zh::ConvertPhonetic(reading, malformed).status == zh::Status::kInvalidData);
  malformed = data;
  malformed.entries.back().syllables_utf8.clear();
  CHECK(zh::ConvertPhonetic(reading, malformed).status == zh::Status::kInvalidData);
  malformed = data;
  malformed.entries.back().syllables_utf8[0] = "n";
  CHECK(zh::ConvertPhonetic(reading, malformed).status == zh::Status::kInvalidData);
  malformed = data;
  malformed.entries.back().text_utf8.assign(zh::kMaxCandidateBytes + 1, 'X');
  CHECK(zh::ConvertPhonetic(reading, malformed).status == zh::Status::kLimitExceeded);
  malformed = data;
  malformed.entries.assign(zh::kMaxDictionaryEntries + 1, data.entries.front());
  CHECK(zh::ConvertPhonetic(reading, malformed).status == zh::Status::kLimitExceeded);
  malformed = data;
  malformed.entries.front().text_utf8.assign(zh::kMaxCandidateBytes, 'X');
  const auto large_synthetic_row = malformed.entries.front();
  malformed.entries.assign(1024, large_synthetic_row);
  CHECK(zh::ConvertPhonetic(reading, malformed).status == zh::Status::kLimitExceeded);
  auto bad_reading = reading;
  bad_reading.syllables[0].tone = static_cast<zh::Tone>(99);
  CHECK(zh::ConvertPhonetic(bad_reading, data).status == zh::Status::kInvalidData);
  bad_reading = reading;
  bad_reading.syllables[0].canonical_pinyin = "li";
  CHECK(zh::ConvertPhonetic(bad_reading, data).status == zh::Status::kInvalidData);
  bad_reading = reading;
  bad_reading.syllables[1].source_bytes.begin_byte = 0;
  CHECK(zh::ConvertPhonetic(bad_reading, data).status == zh::Status::kInvalidData);
  CHECK(zh::ConvertPhonetic(zh::SegmentPinyin("ni'").alternatives.front(), data).status ==
        zh::Status::kIncomplete);
  CHECK(zh::ConvertPhonetic(reading, data, 0).status == zh::Status::kInvalidOptions);
  CHECK(zh::ConvertPhonetic(reading, data, zh::kMaxCandidates + 1).status == zh::Status::kInvalidOptions);
  data.entries.clear();
  result = zh::ConvertPhonetic(reading, data);
  CHECK(result.status == zh::Status::kOk && result.candidates.empty());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::string_view mode(argv[1]);
  if (mode == "--pinyin") PinyinGrammar();
  else if (mode == "--segmentation") Segmentation();
  else if (mode == "--zhuyin") Zhuyin();
  else if (mode == "--composition") Composition();
  else if (mode == "--bounds") BoundsProtection();
  else if (mode == "--dictionary") Dictionary();
  else if (mode == "--forcefail") CHECK(false);
  else return 2;
  if (failures != 0) {
    std::cerr << "RESULT FAIL checks=" << checks << " failures=" << failures << '\n';
    return 1;
  }
  if (checks == 0) return 3;
  std::cout << "RESULT PASS checks=" << checks << '\n';
  return 0;
}
