#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "product_settings.h"

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
                          "two_edit_spelling_enabled"}) {
    Put(path, legacy + key + "=true\n");
    Check(LoadProductSettings(path).status == SettingsLoadStatus::kCorrupt,
          "malformed flag rejected");
    Put(path, legacy + key + "=1\n" + key + "=0\n");
    Check(LoadProductSettings(path).status == SettingsLoadStatus::kCorrupt,
          "duplicate flag rejected");
  }
  if (failures) return 1;
  std::cout << "PASS extended settings contract\n";
  return 0;
}
