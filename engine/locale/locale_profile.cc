#include "locale_profile.h"

#include <algorithm>
#include <cstdint>
#include <set>

namespace epocanva {
namespace locale {
namespace {

bool IsSchemeId(std::string_view id) {
  if (id.empty() || id.size() > 32) return false;
  if (id.front() < 'a' || id.front() > 'z') return false;
  for (char ch : id) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
    if (!ok) return false;
  }
  return true;
}

bool IsProfileId(std::string_view id) {
  if (id.empty() || id.size() > 64) return false;
  for (char ch : id) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.';
    if (!ok) return false;
  }
  return true;
}

// A display label must be nonempty valid UTF-8 with no C0/C1 control characters.
// This mirrors the language-package label rule so the two layers cannot disagree
// about what may be shown to a user.
bool IsDisplayLabel(std::string_view text) {
  if (text.empty()) return false;
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    if (lead == 0) return false;
    std::uint32_t scalar = 0;
    std::size_t continuation = 0;
    if (lead < 0x80) {
      scalar = lead;
    } else if (lead >= 0xc2 && lead <= 0xdf) {
      scalar = lead & 0x1f;
      continuation = 1;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      scalar = lead & 0x0f;
      continuation = 2;
    } else if (lead >= 0xf0 && lead <= 0xf4) {
      scalar = lead & 0x07;
      continuation = 3;
    } else {
      return false;
    }
    if (i + continuation >= text.size()) return false;
    for (std::size_t j = 1; j <= continuation; ++j) {
      const unsigned char byte = static_cast<unsigned char>(text[i + j]);
      if ((byte & 0xc0) != 0x80) return false;
      scalar = (scalar << 6) | (byte & 0x3f);
    }
    if ((continuation == 1 && scalar < 0x80) ||
        (continuation == 2 && scalar < 0x800) ||
        (continuation == 3 && scalar < 0x10000) || scalar > 0x10ffff ||
        (scalar >= 0xd800 && scalar <= 0xdfff) || scalar < 0x20 ||
        (scalar >= 0x7f && scalar <= 0x9f)) {
      return false;
    }
    i += continuation + 1;
  }
  return true;
}

bool IsRelativeEvidenceRef(std::string_view ref) {
  if (ref.empty() || ref.size() > 200) return false;
  if (ref.front() == '/' || ref.back() == '/') return false;
  if (ref.find('\\') != std::string_view::npos) return false;
  if (ref.find(':') != std::string_view::npos) return false;
  if (ref.find('\0') != std::string_view::npos) return false;
  std::size_t begin = 0;
  while (true) {
    const std::size_t slash = ref.find('/', begin);
    const std::string_view segment =
        slash == std::string_view::npos ? ref.substr(begin) : ref.substr(begin, slash - begin);
    if (segment.empty() || segment == "." || segment == "..") return false;
    if (slash == std::string_view::npos) break;
    begin = slash + 1;
  }
  return true;
}

LocaleProfile MakeProfile(std::string profile_id, std::string_view tag_text,
                          std::string label, std::vector<InputScheme> schemes) {
  LocaleProfile profile;
  profile.profile_id = std::move(profile_id);
  LanguageTag parsed;
  if (ParseLanguageTag(tag_text, &parsed) == TagError::kOk) {
    profile.tag = std::move(parsed);
  }
  profile.display_label_utf8 = std::move(label);
  profile.schemes = std::move(schemes);
  return profile;
}

InputScheme Scheme(std::string id, SchemeImplementation implementation,
                   std::string evidence) {
  InputScheme scheme;
  scheme.scheme_id = std::move(id);
  scheme.implementation = implementation;
  scheme.evidence_ref = std::move(evidence);
  return scheme;
}

}  // namespace

const char* RegistryErrorName(RegistryError error) {
  switch (error) {
    case RegistryError::kOk: return "ok";
    case RegistryError::kInvalidProfileId: return "invalid_profile_id";
    case RegistryError::kInvalidTag: return "invalid_tag";
    case RegistryError::kEmptySchemes: return "empty_schemes";
    case RegistryError::kTooManySchemes: return "too_many_schemes";
    case RegistryError::kInvalidSchemeId: return "invalid_scheme_id";
    case RegistryError::kDuplicateSchemeId: return "duplicate_scheme_id";
    case RegistryError::kImplementedWithoutEvidence: return "implemented_without_evidence";
    case RegistryError::kEvidenceOnAbsentScheme: return "evidence_on_absent_scheme";
    case RegistryError::kDuplicateProfileId: return "duplicate_profile_id";
    case RegistryError::kDuplicateTag: return "duplicate_tag";
    case RegistryError::kRegistryFull: return "registry_full";
    case RegistryError::kInvalidDisplayLabel: return "invalid_display_label";
  }
  return "invalid_profile_id";
}

const char* ResolutionStatusName(ResolutionStatus status) {
  switch (status) {
    case ResolutionStatus::kResolved: return "resolved";
    case ResolutionStatus::kInvalidRequest: return "invalid_request";
    case ResolutionStatus::kNoProfile: return "no_profile";
    case ResolutionStatus::kAmbiguous: return "ambiguous";
    case ResolutionStatus::kSchemeAbsent: return "scheme_absent";
    case ResolutionStatus::kSchemeNotVerified: return "scheme_not_verified";
    case ResolutionStatus::kRegistryEmpty: return "registry_empty";
  }
  return "no_profile";
}

RegisterResult LocaleRegistry::Register(LocaleProfile profile) {
  RegisterResult result;
  if (!IsProfileId(profile.profile_id)) {
    result.error = RegistryError::kInvalidProfileId;
    return result;
  }
  if (!IsUsableTag(profile.tag)) {
    result.error = RegistryError::kInvalidTag;
    return result;
  }
  if (!IsDisplayLabel(profile.display_label_utf8)) {
    result.error = RegistryError::kInvalidDisplayLabel;
    return result;
  }
  if (profile.schemes.empty()) {
    result.error = RegistryError::kEmptySchemes;
    return result;
  }
  if (profile.schemes.size() > kMaxSchemesPerProfile) {
    result.error = RegistryError::kTooManySchemes;
    return result;
  }
  if (profiles_.size() >= kMaxProfiles) {
    result.error = RegistryError::kRegistryFull;
    return result;
  }

  std::set<std::string> scheme_ids;
  for (const InputScheme& scheme : profile.schemes) {
    if (!IsSchemeId(scheme.scheme_id)) {
      result.error = RegistryError::kInvalidSchemeId;
      return result;
    }
    if (!scheme_ids.insert(scheme.scheme_id).second) {
      result.error = RegistryError::kDuplicateSchemeId;
      return result;
    }
    if (scheme.implementation == SchemeImplementation::kImplemented &&
        !IsRelativeEvidenceRef(scheme.evidence_ref)) {
      result.error = RegistryError::kImplementedWithoutEvidence;
      return result;
    }
    if (scheme.implementation == SchemeImplementation::kAbsent &&
        !scheme.evidence_ref.empty()) {
      result.error = RegistryError::kEvidenceOnAbsentScheme;
      return result;
    }
  }

  const std::string formatted = FormatLanguageTag(profile.tag);
  for (const LocaleProfile& existing : profiles_) {
    if (existing.profile_id == profile.profile_id) {
      result.error = RegistryError::kDuplicateProfileId;
      return result;
    }
    if (FormatLanguageTag(existing.tag) == formatted) {
      result.error = RegistryError::kDuplicateTag;
      return result;
    }
  }

  profiles_.push_back(std::move(profile));
  result.profile_ordinal = profiles_.size() - 1;
  return result;
}

Resolution LocaleRegistry::Resolve(const ResolveRequest& request) const {
  Resolution result;
  if (profiles_.empty()) {
    result.status = ResolutionStatus::kRegistryEmpty;
    return result;
  }
  if (!IsUsableTag(request.pattern) || !IsSchemeId(request.scheme_id)) {
    result.status = ResolutionStatus::kInvalidRequest;
    return result;
  }

  std::vector<std::size_t> matches;
  for (std::size_t ordinal = 0; ordinal < profiles_.size(); ++ordinal) {
    if (LookupMatches(request.pattern, profiles_[ordinal].tag)) {
      matches.push_back(ordinal);
    }
  }
  result.match_count = matches.size();
  if (matches.empty()) {
    result.status = ResolutionStatus::kNoProfile;
    return result;
  }
  if (matches.size() > 1) {
    // Omitting script or region when several distinct profiles match is a real
    // ambiguity (zh-Hans-CN vs zh-Hant-TW). Guessing would silently pick a
    // script the user did not ask for, so the caller must narrow the request.
    result.status = ResolutionStatus::kAmbiguous;
    return result;
  }

  const LocaleProfile& profile = profiles_[matches.front()];
  for (const InputScheme& scheme : profile.schemes) {
    if (scheme.scheme_id != request.scheme_id) continue;
    result.profile_ordinal = matches.front();
    switch (scheme.implementation) {
      case SchemeImplementation::kImplemented:
        result.status = ResolutionStatus::kResolved;
        return result;
      case SchemeImplementation::kUnverified:
        result.status = ResolutionStatus::kSchemeNotVerified;
        return result;
      case SchemeImplementation::kAbsent:
        result.status = ResolutionStatus::kSchemeAbsent;
        return result;
    }
  }
  result.status = ResolutionStatus::kSchemeAbsent;
  return result;
}

std::vector<LocaleProfile> BuiltinProfiles() {
  // Evidence references point at real local records. They are relative paths so
  // this catalogue never encodes a machine-specific location.
  std::vector<LocaleProfile> profiles;

  profiles.push_back(MakeProfile(
      "en-US-latin", "en-US", u8"English (United States)",
      {Scheme("latin", SchemeImplementation::kImplemented, "engine/languages/en")}));

  profiles.push_back(MakeProfile(
      "fr-FR-latin", "fr-FR", u8"Fran\u00e7ais (France)",
      {Scheme("latin", SchemeImplementation::kImplemented, "engine/languages/fr")}));

  profiles.push_back(MakeProfile(
      "ja-JP-romaji", "ja-JP", u8"\u65e5\u672c\u8a9e (Romaji)",
      {Scheme("romaji", SchemeImplementation::kImplemented,
              "engine/languages/ja/romaji_composer.cc"),
       Scheme("kana", SchemeImplementation::kImplemented,
              "engine/languages/ja/romaji_composer.cc")}));

  profiles.push_back(MakeProfile(
      "ko-KR-two-set", "ko-KR", u8"\ud55c\uad6d\uc5b4 (\ub450\ubc8c\uc2dd)",
      {Scheme("two_set", SchemeImplementation::kImplemented,
              "engine/languages/ko/two_set_composer.cc")}));

  profiles.push_back(MakeProfile(
      "zh-Hans-CN-pinyin", "zh-Hans-CN", u8"\u4e2d\u6587\uff08\u7b80\u4f53\uff0c\u62fc\u97f3\uff09",
      {Scheme("pinyin", SchemeImplementation::kImplemented,
              "engine/languages/zh/pinyin_initialism.cc"),
       Scheme("double_pinyin", SchemeImplementation::kAbsent, ""),
       Scheme("zhuyin", SchemeImplementation::kUnverified, "")}));

  // zh-Hant profiles are registered with unverified schemes only. No registered
  // zh-Hant data set exists, so claiming zhuyin or cangjie here would repeat the
  // "simplified/traditional conversion equals regional support" mistake.
  profiles.push_back(MakeProfile(
      "zh-Hant-TW-zhuyin", "zh-Hant-TW", u8"\u4e2d\u6587\uff08\u7e41\u9ad4\uff0c\u6ce8\u97f3\uff09",
      {Scheme("zhuyin", SchemeImplementation::kUnverified, ""),
       Scheme("cangjie", SchemeImplementation::kAbsent, ""),
       Scheme("pinyin", SchemeImplementation::kUnverified, "")}));

  profiles.push_back(MakeProfile(
      "zh-Hant-HK-cangjie", "zh-Hant-HK", u8"\u4e2d\u6587\uff08\u7e41\u9ad4\uff0c\u9999\u6e2f\uff09",
      {Scheme("cangjie", SchemeImplementation::kAbsent, ""),
       Scheme("zhuyin", SchemeImplementation::kUnverified, "")}));

  profiles.push_back(MakeProfile(
      "yue-Hant-HK-jyutping", "yue-Hant-HK", u8"\u7cb5\u8a9e\uff08\u9999\u6e2f\uff09",
      {Scheme("jyutping", SchemeImplementation::kAbsent, "")}));

  return profiles;
}

}  // namespace locale
}  // namespace epocanva
