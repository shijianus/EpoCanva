#include "windows_learning_repository.h"

#include <windows.h>
#include <aclapi.h>
#include <winioctl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace epocanva::learning;

int failures = 0;
void Check(bool value, const char* label) {
  if (!value) { ++failures; std::fprintf(stderr, "FAIL %s\n", label); }
}
void Expect(Status actual, Status expected, const char* label) {
  if (actual != expected) {
    ++failures;
    std::fprintf(stderr, "FAIL %s (got %s, expected %s)\n", label,
                 StatusName(actual), StatusName(expected));
  }
}

class Handle {
 public:
  Handle() = default;
  explicit Handle(HANDLE value) : value_(value) {}
  ~Handle() { Reset(); }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle(Handle&& other) noexcept : value_(other.value_) { other.value_ = INVALID_HANDLE_VALUE; }
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) { Reset(other.value_); other.value_ = INVALID_HANDLE_VALUE; }
    return *this;
  }
  HANDLE get() const { return value_; }
  bool valid() const { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
  void Reset(HANDLE value = INVALID_HANDLE_VALUE) {
    if (valid()) ::CloseHandle(value_);
    value_ = value;
  }
 private:
  HANDLE value_ = INVALID_HANDLE_VALUE;
};

Catalog TestCatalog() { return {"en.test-v2", std::string(64, 'a'), {11, 22, 33}}; }
Access Normal() { return {PrivacyClass::kNormal, true, 41, 9}; }
Selection Confirmed() { return {41, 9, true, 33, {11, 22}}; }
const PrivacyCallback allow = [] { return Normal(); };

fs::path StoreDirectory(const fs::path& root) { return root / L"learning-v2"; }
fs::path Leaf(const fs::path& root, const wchar_t* extension,
              const Catalog& catalog = TestCatalog()) {
  std::wstring stem = L"n-";
  constexpr wchar_t hex[] = L"0123456789abcdef";
  for (const unsigned char byte : catalog.name_space) {
    stem.push_back(hex[byte >> 4]);
    stem.push_back(hex[byte & 15]);
  }
  return StoreDirectory(root) / (stem + extension);
}
fs::path NewRoot(const fs::path& base, const wchar_t* name) {
  const fs::path root = base / (std::wstring(name) + L"-" +
      std::to_wstring(::GetCurrentProcessId()) + L"-" + std::to_wstring(::GetTickCount64()));
  fs::create_directories(root);
  return root;
}

std::string ReadFixture(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  Check(input.good(), "fixture read opened");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void WriteFixture(const fs::path& path, const std::string& bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  Check(output.good(), "fixture write completed");
}

std::vector<BYTE> CurrentSid() {
  HANDLE raw = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw)) return {};
  Handle token(raw);
  DWORD token_bytes = 0;
  (void)::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &token_bytes);
  std::vector<BYTE> buffer(token_bytes);
  if (!::GetTokenInformation(token.get(), TokenUser, buffer.data(), token_bytes, &token_bytes))
    return {};
  const PSID source = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
  std::vector<BYTE> sid(::GetLengthSid(source));
  if (!::CopySid(static_cast<DWORD>(sid.size()), sid.data(), source)) return {};
  return sid;
}

bool ApplyAcl(const fs::path& path, bool directory, DWORD mask, bool add_world = false) {
  auto sid = CurrentSid();
  if (sid.empty()) return false;
  EXPLICIT_ACCESS_W entries[2] = {};
  entries[0].grfAccessPermissions = mask;
  entries[0].grfAccessMode = SET_ACCESS;
  entries[0].grfInheritance = directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
  entries[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
  entries[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
  entries[0].Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid.data());
  BYTE world[SECURITY_MAX_SID_SIZE] = {};
  DWORD world_bytes = sizeof(world);
  if (add_world) {
    if (!::CreateWellKnownSid(WinWorldSid, nullptr, world, &world_bytes)) return false;
    entries[1] = entries[0];
    entries[1].grfAccessPermissions = FILE_GENERIC_READ;
    entries[1].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    entries[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(world);
  }
  PACL acl = nullptr;
  if (::SetEntriesInAclW(add_world ? 2 : 1, entries, nullptr, &acl) != ERROR_SUCCESS) return false;
  const DWORD result = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
      nullptr, nullptr, acl, nullptr);
  ::LocalFree(acl);
  return result == ERROR_SUCCESS;
}

bool RestrictedAcl(const fs::path& path, bool directory) {
  Handle file(::CreateFileW(path.c_str(), READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0), nullptr));
  if (!file.valid()) return false;
  PSID owner = nullptr;
  PACL acl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (::GetSecurityInfo(file.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION |
      DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr, &descriptor) != ERROR_SUCCESS)
    return false;
  const auto sid = CurrentSid();
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  void* raw_ace = nullptr;
  bool ok = !sid.empty() && owner != nullptr &&
      ::EqualSid(owner, const_cast<BYTE*>(sid.data())) && acl != nullptr &&
      ::IsValidAcl(acl) && acl->AceCount == 1 &&
      ::GetSecurityDescriptorControl(descriptor, &control, &revision) &&
      (control & SE_DACL_PROTECTED) != 0 && ::GetAce(acl, 0, &raw_ace);
  if (ok) {
    const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw_ace);
    ok = ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE &&
        ace->Header.AceFlags == (directory ? OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE : 0) &&
        ace->Mask == FILE_ALL_ACCESS &&
        ::EqualSid(const_cast<DWORD*>(&ace->SidStart), const_cast<BYTE*>(sid.data()));
  }
  ::LocalFree(descriptor);
  return ok;
}

std::uint64_t Count(const Snapshot& snapshot, LexemeId id) {
  for (const auto& word : snapshot.words) if (word.id == id) return word.count;
  return 0;
}
std::uint64_t Pair(const Snapshot& snapshot, LexemeId previous, LexemeId id) {
  for (const auto& pair : snapshot.pairs)
    if (pair.previous_id == previous && pair.id == id) return pair.count;
  return 0;
}
void Cleanup(const fs::path& root) {
  std::error_code error;
  fs::remove_all(root, error);
  Check(!error, "fixture cleanup completed");
}

void Persistence(const fs::path& base) {
  const auto root = NewRoot(base, L"persist");
  const auto catalog = TestCatalog();
  WindowsLearningRepository repository(root);
  Check(!fs::exists(StoreDirectory(root)), "constructor has no filesystem side effects");
  Snapshot snapshot;
  Expect(repository.LoadSnapshot(catalog, allow, &snapshot), Status::kOk, "missing read succeeds empty");
  Check(snapshot.revision == 0 && snapshot.words.empty(), "missing store snapshot empty");
  Check(!fs::exists(StoreDirectory(root)), "missing read creates no store directory");
  for (int transaction = 0; transaction < 3; ++transaction)
    Expect(repository.Record(catalog, Confirmed(), allow), Status::kOk, "persist confirmed aggregate");
  WindowsLearningRepository reopened(root);
  Expect(reopened.LoadSnapshot(catalog, allow, &snapshot), Status::kOk, "fresh instance loads store");
  Check(snapshot.revision == 3 && Count(snapshot, 11) == 3 && Count(snapshot, 22) == 3 &&
        Pair(snapshot, 33, 11) == 3 && Pair(snapshot, 11, 22) == 3,
        "words pairs and revision survive reopen");
  Check(fs::file_size(Leaf(root, L".dat")) <= kMaxStoreBytes, "durable store bounded");
  Check(fs::file_size(Leaf(root, L".lock")) == 0, "stable lock has no personal data");
  const auto bytes = ReadFixture(Leaf(root, L".dat"));
  Check(bytes.find("epocanva-learning\t2\n") == 0 && bytes.find("context") == bytes.npos &&
        bytes.find("generation") == bytes.npos && bytes.find("host") == bytes.npos,
        "only versioned catalog ID/count representation persists");
  Check(!fs::exists(Leaf(root, L".tmp")), "temporary file consumed by atomic rename");
  // A real extra handle to the old file reproduces legacy FileRenameInfo's
  // replacement restriction. POSIX replacement must preserve that reader's old
  // bytes while new opens observe exactly one new revision.
  Handle old_reader(::CreateFileW(Leaf(root, L".dat").c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  Check(old_reader.valid(), "old-target reader held across replacement");
  Expect(repository.Record(catalog, Confirmed(), allow), Status::kOk,
         "atomic replacement succeeds while old target has reader");
  std::string previous_bytes(bytes.size(), '\0');
  DWORD read_utf8_bytes = 0;
  Check(old_reader.valid() && ::ReadFile(old_reader.get(), previous_bytes.data(),
      static_cast<DWORD>(previous_bytes.size()), &read_utf8_bytes, nullptr) &&
      read_utf8_bytes == bytes.size() && previous_bytes == bytes,
      "old reader continues to observe complete previous snapshot");
  Expect(repository.LoadSnapshot(catalog, allow, &snapshot), Status::kOk,
         "new open observes replacement despite old reader");
  Check(snapshot.revision == 4 && Count(snapshot, 11) == 4,
        "held-reader publication increments exactly once");
  old_reader.Reset();
  auto legacy = std::string("epocanva-learning\t1\nnamespace\t") + catalog.name_space +
      "\ncatalog-sha256\t" + catalog.sha256 + "\nrevision\t9\nw\t11\t4\n";
  WriteFixture(Leaf(root, L".dat"), legacy);
  Expect(repository.Record(catalog, Confirmed(), allow), Status::kOk, "real persisted v1 migration");
  Expect(repository.LoadSnapshot(catalog, allow, &snapshot), Status::kOk, "migrated snapshot");
  Check(snapshot.revision == 10 && Count(snapshot, 11) == 5, "migration retains counts and revision");
  Check(ReadFixture(Leaf(root, L".dat")).find("epocanva-learning\t2\n") == 0,
        "migration writes version two only");
  Cleanup(root);
}

bool SameIo(const IO_COUNTERS& left, const IO_COUNTERS& right) {
  return left.ReadOperationCount == right.ReadOperationCount &&
      left.WriteOperationCount == right.WriteOperationCount &&
      left.OtherOperationCount == right.OtherOperationCount &&
      left.ReadTransferCount == right.ReadTransferCount &&
      left.WriteTransferCount == right.WriteTransferCount &&
      left.OtherTransferCount == right.OtherTransferCount;
}

void Privacy(const fs::path& base) {
  const auto root = NewRoot(base, L"privacy");
  const auto catalog = TestCatalog();
  WindowsLearningRepository repository(root);
  Snapshot snapshot;
  std::string exported;
  for (int state = 0; state < 4; ++state) {
    int queries = 0;
    PrivacyCallback callback = [&] {
      ++queries;
      if (state == 3) throw std::runtime_error("test callback failure");
      Access access = Normal();
      if (state == 0) access.privacy = PrivacyClass::kUnknown;
      if (state == 1) access.privacy = PrivacyClass::kSensitive;
      if (state == 2) access.learning_enabled = false;
      return access;
    };
    IO_COUNTERS before = {}, after = {};
    const bool before_ok = ::GetProcessIoCounters(::GetCurrentProcess(), &before) != FALSE;
    const auto record = repository.Record(catalog, Confirmed(), callback);
    const auto load = repository.LoadSnapshot(catalog, callback, &snapshot);
    const bool after_ok = ::GetProcessIoCounters(::GetCurrentProcess(), &after) != FALSE;
    Expect(record, Status::kDenied, "privacy state denies record");
    Expect(load, Status::kDenied, "privacy state denies load");
    Check(queries == 2, "fresh initial callback on each denied operation");
    Check(before_ok && after_ok && SameIo(before, after), "denied learning performs zero process file I/O");
    Check(!fs::exists(StoreDirectory(root)), "denied operations create no storage");
    if (state != 2) {
      exported = "clear this";
      Expect(repository.Export(catalog, true, callback, &exported), Status::kDenied,
             "unknown sensitive and failed callback deny export");
      Check(exported.empty(), "denied export clears caller output");
    }
  }
  Expect(repository.Record(catalog, Confirmed(), {}), Status::kDenied, "empty callback denied");
  auto stale = Confirmed(); ++stale.generation;
  Expect(repository.Record(catalog, stale, allow), Status::kStale, "stale generation before I/O");
  stale = Confirmed(); stale.host_write_confirmed = false;
  Expect(repository.Record(catalog, stale, allow), Status::kStale, "host write must succeed");
  stale = Confirmed(); stale.lexeme_ids = {999};
  Expect(repository.Record(catalog, stale, allow), Status::kInvalidLexeme, "unadmitted ID before I/O");
  Check(!fs::exists(StoreDirectory(root)), "invalid selections cause no filesystem creation");
  Expect(repository.Record(catalog, Confirmed(), allow), Status::kOk, "allowed negative control creates store");
  const auto bytes = ReadFixture(Leaf(root, L".dat"));
  auto denied = [] { return Access{}; };
  snapshot = {999, {{11, 999}}, {}};
  Expect(repository.LoadSnapshot(catalog, denied, &snapshot), Status::kDenied, "denied existing read");
  Check(snapshot.words.empty() && snapshot.revision == 0, "no stale snapshot on denied read");
  Expect(repository.Record(catalog, Confirmed(), denied), Status::kDenied, "no cached permission grant");
  Check(ReadFixture(Leaf(root, L".dat")) == bytes, "denied existing store remains byte-identical");
  Cleanup(root);
}

void Revocation(const fs::path& base) {
  const auto root = NewRoot(base, L"revoke");
  const auto catalog = TestCatalog();
  WindowsLearningRepository repository(root);
  Expect(repository.Record(catalog, Confirmed(), allow), Status::kOk, "revocation baseline store");
  const auto baseline = ReadFixture(Leaf(root, L".dat"));
  int complete_queries = 0;
  Expect(repository.Record(catalog, Confirmed(), [&] { ++complete_queries; return Normal(); }),
         Status::kOk, "measure every live privacy checkpoint");
  Check(complete_queries > 10, "privacy queried throughout actual filesystem transaction");
  // Repeat the same initial store with revocation at EVERY callback invocation,
  // including the final query immediately before the handle-based rename.
  for (int revoke_at = 1; revoke_at <= complete_queries; ++revoke_at) {
    WriteFixture(Leaf(root, L".dat"), baseline);
    int queries = 0;
    const auto status = repository.Record(catalog, Confirmed(), [&] {
      if (++queries >= revoke_at) return Access{};
      return Normal();
    });
    Expect(status, Status::kDenied, "revoked checkpoint denies transaction");
    Check(ReadFixture(Leaf(root, L".dat")) == baseline, "revocation leaves zero durable aggregate update");
    Check(!fs::exists(Leaf(root, L".tmp")), "revocation removes uncommitted temporary file");
  }
  WriteFixture(Leaf(root, L".dat"), baseline);
  int queries = 0;
  Expect(repository.Record(catalog, Confirmed(), [&] {
    if (++queries >= complete_queries) throw std::runtime_error("last callback fails");
    return Normal();
  }), Status::kDenied, "callback throw before publish is Unknown denied");
  Check(ReadFixture(Leaf(root, L".dat")) == baseline, "failed final callback preserves bytes");
  queries = 0;
  Expect(repository.Record(catalog, Confirmed(), [&] {
    auto current = Normal();
    if (++queries >= complete_queries) ++current.generation;
    return current;
  }), Status::kStale, "generation change immediately before publish denied");
  Check(ReadFixture(Leaf(root, L".dat")) == baseline, "stale final callback preserves bytes");
  Cleanup(root);
}

void Errors(const fs::path& base) {
  const auto root = NewRoot(base, L"errors");
  const auto catalog = TestCatalog();
  WindowsLearningRepository repository(root);
  Expect(repository.Record(catalog, Confirmed(), allow), Status::kOk, "error suite baseline");
  const auto path = Leaf(root, L".dat");
  const auto valid = ReadFixture(path);
  Snapshot snapshot;
  WriteFixture(path, std::string(kMaxStoreBytes + 1, 'x'));
  Expect(repository.LoadSnapshot(catalog, allow, &snapshot), Status::kLimitExceeded, "oversize rejected before allocation/read");
  Expect(repository.Record(catalog, Confirmed(), allow), Status::kLimitExceeded, "oversize not overwritten");
  Check(fs::file_size(path) == kMaxStoreBytes + 1, "oversize store unchanged");
  for (const auto& bytes : {std::string("broken\n"), std::string(), valid + "w\t11\t1\n"}) {
    WriteFixture(path, bytes);
    Expect(repository.LoadSnapshot(catalog, allow, &snapshot), Status::kInvalidFormat, "corrupt record rejected");
    Expect(repository.Record(catalog, Confirmed(), allow), Status::kInvalidFormat, "corrupt record not reset silently");
    Check(ReadFixture(path) == bytes, "corrupt bytes preserved for explicit deletion");
  }
  WriteFixture(path, valid);
  auto different = catalog; different.sha256 = std::string(64, 'b');
  Expect(repository.LoadSnapshot(different, allow, &snapshot), Status::kCatalogMismatch, "wrong digest fails closed");
  Expect(repository.Record(different, Confirmed(), allow), Status::kCatalogMismatch, "wrong digest never overwrites counts");
  Check(ReadFixture(path) == valid, "mismatch is nondestructive");
  auto exhausted = valid;
  const auto revision_utf8_byte = exhausted.find("revision\t1\n");
  Check(revision_utf8_byte != exhausted.npos, "revision fixture located");
  if (revision_utf8_byte != exhausted.npos)
    exhausted.replace(revision_utf8_byte, 11, "revision\t18446744073709551615\n");
  WriteFixture(path, exhausted);
  Expect(repository.Record(catalog, Confirmed(), allow), Status::kRevisionExhausted, "revision never wraps");
  WriteFixture(path, valid);
  const DWORD original_attributes = ::GetFileAttributesW(path.c_str());
  Check(original_attributes != INVALID_FILE_ATTRIBUTES &&
      ::SetFileAttributesW(path.c_str(), original_attributes | FILE_ATTRIBUTE_READONLY),
      "read-only fixture set");
  Expect(repository.Record(catalog, Confirmed(), allow), Status::kIoError, "read-only target returns I/O status");
  Check(ReadFixture(path) == valid, "read-only target unchanged");
  Check(::SetFileAttributesW(path.c_str(), original_attributes) != FALSE, "read-only fixture restored");
  const auto lock_path = Leaf(root, L".lock");
  Handle lock(::CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  OVERLAPPED byte_zero = {};
  Check(lock.valid() && ::LockFileEx(lock.get(), LOCKFILE_EXCLUSIVE_LOCK |
      LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &byte_zero), "contention lock acquired");
  const auto before = ::GetTickCount64();
  Expect(repository.Record(catalog, Confirmed(), allow), Status::kBusy, "stable file lock contention reports busy");
  Check(::GetTickCount64() - before < 2000, "busy transaction never waits indefinitely");
  Expect(repository.LoadSnapshot(catalog, allow, &snapshot), Status::kBusy, "load participates in same transaction lock");
  lock.Reset();
  Expect(repository.Record(catalog, Confirmed(), allow), Status::kOk, "retry after contention succeeds");
  Check(ApplyAcl(path, false, FILE_GENERIC_READ | WRITE_DAC | DELETE), "read-only DACL fixture applied");
  Expect(repository.Record(catalog, Confirmed(), allow), Status::kIoError, "weakened/read-only DACL not silently repaired");
  Check(ApplyAcl(path, false, FILE_ALL_ACCESS), "DACL fixture restored");
  Cleanup(root);
}

void Management(const fs::path& base) {
  const auto root = NewRoot(base, L"management");
  const auto catalog = TestCatalog();
  WindowsLearningRepository repository(root);
  std::string exported = "clear this";
  int queries = 0;
  PrivacyCallback count = [&] { ++queries; return Normal(); };
  Expect(repository.Delete(catalog, false, count), Status::kDenied, "delete requires explicit action");
  Expect(repository.Export(catalog, false, count, &exported), Status::kDenied, "export requires explicit action");
  Check(queries == 0 && exported.empty() && !fs::exists(StoreDirectory(root)),
        "nonexplicit management has no callback or file I/O");
  Expect(repository.Delete(catalog, true), Status::kOk, "delete absent store idempotent");
  Expect(repository.Record(catalog, Confirmed(), allow), Status::kOk, "management baseline");
  const auto original = ReadFixture(Leaf(root, L".dat"));
  const PrivacyCallback disabled = [] { return Access{PrivacyClass::kNormal, false, 0, 0}; };
  Expect(repository.Export(catalog, true, disabled, &exported), Status::kOk, "explicit export while learning disabled");
  Check(exported == original, "export is exact bounded versioned ID/count data");
  Check(ReadFixture(Leaf(root, L".dat")) == original, "export does not bump revision");
  auto imported = LearningStore::Create(catalog);
  Expect(imported->Load(exported, Normal()), Status::kOk, "export consumable by actual core");
  Handle lock_before(::CreateFileW(Leaf(root, L".lock").c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  BY_HANDLE_FILE_INFORMATION identity_before = {};
  Check(lock_before.valid() && ::GetFileInformationByHandle(lock_before.get(), &identity_before),
        "stable lock identity captured");
  lock_before.Reset();
  WriteFixture(Leaf(root, L".tmp"), "orphan synthetic aggregate staging bytes");
  Check(ApplyAcl(Leaf(root, L".tmp"), false, FILE_ALL_ACCESS), "orphan temporary ACL matches adapter");
  queries = 0;
  Expect(repository.Delete(catalog, true, [&]() -> Access {
    ++queries; throw std::runtime_error("must never be called");
  }), Status::kOk, "explicit deletion independent of learning and focused field");
  Check(queries == 0 && !fs::exists(Leaf(root, L".dat")) && !fs::exists(Leaf(root, L".tmp")),
        "delete removes counts and orphan stage without field callback");
  Check(fs::exists(Leaf(root, L".lock")) && fs::file_size(Leaf(root, L".lock")) == 0,
        "empty stable lock retained");
  Handle lock_after(::CreateFileW(Leaf(root, L".lock").c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  BY_HANDLE_FILE_INFORMATION identity_after = {};
  Check(lock_after.valid() && ::GetFileInformationByHandle(lock_after.get(), &identity_after) &&
        identity_before.nFileIndexHigh == identity_after.nFileIndexHigh &&
        identity_before.nFileIndexLow == identity_after.nFileIndexLow,
        "delete never replaces transaction lock file");
  lock_after.Reset();
  Snapshot snapshot;
  Expect(repository.LoadSnapshot(catalog, allow, &snapshot), Status::kOk, "deleted snapshot reads empty");
  Check(snapshot.revision == 0 && snapshot.words.empty(), "deletion resets stored revision epoch");
  Expect(repository.Record(catalog, Confirmed(), allow), Status::kOk, "new learning after deletion starts new epoch");
  WriteFixture(Leaf(root, L".dat"), std::string(kMaxStoreBytes + 1, 'x'));
  Expect(repository.Delete(catalog, true), Status::kOk, "explicit delete needs no corrupt/oversize content read");
  Expect(repository.Delete(catalog, true), Status::kOk, "repeated delete idempotent");
  Cleanup(root);
}

void Acl(const fs::path& base) {
  const auto root = NewRoot(base, L"acl");
  const auto catalog = TestCatalog();
  WindowsLearningRepository repository(root);
  Expect(repository.Record(catalog, Confirmed(), allow), Status::kOk, "ACL baseline");
  Check(RestrictedAcl(StoreDirectory(root), true), "store directory protected current-user-only ACL");
  Check(RestrictedAcl(Leaf(root, L".lock"), false), "stable lock protected current-user-only ACL");
  Check(RestrictedAcl(Leaf(root, L".dat"), false), "atomic output protected current-user-only ACL");
  bool saw_temporary = false;
  bool temporary_acl = false;
  Expect(repository.Record(catalog, Confirmed(), [&] {
    if (fs::exists(Leaf(root, L".tmp"))) {
      saw_temporary = true;
      // The adapter owns this handle without sharing; an external open must fail.
      Handle probe(::CreateFileW(Leaf(root, L".tmp").c_str(), GENERIC_READ,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
      temporary_acl = !probe.valid() && ::GetLastError() == ERROR_SHARING_VIOLATION;
    }
    return Normal();
  }), Status::kOk, "atomic save retains exclusive staged handle");
  Check(saw_temporary && temporary_acl, "staged bytes cannot be read via another handle");
  Check(RestrictedAcl(Leaf(root, L".dat"), false), "renamed stage retains explicit private ACL");
  for (const auto& path : {Leaf(root, L".dat"), Leaf(root, L".lock"), StoreDirectory(root)}) {
    const bool directory = path == StoreDirectory(root);
    Check(ApplyAcl(path, directory, FILE_ALL_ACCESS, true), "world-readable negative fixture applied");
    Snapshot snapshot;
    Expect(repository.LoadSnapshot(catalog, allow, &snapshot), Status::kIoError,
           "pre-existing broad DACL rejected before reading IDs");
    Expect(repository.Record(catalog, Confirmed(), allow), Status::kIoError,
           "pre-existing broad DACL not silently used");
    Check(ApplyAcl(path, directory, FILE_ALL_ACCESS), "private DACL restored");
  }
  Cleanup(root);
}

// Test-only mount-point layout, matching the existing lab fixture and SDK tag /
// FSCTL definitions. No system configuration, privilege change, or elevation.
struct JunctionBuffer {
  ULONG tag;
  USHORT data_bytes;
  USHORT reserved;
  USHORT substitute_offset_utf16_bytes;
  USHORT substitute_utf16_bytes;
  USHORT print_offset_utf16_bytes;
  USHORT print_utf16_bytes;
  WCHAR path[1];
};
bool Junction(const fs::path& link, const fs::path& target) {
  if (!fs::create_directory(link)) return false;
  const std::wstring substitute = L"\\??\\" + target.native();
  const std::size_t substitute_utf16_bytes = substitute.size() * sizeof(wchar_t);
  const std::size_t buffer_bytes = offsetof(JunctionBuffer, path) +
      substitute_utf16_bytes + 2 * sizeof(wchar_t);
  std::vector<BYTE> bytes(buffer_bytes, 0);
  auto* reparse = reinterpret_cast<JunctionBuffer*>(bytes.data());
  reparse->tag = IO_REPARSE_TAG_MOUNT_POINT;
  reparse->data_bytes = static_cast<USHORT>(buffer_bytes - 8);
  reparse->substitute_utf16_bytes = static_cast<USHORT>(substitute_utf16_bytes);
  reparse->print_offset_utf16_bytes = static_cast<USHORT>(substitute_utf16_bytes + sizeof(wchar_t));
  std::memcpy(reparse->path, substitute.data(), substitute_utf16_bytes);
  Handle directory(::CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  DWORD returned_bytes = 0;
  return directory.valid() && ::DeviceIoControl(directory.get(), FSCTL_SET_REPARSE_POINT,
      bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, 0, &returned_bytes, nullptr);
}

void Paths(const fs::path& base) {
  const auto root = NewRoot(base, L"paths");
  const auto catalog = TestCatalog();
  const std::vector<fs::path> unsafe = {
      L"\\\\not-a-server.invalid\\share\\learning", L"\\\\?\\C:\\learning", L"\\\\.\\pipe\\learning",
      L"C:relative", L"relative", L"C:\\..\\learning", L"C:\\CON\\learning",
      L"C:\\safe\\trailing.\\learning", L"C:\\safe\\name:stream", root / L".." / L"other"};
  for (const auto& path : unsafe) {
    WindowsLearningRepository repository(path);
    Expect(repository.Record(catalog, Confirmed(), allow), Status::kIoError, "unsafe root syntax refused");
  }
  WindowsLearningRepository repository(root);
  for (const auto& name : {"../escape", "en/escape", "en\\escape", "en:stream", "en\nnext"}) {
    auto bad = catalog; bad.name_space = name;
    Expect(repository.Record(bad, Confirmed(), allow), Status::kInvalidCatalog,
           "schema/path escaping namespace invalid before I/O");
  }
  Check(!fs::exists(StoreDirectory(root)), "unsafe catalog creates no directory");
  auto dots = catalog; dots.name_space = "..";
  Expect(repository.Record(dots, Confirmed(), allow), Status::kOk,
         "valid dot namespace encoded as a harmless hex leaf");
  Check(fs::exists(Leaf(root, L".dat", dots)), "namespace never participates in path traversal");
  Expect(repository.Record(catalog, Confirmed(), allow), Status::kOk, "safe path control succeeds");
  const auto target = NewRoot(base, L"path-target");
  const auto junction = root / L"redirect";
  Check(Junction(junction, target), "junction fixture created without elevation");
  WindowsLearningRepository through_junction(junction);
  Expect(through_junction.Record(catalog, Confirmed(), allow), Status::kIoError,
         "caller root junction rejected");
  Check(!fs::exists(StoreDirectory(target)), "junction target untouched");
  Check(::RemoveDirectoryW(junction.c_str()) != FALSE, "junction fixture removed itself only");
  const auto data = Leaf(root, L".dat");
  const auto alias = target / L"hardlink.dat";
  Check(::CreateHardLinkW(alias.c_str(), data.c_str(), nullptr) != FALSE, "hardlink fixture created");
  Snapshot snapshot;
  Expect(repository.LoadSnapshot(catalog, allow, &snapshot), Status::kIoError,
         "multiply linked data file rejected");
  Check(::DeleteFileW(alias.c_str()) != FALSE, "hardlink fixture removed");
  Cleanup(root);
  Cleanup(target);
}

int Symlinks(const fs::path& base) {
  const auto root = NewRoot(base, L"symlinks");
  const auto target = NewRoot(base, L"symlink-target");
  const auto catalog = TestCatalog();
  const auto symbolic = root / L"symbolic";
  if (!::CreateSymbolicLinkW(symbolic.c_str(), target.c_str(),
      SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
    const DWORD error = ::GetLastError();
    Cleanup(root);
    Cleanup(target);
    if (error == ERROR_PRIVILEGE_NOT_HELD) {
      std::fprintf(stderr, "SKIP real symlink fixtures: Win32 1314; no elevation/configuration attempted\n");
      return failures == 0 ? 77 : 1;
    }
    Check(false, "directory symbolic-link fixture creation");
    return 1;
  }
  WindowsLearningRepository through_symbolic(symbolic);
  Expect(through_symbolic.Record(catalog, Confirmed(), allow), Status::kIoError,
         "symbolic caller root rejected");
  Check(!fs::exists(StoreDirectory(target)), "symbolic target untouched");
  Check(::RemoveDirectoryW(symbolic.c_str()) != FALSE, "symbolic fixture removed");
  WindowsLearningRepository repository(root);
  Expect(repository.Record(catalog, Confirmed(), allow), Status::kOk, "symlink control baseline");
  const auto data = Leaf(root, L".dat");
  Check(::DeleteFileW(data.c_str()) != FALSE, "prepare data leaf redirection fixture");
  const auto external = target / L"external.dat";
  WriteFixture(external, "external fixture must remain unchanged");
  if (::CreateSymbolicLinkW(data.c_str(), external.c_str(), SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
    Expect(repository.Record(catalog, Confirmed(), allow), Status::kIoError, "data leaf symbolic link rejected");
    Expect(repository.Delete(catalog, true), Status::kIoError, "explicit deletion never follows data symlink");
    Check(ReadFixture(external) == "external fixture must remain unchanged", "external data not altered");
    Check(::DeleteFileW(data.c_str()) != FALSE, "data symlink fixture removed");
  } else {
    Check(false, "file symbolic-link fixture creation");
  }
  Cleanup(root);
  Cleanup(target);
  if (failures != 0) return 1;
  std::puts("RESULT PASS");
  return 0;
}

std::wstring Quote(const std::wstring& argument) {
  // Test paths and event names contain no quotation marks or trailing backslash.
  return L"\"" + argument + L"\"";
}
std::wstring Executable() {
  std::vector<wchar_t> bytes(32768);
  const DWORD utf16_units = ::GetModuleFileNameW(nullptr, bytes.data(), static_cast<DWORD>(bytes.size()));
  return utf16_units == 0 || utf16_units >= bytes.size() ? L"" : std::wstring(bytes.data(), utf16_units);
}
Handle Spawn(const std::vector<std::wstring>& arguments) {
  const auto executable = Executable();
  std::wstring command = Quote(executable);
  for (const auto& argument : arguments) command += L" " + Quote(argument);
  STARTUPINFOW startup = {}; startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (!::CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
      0, nullptr, nullptr, &startup, &process)) return {};
  ::CloseHandle(process.hThread);
  return Handle(process.hProcess);
}
int Writer(const fs::path& root, const std::wstring& start_name, int records) {
  Handle start(::OpenEventW(SYNCHRONIZE, FALSE, start_name.c_str()));
  if (!start.valid() || ::WaitForSingleObject(start.get(), 15000) != WAIT_OBJECT_0) return 3;
  WindowsLearningRepository repository(root);
  for (int transaction = 0; transaction < records; ++transaction) {
    const auto deadline_ms = ::GetTickCount64() + 30000;
    for (;;) {
      int queries = 0;
      const Status status = repository.Record(TestCatalog(), Confirmed(), [&] {
        ++queries;
        return Normal();
      });
      if (status == Status::kOk) break;
      if (status != Status::kBusy || ::GetTickCount64() >= deadline_ms) {
        std::fprintf(stderr, "writer failed transaction %d status %s privacy-checks %d win32 %lu\n",
                     transaction, StatusName(status), queries, static_cast<unsigned long>(::GetLastError()));
        return 4;
      }
      ::Sleep(1);
    }
  }
  return 0;
}
int LockChild(const fs::path& root, const std::wstring& ready_name,
              const std::wstring& release_name) {
  Handle ready(::OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name.c_str()));
  Handle release(::OpenEventW(SYNCHRONIZE, FALSE, release_name.c_str()));
  Handle lock(::CreateFileW(Leaf(root, L".lock").c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  OVERLAPPED byte_zero = {};
  if (!ready.valid() || !release.valid() || !lock.valid() ||
      !::LockFileEx(lock.get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, 1, 0, &byte_zero)) return 5;
  if (!::SetEvent(ready.get()) || ::WaitForSingleObject(release.get(), 15000) != WAIT_OBJECT_0) return 6;
  return 0;
}
void WaitChild(Handle& child, const char* label) {
  if (!child.valid()) { Check(false, label); return; }
  const DWORD waited = ::WaitForSingleObject(child.get(), 60000);
  DWORD exit = 99;
  Check(waited == WAIT_OBJECT_0 && ::GetExitCodeProcess(child.get(), &exit) && exit == 0, label);
  if (waited != WAIT_OBJECT_0) {
    (void)::TerminateProcess(child.get(), 90);
    (void)::WaitForSingleObject(child.get(), 5000);
  }
}

void Concurrency(const fs::path& base) {
  const auto root = NewRoot(base, L"concurrency");
  WindowsLearningRepository repository(root);
  Expect(repository.Record(TestCatalog(), Confirmed(), allow), Status::kOk, "concurrency baseline");
  const auto unique = std::to_wstring(::GetCurrentProcessId()) + L"-" +
      std::to_wstring(::GetTickCount64());
  const auto ready_name = L"Local\\EpoCanvaLearningTest-ready-" + unique;
  const auto release_name = L"Local\\EpoCanvaLearningTest-release-" + unique;
  Handle ready(::CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str()));
  Handle release(::CreateEventW(nullptr, TRUE, FALSE, release_name.c_str()));
  Check(ready.valid() && release.valid(), "cross-process lock events created");
  auto holder = Spawn({L"--lock-child", root.native(), ready_name, release_name});
  Check(holder.valid() && ::WaitForSingleObject(ready.get(), 15000) == WAIT_OBJECT_0,
        "independent process owns stable lock");
  const auto before_ms = ::GetTickCount64();
  Expect(repository.Record(TestCatalog(), Confirmed(), allow), Status::kBusy,
         "independent process lock prevents concurrent write");
  Check(::GetTickCount64() - before_ms < 2000, "cross-process contention bounded/nonblocking");
  Expect(repository.Delete(TestCatalog(), true), Status::kBusy, "delete serializes with cross-process record");
  Check(::SetEvent(release.get()) != FALSE, "release independent process lock");
  WaitChild(holder, "cross-process lock child exit zero");
  const auto start_name = L"Local\\EpoCanvaLearningTest-start-" + unique;
  Handle start(::CreateEventW(nullptr, TRUE, FALSE, start_name.c_str()));
  Check(start.valid(), "concurrent writer barrier created");
  constexpr int processes = 4;
  constexpr int per_process_records = 40;
  std::vector<Handle> writers;
  for (int process = 0; process < processes; ++process)
    writers.push_back(Spawn({L"--writer", root.native(), start_name,
                             std::to_wstring(per_process_records)}));
  Check(::SetEvent(start.get()) != FALSE, "concurrent writers released together");
  for (auto& writer : writers) WaitChild(writer, "independent writer exit zero");
  Snapshot snapshot;
  Expect(repository.LoadSnapshot(TestCatalog(), allow, &snapshot), Status::kOk, "load concurrent transaction result");
  constexpr std::uint64_t expected = 1 + processes * per_process_records;
  Check(snapshot.revision == expected && Count(snapshot, 11) == expected &&
        Count(snapshot, 22) == expected && Pair(snapshot, 33, 11) == expected &&
        Pair(snapshot, 11, 22) == expected, "no cross-process lost word/pair updates or revision increments");
  Check(!fs::exists(Leaf(root, L".tmp")), "concurrent writers leave no orphan stage");
  Check(fs::file_size(Leaf(root, L".dat")) <= kMaxStoreBytes, "concurrent store remains bounded");
  Cleanup(root);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 2 && std::wstring(argv[1]) == L"--force-failure") {
    Check(false, "intentional assertion");
    return failures == 1 ? 1 : 9;
  }
  if (argc == 5 && std::wstring(argv[1]) == L"--writer")
    return Writer(fs::path(argv[2]), argv[3], std::stoi(argv[4]));
  if (argc == 5 && std::wstring(argv[1]) == L"--lock-child")
    return LockChild(fs::path(argv[2]), argv[3], argv[4]);
  if (argc != 3) return 2;
  const std::wstring mode = argv[1];
  try {
    const fs::path base = argv[2];
    if (!base.is_absolute() || !fs::is_directory(base)) return 2;
    if (mode == L"persistence") Persistence(base);
    else if (mode == L"privacy") Privacy(base);
    else if (mode == L"revocation") Revocation(base);
    else if (mode == L"errors") Errors(base);
    else if (mode == L"management") Management(base);
    else if (mode == L"acl") Acl(base);
    else if (mode == L"paths") Paths(base);
    else if (mode == L"symlinks") return Symlinks(base);
    else if (mode == L"concurrency") Concurrency(base);
    else return 2;
  } catch (...) {
    Check(false, "test fixture exception");
  }
  if (failures != 0) return 1;
  std::puts("RESULT PASS");
  return 0;
}
