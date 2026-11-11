#include "language_tag.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace epocanva {
namespace locale {
namespace {

bool IsAsciiAlpha(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
}
bool IsAsciiDigit(char ch) { return ch >= '0' && ch <= '9'; }
bool IsAsciiAlnum(char ch) { return IsAsciiAlpha(ch) || IsAsciiDigit(ch); }

char LowerAscii(char ch) {
  return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}
char UpperAscii(char ch) {
  return (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch;
}

std::string LowerAll(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char ch : text) out.push_back(LowerAscii(ch));
  return out;
}

std::vector<std::string_view> Split(std::string_view text) {
  std::vector<std::string_view> parts;
  std::size_t begin = 0;
  while (true) {
    const std::size_t dash = text.find('-', begin);
    if (dash == std::string_view::npos) {
      parts.push_back(text.substr(begin));
      return parts;
    }
    parts.push_back(text.substr(begin, dash - begin));
    begin = dash + 1;
  }
}

bool IsAllAlpha(std::string_view text) {
  for (char ch : text) {
    if (!IsAsciiAlpha(ch)) return false;
  }
  return true;
}

bool IsAllDigit(std::string_view text) {
  for (char ch : text) {
    if (!IsAsciiDigit(ch)) return false;
  }
  return true;
}

bool IsVariantSubtag(std::string_view text) {
  if (text.size() >= 5 && text.size() <= 8) {
    for (char ch : text) {
      if (!IsAsciiAlnum(ch)) return false;
    }
    return true;
  }
  // Four characters must start with a digit so the subtag cannot be confused
  // with a future script subtag.
  if (text.size() == 4 && IsAsciiDigit(text[0])) {
    for (char ch : text) {
      if (!IsAsciiAlnum(ch)) return false;
    }
    return true;
  }
  return false;
}

}  // namespace

const char* TagErrorName(TagError error) {
  switch (error) {
    case TagError::kOk: return "ok";
    case TagError::kEmpty: return "empty";
    case TagError::kTooLong: return "too_long";
    case TagError::kTooManySubtags: return "too_many_subtags";
    case TagError::kInvalidSubtagLength: return "invalid_subtag_length";
    case TagError::kInvalidSubtagCharacter: return "invalid_subtag_character";
    case TagError::kInvalidLanguage: return "invalid_language";
    case TagError::kInvalidScript: return "invalid_script";
    case TagError::kInvalidRegion: return "invalid_region";
    case TagError::kInvalidVariant: return "invalid_variant";
    case TagError::kDuplicateVariant: return "duplicate_variant";
    case TagError::kUnsupportedExtension: return "unsupported_extension";
    case TagError::kUnsupportedPrivateUse: return "unsupported_private_use";
  }
  return "invalid_language";
}

TagError ParseLanguageTag(std::string_view text_utf8, LanguageTag* out) {
  if (out == nullptr) return TagError::kEmpty;
  *out = LanguageTag{};
  if (text_utf8.empty()) return TagError::kEmpty;
  if (text_utf8.size() > kMaxTagBytes) return TagError::kTooLong;

  for (char ch : text_utf8) {
    if (static_cast<unsigned char>(ch) > 0x7f) {
      return TagError::kInvalidSubtagCharacter;
    }
  }

  const std::vector<std::string_view> parts = Split(text_utf8);
  if (parts.size() > kMaxSubtags) return TagError::kTooManySubtags;

  for (const std::string_view part : parts) {
    if (part.empty()) return TagError::kInvalidSubtagLength;
    if (part.size() > 8) return TagError::kInvalidSubtagLength;
    for (char ch : part) {
      if (!IsAsciiAlnum(ch)) return TagError::kInvalidSubtagCharacter;
    }
  }

  const std::string_view language = parts.front();
  if (language.size() == 1 && LowerAscii(language[0]) == 'x') {
    return TagError::kUnsupportedPrivateUse;
  }
  if (language.size() == 1) {
    // A single-letter primary subtag is reserved; it is not a language.
    return TagError::kInvalidSubtagLength;
  }
  if (!((language.size() == 2 || language.size() == 3) ||
        (language.size() >= 5 && language.size() <= 8))) {
    return TagError::kInvalidLanguage;
  }
  if (!IsAllAlpha(language)) return TagError::kInvalidLanguage;
  out->language = LowerAll(language);

  std::size_t index = 1;
  if (index < parts.size() && parts[index].size() == 4) {
    if (!IsAllAlpha(parts[index])) return TagError::kInvalidScript;
    const std::string_view script = parts[index];
    out->script.reserve(4);
    out->script.push_back(UpperAscii(script[0]));
    for (std::size_t i = 1; i < script.size(); ++i) {
      out->script.push_back(LowerAscii(script[i]));
    }
    ++index;
  }

  if (index < parts.size()) {
    const std::string_view region = parts[index];
    if ((region.size() == 2 && IsAllAlpha(region)) ||
        (region.size() == 3 && IsAllDigit(region))) {
      for (char ch : region) out->region.push_back(UpperAscii(ch));
      ++index;
    } else if (region.size() == 2 || region.size() == 3) {
      // No variant subtag is two or three characters long, so a short subtag in
      // this position is a malformed region rather than a malformed variant.
      return TagError::kInvalidRegion;
    }
  }

  for (; index < parts.size(); ++index) {
    const std::string_view variant = parts[index];
    if (variant.size() == 1) return TagError::kUnsupportedExtension;
    if (!IsVariantSubtag(variant)) return TagError::kInvalidVariant;
    const std::string lowered = LowerAll(variant);
    if (std::find(out->variants.begin(), out->variants.end(), lowered) !=
        out->variants.end()) {
      return TagError::kDuplicateVariant;
    }
    out->variants.push_back(lowered);
  }
  return TagError::kOk;
}

std::string FormatLanguageTag(const LanguageTag& tag) {
  std::string out = tag.language;
  if (!tag.script.empty()) {
    if (!out.empty()) out.push_back('-');
    out += tag.script;
  }
  if (!tag.region.empty()) {
    if (!out.empty()) out.push_back('-');
    out += tag.region;
  }
  for (const std::string& variant : tag.variants) {
    if (!out.empty()) out.push_back('-');
    out += variant;
  }
  return out;
}

bool IsUsableTag(const LanguageTag& tag) { return !tag.language.empty(); }

std::string_view DefaultScript(std::string_view language) {
  if (language == "en" || language == "fr" || language == "es" || language == "de" ||
      language == "pt" || language == "it" || language == "nl" || language == "sv" ||
      language == "pl" || language == "cs" || language == "ro" || language == "tr" ||
      language == "id" || language == "vi" || language == "sw") {
    return "Latn";
  }
  if (language == "hi" || language == "bn") return "Deva";
  if (language == "ru" || language == "uk") return "Cyrl";
  if (language == "el") return "Grek";
  if (language == "ar" || language == "fa" || language == "ur") return "Arab";
  if (language == "he") return "Hebr";
  if (language == "ja") return "Jpan";
  if (language == "ko") return "Kore";
  if (language == "th") return "Thai";
  if (language == "yue") return "Hant";
  // Plain "zh" is intentionally left without a default: Simplified vs
  // Traditional is a user choice, and guessing it would silently misrepresent
  // regional variants.
  return {};
}

bool IsRightToLeftScript(std::string_view script) {
  return script == "Arab" || script == "Hebr" || script == "Thaa" ||
         script == "Nkoo" || script == "Adlm" || script == "Syrc";
}

bool LookupMatches(const LanguageTag& pattern, const LanguageTag& candidate) {
  if (pattern.language.empty() || candidate.language.empty()) return false;
  if (pattern.language != candidate.language) return false;
  if (!pattern.script.empty() && pattern.script != candidate.script) return false;
  if (!pattern.region.empty() && pattern.region != candidate.region) return false;
  if (!pattern.variants.empty() && pattern.variants != candidate.variants) {
    return false;
  }
  return true;
}

int LanguagePriority(std::string_view language) {
  static constexpr std::array<std::string_view, 7> kCore = {
      "en", "zh", "fr", "es", "ja", "ko", "de"};
  for (std::size_t i = 0; i < kCore.size(); ++i) {
    if (language == kCore[i]) return static_cast<int>(i);
  }
  return IsDeclaredLanguage(language) ? kPriorityOtherDeclared : kPriorityUnsupported;
}

bool IsCoreLanguage(std::string_view language) {
  return LanguagePriority(language) >= 0 &&
         LanguagePriority(language) < kPriorityOtherDeclared;
}

bool IsDeclaredLanguage(std::string_view language) {
  // First-batch coverage list from the product spec. Identity only: presence
  // here never implies an implemented engine, data file or verified quality.
  static constexpr std::array<std::string_view, 28> kDeclared = {
      "en", "zh", "fr", "es", "ja", "ko", "de", "pt", "it", "ru", "ar", "hi",
      "id", "vi", "th", "tr", "nl", "pl", "uk", "bn", "fa", "ur", "he", "el",
      "sv", "cs", "ro", "sw"};
  return std::find(kDeclared.begin(), kDeclared.end(), language) != kDeclared.end();
}

}  // namespace locale
}  // namespace epocanva
