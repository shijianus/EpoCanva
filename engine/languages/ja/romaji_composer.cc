#include "romaji_composer.h"

#include <string_view>
#include <utility>

namespace epocanva {
namespace ja {
namespace {

// Phonetic spelling rules, not words, dictionary entries, or candidate lists.
// The longest accepted spelling is four ASCII bytes. Every output is a BMP
// Unicode scalar in the hiragana block; script conversion is applied on output.
struct KanaRule {
  std::string_view romaji_ascii;
  std::u32string_view kana_scalars;
};

constexpr KanaRule kKanaRules[] = {
    {"a", U"あ"}, {"i", U"い"}, {"u", U"う"}, {"e", U"え"}, {"o", U"お"},
    {"ka", U"か"}, {"ki", U"き"}, {"ku", U"く"}, {"ke", U"け"}, {"ko", U"こ"},
    {"ga", U"が"}, {"gi", U"ぎ"}, {"gu", U"ぐ"}, {"ge", U"げ"}, {"go", U"ご"},
    {"sa", U"さ"}, {"si", U"し"}, {"su", U"す"}, {"se", U"せ"}, {"so", U"そ"},
    {"za", U"ざ"}, {"zi", U"じ"}, {"zu", U"ず"}, {"ze", U"ぜ"}, {"zo", U"ぞ"},
    {"ta", U"た"}, {"ti", U"ち"}, {"tu", U"つ"}, {"te", U"て"}, {"to", U"と"},
    {"da", U"だ"}, {"di", U"ぢ"}, {"du", U"づ"}, {"de", U"で"}, {"do", U"ど"},
    {"na", U"な"}, {"ni", U"に"}, {"nu", U"ぬ"}, {"ne", U"ね"}, {"no", U"の"},
    {"ha", U"は"}, {"hi", U"ひ"}, {"hu", U"ふ"}, {"he", U"へ"}, {"ho", U"ほ"},
    {"ba", U"ば"}, {"bi", U"び"}, {"bu", U"ぶ"}, {"be", U"べ"}, {"bo", U"ぼ"},
    {"pa", U"ぱ"}, {"pi", U"ぴ"}, {"pu", U"ぷ"}, {"pe", U"ぺ"}, {"po", U"ぽ"},
    {"ma", U"ま"}, {"mi", U"み"}, {"mu", U"む"}, {"me", U"め"}, {"mo", U"も"},
    {"ya", U"や"}, {"yu", U"ゆ"}, {"yo", U"よ"},
    {"ra", U"ら"}, {"ri", U"り"}, {"ru", U"る"}, {"re", U"れ"}, {"ro", U"ろ"},
    {"wa", U"わ"}, {"wi", U"うぃ"}, {"we", U"うぇ"}, {"wo", U"を"},
    {"shi", U"し"}, {"chi", U"ち"}, {"tsu", U"つ"}, {"fu", U"ふ"}, {"ji", U"じ"},
    {"kya", U"きゃ"}, {"kyu", U"きゅ"}, {"kyo", U"きょ"},
    {"gya", U"ぎゃ"}, {"gyu", U"ぎゅ"}, {"gyo", U"ぎょ"},
    {"sya", U"しゃ"}, {"syu", U"しゅ"}, {"syo", U"しょ"},
    {"sha", U"しゃ"}, {"shu", U"しゅ"}, {"sho", U"しょ"},
    {"zya", U"じゃ"}, {"zyu", U"じゅ"}, {"zyo", U"じょ"},
    {"jya", U"じゃ"}, {"jyu", U"じゅ"}, {"jyo", U"じょ"},
    {"ja", U"じゃ"}, {"ju", U"じゅ"}, {"jo", U"じょ"},
    {"tya", U"ちゃ"}, {"tyu", U"ちゅ"}, {"tyo", U"ちょ"},
    {"cya", U"ちゃ"}, {"cyu", U"ちゅ"}, {"cyo", U"ちょ"},
    {"cha", U"ちゃ"}, {"chu", U"ちゅ"}, {"cho", U"ちょ"},
    {"dya", U"ぢゃ"}, {"dyu", U"ぢゅ"}, {"dyo", U"ぢょ"},
    {"nya", U"にゃ"}, {"nyu", U"にゅ"}, {"nyo", U"にょ"},
    {"hya", U"ひゃ"}, {"hyu", U"ひゅ"}, {"hyo", U"ひょ"},
    {"bya", U"びゃ"}, {"byu", U"びゅ"}, {"byo", U"びょ"},
    {"pya", U"ぴゃ"}, {"pyu", U"ぴゅ"}, {"pyo", U"ぴょ"},
    {"mya", U"みゃ"}, {"myu", U"みゅ"}, {"myo", U"みょ"},
    {"rya", U"りゃ"}, {"ryu", U"りゅ"}, {"ryo", U"りょ"},
    {"she", U"しぇ"}, {"sye", U"しぇ"}, {"je", U"じぇ"}, {"jye", U"じぇ"},
    {"che", U"ちぇ"}, {"tye", U"ちぇ"}, {"cye", U"ちぇ"},
    {"fa", U"ふぁ"}, {"fi", U"ふぃ"}, {"fe", U"ふぇ"}, {"fo", U"ふぉ"},
    {"fya", U"ふゃ"}, {"fyu", U"ふゅ"}, {"fyo", U"ふょ"},
    {"va", U"ゔぁ"}, {"vi", U"ゔぃ"}, {"vu", U"ゔ"}, {"ve", U"ゔぇ"}, {"vo", U"ゔぉ"},
    {"vya", U"ゔゃ"}, {"vyu", U"ゔゅ"}, {"vyo", U"ゔょ"},
    {"tsa", U"つぁ"}, {"tsi", U"つぃ"}, {"tse", U"つぇ"}, {"tso", U"つぉ"},
    {"tha", U"てゃ"}, {"thi", U"てぃ"}, {"thu", U"てゅ"}, {"the", U"てぇ"}, {"tho", U"てょ"},
    {"dha", U"でゃ"}, {"dhi", U"でぃ"}, {"dhu", U"でゅ"}, {"dhe", U"でぇ"}, {"dho", U"でょ"},
    {"wha", U"うぁ"}, {"whi", U"うぃ"}, {"whu", U"う"}, {"whe", U"うぇ"}, {"who", U"うぉ"},
    {"xa", U"ぁ"}, {"xi", U"ぃ"}, {"xu", U"ぅ"}, {"xe", U"ぇ"}, {"xo", U"ぉ"},
    {"la", U"ぁ"}, {"li", U"ぃ"}, {"lu", U"ぅ"}, {"le", U"ぇ"}, {"lo", U"ぉ"},
    {"xya", U"ゃ"}, {"xyu", U"ゅ"}, {"xyo", U"ょ"},
    {"lya", U"ゃ"}, {"lyu", U"ゅ"}, {"lyo", U"ょ"},
    {"xtu", U"っ"}, {"xtsu", U"っ"}, {"ltu", U"っ"}, {"ltsu", U"っ"},
    {"xwa", U"ゎ"}, {"lwa", U"ゎ"}, {"xka", U"ゕ"}, {"lka", U"ゕ"},
    {"xke", U"ゖ"}, {"lke", U"ゖ"},
};

char LowerAscii(char ascii_byte) {
  return ascii_byte >= 'A' && ascii_byte <= 'Z'
             ? static_cast<char>(ascii_byte + ('a' - 'A'))
             : ascii_byte;
}

bool IsLetter(char ascii_byte) {
  const char lower_ascii = LowerAscii(ascii_byte);
  return lower_ascii >= 'a' && lower_ascii <= 'z';
}

bool IsVowel(char lower_ascii) {
  return std::string_view("aeiou").find(lower_ascii) != std::string_view::npos;
}

bool IsPrefix(std::string_view input_ascii, std::string_view rule_ascii) {
  if (input_ascii.size() > rule_ascii.size()) return false;
  for (std::size_t byte_offset = 0; byte_offset < input_ascii.size(); ++byte_offset) {
    if (LowerAscii(input_ascii[byte_offset]) != rule_ascii[byte_offset]) return false;
  }
  return true;
}

// Only fixed, non-surrogate BMP scalars from the rules/nasal/sokuon/mark enter.
void AppendBmpScalar(char32_t unicode_scalar, std::string* output_utf8) {
  if (unicode_scalar <= 0x7f) {
    output_utf8->push_back(static_cast<char>(unicode_scalar));
  } else if (unicode_scalar <= 0x7ff) {
    output_utf8->push_back(static_cast<char>(0xc0 | (unicode_scalar >> 6)));
    output_utf8->push_back(static_cast<char>(0x80 | (unicode_scalar & 0x3f)));
  } else {
    output_utf8->push_back(static_cast<char>(0xe0 | (unicode_scalar >> 12)));
    output_utf8->push_back(static_cast<char>(0x80 | ((unicode_scalar >> 6) & 0x3f)));
    output_utf8->push_back(static_cast<char>(0x80 | (unicode_scalar & 0x3f)));
  }
}

}  // namespace

RomajiComposer::RomajiComposer(KanaMode mode) : mode_(mode) {}

InputResult RomajiComposer::PushAscii(char ascii_byte) {
  const auto unsigned_byte = static_cast<unsigned char>(ascii_byte);
  if (unsigned_byte < 0x20 || unsigned_byte > 0x7e) return InputResult::kRejectedByte;
  if (history_ascii_.size() >= kMaxInputSteps) return InputResult::kInputLimit;
  history_ascii_.push_back(ascii_byte);
  ProcessAscii(ascii_byte);
  return InputResult::kAccepted;
}

void RomajiComposer::AppendKana(char32_t unicode_scalar) {
  // Hiragana U+3041..U+3096 and katakana U+30A1..U+30F6 are parallel blocks.
  // The prolonged-sound mark U+30FC is deliberately the same in either mode.
  if (mode_ == KanaMode::kKatakana && unicode_scalar >= 0x3041 &&
      unicode_scalar <= 0x3096) {
    unicode_scalar += 0x60;
  }
  AppendBmpScalar(unicode_scalar, &resolved_utf8_);
}

void RomajiComposer::ProcessAscii(char ascii_byte) {
  const char lower_ascii = LowerAscii(ascii_byte);
  if (reusable_n_ascii_ != '\0') {
    const char previous_n_ascii = reusable_n_ascii_;
    reusable_n_ascii_ = '\0';
    if (ascii_byte == '\'') return;
    if (IsVowel(lower_ascii) || lower_ascii == 'y') {
      pending_ascii_.push_back(previous_n_ascii);
    }
  }

  if (IsLetter(ascii_byte)) {
    pending_ascii_.push_back(ascii_byte);
    ResolvePending();
    return;
  }

  const bool bare_n = pending_ascii_.size() == 1 &&
                      LowerAscii(pending_ascii_.front()) == 'n';
  if (bare_n) {
    AppendKana(U'ん');
  } else {
    resolved_utf8_.append(pending_ascii_);
  }
  pending_ascii_.clear();
  if (bare_n && ascii_byte == '\'') return;
  if (ascii_byte == '-') {
    AppendKana(U'ー');
  } else {
    resolved_utf8_.push_back(ascii_byte);
  }
}

void RomajiComposer::ResolvePending() {
  while (!pending_ascii_.empty()) {
    const char first_ascii = LowerAscii(pending_ascii_.front());
    if (pending_ascii_.size() >= 2) {
      const char second_ascii = LowerAscii(pending_ascii_[1]);
      if (first_ascii == 'n' && second_ascii == 'n') {
        AppendKana(U'ん');
        reusable_n_ascii_ = pending_ascii_[1];
        pending_ascii_.erase(0, 2);  // UTF-8 code units (ASCII bytes).
        continue;
      }
      if (first_ascii == 'n' && !IsVowel(second_ascii) && second_ascii != 'y') {
        AppendKana(U'ん');
        pending_ascii_.erase(0, 1);  // UTF-8 code unit (ASCII byte).
        continue;
      }
      // All repeated supported onsets except n and the small-kana markers x/l.
      // Unsupported q does not invent a sokuon or disappear.
      if (first_ascii == second_ascii &&
          std::string_view("bcdfghjkmprstvwyz").find(first_ascii) !=
              std::string_view::npos) {
        AppendKana(U'っ');
        pending_ascii_.erase(0, 1);
        continue;
      }
      // Hepburn tcha/tchu/tcho: wait at tc, emit sokuon only after h arrives.
      if (first_ascii == 't' && second_ascii == 'c') {
        if (pending_ascii_.size() == 2) return;
        if (LowerAscii(pending_ascii_[2]) == 'h') {
          AppendKana(U'っ');
          pending_ascii_.erase(0, 1);
          continue;
        }
      }
    }

    bool is_incomplete_rule = first_ascii == 'n' && pending_ascii_.size() == 1;
    for (const auto& rule : kKanaRules) {
      if (!IsPrefix(pending_ascii_, rule.romaji_ascii)) continue;
      if (pending_ascii_.size() == rule.romaji_ascii.size()) {
        for (char32_t unicode_scalar : rule.kana_scalars) AppendKana(unicode_scalar);
        pending_ascii_.clear();
        return;
      }
      is_incomplete_rule = true;
    }
    if (is_incomplete_rule) return;

    // No rule can use the leading byte. Preserve it exactly, then retry the
    // suffix. For example kz+a -> kざ, whereas q is immediately literal q.
    resolved_utf8_.push_back(pending_ascii_.front());
    pending_ascii_.erase(0, 1);
  }
}

void RomajiComposer::RebuildFromHistory() {
  resolved_utf8_.clear();
  pending_ascii_.clear();
  reusable_n_ascii_ = '\0';
  for (char ascii_byte : history_ascii_) ProcessAscii(ascii_byte);
}

bool RomajiComposer::Backspace() {
  if (history_ascii_.empty()) return false;
  history_ascii_.pop_back();
  RebuildFromHistory();
  return true;
}

void RomajiComposer::Cancel() {
  history_ascii_.clear();
  resolved_utf8_.clear();
  pending_ascii_.clear();
  reusable_n_ascii_ = '\0';
}

std::string RomajiComposer::CommitUtf8() {
  if (pending_ascii_.size() == 1 && LowerAscii(pending_ascii_.front()) == 'n') {
    AppendKana(U'ん');
  } else {
    resolved_utf8_.append(pending_ascii_);
  }
  std::string committed_utf8 = std::move(resolved_utf8_);
  Cancel();
  return committed_utf8;
}

Preedit RomajiComposer::GetPreedit() const {
  Preedit preedit;
  preedit.text_utf8 = resolved_utf8_ + pending_ascii_;
  preedit.caret_utf8_bytes = preedit.text_utf8.size();
  preedit.active_begin_utf8_bytes = resolved_utf8_.size();
  preedit.active_size_utf8_bytes = pending_ascii_.size();
  preedit.input_step_count = history_ascii_.size();
  return preedit;
}

void RomajiComposer::SetMode(KanaMode mode) {
  if (mode_ == mode) return;
  mode_ = mode;
  RebuildFromHistory();
}

KanaMode RomajiComposer::GetMode() const { return mode_; }

}  // namespace ja
}  // namespace epocanva
