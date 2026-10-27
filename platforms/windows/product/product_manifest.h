// EpoCanva W1 -- package manifest.
//
// A fixed, line-oriented format, not JSON or YAML: one header, two key=value lines and
// one tab-separated row per payload file. The EXE verifies every row with BCrypt SHA-256
// before it reports a ready package. The manifest is an integrity index, not a signature.
#ifndef EPOCANVA_PRODUCT_MANIFEST_H_
#define EPOCANVA_PRODUCT_MANIFEST_H_

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace epocanva {
namespace product {

struct ManifestEntry {
  std::string relative_path;  // forward slashes, ASCII, no "." or ".." component
  std::string sha256;         // 64 lowercase hex characters
  std::uint64_t bytes = 0;
};

struct ProductManifest {
  int format = 0;
  std::string version;
  std::string commit;
  std::vector<ManifestEntry> files;
};

struct FileVerification {
  ManifestEntry entry;
  std::filesystem::path path;
  bool present = false;
  bool size_ok = false;
  bool hash_ok = false;
  std::uint64_t actual_bytes = 0;
  bool ok() const { return present && size_ok && hash_ok; }
};

bool LoadProductManifest(const std::filesystem::path& path, ProductManifest* manifest,
                         std::wstring* why);
// Reads and hashes every entry below `root`. Returns false only when the scan itself
// failed; a missing or changed file is reported in `files` and counted in `failures`.
bool VerifyProductManifest(const std::filesystem::path& root,
                           const ProductManifest& manifest,
                           std::vector<FileVerification>* files, int* failures,
                           std::wstring* why);
const ManifestEntry* FindManifestEntry(const ProductManifest& manifest,
                                       const std::string& relative_path);

}  // namespace product
}  // namespace epocanva

#endif  // EPOCANVA_PRODUCT_MANIFEST_H_
