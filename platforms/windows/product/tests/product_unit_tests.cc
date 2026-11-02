// EpoCanva W1 -- product core tests: settings, atomic files, manifest, paths, digest.
//
// These tests never start the engine and never touch the real install or user profile.
// Every file lives below --run-root, which CTest points at build/product/run/unit.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "product_diagnostics.h"
#include "product_io.h"
#include "product_manifest.h"
#include "product_paths.h"
#include "product_settings.h"
#include "../../local_path/local_path.h"
#include <winioctl.h>
#include <cstring>

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
  Check(missing.settings.candidate_display ==
            epocanva::product::CandidateDisplay::kHost,
        "the default candidate display follows the host");

  epocanva::product::ProductSettings wanted;
  wanted.completion_enabled = false;
  wanted.completion_candidates = 5;
  wanted.spelling_enabled = true;
  wanted.spelling_candidates = 2;
  wanted.candidate_display = epocanva::product::CandidateDisplay::kAlwaysSelf;
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
  Check(loaded.settings.candidate_display ==
            epocanva::product::CandidateDisplay::kAlwaysSelf,
        "the always_self candidate display round-trips");
  Check(ReadText(paths.settings_file).find("candidate_display=always_self") !=
            std::string::npos,
        "the settings file records candidate_display=always_self");

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

  {
    // A settings file written before W3-B has no candidate_display key. It must keep
    // loading instead of being rejected as corrupt, and must follow the host.
    std::ofstream legacy(paths.settings_file, std::ios::binary | std::ios::trunc);
    legacy << "[settings]\nformat=1\ncompletion_enabled=1\n"
              "completion_candidates=7\nspelling_enabled=0\nspelling_candidates=3\n";
  }
  auto legacy = epocanva::product::LoadProductSettings(paths.settings_file);
  Check(legacy.status == epocanva::product::SettingsLoadStatus::kLoaded &&
            legacy.settings.candidate_display ==
                epocanva::product::CandidateDisplay::kHost,
        "a pre-W3-B settings file loads and follows the host");

  {
    std::ofstream bad_display(paths.settings_file, std::ios::binary | std::ios::trunc);
    bad_display << "[settings]\nformat=1\ncompletion_enabled=1\n"
                   "completion_candidates=7\nspelling_enabled=0\nspelling_candidates=3\n"
                   "candidate_display=sometimes\n";
  }
  auto bad_display = epocanva::product::LoadProductSettings(paths.settings_file);
  Check(bad_display.status == epocanva::product::SettingsLoadStatus::kCorrupt,
        "an unknown candidate_display value reports kCorrupt");

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
                       "internal-only=0\nfile\tEpoCanva.exe\t") + exe_hash + "\t" +
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
  expect_load_failure(manifest_text() + "version=other\n", "duplicate version key");
  expect_load_failure(manifest_text() + "commit=other\n", "duplicate commit key");
  expect_load_failure(manifest_text() + "internal-only=1\n", "duplicate policy key");
  expect_load_failure(manifest_text() + "file\tepocanva.EXE\t" + exe_hash + "\t1\n",
                      "case-insensitive duplicate path");
  std::string missing_policy = manifest_text();
  missing_policy.erase(missing_policy.find("internal-only=0\n"), std::string("internal-only=0\n").size());
  expect_load_failure(missing_policy, "missing explicit internal-only policy");
  Check(!epocanva::product::ValidateProductAcceptancePolicy(manifest, &why),
        "format 1 loads for diagnosis but cannot be ready");

  const std::string rows = "file\tEpoCanva.exe\t" + exe_hash + "\t" +
      std::to_string(exe_size) + "\nfile\tdata/x.txt\t" + data_hash + "\t" +
      std::to_string(data_size) + "\n";
  const std::string metadata = "epocanva-package-manifest 2\nversion=0.1.0-w1\ncommit=abc1234\n";
  const std::string policies = "acceptance-profile=internal-unsigned\n"
      "distribution-policy=internal-only\npath-exception=none\n";
  const std::string format2 = metadata + policies + rows;
  Check(epocanva::product::WriteTextAtomically(manifest_path, format2, &why), "write format 2 manifest");
  Check(epocanva::product::LoadProductManifest(manifest_path, &manifest, &why), "format 2 loads");
  Check(manifest.format == 2 && manifest.acceptance_profile == "internal-unsigned" &&
        manifest.distribution_policy == "internal-only" && manifest.path_exception == "none" &&
        epocanva::product::ValidateProductAcceptancePolicy(manifest, &why),
        "internal unsigned and strict paths are independent of public distribution");
  for (const std::string key : {"acceptance-profile=internal-unsigned\n",
                               "distribution-policy=internal-only\n", "path-exception=none\n"}) {
    expect_load_failure(format2 + key, "duplicate format 2 policy");
    std::string missing = format2;
    missing.erase(missing.find(key), key.size());  // UTF-8 code-unit (byte) offsets
    expect_load_failure(missing, "missing format 2 policy");
  }
  expect_load_failure(metadata + "acceptance-profile=true\ndistribution-policy=internal-only\npath-exception=none\n" + rows,
                      "boolean is not an acceptance profile");
  expect_load_failure(format2 + "internal-only=0\n", "mixed legacy and new metadata");
  expect_load_failure(manifest_text() + policies, "new policy on legacy header");
  manifest.path_exception = "embedded-build-paths";
  Check(!epocanva::product::ValidateProductAcceptancePolicy(manifest, &why),
        "internal unsigned cannot waive embedded build paths");
  manifest.path_exception = "none";
  manifest.acceptance_profile = "public-signed";
  Check(!epocanva::product::ValidateProductAcceptancePolicy(manifest, &why),
        "public acceptance requires public candidate metadata");
  manifest.distribution_policy = "public-candidate";
  Check(epocanva::product::ValidateProductAcceptancePolicy(manifest, &why),
        "public candidate policy parses without claiming authorization or signatures");
}

void TestProductPayloadPolicy(const std::filesystem::path& root,
                              const std::filesystem::path& version_file) {
  using namespace epocanva::product;
  // The VERSION argument is already supplied by the existing CTest registration.
  const auto repo = version_file.parent_path().parent_path().parent_path().parent_path();
  const auto package = root / "package";
  const auto user = root / "user";  // outside the package, as in a real installation
  ProductManifest manifest;
  manifest.format = 2;
  manifest.version = WideToUtf8(ProductVersion());
  manifest.commit = WideToUtf8(ProductCommit());
  manifest.acceptance_profile = "internal-unsigned";
  manifest.distribution_policy = "internal-only";
  manifest.path_exception = "none";
  std::wstring why;
  for (const auto& required : RequiredProductFiles()) {
    const auto destination = package / std::filesystem::u8path(required.relative_path);
    const std::string text = required.source[0] == '\0'
        ? std::string("test payload for ") + required.relative_path + "\n"
        : ReadText(repo / std::filesystem::u8path(required.source));
    Check(!text.empty(), std::string("required fixture source exists: ") + required.relative_path);
    Check(WriteTextAtomically(destination, text, &why), "copy required fixture payload");
    ManifestEntry entry;
    entry.relative_path = required.relative_path;
    Check(Sha256File(destination, &entry.sha256, &entry.bytes, &why), "hash fixture payload");
    manifest.files.push_back(entry);
  }
  auto write_manifest = [&]() {
    std::string text = "epocanva-package-manifest " + std::to_string(manifest.format) +
        "\nversion=" + manifest.version + "\ncommit=" + manifest.commit + "\n";
    if (manifest.format == 1) {
      text += std::string("internal-only=") + (manifest.internal_only ? "1\n" : "0\n");
    } else {
      text += "acceptance-profile=" + manifest.acceptance_profile +
          "\ndistribution-policy=" + manifest.distribution_policy +
          "\npath-exception=" + manifest.path_exception + "\n";
    }
    for (const auto& entry : manifest.files) {
      text += "file\t" + entry.relative_path + "\t" + entry.sha256 + "\t" +
              std::to_string(entry.bytes) + "\n";
    }
    Check(WriteTextAtomically(package / "package.manifest", text, &why), "write product fixture manifest");
  };
  auto collect = [&]() {
    SetUserRootOverrideForProcess(user);
    ProductPaths paths;
    Check(ResolveProductPaths(nullptr, &paths, &why), "resolve isolated status paths");
    SetUserRootOverrideForProcess({});
    paths.product_root = package;
    paths.licenses_dir = package / "licenses";
    paths.manifest_file = package / "package.manifest";
    ProductStatus status;
    Check(CollectProductStatusForPaths(paths, &status, &why), "collect isolated package status");
    return status;
  };
  write_manifest();
  Check(ValidateProductPayloadPolicy(manifest, &why), "exact product payload policy accepts fixture");
  Check(VerifyProductFileSet(package, manifest, &why), "exact on-disk product file set accepted");
  Check(collect().package_ready, "complete package reports ready");
  Check(WriteTextAtomically(user / "engine" / "deploy-only.bin", "user deployment\n", &why),
        "create user deployment file outside package");
  Check(collect().package_ready, "user deployment files do not pollute package verification");

  const ProductManifest complete = manifest;
  for (std::size_t entry_index = 0; entry_index < complete.files.size(); ++entry_index) {
    manifest = complete;
    const auto entry = manifest.files[entry_index];
    const auto file = package / std::filesystem::u8path(entry.relative_path);
    const std::string original = ReadText(file);
    std::error_code ec;
    std::filesystem::remove(file, ec);
    Check(!collect().package_ready, "missing required file rejected: " + entry.relative_path);
    manifest.files.erase(manifest.files.begin() + entry_index);
    write_manifest();
    Check(!ValidateProductPayloadPolicy(manifest, &why),
          "deleted manifest row rejected: " + entry.relative_path);
    Check(!collect().package_ready, "removing both file and manifest row cannot hide omission");
    Check(WriteTextAtomically(file, original, &why), "restore required file");
    manifest = complete;
    write_manifest();
  }

  // Alter every fixed-content file while also updating its manifest digest. Checking
  // only the mutable manifest would accept each of these packages.
  for (const auto& required : RequiredProductFiles()) {
    if (required.sha256[0] == '\0') { continue; }
    manifest = complete;
    const auto file = package / std::filesystem::u8path(required.relative_path);
    const std::string original = ReadText(file);
    std::string changed = original;
    changed[0] = changed[0] == 'X' ? 'Y' : 'X';
    Check(WriteTextAtomically(file, changed, &why), "change fixed payload without changing byte count");
    auto found = std::find_if(manifest.files.begin(), manifest.files.end(),
        [&required](const ManifestEntry& entry) { return entry.relative_path == required.relative_path; });
    Check(Sha256File(file, &found->sha256, &found->bytes, &why), "rehash altered fixed payload");
    write_manifest();
    Check(!collect().package_ready,
          std::string("rewriting manifest cannot bless changed fixed content: ") + required.relative_path);
    Check(WriteTextAtomically(file, original, &why), "restore fixed payload");
  }
  manifest = complete;
  write_manifest();
  const auto extra = package / "unlisted.dll";
  Check(WriteTextAtomically(extra, "unlisted\n", &why), "inject extra package file");
  Check(!collect().package_ready, "unlisted file is rejected");
  ManifestEntry extra_entry;
  extra_entry.relative_path = "unlisted.dll";
  Check(Sha256File(extra, &extra_entry.sha256, &extra_entry.bytes, &why), "hash extra file");
  manifest.files.push_back(extra_entry);
  write_manifest();
  Check(!collect().package_ready, "listing an extra file still violates exact policy");
  std::error_code ec;
  std::filesystem::remove(extra, ec);
  manifest = complete;
  manifest.commit += "-wrong";
  write_manifest();
  Check(!collect().package_ready, "commit mismatch is an error, not a warning");
  manifest = complete;
  manifest.path_exception = "embedded-build-paths";
  write_manifest();
  const auto internal = collect();
  Check(!internal.package_ready && internal.internal_only,
        "path exception cannot report ready even for internal acceptance");
  manifest = complete;
  manifest.format = 1;
  manifest.internal_only = false;
  write_manifest();
  const auto legacy = collect();
  Check(legacy.manifest_ok && !legacy.package_ready && legacy.manifest_format == 1,
        "legacy strict-path manifest remains diagnostic-only");
  manifest = complete;
  write_manifest();
  const auto restored = collect();
  Check(restored.package_ready && restored.internal_only, "strict internal format 2 reports local integrity ready");
  const auto report = FormatProductReport(restored);
  Check(report.find("acceptance_profile=internal-unsigned\n") != std::string::npos &&
        report.find("distribution_policy=internal-only\n") != std::string::npos &&
        report.find("path_exception=none\n") != std::string::npos &&
        report.find("readiness_scope=local-package-integrity-only\n") != std::string::npos,
        "diagnosis distinguishes local integrity from release acceptance");
}

void TestVersionAndIcon(const std::filesystem::path& exe,
                        const std::filesystem::path& dll,
                        const std::filesystem::path& version_file) {
  std::string version = ReadText(version_file);
  while (!version.empty() &&
         (version.back() == '\n' || version.back() == '\r' || version.back() == ' ')) {
    version.pop_back();
  }
  Check(version == "0.2.6-w3b", "VERSION is the W3-B internal version");
  unsigned major = 0;
  unsigned minor = 0;
  unsigned patch = 0;
  if (std::sscanf(version.c_str(), "%u.%u.%u", &major, &minor, &patch) != 3) {
    Check(false, "VERSION has a numeric major.minor.patch prefix");
    return;
  }
  auto fixed_version = [](const std::filesystem::path& binary, unsigned* m,
                          unsigned* n, unsigned* p) {
    DWORD size = ::GetFileVersionInfoSizeW(binary.c_str(), nullptr);
    if (size == 0) {
      return false;
    }
    std::vector<BYTE> data(size);
    if (!::GetFileVersionInfoW(binary.c_str(), 0, size, data.data())) {
      return false;
    }
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!::VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) ||
        info == nullptr || length < sizeof(VS_FIXEDFILEINFO) ||
        info->dwSignature != 0xFEEF04BD) {
      return false;
    }
    *m = HIWORD(info->dwFileVersionMS);
    *n = LOWORD(info->dwFileVersionMS);
    *p = HIWORD(info->dwFileVersionLS);
    return true;
  };
  for (const auto& binary : {exe, dll}) {
    unsigned m = 0;
    unsigned n = 0;
    unsigned p = 0;
    const bool ok = fixed_version(binary, &m, &n, &p);
    const std::string name = binary.filename().string();
    Check(ok && m == major && n == minor && p == patch,
          name + " FILEVERSION matches VERSION (" + std::to_string(major) + "." +
              std::to_string(minor) + "." + std::to_string(patch) + ")");
    if (ok) {
      DWORD size = ::GetFileVersionInfoSizeW(binary.c_str(), nullptr);
      std::vector<BYTE> data(size);
      if (size != 0 && ::GetFileVersionInfoW(binary.c_str(), 0, size, data.data())) {
        VS_FIXEDFILEINFO* info = nullptr;
        UINT length = 0;
        if (::VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info),
                             &length) &&
            info != nullptr) {
          Check(HIWORD(info->dwProductVersionMS) == major &&
                    LOWORD(info->dwProductVersionMS) == minor &&
                    HIWORD(info->dwProductVersionLS) == patch,
                name + " PRODUCTVERSION matches VERSION");
        }
      }
    }
    Check(::ExtractIconExW(binary.c_str(), -1, nullptr, nullptr, 0) > 0,
          name + " carries at least one icon resource");
  }
}

bool MakePathJunction(const std::filesystem::path& link, const std::filesystem::path& target) {
  struct Buffer {
    ULONG tag;
    USHORT data_bytes, reserved, substitute_offset_utf16_bytes, substitute_utf16_bytes;
    USHORT print_offset_utf16_bytes, print_utf16_bytes;
    WCHAR path[1];
  };
  if (!std::filesystem::create_directory(link)) return false;
  const std::wstring substitute = L"\\??\\" + target.native();
  const std::size_t name_bytes = substitute.size() * sizeof(wchar_t);
  std::vector<BYTE> bytes(offsetof(Buffer, path) + name_bytes + 2 * sizeof(wchar_t), 0);
  auto* buffer = reinterpret_cast<Buffer*>(bytes.data());
  buffer->tag = IO_REPARSE_TAG_MOUNT_POINT;
  buffer->data_bytes = static_cast<USHORT>(bytes.size() - 8);
  buffer->substitute_utf16_bytes = static_cast<USHORT>(name_bytes);
  buffer->print_offset_utf16_bytes = static_cast<USHORT>(name_bytes + sizeof(wchar_t));
  std::memcpy(buffer->path, substitute.data(), name_bytes);
  const HANDLE handle = ::CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  DWORD returned_bytes = 0;
  const BOOL ok = ::DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, bytes.data(),
      static_cast<DWORD>(bytes.size()), nullptr, 0, &returned_bytes, nullptr);
  ::CloseHandle(handle);
  return ok != FALSE;
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

  namespace local = epocanva::windows::local_path;
  for (const auto& unsafe : std::vector<std::filesystem::path>{
      L"\\\\not-a-server.invalid\\share", L"\\\\?\\C:\\unsafe", L"\\\\.\\pipe\\unsafe",
      L"C:relative", L"C:\\NUL.txt", L"C:\\safe\\file:stream", L"C:\\CONIN$",
      L"C:\\COM\u00b9.log", L"C:\\safe.\\child", root / L".." / L"escape",
      std::filesystem::path(std::wstring(L"C:\\bad") + wchar_t(0) + L"suffix"),
      std::filesystem::path(std::wstring(L"C:\\bad") + wchar_t(0xd800))}) {
    local::PreparedPath prepared;
    Check(!local::Prepare(unsafe, &prepared), "unsafe local path syntax rejected before filesystem work");
    epocanva::product::SetUserRootOverrideForProcess(unsafe);
    Check(!epocanva::product::ResolveProductPaths(nullptr, &rejected, &why),
          "unsafe process root override rejected");
    epocanva::product::SetUserRootOverrideForProcess({});
    if (unsafe.native().find(L'\0') == std::wstring::npos) {
      ::SetEnvironmentVariableW(L"EPOCANVA_USER_DATA_ROOT", unsafe.c_str());
      Check(!epocanva::product::ResolveProductPaths(nullptr, &rejected, &why),
            "unsafe environment root override rejected");
      ::SetEnvironmentVariableW(L"EPOCANVA_USER_DATA_ROOT", nullptr);
    }
  }
  Check(!local::PhysicalDiskDevice(L"\\Device\\Mup\\server\\share") &&
        !local::PhysicalDiskDevice(L"\\Device\\LanmanRedirector\\;Z:1\\server\\share") &&
        !local::PhysicalDiskDevice(L"\\??\\C:\\local-subst") &&
        local::PhysicalDiskDevice(L"\\Device\\HarddiskVolume12"),
        "remote and SUBST device mappings rejected without configuring any drive");
  Check(local::AllowedAttributes(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT,
                                IO_REPARSE_TAG_CLOUD_A, true),
        "actual OneDrive directory tag is not rejected as a redirect");
  Check(!local::AllowedAttributes(FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_OFFLINE,
                                 IO_REPARSE_TAG_CLOUD, false),
        "offline cloud files cannot be hydrated by admission");
  {
    local::PinnedPath pin;
    Check(pin.Open(user_root, true), "ordinary Unicode local directory pins");
    const auto moved = root / L"moved-user";
    Check(!::MoveFileExW(user_root.c_str(), moved.c_str(), 0),
          "pinned root cannot be renamed while a consumer holds it");
    const HANDLE writer = ::CreateFileW(user_root.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    Check(writer == INVALID_HANDLE_VALUE, "pinned directory denies reparse mutation handle");
    if (writer != INVALID_HANDLE_VALUE) ::CloseHandle(writer);
  }
  const auto target = root / L"junction-target";
  std::filesystem::create_directory(target);
  const auto junction = root / L"junction";
  Check(MakePathJunction(junction, target), "unprivileged junction fixture created");
  epocanva::product::SetUserRootOverrideForProcess(junction / L"missing");
  Check(!epocanva::product::ResolveProductPaths(nullptr, &rejected, &why),
        "missing override below a junction rejected before creation");
  epocanva::product::SetUserRootOverrideForProcess({});
  auto redirected = paths;
  redirected.user_root = junction / L"missing";
  redirected.engine_user_dir = redirected.user_root / L"engine";
  redirected.log_dir = redirected.user_root / L"logs";
  Check(!epocanva::product::EnsureUserDirectories(redirected, &why) &&
        !epocanva::product::ProbeUserDataWritable(redirected, &why),
        "creation and probe cannot traverse a junction");
  Check(!std::filesystem::exists(target / L"missing"), "rejected target remains untouched");
  Check(::RemoveDirectoryW(junction.c_str()) != FALSE, "junction fixture removed, not target");
  auto inconsistent = paths;
  inconsistent.log_dir = root / L"outside";
  Check(!epocanva::product::EnsureUserDirectories(inconsistent, &why),
        "manually supplied child outside the user root rejected");

  Check(epocanva::product::WideToUtf8(L"中文路径 / café") ==
            "\xE4\xB8\xAD\xE6\x96\x87\xE8\xB7\xAF\xE5\xBE\x84 / caf\xC3\xA9",
        "wide to UTF-8 round-trips non-ASCII text");
  Check(epocanva::product::Utf8ToWide(epocanva::product::WideToUtf8(L"中文")) == L"中文",
        "UTF-8 to wide round-trips Chinese");
}

}  // namespace

int main(int argc, char** argv) {
  std::filesystem::path run_root;
  std::filesystem::path exe;
  std::filesystem::path product_dll;
  std::filesystem::path version_file;
  bool paths_only = false;
  bool force_path_failure = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--run-root" && index + 1 < argc) {
      run_root = std::filesystem::path(argv[++index]);
    } else if (argument == "--exe" && index + 1 < argc) {
      exe = std::filesystem::path(argv[++index]);
    } else if (argument == "--product-dll" && index + 1 < argc) {
      product_dll = std::filesystem::path(argv[++index]);
    } else if (argument == "--version-file" && index + 1 < argc) {
      version_file = std::filesystem::path(argv[++index]);
    } else if (argument == "--paths-only") {
      paths_only = true;
    } else if (argument == "--paths-can-fail") {
      paths_only = true;
      force_path_failure = true;
    }
  }
  if (run_root.empty() || !run_root.is_absolute() ||
      (!paths_only && (exe.empty() || product_dll.empty() || version_file.empty()))) {
    std::cout << "FAIL: --run-root, --exe, --product-dll and --version-file are "
                 "required\n";
    return 2;
  }
  const std::filesystem::path root = PrepareRoot(run_root, "unit");
  if (root.empty()) {
    std::cout << "FAIL: cannot prepare the run root\n";
    return 2;
  }

  if (paths_only) {
    TestPaths(root / "paths");
    if (force_path_failure) {
      epocanva::windows::local_path::PreparedPath path;
      Check(epocanva::windows::local_path::Prepare(L"\\\\invalid.example\\share", &path),
            "deliberately false claim that UNC is admitted");
    }
    if (g_failures != 0) return 14;
    std::cout << "RESULT PASS local paths\n";
    return 0;
  }
  TestDigest();
  TestSettingsRoundTrip(root / "settings");
  {
    std::error_code ec;
    std::filesystem::create_directories(root / "atomic", ec);
  }
  TestAtomicWrite(root / "atomic");
  TestManifest(root / "manifest");
  TestProductPayloadPolicy(root / "payload-policy", version_file);
  TestPaths(root / "paths");
  TestVersionAndIcon(exe, product_dll, version_file);

  if (g_failures != 0) {
    std::cout << "FAILED: " << g_failures << " check(s)\n";
    return 14;
  }
  std::cout << "RESULT PASS\n";
  return 0;
}
