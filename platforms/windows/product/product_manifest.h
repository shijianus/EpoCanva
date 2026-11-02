// EpoCanva W1 -- package manifest.
//
// A fixed, line-oriented format: one header, unique identity/policy keys and one
// tab-separated row per payload file. RequiredProductFiles is compiled in from the
// shared staging policy. SHA-256 verifies bytes, not publisher identity or legal clearance.
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
  bool internal_only = true;  // format 1 legacy field; never authorizes acceptance
  std::string acceptance_profile;   // internal-unsigned | public-signed (format 2)
  std::string distribution_policy;  // internal-only | public-candidate (not approval)
  std::string path_exception;       // none | embedded-build-paths
  std::vector<ManifestEntry> files;
};

struct RequiredProductFile {
  const char* relative_path;
  const char* source;         // repository-relative; empty for receipt/generated files
  const char* sha256;         // fixed exact-byte digest, or empty for receipt/generated
  std::uint64_t bytes;        // byte count, zero only for receipt/generated files
};

const std::vector<RequiredProductFile>& RequiredProductFiles();
// Format 1 is readable for diagnosis only. Format 2 policy values are independent;
// a path exception always prevents package readiness, even for internal acceptance.
bool ValidateProductAcceptancePolicy(const ProductManifest& manifest, std::wstring* why);
// Exact required path set and fixed-content identities, independent of on-disk bytes.
bool ValidateProductPayloadPolicy(const ProductManifest& manifest, std::wstring* why);
// Rejects unlisted files/reparse points inside the package root only. User data is
// deliberately not scanned: it lives outside the package root.
bool VerifyProductFileSet(const std::filesystem::path& root,
                          const ProductManifest& manifest, std::wstring* why);

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
