#include "language_package.h"

#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <utility>

namespace epocanva {
namespace language_packages {
namespace {

void Add(ValidationResult* result, ValidationCode code, const char* field) {
  result->issues.push_back({code, field});
}

bool IsLanguageId(std::string_view id) {
  return id == "en" || id == "zh" || id == "fr" || id == "ja" || id == "ko";
}

bool IsHexDigest(std::string_view value) {
  if (value.size() != 64) return false;
  for (char ch : value) {
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
  }
  return true;
}

bool IsPackageReference(std::string_view value) {
  if (value.empty() || value.front() == '/' || value.back() == '/' ||
      value.find('\\') != std::string_view::npos || value.find(':') != std::string_view::npos ||
      value.find('\0') != std::string_view::npos) return false;
  std::size_t segment_begin = 0;
  for (std::size_t i = 0; i <= value.size(); ++i) {
    if (i == value.size() || value[i] == '/') {
      const std::string_view segment = value.substr(segment_begin, i - segment_begin);
      if (segment.empty() || segment == "." || segment == "..") return false;
      segment_begin = i + 1;
      continue;
    }
    const char ch = value[i];
    if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) return false;
  }
  return true;
}

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
        (continuation == 3 && scalar < 0x10000) ||
        scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff) ||
        scalar < 0x20 || (scalar >= 0x7f && scalar <= 0x9f)) return false;
    i += continuation + 1;
  }
  return true;
}

bool IsCapability(Capability value) {
  switch (value) {
    case Capability::kComposition:
    case Capability::kPhoneticAnalysis:
    case Capability::kLexicalConversion:
    case Capability::kCompletion:
    case Capability::kSpellingCorrection:
    case Capability::kMorphology:
    case Capability::kAutoSegmentation:
    case Capability::kAbbreviationExpansion:
    case Capability::kFrequencyRanking:
    case Capability::kContextLearning:
    case Capability::kCrossAppConsistency:
      return true;
  }
  return false;
}

bool IsDataRole(DataRole value) {
  switch (value) {
    case DataRole::kSchema:
    case DataRole::kDictionary:
    case DataRole::kFrequency:
    case DataRole::kMorphology:
    case DataRole::kConversionModel:
    case DataRole::kAbbreviations:
      return true;
  }
  return false;
}

bool IsAvailability(DataAvailability value) {
  return value == DataAvailability::kMissing || value == DataAvailability::kPackaged;
}

bool IsInputMode(InputMode value) {
  switch (value) {
    case InputMode::kLatin:
    case InputMode::kPinyin:
    case InputMode::kZhuyin:
    case InputMode::kRomaji:
    case InputMode::kKana:
    case InputMode::kTwoSetKorean:
      return true;
  }
  return false;
}

bool IsOutputMode(OutputMode value) {
  switch (value) {
    case OutputMode::kLatin:
    case OutputMode::kPhonetic:
    case OutputMode::kHanzi:
    case OutputMode::kHiragana:
    case OutputMode::kKatakana:
    case OutputMode::kKanji:
    case OutputMode::kHangul:
      return true;
  }
  return false;
}

bool IsReadinessStage(ReadinessStage value) {
  return value == ReadinessStage::kUnavailable || value == ReadinessStage::kPartial ||
         value == ReadinessStage::kIntegrated;
}

bool UniqueInputMode(const std::vector<InputMode>& modes, std::size_t ordinal) {
  return std::find(modes.begin(), modes.begin() + static_cast<std::ptrdiff_t>(ordinal),
                   modes[ordinal]) == modes.begin() + static_cast<std::ptrdiff_t>(ordinal);
}

bool UniqueOutputMode(const std::vector<OutputMode>& modes, std::size_t ordinal) {
  return std::find(modes.begin(), modes.begin() + static_cast<std::ptrdiff_t>(ordinal),
                   modes[ordinal]) == modes.begin() + static_cast<std::ptrdiff_t>(ordinal);
}

const DataResource* FindResource(const Manifest& manifest, const std::string& resource_id) {
  const auto it = std::find_if(manifest.resources.begin(), manifest.resources.end(),
                               [&](const DataResource& resource) {
                                 return resource.resource_id == resource_id;
                               });
  return it == manifest.resources.end() ? nullptr : &*it;
}

const ResourceEvidence* FindEvidence(const std::vector<ResourceEvidence>& evidence,
                                     const std::string& resource_id) {
  const auto it = std::find_if(evidence.begin(), evidence.end(),
                               [&](const ResourceEvidence& item) {
                                 return item.resource_id == resource_id;
                               });
  return it == evidence.end() ? nullptr : &*it;
}

bool MatchesIdentity(const Manifest& manifest, const DataResource& resource,
                     const ResourceEvidence& item) {
  return item.resource_id == resource.resource_id && item.language_id == manifest.language_id &&
         item.schema_id == manifest.schema_id && item.source_id == resource.source_id &&
         item.source_revision == resource.source_revision && item.sha256 == resource.sha256 &&
         item.license_ref == resource.license_ref;
}

}  // namespace

int LanguagePriority(std::string_view language_id) {
  if (language_id == "en") return 0;
  if (language_id == "zh") return 1;
  if (language_id == "fr") return 2;
  if (language_id == "ja") return 3;
  if (language_id == "ko") return 4;
  return -1;
}

ValidationResult ValidateManifest(const Manifest& manifest) {
  ValidationResult result;
  if (!IsLanguageId(manifest.language_id)) {
    Add(&result, ValidationCode::kInvalidLanguageId, "language_id");
  }
  if (!IsApprovedSchemaId(manifest.language_id, manifest.schema_id)) {
    Add(&result, ValidationCode::kInvalidSchemaId, "schema_id");
  }
  if (!IsDisplayLabel(manifest.display_label_utf8)) {
    Add(&result, ValidationCode::kInvalidDisplayLabel, "display_label_utf8");
  }
  if (ValidateModuleRequirements(manifest.required_modules) != ModuleRequirementsStatus::kOk) {
    Add(&result, ValidationCode::kInvalidModules, "required_modules");
  }
  if (!IsReadinessStage(manifest.readiness.stage) ||
      (manifest.readiness.stage == ReadinessStage::kUnavailable &&
       !manifest.readiness.evidence_ref.empty()) ||
      (manifest.readiness.stage != ReadinessStage::kUnavailable &&
       !IsPackageReference(manifest.readiness.evidence_ref))) {
    Add(&result, ValidationCode::kInvalidReadiness, "readiness");
  }
  for (const std::string& blocker : manifest.readiness.blockers_utf8) {
    if (!IsDisplayLabel(blocker)) Add(&result, ValidationCode::kInvalidReadiness, "readiness.blockers");
  }

  for (std::size_t i = 0; i < manifest.input_modes.size(); ++i) {
    if (!IsInputMode(manifest.input_modes[i])) {
      Add(&result, ValidationCode::kInvalidMode, "input_modes");
    } else if (!UniqueInputMode(manifest.input_modes, i)) {
      Add(&result, ValidationCode::kDuplicateMode, "input_modes");
    }
  }
  for (std::size_t i = 0; i < manifest.output_modes.size(); ++i) {
    if (!IsOutputMode(manifest.output_modes[i])) {
      Add(&result, ValidationCode::kInvalidMode, "output_modes");
    } else if (!UniqueOutputMode(manifest.output_modes, i)) {
      Add(&result, ValidationCode::kDuplicateMode, "output_modes");
    }
  }

  for (std::size_t i = 0; i < manifest.resources.size(); ++i) {
    const DataResource& resource = manifest.resources[i];
    if (!IsSafeIdentifier(resource.resource_id) || !IsDataRole(resource.role) ||
        !IsAvailability(resource.availability)) {
      Add(&result, ValidationCode::kInvalidResource, "resources");
    }
    for (std::size_t earlier = 0; earlier < i; ++earlier) {
      if (manifest.resources[earlier].resource_id == resource.resource_id) {
        Add(&result, ValidationCode::kDuplicateResource, "resources.resource_id");
        break;
      }
    }
    if (resource.availability == DataAvailability::kPackaged) {
      if (!IsPackageReference(resource.package_ref) || resource.source_id.empty() ||
          !IsSafeProvenanceAtom(resource.source_id) || resource.source_revision.empty() ||
          !IsSafeProvenanceAtom(resource.source_revision) || !IsHexDigest(resource.sha256) ||
          !IsPackageReference(resource.license_ref)) {
        Add(&result, ValidationCode::kInvalidResource, "resources.provenance");
      }
    } else if (!resource.package_ref.empty() || !resource.source_id.empty() ||
               !resource.source_revision.empty() || !resource.sha256.empty() ||
               !resource.license_ref.empty()) {
      Add(&result, ValidationCode::kInvalidResource, "resources.missing");
    }
  }

  for (std::size_t i = 0; i < manifest.capabilities.size(); ++i) {
    const CapabilityDeclaration& declaration = manifest.capabilities[i];
    if (!IsCapability(declaration.capability)) {
      Add(&result, ValidationCode::kInvalidCapability, "capabilities");
    }
    for (std::size_t earlier = 0; earlier < i; ++earlier) {
      if (manifest.capabilities[earlier].capability == declaration.capability) {
        Add(&result, ValidationCode::kDuplicateCapability, "capabilities");
        break;
      }
    }
    bool has_dictionary = false;
    for (std::size_t resource_ordinal = 0; resource_ordinal < declaration.resource_ids.size();
         ++resource_ordinal) {
      const std::string& id = declaration.resource_ids[resource_ordinal];
      for (std::size_t earlier = 0; earlier < resource_ordinal; ++earlier) {
        if (declaration.resource_ids[earlier] == id) {
          Add(&result, ValidationCode::kDuplicateResourceRequirement,
              "capabilities.resource_ids");
          break;
        }
      }
      const DataResource* resource = FindResource(manifest, id);
      if (resource == nullptr) {
        Add(&result, ValidationCode::kUnknownResourceRequirement,
            "capabilities.resource_ids");
      } else if (resource->role == DataRole::kDictionary) {
        has_dictionary = true;
      }
    }
    if (declaration.capability == Capability::kLexicalConversion && !has_dictionary) {
      Add(&result, ValidationCode::kRequiredDataRoleMissing,
          "capabilities.lexical_conversion");
    }
  }
  return result;
}

ValidationResult ValidateCatalog(const std::vector<Manifest>& manifests) {
  ValidationResult result;
  for (std::size_t i = 0; i < manifests.size(); ++i) {
    ValidationResult current = ValidateManifest(manifests[i]);
    result.issues.insert(result.issues.end(), current.issues.begin(), current.issues.end());
    for (std::size_t earlier = 0; earlier < i; ++earlier) {
      if (manifests[earlier].language_id == manifests[i].language_id) {
        Add(&result, ValidationCode::kDuplicateLanguageIdentity, "language_id");
      }
      if (manifests[earlier].schema_id == manifests[i].schema_id) {
        Add(&result, ValidationCode::kDuplicateSchemaIdentity, "schema_id");
      }
    }
  }
  return result;
}

bool AdmissionResult::Has(Capability capability) const {
  return std::find(admitted.begin(), admitted.end(), capability) != admitted.end();
}

AdmissionResult AdmitCapabilities(const Manifest& manifest,
                                  const std::vector<ResourceEvidence>& evidence) {
  AdmissionResult result;
  result.validation = ValidateManifest(manifest);
  if (!result.validation.ok()) {
    for (const CapabilityDeclaration& declaration : manifest.capabilities) {
      result.refused.push_back({declaration.capability, AdmissionReason::kManifestInvalid});
    }
    return result;
  }

  std::unordered_set<std::string> evidence_ids;
  for (const ResourceEvidence& item : evidence) {
    if (!IsSafeIdentifier(item.resource_id) || !IsLanguageId(item.language_id) ||
        !IsSafeIdentifier(item.schema_id) || !IsSafeProvenanceAtom(item.source_id) ||
        !IsSafeProvenanceAtom(item.source_revision) || !IsHexDigest(item.sha256) ||
        !IsPackageReference(item.license_ref) || !evidence_ids.insert(item.resource_id).second ||
        FindResource(manifest, item.resource_id) == nullptr) {
      result.evidence_valid = false;
    }
  }
  if (!result.evidence_valid) {
    for (const CapabilityDeclaration& declaration : manifest.capabilities) {
      result.refused.push_back({declaration.capability, AdmissionReason::kInvalidEvidence});
    }
    return result;
  }

  for (const CapabilityDeclaration& declaration : manifest.capabilities) {
    if (!declaration.implementation_available) {
      result.refused.push_back({declaration.capability,
                                AdmissionReason::kImplementationUnavailable});
      continue;
    }
    AdmissionReason failure = AdmissionReason::kManifestInvalid;
    bool admitted = true;
    for (const std::string& resource_id : declaration.resource_ids) {
      const DataResource* resource = FindResource(manifest, resource_id);
      if (resource->availability == DataAvailability::kMissing) {
        failure = AdmissionReason::kResourceMissing;
        admitted = false;
        break;
      }
      const ResourceEvidence* item = FindEvidence(evidence, resource_id);
      if (item == nullptr || !item->available) {
        failure = AdmissionReason::kPackageUnavailable;
        admitted = false;
        break;
      }
      if (!MatchesIdentity(manifest, *resource, *item)) {
        failure = AdmissionReason::kResourceIdentityMismatch;
        admitted = false;
        break;
      }
      if (!item->approved_for_use) {
        failure = AdmissionReason::kResourceUnapproved;
        admitted = false;
        break;
      }
    }
    if (admitted) result.admitted.push_back(declaration.capability);
    else result.refused.push_back({declaration.capability, failure});
  }
  return result;
}

}  // namespace language_packages
}  // namespace epocanva
