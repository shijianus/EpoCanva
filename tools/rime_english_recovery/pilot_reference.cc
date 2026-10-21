// EpoCanva P2.4-recovery -- implementation of the independent reference. See
// pilot_reference.h for what it deliberately does not reuse and for the rules it encodes.
#include "pilot_reference.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace epocanva {
namespace recovery {
namespace {

constexpr char kApos = '\'';   // U+0027
constexpr char kHyphen = '-';  // U+002D

bool IsLower(char c) { return c >= 'a' && c <= 'z'; }
bool IsUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsDelimiter(char c) { return c == kApos || c == kHyphen; }

char ToLowerChar(char c) { return IsUpper(c) ? static_cast<char>(c + 32) : c; }

}  // namespace

const char* RefShapeName(RefShape shape) {
  switch (shape) {
    case RefShape::kEmpty:
      return "empty";
    case RefShape::kLowercase:
      return "lowercase";
    case RefShape::kInitialCapital:
      return "initial-capital";
    case RefShape::kUppercase:
      return "uppercase";
    case RefShape::kMixed:
      return "mixed";
    case RefShape::kNotAlphabetic:
      return "not-alphabetic";
    case RefShape::kTooLong:
      return "too-long";
  }
  return "?";
}

RefShape RefClassify(const std::string& text) {
  if (text.empty()) {
    return RefShape::kEmpty;
  }
  if (text.size() > kRefMaxWordBytes) {
    return RefShape::kTooLong;
  }
  bool any_letter = false;
  for (const char c : text) {
    if (IsDelimiter(c)) {
      continue;  // case-transparent: neither a letter nor an unsupported byte
    }
    if (!IsLower(c) && !IsUpper(c)) {
      return RefShape::kNotAlphabetic;
    }
    any_letter = true;
  }
  if (!any_letter) {
    return RefShape::kNotAlphabetic;  // "'", "-", "''": no letter to give a shape to
  }
  if (text.size() == 1) {
    return IsUpper(text[0]) ? RefShape::kInitialCapital : RefShape::kLowercase;
  }
  bool all_upper = true, all_lower = true;
  for (const char c : text) {
    if (IsDelimiter(c)) {
      continue;
    }
    all_upper = all_upper && IsUpper(c);
    all_lower = all_lower && IsLower(c);
  }
  if (all_upper) {
    return RefShape::kUppercase;
  }
  if (all_lower) {
    return RefShape::kLowercase;
  }
  // First letter upper and every LATER letter lower -- letters, not bytes, so a
  // delimiter in between does not reset anything.
  bool first_seen = false, first_upper = false, later_ok = true;
  for (const char c : text) {
    if (IsDelimiter(c)) {
      continue;
    }
    if (!first_seen) {
      first_upper = IsUpper(c);
      first_seen = true;
      continue;
    }
    if (!IsLower(c)) {
      later_ok = false;
    }
  }
  if (first_upper && later_ok) {
    return RefShape::kInitialCapital;
  }
  return RefShape::kMixed;
}

bool RefLookupKey(const std::string& text, std::string* out) {
  const RefShape shape = RefClassify(text);
  if (shape != RefShape::kLowercase && shape != RefShape::kInitialCapital &&
      shape != RefShape::kUppercase) {
    return false;
  }
  std::string key;
  key.reserve(text.size());
  for (const char c : text) {
    key.push_back(IsDelimiter(c) ? c : ToLowerChar(c));
  }
  *out = key;
  return true;
}

std::string RefRender(const std::string& canonical, RefShape shape) {
  if (shape != RefShape::kLowercase && shape != RefShape::kInitialCapital &&
      shape != RefShape::kUppercase) {
    return canonical;  // no invented transformation for a shape we do not complete
  }
  if (shape == RefShape::kLowercase) {
    return canonical;
  }
  std::string out;
  out.reserve(canonical.size());
  bool first_letter_done = false;
  for (const char c : canonical) {
    if (IsDelimiter(c)) {
      out.push_back(c);
      continue;
    }
    if (shape == RefShape::kUppercase) {
      out.push_back(IsLower(c) ? static_cast<char>(c - 32) : c);
      first_letter_done = true;
      continue;
    }
    // initial-capital: the FIRST letter only; a delimiter before it does not count.
    if (!first_letter_done) {
      out.push_back(IsLower(c) ? static_cast<char>(c - 32) : c);
      first_letter_done = true;
    } else {
      out.push_back(IsUpper(c) ? static_cast<char>(c + 32) : c);
    }
  }
  return out;
}

std::string RefJoinMenu(const std::vector<std::string>& items) {
  std::string line;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0) {
      line += "|";
    }
    line += items[i];
  }
  return line;
}

namespace {

// The grammar of one row, checked here rather than trusted from the converter.
bool WordIsLoadable(const std::string& w, std::string* why) {
  if (w.empty()) {
    *why = "empty word field";
    return false;
  }
  if (w.size() > kRefMaxWordBytes) {
    *why = "word longer than 64 code units";
    return false;
  }
  int delimiters = 0;
  bool apos = false, hyphen = false;
  for (std::size_t i = 0; i < w.size(); ++i) {
    const char c = w[i];
    if (IsDelimiter(c)) {
      ++delimiters;
      if (c == kApos) {
        apos = true;
      } else {
        hyphen = true;
      }
      if (i == 0 || i + 1 == w.size()) {
        *why = "delimiter at either end of the word";
        return false;
      }
      continue;
    }
    if (!IsLower(c)) {
      *why = std::string("byte outside the word alphabet: '") + c + "'";
      return false;
    }
  }
  if (delimiters > 1) {
    *why = "more than one delimiter";
    return false;
  }
  if (apos && hyphen) {
    *why = "both delimiter kinds in one word";
    return false;
  }
  return true;
}

bool WeightIsLoadable(const std::string& f, std::uint32_t* value, std::string* why) {
  if (f.empty()) {
    *why = "empty weight field";
    return false;
  }
  if (f.size() > 1 && f[0] == '0') {
    *why = "weight with a leading zero";
    return false;
  }
  std::uint64_t acc = 0;
  for (const char c : f) {
    if (!IsDigit(c)) {
      *why = "non-digit in the weight field";
      return false;
    }
    acc = acc * 10u + static_cast<std::uint64_t>(c - '0');
    if (acc > kRefMaxWeight) {
      *why = "weight overflows uint32";
      return false;
    }
  }
  *value = static_cast<std::uint32_t>(acc);
  return true;
}

}  // namespace

bool PilotReference::Load(const std::filesystem::path& tsv, std::string* error) {
  rows_.clear();
  std::ifstream in(tsv, std::ios::binary);
  if (!in) {
    *error = "the pilot word list cannot be opened";
    return false;
  }
  std::string prev;
  std::size_t line = 0;
  for (std::string text; std::getline(in, text);) {
    ++line;
    if (!text.empty() && text.back() == '\r') {
      text.pop_back();
    }
    if (text.empty()) {
      std::ostringstream os;
      os << "line " << line << ": blank line in the word list";
      *error = os.str();
      return false;
    }
    const std::size_t tab = text.find('\t');
    if (tab == std::string::npos) {
      std::ostringstream os;
      os << "line " << line << ": no tab separator";
      *error = os.str();
      return false;
    }
    if (text.find('\t', tab + 1) != std::string::npos) {
      std::ostringstream os;
      os << "line " << line << ": more than one tab";
      *error = os.str();
      return false;
    }
    // No trailing-empty-record special case: std::getline() does not produce a record for a
    // final LF, so an empty text here is a blank line in the middle of the file -- or a file
    // ending with two newlines -- and both are the grammar break the converter documents as
    // impossible. A reference that quietly accepted them would stop checking the data.
    std::string why;
    const std::string word = text.substr(0, tab);
    std::uint32_t weight = 0;
    if (!WordIsLoadable(word, &why)) {
      *error = "line " + std::to_string(line) + ": " + why;
      return false;
    }
    if (!WeightIsLoadable(text.substr(tab + 1), &weight, &why)) {
      *error = "line " + std::to_string(line) + ": " + why;
      return false;
    }
    if (!rows_.empty()) {
      if (word == prev) {
        *error = "line " + std::to_string(line) + ": duplicate word";
        return false;
      }
      if (word < prev) {
        *error = "line " + std::to_string(line) + ": out of ascending byte order";
        return false;
      }
    }
    prev = word;
    rows_.push_back(RefRow{word, weight});
  }
  if (rows_.empty()) {
    *error = "the pilot word list holds no rows";
    return false;
  }
  return true;
}

bool PilotReference::ContainsExact(const std::string& canonical_word) const {
  const auto it = std::lower_bound(rows_.begin(), rows_.end(), canonical_word,
                                   [](const RefRow& r, const std::string& w) {
                                     return r.word < w;
                                   });
  return it != rows_.end() && it->word == canonical_word;
}

std::vector<std::string> PilotReference::Candidates(const std::string& raw_input,
                                                    std::size_t total_cap) const {
  std::vector<std::string> out{raw_input};
  std::string key;
  const RefShape shape = RefClassify(raw_input);
  if (!RefLookupKey(raw_input, &key)) {
    return out;  // mixed, non-alphabetic, over-long or empty: nothing is completed
  }
  struct Hit {
    std::uint32_t weight;
    std::string word;
  };
  std::vector<Hit> hits;
  for (const RefRow& r : rows_) {  // the full scan, on purpose
    if (r.word.size() >= key.size() &&
        std::equal(key.begin(), key.end(), r.word.begin())) {
      hits.push_back(Hit{r.weight, r.word});
    }
  }
  std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
    if (a.weight != b.weight) {
      return a.weight > b.weight;
    }
    return a.word < b.word;  // unsigned byte order of the canonical row
  });
  for (const Hit& hit : hits) {
    if (out.size() >= total_cap) {
      break;
    }
    const std::string rendered = RefRender(hit.word, shape);
    if (rendered == raw_input) {
      continue;  // dedup on the display text, so "Hello" does not offer "Hello" twice
    }
    out.push_back(rendered);
  }
  return out;
}

std::string PilotReference::Menu(const std::string& raw_input,
                                 std::size_t total_cap) const {
  return RefJoinMenu(Candidates(raw_input, total_cap));
}

}  // namespace recovery
}  // namespace epocanva
