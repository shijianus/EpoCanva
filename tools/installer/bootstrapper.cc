#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <sddl.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>

#include "epocanva_embedded_identity.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace {
using Digest = std::array<std::uint8_t, 32>;
constexpr std::array<std::uint8_t, 8> kMagic{{'E', 'P', 'O', 'I', 'N', 'S', 'T', '1'}};
constexpr std::array<std::uint8_t, 8> kMsiMagic{{0xd0, 0xcf, 0x11, 0xe0, 0xa1, 0xb1, 0x1a, 0xe1}};
constexpr std::size_t kPayloadHeaderBytes = 16 + sizeof(Digest);

class Handle {
 public:
  explicit Handle(HANDLE value = nullptr) : value_(value) {}
  ~Handle() { Reset(); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  HANDLE Get() const { return value_; }
  explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
  void Reset(HANDLE value = nullptr) {
    if (*this) ::CloseHandle(value_);
    value_ = value;
  }

 private:
  HANDLE value_;
};

bool ReadExact(HANDLE file, void* output, DWORD bytes) {
  DWORD received = 0;
  return ::ReadFile(file, output, bytes, &received, nullptr) && received == bytes;
}

bool Seek(HANDLE file, std::uint64_t byte_offset) {
  LARGE_INTEGER distance{};
  distance.QuadPart = static_cast<LONGLONG>(byte_offset);
  return ::SetFilePointerEx(file, distance, nullptr, FILE_BEGIN) != FALSE;
}

template <typename Value>
bool ReadAt(HANDLE file, std::uint64_t byte_offset, Value* output) {
  return Seek(file, byte_offset) && ReadExact(file, output, static_cast<DWORD>(sizeof(Value)));
}

bool FileSize(HANDLE file, std::uint64_t* bytes) {
  LARGE_INTEGER result{};
  if (!::GetFileSizeEx(file, &result) || result.QuadPart <= 0) return false;
  *bytes = static_cast<std::uint64_t>(result.QuadPart);
  return true;
}

bool Transfer(HANDLE source, std::uint64_t byte_offset, std::uint64_t bytes,
              HANDLE destination, BCRYPT_HASH_HANDLE hash) {
  if (!Seek(source, byte_offset)) return false;
  std::array<std::uint8_t, 64 * 1024> buffer;
  while (bytes != 0) {
    const auto chunk_bytes = static_cast<DWORD>(std::min(bytes, std::uint64_t{buffer.size()}));
    if (!ReadExact(source, buffer.data(), chunk_bytes)) return false;
    if (hash && ::BCryptHashData(hash, buffer.data(), chunk_bytes, 0) < 0) return false;
    if (destination) {
      DWORD written = 0;
      if (!::WriteFile(destination, buffer.data(), chunk_bytes, &written, nullptr) ||
          written != chunk_bytes) return false;
    }
    bytes -= chunk_bytes;
  }
  return true;
}

bool HashMatches(HANDLE file, std::uint64_t byte_offset, std::uint64_t bytes,
                 const Digest& expected) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
  BCRYPT_HASH_HANDLE hash = nullptr;
  Digest actual{};
  const bool ok = ::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
                  Transfer(file, byte_offset, bytes, nullptr, hash) &&
                  ::BCryptFinishHash(hash, actual.data(), static_cast<ULONG>(actual.size()), 0) >= 0;
  if (hash) ::BCryptDestroyHash(hash);
  ::BCryptCloseAlgorithmProvider(algorithm, 0);
  return ok && actual == expected;
}

int HexDigit(char digit) {
  if (digit >= '0' && digit <= '9') return digit - '0';
  if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
  return -1;
}

bool DecodeDigest(Digest* digest) {
  static const char expected[] = EPOCANVA_EMBEDDED_MSI_SHA256;
  if (sizeof(expected) != 2 * digest->size() + 1) return false;
  for (std::size_t byte_index = 0; byte_index < digest->size(); ++byte_index) {
    const int high = HexDigit(expected[byte_index * 2]);
    const int low = HexDigit(expected[byte_index * 2 + 1]);
    if (high < 0 || low < 0) return false;
    (*digest)[byte_index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return true;
}

struct Payload {
  std::uint64_t byte_offset = 0;
  std::uint64_t bytes = 0;
  Digest digest{};
};

bool ReadVerifiedPayload(HANDLE file, Payload* payload) {
  std::uint64_t file_bytes = 0;
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS64 pe{};
  if (!FileSize(file, &file_bytes) || !ReadAt(file, 0, &dos) ||
      dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
      !ReadAt(file, static_cast<std::uint64_t>(dos.e_lfanew), &pe) ||
      pe.Signature != IMAGE_NT_SIGNATURE || pe.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
      !(pe.FileHeader.Characteristics & IMAGE_FILE_EXECUTABLE_IMAGE) ||
      (pe.FileHeader.Characteristics & IMAGE_FILE_DLL) ||
      pe.FileHeader.SizeOfOptionalHeader != sizeof(pe.OptionalHeader) ||
      pe.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      pe.OptionalHeader.NumberOfRvaAndSizes != IMAGE_NUMBEROF_DIRECTORY_ENTRIES) return false;

  std::array<IMAGE_SECTION_HEADER, 96> sections{};
  const auto section_count = pe.FileHeader.NumberOfSections;
  const std::uint64_t table_byte_offset = static_cast<std::uint64_t>(dos.e_lfanew) + sizeof(pe);
  const std::uint64_t table_bytes = section_count * sizeof(IMAGE_SECTION_HEADER);
  const std::uint64_t header_bytes = pe.OptionalHeader.SizeOfHeaders;
  if (section_count == 0 || section_count > sections.size() ||
      table_byte_offset + table_bytes > header_bytes || header_bytes > file_bytes ||
      !Seek(file, table_byte_offset) ||
      !ReadExact(file, sections.data(), static_cast<DWORD>(table_bytes))) return false;

  std::uint64_t image_bytes = header_bytes;
  const auto sections_end = sections.begin() + section_count;
  std::sort(sections.begin(), sections_end, [](const auto& left, const auto& right) {
    return left.PointerToRawData < right.PointerToRawData;
  });
  for (auto section = sections.begin(); section != sections_end; ++section) {
    if (section->SizeOfRawData == 0) continue;
    if (section->PointerToRawData < image_bytes) return false;
    image_bytes = std::uint64_t{section->PointerToRawData} + section->SizeOfRawData;
  }

  const auto& certificate = pe.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_SECURITY];
  std::uint64_t payload_limit = file_bytes;
  if (certificate.VirtualAddress != 0 || certificate.Size != 0) {
    payload_limit = certificate.VirtualAddress;
    if (payload_limit % 8 != 0 || certificate.Size < 8 || certificate.Size % 8 != 0 ||
        payload_limit + certificate.Size != file_bytes) return false;
  }

  std::array<std::uint8_t, kPayloadHeaderBytes> header{};
  if (image_bytes > payload_limit || payload_limit - image_bytes < header.size() ||
      !ReadAt(file, image_bytes, &header) || !std::equal(kMagic.begin(), kMagic.end(), header.begin())) return false;
  payload->byte_offset = image_bytes + header.size();
  for (std::size_t byte_index = 0; byte_index < 8; ++byte_index) {
    payload->bytes |= std::uint64_t{header[8 + byte_index]} << (byte_index * 8);
  }
  if (payload->bytes < kMsiMagic.size() || payload->bytes > payload_limit - payload->byte_offset ||
      !DecodeDigest(&payload->digest) ||
      !std::equal(payload->digest.begin(), payload->digest.end(), header.begin() + 16)) return false;

  const std::uint64_t end_byte_offset = payload->byte_offset + payload->bytes;
  const std::uint64_t padding_bytes = payload_limit - end_byte_offset;
  std::array<std::uint8_t, 7> padding{};
  if (padding_bytes && (certificate.Size == 0 || padding_bytes > padding.size() ||
      !Seek(file, end_byte_offset) || !ReadExact(file, padding.data(), static_cast<DWORD>(padding_bytes)) ||
      std::any_of(padding.begin(), padding.end(), [](auto byte) { return byte != 0; }))) return false;
  std::array<std::uint8_t, kMsiMagic.size()> actual_magic{};
  return ReadAt(file, payload->byte_offset, &actual_magic) && actual_magic == kMsiMagic &&
         HashMatches(file, payload->byte_offset, payload->bytes, payload->digest);
}

std::wstring ModulePath() {
  std::wstring path(32768, L'\0');
  const DWORD chars = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (chars == 0 || chars >= path.size()) return {};
  path.resize(chars);
  return path;
}

class TemporaryMsi {
 public:
  ~TemporaryMsi() {
    read_lease_.Reset();
    if (!preserve_) {
      if (owns_file_) ::DeleteFileW(path_.c_str());
      directory_lease_.Reset();
      if (!directory_.empty()) ::RemoveDirectoryW(directory_.c_str());
    }
  }

  const std::wstring& Path() const { return path_; }
  void Preserve() { preserve_ = true; }

  bool Write(HANDLE source, const Payload& payload) {
    wchar_t temp[MAX_PATH]{};
    const DWORD temp_chars = ::GetTempPathW(MAX_PATH, temp);
    std::array<std::uint8_t, 16> random{};
    if (temp_chars == 0 || temp_chars >= MAX_PATH ||
        ::BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return false;
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring directory = std::wstring(temp, temp_chars) + L"EpoCanvaSetup-";
    for (auto byte : random) {
      directory.push_back(hex[byte >> 4]);
      directory.push_back(hex[byte & 0x0f]);
    }
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr)) return false;
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const BOOL created = ::CreateDirectoryW(directory.c_str(), &attributes);
    ::LocalFree(descriptor);
    if (!created) return false;
    directory_ = std::move(directory);
    directory_lease_.Reset(::CreateFileW(directory_.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION directory_info{};
    if (!directory_lease_ || !::GetFileInformationByHandle(directory_lease_.Get(), &directory_info) ||
        (directory_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    wchar_t canonical[MAX_PATH]{};
    const DWORD canonical_chars = ::GetFinalPathNameByHandleW(directory_lease_.Get(), canonical, MAX_PATH, VOLUME_NAME_DOS);
    std::wstring full = canonical_chars >= 7 && canonical_chars < MAX_PATH
        ? std::wstring(canonical, canonical_chars) : std::wstring{};
    if (full.rfind(L"\\\\?\\", 0) != 0 || full[5] != L':' || full[6] != L'\\') return false;
    directory_ = full.substr(4);
    path_ = directory_ + L"\\EpoCanva.msi";
    if (path_.size() >= MAX_PATH) return false;

    {
      Handle output(::CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_TEMPORARY, nullptr));
      if (!output) return false;
      owns_file_ = true;
      if (!Transfer(source, payload.byte_offset, payload.bytes, output.Get(), nullptr) ||
          !::FlushFileBuffers(output.Get())) return false;
    }
    read_lease_.Reset(::CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    BY_HANDLE_FILE_INFORMATION file_info{};
    std::uint64_t bytes = 0;
    return read_lease_ && ::GetFileInformationByHandle(read_lease_.Get(), &file_info) &&
           !(file_info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
           FileSize(read_lease_.Get(), &bytes) && bytes == payload.bytes &&
           HashMatches(read_lease_.Get(), 0, bytes, payload.digest);
  }

 private:
  std::wstring directory_;
  std::wstring path_;
  Handle directory_lease_;
  Handle read_lease_;
  bool owns_file_ = false;
  bool preserve_ = false;
};

int RunInstaller(TemporaryMsi& msi, const std::wstring& options) {
  wchar_t system[MAX_PATH]{};
  const UINT chars = ::GetSystemDirectoryW(system, MAX_PATH);
  if (!chars || chars >= MAX_PATH) return 3;
  const std::wstring executable = std::wstring(system, chars) + L"\\msiexec.exe";
  const std::wstring command = L"/i \"" + msi.Path() + L"\"" + options;
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
  info.lpVerb = L"runas";
  info.lpFile = executable.c_str();
  info.lpDirectory = system;
  info.lpParameters = command.c_str();
  info.nShow = SW_SHOWNORMAL;
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 3;
  const BOOL launched = ::ShellExecuteExW(&info);
  const DWORD launch_error = launched ? ERROR_SUCCESS : ::GetLastError();
  ::CoUninitialize();
  Handle process(info.hProcess);
  if (!launched) return launch_error == ERROR_CANCELLED ? ERROR_CANCELLED : 3;
  if (!process || ::WaitForSingleObject(process.Get(), INFINITE) != WAIT_OBJECT_0) {
    msi.Preserve();
    return 3;
  }
  DWORD exit_code = 0;
  return ::GetExitCodeProcess(process.Get(), &exit_code) ? static_cast<int>(exit_code) : 3;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argument_count = 0;
  LPWSTR* arguments = ::CommandLineToArgvW(::GetCommandLineW(), &argument_count);
  if (!arguments) return 4;
  const bool verify_only = argument_count == 2 && std::wstring(arguments[1]) == L"--verify";
  bool valid_arguments = true;
  std::wstring options;
  if (!verify_only) {
    for (int argument_index = 1; argument_index < argument_count; ++argument_index) {
      const std::wstring option = arguments[argument_index];
      if (option != L"/quiet" && option != L"/passive" && option != L"/norestart") {
        valid_arguments = false;
        break;
      }
      options += L" " + option;
    }
  }
  ::LocalFree(arguments);
  if (!valid_arguments) return 4;

  const std::wstring self = ModulePath();
  Handle file(::CreateFileW(self.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
  Payload payload;
  if (!file || !ReadVerifiedPayload(file.Get(), &payload)) {
    if (!verify_only) {
      ::MessageBoxW(nullptr, L"The embedded installer payload failed integrity verification.",
                    L"EpoCanva Setup", MB_ICONERROR | MB_OK);
    }
    return 2;
  }
  if (verify_only) return 0;
  TemporaryMsi temporary;
  if (!temporary.Write(file.Get(), payload)) return 3;
  return RunInstaller(temporary, options);
}
