#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "rime_runtime.h"

namespace {
namespace engine = epocanva::windows::engine;
int failures = 0;
void Check(bool condition, const char* label) {
  if (!condition) { std::cerr << "FAIL " << label << '\n'; ++failures; }
}
void Run(const std::filesystem::path& dll, const std::filesystem::path& root,
         const std::vector<std::string>& modules, engine::RuntimeStartStatus expected,
         bool expect_english) {
  std::filesystem::create_directories(root / "shared");
  std::filesystem::create_directories(root / "user");
  std::filesystem::create_directories(root / "logs");
  engine::RuntimeOptions options;
  options.rime_dll_path = dll;
  options.shared_data_dir = root / "shared";
  options.user_data_dir = root / "user";
  options.log_dir = root / "logs";
  options.required_modules = modules;
  engine::RimeRuntime runtime;
  const auto status = runtime.Start(options);
  Check(status == expected, "runtime module start status");
  const auto info = runtime.Info();
  if (expected == engine::RuntimeStartStatus::kInvalidOptions) {
    Check(info.initialize_calls == 0 && info.finalize_calls == 0,
          "invalid requirements never initialize backend");
  }
  if (status == engine::RuntimeStartStatus::kOk) {
    Check(info.base_modules_registered && info.english_module_loaded == expect_english,
          "real loaded modules match requirements");
    Check(runtime.Shutdown() == engine::RuntimeShutdownStatus::kOk, "module runtime shutdown");
    Check(runtime.Info().finalize_calls == 1, "module runtime finalized exactly once");
  }
}
}  // namespace
int main(int argc, char** argv) {
  if (argc != 4) return 2;
  const auto english = std::filesystem::absolute(std::filesystem::u8path(argv[1]));
  const auto baseline = std::filesystem::absolute(std::filesystem::u8path(argv[2]));
  const auto root = std::filesystem::absolute(std::filesystem::u8path(argv[3]));
  Run(english, root / "default-english", {"epocanva_english"}, engine::RuntimeStartStatus::kOk, true);
  Run(baseline, root / "baseline-base", {"default"}, engine::RuntimeStartStatus::kOk, false);
  Run(baseline, root / "baseline-explicit-base", {"core", "dict", "gears"},
      engine::RuntimeStartStatus::kOk, false);
  Run(baseline, root / "baseline-missing-english", {"epocanva_english"},
      engine::RuntimeStartStatus::kEnglishModuleMissing, false);
  Run(english, root / "empty", {}, engine::RuntimeStartStatus::kInvalidOptions, false);
  Run(english, root / "duplicate", {"default", "default"}, engine::RuntimeStartStatus::kInvalidOptions, false);
  Run(english, root / "unapproved", {"external_plugin"}, engine::RuntimeStartStatus::kInvalidOptions, false);
  Run(english, root / "unsafe", {"../default"}, engine::RuntimeStartStatus::kInvalidOptions, false);
  if (failures) return 1;
  std::cout << "PASS real runtime module requirements\n";
  return 0;
}
