// Locale identity and profile admission tests.
//
// Every check here can fail: the --forcefail case proves the harness reports a
// red run, and each assertion encodes a behaviour the implementation must have
// rather than a property it trivially satisfies.
#include "language_tag.h"
#include "locale_profile.h"

#include <iostream>
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

void Run() {
  ParseTests();
  MatchingTests();
  PriorityTests();
  BuiltinResolutionTests();
  AdmissionTests();
}

}  // namespace

int main(int argc, char** argv) {
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
