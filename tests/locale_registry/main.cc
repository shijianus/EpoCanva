// Locale identity and profile admission tests.
//
// Every check here can fail: the --forcefail case proves the harness reports a
// red run, and each assertion encodes a behaviour the implementation must have
// rather than a property it trivially satisfies.
#include "backend_abi_values.h"
#include "backend_manifest.h"
#include "epocanva_backend.h"
#include "language_tag.h"
#include "locale_profile.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

namespace loc = epocanva::locale;

int failed = 0;

void Check(bool value, const char* label) {
  if (!value) {
    ++failed;
    std::cerr << "ASSERT FAIL " << label << '\n';
  }
}

loc::LanguageTag Parsed(std::string_view text) {
  loc::LanguageTag tag;
  loc::ParseLanguageTag(text, &tag);
  return tag;
}

loc::LocaleRegistry BuiltinRegistry() {
  loc::LocaleRegistry registry;
  for (auto& profile : loc::BuiltinProfiles()) {
    const loc::RegisterResult result = registry.Register(std::move(profile));
    if (!result.ok()) {
      ++failed;
      std::cerr << "ASSERT FAIL builtin profile registration: "
                << loc::RegistryErrorName(result.error) << '\n';
    }
  }
  return registry;
}

void ParseTests() {
  // Canonicalisation is case-insensitive in, canonical out.
  loc::LanguageTag tag;
  Check(loc::ParseLanguageTag("zh-hant-tw", &tag) == loc::TagError::kOk,
        "lowercase bilingual tag parses");
  Check(tag.language == "zh" && tag.script == "Hant" && tag.region == "TW",
        "tag canonicalises script and region case");
  Check(loc::FormatLanguageTag(tag) == "zh-Hant-TW", "canonical format is stable");

  Check(loc::ParseLanguageTag("EN-us", &tag) == loc::TagError::kOk, "mixed case parses");
  Check(loc::FormatLanguageTag(tag) == "en-US", "mixed case canonicalises");

  Check(loc::ParseLanguageTag("yue-Hant-HK", &tag) == loc::TagError::kOk,
        "three-letter language parses");
  Check(tag.language == "yue", "yue is preserved as a language, not folded to zh");

  Check(loc::ParseLanguageTag("es-419", &tag) == loc::TagError::kOk,
        "numeric region parses");
  Check(tag.region == "419", "numeric region is uppercase-preserved");

  Check(loc::ParseLanguageTag("de-DE-1901", &tag) == loc::TagError::kOk,
        "digit-initial variant parses");
  Check(tag.variants.size() == 1 && tag.variants[0] == "1901",
        "variant subtag retained");

  // Rejections. None of these may silently degrade to a valid tag.
  struct Reject {
    const char* text;
    loc::TagError error;
    const char* label;
  };
  const Reject rejects[] = {
      {"", loc::TagError::kEmpty, "empty tag rejected"},
      {"e", loc::TagError::kInvalidSubtagLength, "single-letter language rejected"},
      {"en--US", loc::TagError::kInvalidSubtagLength, "empty subtag rejected"},
      {"en_US", loc::TagError::kInvalidSubtagCharacter, "underscore rejected"},
      {"engl-US", loc::TagError::kInvalidLanguage, "four-letter primary rejected"},
      {"en-USA", loc::TagError::kInvalidRegion, "three-letter region rejected"},
      {"x-private", loc::TagError::kUnsupportedPrivateUse, "private use is explicit"},
      {"en-a-bbb", loc::TagError::kUnsupportedExtension, "extension is explicit"},
      {"de-DE-1901-1901", loc::TagError::kDuplicateVariant, "duplicate variant rejected"},
  };
  for (const Reject& item : rejects) {
    const std::string text(item.text);
    loc::LanguageTag rejected;
    const loc::TagError error = loc::ParseLanguageTag(text, &rejected);
    Check(error == item.error, item.label);
  }

  // Embedded NUL must be rejected explicitly rather than truncating the tag.
  const std::string with_nul("zh-Hant\0TW", 10);
  loc::LanguageTag nul_tag;
  Check(loc::ParseLanguageTag(with_nul, &nul_tag) == loc::TagError::kInvalidSubtagCharacter,
        "embedded NUL rejected");

  // Overlong input is rejected before any subtag work.
  const std::string overlong(200, 'a');
  loc::LanguageTag long_tag;
  Check(loc::ParseLanguageTag(overlong, &long_tag) == loc::TagError::kTooLong,
        "overlong tag rejected");

  // Default script must not guess Simplified for plain "zh".
  Check(loc::DefaultScript("zh").empty(), "plain zh has no default script");
  Check(loc::DefaultScript("en") == "Latn", "en defaults to Latin");
  Check(loc::DefaultScript("yue") == "Hant", "yue defaults to Traditional");
  Check(loc::DefaultScript("ar") == "Arab", "ar defaults to Arabic");
  Check(loc::IsRightToLeftScript("Arab") && loc::IsRightToLeftScript("Hebr"),
        "RTL scripts identified");
  Check(!loc::IsRightToLeftScript("Latn"), "Latin is not RTL");
}

void MatchingTests() {
  const loc::LanguageTag pattern = Parsed("zh-Hant");
  const loc::LanguageTag traditional = Parsed("zh-Hant-TW");
  const loc::LanguageTag simplified = Parsed("zh-Hans-CN");
  Check(loc::LookupMatches(pattern, traditional), "zh-Hant matches zh-Hant-TW");
  Check(!loc::LookupMatches(pattern, simplified), "zh-Hant does not match zh-Hans-CN");

  const loc::LanguageTag en = Parsed("en");
  Check(loc::LookupMatches(en, Parsed("en-US")) && loc::LookupMatches(en, Parsed("en-GB")),
        "en matches both English profiles");
  Check(!loc::LookupMatches(Parsed("en-GB"), Parsed("en-US")),
        "en-GB does not match en-US");
  Check(!loc::LookupMatches(Parsed("zh"), Parsed("yue-Hant-HK")),
        "zh does not match yue");
}

void PriorityTests() {
  // Fixed product order: en > zh > fr > es > ja > ko > de.
  Check(loc::LanguagePriority("en") == 0, "en highest priority");
  Check(loc::LanguagePriority("zh") == 1, "zh second");
  Check(loc::LanguagePriority("fr") == 2, "fr third");
  Check(loc::LanguagePriority("es") == 3, "es fourth");
  Check(loc::LanguagePriority("ja") == 4, "ja fifth");
  Check(loc::LanguagePriority("ko") == 5, "ko sixth");
  Check(loc::LanguagePriority("de") == 6, "de seventh");
  Check(loc::LanguagePriority("pt") == loc::kPriorityOtherDeclared,
        "declared non-core language is supported but lower priority");
  Check(loc::LanguagePriority("zz") == loc::kPriorityUnsupported,
        "unknown language is unsupported, not silently accepted");
  Check(loc::IsCoreLanguage("es") && loc::IsCoreLanguage("de"),
        "Spanish and German are core-required");
  Check(!loc::IsCoreLanguage("pt"), "Portuguese is declared but not core");
  Check(loc::IsDeclaredLanguage("ar") && loc::IsDeclaredLanguage("th"),
        "first-batch coverage includes ar and th");
}

void BuiltinResolutionTests() {
  const loc::LocaleRegistry registry = BuiltinRegistry();
  Check(registry.size() == 8, "builtin catalogue has eight profiles");

  auto resolve = [&](const char* tag, const char* scheme) {
    loc::ResolveRequest request;
    request.pattern = Parsed(tag);
    request.scheme_id = scheme;
    return registry.Resolve(request);
  };

  Check(resolve("en-US", "latin").status == loc::ResolutionStatus::kResolved,
        "en-US latin resolves");
  Check(resolve("fr-FR", "latin").status == loc::ResolutionStatus::kResolved,
        "fr-FR latin resolves");
  Check(resolve("ja-JP", "romaji").status == loc::ResolutionStatus::kResolved,
        "ja-JP romaji resolves");
  Check(resolve("ko-KR", "two_set").status == loc::ResolutionStatus::kResolved,
        "ko-KR two-set resolves");
  Check(resolve("zh-Hans-CN", "pinyin").status == loc::ResolutionStatus::kResolved,
        "zh-Hans-CN pinyin resolves");

  // Plain "zh" must be ambiguous, not silently Simplified.
  const loc::Resolution plain_zh = resolve("zh", "pinyin");
  Check(plain_zh.status == loc::ResolutionStatus::kAmbiguous,
        "plain zh is ambiguous across script/region profiles");
  Check(plain_zh.match_count == 3, "three Chinese profiles match plain zh");

  Check(resolve("zh-Hans-CN", "double_pinyin").status ==
            loc::ResolutionStatus::kSchemeAbsent,
        "double pinyin is absent, not faked");
  Check(resolve("zh-Hans-CN", "zhuyin").status ==
            loc::ResolutionStatus::kSchemeNotVerified,
        "zhuyin on Simplified is unverified, not offered");
  Check(resolve("zh-Hant-TW", "zhuyin").status ==
            loc::ResolutionStatus::kSchemeNotVerified,
        "traditional zhuyin is unverified");
  Check(resolve("yue-Hant-HK", "jyutping").status ==
            loc::ResolutionStatus::kSchemeAbsent,
        "jyutping is absent");
  Check(resolve("zh-Hant-HK", "cangjie").status == loc::ResolutionStatus::kSchemeAbsent,
        "cangjie is absent");

  // Spanish and German are core-required but have no engine yet. The registry
  // must say so instead of returning a name-only profile.
  Check(resolve("es-ES", "latin").status == loc::ResolutionStatus::kNoProfile,
        "Spanish has no profile yet");
  Check(resolve("de-DE", "latin").status == loc::ResolutionStatus::kNoProfile,
        "German has no profile yet");

  Check(resolve("en-US", "nonexistent").status == loc::ResolutionStatus::kSchemeAbsent,
        "unknown scheme reports absent");

  loc::ResolveRequest bad;
  bad.pattern = Parsed("EN");
  bad.scheme_id = "Bad Scheme";
  Check(registry.Resolve(bad).status == loc::ResolutionStatus::kInvalidRequest,
        "malformed request rejected");

  loc::LocaleRegistry empty;
  loc::ResolveRequest any;
  any.pattern = Parsed("en-US");
  any.scheme_id = "latin";
  Check(empty.Resolve(any).status == loc::ResolutionStatus::kRegistryEmpty,
        "empty registry reports empty");
}

void AdmissionTests() {
  loc::LocaleRegistry registry;

  loc::LocaleProfile implemented_claim;
  implemented_claim.profile_id = "xx-latin";
  implemented_claim.tag = Parsed("xx");
  implemented_claim.display_label_utf8 = "Test";
  implemented_claim.schemes = {{"latin", loc::SchemeImplementation::kImplemented, ""}};
  Check(registry.Register(implemented_claim).error ==
            loc::RegistryError::kImplementedWithoutEvidence,
        "implemented claim requires evidence");

  implemented_claim.schemes[0].evidence_ref = "../escape";
  Check(registry.Register(implemented_claim).error ==
            loc::RegistryError::kImplementedWithoutEvidence,
        "evidence path traversal rejected");

  implemented_claim.schemes[0].evidence_ref = "engine/locale";
  Check(registry.Register(implemented_claim).ok(), "substantiated profile registers");
  Check(registry.size() == 1, "registry holds one profile");

  loc::LocaleProfile duplicate = implemented_claim;
  duplicate.profile_id = "yy-latin";
  Check(registry.Register(duplicate).error == loc::RegistryError::kDuplicateTag,
        "duplicate tag rejected even with a new id");

  loc::LocaleProfile duplicate_id = implemented_claim;
  duplicate_id.tag = Parsed("yy");
  Check(registry.Register(duplicate_id).error == loc::RegistryError::kDuplicateProfileId,
        "duplicate profile id rejected");

  loc::LocaleProfile absent_with_evidence;
  absent_with_evidence.profile_id = "zz-latin";
  absent_with_evidence.tag = Parsed("zz");
  absent_with_evidence.display_label_utf8 = "Test";
  absent_with_evidence.schemes = {{"latin", loc::SchemeImplementation::kAbsent,
                                  "engine/locale"}};
  Check(registry.Register(absent_with_evidence).error ==
            loc::RegistryError::kEvidenceOnAbsentScheme,
        "absent scheme cannot carry evidence");

  loc::LocaleProfile no_schemes = absent_with_evidence;
  no_schemes.profile_id = "za-latin";
  no_schemes.tag = Parsed("za");
  no_schemes.schemes[0].evidence_ref.clear();
  no_schemes.schemes.clear();
  Check(registry.Register(no_schemes).error == loc::RegistryError::kEmptySchemes,
        "profile without schemes rejected");

  loc::LocaleProfile bad_label = implemented_claim;
  bad_label.profile_id = "yz-latin";
  bad_label.tag = Parsed("yz");
  bad_label.display_label_utf8 = "\xED\xA0\x80";
  Check(registry.Register(bad_label).error == loc::RegistryError::kInvalidDisplayLabel,
        "surrogate display label rejected");

  loc::LocaleProfile bad_id = implemented_claim;
  bad_id.profile_id = "../evil";
  Check(registry.Register(bad_id).error == loc::RegistryError::kInvalidProfileId,
        "profile id traversal rejected");

  loc::LocaleProfile duplicate_scheme = implemented_claim;
  duplicate_scheme.profile_id = "yq-latin";
  duplicate_scheme.tag = Parsed("yq");
  duplicate_scheme.schemes.push_back(duplicate_scheme.schemes[0]);
  Check(registry.Register(duplicate_scheme).error ==
            loc::RegistryError::kDuplicateSchemeId,
        "duplicate scheme rejected");
}

struct ExpectedBinding {
  std::uint32_t id;
  const char* locale_profile;
  const char* language_tag;
  const char* language_enum;
  std::uint32_t language;
  const char* input_enum;
  std::uint32_t input;
  const char* output_enum;
  std::uint32_t output;
  const char* keyboard_enum;
  std::uint32_t keyboard;
  const char* variant;
  const char* scheme;
  const char* implementation;
};

// Full-field oracle for the 13 stable backend profile ids. This is a test
// oracle, not a second production catalogue: every value is compared against
// BackendManifestRows(), which is derived from BuiltinProfiles().
constexpr std::array<ExpectedBinding, 13> kExpectedBindings{{
    {1, "en-US-latin", "en", "EPOCANVA_BACKEND_EN", 1, "EPOCANVA_BACKEND_LATIN", 1,
     "EPOCANVA_BACKEND_OUTPUT_LATIN", 1, "EPOCANVA_BACKEND_KEYBOARD_US_QWERTY", 1,
     "en-US", "latin", "implemented"},
    {2, "zh-Hans-CN-pinyin", "zh", "EPOCANVA_BACKEND_ZH", 2, "EPOCANVA_BACKEND_PINYIN", 2,
     "EPOCANVA_BACKEND_OUTPUT_HANZI", 2, "EPOCANVA_BACKEND_KEYBOARD_US_QWERTY", 1,
     "zh-pinyin", "pinyin", "implemented"},
    {3, "fr-FR-latin", "fr", "EPOCANVA_BACKEND_FR", 3, "EPOCANVA_BACKEND_LATIN", 1,
     "EPOCANVA_BACKEND_OUTPUT_LATIN", 1, "EPOCANVA_BACKEND_KEYBOARD_FRENCH_AZERTY", 2,
     "fr-FR", "latin", "implemented"},
    {4, "ja-JP-romaji", "ja", "EPOCANVA_BACKEND_JA", 4, "EPOCANVA_BACKEND_ROMAJI", 4,
     "EPOCANVA_BACKEND_OUTPUT_KANJI", 5, "EPOCANVA_BACKEND_KEYBOARD_US_QWERTY", 1,
     "ja-JP", "romaji", "implemented"},
    {5, "ko-KR-two-set", "ko", "EPOCANVA_BACKEND_KO", 5, "EPOCANVA_BACKEND_TWO_SET", 6,
     "EPOCANVA_BACKEND_OUTPUT_HANGUL", 6, "EPOCANVA_BACKEND_KEYBOARD_US_QWERTY", 1,
     "ko-KR", "two_set", "implemented"},
    {6, "zh-Hans-CN-pinyin", "zh", "EPOCANVA_BACKEND_ZH", 2, "EPOCANVA_BACKEND_ZHUYIN", 3,
     "EPOCANVA_BACKEND_OUTPUT_HANZI", 2, "EPOCANVA_BACKEND_KEYBOARD_US_QWERTY", 1,
     "zh-zhuyin", "zhuyin", "unverified"},
    {7, "ja-JP-romaji", "ja", "EPOCANVA_BACKEND_JA", 4, "EPOCANVA_BACKEND_ROMAJI", 4,
     "EPOCANVA_BACKEND_OUTPUT_HIRAGANA", 3, "EPOCANVA_BACKEND_KEYBOARD_US_QWERTY", 1,
     "ja-JP", "romaji", "implemented"},
    {8, "ja-JP-romaji", "ja", "EPOCANVA_BACKEND_JA", 4, "EPOCANVA_BACKEND_ROMAJI", 4,
     "EPOCANVA_BACKEND_OUTPUT_KATAKANA", 4, "EPOCANVA_BACKEND_KEYBOARD_US_QWERTY", 1,
     "ja-JP", "romaji", "implemented"},
    {9, "ja-JP-romaji", "ja", "EPOCANVA_BACKEND_JA", 4, "EPOCANVA_BACKEND_KANA", 7,
     "EPOCANVA_BACKEND_OUTPUT_KANJI", 5, "EPOCANVA_BACKEND_KEYBOARD_US_QWERTY", 1,
     "ja-JP", "kana", "implemented"},
    {10, "ko-KR-two-set", "ko", "EPOCANVA_BACKEND_KO", 5, "EPOCANVA_BACKEND_LATIN", 1,
     "EPOCANVA_BACKEND_OUTPUT_HANGUL", 6, "EPOCANVA_BACKEND_KEYBOARD_US_QWERTY", 1,
     "ko-KR", "two_set", "implemented"},
    {11, "en-US-latin", "en", "EPOCANVA_BACKEND_EN", 1, "EPOCANVA_BACKEND_LATIN", 1,
     "EPOCANVA_BACKEND_OUTPUT_LATIN", 1, "EPOCANVA_BACKEND_KEYBOARD_GERMAN_QWERTZ", 3,
     "en-US", "latin", "implemented"},
    {12, "en-US-latin", "en", "EPOCANVA_BACKEND_EN", 1, "EPOCANVA_BACKEND_LATIN", 1,
     "EPOCANVA_BACKEND_OUTPUT_LATIN", 1, "EPOCANVA_BACKEND_KEYBOARD_DVORAK", 4,
     "en-US", "latin", "implemented"},
    {13, "en-US-latin", "en", "EPOCANVA_BACKEND_EN", 1, "EPOCANVA_BACKEND_LATIN", 1,
     "EPOCANVA_BACKEND_OUTPUT_LATIN", 1, "EPOCANVA_BACKEND_KEYBOARD_FRENCH_AZERTY", 2,
     "en-US", "latin", "implemented"},
}};

std::uint32_t AbiValue(const std::string& name) {
  if (name == "EPOCANVA_BACKEND_EN") return EPOCANVA_BACKEND_EN;
  if (name == "EPOCANVA_BACKEND_ZH") return EPOCANVA_BACKEND_ZH;
  if (name == "EPOCANVA_BACKEND_FR") return EPOCANVA_BACKEND_FR;
  if (name == "EPOCANVA_BACKEND_JA") return EPOCANVA_BACKEND_JA;
  if (name == "EPOCANVA_BACKEND_KO") return EPOCANVA_BACKEND_KO;
  if (name == "EPOCANVA_BACKEND_LATIN") return EPOCANVA_BACKEND_LATIN;
  if (name == "EPOCANVA_BACKEND_PINYIN") return EPOCANVA_BACKEND_PINYIN;
  if (name == "EPOCANVA_BACKEND_ZHUYIN") return EPOCANVA_BACKEND_ZHUYIN;
  if (name == "EPOCANVA_BACKEND_ROMAJI") return EPOCANVA_BACKEND_ROMAJI;
  if (name == "EPOCANVA_BACKEND_KATAKANA") return EPOCANVA_BACKEND_KATAKANA;
  if (name == "EPOCANVA_BACKEND_TWO_SET") return EPOCANVA_BACKEND_TWO_SET;
  if (name == "EPOCANVA_BACKEND_KANA") return EPOCANVA_BACKEND_KANA;
  if (name == "EPOCANVA_BACKEND_OUTPUT_LATIN") return EPOCANVA_BACKEND_OUTPUT_LATIN;
  if (name == "EPOCANVA_BACKEND_OUTPUT_HANZI") return EPOCANVA_BACKEND_OUTPUT_HANZI;
  if (name == "EPOCANVA_BACKEND_OUTPUT_HIRAGANA") return EPOCANVA_BACKEND_OUTPUT_HIRAGANA;
  if (name == "EPOCANVA_BACKEND_OUTPUT_KATAKANA") return EPOCANVA_BACKEND_OUTPUT_KATAKANA;
  if (name == "EPOCANVA_BACKEND_OUTPUT_KANJI") return EPOCANVA_BACKEND_OUTPUT_KANJI;
  if (name == "EPOCANVA_BACKEND_OUTPUT_HANGUL") return EPOCANVA_BACKEND_OUTPUT_HANGUL;
  if (name == "EPOCANVA_BACKEND_KEYBOARD_NEUTRAL") return EPOCANVA_BACKEND_KEYBOARD_NEUTRAL;
  if (name == "EPOCANVA_BACKEND_KEYBOARD_US_QWERTY") return EPOCANVA_BACKEND_KEYBOARD_US_QWERTY;
  if (name == "EPOCANVA_BACKEND_KEYBOARD_FRENCH_AZERTY") return EPOCANVA_BACKEND_KEYBOARD_FRENCH_AZERTY;
  if (name == "EPOCANVA_BACKEND_KEYBOARD_GERMAN_QWERTZ") return EPOCANVA_BACKEND_KEYBOARD_GERMAN_QWERTZ;
  if (name == "EPOCANVA_BACKEND_KEYBOARD_DVORAK") return EPOCANVA_BACKEND_KEYBOARD_DVORAK;
  return 0;
}

void BackendBindingTests() {
  // The locale layer's copied ABI values must equal the public header exactly.
  namespace abi = loc::abi_values;
  Check(abi::kLanguageEn == EPOCANVA_BACKEND_EN, "locale EN equals ABI EN");
  Check(abi::kLanguageZh == EPOCANVA_BACKEND_ZH, "locale ZH equals ABI ZH");
  Check(abi::kLanguageFr == EPOCANVA_BACKEND_FR, "locale FR equals ABI FR");
  Check(abi::kLanguageJa == EPOCANVA_BACKEND_JA, "locale JA equals ABI JA");
  Check(abi::kLanguageKo == EPOCANVA_BACKEND_KO, "locale KO equals ABI KO");
  Check(abi::kInputLatin == EPOCANVA_BACKEND_LATIN, "locale LATIN input equals ABI");
  Check(abi::kInputPinyin == EPOCANVA_BACKEND_PINYIN, "locale PINYIN input equals ABI");
  Check(abi::kInputZhuyin == EPOCANVA_BACKEND_ZHUYIN, "locale ZHUYIN input equals ABI");
  Check(abi::kInputRomaji == EPOCANVA_BACKEND_ROMAJI, "locale ROMAJI input equals ABI");
  Check(abi::kInputKatakana == EPOCANVA_BACKEND_KATAKANA, "locale KATAKANA input equals ABI");
  Check(abi::kInputTwoSet == EPOCANVA_BACKEND_TWO_SET, "locale TWO_SET input equals ABI");
  Check(abi::kInputKana == EPOCANVA_BACKEND_KANA, "locale KANA input equals ABI");
  Check(abi::kOutputLatin == EPOCANVA_BACKEND_OUTPUT_LATIN, "locale LATIN output equals ABI");
  Check(abi::kOutputHanzi == EPOCANVA_BACKEND_OUTPUT_HANZI, "locale HANZI output equals ABI");
  Check(abi::kOutputHiragana == EPOCANVA_BACKEND_OUTPUT_HIRAGANA, "locale HIRAGANA output equals ABI");
  Check(abi::kOutputKatakana == EPOCANVA_BACKEND_OUTPUT_KATAKANA, "locale KATAKANA output equals ABI");
  Check(abi::kOutputKanji == EPOCANVA_BACKEND_OUTPUT_KANJI, "locale KANJI output equals ABI");
  Check(abi::kOutputHangul == EPOCANVA_BACKEND_OUTPUT_HANGUL, "locale HANGUL output equals ABI");
  Check(abi::kKeyboardNeutral == EPOCANVA_BACKEND_KEYBOARD_NEUTRAL, "locale NEUTRAL keyboard equals ABI");
  Check(abi::kKeyboardUsQwerty == EPOCANVA_BACKEND_KEYBOARD_US_QWERTY, "locale US_QWERTY keyboard equals ABI");
  Check(abi::kKeyboardFrenchAzerty == EPOCANVA_BACKEND_KEYBOARD_FRENCH_AZERTY,
        "locale FRENCH_AZERTY keyboard equals ABI");
  Check(abi::kKeyboardGermanQwertz == EPOCANVA_BACKEND_KEYBOARD_GERMAN_QWERTZ,
        "locale GERMAN_QWERTZ keyboard equals ABI");
  Check(abi::kKeyboardDvorak == EPOCANVA_BACKEND_KEYBOARD_DVORAK, "locale DVORAK keyboard equals ABI");

  const auto rows = loc::BackendManifestRows();
  const auto profiles = loc::BuiltinProfiles();
  Check(rows.size() == kExpectedBindings.size(), "manifest exports exactly thirteen bindings");
  Check(loc::ValidateBackendManifestRows(rows, profiles) == loc::BackendManifestError::kOk,
        "builtin manifest rows validate against their profiles");

  for (std::size_t ordinal = 0; ordinal < rows.size() && ordinal < kExpectedBindings.size();
       ++ordinal) {
    const auto& row = rows[ordinal];
    const auto& expected = kExpectedBindings[ordinal];
    Check(row.numeric_id == expected.id, "manifest numeric id is stable");
    Check(row.locale_profile_id == expected.locale_profile, "manifest owning profile is exact");
    Check(row.language_tag == expected.language_tag, "manifest language tag is exact");
    Check(row.language_enum == expected.language_enum, "manifest language enum name is exact");
    Check(row.language == expected.language, "manifest language value is exact");
    Check(row.input_mode_enum == expected.input_enum, "manifest input enum name is exact");
    Check(row.input_mode == expected.input, "manifest input value is exact");
    Check(row.output_mode_enum == expected.output_enum, "manifest output enum name is exact");
    Check(row.output_mode == expected.output, "manifest output value is exact");
    Check(row.keyboard_layout_enum == expected.keyboard_enum, "manifest keyboard enum name is exact");
    Check(row.keyboard_layout == expected.keyboard, "manifest keyboard value is exact");
    Check(row.variant_id == expected.variant, "manifest variant is exact");
    Check(row.scheme_id == expected.scheme, "manifest scheme is exact");
    Check(row.scheme_implementation == expected.implementation,
          "manifest scheme implementation state is exact");
    // The manifest's own symbolic name must resolve to the ABI constant and to
    // the exported value. A changed header enum fails here even when the locale
    // numeric copy was changed to match.
    Check(AbiValue(row.language_enum) == row.language, "language enum name matches ABI header");
    Check(AbiValue(row.input_mode_enum) == row.input_mode, "input enum name matches ABI header");
    Check(AbiValue(row.output_mode_enum) == row.output_mode, "output enum name matches ABI header");
    Check(AbiValue(row.keyboard_layout_enum) == row.keyboard_layout,
          "keyboard enum name matches ABI header");
  }

  // AllBackendBindings() and the exported manifest must describe the same set.
  const auto bindings = loc::AllBackendBindings();
  Check(bindings.size() == rows.size(), "binding catalogue and manifest have the same size");
  for (const auto& binding : bindings) {
    const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
      return row.numeric_id == binding.numeric_id;
    });
    Check(found != rows.end(), "every catalogue binding is exported");
    if (found == rows.end()) continue;
    Check(found->language == binding.language && found->input_mode == binding.input_mode &&
              found->output_mode == binding.output_mode &&
              found->keyboard_layout == binding.keyboard_layout &&
              found->variant_id == binding.variant_id && found->scheme_id == binding.scheme_id,
          "catalogue binding fields equal the exported row");
  }

  // Unverified schemes stay unverified even though a binding and staging row
  // exist. A resource file is not implementation evidence.
  const auto zhuyin = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
    return row.numeric_id == 6;
  });
  Check(zhuyin != rows.end() && zhuyin->scheme_implementation == "unverified",
        "zhuyin binding is exported as unverified");
  Check(zhuyin != rows.end() && zhuyin->scheme_id == "zhuyin", "zhuyin binding names its scheme");

  // Same language under different keyboard layouts or output modes is never
  // collapsed into one configuration.
  Check(rows[0].keyboard_layout != rows[10].keyboard_layout &&
            rows[0].keyboard_layout != rows[11].keyboard_layout &&
            rows[0].keyboard_layout != rows[12].keyboard_layout,
        "English keyboard layouts remain distinct");
  Check(rows[6].output_mode != rows[7].output_mode &&
            rows[6].input_mode != rows[8].input_mode,
        "Japanese output and input modes remain distinct");
  Check(rows[4].input_mode != rows[9].input_mode,
        "Korean two-set and Latin input remain distinct");

  // Deterministic export: the same catalogue always serializes to the same bytes.
  const std::string first = loc::BackendManifestJson();
  const std::string second = loc::BackendManifestJson();
  Check(!first.empty() && first == second, "manifest export is deterministic");
  Check(first.find("\"owner\": \"epocanva-locale-backend-bindings-v1\"") != std::string::npos,
        "manifest carries the owner tag");
  Check(first.find("\"numeric_id\": 13") != std::string::npos, "manifest contains the last stable id");

  // Negative validation: every mutation below must be rejected.
  auto validate = [&](std::vector<loc::BackendManifestRow> mutated,
                      loc::BackendManifestError expected, const char* label) {
    Check(loc::ValidateBackendManifestRows(mutated, profiles) == expected, label);
  };
  validate({}, loc::BackendManifestError::kEmpty, "empty manifest rejected");
  {
    auto mutated = rows;
    mutated[0].numeric_id = 0;
    validate(mutated, loc::BackendManifestError::kInvalidNumericField,
             "zero numeric id rejected");
  }
  {
    auto mutated = rows;
    mutated[1].numeric_id = mutated[0].numeric_id;
    validate(mutated, loc::BackendManifestError::kDuplicateNumericId,
             "duplicate numeric id rejected");
  }
  {
    auto mutated = rows;
    mutated[0].locale_profile_id = "missing-profile";
    validate(mutated, loc::BackendManifestError::kUnknownLocaleProfile,
             "unknown owning profile rejected");
  }
  {
    auto mutated = rows;
    mutated[0].scheme_id = "missing_scheme";
    validate(mutated, loc::BackendManifestError::kSchemeNotInProfile,
             "scheme outside the owning profile rejected");
  }
  {
    auto mutated = rows;
    mutated[1].scheme_id = "double_pinyin";
    validate(mutated, loc::BackendManifestError::kBindingForAbsentScheme,
             "binding for an absent scheme rejected");
  }
  {
    auto mutated = rows;
    mutated[0].language_enum = "EPOCANVA_BACKEND_ZH";
    validate(mutated, loc::BackendManifestError::kEnumNameValueMismatch,
             "enum name/value drift rejected");
  }
  {
    auto mutated = rows;
    mutated[0].input_mode_enum.clear();
    validate(mutated, loc::BackendManifestError::kMissingEnumName,
             "missing enum name rejected");
  }
  {
    auto mutated = rows;
    mutated[0].language_tag = "fr";
    validate(mutated, loc::BackendManifestError::kLocaleTagMismatch,
             "language tag drift rejected");
  }
  {
    auto mutated = rows;
    auto duplicate = mutated[0];
    duplicate.numeric_id = 99;
    mutated.push_back(duplicate);
    validate(mutated, loc::BackendManifestError::kDuplicateConfiguration,
             "duplicate language/config tuple rejected");
  }
  {
    auto mutated = rows;
    mutated[5].scheme_implementation = "implemented";
    validate(mutated, loc::BackendManifestError::kSchemeImplementationMismatch,
             "unverified scheme cannot be relabelled implemented");
  }
  {
    auto mutated = rows;
    mutated[0].output_mode = 0;
    validate(mutated, loc::BackendManifestError::kInvalidNumericField,
             "unknown output mode rejected");
  }
}

void BackendBindingAdmissionTests() {
  auto base = [](std::string profile_id, std::string tag) {
    loc::LocaleProfile profile;
    profile.profile_id = std::move(profile_id);
    profile.tag = Parsed(std::move(tag));
    profile.display_label_utf8 = "Test";
    profile.schemes = {{"latin", loc::SchemeImplementation::kImplemented, "engine/locale"}};
    return profile;
  };

  loc::LocaleRegistry registry;
  auto absent = base("xx-latin", "xx");
  absent.schemes[0].implementation = loc::SchemeImplementation::kAbsent;
  absent.schemes[0].evidence_ref.clear();
  absent.backend_bindings = {{1, 1, 1, 1, 1, "xx-XX", "latin"}};
  Check(registry.Register(absent).error == loc::RegistryError::kBackendBindingForAbsentScheme,
        "registering a binding for an absent scheme is rejected");

  auto zero_id = base("xy-latin", "xy");
  zero_id.backend_bindings = {{0, 1, 1, 1, 1, "xy-XX", "latin"}};
  Check(registry.Register(zero_id).error == loc::RegistryError::kInvalidBackendBinding,
        "zero binding id is rejected");

  auto unknown_scheme = base("xz-latin", "xz");
  unknown_scheme.backend_bindings = {{900, 1, 1, 1, 1, "xz-XX", "missing"}};
  Check(registry.Register(unknown_scheme).error == loc::RegistryError::kInvalidBackendBinding,
        "binding for a scheme the profile does not declare is rejected");

  auto first = base("ya-latin", "ya");
  first.backend_bindings = {{901, 1, 1, 1, 1, "ya-XX", "latin"}};
  Check(registry.Register(first).ok(), "first profile with a unique binding registers");
  auto second = base("yb-latin", "yb");
  second.backend_bindings = {{901, 1, 1, 1, 1, "yb-XX", "latin"}};
  Check(registry.Register(second).error == loc::RegistryError::kDuplicateBackendBindingId,
        "binding ids are unique across the registry");
}

void Run() {
  ParseTests();
  MatchingTests();
  PriorityTests();
  BuiltinResolutionTests();
  AdmissionTests();
  BackendBindingTests();
  BackendBindingAdmissionTests();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--manifest") {
    const std::string json = loc::BackendManifestJson();
    if (json.empty()) return 1;
    std::ofstream file(argv[2], std::ios::binary | std::ios::trunc);
    if (!file) return 1;
    file.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!file) return 1;
    std::cout << "MANIFEST WRITTEN " << json.size() << " UTF-8 bytes\n";
    return 0;
  }
  if (argc != 2) return 2;
  const std::string mode = argv[1];
  if (mode == "--forcefail") {
    Check(false, "intentional falsifiability control");
  } else if (mode == "--unit") {
    Run();
  } else {
    return 2;
  }
  if (failed != 0) return 1;
  std::cout << "RESULT PASS\n";
  return 0;
}
