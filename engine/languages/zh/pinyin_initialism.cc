#include "pinyin_initialism.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace epocanva {
namespace zh {
namespace {

constexpr std::size_t kMaxRows = 250000;
constexpr std::size_t kMaxSyllables = 4096;
constexpr std::size_t kMaxCharactersPerSyllable = 8;
constexpr std::size_t kMaxSyllablesPerInitial = 16;
constexpr std::size_t kMaxCharactersPerInitialSyllable = 4;
constexpr std::size_t kMaxBeam = 12;
constexpr std::size_t kMaxSyllableBytes = 6;
constexpr double kSyllablePenalty = -1.0;

std::size_t ScalarBytes(unsigned char lead) {
  if (lead < 0x80) return 1;
  if ((lead & 0xe0) == 0xc0) return 2;
  if ((lead & 0xf0) == 0xe0) return 3;
  if ((lead & 0xf8) == 0xf0) return 4;
  return 0;
}

// The dictionary is already trusted data, but an invalid scalar must not become
// a candidate surface. Returns 0 when `text` is not exactly one valid scalar.
std::size_t OneScalarBytes(std::string_view text) {
  if (text.empty()) return 0;
  const auto lead = static_cast<unsigned char>(text[0]);
  const std::size_t width = ScalarBytes(lead);
  if (width == 0 || width != text.size()) return 0;
  for (std::size_t index = 1; index < width; ++index) {
    if ((static_cast<unsigned char>(text[index]) & 0xc0) != 0x80) return 0;
  }
  return width;
}

bool NormalizeSyllable(std::string_view raw, std::string* out) {
  if (raw.empty()) return false;
  std::size_t end = raw.size();
  const char last = raw[end - 1];
  if (last >= '1' && last <= '5') --end;
  if (end == 0 || end > kMaxSyllableBytes) return false;
  out->clear();
  out->reserve(end);
  for (std::size_t index = 0; index < end; ++index) {
    char unit = raw[index];
    if (unit >= 'A' && unit <= 'Z') unit = static_cast<char>(unit - 'A' + 'a');
    if (unit < 'a' || unit > 'z') return false;
    out->push_back(unit);
  }
  return true;
}

bool AllLowerAscii(std::string_view text, std::size_t minimum, std::size_t maximum) {
  if (text.size() < minimum || text.size() > maximum) return false;
  return std::all_of(text.begin(), text.end(), [](char unit) {
    return unit >= 'a' && unit <= 'z';
  });
}

}  // namespace

std::shared_ptr<const PinyinInitialism> PinyinInitialism::Build(
    std::string_view dict_utf8, const prediction::Model& model,
    std::string* diagnostic) {
  auto fail = [diagnostic](const char* reason) -> std::shared_ptr<const PinyinInitialism> {
    if (diagnostic != nullptr) *diagnostic = reason;
    return nullptr;
  };
  if (!model.available()) return fail("model-unavailable");
  auto decoder = std::make_shared<PinyinInitialism>();
  decoder->model_ = &model;
  // Surface text -> characters for that syllable. The temporary map is bounded
  // by kMaxSyllables and is converted to the query-shaped index below.
  std::map<std::string, std::vector<Character>> characters_by_syllable;
  std::map<char, std::vector<std::string>> syllables_by_initial;
  bool in_records = false;
  std::size_t rows = 0;
  for (std::size_t begin = 0; begin <= dict_utf8.size();) {
    const std::size_t newline = dict_utf8.find('\n', begin);
    std::string_view line = dict_utf8.substr(
        begin, newline == std::string_view::npos ? std::string_view::npos : newline - begin);
    begin = newline == std::string_view::npos ? dict_utf8.size() + 1 : newline + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (!in_records) {
      if (line == "...") in_records = true;
      continue;
    }
    if (line.empty() || line.front() == '#') continue;
    if (++rows > kMaxRows) return fail("row-limit");
    const std::size_t first_tab = line.find('\t');
    if (first_tab == std::string_view::npos) continue;
    const std::size_t second_tab = line.find('\t', first_tab + 1);
    const std::string_view surface = line.substr(0, first_tab);
    const std::string_view reading = line.substr(
        first_tab + 1, second_tab == std::string_view::npos ? std::string_view::npos
                                                            : second_tab - first_tab - 1);
    if (OneScalarBytes(surface) == 0) continue;
    const auto* entry = model.Find("zh", surface);
    if (entry == nullptr || entry->kind != prediction::TokenKind::kWord) continue;
    for (std::size_t start = 0; start <= reading.size();) {
      const std::size_t space = reading.find(' ', start);
      const std::string_view syllable_view = reading.substr(
          start, space == std::string_view::npos ? std::string_view::npos : space - start);
      start = space == std::string_view::npos ? reading.size() + 1 : space + 1;
      std::string syllable;
      if (!NormalizeSyllable(syllable_view, &syllable)) continue;
      if (decoder->syllables_.insert(syllable).second) {
        if (decoder->syllables_.size() > kMaxSyllables) return fail("syllable-limit");
        syllables_by_initial[syllable[0]].push_back(syllable);
      }
      auto& characters = characters_by_syllable[syllable];
      const bool known = std::any_of(characters.begin(), characters.end(),
          [&](const Character& value) { return value.id == entry->id; });
      if (known) continue;
      if (characters.size() >= kMaxCharactersPerSyllable) continue;
      Character character;
      character.id = entry->id;
      character.surface_utf8.assign(surface);
      character.unigram = model.UnigramCount(entry->id);
      characters.push_back(std::move(character));
    }
  }
  if (!in_records) return fail("missing-record-separator");
  for (auto& entry : characters_by_syllable) {
    auto& characters = entry.second;
    std::sort(characters.begin(), characters.end(), [](const Character& left, const Character& right) {
      if (left.unigram != right.unigram) return left.unigram > right.unigram;
      return left.id < right.id;
    });
  }
  for (auto& initial : syllables_by_initial) {
    initial.second.erase(std::remove_if(initial.second.begin(), initial.second.end(),
        [&](const std::string& syllable) {
          return characters_by_syllable[syllable].empty();
        }), initial.second.end());
    std::sort(initial.second.begin(), initial.second.end(),
        [&](const std::string& left, const std::string& right) {
          const auto left_best = characters_by_syllable[left].front().unigram;
          const auto right_best = characters_by_syllable[right].front().unigram;
          if (left_best != right_best) return left_best > right_best;
          return left < right;
        });
    if (initial.second.size() > kMaxSyllablesPerInitial) initial.second.resize(kMaxSyllablesPerInitial);
    for (const auto& syllable : initial.second) {
      auto& characters = characters_by_syllable[syllable];
      if (characters.size() > kMaxCharactersPerInitialSyllable)
        characters.resize(kMaxCharactersPerInitialSyllable);
      decoder->by_initial_[initial.first].push_back(Syllable{syllable, std::move(characters)});
    }
  }
  if (diagnostic != nullptr) diagnostic->clear();
  return decoder;
}

bool PinyinInitialism::FullPinyin(std::string_view input) const {
  if (input.empty() || input.size() > 12 || syllables_.empty()) return false;
  std::vector<char> reachable(input.size() + 1, 0);
  reachable[0] = 1;
  for (std::size_t offset = 0; offset < input.size(); ++offset) {
    if (!reachable[offset]) continue;
    for (std::size_t width = 1; width <= kMaxSyllableBytes && offset + width <= input.size();
         ++width) {
      if (syllables_.count(std::string(input.substr(offset, width))) != 0)
        reachable[offset + width] = 1;
    }
  }
  return reachable[input.size()] != 0;
}

std::vector<InitialismCandidate> PinyinInitialism::Query(
    std::string_view ascii_input, const prediction::Context& context,
    const prediction::PersonalCounts* personal, std::size_t max_candidates) const {
  std::vector<InitialismCandidate> result;
  if (model_ == nullptr) return result;
  if (!AllLowerAscii(ascii_input, 2, 8) || FullPinyin(ascii_input)) return result;
  if (max_candidates == 0) return result;
  if (max_candidates > 8) max_candidates = 8;
  struct State {
    std::string text_utf8;
    std::vector<prediction::LexemeId> ids;
    double score = 0.0;
  };
  std::vector<State> beam(1);
  for (const char initial : ascii_input) {
    const auto found = by_initial_.find(initial);
    if (found == by_initial_.end()) return {};
    std::vector<State> expanded;
    for (const auto& state : beam) {
      prediction::Context previous = context;
      for (const auto id : state.ids) {
        previous.push_back(id);
        if (previous.size() > 2) previous.erase(previous.begin());
      }
      for (const auto& syllable : found->second) {
        for (const auto& character : syllable.characters) {
          const auto scored = model_->Score(previous, character.id, personal);
          if (scored.status != prediction::Status::kOk) continue;
          State next;
          next.text_utf8 = state.text_utf8 + character.surface_utf8;
          next.ids = state.ids;
          next.ids.push_back(character.id);
          next.score = state.score + scored.log_probability + kSyllablePenalty;
          expanded.push_back(std::move(next));
        }
      }
    }
    if (expanded.empty()) return {};
    std::sort(expanded.begin(), expanded.end(), [](const State& left, const State& right) {
      if (left.score != right.score) return left.score > right.score;
      if (left.ids != right.ids) return left.ids < right.ids;
      return left.text_utf8 < right.text_utf8;
    });
    std::set<std::vector<prediction::LexemeId>> seen;
    std::vector<State> pruned;
    for (auto& state : expanded) {
      if (!seen.insert(state.ids).second) continue;
      pruned.push_back(std::move(state));
      if (pruned.size() == kMaxBeam) break;
    }
    beam = std::move(pruned);
  }
  std::sort(beam.begin(), beam.end(), [](const State& left, const State& right) {
    if (left.score != right.score) return left.score > right.score;
    if (left.text_utf8 != right.text_utf8) return left.text_utf8 < right.text_utf8;
    return left.ids < right.ids;
  });
  std::set<std::string> seen_text;
  for (auto& state : beam) {
    if (!seen_text.insert(state.text_utf8).second) continue;
    InitialismCandidate candidate;
    candidate.text_utf8 = std::move(state.text_utf8);
    candidate.lexeme_ids = std::move(state.ids);
    candidate.log_score = state.score;
    result.push_back(std::move(candidate));
    if (result.size() == max_candidates) break;
  }
  return result;
}

}  // namespace zh
}  // namespace epocanva
