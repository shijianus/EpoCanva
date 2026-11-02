#ifndef EPOCANVA_LANGUAGE_PACKAGES_MODULE_REQUIREMENTS_H_
#define EPOCANVA_LANGUAGE_PACKAGES_MODULE_REQUIREMENTS_H_

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace epocanva {
namespace language_packages {

// ASCII identifiers: [a-z][a-z0-9_]*, at most 64 UTF-8 code units (bytes).
// No paths, extensions, whitespace, embedded NUL or locale-dependent folding.
inline bool IsSafeIdentifier(std::string_view id) {
  if (id.empty() || id.size() > 64 || id.front() < 'a' || id.front() > 'z') {
    return false;
  }
  for (const char ch : id) {
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) {
      return false;
    }
  }
  return true;
}

inline bool IsApprovedSchemaId(std::string_view language_id, std::string_view schema_id) {
  return (language_id == "en" && schema_id == "epocanva_english") ||
         (language_id == "zh" && schema_id == "epocanva_chinese") ||
         (language_id == "fr" && schema_id == "epocanva_french") ||
         (language_id == "ja" && schema_id == "epocanva_japanese") ||
         (language_id == "ko" && schema_id == "epocanva_korean");
}

inline bool IsSafeProvenanceAtom(std::string_view value) {
  if (value.empty() || value.size() > 128) return false;
  for (const char ch : value) {
    const bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    const bool digit = ch >= '0' && ch <= '9';
    if (!letter && !digit && ch != '_' && ch != '-' && ch != '.' && ch != '+') return false;
  }
  return true;
}

// EXACT compiled-in module allowlist. Returning static storage also avoids
// borrowing RuntimeOptions string storage for RimeTraits.modules. This does not
// certify that the chosen DLL contains the module: find_module still must pass.
// levers is deliberately deployment-only, not a language package requirement.
inline const char* ApprovedModuleName(std::string_view name) {
  constexpr const char* names[] = {
      "default", "core", "dict", "gears", "epocanva_english"};
  for (const char* approved : names) {
    if (name == approved) return approved;
  }
  return nullptr;
}

enum class ModuleRequirementsStatus { kOk, kEmpty, kUnsafeName, kUnapprovedName, kDuplicate };

inline ModuleRequirementsStatus ValidateModuleRequirements(
    const std::vector<std::string>& modules) {
  if (modules.empty()) return ModuleRequirementsStatus::kEmpty;
  for (std::size_t ordinal = 0; ordinal < modules.size(); ++ordinal) {
    if (!IsSafeIdentifier(modules[ordinal])) return ModuleRequirementsStatus::kUnsafeName;
    if (ApprovedModuleName(modules[ordinal]) == nullptr) {
      return ModuleRequirementsStatus::kUnapprovedName;
    }
    for (std::size_t earlier = 0; earlier < ordinal; ++earlier) {
      if (modules[earlier] == modules[ordinal]) return ModuleRequirementsStatus::kDuplicate;
    }
  }
  return ModuleRequirementsStatus::kOk;
}

}  // namespace language_packages
}  // namespace epocanva
#endif  // EPOCANVA_LANGUAGE_PACKAGES_MODULE_REQUIREMENTS_H_
