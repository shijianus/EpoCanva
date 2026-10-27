#include "product_io.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace epocanva {
namespace product {
namespace {

struct BcryptAlgorithmCloser {
  void operator()(BCRYPT_ALG_HANDLE handle) const noexcept {
    if (handle != nullptr) {
      ::BCryptCloseAlgorithmProvider(handle, 0);
    }
  }
};

struct BcryptHashCloser {
  void operator()(BCRYPT_HASH_HANDLE handle) const noexcept {
    if (handle != nullptr) {
      ::BCryptDestroyHash(handle);
    }
  }
};

using AlgorithmHandle = std::unique_ptr<void, BcryptAlgorithmCloser>;
using HashHandle = std::unique_ptr<void, BcryptHashCloser>;

std::wstring StatusText(const wchar_t* what, NTSTATUS status) {
  return std::wstring(what) + L" failed (NTSTATUS 0x" +
         [&status] {
           wchar_t buffer[16] = {};
           static const wchar_t kHex[] = L"0123456789abcdef";
           const auto value = static_cast<unsigned long>(status);
           buffer[0] = L'0';
           buffer[1] = L'x';
           for (int index = 0; index < 8; ++index) {
             buffer[2 + index] =
                 kHex[(value >> ((7 - index) * 4)) & 0xFu];
           }
           return std::wstring(buffer, 10);
         }() +
         L")";
}

void HexLower(const unsigned char* bytes, std::size_t size, std::string* out) {
  static const char kHex[] = "0123456789abcdef";
  out->clear();
  out->reserve(size * 2);
  for (std::size_t index = 0; index < size; ++index) {
    out->push_back(kHex[bytes[index] >> 4]);
    out->push_back(kHex[bytes[index] & 0x0F]);
  }
}

}  // namespace

bool ReadFileBytes(const std::filesystem::path& path, std::vector<std::uint8_t>* bytes,
                   std::wstring* why) {
  if (bytes == nullptr) {
    if (why != nullptr) {
      *why = L"ReadFileBytes received a null output buffer";
    }
    return false;
  }
  bytes->clear();
  const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE |
                                        FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (why != nullptr) {
      *why = L"cannot open " + path.wstring() + L" (error " +
             std::to_wstring(::GetLastError()) + L")";
    }
    return false;
  }
  LARGE_INTEGER size = {};
  if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
      size.QuadPart > (64ll << 20)) {
    if (why != nullptr) {
      *why = L"invalid file size for " + path.wstring();
    }
    ::CloseHandle(file);
    return false;
  }
  bytes->resize(static_cast<std::size_t>(size.QuadPart));
  std::size_t offset = 0;
  while (offset < bytes->size()) {
    const DWORD want = static_cast<DWORD>(
        std::min<std::size_t>(bytes->size() - offset, 1u << 20));
    DWORD read = 0;
    if (!::ReadFile(file, bytes->data() + offset, want, &read, nullptr) ||
        read == 0) {
      if (why != nullptr) {
        *why = L"reading " + path.wstring() + L" failed (error " +
               std::to_wstring(::GetLastError()) + L")";
      }
      ::CloseHandle(file);
      bytes->clear();
      return false;
    }
    offset += read;
  }
  ::CloseHandle(file);
  return true;
}

bool WriteFileAtomically(const std::filesystem::path& path, const void* data,
                         std::size_t size, std::wstring* why) {
  if (path.is_absolute() == false || !path.has_filename()) {
    if (why != nullptr) {
      *why = L"atomic writes require an absolute file path";
    }
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    if (why != nullptr) {
      *why = L"cannot create " + path.parent_path().wstring() + L" (error " +
             std::to_wstring(ec.value()) + L")";
    }
    return false;
  }
  wchar_t temp_name[MAX_PATH] = {};
  if (::GetTempFileNameW(path.parent_path().c_str(), L"epc", 0, temp_name) == 0) {
    if (why != nullptr) {
      *why = L"GetTempFileNameW failed in " + path.parent_path().wstring() +
             L" (error " + std::to_wstring(::GetLastError()) + L")";
    }
    return false;
  }
  const std::filesystem::path temp_path(temp_name);
  const HANDLE file = ::CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (why != nullptr) {
      *why = L"cannot open the temporary file (error " +
             std::to_wstring(::GetLastError()) + L")";
    }
    ::DeleteFileW(temp_path.c_str());
    return false;
  }
  const auto* cursor = static_cast<const std::uint8_t*>(data);
  std::size_t offset = 0;
  bool ok = true;
  while (offset < size) {
    const DWORD want =
        static_cast<DWORD>(std::min<std::size_t>(size - offset, 1u << 20));
    DWORD written = 0;
    if (!::WriteFile(file, cursor + offset, want, &written, nullptr) ||
        written == 0) {
      ok = false;
      break;
    }
    offset += written;
  }
  if (ok && !::FlushFileBuffers(file)) {
    ok = false;
  }
  if (!::CloseHandle(file)) {
    ok = false;
  }
  if (!ok) {
    if (why != nullptr) {
      *why = L"writing the temporary file failed (error " +
             std::to_wstring(::GetLastError()) + L")";
    }
    ::DeleteFileW(temp_path.c_str());
    return false;
  }
  if (!::MoveFileExW(temp_path.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    if (why != nullptr) {
      *why = L"replacing " + path.wstring() + L" failed (error " +
             std::to_wstring(::GetLastError()) + L")";
    }
    ::DeleteFileW(temp_path.c_str());
    return false;
  }
  return true;
}

bool WriteTextAtomically(const std::filesystem::path& path, const std::string& text,
                         std::wstring* why) {
  return WriteFileAtomically(path, text.data(), text.size(), why);
}

bool Sha256(const void* data, std::size_t size, std::string* lowercase_hex,
            std::wstring* why) {
  if (lowercase_hex == nullptr) {
    if (why != nullptr) {
      *why = L"Sha256 received a null output string";
    }
    return false;
  }
  BCRYPT_ALG_HANDLE raw_algorithm = nullptr;
  NTSTATUS status = ::BCryptOpenAlgorithmProvider(
      &raw_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
  if (!BCRYPT_SUCCESS(status)) {
    if (why != nullptr) {
      *why = StatusText(L"BCryptOpenAlgorithmProvider", status);
    }
    return false;
  }
  AlgorithmHandle algorithm(raw_algorithm);
  DWORD object_size = 0;
  DWORD result_size = 0;
  status = ::BCryptGetProperty(
      algorithm.get(), BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &result_size, 0);
  if (!BCRYPT_SUCCESS(status) || object_size == 0) {
    if (why != nullptr) {
      *why = StatusText(L"BCryptGetProperty(object length)", status);
    }
    return false;
  }
  std::vector<std::uint8_t> object(object_size);
  BCRYPT_HASH_HANDLE raw_hash = nullptr;
  status = ::BCryptCreateHash(algorithm.get(), &raw_hash, object.data(), object_size,
                              nullptr, 0, 0);
  if (!BCRYPT_SUCCESS(status)) {
    if (why != nullptr) {
      *why = StatusText(L"BCryptCreateHash", status);
    }
    return false;
  }
  HashHandle hash(raw_hash);
  if (size != 0) {
    auto* bytes = const_cast<unsigned char*>(
        static_cast<const unsigned char*>(data));
    std::size_t offset = 0;
    while (offset < size) {
      const ULONG chunk = static_cast<ULONG>(
          std::min<std::size_t>(size - offset, (1u << 31) - 1));
      status = ::BCryptHashData(hash.get(), bytes + offset, chunk, 0);
      if (!BCRYPT_SUCCESS(status)) {
        if (why != nullptr) {
          *why = StatusText(L"BCryptHashData", status);
        }
        return false;
      }
      offset += chunk;
    }
  }
  std::array<unsigned char, 32> digest = {};
  status = ::BCryptFinishHash(hash.get(), digest.data(),
                              static_cast<ULONG>(digest.size()), 0);
  if (!BCRYPT_SUCCESS(status)) {
    if (why != nullptr) {
      *why = StatusText(L"BCryptFinishHash", status);
    }
    return false;
  }
  HexLower(digest.data(), digest.size(), lowercase_hex);
  return true;
}

bool Sha256File(const std::filesystem::path& path, std::string* lowercase_hex,
                std::uint64_t* size, std::wstring* why) {
  std::vector<std::uint8_t> bytes;
  if (!ReadFileBytes(path, &bytes, why)) {
    return false;
  }
  if (size != nullptr) {
    *size = bytes.size();
  }
  return Sha256(bytes.data(), bytes.size(), lowercase_hex, why);
}

bool IsLowercaseHexSha256(const std::string& text) {
  if (text.size() != 64) {
    return false;
  }
  for (const char ch : text) {
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
      return false;
    }
  }
  return true;
}

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty()) {
    return std::wstring();
  }
  const int chars = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          text.data(), static_cast<int>(text.size()),
                                          nullptr, 0);
  if (chars <= 0) {
    return std::wstring();
  }
  std::wstring out(static_cast<std::size_t>(chars), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), out.data(), chars) !=
      chars) {
    return std::wstring();
  }
  return out;
}

std::string WideToUtf8(const std::wstring& text) {
  if (text.empty()) {
    return std::string();
  }
  const int bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                          text.data(), static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) {
    return std::string();
  }
  std::string out(static_cast<std::size_t>(bytes), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), out.data(), bytes,
                            nullptr, nullptr) != bytes) {
    return std::string();
  }
  return out;
}

}  // namespace product
}  // namespace epocanva
