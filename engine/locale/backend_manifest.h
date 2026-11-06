// Deterministic, machine-readable export of the backend binding catalogue.
//
// BuiltinProfiles() is the single authored source for profile identity, input
// schemes and backend bindings. This module flattens that catalogue into rows a
// build tool can serialize and a Python staging script can consume. It is not a
// second catalogue: every field is read from LocaleProfile/BackendBinding, and
// tests compare the committed JSON with a fresh export byte for byte.
//
// A row keeps both the symbolic C ABI enum name and its numeric value so the
// export can be checked against engine/include/epocanva_backend.h without that
// header becoming a dependency of the locale layer.
#ifndef EPOCANVA_LOCALE_BACKEND_MANIFEST_H_
#define EPOCANVA_LOCALE_BACKEND_MANIFEST_H_

#include <cstdint>
#include <string>
#include <vector>

#include "locale_profile.h"

namespace epocanva {
namespace locale {

constexpr char kBackendManifestOwner[] = "epocanva-locale-backend-bindings-v1";
constexpr std::uint32_t kBackendManifestFormat = 1;

struct BackendManifestRow {
  std::uint32_t numeric_id = 0;      // epocanva_backend_profile.profile_id
  std::string locale_profile_id;     // owning LocaleProfile::profile_id
  std::string language_tag;          // LocaleProfile tag language subtag (en, zh, ...)
  std::string language_enum;         // EPOCANVA_BACKEND_{EN,ZH,FR,JA,KO}
  std::uint32_t language = 0;
  std::string input_mode_enum;       // EPOCANVA_BACKEND_{LATIN,PINYIN,...}
  std::uint32_t input_mode = 0;
  std::string output_mode_enum;      // EPOCANVA_BACKEND_OUTPUT_*
  std::uint32_t output_mode = 0;
  std::string keyboard_layout_enum;  // EPOCANVA_BACKEND_KEYBOARD_*
  std::uint32_t keyboard_layout = 0;
  std::string variant_id;
  std::string scheme_id;
  // "implemented" or "unverified": the owning profile's implementation state
  // for scheme_id. An exported binding never upgrades this state.
  std::string scheme_implementation;
};

enum class BackendManifestError {
  kOk = 0,
  kEmpty,
  kDuplicateNumericId,
  kUnknownLocaleProfile,
  kSchemeNotInProfile,
  kBindingForAbsentScheme,
  kDuplicateConfiguration,
  kInvalidNumericField,
  kMissingEnumName,
  kEnumNameValueMismatch,
  kLocaleTagMismatch,
  kSchemeImplementationMismatch,
};

const char* BackendManifestErrorName(BackendManifestError error);

// Checks the flattened rows against the catalogue they came from. Every check
// is falsifiable from the row data alone, so tests can mutate one field and see
// the validator reject it.
BackendManifestError ValidateBackendManifestRows(
    const std::vector<BackendManifestRow>& rows,
    const std::vector<LocaleProfile>& profiles);

// Rows sorted by numeric_id. Sorting is part of the export contract: the same
// catalogue always produces the same order regardless of profile registration.
std::vector<BackendManifestRow> BackendManifestRows();

// Canonical JSON (UTF-8, LF, two-space indent, fixed field order). Returns an
// empty string when the catalogue fails validation; callers must treat that as
// an error, never as an empty manifest.
std::string BackendManifestJson();

}  // namespace locale
}  // namespace epocanva
#endif  // EPOCANVA_LOCALE_BACKEND_MANIFEST_H_
