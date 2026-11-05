// BCP 47 (RFC 5646) language tag identity for input profiles.
//
// Scope is deliberately narrower than the whole RFC: language + optional script
// + optional region + variants. Extension and private-use subtags are rejected
// rather than accepted-and-ignored, because an input profile must never claim a
// locale it cannot act on. All string lengths in this file are UTF-8 code units
// (bytes); no caret or text offset exists here, and no subtag is ever counted in
// UTF-16 units or grapheme clusters.
//
// This layer performs no I/O, no case folding beyond ASCII case, and no locale
// detection. It is pure syntax and identity.
#ifndef EPOCANVA_LOCALE_LANGUAGE_TAG_H_
#define EPOCANVA_LOCALE_LANGUAGE_TAG_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace epocanva {
namespace locale {

// R&D depth order fixed by the product spec. Lower is higher priority.
// Spanish and German are core-required languages even while their engine
// implementations are still absent; declaring them here is an identity and
// priority statement, never a claim that they can already produce candidates.
constexpr int kPriorityUnsupported = -1;
constexpr int kPriorityOtherDeclared = 7;
constexpr std::size_t kMaxTagBytes = 96;
constexpr std::size_t kMaxSubtags = 8;

struct LanguageTag {
  // Primary language subtag, lowercase ASCII, 2-3 or 5-8 letters.
  std::string language;
  // Titlecase 4-letter ISO 15924 script, empty when the tag omits it.
  std::string script;
  // Uppercase 2-letter or 3-digit region, empty when the tag omits it.
  std::string region;
  // Lowercase variant subtags in source order; never duplicated.
  std::vector<std::string> variants;
};

enum class TagError {
  kOk = 0,
  kEmpty,
  kTooLong,
  kTooManySubtags,
  kInvalidSubtagLength,
  kInvalidSubtagCharacter,
  kInvalidLanguage,
  kInvalidScript,
  kInvalidRegion,
  kInvalidVariant,
  kDuplicateVariant,
  kUnsupportedExtension,
  kUnsupportedPrivateUse,
};

const char* TagErrorName(TagError error);

// Accepts the private-use/extension forms so they can be reported as explicitly
// unsupported instead of being mistaken for malformed syntax.
TagError ParseLanguageTag(std::string_view text_utf8, LanguageTag* out);
// Canonical form ("zh-Hant-TW"). Always safe to call on a parsed tag.
std::string FormatLanguageTag(const LanguageTag& tag);
// A tag is usable when ParseLanguageTag succeeded and the language is nonempty.
bool IsUsableTag(const LanguageTag& tag);

// Default script for a language when the tag omits one. Returns empty when the
// language has no single defensible default (for example plain "zh", whose
// simplification/traditional split must be chosen by the user, not guessed).
std::string_view DefaultScript(std::string_view language);
// Right-to-left scripts require bidirectional care in the platform adapter.
bool IsRightToLeftScript(std::string_view script);

// RFC 5646 style lookup matching: the pattern matches when its subtags are a
// case-insensitive prefix of the candidate's canonical subtags. This is why
// "zh-Hant" matches "zh-Hant-TW" but "en-GB" does not match "en-US".
bool LookupMatches(const LanguageTag& pattern, const LanguageTag& candidate);

// en=0, zh=1, fr=2, es=3, ja=4, ko=5, de=6, other declared=7, unknown=-1.
int LanguagePriority(std::string_view language);
// True for exactly the seven core-required languages above.
bool IsCoreLanguage(std::string_view language);
// True for the wider first-batch coverage list. These languages have identity
// and priority but not necessarily an implemented input engine.
bool IsDeclaredLanguage(std::string_view language);

}  // namespace locale
}  // namespace epocanva
#endif  // EPOCANVA_LOCALE_LANGUAGE_TAG_H_
