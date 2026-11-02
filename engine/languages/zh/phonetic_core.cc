#include "phonetic_core.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <utility>

namespace epocanva {
namespace zh {
namespace {

struct Scalar {
  char32_t value = 0;
  ByteSpan bytes;
};

bool DecodeUtf8(std::string_view text, std::vector<Scalar>* scalars,
                std::size_t* error_byte) {
  for (std::size_t byte = 0; byte < text.size();) {
    const std::size_t begin_byte = byte;
    const auto lead = static_cast<unsigned char>(text[byte++]);
    char32_t value = 0;
    std::size_t continuation_bytes = 0;
    char32_t minimum = 0;
    if (lead < 0x80) {
      value = lead;
    } else if (lead >= 0xc2 && lead <= 0xdf) {
      value = lead & 0x1f;
      continuation_bytes = 1;
      minimum = 0x80;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      value = lead & 0x0f;
      continuation_bytes = 2;
      minimum = 0x800;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      value = lead & 0x07;
      continuation_bytes = 3;
      minimum = 0x10000;
    } else {
      *error_byte = begin_byte;
      return false;
    }
    if (continuation_bytes > text.size() - byte) {
      *error_byte = begin_byte;
      return false;
    }
    for (std::size_t count = 0; count < continuation_bytes; ++count) {
      const auto next = static_cast<unsigned char>(text[byte]);
      if ((next & 0xc0) != 0x80) {
        *error_byte = byte;
        return false;
      }
      ++byte;
      value = (value << 6) | (next & 0x3f);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) {
      *error_byte = begin_byte;
      return false;
    }
    if (scalars != nullptr) scalars->push_back({value, {begin_byte, byte}});
  }
  return true;
}

bool ValidLimits(const Limits& limits) {
  return limits.max_input_bytes > 0 && limits.max_input_bytes <= kMaxInputBytes &&
         limits.max_syllables > 0 && limits.max_syllables <= kMaxSyllables &&
         limits.max_alternatives > 0 &&
         limits.max_alternatives <= kMaxAlternatives;
}

bool ValidSystem(PhoneticSystem system) {
  return system == PhoneticSystem::kPinyin || system == PhoneticSystem::kZhuyin;
}

bool ValidTone(Tone tone) {
  return tone >= Tone::kUnspecified && tone <= Tone::kNeutral;
}

Tone ToneDigit(char32_t scalar) {
  return static_cast<Tone>(scalar - U'0');
}

struct PinyinLetter {
  char ascii = 0;
  Tone tone = Tone::kUnspecified;
};

PinyinLetter MapPinyinLetter(char32_t scalar) {
  if (scalar >= U'A' && scalar <= U'Z') scalar += U'a' - U'A';
  if (scalar >= U'a' && scalar <= U'z') {
    return {static_cast<char>(scalar), Tone::kUnspecified};
  }
  // Precomposed spellings only: no implicit normalization across byte boundaries.
  static constexpr char32_t marked[][8] = {
      {U'ā', U'á', U'ǎ', U'à', U'Ā', U'Á', U'Ǎ', U'À'},
      {U'ē', U'é', U'ě', U'è', U'Ē', U'É', U'Ě', U'È'},
      {U'ī', U'í', U'ǐ', U'ì', U'Ī', U'Í', U'Ǐ', U'Ì'},
      {U'ō', U'ó', U'ǒ', U'ò', U'Ō', U'Ó', U'Ǒ', U'Ò'},
      {U'ū', U'ú', U'ǔ', U'ù', U'Ū', U'Ú', U'Ǔ', U'Ù'},
      {U'ǖ', U'ǘ', U'ǚ', U'ǜ', U'Ǖ', U'Ǘ', U'Ǚ', U'Ǜ'},
  };
  static constexpr char vowels[] = "aeiouv";
  for (std::size_t vowel_ordinal = 0; vowel_ordinal < 6; ++vowel_ordinal) {
    for (std::size_t form_ordinal = 0; form_ordinal < 8; ++form_ordinal) {
      if (scalar == marked[vowel_ordinal][form_ordinal]) {
        return {vowels[vowel_ordinal],
                static_cast<Tone>(form_ordinal % 4 + 1)};
      }
    }
  }
  if (scalar == U'ü' || scalar == U'Ü') return {'v', Tone::kUnspecified};
  // A single ê is expanded to the documented "eh" key by the parser.
  if (scalar == U'ê' || scalar == U'Ê') return {'^', Tone::kUnspecified};
  return {};
}

Tone ZhuyinTone(char32_t scalar) {
  switch (scalar) {
    case U'ˉ': return Tone::kFirst;
    case U'ˊ': return Tone::kSecond;
    case U'ˇ': return Tone::kThird;
    case U'ˋ': return Tone::kFourth;
    case U'˙': return Tone::kNeutral;
    default: return Tone::kUnspecified;
  }
}

bool IsZhuyinLetter(char32_t scalar) {
  return scalar >= U'ㄅ' && scalar <= U'ㄩ';
}

// Initial/final compatibility is phonotactic grammar, not words, a frequency
// table, or a Rime dictionary. y/w are orthographic zero-initial spellings.
struct InitialRule {
  std::string_view initial;
  std::string_view finals;
  std::string_view zhuyin_initial;
};

constexpr InitialRule kInitialRules[] = {
    {"", "a ai an ang ao o ou e ei en eng er eh", ""},
    {"b", "a ai an ang ao ei en eng i ian iao ie in ing o u", u8"ㄅ"},
    {"p", "a ai an ang ao ei en eng i ian iao ie in ing o ou u", u8"ㄆ"},
    {"m", "a ai an ang ao e ei en eng i ian iao ie in ing iu o ou u", u8"ㄇ"},
    {"f", "a an ang ei en eng o ou u", u8"ㄈ"},
    {"d", "a ai an ang ao e ei en eng i ia ian iao ie ing iu ong ou u uan ui un uo", u8"ㄉ"},
    {"t", "a ai an ang ao e eng i ian iao ie ing ong ou u uan ui un uo", u8"ㄊ"},
    {"n", "a ai an ang ao e ei en eng i ian iang iao ie in ing iu ong ou u uan un uo v ve", u8"ㄋ"},
    {"l", "a ai an ang ao e ei eng i ia ian iang iao ie in ing iu o ong ou u uan un uo v ve", u8"ㄌ"},
    {"g", "a ai an ang ao e ei en eng ong ou u ua uai uan uang ui un uo", u8"ㄍ"},
    {"k", "a ai an ang ao e en eng ong ou u ua uai uan uang ui un uo", u8"ㄎ"},
    {"h", "a ai an ang ao e ei en eng ong ou u ua uai uan uang ui un uo", u8"ㄏ"},
    {"j", "i ia ian iang iao ie in ing iong iu u uan ue un", u8"ㄐ"},
    {"q", "i ia ian iang iao ie in ing iong iu u uan ue un", u8"ㄑ"},
    {"x", "i ia ian iang iao ie in ing iong iu u uan ue un", u8"ㄒ"},
    {"zh", "a ai an ang ao e ei en eng i ong ou u ua uai uan uang ui un uo", u8"ㄓ"},
    {"ch", "a ai an ang ao e en eng i ong ou u ua uai uan uang ui un uo", u8"ㄔ"},
    {"sh", "a ai an ang ao e ei en eng i ou u ua uai uan uang ui un uo", u8"ㄕ"},
    {"r", "an ang ao e en eng i ong ou u uan ui un uo", u8"ㄖ"},
    {"z", "a ai an ang ao e ei en eng i ong ou u uan ui un uo", u8"ㄗ"},
    {"c", "a ai an ang ao e en eng i ong ou u uan ui un uo", u8"ㄘ"},
    {"s", "a ai an ang ao e en eng i ong ou u uan ui un uo", u8"ㄙ"},
    {"y", "a ai an ang ao e i in ing o ong ou u uan ue un", ""},
    {"w", "a ai an ang ei en eng o u", ""},
};

struct FinalRule {
  std::string_view pinyin;
  std::string_view zhuyin;
};

constexpr FinalRule kFinalRules[] = {
    {"a", u8"ㄚ"}, {"ai", u8"ㄞ"}, {"an", u8"ㄢ"},
    {"ang", u8"ㄤ"}, {"ao", u8"ㄠ"}, {"o", u8"ㄛ"},
    {"ou", u8"ㄡ"}, {"e", u8"ㄜ"}, {"ei", u8"ㄟ"},
    {"en", u8"ㄣ"}, {"eng", u8"ㄥ"}, {"er", u8"ㄦ"}, {"eh", u8"ㄝ"},
    {"i", u8"ㄧ"}, {"ia", u8"ㄧㄚ"}, {"iai", u8"ㄧㄞ"},
    {"ian", u8"ㄧㄢ"}, {"iang", u8"ㄧㄤ"}, {"iao", u8"ㄧㄠ"},
    {"ie", u8"ㄧㄝ"}, {"in", u8"ㄧㄣ"}, {"ing", u8"ㄧㄥ"},
    {"io", u8"ㄧㄛ"}, {"iong", u8"ㄩㄥ"}, {"iu", u8"ㄧㄡ"},
    {"u", u8"ㄨ"}, {"ua", u8"ㄨㄚ"}, {"uai", u8"ㄨㄞ"},
    {"uan", u8"ㄨㄢ"}, {"uang", u8"ㄨㄤ"}, {"ui", u8"ㄨㄟ"},
    {"un", u8"ㄨㄣ"}, {"uo", u8"ㄨㄛ"}, {"ueng", u8"ㄨㄥ"},
    {"ong", u8"ㄨㄥ"}, {"v", u8"ㄩ"}, {"ve", u8"ㄩㄝ"},
    {"van", u8"ㄩㄢ"}, {"vn", u8"ㄩㄣ"},
};

struct GrammarSyllable {
  std::string pinyin;
  std::string zhuyin;
};

bool IsFrontRoundedInitial(std::string_view initial) {
  return initial == "j" || initial == "q" || initial == "x" || initial == "y";
}

std::string ZhuyinFor(const InitialRule& initial, std::string_view final) {
  std::string rewritten(final);
  if (initial.initial == "y") {
    if (final == "i" || final == "in" || final == "ing") {
      // yi/yin/ying: y is orthographic, not another ㄧ.
    } else if (final == "ong") {
      rewritten = "iong";
    } else if (final == "ou") {
      rewritten = "iu";
    } else if (!final.empty() && final.front() != 'u') {
      rewritten = "i" + rewritten;
    }
  } else if (initial.initial == "w") {
    if (final == "ei") rewritten = "ui";
    else if (final == "en") rewritten = "un";
    else if (final == "eng") rewritten = "ueng";
    else if (final != "u") rewritten = "u" + rewritten;
  }
  if (IsFrontRoundedInitial(initial.initial) && !rewritten.empty() &&
      rewritten.front() == 'u') {
    rewritten.front() = 'v';
  }
  const bool apical = initial.initial == "zh" || initial.initial == "ch" ||
                      initial.initial == "sh" || initial.initial == "r" ||
                      initial.initial == "z" || initial.initial == "c" ||
                      initial.initial == "s";
  std::string result(initial.zhuyin_initial);
  if (apical && final == "i") return result;
  for (const auto& rule : kFinalRules) {
    if (rule.pinyin == rewritten) {
      result += rule.zhuyin;
      return result;
    }
  }
  return {};  // An unhandled grammar rule cannot become a fabricated syllable.
}

const std::vector<GrammarSyllable>& Grammar() {
  static const std::vector<GrammarSyllable> grammar = [] {
    std::vector<GrammarSyllable> result;
    for (const auto& rule : kInitialRules) {
      std::size_t begin_byte = 0;
      while (begin_byte < rule.finals.size()) {
        auto end_byte = rule.finals.find(' ', begin_byte);
        if (end_byte == std::string_view::npos) end_byte = rule.finals.size();
        const auto final = rule.finals.substr(begin_byte, end_byte - begin_byte);
        result.push_back({std::string(rule.initial) + std::string(final),
                          ZhuyinFor(rule, final)});
        begin_byte = end_byte + 1;
      }
    }
    return result;
  }();
  return grammar;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string RenderPinyin(std::string_view canonical, Tone tone) {
  std::string text;
  if (canonical == "eh") text = u8"ê";
  else {
    for (const char ascii : canonical) {
      if (ascii == 'v') text += u8"ü";
      else text += ascii;
    }
  }
  if (tone != Tone::kUnspecified) {
    text += static_cast<char>('0' + static_cast<int>(tone));
  }
  return text;
}

std::string RenderZhuyin(std::string_view body, Tone tone) {
  if (tone == Tone::kNeutral) return std::string(u8"˙") + std::string(body);
  std::string text(body);
  switch (tone) {
    case Tone::kFirst: text += u8"ˉ"; break;
    case Tone::kSecond: text += u8"ˊ"; break;
    case Tone::kThird: text += u8"ˇ"; break;
    case Tone::kFourth: text += u8"ˋ"; break;
    default: break;
  }
  return text;
}

void RenderPath(PhoneticPath* path) {
  path->phonetic_utf8.clear();
  for (const auto& syllable : path->syllables) {
    if (!path->phonetic_utf8.empty()) {
      path->phonetic_utf8 += path->system == PhoneticSystem::kPinyin ? '\'' : ' ';
    }
    path->phonetic_utf8 += syllable.phonetic_utf8;
  }
}

std::size_t ToneVowelByte(std::string_view ascii) {
  auto byte = ascii.find('a');
  if (byte != std::string_view::npos) return byte;
  byte = ascii.find('e');
  if (byte != std::string_view::npos) return byte;
  byte = ascii.find("ou");
  if (byte != std::string_view::npos) return byte;
  return ascii.find_last_of("iouv");
}

Status CheckInput(std::string_view input, const Limits& limits,
                  PhoneticSystem system, std::vector<Scalar>* scalars,
                  std::size_t* error_byte) {
  if (!ValidLimits(limits)) return Status::kInvalidOptions;
  if (input.size() > limits.max_input_bytes) return Status::kLimitExceeded;
  if (input.empty()) return Status::kEmpty;
  if (!DecodeUtf8(input, scalars, error_byte)) return Status::kInvalidUtf8;
  for (const auto& scalar : *scalars) {
    const bool allowed = system == PhoneticSystem::kPinyin
        ? (MapPinyinLetter(scalar.value).ascii != 0 || scalar.value == U'\'' ||
           (scalar.value >= U'1' && scalar.value <= U'5'))
        : (IsZhuyinLetter(scalar.value) || scalar.value == U' ' ||
           ZhuyinTone(scalar.value) != Tone::kUnspecified);
    if (!allowed) {
      *error_byte = scalar.bytes.begin_byte;
      return Status::kProtectedInput;
    }
  }
  return Status::kOk;
}

SyllableResult SingleFailure(Status status, std::size_t error_byte) {
  SyllableResult result;
  result.status = status;
  result.error_byte = error_byte;
  return result;
}

}  // namespace

SyllableResult ParsePinyinSyllable(std::string_view input_utf8) {
  if (input_utf8.empty()) return {};
  if (input_utf8.size() > kMaxSyllableBytes) {
    return SingleFailure(Status::kLimitExceeded, kNoByteOffset);
  }
  std::vector<Scalar> scalars;
  std::size_t error_byte = kNoByteOffset;
  if (!DecodeUtf8(input_utf8, &scalars, &error_byte)) {
    return SingleFailure(Status::kInvalidUtf8, error_byte);
  }
  std::string ascii;
  Tone tone = Tone::kUnspecified;
  std::size_t marked_ascii_byte = kNoByteOffset;
  std::size_t marked_source_byte = kNoByteOffset;
  bool digit_tone = false;
  for (const auto& scalar : scalars) {
    if (scalar.value >= U'1' && scalar.value <= U'5') {
      if (tone != Tone::kUnspecified || ascii.empty() ||
          scalar.bytes.end_byte != input_utf8.size()) {
        return SingleFailure(Status::kInvalidPhonetic, scalar.bytes.begin_byte);
      }
      tone = ToneDigit(scalar.value);
      digit_tone = true;
      continue;
    }
    const auto letter = MapPinyinLetter(scalar.value);
    if (letter.ascii == 0) {
      return SingleFailure(scalar.value == U'\'' ? Status::kInvalidPhonetic
                                                : Status::kProtectedInput,
                           scalar.bytes.begin_byte);
    }
    if (letter.tone != Tone::kUnspecified) {
      if (tone != Tone::kUnspecified) {
        return SingleFailure(Status::kInvalidPhonetic, scalar.bytes.begin_byte);
      }
      tone = letter.tone;
      marked_ascii_byte = ascii.size();
      marked_source_byte = scalar.bytes.begin_byte;
    }
    if (letter.ascii == '^') ascii += "eh";
    else ascii += letter.ascii;
  }
  if (marked_ascii_byte != kNoByteOffset &&
      marked_ascii_byte != ToneVowelByte(ascii)) {
    return SingleFailure(Status::kInvalidPhonetic, marked_source_byte);
  }
  // Explicit front-rounded aliases are not permission to confuse nu/lu with nü/lü.
  if (ascii.size() >= 2 && (ascii[0] == 'j' || ascii[0] == 'q' ||
                            ascii[0] == 'x' || ascii[0] == 'y') &&
      ascii[1] == 'v') {
    ascii[1] = 'u';
  }
  bool prefix = false;
  for (const auto& grammar : Grammar()) {
    if (grammar.pinyin == ascii) {
      SyllableResult result;
      result.status = Status::kOk;
      result.syllable = {{0, input_utf8.size()}, ascii, tone,
                         RenderPinyin(ascii, tone)};
      return result;
    }
    if (StartsWith(grammar.pinyin, ascii)) prefix = true;
  }
  if (prefix && !digit_tone) return SingleFailure(Status::kIncomplete, kNoByteOffset);
  return SingleFailure(Status::kInvalidPhonetic, 0);
}

SyllableResult ParseZhuyinSyllable(std::string_view input_utf8) {
  if (input_utf8.empty()) return {};
  if (input_utf8.size() > kMaxSyllableBytes) {
    return SingleFailure(Status::kLimitExceeded, kNoByteOffset);
  }
  std::vector<Scalar> scalars;
  std::size_t error_byte = kNoByteOffset;
  if (!DecodeUtf8(input_utf8, &scalars, &error_byte)) {
    return SingleFailure(Status::kInvalidUtf8, error_byte);
  }
  std::string body;
  Tone tone = Tone::kUnspecified;
  bool suffix_tone = false;
  for (const auto& scalar : scalars) {
    const Tone mark = ZhuyinTone(scalar.value);
    if (mark != Tone::kUnspecified) {
      if (tone != Tone::kUnspecified) {
        return SingleFailure(Status::kInvalidPhonetic, scalar.bytes.begin_byte);
      }
      if (scalar.bytes.begin_byte == 0 && mark == Tone::kNeutral) {
        tone = mark;
      } else if (!body.empty() && scalar.bytes.end_byte == input_utf8.size()) {
        tone = mark;
        suffix_tone = true;
      } else {
        return SingleFailure(Status::kInvalidPhonetic, scalar.bytes.begin_byte);
      }
    } else if (IsZhuyinLetter(scalar.value)) {
      body += input_utf8.substr(scalar.bytes.begin_byte,
                               scalar.bytes.end_byte - scalar.bytes.begin_byte);
    } else {
      return SingleFailure(Status::kProtectedInput, scalar.bytes.begin_byte);
    }
  }
  bool prefix = false;
  for (const auto& grammar : Grammar()) {
    if (!grammar.zhuyin.empty() && grammar.zhuyin == body) {
      SyllableResult result;
      result.status = Status::kOk;
      result.syllable = {{0, input_utf8.size()}, grammar.pinyin, tone,
                         RenderZhuyin(body, tone)};
      return result;
    }
    if (!grammar.zhuyin.empty() && StartsWith(grammar.zhuyin, body)) prefix = true;
  }
  if (prefix && !suffix_tone) return SingleFailure(Status::kIncomplete, kNoByteOffset);
  return SingleFailure(Status::kInvalidPhonetic, 0);
}

SegmentationResult SegmentPinyin(std::string_view input_utf8, Limits limits) {
  SegmentationResult result;
  result.input_bytes = input_utf8.size();
  std::vector<Scalar> scalars;
  result.status = CheckInput(input_utf8, limits, PhoneticSystem::kPinyin,
                             &scalars, &result.error_byte);
  if (result.status != Status::kOk) return result;
  bool last_apostrophe = true;
  for (const auto& scalar : scalars) {
    if (scalar.value == U'\'' && last_apostrophe) {
      result.status = Status::kInvalidPhonetic;
      result.error_byte = scalar.bytes.begin_byte;
      return result;
    }
    last_apostrophe = scalar.value == U'\'';
  }

  struct Edge {
    std::size_t next_byte = 0;
    Syllable syllable;
  };
  const std::size_t input_bytes = input_utf8.size();
  std::vector<std::vector<Edge>> edges(input_bytes + 1);
  std::vector<bool> pending(input_bytes + 1, false);
  for (const auto& start : scalars) {
    const std::size_t begin_byte = start.bytes.begin_byte;
    if (start.value == U'\'') continue;
    for (const auto& end : scalars) {
      if (end.bytes.begin_byte < begin_byte) continue;
      if (end.value == U'\'' || end.bytes.end_byte - begin_byte > kMaxSyllableBytes) break;
      const auto part = input_utf8.substr(begin_byte, end.bytes.end_byte - begin_byte);
      auto parsed = ParsePinyinSyllable(part);
      if (parsed.status == Status::kOk) {
        std::size_t next_byte = end.bytes.end_byte;
        if (next_byte < input_bytes && input_utf8[next_byte] == '\'') ++next_byte;
        parsed.syllable.source_bytes = {begin_byte, end.bytes.end_byte};
        edges[begin_byte].push_back({next_byte, std::move(parsed.syllable)});
      }
      if (end.bytes.end_byte == input_bytes && parsed.status == Status::kIncomplete) {
        pending[begin_byte] = true;
      }
    }
    // Ties of equal syllable count prefer longer leading syllables, never a word score.
    std::reverse(edges[begin_byte].begin(), edges[begin_byte].end());
  }
  pending[input_bytes] = last_apostrophe;

  using Counts = std::array<std::size_t, kMaxSyllables + 1>;
  std::vector<Counts> complete_counts(input_bytes + 1);
  std::vector<Counts> pending_counts(input_bytes + 1);
  // Count paths independently of the syllable bound as well. A caller lowering
  // that bound must not make a genuinely ambiguous spelling silently unique.
  std::vector<std::size_t> all_complete(input_bytes + 1, 0);
  std::vector<std::size_t> all_pending(input_bytes + 1, 0);
  const std::size_t infinity = input_bytes + 2;
  std::vector<std::size_t> min_complete(input_bytes + 1, infinity);
  std::vector<std::size_t> min_pending(input_bytes + 1, infinity);
  if (!last_apostrophe) {
    complete_counts[input_bytes][0] = 1;
    all_complete[input_bytes] = 1;
    min_complete[input_bytes] = 0;
  }
  const std::size_t saturation = limits.max_alternatives + 1;
  const auto add_count = [saturation](std::size_t left, std::size_t right) {
    return std::min(saturation, left + right);
  };
  for (std::size_t remaining_bytes = input_bytes + 1; remaining_bytes > 0;) {
    const std::size_t byte = --remaining_bytes;
    if (pending[byte]) {
      pending_counts[byte][1] = 1;
      all_pending[byte] = 1;
      min_pending[byte] = 1;
    }
    for (const auto& edge : edges[byte]) {
      all_complete[byte] = add_count(all_complete[byte], all_complete[edge.next_byte]);
      all_pending[byte] = add_count(all_pending[byte], all_pending[edge.next_byte]);
      min_complete[byte] = std::min(min_complete[byte], min_complete[edge.next_byte] + 1);
      min_pending[byte] = std::min(min_pending[byte], min_pending[edge.next_byte] + 1);
      for (std::size_t count = 1; count <= limits.max_syllables; ++count) {
        complete_counts[byte][count] = add_count(complete_counts[byte][count],
                                                complete_counts[edge.next_byte][count - 1]);
        pending_counts[byte][count] = add_count(pending_counts[byte][count],
                                               pending_counts[edge.next_byte][count - 1]);
      }
    }
  }
  const bool complete = min_complete[0] < infinity;
  const std::size_t minimum = complete ? min_complete[0] : min_pending[0];
  if (minimum == infinity) {
    result.status = Status::kInvalidPhonetic;
    std::vector<bool> reached(input_bytes + 1, false);
    reached[0] = true;
    result.error_byte = 0;
    for (std::size_t byte = 0; byte <= input_bytes; ++byte) {
      if (!reached[byte]) continue;
      result.error_byte = byte;
      for (const auto& edge : edges[byte]) reached[edge.next_byte] = true;
    }
    return result;
  }
  if (minimum > limits.max_syllables) {
    result.status = Status::kLimitExceeded;
    return result;
  }
  const auto& counts = complete ? complete_counts : pending_counts;
  std::size_t retained_path_count = 0;
  for (std::size_t count = 1; count <= limits.max_syllables; ++count) {
    retained_path_count = add_count(retained_path_count, counts[0][count]);
  }
  const std::size_t path_count = complete ? all_complete[0] : all_pending[0];
  result.ambiguous = path_count > 1;
  result.alternatives_truncated = path_count > limits.max_alternatives ||
                                  path_count > retained_path_count;
  result.status = complete ? Status::kOk : Status::kIncomplete;
  PhoneticPath path;
  path.system = PhoneticSystem::kPinyin;
  std::function<void(std::size_t, std::size_t)> collect;
  collect = [&](std::size_t byte, std::size_t syllable_count) {
    if (result.alternatives.size() == limits.max_alternatives) return;
    if ((complete && byte == input_bytes && syllable_count == 0) ||
        (!complete && pending[byte] && syllable_count == 1)) {
      auto answer = path;
      if (!complete) answer.pending_bytes = ByteSpan{byte, input_bytes};
      RenderPath(&answer);
      result.alternatives.push_back(std::move(answer));
      return;
    }
    if (syllable_count == 0) return;
    for (const auto& edge : edges[byte]) {
      if (counts[edge.next_byte][syllable_count - 1] == 0) continue;
      path.syllables.push_back(edge.syllable);
      collect(edge.next_byte, syllable_count - 1);
      path.syllables.pop_back();
      if (result.alternatives.size() == limits.max_alternatives) return;
    }
  };
  for (std::size_t count = minimum; count <= limits.max_syllables; ++count) {
    if (counts[0][count] != 0) collect(0, count);
    if (result.alternatives.size() == limits.max_alternatives) break;
  }
  return result;
}

SegmentationResult AnalyzeZhuyin(std::string_view input_utf8, Limits limits) {
  SegmentationResult result;
  result.input_bytes = input_utf8.size();
  std::vector<Scalar> scalars;
  result.status = CheckInput(input_utf8, limits, PhoneticSystem::kZhuyin,
                             &scalars, &result.error_byte);
  if (result.status != Status::kOk) return result;
  PhoneticPath path;
  path.system = PhoneticSystem::kZhuyin;
  std::size_t begin_byte = 0;
  bool after_space = false;
  bool after_tone = false;
  const auto add_syllable = [&](std::size_t end_byte, bool allow_pending) {
    if (path.syllables.size() >= limits.max_syllables) {
      result.status = Status::kLimitExceeded;
      result.error_byte = begin_byte;
      return false;
    }
    auto parsed = ParseZhuyinSyllable(input_utf8.substr(begin_byte, end_byte - begin_byte));
    if (parsed.status == Status::kIncomplete && allow_pending) {
      path.pending_bytes = ByteSpan{begin_byte, end_byte};
      result.status = Status::kIncomplete;
      return true;
    }
    if (parsed.status != Status::kOk) {
      result.status = parsed.status == Status::kIncomplete ? Status::kInvalidPhonetic
                                                         : parsed.status;
      result.error_byte = parsed.error_byte == kNoByteOffset ? begin_byte
                                                            : begin_byte + parsed.error_byte;
      return false;
    }
    parsed.syllable.source_bytes = {begin_byte, end_byte};
    path.syllables.push_back(std::move(parsed.syllable));
    return true;
  };
  for (const auto& scalar : scalars) {
    if (scalar.value == U' ') {
      if (after_space || (scalar.bytes.begin_byte == begin_byte && !after_tone)) {
        result.status = Status::kInvalidPhonetic;
        result.error_byte = scalar.bytes.begin_byte;
        return result;
      }
      if (scalar.bytes.begin_byte != begin_byte &&
          !add_syllable(scalar.bytes.begin_byte, false)) return result;
      begin_byte = scalar.bytes.end_byte;
      after_space = true;
      after_tone = false;
      continue;
    }
    after_space = false;
    after_tone = false;
    const Tone tone = ZhuyinTone(scalar.value);
    if (tone == Tone::kUnspecified) continue;
    if (scalar.bytes.begin_byte == begin_byte && tone == Tone::kNeutral) continue;
    if (!add_syllable(scalar.bytes.end_byte, false)) return result;
    begin_byte = scalar.bytes.end_byte;
    after_tone = true;
  }
  if (begin_byte < input_utf8.size()) {
    if (!add_syllable(input_utf8.size(), true)) return result;
  } else if (after_space) {
    if (path.syllables.size() == limits.max_syllables) {
      result.status = Status::kLimitExceeded;
      return result;
    }
    path.pending_bytes = ByteSpan{input_utf8.size(), input_utf8.size()};
    result.status = Status::kIncomplete;
  }
  RenderPath(&path);
  result.alternatives.push_back(std::move(path));
  return result;
}

PhoneticComposer::PhoneticComposer(InputMode mode, Limits limits)
    : mode_(mode), limits_(limits) {}

Status PhoneticComposer::AppendText(std::string_view text_utf8) {
  if (!ValidLimits(limits_) ||
      (mode_ != InputMode::kPinyin && mode_ != InputMode::kZhuyinSymbols &&
       mode_ != InputMode::kZhuyinStandardKeyboard)) return Status::kInvalidOptions;
  if (text_utf8.empty()) return Status::kEmpty;
  if (text_utf8.size() > limits_.max_input_bytes - preedit_utf8_.size()) {
    return Status::kLimitExceeded;
  }
  std::string proposed = preedit_utf8_;
  proposed += text_utf8;
  const auto result = mode_ == InputMode::kPinyin ? SegmentPinyin(proposed, limits_)
                                                : AnalyzeZhuyin(proposed, limits_);
  if (result.status == Status::kOk || result.status == Status::kIncomplete) {
    preedit_utf8_ = std::move(proposed);
  }
  return result.status;
}

Status PhoneticComposer::TypeKey(char ascii_key) {
  if (mode_ != InputMode::kZhuyinStandardKeyboard) return Status::kInvalidOptions;
  struct Key { char ascii; std::string_view phonetic; };
  static constexpr Key keys[] = {
      {'1', u8"ㄅ"}, {'q', u8"ㄆ"}, {'a', u8"ㄇ"}, {'z', u8"ㄈ"},
      {'2', u8"ㄉ"}, {'w', u8"ㄊ"}, {'s', u8"ㄋ"}, {'x', u8"ㄌ"},
      {'e', u8"ㄍ"}, {'d', u8"ㄎ"}, {'c', u8"ㄏ"}, {'r', u8"ㄐ"},
      {'f', u8"ㄑ"}, {'v', u8"ㄒ"}, {'5', u8"ㄓ"}, {'t', u8"ㄔ"},
      {'g', u8"ㄕ"}, {'b', u8"ㄖ"}, {'y', u8"ㄗ"}, {'h', u8"ㄘ"},
      {'n', u8"ㄙ"}, {'u', u8"ㄧ"}, {'j', u8"ㄨ"}, {'m', u8"ㄩ"},
      {'8', u8"ㄚ"}, {'i', u8"ㄛ"}, {'k', u8"ㄜ"}, {',', u8"ㄝ"},
      {'9', u8"ㄞ"}, {'o', u8"ㄟ"}, {'l', u8"ㄠ"}, {'.', u8"ㄡ"},
      {'0', u8"ㄢ"}, {'p', u8"ㄣ"}, {';', u8"ㄤ"}, {'/', u8"ㄥ"},
      {'-', u8"ㄦ"}, {'6', u8"ˊ"}, {'3', u8"ˇ"}, {'4', u8"ˋ"},
      {'7', u8"˙"}, {' ', u8"ˉ"},
  };
  for (const auto& key : keys) {
    if (key.ascii == ascii_key) return AppendText(key.phonetic);
  }
  return Status::kProtectedInput;
}

bool PhoneticComposer::Backspace() {
  if (preedit_utf8_.empty()) return false;
  std::size_t begin_byte = preedit_utf8_.size() - 1;
  while (begin_byte > 0 &&
         (static_cast<unsigned char>(preedit_utf8_[begin_byte]) & 0xc0) == 0x80) {
    --begin_byte;
  }
  preedit_utf8_.erase(begin_byte);
  return true;
}

void PhoneticComposer::Cancel() noexcept { preedit_utf8_.clear(); }

const std::string& PhoneticComposer::preedit_utf8() const noexcept { return preedit_utf8_; }

SegmentationResult PhoneticComposer::Analyze() const {
  if (mode_ != InputMode::kPinyin && mode_ != InputMode::kZhuyinSymbols &&
      mode_ != InputMode::kZhuyinStandardKeyboard) {
    SegmentationResult result;
    result.status = Status::kInvalidOptions;
    return result;
  }
  return mode_ == InputMode::kPinyin ? SegmentPinyin(preedit_utf8_, limits_)
                                    : AnalyzeZhuyin(preedit_utf8_, limits_);
}

CommitResult PhoneticComposer::CommitPhonetic(
    std::optional<std::size_t> alternative_ordinal) {
  const auto analysis = Analyze();
  CommitResult result;
  result.status = analysis.status;
  if (analysis.status != Status::kOk) return result;
  if (alternative_ordinal && *alternative_ordinal >= analysis.alternatives.size()) {
    result.status = Status::kInvalidSelection;
    return result;
  }
  if (analysis.ambiguous && !alternative_ordinal) {
    result.status = Status::kAmbiguous;
    return result;
  }
  result.phonetic_utf8 = analysis.alternatives[alternative_ordinal.value_or(0)].phonetic_utf8;
  result.consumed_input_bytes = preedit_utf8_.size();
  Cancel();
  return result;
}

namespace {

bool ValidDataText(std::string_view text) {
  if (text.empty()) return false;
  std::vector<Scalar> scalars;
  std::size_t error_byte = kNoByteOffset;
  if (!DecodeUtf8(text, &scalars, &error_byte)) return false;
  for (const auto& scalar : scalars) {
    if (scalar.value < 0x20 || (scalar.value >= 0x7f && scalar.value <= 0x9f)) return false;
  }
  return true;
}

ConversionResult ConversionFailure(Status status, SourceStatus source) {
  ConversionResult result;
  result.status = status;
  result.source_status = source;
  return result;
}

}  // namespace

ConversionResult ConvertPhonetic(const PhoneticPath& reading,
                                 const DictionaryData& data,
                                 std::size_t max_candidates) {
  if (max_candidates == 0 || max_candidates > kMaxCandidates) {
    return ConversionFailure(Status::kInvalidOptions, SourceStatus::kUnavailable);
  }
  if (data.source_status == SourceStatus::kUnavailable) return {};
  if (data.source_status != SourceStatus::kCallerSupplied ||
      !ValidSystem(data.schema.system) || !ValidSystem(reading.system)) {
    return ConversionFailure(Status::kInvalidData, data.source_status);
  }
  if (data.source_id.size() > 128 || data.schema.schema_id.size() > 128 ||
      data.entries.size() > kMaxDictionaryEntries) {
    return ConversionFailure(Status::kLimitExceeded, data.source_status);
  }
  if (!ValidDataText(data.source_id) || !ValidDataText(data.schema.schema_id)) {
    return ConversionFailure(Status::kInvalidData, data.source_status);
  }
  if (reading.pending_bytes) return ConversionFailure(Status::kIncomplete, data.source_status);
  if (reading.syllables.empty()) return ConversionFailure(Status::kEmpty, data.source_status);
  if (reading.syllables.size() > kMaxSyllables) {
    return ConversionFailure(Status::kLimitExceeded, data.source_status);
  }
  std::size_t previous_end_byte = 0;
  for (const auto& syllable : reading.syllables) {
    if (!ValidTone(syllable.tone) || syllable.source_bytes.begin_byte < previous_end_byte ||
        syllable.source_bytes.begin_byte >= syllable.source_bytes.end_byte ||
        syllable.source_bytes.end_byte > kMaxInputBytes ||
        syllable.canonical_pinyin.size() > kMaxSyllableBytes) {
      return ConversionFailure(Status::kInvalidData, data.source_status);
    }
    const auto parsed = reading.system == PhoneticSystem::kPinyin
        ? ParsePinyinSyllable(syllable.phonetic_utf8)
        : ParseZhuyinSyllable(syllable.phonetic_utf8);
    if (parsed.status != Status::kOk || parsed.syllable.canonical_pinyin != syllable.canonical_pinyin ||
        parsed.syllable.tone != syllable.tone) {
      return ConversionFailure(Status::kInvalidData, data.source_status);
    }
    previous_end_byte = syllable.source_bytes.end_byte;
  }
  ConversionResult result;
  result.status = Status::kOk;
  result.source_status = data.source_status;
  std::size_t total_bytes = data.source_id.size() + data.schema.schema_id.size();
  for (std::size_t row_ordinal = 0; row_ordinal < data.entries.size(); ++row_ordinal) {
    const auto& entry = data.entries[row_ordinal];
    if (entry.syllables_utf8.size() > kMaxSyllables || entry.text_utf8.size() > kMaxCandidateBytes ||
        entry.text_utf8.size() > kMaxDictionaryBytes - total_bytes) {
      return ConversionFailure(Status::kLimitExceeded, data.source_status);
    }
    total_bytes += entry.text_utf8.size();
    if (entry.syllables_utf8.empty() || !ValidDataText(entry.text_utf8)) {
      return ConversionFailure(Status::kInvalidData, data.source_status);
    }
    bool match = entry.syllables_utf8.size() == reading.syllables.size();
    for (std::size_t syllable_ordinal = 0; syllable_ordinal < entry.syllables_utf8.size();
         ++syllable_ordinal) {
      const auto& encoded = entry.syllables_utf8[syllable_ordinal];
      if (encoded.size() > kMaxSyllableBytes || encoded.size() > kMaxDictionaryBytes - total_bytes) {
        return ConversionFailure(Status::kLimitExceeded, data.source_status);
      }
      total_bytes += encoded.size();
      const auto parsed = data.schema.system == PhoneticSystem::kPinyin
          ? ParsePinyinSyllable(encoded) : ParseZhuyinSyllable(encoded);
      if (parsed.status != Status::kOk) return ConversionFailure(Status::kInvalidData, data.source_status);
      if (match) {
        const auto& wanted = reading.syllables[syllable_ordinal];
        match = parsed.syllable.canonical_pinyin == wanted.canonical_pinyin &&
                (!data.schema.tone_sensitive || parsed.syllable.tone == wanted.tone);
      }
    }
    if (!match) continue;
    const bool duplicate = std::any_of(result.candidates.begin(), result.candidates.end(),
        [&](const Candidate& candidate) { return candidate.text_utf8 == entry.text_utf8; });
    if (duplicate) continue;
    if (result.candidates.size() == max_candidates) result.candidates_truncated = true;
    else result.candidates.push_back({entry.text_utf8, row_ordinal});
  }
  return result;
}

}  // namespace zh
}  // namespace epocanva
