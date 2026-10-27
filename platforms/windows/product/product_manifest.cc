#include "product_manifest.h"

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
      path.find(':') != std::string::npos) {
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
    if (component.empty() || component == "." || component == "..") {
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
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    if (!header_seen) {
      if (line != "epocanva-package-manifest 1") {
        if (why != nullptr) {
          *why = L"package.manifest does not start with the format-1 header";
        }
        return false;
      }
      parsed.format = 1;
      header_seen = true;
      continue;
    }
    if (line.rfind("version=", 0) == 0) {
      parsed.version = line.substr(8);
      continue;
    }
    if (line.rfind("commit=", 0) == 0) {
      parsed.commit = line.substr(7);
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
      if (!seen.insert(entry.relative_path).second) {
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
  if (parsed.files.empty()) {
    if (why != nullptr) {
      *why = L"package.manifest lists no files";
    }
    return false;
  }
  *manifest = std::move(parsed);
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
