#ifndef EPOCANVA_LANGUAGE_PACKAGES_LANGUAGE_PACKAGE_H_
#define EPOCANVA_LANGUAGE_PACKAGES_LANGUAGE_PACKAGE_H_

#include <string>
#include <string_view>
#include <vector>

#include "module_requirements.h"

namespace epocanva {
namespace language_packages {

// This value contract performs no I/O, hashing, loading, deployment or policy
// changes. Text fields are UTF-8; identifiers and references use the rules below.
// Collection ordinals count records, never text/code units. No caret API exists.
enum class Capability {
  kComposition,
  kPhoneticAnalysis,
  kLexicalConversion,
  kCompletion,
  kSpellingCorrection,
  kMorphology,
  kAutoSegmentation,
  kAbbreviationExpansion,
  kFrequencyRanking,
  kContextLearning,
  kCrossAppConsistency,
};
enum class DataRole { kSchema, kDictionary, kFrequency, kMorphology, kConversionModel, kAbbreviations };
enum class DataAvailability { kMissing, kPackaged };
enum class InputMode { kLatin, kPinyin, kZhuyin, kRomaji, kKana, kTwoSetKorean };
enum class OutputMode { kLatin, kPhonetic, kHanzi, kHiragana, kKatakana, kKanji, kHangul };
enum class ReadinessStage { kUnavailable, kPartial, kIntegrated };

struct DataResource {
  std::string resource_id;
  DataRole role = DataRole::kDictionary;
  DataAvailability availability = DataAvailability::kMissing;
  // Slash-separated relative package reference, never an OS path or a URL.
  std::string package_ref;
  std::string source_id;
  std::string source_revision;
  std::string sha256;  // Exactly 64 lowercase ASCII hex bytes when known.
  std::string license_ref;  // Relative package reference; not a licence grant.
};

struct CapabilityDeclaration {
  Capability capability = Capability::kComposition;
  bool implementation_available = false;
  // ALL listed resources are required for this capability. No implicit fallback.
  std::vector<std::string> resource_ids;
};

struct Readiness {
  ReadinessStage stage = ReadinessStage::kUnavailable;
  std::string evidence_ref;  // Relative local test/review record, mandatory if not unavailable.
  std::vector<std::string> blockers_utf8;
};

struct Manifest {
  std::string language_id;  // Exactly en, zh, fr, ja or ko (priority in that order).
  std::string schema_id;    // IsSafeIdentifier; unique in a catalog.
  std::string display_label_utf8;
  std::vector<CapabilityDeclaration> capabilities;
  std::vector<DataResource> resources;
  std::vector<InputMode> input_modes;
  std::vector<OutputMode> output_modes;
  std::vector<std::string> required_modules;
  Readiness readiness;
};

enum class ValidationCode {
  kInvalidLanguageId,
  kInvalidSchemaId,
  kInvalidDisplayLabel,
  kInvalidModules,
  kInvalidMode,
  kDuplicateMode,
  kInvalidReadiness,
  kInvalidCapability,
  kDuplicateCapability,
  kInvalidResource,
  kDuplicateResource,
  kDuplicateResourceRequirement,
  kUnknownResourceRequirement,
  kRequiredDataRoleMissing,
  kDuplicateLanguageIdentity,
  kDuplicateSchemaIdentity,
};
struct ValidationIssue {
  ValidationCode code;
  // A fixed contract field name, never caller text or data content.
  const char* field;
};
struct ValidationResult {
  std::vector<ValidationIssue> issues;
  bool ok() const { return issues.empty(); }
};

// Catalog identity is deliberately one record per language; multiple input/output
// modes belong to that record. No case-folding, aliasing, sorting or merging.
ValidationResult ValidateManifest(const Manifest& manifest);
ValidationResult ValidateCatalog(const std::vector<Manifest>& manifests);
int LanguagePriority(std::string_view language_id);  // en=0 .. ko=4; unknown=-1.

// Evidence is supplied by a TRUSTED local verifier/installer, not by the manifest.
// This layer only matches its values; it never reads a file or certifies a hash,
// a licence or a review. Approval must cover this resource's exact identity and
// intended use. Empty/absent evidence is a refusal, not a skipped verification.
struct ResourceEvidence {
  std::string resource_id;
  std::string language_id;
  std::string schema_id;
  std::string source_id;
  std::string source_revision;
  std::string sha256;
  std::string license_ref;
  bool available = false;
  bool approved_for_use = false;
};

enum class AdmissionReason {
  kManifestInvalid,
  kInvalidEvidence,
  kImplementationUnavailable,
  kPackageUnavailable,
  kResourceMissing,
  kResourceIdentityMismatch,
  kResourceUnapproved,
};
struct CapabilityRefusal {
  Capability capability;
  AdmissionReason reason;
};
struct AdmissionResult {
  ValidationResult validation;
  bool evidence_valid = true;
  std::vector<Capability> admitted;
  std::vector<CapabilityRefusal> refused;
  bool Has(Capability capability) const;
};

// Partial admission is explicit: an independent implemented algorithm can remain
// available when lexical data is absent. Dictionary-backed claims require at
// least one dictionary, and ALL declared dependencies require matching approved
// evidence. This is never product enablement, language acceptance or a release gate.
AdmissionResult AdmitCapabilities(const Manifest& manifest,
                                  const std::vector<ResourceEvidence>& evidence);

}  // namespace language_packages
}  // namespace epocanva
#endif  // EPOCANVA_LANGUAGE_PACKAGES_LANGUAGE_PACKAGE_H_
