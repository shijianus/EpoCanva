#include "product_manifest.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <vector>

#include "product_io.h"

namespace epocanva {
namespace product {
namespace {

bool IsSafeRelativePath(const std::string& path) {
  if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos ||
      path.find(':') != std::string::npos ||
      !std::all_of(path.begin(), path.end(), [](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' ||
               ch == '.' || ch == '/';
      })) {
    return false;
  }
  if (path.find('\t') != std::string::npos || path.find('\n') != std::string::npos ||
      path.find('\r') != std::string::npos) {
    return false;
  }
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t slash = path.find('/', start);
    const std::string component =
        path.substr(start, slash == std::string::npos ? std::string::npos
                                                      : slash - start);
    if (component.empty() || component == "." || component == ".." ||
        component.back() == '.') {
      return false;
    }
    if (slash == std::string::npos) {
      break;
    }
    start = slash + 1;
  }
  return true;
}

bool ParseUint64(const std::string& text, std::uint64_t* value) {
  if (text.empty() || text.size() > 20) {
    return false;
  }
  std::uint64_t result = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
    if (result > (UINT64_MAX - digit) / 10) {
      return false;
    }
    result = result * 10 + digit;
  }
  *value = result;
  return true;
}

std::vector<std::string> SplitTabs(const std::string& line) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  for (;;) {
    const std::size_t tab = line.find('\t', start);
    parts.push_back(line.substr(
        start, tab == std::string::npos ? std::string::npos : tab - start));
    if (tab == std::string::npos) {
      break;
    }
    start = tab + 1;
  }
  return parts;
}

}  // namespace

bool LoadProductManifest(const std::filesystem::path& path, ProductManifest* manifest,
                         std::wstring* why) {
  if (manifest == nullptr) {
    if (why != nullptr) {
      *why = L"LoadProductManifest received a null manifest pointer";
    }
    return false;
  }
  std::vector<std::uint8_t> bytes;
  if (!ReadFileBytes(path, &bytes, why)) {
    return false;
  }
  std::string text(bytes.begin(), bytes.end());
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF) {
    text.erase(0, 3);
  }
  std::istringstream stream(text);
  std::string line;
  ProductManifest parsed;
  std::set<std::string> seen;
  bool header_seen = false;
  bool version_seen = false;
  bool commit_seen = false;
  bool internal_only_seen = false;
  bool profile_seen = false;
  bool distribution_seen = false;
  bool path_exception_seen = false;
  auto reject_key = [&why]() {
    if (why != nullptr) {
      *why = L"package.manifest has a missing, duplicate or invalid metadata key";
    }
    return false;
  };
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    if (!header_seen) {
      if (line == "epocanva-package-manifest 1") {
        parsed.format = 1;
      } else if (line == "epocanva-package-manifest 2") {
        parsed.format = 2;
      } else {
        if (why != nullptr) {
          *why = L"package.manifest does not start with a supported header";
        }
        return false;
      }
      header_seen = true;
      continue;
    }
    if (line.rfind("version=", 0) == 0) {
      if (version_seen || line.size() == 8) { return reject_key(); }
      version_seen = true;
      parsed.version = line.substr(8);
      continue;
    }
    if (line.rfind("commit=", 0) == 0) {
      if (commit_seen || line.size() == 7) { return reject_key(); }
      commit_seen = true;
      parsed.commit = line.substr(7);
      continue;
    }
    if (line.rfind("internal-only=", 0) == 0) {
      if (parsed.format != 1 || internal_only_seen ||
          (line != "internal-only=0" && line != "internal-only=1")) {
        return reject_key();
      }
      internal_only_seen = true;
      parsed.internal_only = line == "internal-only=1";
      continue;
    }
    if (line.rfind("acceptance-profile=", 0) == 0) {
      if (parsed.format != 2 || profile_seen ||
          (line != "acceptance-profile=internal-unsigned" &&
           line != "acceptance-profile=public-signed")) { return reject_key(); }
      profile_seen = true;
      parsed.acceptance_profile = line.substr(19);
      continue;
    }
    if (line.rfind("distribution-policy=", 0) == 0) {
      if (parsed.format != 2 || distribution_seen ||
          (line != "distribution-policy=internal-only" &&
           line != "distribution-policy=public-candidate")) { return reject_key(); }
      distribution_seen = true;
      parsed.distribution_policy = line.substr(20);
      continue;
    }
    if (line.rfind("path-exception=", 0) == 0) {
      if (parsed.format != 2 || path_exception_seen ||
          (line != "path-exception=none" &&
           line != "path-exception=embedded-build-paths")) { return reject_key(); }
      path_exception_seen = true;
      parsed.path_exception = line.substr(15);
      continue;
    }
    if (line.rfind("file\t", 0) == 0) {
      const std::vector<std::string> parts = SplitTabs(line);
      if (parts.size() != 4) {
        if (why != nullptr) {
          *why = L"a package.manifest file row does not have four tab-separated fields";
        }
        return false;
      }
      ManifestEntry entry;
      entry.relative_path = parts[1];
      entry.sha256 = parts[2];
      if (!IsSafeRelativePath(entry.relative_path)) {
        if (why != nullptr) {
          *why = L"a package.manifest path is not a safe relative path";
        }
        return false;
      }
      if (!IsLowercaseHexSha256(entry.sha256)) {
        if (why != nullptr) {
          *why = L"a package.manifest digest is not 64 lowercase hex characters";
        }
        return false;
      }
      if (!ParseUint64(parts[3], &entry.bytes)) {
        if (why != nullptr) {
          *why = L"a package.manifest byte count is not a decimal integer";
        }
        return false;
      }
      std::string folded_path = entry.relative_path;
      std::transform(folded_path.begin(), folded_path.end(), folded_path.begin(),
                     [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
      if (!seen.insert(folded_path).second) {
        if (why != nullptr) {
          *why = L"package.manifest lists the same path twice";
        }
        return false;
      }
      parsed.files.push_back(std::move(entry));
      continue;
    }
    if (why != nullptr) {
      *why = L"package.manifest contains an unrecognised line";
    }
    return false;
  }
  if (!header_seen) {
    if (why != nullptr) {
      *why = L"package.manifest is empty";
    }
    return false;
  }
  if (!version_seen || !commit_seen ||
      (parsed.format == 1 && !internal_only_seen) ||
      (parsed.format == 2 && (!profile_seen || !distribution_seen || !path_exception_seen))) {
    return reject_key();
  }
  if (parsed.format == 2) {
    // Preserve this compatibility field as scope only. It is never a path waiver.
    parsed.internal_only = parsed.distribution_policy == "internal-only";
  }
  if (parsed.files.empty()) {
    if (why != nullptr) {
      *why = L"package.manifest lists no files";
    }
    return false;
  }
  *manifest = std::move(parsed);
  return true;
}

const std::vector<RequiredProductFile>& RequiredProductFiles() {
  static const std::vector<RequiredProductFile> required = {
#define EPOCANVA_PACKAGE_FILE(path, source, sha256, bytes) {path, source, sha256, bytes},
#include "../../../data/manifests/product-package-files.inc"
#undef EPOCANVA_PACKAGE_FILE
  };
  return required;
}

bool ValidateProductAcceptancePolicy(const ProductManifest& manifest, std::wstring* why) {
  if (manifest.format != 2) {
    if (why != nullptr) { *why = L"legacy manifest is diagnostic-only; restage in format 2"; }
    return false;
  }
  if ((manifest.acceptance_profile != "internal-unsigned" &&
       manifest.acceptance_profile != "public-signed") ||
      (manifest.distribution_policy != "internal-only" &&
       manifest.distribution_policy != "public-candidate") ||
      (manifest.path_exception != "none" && manifest.path_exception != "embedded-build-paths")) {
    if (why != nullptr) { *why = L"manifest acceptance, distribution or path policy is invalid"; }
    return false;
  }
  if (manifest.path_exception != "none") {
    if (why != nullptr) { *why = L"embedded build-path exception prevents acceptance readiness"; }
    return false;
  }
  if (manifest.acceptance_profile == "public-signed" &&
      manifest.distribution_policy != "public-candidate") {
    if (why != nullptr) { *why = L"public acceptance requires an explicit public candidate"; }
    return false;
  }
  return true;
}

// The fixed policy covers the binaries, product data and licences. An internal
// diagnostics package may additionally carry the reviewed five-language runtime
// payload. This exception is scoped to distribution_policy=internal-only and to
// the runtime paths the backend loader actually reads; public candidates still
// require an exactly reviewed policy.
bool IsInternalBackendRuntimePath(const std::string& path) {
  if (path.rfind("data/backend/", 0) == 0) return true;
  return path == "data/epocanva_backend_pinyin.schema.yaml" ||
      path == "data/epocanva_backend_zhuyin.schema.yaml" ||
      path == "data/epocanva_backend_terra.dict.yaml" ||
      path == "data/zhuyin.yaml";
}

bool ValidateProductPayloadPolicy(const ProductManifest& manifest, std::wstring* why) {
  const auto& required = RequiredProductFiles();
  if (manifest.files.size() < required.size()) {
    if (why != nullptr) { *why = L"package.manifest does not list the required payload"; }
    return false;
  }
  const bool internal_only = manifest.distribution_policy == "internal-only";
  std::set<std::string> seen;
  bool has_backend_payload = false;
  for (const ManifestEntry& entry : manifest.files) {
    if (!seen.insert(entry.relative_path).second) {
      if (why != nullptr) { *why = L"package.manifest contains a duplicate payload path"; }
      return false;
    }
    const bool is_required = std::any_of(required.begin(), required.end(),
        [&entry](const RequiredProductFile& file) {
          return entry.relative_path == file.relative_path;
        });
    if (!is_required) {
      if (!internal_only || !IsInternalBackendRuntimePath(entry.relative_path)) {
        if (why != nullptr) { *why = L"package.manifest contains an unapproved payload path"; }
        return false;
      }
      has_backend_payload = true;
    }
  }
  if (has_backend_payload &&
      FindManifestEntry(manifest, "data/backend/resources.tsv") == nullptr) {
    if (why != nullptr) { *why = L"internal backend payload has no verified resources.tsv"; }
    return false;
  }
  for (const RequiredProductFile& file : required) {
    const ManifestEntry* entry = FindManifestEntry(manifest, file.relative_path);
    if (entry == nullptr || (file.sha256[0] != '\0' &&
        (entry->sha256 != file.sha256 || entry->bytes != file.bytes))) {
      if (why != nullptr) {
        *why = L"required payload identity is missing or changed: " + Utf8ToWide(file.relative_path);
      }
      return false;
    }
  }
  return true;
}

bool VerifyProductFileSet(const std::filesystem::path& root,
                          const ProductManifest& manifest, std::wstring* why) {
  std::set<std::string> expected = {"package.manifest"};
  for (const ManifestEntry& entry : manifest.files) { expected.insert(entry.relative_path); }
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(root, ec), end;
  if (ec) {
    if (why != nullptr) { *why = L"cannot enumerate the product package root"; }
    return false;
  }
  for (; it != end; it.increment(ec)) {
    if (ec) { break; }
    const auto& path = it->path();
    WIN32_FIND_DATAW info = {};
    const HANDLE find = ::FindFirstFileW(path.c_str(), &info);
    if (find == INVALID_HANDLE_VALUE) {
      if (why != nullptr) { *why = L"package contains an unreadable file"; }
      return false;
    }
    ::FindClose(find);
    // Cloud placeholders use non-surrogate reparse tags. Only name-surrogate tags
    // redirect traversal, including junctions and symbolic links.
    constexpr DWORD kNameSurrogate = 0x20000000UL;
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
        (info.dwReserved0 & kNameSurrogate) != 0) {
      if (why != nullptr) { *why = L"package contains a path-redirection reparse point"; }
      return false;
    }
    if (it->is_directory(ec)) { continue; }
    const std::string relative = path.lexically_relative(root).generic_u8string();
    if (!it->is_regular_file(ec) || expected.erase(relative) != 1) {
      if (why != nullptr) { *why = L"unlisted file in package root: " + Utf8ToWide(relative); }
      return false;
    }
  }
  if (ec || !expected.empty()) {
    if (why != nullptr) { *why = L"package file set is incomplete or could not be enumerated"; }
    return false;
  }
  return true;
}

bool VerifyProductManifest(const std::filesystem::path& root,
                           const ProductManifest& manifest,
                           std::vector<FileVerification>* files, int* failures,
                           std::wstring* why) {
  if (files == nullptr || failures == nullptr) {
    if (why != nullptr) {
      *why = L"VerifyProductManifest received a null output";
    }
    return false;
  }
  files->clear();
  *failures = 0;
  for (const ManifestEntry& entry : manifest.files) {
    FileVerification verification;
    verification.entry = entry;
    verification.path = root / std::filesystem::u8path(entry.relative_path);
    std::error_code ec;
    verification.present =
        std::filesystem::is_regular_file(verification.path, ec) && !ec;
    if (!verification.present) {
      ++*failures;
      files->push_back(std::move(verification));
      continue;
    }
    std::wstring hash_why;
    std::string actual_hash;
    std::uint64_t actual_bytes = 0;
    if (!Sha256File(verification.path, &actual_hash, &actual_bytes, &hash_why)) {
      if (why != nullptr) {
        *why = L"cannot hash " + verification.path.wstring() + L": " + hash_why;
      }
      return false;
    }
    verification.actual_bytes = actual_bytes;
    verification.size_ok = actual_bytes == entry.bytes;
    verification.hash_ok = actual_hash == entry.sha256;
    if (!verification.ok()) {
      ++*failures;
    }
    files->push_back(std::move(verification));
  }
  return true;
}

const ManifestEntry* FindManifestEntry(const ProductManifest& manifest,
                                       const std::string& relative_path) {
  for (const ManifestEntry& entry : manifest.files) {
    if (entry.relative_path == relative_path) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace product
}  // namespace epocanva
