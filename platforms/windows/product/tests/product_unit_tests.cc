// EpoCanva W1 -- product core tests: settings, atomic files, manifest, paths, digest.
//
// These tests never start the engine and never touch the real install or user profile.
// Every file lives below --run-root, which CTest points at build/product/run/unit.
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "product_io.h"
#include "product_manifest.h"
#include "product_paths.h"
#include "product_settings.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& what) {
  if (!condition) {
    std::cout << "FAIL: " << what << "\n";
    ++g_failures;
  }
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

std::filesystem::path PrepareRoot(const std::filesystem::path& run_root,
                                  const char* name) {
  const std::filesystem::path root = run_root / name;
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  return root;
}

void TestDigest() {
  std::string digest;
  std::wstring why;
  const std::string abc = "abc";
  Check(epocanva::product::Sha256(abc.data(), abc.size(), &digest, &why),
        "SHA-256 of abc succeeds");
  Check(digest ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA-256 of abc matches the published vector");
  Check(epocanva::product::Sha256(nullptr, 0, &digest, &why), "SHA-256 of empty");
  Check(digest ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "SHA-256 of empty matches the published vector");
  Check(epocanva::product::IsLowercaseHexSha256(digest), "digest is lowercase hex");
  Check(!epocanva::product::IsLowercaseHexSha256("ABC"), "short digest rejected");
}

void TestSettingsRoundTrip(const std::filesystem::path& root) {
  epocanva::product::ProductPaths paths;
  paths.user_root = root / L"用户 数据";
  paths.engine_user_dir = paths.user_root / L"engine";
  paths.log_dir = paths.user_root / L"logs";
  paths.settings_file = paths.user_root / L"settings.ini";
  paths.schema_patch_file = paths.engine_user_dir / L"epocanva_english.custom.yaml";
  paths.deploy_marker_file = paths.engine_user_dir / L"deploy-required";

  auto missing = epocanva::product::LoadProductSettings(paths.settings_file);
  Check(missing.status == epocanva::product::SettingsLoadStatus::kMissing,
        "missing settings reports kMissing");
  Check(missing.settings.completion_enabled && !missing.settings.spelling_enabled,
        "defaults are completion on, spelling off");

  epocanva::product::ProductSettings wanted;
  wanted.completion_enabled = false;
  wanted.completion_candidates = 5;
  wanted.spelling_enabled = true;
  wanted.spelling_candidates = 2;
  std::string digest;
  std::wstring why;
  Check(epocanva::product::SaveSettingsBundle(paths, wanted, &digest, &why),
        "SaveSettingsBundle succeeds in a non-ASCII path");
  Check(epocanva::product::IsLowercaseHexSha256(digest),
        "saved settings digest is 64 lowercase hex characters");

  auto loaded = epocanva::product::LoadProductSettings(paths.settings_file);
  Check(loaded.status == epocanva::product::SettingsLoadStatus::kLoaded,
        "saved settings load as kLoaded");
  Check(!loaded.settings.completion_enabled && loaded.settings.completion_candidates == 5,
        "completion values round-trip");
  Check(loaded.settings.spelling_enabled && loaded.settings.spelling_candidates == 2,
        "spelling values round-trip");

  const std::string patch = ReadText(paths.schema_patch_file);
  Check(patch.find("english/max_completions\": 0") != std::string::npos,
        "disabled completion is projected as max_completions 0");
  Check(patch.find("english/enable_suggestions\": true") != std::string::npos,
        "spelling is projected as enabled");
  Check(patch.find("english/max_suggestions\": 2") != std::string::npos,
        "spelling count is projected");

  std::string token;
  Check(epocanva::product::ReadDeployMarkerToken(paths.deploy_marker_file, &token),
        "deploy marker carries a token");
  Check(token == digest, "deploy marker token is the settings digest");
  Check(!epocanva::product::ClearDeployMarkerIfToken(paths.deploy_marker_file,
                                                     std::string(64, '0')),
        "a newer marker is not cleared by an old token");
  Check(std::filesystem::is_regular_file(paths.deploy_marker_file),
        "the newer marker still exists");
  Check(epocanva::product::ClearDeployMarkerIfToken(paths.deploy_marker_file, token),
        "the matching marker is cleared");
  Check(!std::filesystem::exists(paths.deploy_marker_file),
        "the cleared marker is gone");

  // The patch and settings live only under the user root; the product tree is untouched.
  int files_in_user = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(paths.user_root)) {
    if (entry.is_regular_file()) {
      ++files_in_user;
    }
  }
  Check(files_in_user == 2, "only settings.ini and the patch were written");

  {
    std::ofstream garbage(paths.settings_file, std::ios::binary | std::ios::trunc);
    garbage << "this is not an ini\n";
  }
  auto corrupt = epocanva::product::LoadProductSettings(paths.settings_file);
  Check(corrupt.status == epocanva::product::SettingsLoadStatus::kCorrupt,
        "garbage settings report kCorrupt");
  Check(corrupt.settings.completion_candidates == 7,
        "corrupt settings fall back to defaults");

  {
    std::ofstream future(paths.settings_file, std::ios::binary | std::ios::trunc);
    future << "[settings]\nformat=2\n";
  }
  auto unsupported = epocanva::product::LoadProductSettings(paths.settings_file);
  Check(unsupported.status == epocanva::product::SettingsLoadStatus::kUnsupportedVersion,
        "a future format reports kUnsupportedVersion");

  {
    std::ofstream bad_value(paths.settings_file, std::ios::binary | std::ios::trunc);
    bad_value << "[settings]\nformat=1\ncompletion_enabled=1\n"
                 "completion_candidates=9\nspelling_enabled=0\nspelling_candidates=3\n";
  }
  auto bad = epocanva::product::LoadProductSettings(paths.settings_file);
  Check(bad.status == epocanva::product::SettingsLoadStatus::kCorrupt,
        "out-of-range candidates report kCorrupt");

  Check(epocanva::product::SaveSettingsBundle(paths, wanted, &digest, &why),
        "a save repairs the corrupt file");
  auto repaired = epocanva::product::LoadProductSettings(paths.settings_file);
  Check(repaired.status == epocanva::product::SettingsLoadStatus::kLoaded,
        "repaired settings load cleanly");
}

void TestAtomicWrite(const std::filesystem::path& root) {
  const std::filesystem::path file = root / L"原子 写.txt";
  std::wstring why;
  Check(epocanva::product::WriteTextAtomically(file, "first\n", &why),
        "atomic write creates a new file");
  Check(ReadText(file) == "first\n", "first write is readable");
  Check(epocanva::product::WriteTextAtomically(file, "second\n", &why),
        "atomic write replaces the file");
  Check(ReadText(file) == "second\n", "second write is readable");
  int temporaries = 0;
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("epc", 0) == 0 && name.size() > 3 &&
        name.substr(name.size() - 4) == ".tmp") {
      ++temporaries;
    }
  }
  Check(temporaries == 0, "no temporary file is left behind");

  // A reader that denies delete/write (no FILE_SHARE_DELETE) blocks MoveFileEx with
  // ERROR_ACCESS_DENIED. The save must survive that short window instead of failing the
  // user's settings change while the engine is closing the previous file.
  const HANDLE holder =
      ::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(holder != INVALID_HANDLE_VALUE, "a transient reader opened the destination");
  if (holder != INVALID_HANDLE_VALUE) {
    std::thread closer([holder]() {
      ::Sleep(250);
      ::CloseHandle(holder);
    });
    const bool replaced = epocanva::product::WriteTextAtomically(file, "after-lock\n", &why);
    closer.join();
    Check(replaced, "atomic write retries through a transient sharing lock");
    Check(ReadText(file) == "after-lock\n", "the retried write is the final content");
  }
}

void TestManifest(const std::filesystem::path& root) {
  const std::filesystem::path package = root / L"package";
  std::error_code ec;
  std::filesystem::create_directories(package / "data", ec);
  std::wstring why;
  Check(epocanva::product::WriteTextAtomically(package / "EpoCanva.exe", "exe\n", &why),
        "dummy exe written");
  Check(epocanva::product::WriteTextAtomically(package / "data" / "x.txt", "x\n", &why),
        "dummy data written");
  std::string exe_hash;
  std::string data_hash;
  std::uint64_t exe_size = 0;
  std::uint64_t data_size = 0;
  Check(epocanva::product::Sha256File(package / "EpoCanva.exe", &exe_hash, &exe_size,
                                      &why),
        "hash dummy exe");
  Check(epocanva::product::Sha256File(package / "data" / "x.txt", &data_hash, &data_size,
                                      &why),
        "hash dummy data");

  auto manifest_text = [&]() {
    return std::string("epocanva-package-manifest 1\nversion=0.1.0-w1\ncommit=abc1234\n"
                       "file\tEpoCanva.exe\t") + exe_hash + "\t" +
           std::to_string(exe_size) + "\nfile\tdata/x.txt\t" + data_hash + "\t" +
           std::to_string(data_size) + "\n";
  };
  const std::filesystem::path manifest_path = root / "package.manifest";
  Check(epocanva::product::WriteTextAtomically(manifest_path, manifest_text(), &why),
        "manifest written");

  epocanva::product::ProductManifest manifest;
  Check(epocanva::product::LoadProductManifest(manifest_path, &manifest, &why),
        "manifest loads");
  Check(manifest.format == 1 && manifest.files.size() == 2 &&
            manifest.version == "0.1.0-w1",
        "manifest header and entries are parsed");
  std::vector<epocanva::product::FileVerification> files;
  int failures = -1;
  Check(epocanva::product::VerifyProductManifest(package, manifest, &files, &failures,
                                                 &why),
        "manifest verification scans");
  Check(failures == 0 && files.size() == 2, "both manifest entries verify");
  Check(epocanva::product::FindManifestEntry(manifest, "data/x.txt") != nullptr,
        "entry lookup finds data/x.txt");

  Check(epocanva::product::WriteTextAtomically(package / "data" / "x.txt", "changed\n",
                                               &why),
        "tamper with data/x.txt");
  Check(epocanva::product::VerifyProductManifest(package, manifest, &files, &failures,
                                                 &why),
        "tampered package still scans");
  Check(failures == 1, "one tampered file is counted");

  auto expect_load_failure = [&](const std::string& text, const char* what) {
    Check(epocanva::product::WriteTextAtomically(manifest_path, text, &why),
          std::string("write malformed manifest: ") + what);
    epocanva::product::ProductManifest rejected;
    Check(!epocanva::product::LoadProductManifest(manifest_path, &rejected, &why),
          std::string("reject malformed manifest: ") + what);
  };
  expect_load_failure("not-a-manifest\n", "bad header");
  expect_load_failure("epocanva-package-manifest 1\nfile\t../escape\t" + exe_hash +
                          "\t1\n",
                      "path traversal");
  expect_load_failure("epocanva-package-manifest 1\nfile\tEpoCanva.exe\t" + exe_hash +
                          "\t1\nfile\tEpoCanva.exe\t" + exe_hash + "\t1\n",
                      "duplicate path");
  expect_load_failure("epocanva-package-manifest 1\nfile\tEpoCanva.exe\t" +
                          std::string(63, 'a') + "\t1\n",
                      "short digest");
  expect_load_failure("epocanva-package-manifest 1\nfile\tEpoCanva.exe\t" + exe_hash +
                          "\tnot-a-number\n",
                      "bad byte count");
}

void TestPaths(const std::filesystem::path& root) {
  const std::filesystem::path user_root = root / L"用户 数据";
  epocanva::product::SetUserRootOverrideForProcess(user_root);
  epocanva::product::ProductPaths paths;
  std::wstring why;
  Check(epocanva::product::ResolveProductPaths(nullptr, &paths, &why),
        "resolve product paths with an override");
  Check(paths.user_root == user_root, "user root honours the override");
  Check(paths.settings_file == user_root / L"settings.ini",
        "settings file is below the user root");
  Check(paths.engine_user_dir == user_root / L"engine",
        "engine user data is below the user root");
  Check(paths.product_root == paths.module_path.parent_path(),
        "product root is the module directory, not the working directory");
  Check(epocanva::product::EnsureUserDirectories(paths, &why),
        "user directories are created");
  Check(epocanva::product::ProbeUserDataWritable(paths, &why),
        "the user root is writable");

  epocanva::product::SetUserRootOverrideForProcess({});
  ::SetEnvironmentVariableW(L"EPOCANVA_USER_DATA_ROOT", user_root.c_str());
  epocanva::product::ProductPaths from_env;
  Check(epocanva::product::ResolveProductPaths(nullptr, &from_env, &why),
        "resolve product paths from the environment override");
  Check(from_env.user_root == user_root, "environment override is honoured");
  ::SetEnvironmentVariableW(L"EPOCANVA_USER_DATA_ROOT", L"relative-not-allowed");
  epocanva::product::ProductPaths rejected;
  Check(!epocanva::product::ResolveProductPaths(nullptr, &rejected, &why),
        "relative environment override is rejected");
  ::SetEnvironmentVariableW(L"EPOCANVA_USER_DATA_ROOT", nullptr);

  Check(epocanva::product::WideToUtf8(L"中文路径 / café") ==
            "\xE4\xB8\xAD\xE6\x96\x87\xE8\xB7\xAF\xE5\xBE\x84 / caf\xC3\xA9",
        "wide to UTF-8 round-trips non-ASCII text");
  Check(epocanva::product::Utf8ToWide(epocanva::product::WideToUtf8(L"中文")) == L"中文",
        "UTF-8 to wide round-trips Chinese");
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path run_root;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--run-root" && index + 1 < argc) {
      run_root = std::filesystem::path(argv[++index]);
    }
  }
  if (run_root.empty() || !run_root.is_absolute()) {
    std::cout << "FAIL: --run-root <absolute path> is required\n";
    return 2;
  }
  const std::filesystem::path root = PrepareRoot(run_root, "unit");
  if (root.empty()) {
    std::cout << "FAIL: cannot prepare the run root\n";
    return 2;
  }

  TestDigest();
  TestSettingsRoundTrip(root / "settings");
  {
    std::error_code ec;
    std::filesystem::create_directories(root / "atomic", ec);
  }
  TestAtomicWrite(root / "atomic");
  TestManifest(root / "manifest");
  TestPaths(root / "paths");

  if (g_failures != 0) {
    std::cout << "FAILED: " << g_failures << " check(s)\n";
    return 14;
  }
  std::cout << "RESULT PASS\n";
  return 0;
}
