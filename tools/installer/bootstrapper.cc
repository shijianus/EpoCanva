// EpoCanva offline installer bootstrapper.
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

#ifndef EPOCANVA_EMBEDDED_MSI_SHA256
#define EPOCANVA_EMBEDDED_MSI_SHA256 ""
#endif

namespace {
constexpr std::array<std::uint8_t, 8> kMagic{{'E','P','O','I','N','S','T','1'}};
constexpr std::size_t kDigestBytes = 32;
constexpr std::size_t kHeaderBytes = 8 + 8 + kDigestBytes;

std::wstring ModulePath() {
  std::wstring value(32768, L'\0');
  const DWORD size = ::GetModuleFileNameW(nullptr, value.data(), static_cast<DWORD>(value.size()));
  if (size == 0 || size >= value.size()) return {};
  value.resize(size);
  return value;
}

bool Sha256(const std::uint8_t* bytes, std::size_t size, std::array<std::uint8_t, 32>* output) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD object_bytes = 0;
  DWORD received = 0;
  std::vector<std::uint8_t> object;
  if (::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
  bool ok = false;
  do {
    if (::BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
          reinterpret_cast<PUCHAR>(&object_bytes), sizeof(object_bytes), &received, 0) < 0) break;
    object.resize(object_bytes);
    if (::BCryptCreateHash(algorithm, &hash, object.data(), object_bytes, nullptr, 0, 0) < 0) break;
    if (::BCryptHashData(hash, const_cast<PUCHAR>(bytes), static_cast<ULONG>(size), 0) < 0) break;
    if (::BCryptFinishHash(hash, output->data(), static_cast<ULONG>(output->size()), 0) < 0) break;
    ok = true;
  } while (false);
  if (hash) ::BCryptDestroyHash(hash);
  ::BCryptCloseAlgorithmProvider(algorithm, 0);
  return ok;
}

bool HexDigest(const char* text, std::array<std::uint8_t, 32>* digest) {
  if (!text || std::char_traits<char>::length(text) != 64) return false;
  for (std::size_t index = 0; index < digest->size(); ++index) {
    unsigned value = 0;
    if (sscanf_s(text + index * 2, "%2x", &value) != 1) return false;
    (*digest)[index] = static_cast<std::uint8_t>(value);
  }
  return true;
}

std::wstring Quote(const std::wstring& value) {
  std::wstring result = L"\"";
  for (const wchar_t ch : value) {
    if (ch == L'"') result += L"\\\"";
    else result.push_back(ch);
  }
  result += L"\"";
  return result;
}

bool ExtractEmbeddedMsi(const std::wstring& self, std::wstring* output) {
  HANDLE file = ::CreateFileW(self.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(file, &size) || size.QuadPart < static_cast<LONGLONG>(kHeaderBytes)) {
    ::CloseHandle(file); return false;
  }
  std::vector<std::uint8_t> image(static_cast<std::size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL read_ok = ::ReadFile(file, image.data(), static_cast<DWORD>(image.size()), &read, nullptr);
  ::CloseHandle(file);
  if (!read_ok || read != image.size() || !std::equal(kMagic.begin(), kMagic.end(), image.begin())) return false;
  std::uint64_t payload_bytes = 0;
  for (std::size_t index = 0; index < 8; ++index)
    payload_bytes |= static_cast<std::uint64_t>(image[8 + index]) << (index * 8);
  if (payload_bytes == 0 || payload_bytes != image.size() - kHeaderBytes) return false;
  std::array<std::uint8_t, 32> embedded{};
  std::copy_n(image.begin() + 16, embedded.size(), embedded.begin());
  std::array<std::uint8_t, 32> expected{};
  if (!HexDigest(EPOCANVA_EMBEDDED_MSI_SHA256, &expected) || embedded != expected) return false;
  std::array<std::uint8_t, 32> actual{};
  if (!Sha256(image.data() + kHeaderBytes, static_cast<std::size_t>(payload_bytes), &actual) || actual != expected) return false;
  wchar_t temp[MAX_PATH]{};
  if (!::GetTempPathW(MAX_PATH, temp)) return false;
  wchar_t name[MAX_PATH]{};
  if (!::GetTempFileNameW(temp, L"Epi", 0, name)) return false;
  std::wstring path(name);
  path += L".msi";
  ::DeleteFileW(name);
  HANDLE out = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
      FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (out == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL write_ok = ::WriteFile(out, image.data() + kHeaderBytes,
      static_cast<DWORD>(payload_bytes), &written, nullptr);
  ::FlushFileBuffers(out);
  ::CloseHandle(out);
  if (!write_ok || written != payload_bytes) { ::DeleteFileW(path.c_str()); return false; }
  *output = std::move(path);
  return true;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  const std::wstring self = ModulePath();
  std::wstring msi;
  if (self.empty() || !ExtractEmbeddedMsi(self, &msi)) {
    ::MessageBoxW(nullptr, L"The embedded installer payload failed integrity verification.",
        L"EpoCanva Setup", MB_ICONERROR | MB_OK);
    return 2;
  }
  int count = 0;
  LPWSTR* args = ::CommandLineToArgvW(::GetCommandLineW(), &count);
  std::wstring tail;
  for (int index = 1; args && index < count; ++index) tail += L" " + Quote(args[index]);
  if (args) ::LocalFree(args);
  const std::wstring command = L"/i " + Quote(msi) + tail;
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS;
  info.lpVerb = L"runas";
  info.lpFile = L"msiexec.exe";
  info.lpParameters = command.c_str();
  info.nShow = SW_SHOWNORMAL;
  if (!::ShellExecuteExW(&info)) {
    const DWORD error = ::GetLastError();
    ::DeleteFileW(msi.c_str());
    return error == ERROR_CANCELLED ? 1223 : 3;
  }
  const DWORD wait = ::WaitForSingleObject(info.hProcess, INFINITE);
  DWORD exit_code = 3;
  if (wait == WAIT_OBJECT_0) (void)::GetExitCodeProcess(info.hProcess, &exit_code);
  ::CloseHandle(info.hProcess);
  ::DeleteFileW(msi.c_str());
  return static_cast<int>(exit_code);
}
