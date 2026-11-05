// Input locale profiles: language tag + text direction + named input schemes.
//
// This registry is identity and admission only. It does not load a dictionary,
// open a schema, generate a candidate or claim language acceptance. An entry is
// resolvable only when the requested input scheme reaches kImplemented, whose
// evidence_ref must point at a local test or source record that actually exists.
// Registering a language name with no engine is deliberately not enough.
//
// No I/O. No global mutable state in the query path. Collection ordinals count
// profiles or schemes, never text units.
#ifndef EPOCANVA_LOCALE_LOCALE_PROFILE_H_
#define EPOCANVA_LOCALE_LOCALE_PROFILE_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "language_tag.h"

namespace epocanva {
namespace locale {

constexpr std::size_t kMaxProfiles = 64;
constexpr std::size_t kMaxSchemesPerProfile = 12;

// kAbsent: nothing exists. kUnverified: data or schema exists but no passing
// end-to-end evidence, so it must not be offered as working. kImplemented:
// real code plus a local test record named in evidence_ref.
enum class SchemeImplementation { kAbsent = 0, kUnverified, kImplemented };

struct InputScheme {
  // Lowercase ASCII identifier: pinyin, double_pinyin, zhuyin, cangjie, wubi,
  // jyutping, romaji, kana, two_set, latin.
  std::string scheme_id;
  SchemeImplementation implementation = SchemeImplementation::kAbsent;
  // Relative local evidence reference (test path or source file). Required for
  // kImplemented, must be empty for kAbsent.
  std::string evidence_ref;
};

struct LocaleProfile {
  // Stable ASCII identifier, unique in the registry. This is the value a
  // platform adapter stores in its settings; it is never a display label.
  std::string profile_id;
  LanguageTag tag;  // Canonical, script always resolved when resolvable.
  // Human-readable label. Never used for lookup, ordering or persistence keys.
  std::string display_label_utf8;
  std::vector<InputScheme> schemes;
};

enum class RegistryError {
  kOk = 0,
  kInvalidProfileId,
  kInvalidTag,
  kEmptySchemes,
  kTooManySchemes,
  kInvalidSchemeId,
  kDuplicateSchemeId,
  kImplementedWithoutEvidence,
  kEvidenceOnAbsentScheme,
  kDuplicateProfileId,
  kDuplicateTag,
  kRegistryFull,
  kInvalidDisplayLabel,
};

const char* RegistryErrorName(RegistryError error);

struct RegisterResult {
  RegistryError error = RegistryError::kOk;
  std::size_t profile_ordinal = 0;  // valid only when error == kOk
  bool ok() const { return error == RegistryError::kOk; }
};

// Requested locale. Empty fields are wildcards; an explicit region or script
// narrows the match. scheme_id must name a scheme available in the profile.
struct ResolveRequest {
  LanguageTag pattern;
  std::string scheme_id;
};

enum class ResolutionStatus {
  kResolved = 0,
  kInvalidRequest,      // malformed tag or scheme identifier
  kNoProfile,           // no profile for this language at all
  kAmbiguous,           // several profiles match; caller must add script/region
  kSchemeAbsent,        // the profile exists but has no such scheme
  kSchemeNotVerified,   // the scheme exists but has no passing evidence
  kRegistryEmpty,
};

const char* ResolutionStatusName(ResolutionStatus status);

struct Resolution {
  ResolutionStatus status = ResolutionStatus::kNoProfile;
  std::size_t profile_ordinal = 0;  // valid only when status == kResolved
  std::size_t match_count = 0;      // matching profiles, 1..kMaxProfiles
};

class LocaleRegistry {
 public:
  LocaleRegistry() = default;

  // Rejects duplicates, malformed identifiers and unsubstantiated
  // kImplemented claims. Registration order is preserved for stable output.
  RegisterResult Register(LocaleProfile profile);
  std::size_t size() const { return profiles_.size(); }
  const std::vector<LocaleProfile>& profiles() const { return profiles_; }

  // Deterministic resolution:
  //   1. candidates are profiles whose tag LookupMatches the request pattern;
  //   2. when more than one remains and the request omitted script or region,
  //      the result is kAmbiguous instead of an arbitrary pick;
  //   3. otherwise the single candidate is checked for the requested scheme.
  Resolution Resolve(const ResolveRequest& request) const;

 private:
  std::vector<LocaleProfile> profiles_;
};

// The compiled-in catalogue. Profiles whose engines do not exist are simply not
// listed; the list is an implementation statement, not a wish list.
//
// Coverage as of this revision, stated plainly:
//   en-US latin          implemented (ASCII English core + lexicon + spelling)
//   fr-FR latin          implemented (French core + Hunspell dictionary)
//   ja-JP romaji,kana    implemented (romaji composer, kana output)
//   ko-KR two_set        implemented (two-set composer)
//   zh-Hans-CN pinyin    implemented (Rime pinyin + bounded initialism decoder)
//   zh-Hans-CN double_pinyin  absent
//   zh-Hans-CN zhuyin    unverified (schema exists, no end-to-end evidence)
//   zh-Hant-TW zhuyin    unverified (no registered zh-Hant data set)
//   zh-Hant-HK           unverified (no registered zh-Hant data set)
//   yue-Hant-HK jyutping absent
// es, de and the remaining declared languages have no profile entry at all.
std::vector<LocaleProfile> BuiltinProfiles();

}  // namespace locale
}  // namespace epocanva
#endif  // EPOCANVA_LOCALE_LOCALE_PROFILE_H_
