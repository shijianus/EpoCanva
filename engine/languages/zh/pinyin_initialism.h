// Bounded Mandarin pinyin-initialism decoding over the verified Terra readings.
//
// This is deliberately NOT a second pinyin transliterator: it only answers the
// 简拼 case the Rime script_translator does not cover ("nh" -> 你好). Full pinyin
// (for example "nihao") is detected as a legal syllable sequence and left to
// Rime. Entries come from the same local Terra dictionary Rime already loads;
// characters must exist in the admitted prediction catalog so every candidate
// has a real identity and can be scored with the character n-gram model.
//
// No platform headers, no I/O, no history. All text is UTF-8; input is restricted
// to lowercase ASCII initials.
#ifndef EPOCANVA_ZH_PINYIN_INITIALISM_H_
#define EPOCANVA_ZH_PINYIN_INITIALISM_H_

#include "engine/prediction/model.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace epocanva {
namespace zh {

struct InitialismCandidate {
  std::string text_utf8;
  std::vector<prediction::LexemeId> lexeme_ids;
  // Sum of conditional character log probabilities and the abbreviation penalty.
  double log_score = 0.0;
};

class PinyinInitialism {
 public:
  // `dict_utf8` is the complete Terra dictionary body, already hash-verified by
  // the caller. Build rejects malformed/oversized input and never retains the
  // source buffer. Characters absent from the model are skipped; a result with
  // no usable characters is still a valid (empty) decoder.
  static std::shared_ptr<const PinyinInitialism> Build(
      std::string_view dict_utf8, const prediction::Model& model,
      std::string* diagnostic = nullptr);

  // `ascii_input` must be 2..8 lowercase ASCII letters. Returns empty when the
  // input is already a legal full-pinyin syllable sequence or when no admitted
  // character path exists. Results are bounded by `max_candidates` (1..8) and
  // by the internal beam; this is a suggestion source, not a complete decoder.
  std::vector<InitialismCandidate> Query(
      std::string_view ascii_input, const prediction::Context& context,
      const prediction::PersonalCounts* personal,
      std::size_t max_candidates) const;

  std::size_t syllable_count() const { return syllables_.size(); }
  bool empty() const { return syllables_.empty(); }

 private:
  struct Character {
    prediction::LexemeId id = 0;
    std::string surface_utf8;
    std::uint64_t unigram = 0;
  };
  struct Syllable {
    std::string text;
    std::vector<Character> characters;
  };
  bool FullPinyin(std::string_view input) const;

  const prediction::Model* model_ = nullptr;
  std::map<char, std::vector<Syllable>> by_initial_;
  std::set<std::string> syllables_;
};

}  // namespace zh
}  // namespace epocanva
#endif  // EPOCANVA_ZH_PINYIN_INITIALISM_H_
