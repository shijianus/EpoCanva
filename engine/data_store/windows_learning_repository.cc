#include "windows_learning_repository.h"

#ifndef _WIN32
#error WindowsLearningRepository requires Windows.
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace epocanva {
namespace learning {
namespace {

constexpr wchar_t kDirectory[] = L"learning-v2";
constexpr std::size_t kMaxPathUtf16Units = 30000;
constexpr DWORD kDirectoryFlags = FILE_FLAG_BACKUP_SEMANTICS |
    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL;
constexpr DWORD kFileFlags = FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OPEN_NO_RECALL;

class Handle {
 public:
  Handle() = default;
  explicit Handle(HANDLE value) : value_(value) {}
  ~Handle() { Reset(); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value_(other.Release()) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) Reset(other.Release());
    return *this;
  }
  HANDLE get() const { return value_; }
  bool valid() const { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
  void Reset(HANDLE value = INVALID_HANDLE_VALUE) {
    if (valid()) ::CloseHandle(value_);
    value_ = value;
  }
  HANDLE Release() {
    const HANDLE value = value_;
    value_ = INVALID_HANDLE_VALUE;
    return value;
  }
 private:
  HANDLE value_ = INVALID_HANDLE_VALUE;
};

struct LocalFreeDeleter {
  void operator()(void* value) const { if (value != nullptr) ::LocalFree(value); }
};
using LocalMemory = std::unique_ptr<void, LocalFreeDeleter>;

enum class Operation { kLearning, kExport, kDelete };

// A new query precedes each filesystem operation, not just construction/open.
// Closing handles and deleting our uncommitted temporary file are unconditional
// cleanup: a permission change must not leak handles or staged ID/count data.
class Gate {
 public:
  Gate(Operation operation, const PrivacyCallback& callback,
       const Selection* selection = nullptr)
      : operation_(operation), callback_(callback), selection_(selection) {}

  Status Check() {
    if (operation_ == Operation::kDelete) return Status::kOk;
    Access current;
    try {
      if (callback_) current = callback_();
    } catch (...) {
      current = {};  // Unknown, never a cached earlier grant.
    }
    if (current.privacy != PrivacyClass::kNormal) return Status::kDenied;
    if (operation_ == Operation::kLearning) {
      if (!current.learning_enabled || current.context_target == 0 ||
          current.generation == 0) return Status::kDenied;
      if (selection_ != nullptr && (!selection_->host_write_confirmed ||
          selection_->context_target != current.context_target ||
          selection_->generation != current.generation)) return Status::kStale;
      if (queried_ && (current.context_target != current_.context_target ||
          current.generation != current_.generation)) return Status::kStale;
    }
    current_ = current;
    queried_ = true;
    return Status::kOk;
  }

  Access CoreAccess() const {
    if (operation_ != Operation::kExport) return current_;
    // Explicit export is authorized by its fresh Normal callback and user action,
    // not by the focused-field learning toggle. These in-memory sentinel values
    // adapt the core's Load gate; they are never persisted or returned.
    return {PrivacyClass::kNormal, true, 1, 1};
  }
 private:
  Operation operation_;
  const PrivacyCallback& callback_;
  const Selection* selection_;
  bool queried_ = false;
  Access current_;
};

Status FileError(DWORD error) {
  if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION ||
      error == ERROR_IO_PENDING) return Status::kBusy;
  return Status::kIoError;
}

bool Missing(DWORD error) {
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

// Build explicit, protected, single-user ACLs; do not use the inherited/default
// DACL of a TSF host process. TokenUser is read afresh for each transaction.
class UserSecurity {
 public:
  bool Initialize() {
    HANDLE raw_token = nullptr;
    if (!::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY, TRUE, &raw_token)) {
      if (::GetLastError() != ERROR_NO_TOKEN ||
          !::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw_token))
        return false;
    }
    Handle token(raw_token);
    DWORD token_bytes = 0;
    if (::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &token_bytes) ||
        ::GetLastError() != ERROR_INSUFFICIENT_BUFFER || token_bytes == 0 ||
        token_bytes > 65536) return false;
    std::vector<BYTE> token_data(token_bytes);
    if (!::GetTokenInformation(token.get(), TokenUser, token_data.data(),
                              token_bytes, &token_bytes)) return false;
    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(token_data.data());
    if (!::IsValidSid(token_user->User.Sid)) return false;
    const DWORD sid_bytes = ::GetLengthSid(token_user->User.Sid);
    sid_.resize(sid_bytes);
    if (!::CopySid(sid_bytes, sid_.data(), token_user->User.Sid)) return false;
    return MakeDescriptor(false, &file_acl_, &file_descriptor_, &file_attributes_) &&
           MakeDescriptor(true, &directory_acl_, &directory_descriptor_,
                          &directory_attributes_);
  }

  SECURITY_ATTRIBUTES* attributes(bool directory) {
    return directory ? &directory_attributes_ : &file_attributes_;
  }

  Status Check(HANDLE handle, bool directory, Gate& gate) const {
    Status status = gate.Check();
    if (status != Status::kOk) return status;
    PSID owner = nullptr;
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    if (::GetSecurityInfo(handle, SE_FILE_OBJECT,
          OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
          &acl, nullptr, &raw_descriptor) != ERROR_SUCCESS) return Status::kIoError;
    LocalMemory descriptor(raw_descriptor);
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD descriptor_revision = 0;
    if (owner == nullptr || !::IsValidSid(owner) ||
        !::EqualSid(owner, const_cast<BYTE*>(sid_.data())) || acl == nullptr ||
        !::IsValidAcl(acl) || acl->AceCount != 1 ||
        !::GetSecurityDescriptorControl(raw_descriptor, &control, &descriptor_revision) ||
        (control & SE_DACL_PROTECTED) == 0) return Status::kIoError;
    void* raw_ace = nullptr;
    if (!::GetAce(acl, 0, &raw_ace)) return Status::kIoError;
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
    const BYTE expected_flags = directory ? OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE : 0;
    if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE ||
        ace->Header.AceFlags != expected_flags || ace->Mask != FILE_ALL_ACCESS ||
        ace->Header.AceSize < sizeof(ACCESS_ALLOWED_ACE)) return Status::kIoError;
    PSID ace_sid = const_cast<DWORD*>(&ace->SidStart);
    if (!::IsValidSid(ace_sid) || !::EqualSid(ace_sid, const_cast<BYTE*>(sid_.data())))
      return Status::kIoError;
    return Status::kOk;
  }

 private:
  bool MakeDescriptor(bool directory, std::vector<BYTE>* bytes,
                      SECURITY_DESCRIPTOR* descriptor, SECURITY_ATTRIBUTES* attributes) {
    const DWORD acl_bytes = static_cast<DWORD>(sizeof(ACL) +
        sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD) + sid_.size());
    bytes->resize(acl_bytes);
    auto* acl = reinterpret_cast<ACL*>(bytes->data());
    if (!::InitializeAcl(acl, acl_bytes, ACL_REVISION) ||
        !::AddAccessAllowedAceEx(acl, ACL_REVISION,
          directory ? OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE : 0,
          FILE_ALL_ACCESS, sid_.data()) ||
        !::InitializeSecurityDescriptor(descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !::SetSecurityDescriptorOwner(descriptor, sid_.data(), FALSE) ||
        !::SetSecurityDescriptorDacl(descriptor, TRUE, acl, FALSE) ||
        !::SetSecurityDescriptorControl(descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
      return false;
    *attributes = {sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    return true;
  }

  std::vector<BYTE> sid_;
  std::vector<BYTE> file_acl_;
  std::vector<BYTE> directory_acl_;
  SECURITY_DESCRIPTOR file_descriptor_ = {};
  SECURITY_DESCRIPTOR directory_descriptor_ = {};
  SECURITY_ATTRIBUTES file_attributes_ = {};
  SECURITY_ATTRIBUTES directory_attributes_ = {};
};

bool CloudTag(DWORD tag) {
  return (tag & ~static_cast<DWORD>(IO_REPARSE_TAG_CLOUD_MASK)) == IO_REPARSE_TAG_CLOUD;
}

Status CheckObject(HANDLE handle, bool directory, Gate& gate,
                   BY_HANDLE_FILE_INFORMATION* out = nullptr) {
  Status status = gate.Check();
  if (status != Status::kOk) return status;
  if (::GetFileType(handle) != FILE_TYPE_DISK) return Status::kIoError;
  status = gate.Check();
  if (status != Status::kOk) return status;
  FILE_ATTRIBUTE_TAG_INFO tag = {};
  if (!::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)))
    return Status::kIoError;
  if (((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)
    return Status::kIoError;
  if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
      (IsReparseTagNameSurrogate(tag.ReparseTag) || !CloudTag(tag.ReparseTag)))
    return Status::kIoError;
  // Cloud tags are not path redirects. Never hydrate an offline/recall file.
  // A caller must choose a non-synced local root if external synchronization is
  // unwanted: this library cannot constrain another Windows process/provider.
  if (!directory && (tag.FileAttributes & (FILE_ATTRIBUTE_OFFLINE |
          FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS)) != 0)
    return Status::kIoError;
  status = gate.Check();
  if (status != Status::kOk) return status;
  BY_HANDLE_FILE_INFORMATION info = {};
  if (!::GetFileInformationByHandle(handle, &info) ||
      (!directory && info.nNumberOfLinks != 1)) return Status::kIoError;
  if (out != nullptr) *out = info;
  return Status::kOk;
}

bool ReservedComponent(std::wstring component) {
  for (auto& unit : component) {
    if (unit >= L'a' && unit <= L'z') unit -= L'a' - L'A';
  }
  component.resize(component.find(L'.') == std::wstring::npos ?
                   component.size() : component.find(L'.'));
  if (component == L"CON" || component == L"PRN" || component == L"AUX" ||
      component == L"NUL" || component == L"CONIN$" || component == L"CONOUT$") return true;
  return component.size() == 4 &&
      (component.compare(0, 3, L"COM") == 0 || component.compare(0, 3, L"LPT") == 0) &&
      ((component[3] >= L'0' && component[3] <= L'9') ||
       component[3] == L'\u00b9' || component[3] == L'\u00b2' || component[3] == L'\u00b3');
}

// Pure syntax validation happens without GetFullPathName/filesystem resolution.
// UNC, extended caller paths, device names, drive-relative paths, ADS, dot
// traversal, trailing-dot/space aliases and malformed UTF-16 are not admitted.
bool ParseRoot(const std::filesystem::path& root, std::wstring* drive,
               std::vector<std::wstring>* components) {
  std::wstring path = root.native();
  if (path.size() < 3 || path.size() > kMaxPathUtf16Units) return false;
  std::replace(path.begin(), path.end(), L'/', L'\\');
  if (!((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
      path[1] != L':' || path[2] != L'\\') return false;
  *drive = path.substr(0, 2);
  if (path.size() > 3 && path.back() == L'\\') path.pop_back();
  for (std::size_t start_utf16_unit = 3; start_utf16_unit < path.size();) {
    const auto next_utf16_unit = path.find(L'\\', start_utf16_unit);
    const auto component = path.substr(start_utf16_unit,
        next_utf16_unit == path.npos ? path.npos : next_utf16_unit - start_utf16_unit);
    if (component.empty() || component.size() > 255 || component == L"." ||
        component == L".." || component.back() == L'.' || component.back() == L' ' ||
        ReservedComponent(component)) return false;
    for (std::size_t unit_index = 0; unit_index < component.size(); ++unit_index) {
      const wchar_t unit = component[unit_index];
      if (unit < 32 || unit == L':' || unit == L'<' || unit == L'>' || unit == L'"' ||
          unit == L'|' || unit == L'?' || unit == L'*') return false;
      if (unit >= 0xd800 && unit <= 0xdbff) {
        if (++unit_index == component.size() || component[unit_index] < 0xdc00 ||
            component[unit_index] > 0xdfff) return false;
      } else if (unit >= 0xdc00 && unit <= 0xdfff) {
        return false;
      }
    }
    components->push_back(component);
    if (next_utf16_unit == path.npos) break;
    start_utf16_unit = next_utf16_unit + 1;
  }
  return true;
}

std::wstring CatalogStem(const Catalog& catalog) {
  constexpr wchar_t hex[] = L"0123456789abcdef";
  std::wstring stem = L"n-";
  for (const unsigned char byte : catalog.name_space) {
    stem.push_back(hex[byte >> 4]);
    stem.push_back(hex[byte & 15]);
  }
  return stem;
}

class Transaction {
 public:
  explicit Transaction(Gate& gate) : gate_(gate) {}

  Status Open(const std::filesystem::path& root, const Catalog& catalog,
              bool create_directory) {
    std::wstring drive;
    std::vector<std::wstring> components;
    if (!ParseRoot(root, &drive, &components)) return Status::kIoError;
    Status status = gate_.Check();
    if (status != Status::kOk) return status;
    // QueryDosDevice only queries the local DOS-device namespace. Restrict its
    // result to physical disk volume names BEFORE opening anything; even a drive
    // mapped to a UNC share, SUBST, WebDAV or another object is never followed.
    wchar_t device_buffer[1024] = {};
    if (::QueryDosDeviceW(drive.c_str(), device_buffer, 1024) == 0) return Status::kIoError;
    const std::wstring device(device_buffer);
    const std::wstring prefix = L"\\Device\\HarddiskVolume";
    if (device.compare(0, prefix.size(), prefix) != 0 || device.size() == prefix.size() ||
        !std::all_of(device.begin() + static_cast<std::ptrdiff_t>(prefix.size()),
                     device.end(), [](wchar_t unit) { return unit >= L'0' && unit <= L'9'; }))
      return Status::kIoError;
    if (!security_.Initialize()) return Status::kIoError;
    // Use the resolved physical volume, not a DOS drive letter that another
    // thread/process could remap after the preceding check.
    std::wstring current = L"\\\\?\\GLOBALROOT" + device + L"\\";
    status = PinDirectory(current, false);
    if (status != Status::kOk) return status;
    status = gate_.Check();
    if (status != Status::kOk) return status;
    DWORD volume_flags = 0;
    if (!::GetVolumeInformationByHandleW(directories_.back().get(), nullptr, 0,
          nullptr, nullptr, &volume_flags, nullptr, 0) ||
        (volume_flags & FILE_PERSISTENT_ACLS) == 0) return Status::kIoError;
    for (const auto& component : components) {
      if (current.back() != L'\\') current += L'\\';
      current += component;
      status = PinDirectory(current, false);
      if (status != Status::kOk) return status;
    }
    if (current.back() != L'\\') current += L'\\';
    directory_ = current + kDirectory;
    status = PinDirectory(directory_, true);
    if (status != Status::kOk) {
      if (!missing_) return status;
      missing_ = false;
      if (!create_directory) { missing_ = true; return Status::kOk; }
      status = gate_.Check();
      if (status != Status::kOk) return status;
      if (!::CreateDirectoryW(directory_.c_str(), security_.attributes(true)) &&
          ::GetLastError() != ERROR_ALREADY_EXISTS) return FileError(::GetLastError());
      status = PinDirectory(directory_, true);
      if (status != Status::kOk) return status;
    }
    const std::wstring stem = directory_ + L"\\" + CatalogStem(catalog);
    data_path_ = stem + L".dat";
    temporary_path_ = stem + L".tmp";
    status = gate_.Check();
    if (status != Status::kOk) return status;
    lock_.Reset(::CreateFileW((stem + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE |
          READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE, security_.attributes(false),
          OPEN_ALWAYS, kFileFlags, nullptr));
    if (!lock_.valid()) return FileError(::GetLastError());
    BY_HANDLE_FILE_INFORMATION info = {};
    status = CheckObject(lock_.get(), false, gate_, &info);
    if (status != Status::kOk) return status;
    if (info.nFileSizeHigh != 0 || info.nFileSizeLow != 0) return Status::kInvalidFormat;
    status = security_.Check(lock_.get(), false, gate_);
    if (status != Status::kOk) return status;
    status = gate_.Check();
    if (status != Status::kOk) return status;
    OVERLAPPED byte_zero = {};  // UTF-8 byte offset zero, lock extent one byte.
    if (!::LockFileEx(lock_.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                     0, 1, 0, &byte_zero)) return FileError(::GetLastError());
    return Status::kOk;
  }

  bool missing() const { return missing_; }

  Status Load(LearningStore* store, bool* read_only = nullptr) {
    if (missing_) return Status::kOk;
    Handle file;
    bool absent = false;
    BY_HANDLE_FILE_INFORMATION info = {};
    Status status = OpenFile(data_path_, GENERIC_READ | READ_CONTROL, &file, &absent, &info);
    if (status != Status::kOk || absent) return status;
    if (read_only != nullptr) *read_only = (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    if (info.nFileSizeHigh != 0 || info.nFileSizeLow > kMaxStoreBytes)
      return Status::kLimitExceeded;
    std::string bytes(info.nFileSizeLow, '\0');
    std::size_t offset_utf8_byte = 0;
    while (offset_utf8_byte < bytes.size()) {
      status = gate_.Check();
      if (status != Status::kOk) return status;
      DWORD read_utf8_bytes = 0;
      if (!::ReadFile(file.get(), bytes.data() + offset_utf8_byte,
            static_cast<DWORD>(bytes.size() - offset_utf8_byte), &read_utf8_bytes, nullptr) ||
          read_utf8_bytes == 0) return Status::kIoError;
      offset_utf8_byte += read_utf8_bytes;
    }
    status = gate_.Check();
    if (status != Status::kOk) return status;
    return store->Load(bytes, gate_.CoreAccess());
  }

  Status Save(const std::string& bytes) {
    if (bytes.size() > kMaxStoreBytes) return Status::kLimitExceeded;
    Status status = Remove(temporary_path_);
    if (status != Status::kOk) return status;
    status = gate_.Check();
    if (status != Status::kOk) return status;
    Handle temporary(::CreateFileW(temporary_path_.c_str(), GENERIC_READ | GENERIC_WRITE |
        DELETE | READ_CONTROL, 0, security_.attributes(false), CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | kFileFlags, nullptr));
    if (!temporary.valid()) return FileError(::GetLastError());
    // Unconditional handle-based cleanup also covers exceptions and callback
    // revocation. It never follows a pathname or reads staged content.
    struct Cleanup {
      HANDLE file;
      bool published = false;
      ~Cleanup() {
        if (!published) {
          FILE_DISPOSITION_INFO remove = {TRUE};
          (void)::SetFileInformationByHandle(file, FileDispositionInfo, &remove, sizeof(remove));
        }
      }
    } cleanup{temporary.get()};
    status = CheckObject(temporary.get(), false, gate_);
    if (status != Status::kOk) return status;
    status = security_.Check(temporary.get(), false, gate_);
    if (status != Status::kOk) return status;
    std::size_t offset_utf8_byte = 0;
    while (offset_utf8_byte < bytes.size()) {
      status = gate_.Check();  // fresh immediately before any ID/count bytes are written
      if (status != Status::kOk) return status;
      DWORD written_utf8_bytes = 0;
      if (!::WriteFile(temporary.get(), bytes.data() + offset_utf8_byte,
            static_cast<DWORD>(bytes.size() - offset_utf8_byte), &written_utf8_bytes, nullptr) ||
          written_utf8_bytes == 0) return Status::kIoError;
      offset_utf8_byte += written_utf8_bytes;
    }
    status = gate_.Check();
    if (status != Status::kOk) return status;
    if (!::FlushFileBuffers(temporary.get())) return Status::kIoError;
    // Rename the already validated temporary HANDLE, not a re-resolved source
    // pathname. All directories remain pinned without write/delete sharing, and
    // the distinct stable lock remains held across this atomic same-volume move.
    const std::size_t path_utf16_bytes = data_path_.size() * sizeof(wchar_t);
    const std::size_t rename_bytes = sizeof(FILE_RENAME_INFO) + path_utf16_bytes;
    std::vector<BYTE> rename_buffer(rename_bytes, 0);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(rename_buffer.data());
    // Windows 10+ POSIX replacement tolerates an already-open old target (for
    // example an indexer or cloud provider) without retargeting its handle. The
    // old handle still observes its old file; later opens get the new file.
    // No IGNORE_READONLY flag: read-only targets remain real I/O errors.
    rename->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = static_cast<DWORD>(path_utf16_bytes);
    std::memcpy(rename->FileName, data_path_.data(), path_utf16_bytes);
    status = gate_.Check();  // no filesystem work between this query and publication
    if (status != Status::kOk) return status;
    if (!::SetFileInformationByHandle(temporary.get(), FileRenameInfoEx, rename,
                                     static_cast<DWORD>(rename_bytes)))
      return FileError(::GetLastError());
    cleanup.published = true;
    return Status::kOk;
  }

  Status DeleteFiles() {
    if (missing_) return Status::kOk;
    Status status = Remove(data_path_);
    if (status != Status::kOk) return status;
    return Remove(temporary_path_);
  }

 private:
  Status PinDirectory(const std::wstring& path, bool private_directory) {
    Status status = gate_.Check();
    if (status != Status::kOk) return status;
    Handle handle(::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, kDirectoryFlags, nullptr));
    if (!handle.valid()) {
      const DWORD error = ::GetLastError();
      missing_ = private_directory && Missing(error);
      return FileError(error);
    }
    status = CheckObject(handle.get(), true, gate_);
    if (status != Status::kOk) return status;
    if (private_directory) {
      status = security_.Check(handle.get(), true, gate_);
      if (status != Status::kOk) return status;
    }
    directories_.push_back(std::move(handle));
    return Status::kOk;
  }

  Status OpenFile(const std::wstring& path, DWORD access, Handle* file,
                  bool* absent, BY_HANDLE_FILE_INFORMATION* info = nullptr) {
    *absent = false;
    Status status = gate_.Check();
    if (status != Status::kOk) return status;
    file->Reset(::CreateFileW(path.c_str(), access, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, kFileFlags, nullptr));
    if (!file->valid()) {
      const DWORD error = ::GetLastError();
      if (Missing(error)) { *absent = true; return Status::kOk; }
      return FileError(error);
    }
    status = CheckObject(file->get(), false, gate_, info);
    if (status != Status::kOk) return status;
    return security_.Check(file->get(), false, gate_);
  }

  Status Remove(const std::wstring& path) {
    Handle file;
    bool absent = false;
    Status status = OpenFile(path, DELETE | FILE_READ_ATTRIBUTES | READ_CONTROL,
                             &file, &absent);
    if (status != Status::kOk || absent) return status;
    status = gate_.Check();
    if (status != Status::kOk) return status;
    FILE_DISPOSITION_INFO remove = {TRUE};
    if (!::SetFileInformationByHandle(file.get(), FileDispositionInfo, &remove, sizeof(remove)))
      return FileError(::GetLastError());
    return Status::kOk;
  }

  Gate& gate_;
  UserSecurity security_;
  std::vector<Handle> directories_;
  Handle lock_;  // closes before directory handles; CloseHandle releases LockFileEx
  bool missing_ = false;
  std::wstring directory_;
  std::wstring data_path_;
  std::wstring temporary_path_;
};

Status ValidateSelection(const LearningStore& store, const Selection& selection) {
  if (selection.lexeme_ids.empty() || selection.lexeme_ids.size() > kMaxCommitLexemes)
    return Status::kLimitExceeded;
  if ((selection.previous_id != 0 && !store.Admits(selection.previous_id)) ||
      !std::all_of(selection.lexeme_ids.begin(), selection.lexeme_ids.end(),
                   [&store](LexemeId id) { return store.Admits(id); }))
    return Status::kInvalidLexeme;
  return Status::kOk;
}

}  // namespace

WindowsLearningRepository::WindowsLearningRepository(std::filesystem::path root)
    : root_(std::move(root)) {}

Status WindowsLearningRepository::Record(const Catalog& catalog,
    const Selection& selection, const PrivacyCallback& privacy) const noexcept {
  try {
    Gate gate(Operation::kLearning, privacy, &selection);
    Status status = gate.Check();
    if (status != Status::kOk) return status;
    auto store = LearningStore::Create(catalog);
    if (!store) return Status::kInvalidCatalog;
    status = ValidateSelection(*store, selection);
    if (status != Status::kOk) return status;
    Transaction transaction(gate);
    status = transaction.Open(root_, catalog, true);
    if (status != Status::kOk) return status;
    bool read_only = false;
    status = transaction.Load(store.get(), &read_only);
    if (status != Status::kOk) return status;
    if (read_only) return Status::kIoError;
    status = gate.Check();
    if (status != Status::kOk) return status;
    status = store->Record(selection, gate.CoreAccess());
    if (status != Status::kOk) return status;
    std::string bytes;
    status = gate.Check();
    if (status != Status::kOk) return status;
    status = store->Serialize(gate.CoreAccess(), &bytes);
    if (status != Status::kOk) return status;
    return transaction.Save(bytes);
  } catch (...) {
    return Status::kIoError;
  }
}

Status WindowsLearningRepository::LoadSnapshot(const Catalog& catalog,
    const PrivacyCallback& privacy, Snapshot* out) const noexcept {
  if (out != nullptr) *out = {};
  try {
    Gate gate(Operation::kLearning, privacy);
    Status status = gate.Check();
    if (status != Status::kOk) return status;
    if (out == nullptr) return Status::kInvalidFormat;
    auto store = LearningStore::Create(catalog);
    if (!store) return Status::kInvalidCatalog;
    Transaction transaction(gate);
    status = transaction.Open(root_, catalog, false);
    if (status != Status::kOk) return status;
    status = transaction.Load(store.get());
    if (status != Status::kOk) return status;
    status = gate.Check();
    if (status != Status::kOk) return status;
    return store->Read(gate.CoreAccess(), out);
  } catch (...) {
    if (out != nullptr) *out = {};
    return Status::kIoError;
  }
}

Status WindowsLearningRepository::Delete(const Catalog& catalog, bool explicit_user_action,
    const PrivacyCallback& privacy) const noexcept {
  if (!explicit_user_action) return Status::kDenied;
  try {
    if (!LearningStore::Create(catalog)) return Status::kInvalidCatalog;
    Gate gate(Operation::kDelete, privacy);  // explicit control action: never invokes privacy
    Transaction transaction(gate);
    const Status status = transaction.Open(root_, catalog, false);
    if (status != Status::kOk) return status;
    return transaction.DeleteFiles();
  } catch (...) {
    return Status::kIoError;
  }
}

Status WindowsLearningRepository::Export(const Catalog& catalog, bool explicit_user_action,
    const PrivacyCallback& privacy, std::string* out) const noexcept {
  if (out != nullptr) out->clear();
  if (!explicit_user_action) return Status::kDenied;
  try {
    Gate gate(Operation::kExport, privacy);
    Status status = gate.Check();
    if (status != Status::kOk) return status;
    if (out == nullptr) return Status::kInvalidFormat;
    auto store = LearningStore::Create(catalog);
    if (!store) return Status::kInvalidCatalog;
    Transaction transaction(gate);
    status = transaction.Open(root_, catalog, false);
    if (status != Status::kOk) return status;
    status = transaction.Load(store.get());
    if (status != Status::kOk) return status;
    std::string bytes;
    status = store->Export(true, &bytes);
    if (status != Status::kOk) return status;
    if (bytes.size() > kMaxStoreBytes) return Status::kLimitExceeded;
    status = gate.Check();
    if (status != Status::kOk) return status;
    *out = std::move(bytes);
    return Status::kOk;
  } catch (...) {
    return Status::kIoError;
  }
}

}  // namespace learning
}  // namespace epocanva
