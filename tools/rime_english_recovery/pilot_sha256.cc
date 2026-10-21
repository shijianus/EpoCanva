// EpoCanva P2.4-recovery -- the file digest the pilot data identity check is built on,
// computed by Windows CNG. See pilot_sha256.h for why the harness needs it, what its own
// correctness is checked against, and which failure kinds are kept apart.
#include "pilot_sha256.h"

#include <windows.h>

#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace epocanva {
namespace recovery {
namespace {

// One ReadFile of at most this many bytes, fed straight into BCryptHashData. The file's size
// never decides how much memory this helper holds.
constexpr DWORD kChunkBytes = 65536;

// SHA-256's output in bytes; the hex rendering doubles it. The value is not assumed -- it is
// checked against what CNG reports for the hash object below.
constexpr ULONG kDigestBytes = 32;

std::string Win32Detail(const char* call, DWORD error) {
  char buffer[128];
  std::snprintf(buffer, sizeof(buffer), "%s failed with win32 error %lu (0x%08lx)", call,
                static_cast<unsigned long>(error), static_cast<unsigned long>(error));
  return std::string(buffer);
}

std::string NtDetail(const char* call, NTSTATUS status) {
  char buffer[128];
  std::snprintf(buffer, sizeof(buffer), "%s returned NTSTATUS 0x%08lx", call,
                static_cast<unsigned long>(status));
  return std::string(buffer);
}

// Handles are released in exactly one place each -- the destructor -- so no early return on an
// error path can leave one open. Nothing here is refcounted or shared: one file, one hash
// object, one algorithm provider, all of them out of scope when the digest call returns.
class ReadableFile {
 public:
  explicit ReadableFile(const std::wstring& path)
      : handle_(CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)) {}
  ~ReadableFile() {
    if (is_open()) {
      CloseHandle(handle_);
    }
  }
  ReadableFile(const ReadableFile&) = delete;
  ReadableFile& operator=(const ReadableFile&) = delete;

  bool is_open() const { return handle_ != INVALID_HANDLE_VALUE; }
  HANDLE get() const { return handle_; }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class AlgorithmProvider {
 public:
  AlgorithmProvider()
      : status_(BCryptOpenAlgorithmProvider(&handle_, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) {}
  ~AlgorithmProvider() {
    if (handle_ != nullptr) {
      BCryptCloseAlgorithmProvider(handle_, 0);
    }
  }
  AlgorithmProvider(const AlgorithmProvider&) = delete;
  AlgorithmProvider& operator=(const AlgorithmProvider&) = delete;

  NTSTATUS status() const { return status_; }
  BCRYPT_ALG_HANDLE get() const { return handle_; }

 private:
  BCRYPT_ALG_HANDLE handle_ = nullptr;
  NTSTATUS status_ = -1;  // negative until the open really ran
};

class HashObject {
 public:
  explicit HashObject(BCRYPT_ALG_HANDLE algorithm)
      : status_(BCryptCreateHash(algorithm, &handle_, nullptr, 0, nullptr, 0, 0)) {}
  ~HashObject() {
    if (handle_ != nullptr) {
      BCryptDestroyHash(handle_);
    }
  }
  HashObject(const HashObject&) = delete;
  HashObject& operator=(const HashObject&) = delete;

  NTSTATUS status() const { return status_; }
  BCRYPT_HASH_HANDLE get() const { return handle_; }

 private:
  BCRYPT_HASH_HANDLE handle_ = nullptr;
  NTSTATUS status_ = -1;
};

}  // namespace

const char* DigestFailureName(DigestFailure failure) {
  switch (failure) {
    case DigestFailure::kNone:
      return "none";
    case DigestFailure::kOpenFailed:
      return "open-failed";
    case DigestFailure::kReadFailed:
      return "read-failed";
    case DigestFailure::kHashFailed:
      return "hash-failed";
  }
  return "unclassified";
}

FileDigest Sha256FileDigest(const std::filesystem::path& file) {
  FileDigest out;

  const ReadableFile handle(file.wstring());
  if (!handle.is_open()) {
    out.failure = DigestFailure::kOpenFailed;
    out.detail = Win32Detail("CreateFileW", GetLastError());
    return out;
  }

  const AlgorithmProvider algorithm;
  if (!BCRYPT_SUCCESS(algorithm.status())) {
    out.failure = DigestFailure::kHashFailed;
    out.detail = NtDetail("BCryptOpenAlgorithmProvider(SHA256)", algorithm.status());
    return out;
  }

  const HashObject hash(algorithm.get());
  if (!BCRYPT_SUCCESS(hash.status())) {
    out.failure = DigestFailure::kHashFailed;
    out.detail = NtDetail("BCryptCreateHash", hash.status());
    return out;
  }

  // The size CNG gives this particular hash object, read back rather than assumed: a provider
  // that answered with anything other than 32 bytes would otherwise have its output rendered
  // as a SHA-256 digest.
  ULONG digest_bytes = 0;
  ULONG returned_bytes = 0;
  NTSTATUS status = BCryptGetProperty(hash.get(), BCRYPT_HASH_LENGTH,
                                      reinterpret_cast<PUCHAR>(&digest_bytes),
                                      static_cast<ULONG>(sizeof(digest_bytes)),
                                      &returned_bytes, 0);
  if (!BCRYPT_SUCCESS(status)) {
    out.failure = DigestFailure::kHashFailed;
    out.detail = NtDetail("BCryptGetProperty(HashDigestLength)", status);
    return out;
  }
  if (returned_bytes != static_cast<ULONG>(sizeof(digest_bytes)) ||
      digest_bytes != kDigestBytes) {
    out.failure = DigestFailure::kHashFailed;
    out.detail = "CNG reported a hash length of " + std::to_string(digest_bytes) +
                 " bytes in " + std::to_string(returned_bytes) +
                 " result bytes; this helper renders nothing but a 32-byte SHA-256";
    return out;
  }

  std::vector<unsigned char> chunk(kChunkBytes);
  unsigned long long fed_bytes = 0;
  for (;;) {
    DWORD got = 0;
    if (!ReadFile(handle.get(), chunk.data(), kChunkBytes, &got, nullptr)) {
      out.failure = DigestFailure::kReadFailed;
      out.detail = Win32Detail("ReadFile", GetLastError()) + " after " +
                   std::to_string(fed_bytes) + " bytes";
      return out;
    }
    if (got == 0) {
      break;  // end of file
    }
    status = BCryptHashData(hash.get(), chunk.data(), got, 0);
    if (!BCRYPT_SUCCESS(status)) {
      out.failure = DigestFailure::kHashFailed;
      out.detail = NtDetail("BCryptHashData", status) + " on the block at byte " +
                   std::to_string(fed_bytes);
      return out;
    }
    fed_bytes += got;
  }

  std::array<unsigned char, kDigestBytes> digest{};
  status = BCryptFinishHash(hash.get(), digest.data(), kDigestBytes, 0);
  if (!BCRYPT_SUCCESS(status)) {
    out.failure = DigestFailure::kHashFailed;
    out.detail = NtDetail("BCryptFinishHash", status);
    return out;
  }

  char hex[2 * kDigestBytes + 1];
  for (ULONG i = 0; i < kDigestBytes; ++i) {
    std::snprintf(hex + 2 * i, sizeof(hex) - 2 * i, "%02x", static_cast<unsigned>(digest[i]));
  }
  out.hex.assign(hex, 2 * kDigestBytes);
  out.failure = DigestFailure::kNone;
  return out;
}

std::string Sha256HexOfFile(const std::filesystem::path& file) {
  const FileDigest digest = Sha256FileDigest(file);
  if (digest.failure != DigestFailure::kNone) {
    return std::string();
  }
  return digest.hex;
}

}  // namespace recovery
}  // namespace epocanva
