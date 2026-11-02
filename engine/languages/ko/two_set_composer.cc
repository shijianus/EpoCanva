#include "two_set_composer.h"

namespace epocanva {
namespace ko {
namespace {

// Modern Hangul L/V/T alphabet ordinals, not text offsets. Keys are the ASCII
// characters produced by the standard two-set layout, not platform scan codes.
struct KeyJamo {
  int initial_ordinal;
  int medial_ordinal;
  int final_ordinal;
};

KeyJamo DecodeKey(char ascii_byte) {
  switch (ascii_byte) {
    case 'R': return {1, -1, 2};   // ㄲ
    case 'E': return {4, -1, 0};   // ㄸ (not a final)
    case 'Q': return {8, -1, 0};   // ㅃ (not a final)
    case 'T': return {10, -1, 20}; // ㅆ
    case 'W': return {13, -1, 0};  // ㅉ (not a final)
    case 'O': return {-1, 3, 0};   // ㅒ
    case 'P': return {-1, 7, 0};   // ㅖ
    default: break;
  }
  if (ascii_byte >= 'A' && ascii_byte <= 'Z') {
    ascii_byte = static_cast<char>(ascii_byte + ('a' - 'A'));
  }
  switch (ascii_byte) {
    case 'r': return {0, -1, 1};
    case 's': return {2, -1, 4};
    case 'e': return {3, -1, 7};
    case 'f': return {5, -1, 8};
    case 'a': return {6, -1, 16};
    case 'q': return {7, -1, 17};
    case 't': return {9, -1, 19};
    case 'd': return {11, -1, 21};
    case 'w': return {12, -1, 22};
    case 'c': return {14, -1, 23};
    case 'z': return {15, -1, 24};
    case 'x': return {16, -1, 25};
    case 'v': return {17, -1, 26};
    case 'g': return {18, -1, 27};
    case 'k': return {-1, 0, 0};
    case 'o': return {-1, 1, 0};
    case 'i': return {-1, 2, 0};
    case 'j': return {-1, 4, 0};
    case 'p': return {-1, 5, 0};
    case 'u': return {-1, 6, 0};
    case 'h': return {-1, 8, 0};
    case 'y': return {-1, 12, 0};
    case 'n': return {-1, 13, 0};
    case 'b': return {-1, 17, 0};
    case 'm': return {-1, 18, 0};
    case 'l': return {-1, 20, 0};
    default: return {-1, -1, 0};
  }
}

// Exact two-keystroke compound vowels. No sequential ㅏ+ㅣ/ㅓ+ㅣ guesses:
// ㅐ/ㅔ have their own keys. Likewise ㅘ+ㅣ is not an extra alias for ㅙ.
int CombineVowels(int first_medial_ordinal, int second_medial_ordinal) {
  if (first_medial_ordinal == 8) {  // ㅗ
    if (second_medial_ordinal == 0) return 9;   // ㅘ
    if (second_medial_ordinal == 1) return 10;  // ㅙ
    if (second_medial_ordinal == 20) return 11; // ㅚ
  }
  if (first_medial_ordinal == 13) { // ㅜ
    if (second_medial_ordinal == 4) return 14;  // ㅝ
    if (second_medial_ordinal == 5) return 15;  // ㅞ
    if (second_medial_ordinal == 20) return 16; // ㅟ
  }
  if (first_medial_ordinal == 18 && second_medial_ordinal == 20) return 19; // ㅢ
  return -1;
}

struct FinalCluster {
  int first_final_ordinal;
  int second_final_ordinal;
  int compound_final_ordinal;
  int next_initial_ordinal;
};

constexpr FinalCluster kFinalClusters[] = {
    {1, 19, 3, 9},   // ㄱ + ㅅ -> ㄳ, move ㅅ
    {4, 22, 5, 12},  // ㄴ + ㅈ -> ㄵ, move ㅈ
    {4, 27, 6, 18},  // ㄴ + ㅎ -> ㄶ, move ㅎ
    {8, 1, 9, 0},    // ㄹ + ㄱ -> ㄺ, move ㄱ
    {8, 16, 10, 6},  // ㄹ + ㅁ -> ㄻ, move ㅁ
    {8, 17, 11, 7},  // ㄹ + ㅂ -> ㄼ, move ㅂ
    {8, 19, 12, 9},  // ㄹ + ㅅ -> ㄽ, move ㅅ
    {8, 25, 13, 16}, // ㄹ + ㅌ -> ㄾ, move ㅌ
    {8, 26, 14, 17}, // ㄹ + ㅍ -> ㄿ, move ㅍ
    {8, 27, 15, 18}, // ㄹ + ㅎ -> ㅀ, move ㅎ
    {17, 19, 18, 9}, // ㅂ + ㅅ -> ㅄ, move ㅅ
};

int CombineFinals(int first_final_ordinal, int second_final_ordinal) {
  for (const auto& cluster : kFinalClusters) {
    if (cluster.first_final_ordinal == first_final_ordinal &&
        cluster.second_final_ordinal == second_final_ordinal) {
      return cluster.compound_final_ordinal;
    }
  }
  // Repeated unshifted consonants never synthesize tense consonants, including
  // ㄱ+ㄱ and ㅅ+ㅅ as finals. ㄲ/ㅆ must come from shifted R/T in one input step.
  return 0;
}

int InitialForSimpleFinal(int final_ordinal) {
  switch (final_ordinal) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    case 7: return 3;
    case 8: return 5;
    case 16: return 6;
    case 17: return 7;
    case 19: return 9;
    case 20: return 10;
    case 21: return 11;
    case 22: return 12;
    case 23: return 14;
    case 24: return 15;
    case 25: return 16;
    case 26: return 17;
    case 27: return 18;
    default: return -1;
  }
}

constexpr char32_t kInitialCompatibilityJamo[] = {
    U'ㄱ', U'ㄲ', U'ㄴ', U'ㄷ', U'ㄸ', U'ㄹ', U'ㅁ', U'ㅂ', U'ㅃ', U'ㅅ',
    U'ㅆ', U'ㅇ', U'ㅈ', U'ㅉ', U'ㅊ', U'ㅋ', U'ㅌ', U'ㅍ', U'ㅎ',
};

// All callers supply U+3131..U+3163 or U+AC00..U+D7A3, which are three-byte
// non-surrogate Unicode scalars. No raw non-ASCII input bytes reach this encoder.
void AppendHangulScalar(char32_t unicode_scalar, std::string* output_utf8) {
  output_utf8->push_back(static_cast<char>(0xe0 | (unicode_scalar >> 12)));
  output_utf8->push_back(static_cast<char>(0x80 | ((unicode_scalar >> 6) & 0x3f)));
  output_utf8->push_back(static_cast<char>(0x80 | (unicode_scalar & 0x3f)));
}

}  // namespace

InputResult TwoSetComposer::PushAscii(char ascii_byte) {
  const auto unsigned_byte = static_cast<unsigned char>(ascii_byte);
  if (unsigned_byte < 0x20 || unsigned_byte > 0x7e) return InputResult::kRejectedByte;
  if (history_ascii_.size() >= kMaxInputSteps) return InputResult::kInputLimit;
  history_ascii_.push_back(ascii_byte);
  ProcessAscii(ascii_byte);
  return InputResult::kAccepted;
}

void TwoSetComposer::ProcessAscii(char ascii_byte) {
  const auto jamo = DecodeKey(ascii_byte);
  if (jamo.initial_ordinal >= 0) {
    ProcessConsonant(jamo.initial_ordinal, jamo.final_ordinal);
  } else if (jamo.medial_ordinal >= 0) {
    ProcessVowel(jamo.medial_ordinal);
  } else {
    FlushActive();
    resolved_utf8_.push_back(ascii_byte);
  }
}

void TwoSetComposer::ProcessConsonant(int initial_ordinal, int final_ordinal) {
  if (initial_ordinal_ >= 0 && medial_ordinal_ >= 0 && final_ordinal != 0) {
    if (final_ordinal_ == 0) {
      final_ordinal_ = final_ordinal;
      return;
    }
    const int compound_final_ordinal = CombineFinals(final_ordinal_, final_ordinal);
    if (compound_final_ordinal != 0) {
      final_ordinal_ = compound_final_ordinal;
      return;
    }
  }
  FlushActive();
  initial_ordinal_ = initial_ordinal;
}

void TwoSetComposer::ProcessVowel(int medial_ordinal) {
  if (medial_ordinal_ == -1) {
    // Empty -> standalone vowel, or initial -> syllable; never insert implicit ㅇ.
    medial_ordinal_ = medial_ordinal;
    return;
  }
  if (final_ordinal_ != 0) {
    int remaining_final_ordinal = 0;
    int next_initial_ordinal = InitialForSimpleFinal(final_ordinal_);
    for (const auto& cluster : kFinalClusters) {
      if (cluster.compound_final_ordinal == final_ordinal_) {
        remaining_final_ordinal = cluster.first_final_ordinal;
        next_initial_ordinal = cluster.next_initial_ordinal;
        break;
      }
    }
    // Preserve a cluster's first final, or remove a simple/shifted tense final;
    // that final (or the cluster's second component) starts the next syllable.
    final_ordinal_ = remaining_final_ordinal;
    FlushActive();
    initial_ordinal_ = next_initial_ordinal;
    medial_ordinal_ = medial_ordinal;
    return;
  }
  const int compound_medial_ordinal = CombineVowels(medial_ordinal_, medial_ordinal);
  if (compound_medial_ordinal >= 0) {
    medial_ordinal_ = compound_medial_ordinal;
    return;
  }
  FlushActive();
  medial_ordinal_ = medial_ordinal;
}

void TwoSetComposer::AppendActive(std::string* output_utf8) const {
  if (initial_ordinal_ >= 0 && medial_ordinal_ >= 0) {
    // Unicode modern Hangul composition: 19 L x 21 V x 28 T, T=0 is no final.
    const auto unicode_scalar = static_cast<char32_t>(
        0xac00 + (initial_ordinal_ * 21 + medial_ordinal_) * 28 + final_ordinal_);
    AppendHangulScalar(unicode_scalar, output_utf8);
  } else if (initial_ordinal_ >= 0) {
    AppendHangulScalar(kInitialCompatibilityJamo[initial_ordinal_], output_utf8);
  } else if (medial_ordinal_ >= 0) {
    AppendHangulScalar(static_cast<char32_t>(0x314f + medial_ordinal_), output_utf8);
  }
}

void TwoSetComposer::FlushActive() {
  AppendActive(&resolved_utf8_);
  initial_ordinal_ = -1;
  medial_ordinal_ = -1;
  final_ordinal_ = 0;
}

void TwoSetComposer::RebuildFromHistory() {
  resolved_utf8_.clear();
  initial_ordinal_ = -1;
  medial_ordinal_ = -1;
  final_ordinal_ = 0;
  for (char ascii_byte : history_ascii_) ProcessAscii(ascii_byte);
}

bool TwoSetComposer::Backspace() {
  if (history_ascii_.empty()) return false;
  history_ascii_.pop_back();
  RebuildFromHistory();
  return true;
}

void TwoSetComposer::Cancel() {
  history_ascii_.clear();
  resolved_utf8_.clear();
  initial_ordinal_ = -1;
  medial_ordinal_ = -1;
  final_ordinal_ = 0;
}

std::string TwoSetComposer::CommitUtf8() {
  std::string committed_utf8 = resolved_utf8_;
  AppendActive(&committed_utf8);
  Cancel();
  return committed_utf8;
}

Preedit TwoSetComposer::GetPreedit() const {
  Preedit preedit;
  preedit.text_utf8 = resolved_utf8_;
  preedit.active_begin_utf8_bytes = preedit.text_utf8.size();
  AppendActive(&preedit.text_utf8);
  preedit.caret_utf8_bytes = preedit.text_utf8.size();
  preedit.active_size_utf8_bytes =
      preedit.text_utf8.size() - preedit.active_begin_utf8_bytes;
  preedit.input_step_count = history_ascii_.size();
  return preedit;
}

}  // namespace ko
}  // namespace epocanva
