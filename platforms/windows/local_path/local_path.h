#ifndef EPOCANVA_WINDOWS_LOCAL_PATH_H_
#define EPOCANVA_WINDOWS_LOCAL_PATH_H_

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace epocanva::windows::local_path {

// Only caller-supplied drive-absolute paths are accepted. Never resolve a UNC,
// DOS device, SUBST or redirector mapping with filesystem I/O. All string indices
// and component sizes below are UTF-16 code units.
struct PreparedPath {
  std::filesystem::path display;
  std::wstring volume_root;
  std::vector<std::wstring> components;
  std::filesystem::path physical;
};

inline bool ReservedComponent(std::wstring value) {
  for (auto& unit : value) {
    if (unit >= L'a' && unit <= L'z') unit -= L'a' - L'A';
  }
  value.resize(value.find(L'.') == value.npos ? value.size() : value.find(L'.'));
  if (value == L"CON" || value == L"PRN" || value == L"AUX" || value == L"NUL" ||
      value == L"CONIN$" || value == L"CONOUT$") return true;
  return value.size() == 4 &&
      (value.compare(0, 3, L"COM") == 0 || value.compare(0, 3, L"LPT") == 0) &&
      ((value[3] >= L'0' && value[3] <= L'9') || value[3] == L'\u00b9' ||
       value[3] == L'\u00b2' || value[3] == L'\u00b3');
}

inline bool SafeComponent(const std::wstring& value) {
  if (value.empty() || value.size() > 255 || value == L"." || value == L".." ||
      value.back() == L'.' || value.back() == L' ' || ReservedComponent(value)) return false;
  for (std::size_t index_utf16 = 0; index_utf16 < value.size(); ++index_utf16) {
    const wchar_t unit = value[index_utf16];
    if (unit < 32 || std::wstring(L"\\/:<>\"|?*").find(unit) != std::wstring::npos)
      return false;
    if (unit >= 0xd800 && unit <= 0xdbff) {
      if (++index_utf16 == value.size() || value[index_utf16] < 0xdc00 ||
          value[index_utf16] > 0xdfff) return false;
    } else if (unit >= 0xdc00 && unit <= 0xdfff) {
      return false;
    }
  }
  return true;
}

inline bool Parse(const std::filesystem::path& input, PreparedPath* out) {
  *out = {};
  std::wstring value = input.native();
  std::replace(value.begin(), value.end(), L'/', L'\\');
  if (value.size() < 3 || value.size() > 30000 || value[1] != L':' || value[2] != L'\\' ||
      !((value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z')))
    return false;
  if (value.size() > 3 && value.back() == L'\\') value.pop_back();
  for (std::size_t begin_utf16 = 3; begin_utf16 < value.size();) {
    const auto end_utf16 = value.find(L'\\', begin_utf16);
    const auto component = value.substr(begin_utf16,
        end_utf16 == value.npos ? value.npos : end_utf16 - begin_utf16);
    if (!SafeComponent(component)) return false;
    out->components.push_back(component);
    if (end_utf16 == value.npos) break;
    begin_utf16 = end_utf16 + 1;
    if (begin_utf16 == value.size()) return false;
  }
  out->display = value;
  return true;
}

inline bool PhysicalDiskDevice(const std::wstring& device) {
  const std::wstring prefix = L"\\Device\\HarddiskVolume";
  return device.compare(0, prefix.size(), prefix) == 0 && device.size() > prefix.size() &&
      std::all_of(device.begin() + static_cast<std::ptrdiff_t>(prefix.size()), device.end(),
                  [](wchar_t unit) { return unit >= L'0' && unit <= L'9'; });
}

// QueryDosDevice is a local namespace query, not an open on the supplied path.
// Using its physical target for subsequent I/O avoids a later DOS-drive remap.
inline bool Prepare(const std::filesystem::path& input, PreparedPath* out) {
  if (!Parse(input, out)) return false;
  wchar_t device[32768]{};
  if (!::QueryDosDeviceW(out->display.native().substr(0, 2).c_str(), device, 32768) ||
      !PhysicalDiskDevice(device)) return false;
  out->volume_root = L"\\\\?\\GLOBALROOT" + std::wstring(device) + L"\\";
  out->physical = out->volume_root;
  for (const auto& component : out->components) out->physical /= component;
  return true;
}

inline bool AllowedAttributes(DWORD attributes, DWORD tag, bool directory) {
  if (((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) return false;
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    const bool cloud = (tag & ~static_cast<DWORD>(IO_REPARSE_TAG_CLOUD_MASK)) ==
                       IO_REPARSE_TAG_CLOUD;
    if (IsReparseTagNameSurrogate(tag) || !cloud) return false;
  }
  // Non-surrogate cloud directory metadata is not a redirect (e.g. OneDrive).
  // Never read/hydrate an offline or recall-on-access file. An external provider's
  // synchronization is outside our control; this is not an OS network sandbox.
  return directory || (attributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN |
                                     FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) == 0;
}

class PinnedPath {
 public:
  PinnedPath() = default;
  ~PinnedPath() { Reset(); }
  PinnedPath(const PinnedPath&) = delete;
  PinnedPath& operator=(const PinnedPath&) = delete;
  PinnedPath(PinnedPath&& other) noexcept { Swap(other); }
  PinnedPath& operator=(PinnedPath&& other) noexcept {
    if (this != &other) { Reset(); Swap(other); }
    return *this;
  }

  void Reset() {
    for (auto it = handles_.rbegin(); it != handles_.rend(); ++it) ::CloseHandle(*it);
    handles_.clear();
    path_.clear();
    exists_ = false;
    error_ = ERROR_SUCCESS;
  }
  bool Open(const std::filesystem::path& input, bool directory, bool allow_missing = false) {
    PreparedPath prepared;
    if (!Prepare(input, &prepared)) { error_ = ERROR_INVALID_NAME; return false; }
    return Open(prepared, directory, allow_missing);
  }
  bool Open(const PreparedPath& prepared, bool directory, bool allow_missing = false) {
    Reset();
    if (prepared.volume_root.empty() || (!directory && prepared.components.empty())) {
      error_ = ERROR_INVALID_NAME;
      return false;
    }
    std::filesystem::path current = prepared.volume_root;
    if (!Pin(current, true)) return false;
    for (std::size_t ordinal = 0; ordinal < prepared.components.size(); ++ordinal) {
      current /= prepared.components[ordinal];
      if (!Pin(current, directory || ordinal + 1 < prepared.components.size())) {
        if (!allow_missing || (error_ != ERROR_FILE_NOT_FOUND && error_ != ERROR_PATH_NOT_FOUND))
          return false;
        path_ = prepared.physical;
        return true;  // Every existing ancestor remains pinned; no creation yet.
      }
    }
    exists_ = true;
    path_ = prepared.physical;
    return true;
  }

  // One directory at a time; every parent remains pinned before CreateDirectoryW,
  // and each result is re-opened without following reparses before the next child.
  // A competitor can replace a newly created leaf before its pin; that leaf is
  // validated rather than trusted. No unvalidated child is ever traversed here.
  bool CreateDirectories(const PreparedPath& prepared) {
    Reset();
    if (prepared.volume_root.empty()) { error_ = ERROR_INVALID_NAME; return false; }
    std::filesystem::path current = prepared.volume_root;
    if (!Pin(current, true)) return false;
    for (const auto& component : prepared.components) {
      current /= component;
      if (!Pin(current, true)) {
        if (error_ != ERROR_FILE_NOT_FOUND && error_ != ERROR_PATH_NOT_FOUND) return false;
        if (!::CreateDirectoryW(current.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
          error_ = ::GetLastError();
          return false;
        }
        if (!Pin(current, true)) return false;
      }
    }
    exists_ = true;
    path_ = current;
    return true;
  }

  const std::filesystem::path& path() const { return path_; }
  bool exists() const { return exists_; }
  DWORD error() const { return error_; }
  HANDLE handle() const { return handles_.empty() ? INVALID_HANDLE_VALUE : handles_.back(); }

 private:
  bool Pin(const std::filesystem::path& path, bool directory) {
    const HANDLE handle = ::CreateFileW(path.c_str(),
        directory ? FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY : GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL |
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { error_ = ::GetLastError(); return false; }
    FILE_ATTRIBUTE_TAG_INFO info{};
    const bool ok = ::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info,
                                                  sizeof(info)) != FALSE &&
        ::GetFileType(handle) == FILE_TYPE_DISK &&
        AllowedAttributes(info.FileAttributes, info.ReparseTag, directory);
    if (!ok) { ::CloseHandle(handle); error_ = ERROR_ACCESS_DENIED; return false; }
    try { handles_.push_back(handle); }
    catch (...) { ::CloseHandle(handle); throw; }
    error_ = ERROR_SUCCESS;
    return true;
  }
  void Swap(PinnedPath& other) noexcept {
    handles_.swap(other.handles_);
    path_.swap(other.path_);
    std::swap(exists_, other.exists_);
    std::swap(error_, other.error_);
  }
  std::vector<HANDLE> handles_;
  std::filesystem::path path_;
  bool exists_ = false;
  DWORD error_ = ERROR_SUCCESS;
};

}  // namespace epocanva::windows::local_path
#endif
