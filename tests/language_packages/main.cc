#include "language_package.h"

#include <iostream>
#include <string>
#include <vector>

namespace {
namespace lp = epocanva::language_packages;
int failed = 0;
void Check(bool value, const char* label) {
  if (!value) { ++failed; std::cerr << "ASSERT FAIL " << label << '\n'; }
}

lp::Manifest PartialChinese() {
  lp::Manifest manifest;
  manifest.language_id = "zh";
  manifest.schema_id = "epocanva_chinese";
  manifest.display_label_utf8 = u8"中文";
  manifest.input_modes = {lp::InputMode::kPinyin, lp::InputMode::kZhuyin};
  manifest.output_modes = {lp::OutputMode::kPhonetic};
  manifest.required_modules = {"default"};
  manifest.readiness.stage = lp::ReadinessStage::kPartial;
  manifest.readiness.evidence_ref = "tests/chinese_core";
  manifest.readiness.blockers_utf8 = {"No approved dictionary; algorithms only"};
  manifest.resources = {{"dictionary", lp::DataRole::kDictionary,
                         lp::DataAvailability::kMissing, {}, {}, {}, {}, {}}};
  manifest.capabilities = {
      {lp::Capability::kPhoneticAnalysis, true, {}},
      {lp::Capability::kLexicalConversion, true, {"dictionary"}}};
  return manifest;
}

bool HasIssue(const lp::ValidationResult& result, lp::ValidationCode code) {
  for (const auto& issue : result.issues) if (issue.code == code) return true;
  return false;
}

void Tests() {
  const lp::Manifest partial = PartialChinese();
  Check(lp::ValidateManifest(partial).ok(), "explicit partial manifest is valid");
  auto admission = lp::AdmitCapabilities(partial, {});
  Check(admission.validation.ok(), "partial admission remains structurally valid");
  Check(admission.Has(lp::Capability::kPhoneticAnalysis), "independent phonetics admitted");
  Check(!admission.Has(lp::Capability::kLexicalConversion), "missing dictionary blocks conversion");
  Check(admission.refused.size() == 1 &&
        admission.refused.front().reason == lp::AdmissionReason::kResourceMissing,
        "declared missing data fails capability admission");

  // Product order: English > Chinese > French > Spanish > Japanese > Korean >
  // German. Spanish and German are core-required by the spec even though their
  // engines do not exist yet; priority is a planning statement, not a claim.
  Check(lp::LanguagePriority("en") == 0 && lp::LanguagePriority("zh") == 1 &&
        lp::LanguagePriority("fr") == 2 && lp::LanguagePriority("es") == 3 &&
        lp::LanguagePriority("ja") == 4 && lp::LanguagePriority("ko") == 5 &&
        lp::LanguagePriority("de") == 6,
        "fixed seven-language priority");
  Check(lp::LanguagePriority("pt") == -1 && lp::LanguagePriority("zz") == -1,
        "manifest priority only ranks the core languages");
  Check(lp::ValidateModuleRequirements({"epocanva_english"}) ==
        lp::ModuleRequirementsStatus::kOk, "default English module valid");
  Check(lp::ValidateModuleRequirements({}) == lp::ModuleRequirementsStatus::kEmpty,
        "empty modules rejected");
  Check(lp::ValidateModuleRequirements({"default", "default"}) ==
        lp::ModuleRequirementsStatus::kDuplicate, "duplicate modules rejected");
  Check(lp::ValidateModuleRequirements({"../evil"}) ==
        lp::ModuleRequirementsStatus::kUnsafeName, "module path rejected");
  Check(lp::ValidateModuleRequirements({"external_plugin"}) ==
        lp::ModuleRequirementsStatus::kUnapprovedName, "unknown safe module rejected");

  lp::Manifest changed = partial;
  changed.schema_id = "../schema";
  Check(HasIssue(lp::ValidateManifest(changed), lp::ValidationCode::kInvalidSchemaId),
        "unsafe schema rejected");
  changed = partial;
  changed.language_id = "ZH";
  Check(HasIssue(lp::ValidateManifest(changed), lp::ValidationCode::kInvalidLanguageId),
        "language identifiers are exact");
  changed = partial;
  changed.display_label_utf8 = "\xED\xA0\x80";
  Check(HasIssue(lp::ValidateManifest(changed), lp::ValidationCode::kInvalidDisplayLabel),
        "surrogate UTF-8 label rejected");
  changed = partial;
  changed.capabilities.back().resource_ids.clear();
  Check(HasIssue(lp::ValidateManifest(changed), lp::ValidationCode::kRequiredDataRoleMissing),
        "conversion cannot omit dictionary dependency");
  changed = partial;
  changed.capabilities.back().resource_ids = {"not_declared"};
  Check(HasIssue(lp::ValidateManifest(changed), lp::ValidationCode::kUnknownResourceRequirement),
        "unknown resource rejected");
  changed = partial;
  changed.resources.push_back(changed.resources.front());
  Check(HasIssue(lp::ValidateManifest(changed), lp::ValidationCode::kDuplicateResource),
        "duplicate resource identity rejected");
  changed = partial;
  changed.capabilities.push_back(changed.capabilities.front());
  Check(HasIssue(lp::ValidateManifest(changed), lp::ValidationCode::kDuplicateCapability),
        "duplicate capability rejected");
  const auto catalog = lp::ValidateCatalog({partial, partial});
  Check(HasIssue(catalog, lp::ValidationCode::kDuplicateLanguageIdentity),
        "duplicate language identity rejected");
  Check(HasIssue(catalog, lp::ValidationCode::kDuplicateSchemaIdentity),
        "duplicate schema identity rejected");
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::string mode = argv[1];
  if (mode == "--forcefail") Check(false, "intentional falsifiability control");
  else if (mode == "--unit") Tests();
  else return 2;
  if (failed != 0) return 1;
  std::cout << "RESULT PASS\n";
  return 0;
}
