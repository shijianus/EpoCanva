#include "morphology_rules.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "english_lexicon.h"

namespace epocanva {
namespace en {
namespace {

struct IrregularNoun {
  std::string_view lemma;
  std::string_view plural;
};
// Project-authored relationships; source/scope is declared in the header.
constexpr IrregularNoun kNouns[] = {
    {"child", "children"}, {"person", "people"}, {"man", "men"},
    {"woman", "women"}, {"mouse", "mice"}, {"goose", "geese"},
    {"tooth", "teeth"}, {"foot", "feet"}, {"ox", "oxen"},
    {"sheep", "sheep"}, {"deer", "deer"}, {"fish", "fish"},
    {"knife", "knives"}, {"life", "lives"}, {"wife", "wives"},
    {"leaf", "leaves"}, {"loaf", "loaves"}, {"wolf", "wolves"},
};
struct IrregularVerb {
  std::string_view lemma;
  std::string_view third;
  std::string_view past;  // alternatives separated by |, not copied as output
  std::string_view participle;
};
constexpr IrregularVerb kVerbs[] = {
    {"be", "is", "was|were", "been"},
    {"go", "goes", "went", "gone"},
    {"do", "does", "did", "done"},
    {"have", "has", "had", "had"},
    {"eat", "eats", "ate", "eaten"},
    {"drink", "drinks", "drank", "drunk"},
    {"begin", "begins", "began", "begun"},
    {"sing", "sings", "sang", "sung"},
    {"swim", "swims", "swam", "swum"},
    {"see", "sees", "saw", "seen"},
    {"take", "takes", "took", "taken"},
    {"give", "gives", "gave", "given"},
    {"write", "writes", "wrote", "written"},
    {"run", "runs", "ran", "run"},
    {"come", "comes", "came", "come"},
    {"make", "makes", "made", "made"},
    {"get", "gets", "got", "got|gotten"},
    {"say", "says", "said", "said"},
    {"think", "thinks", "thought", "thought"},
    {"buy", "buys", "bought", "bought"},
    {"bring", "brings", "brought", "brought"},
    {"catch", "catches", "caught", "caught"},
    {"teach", "teaches", "taught", "taught"},
    {"feel", "feels", "felt", "felt"},
    {"leave", "leaves", "left", "left"},
    {"keep", "keeps", "kept", "kept"},
    {"sleep", "sleeps", "slept", "slept"},
    {"put", "puts", "put", "put"},
    {"cut", "cuts", "cut", "cut"},
    {"read", "reads", "read", "read"},
    {"find", "finds", "found", "found"},
    {"meet", "meets", "met", "met"},
};

bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.substr(text.size() - suffix.size()) == suffix;
}
bool In(std::string_view word,
        std::initializer_list<std::string_view> values) {
  return std::find(values.begin(), values.end(), word) != values.end();
}
bool Vowel(char c) {
  return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}
bool LowerLetters(std::string_view word) {
  return !word.empty() && word.size() <= kMaxWordBytes &&
         std::all_of(word.begin(), word.end(),
                     [](char c) { return c >= 'a' && c <= 'z'; });
}
bool ConsonantY(std::string_view word) {
  return word.size() >= 2 && word.back() == 'y' &&
         !Vowel(word[word.size() - 2]);
}

// This is an orthographic restriction, not a syllable/stress estimator. In
// particular open/visit/offer must NOT become openned/visitted/offerred.
bool DoubleFinal(std::string_view word) {
  if (word.size() < 3) {
    return false;
  }
  const char last = word.back();
  if (Vowel(last) || last == 'w' || last == 'x' || last == 'y' ||
      !Vowel(word[word.size() - 2]) || Vowel(word[word.size() - 3])) {
    return false;
  }
  if (In(word, {"admit", "begin", "commit", "control", "defer", "infer",
                "occur", "omit", "permit", "prefer", "refer", "regret",
                "submit"})) {
    return true;
  }
  std::size_t vowel_groups = 0;  // groups of ASCII letters, not byte offsets
  bool in_group = false;
  for (char c : word) {
    const bool vowel = Vowel(c) || c == 'y';
    if (vowel && !in_group) {
      ++vowel_groups;
    }
    in_group = vowel;
  }
  return vowel_groups == 1;
}

std::string SForm(std::string_view lemma) {
  const std::string word(lemma);
  if (ConsonantY(lemma)) {
    return word.substr(0, word.size() - 1) + "ies";
  }
  if (lemma == "quiz") {
    return "quizzes";
  }
  if (EndsWith(lemma, "s") || EndsWith(lemma, "x") ||
      EndsWith(lemma, "z") || EndsWith(lemma, "ch") ||
      EndsWith(lemma, "sh")) {
    return word + "es";
  }
  return word + "s";
}
std::string PastForm(std::string_view lemma) {
  const std::string word(lemma);
  if (lemma.back() == 'e') {
    return word + "d";
  }
  if (ConsonantY(lemma)) {
    return word.substr(0, word.size() - 1) + "ied";
  }
  if (EndsWith(lemma, "ic")) {
    return word + "ked";
  }
  if (DoubleFinal(lemma)) {
    return word + lemma.back() + "ed";
  }
  return word + "ed";
}
std::string ProgressiveForm(std::string_view lemma) {
  const std::string word(lemma);
  if (EndsWith(lemma, "ie")) {
    return word.substr(0, word.size() - 2) + "ying";
  }
  if (lemma.back() == 'e' && !EndsWith(lemma, "ee") &&
      !EndsWith(lemma, "ye") && !EndsWith(lemma, "oe") &&
      !In(lemma, {"be", "singe", "tinge"})) {
    return word.substr(0, word.size() - 1) + "ing";
  }
  if (EndsWith(lemma, "ic")) {
    return word + "king";
  }
  if (DoubleFinal(lemma)) {
    return word + lemma.back() + "ing";
  }
  return word + "ing";
}

void Add(std::vector<GeneratedWordForm>* forms, std::string canonical,
         std::uint32_t mask, std::uint32_t requested) {
  mask &= requested;
  if (mask == 0 || canonical.empty() || canonical.size() > kMaxWordBytes) {
    return;
  }
  const auto existing = std::find_if(
      forms->begin(), forms->end(), [&](const GeneratedWordForm& form) {
        return form.canonical == canonical;
      });
  if (existing != forms->end()) {
    existing->forms |= mask;
  } else {
    forms->push_back({std::move(canonical), mask});
  }
}
void Alternatives(std::vector<GeneratedWordForm>* forms,
                  std::string_view alternatives, std::uint32_t mask,
                  std::uint32_t requested) {
  while (!alternatives.empty()) {
    const std::size_t delimiter_utf8_byte = alternatives.find('|');
    Add(forms, std::string(alternatives.substr(0, delimiter_utf8_byte)),
        mask, requested);
    if (delimiter_utf8_byte == std::string_view::npos) {
      break;
    }
    alternatives.remove_prefix(delimiter_utf8_byte + 1);
  }
}
bool IsAlternative(std::string_view word, std::string_view alternatives) {
  while (!alternatives.empty()) {
    const std::size_t delimiter_utf8_byte = alternatives.find('|');
    if (word == alternatives.substr(0, delimiter_utf8_byte)) {
      return true;
    }
    if (delimiter_utf8_byte == std::string_view::npos) {
      break;
    }
    alternatives.remove_prefix(delimiter_utf8_byte + 1);
  }
  return false;
}

}  // namespace

std::vector<GeneratedWordForm> GenerateWordForms(
    std::string_view lemma, std::uint32_t requested_forms) {
  std::vector<GeneratedWordForm> result;
  if (!LowerLetters(lemma) || lemma.size() < 2 || requested_forms == 0 ||
      In(lemma, {"an", "and", "as", "at", "but", "by", "can", "could",
                 "for", "from", "if", "in", "into", "may", "might",
                 "must", "not", "of", "on", "or", "our", "shall",
                 "should", "the", "their", "these", "this", "those",
                 "to", "upon", "us", "we", "will", "with", "would",
                 "you", "your", "he", "her", "him", "his", "it", "its",
                 "me", "my", "she", "that", "them", "they", "who"})) {
    return result;
  }
  const IrregularNoun* noun = nullptr;
  const IrregularVerb* verb = nullptr;
  for (const auto& row : kNouns) {
    if (row.lemma == lemma) {
      noun = &row;
      break;
    }
  }
  for (const auto& row : kVerbs) {
    if (row.lemma == lemma) {
      verb = &row;
      break;
    }
  }
  if (noun == nullptr && verb == nullptr) {
    // Do not reinflect known non-lemma rows (childrened, wenting, hased).
    // Homograph senses of these forms are outside this POS-free rule set.
    for (const auto& row : kNouns) {
      if (row.plural == lemma) {
        return result;
      }
    }
    for (const auto& row : kVerbs) {
      if (row.third == lemma || IsAlternative(lemma, row.past) ||
          IsAlternative(lemma, row.participle)) {
        return result;
      }
    }
    if (lemma == "am" || lemma == "are") {
      return result;
    }
  }

  if (verb == nullptr) {
    const std::string plural = noun ? std::string(noun->plural) : SForm(lemma);
    Add(&result, plural, kPlural, requested_forms);
    Add(&result, std::string(lemma) + "'s", kSingularPossessive,
        requested_forms);
    Add(&result, plural + (plural.back() == 's' ? "'" : "'s"),
        kPluralPossessive, requested_forms);
  }
  if (noun == nullptr) {
    Add(&result, verb ? std::string(verb->third) : SForm(lemma),
        kThirdPersonSingular, requested_forms);
    if (verb) {
      Alternatives(&result, verb->past, kPast, requested_forms);
      Alternatives(&result, verb->participle, kPastParticiple, requested_forms);
    } else {
      Add(&result, PastForm(lemma), kPast | kPastParticiple, requested_forms);
    }
    Add(&result, ProgressiveForm(lemma), kProgressive, requested_forms);
    if (lemma == "be") {
      Add(&result, "am", kIrregularPresent, requested_forms);
      Add(&result, "are", kIrregularPresent, requested_forms);
    }
  }
  std::sort(result.begin(), result.end(),
            [](const GeneratedWordForm& a, const GeneratedWordForm& b) {
              return a.canonical < b.canonical;
            });
  return result;
}

const char* MorphologyRuleSetVersion() { return "epocanva.en.morphology/v1"; }

}  // namespace en
}  // namespace epocanva
