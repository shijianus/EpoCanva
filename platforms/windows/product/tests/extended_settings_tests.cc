#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "product_settings.h"
#include "product_learning.h"
#include "engine/data_store/windows_learning_repository.h"

#include <cstdint>

namespace {
int failures = 0;
void Check(bool condition, const char* label) {
  if (!condition) { std::cerr << "FAIL " << label << '\n'; ++failures; }
}
void Put(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << bytes;
  Check(file.good(), "settings fixture write");
}
}  // namespace

int main(int argc, char** argv) {
  using namespace epocanva::product;
  if (argc != 2) return 2;
  const auto root = std::filesystem::u8path(argv[1]);
  std::filesystem::create_directories(root);
  const auto path = root / "extended-settings.ini";
  const std::string legacy = "[settings]\nformat=1\ncompletion_enabled=1\n"
      "completion_candidates=7\nspelling_enabled=0\nspelling_candidates=3\n";
  Put(path, legacy);
  const auto old = LoadProductSettings(path);
  Check(old.ok() && !old.settings.morphology_enabled &&
        old.settings.abbreviation_enabled && !old.settings.two_edit_spelling_enabled,
        "legacy settings keep expansion on and the noisier features opt-in");
  Check(old.ok() && !old.settings.learning_recording_enabled,
        "legacy settings default durable recording off");
  Check(!ProductLearningWritesPermitted(),
        "durable product learning stays closed without a proven field lock");
  {
    ProductSettings wanted;
    wanted.learning_recording_enabled = true;
    Put(path, BuildSettingsText(wanted));
    const auto loaded = LoadProductSettings(path);
    Check(loaded.ok() && loaded.settings.learning_recording_enabled,
          "learning recording intent round trips");
  }
  Check(ProductSettings{}.abbreviation_enabled,
        "a fresh install enables contextual abbreviation expansion");
  for (unsigned mask = 0; mask < 8; ++mask) {
    ProductSettings wanted;
    wanted.morphology_enabled = (mask & 1) != 0;
    wanted.abbreviation_enabled = (mask & 2) != 0;
    wanted.two_edit_spelling_enabled = (mask & 4) != 0;
    Put(path, BuildSettingsText(wanted));
    const auto loaded = LoadProductSettings(path);
    Check(loaded.ok() && loaded.settings.morphology_enabled == wanted.morphology_enabled &&
          loaded.settings.abbreviation_enabled == wanted.abbreviation_enabled &&
          loaded.settings.two_edit_spelling_enabled == wanted.two_edit_spelling_enabled,
          "all feature combinations round trip");
    const auto patch = BuildSchemaPatch(loaded.settings);
    Check(patch.find(std::string("\"english/enable_morphology\": ") +
          (wanted.morphology_enabled ? "true\n" : "false\n")) != patch.npos,
          "word form switch projected");
    Check(patch.find(std::string("\"english/enable_abbreviation\": ") +
          (wanted.abbreviation_enabled ? "true\n" : "false\n")) != patch.npos,
          "expansion switch projected");
    Check(patch.find(std::string("\"english/enable_two_edit_spelling\": ") +
          (wanted.two_edit_spelling_enabled ? "true\n" : "false\n")) != patch.npos,
          "two-edit switch projected");
  }
  for (const auto* key : {"morphology_enabled", "abbreviation_enabled",
                          "two_edit_spelling_enabled", "learning_recording_enabled"}) {
    Put(path, legacy + key + "=true\n");
    Check(LoadProductSettings(path).status == SettingsLoadStatus::kCorrupt,
          "malformed flag rejected");
    Put(path, legacy + key + "=1\n" + key + "=0\n");
    Check(LoadProductSettings(path).status == SettingsLoadStatus::kCorrupt,
          "duplicate flag rejected");
  }
  {
    const auto learning_root = root / "learning-clear";
    std::filesystem::create_directories(learning_root);
    ProductPaths paths;
    paths.user_root = learning_root;
    paths.learning_dir = learning_root / "learning-v2";
    epocanva::learning::Catalog catalog;
    catalog.name_space = "product.test.v1";
    catalog.sha256 = std::string(64, 'a');
    catalog.admitted_ids = {1};
    catalog.profile_id = "1";
    catalog.language = "en";
    epocanva::learning::WindowsLearningRepository repository(learning_root);
    epocanva::learning::Access access{epocanva::learning::PrivacyClass::kNormal, true, 7, 9,
                                      epocanva::learning::CurrentTickMilliseconds()};
    const epocanva::learning::PrivacyCallback allow = [access] { return access; };
    Check(repository.RecordFeedback(catalog,
          epocanva::learning::FeedbackKind::kManualWordAdded, "manual:hello", "hello", {},
          false, 0, true, allow) == epocanva::learning::Status::kOk,
          "learning fixture persists");
    Check(LocalLearningDataPresent(paths), "learning data is detected");
    std::wstring why;
    Check(ClearAllLocalLearning(paths, &why), "explicit clear removes learning data");
    Check(!LocalLearningDataPresent(paths), "clear leaves only empty locks");
  }
  if (failures) return 1;
  std::cout << "PASS extended settings contract\n";
  return 0;
}
