// EpoCanva W3-B -- COM registration. See registration.h for the contract.
#include "registration.h"

#include <cstdio>
#include <iterator>
#include <new>
#include <sstream>
#include <utility>

#include "identifiers.h"
#include "module_state.h"

namespace epocanva {
namespace tsf {
namespace {

constexpr wchar_t kMarkerName[] = L"EpoCanvaProduct";
constexpr wchar_t kMarkerValue[] = L"epocanva_tsf";
constexpr wchar_t kDllFileName[] = L"epocanva_tsf.dll";
constexpr wchar_t kThreadingModel[] = L"Apartment";
constexpr wchar_t kKeyboardCategory[] = L"{34745C63-B2F0-4784-8B67-5E12C8701A31}";
constexpr wchar_t kUiElementCategory[] = L"{49D2F9CF-1F5E-11D7-A6D3-00065B84435C}";

std::wstring ClsidText() {
  wchar_t buffer[64] = {};
  const GUID& guid = kTextServiceClsid;
  const int written = _snwprintf_s(
      buffer, _TRUNCATE, L"{%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}",
      static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0],
      guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
      guid.Data4[6], guid.Data4[7]);
  return written > 0 ? std::wstring(buffer) : std::wstring();
}

std::wstring BaseKey() {
  return L"Software\\Classes\\CLSID\\" + ClsidText();
}
std::wstring InprocKey() { return BaseKey() + L"\\InprocServer32"; }
std::wstring CategoryRootKey() { return BaseKey() + L"\\Implemented Categories"; }
std::wstring KeyboardCategoryKey() {
  return CategoryRootKey() + L"\\" + kKeyboardCategory;
}
std::wstring UiElementCategoryKey() {
  return CategoryRootKey() + L"\\" + kUiElementCategory;
}

std::wstring ToLower(std::wstring text) {
  for (wchar_t& ch : text) {
    if (ch >= L'A' && ch <= L'Z') {
      ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
  }
  return text;
}

bool EndsWithIgnoreCase(const std::wstring& text, const std::wstring& suffix) {
  if (text.size() < suffix.size()) {
    return false;
  }
  return ToLower(text.substr(text.size() - suffix.size())) == ToLower(suffix);
}

std::string Narrow(const std::wstring& text) {
  if (text.empty()) {
    return std::string();
  }
  const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
  if (bytes <= 0) {
    return std::string();
  }
  std::string out(static_cast<std::size_t>(bytes), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), bytes, nullptr, nullptr);
  return out;
}

std::string JsonEscape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char ch : text) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += ch;
        break;
    }
  }
  return out;
}

const char* ScopeName(RegistrationScope scope) {
  return scope == RegistrationScope::kMachine ? "machine" : "user";
}

// DllCanUnloadNow must answer S_FALSE for the whole registry sequence, including a call
// made directly through the API (the install helper does not go through regsvr32).
class RegistrationGuard {
 public:
  RegistrationGuard() { ModuleBeginRegistration(); }
  ~RegistrationGuard() { ModuleEndRegistration(); }
  RegistrationGuard(const RegistrationGuard&) = delete;
  RegistrationGuard& operator=(const RegistrationGuard&) = delete;
};

void Record(RegistrationResult* result, const char* action, const std::wstring& path,
            const std::wstring& value, const char* outcome) {
  RegistrationOperation operation;
  operation.action = action;
  operation.path = Narrow(path);
  operation.value = Narrow(value);
  operation.result = outcome;
  result->operations.push_back(std::move(operation));
}

struct ValueSnapshot {
  bool existed = false;
  std::wstring value;
};

struct ScopeSnapshot {
  bool base_existed = false;
  bool inproc_existed = false;
  bool category_root_existed = false;
  bool keyboard_existed = false;
  bool ui_element_existed = false;
  ValueSnapshot marker;
  ValueSnapshot inproc_default;
  ValueSnapshot threading;
};

enum class Owner {
  kNone,
  kEpoCanva,
  kForeign,
  kError,
};

Owner InspectOwner(RegistrationScope scope, RegistryOps* ops, ScopeSnapshot* snapshot,
                   std::wstring* why) {
  bool base_exists = false;
  if (!ops->KeyExists(scope, BaseKey(), &base_exists, why)) {
    return Owner::kError;
  }
  snapshot->base_existed = base_exists;
  if (!base_exists) {
    return Owner::kNone;
  }
  if (!ops->ReadString(scope, BaseKey(), kMarkerName, &snapshot->marker.value,
                       &snapshot->marker.existed, why)) {
    return Owner::kError;
  }
  if (!ops->KeyExists(scope, InprocKey(), &snapshot->inproc_existed, why)) {
    return Owner::kError;
  }
  if (snapshot->inproc_existed) {
    if (!ops->ReadString(scope, InprocKey(), L"", &snapshot->inproc_default.value,
                         &snapshot->inproc_default.existed, why) ||
        !ops->ReadString(scope, InprocKey(), L"ThreadingModel",
                         &snapshot->threading.value, &snapshot->threading.existed,
                         why)) {
      return Owner::kError;
    }
  }
  if (!ops->KeyExists(scope, CategoryRootKey(), &snapshot->category_root_existed, why) ||
      !ops->KeyExists(scope, KeyboardCategoryKey(), &snapshot->keyboard_existed, why) ||
      !ops->KeyExists(scope, UiElementCategoryKey(), &snapshot->ui_element_existed, why)) {
    return Owner::kError;
  }
  const bool owned =
      (snapshot->marker.existed && snapshot->marker.value == kMarkerValue) ||
      (snapshot->inproc_default.existed &&
       EndsWithIgnoreCase(snapshot->inproc_default.value, kDllFileName));
  return owned ? Owner::kEpoCanva : Owner::kForeign;
}

bool EnsureKey(RegistrationScope scope, RegistryOps* ops, const std::wstring& path,
               RegistrationResult* result, std::wstring* why) {
  bool created = false;
  if (!ops->CreateKey(scope, path, &created, why)) {
    result->error = *why;
    return false;
  }
  if (created) {
    result->changed = true;
    Record(result, "create-key", path, L"", "ok");
  }
  return true;
}

bool SetValueIfDifferent(RegistrationScope scope, RegistryOps* ops,
                         const std::wstring& path, const std::wstring& name,
                         const std::wstring& wanted, const ValueSnapshot& previous,
                         const char* action, RegistrationResult* result,
                         std::wstring* why) {
  if (previous.existed && previous.value == wanted) {
    return true;
  }
  if (!ops->WriteString(scope, path, name, wanted, why)) {
    result->error = *why;
    return false;
  }
  result->changed = true;
  Record(result, action, path, wanted, "ok");
  return true;
}

bool VerifyValue(RegistrationScope scope, RegistryOps* ops, const std::wstring& path,
                 const std::wstring& name, const std::wstring& wanted,
                 RegistrationResult* result, std::wstring* why) {
  std::wstring value;
  bool exists = false;
  if (!ops->ReadString(scope, path, name, &value, &exists, why)) {
    result->error = *why;
    return false;
  }
  if (!exists || value != wanted) {
    result->error = L"readback mismatch at " + path + L" (" + name + L")";
    return false;
  }
  return true;
}

bool VerifyKey(RegistrationScope scope, RegistryOps* ops, const std::wstring& path,
               RegistrationResult* result, std::wstring* why) {
  bool exists = false;
  if (!ops->KeyExists(scope, path, &exists, why)) {
    result->error = *why;
    return false;
  }
  if (!exists) {
    result->error = L"readback mismatch: key is absent: " + path;
    return false;
  }
  return true;
}

void Rollback(RegistrationScope scope, RegistryOps* ops, const ScopeSnapshot& snapshot,
              RegistrationResult* result) {
  bool complete = true;
  auto record_outcome = [&](const wchar_t* action, const std::wstring& path,
                            const std::wstring& value, bool ok) {
    Record(result, Narrow(action).c_str(), path, value, ok ? "ok" : "failed");
    complete = complete && ok;
  };
  std::wstring ignored;
  if (snapshot.ui_element_existed == false) {
    bool removed = false;
    if (ops->DeleteKey(scope, UiElementCategoryKey(), &removed, &ignored)) {
      record_outcome(L"rollback", UiElementCategoryKey(), L"", true);
    } else {
      record_outcome(L"rollback", UiElementCategoryKey(), L"", false);
    }
  }
  if (snapshot.keyboard_existed == false) {
    bool removed = false;
    if (ops->DeleteKey(scope, KeyboardCategoryKey(), &removed, &ignored)) {
      record_outcome(L"rollback", KeyboardCategoryKey(), L"", true);
    } else {
      record_outcome(L"rollback", KeyboardCategoryKey(), L"", false);
    }
  }
  auto restore = [&](const std::wstring& path, const std::wstring& name,
                     const ValueSnapshot& previous) {
    bool ok = false;
    if (previous.existed) {
      ok = ops->WriteString(scope, path, name, previous.value, &ignored);
    } else {
      bool removed = false;
      ok = ops->DeleteValue(scope, path, name, &removed, &ignored);
    }
    record_outcome(L"rollback", path, previous.value, ok);
  };
  restore(InprocKey(), L"ThreadingModel", snapshot.threading);
  restore(InprocKey(), L"", snapshot.inproc_default);
  restore(BaseKey(), kMarkerName, snapshot.marker);
  if (!snapshot.inproc_existed) {
    bool removed = false;
    if (ops->DeleteKey(scope, InprocKey(), &removed, &ignored)) {
      record_outcome(L"rollback", InprocKey(), L"", true);
    } else {
      record_outcome(L"rollback", InprocKey(), L"", false);
    }
  }
  if (!snapshot.base_existed) {
    bool removed = false;
    if (ops->DeleteKey(scope, BaseKey(), &removed, &ignored)) {
      record_outcome(L"rollback", BaseKey(), L"", true);
    } else {
      record_outcome(L"rollback", BaseKey(), L"", false);
    }
  }
  result->rolled_back = complete;
  if (!complete) {
    result->error += L" (rollback incomplete)";
  }
}

}  // namespace

const wchar_t* RegistrationClsidText() {
  static const std::wstring text = ClsidText();
  return text.c_str();
}
const wchar_t* RegistrationKeyboardCategoryText() { return kKeyboardCategory; }
const wchar_t* RegistrationUiElementCategoryText() { return kUiElementCategory; }
const wchar_t* RegistrationMarkerName() { return kMarkerName; }
const wchar_t* RegistrationMarkerValue() { return kMarkerValue; }

std::string RegistrationResult::ToJson() const {
  std::ostringstream json;
  json << "{";
  json << "\"ok\":" << (ok ? "true" : "false");
  json << ",\"changed\":" << (changed ? "true" : "false");
  json << ",\"rolled_back\":" << (rolled_back ? "true" : "false");
  json << ",\"foreign_registration\":"
       << (foreign_registration ? "true" : "false");
  json << ",\"machine_present_before\":"
       << (machine_present_before ? "true" : "false");
  json << ",\"user_present_before\":"
       << (user_present_before ? "true" : "false");
  json << ",\"scope\":\"" << ScopeName(scope) << "\"";
  json << ",\"error\":\"" << JsonEscape(Narrow(error)) << "\"";
  json << ",\"operations\":[";
  for (std::size_t index = 0; index < operations.size(); ++index) {
    if (index != 0) {
      json << ",";
    }
    const RegistrationOperation& operation = operations[index];
    json << "{\"action\":\"" << JsonEscape(operation.action) << "\""
         << ",\"path\":\"" << JsonEscape(operation.path) << "\""
         << ",\"value\":\"" << JsonEscape(operation.value) << "\""
         << ",\"result\":\"" << JsonEscape(operation.result) << "\"}";
  }
  json << "]}";
  return json.str();
}

bool Win32RegistryOps::KeyExists(RegistrationScope scope, const std::wstring& subkey,
                                 bool* exists, std::wstring* why) {
  if (exists == nullptr) {
    if (why != nullptr) *why = L"null exists pointer";
    return false;
  }
  *exists = false;
  const HKEY root = scope == RegistrationScope::kMachine ? HKEY_LOCAL_MACHINE
                                                         : HKEY_CURRENT_USER;
  const REGSAM view =
      scope == RegistrationScope::kMachine ? KEY_WOW64_64KEY : 0;
  HKEY key = nullptr;
  const LONG status =
      ::RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | view, &key);
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
    return true;
  }
  if (status != ERROR_SUCCESS) {
    if (why != nullptr) *why = L"RegOpenKeyExW failed with error " + std::to_wstring(status);
    return false;
  }
  ::RegCloseKey(key);
  *exists = true;
  return true;
}

bool Win32RegistryOps::CreateKey(RegistrationScope scope, const std::wstring& subkey,
                                 bool* created, std::wstring* why) {
  if (created == nullptr) {
    if (why != nullptr) *why = L"null created pointer";
    return false;
  }
  const HKEY root = scope == RegistrationScope::kMachine ? HKEY_LOCAL_MACHINE
                                                         : HKEY_CURRENT_USER;
  const REGSAM view =
      scope == RegistrationScope::kMachine ? KEY_WOW64_64KEY : 0;
  HKEY key = nullptr;
  DWORD disposition = 0;
  const LONG status = ::RegCreateKeyExW(
      root, subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
      KEY_READ | KEY_WRITE | view, nullptr, &key, &disposition);
  if (status != ERROR_SUCCESS) {
    if (why != nullptr) *why = L"RegCreateKeyExW failed with error " + std::to_wstring(status);
    return false;
  }
  ::RegCloseKey(key);
  *created = disposition == REG_CREATED_NEW_KEY;
  return true;
}

bool Win32RegistryOps::DeleteKey(RegistrationScope scope, const std::wstring& subkey,
                                 bool* removed, std::wstring* why) {
  if (removed == nullptr) {
    if (why != nullptr) *why = L"null removed pointer";
    return false;
  }
  bool exists = false;
  if (!KeyExists(scope, subkey, &exists, why)) {
    return false;
  }
  *removed = false;
  if (!exists) {
    return true;
  }
  const HKEY root = scope == RegistrationScope::kMachine ? HKEY_LOCAL_MACHINE
                                                         : HKEY_CURRENT_USER;
  const REGSAM view =
      scope == RegistrationScope::kMachine ? KEY_WOW64_64KEY : 0;
  const LONG status = ::RegDeleteKeyExW(root, subkey.c_str(), view, 0);
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) {
    return true;
  }
  if (status != ERROR_SUCCESS) {
    if (why != nullptr) *why = L"RegDeleteKeyExW failed with error " + std::to_wstring(status);
    return false;
  }
  *removed = true;
  return true;
}

bool Win32RegistryOps::WriteString(RegistrationScope scope, const std::wstring& subkey,
                                   const std::wstring& value_name,
                                   const std::wstring& value, std::wstring* why) {
  const HKEY root = scope == RegistrationScope::kMachine ? HKEY_LOCAL_MACHINE
                                                         : HKEY_CURRENT_USER;
  const REGSAM view =
      scope == RegistrationScope::kMachine ? KEY_WOW64_64KEY : 0;
  HKEY key = nullptr;
  DWORD disposition = 0;
  const LONG opened = ::RegCreateKeyExW(
      root, subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
      KEY_READ | KEY_WRITE | view, nullptr, &key, &disposition);
  if (opened != ERROR_SUCCESS) {
    if (why != nullptr) *why = L"RegCreateKeyExW failed with error " + std::to_wstring(opened);
    return false;
  }
  const wchar_t* name = value_name.empty() ? nullptr : value_name.c_str();
  const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  const LONG status = ::RegSetValueExW(key, name, 0, REG_SZ,
                                       reinterpret_cast<const BYTE*>(value.c_str()),
                                       bytes);
  ::RegCloseKey(key);
  if (status != ERROR_SUCCESS) {
    if (why != nullptr) *why = L"RegSetValueExW failed with error " + std::to_wstring(status);
    return false;
  }
  return true;
}

bool Win32RegistryOps::ReadString(RegistrationScope scope, const std::wstring& subkey,
                                  const std::wstring& value_name, std::wstring* value,
                                  bool* exists, std::wstring* why) {
  if (value == nullptr || exists == nullptr) {
    if (why != nullptr) *why = L"null readback pointer";
    return false;
  }
  *exists = false;
  value->clear();
  const HKEY root = scope == RegistrationScope::kMachine ? HKEY_LOCAL_MACHINE
                                                         : HKEY_CURRENT_USER;
  const REGSAM view =
      scope == RegistrationScope::kMachine ? KEY_WOW64_64KEY : 0;
  HKEY key = nullptr;
  const LONG opened =
      ::RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | view, &key);
  if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) {
    return true;
  }
  if (opened != ERROR_SUCCESS) {
    if (why != nullptr) *why = L"RegOpenKeyExW failed with error " + std::to_wstring(opened);
    return false;
  }
  const wchar_t* name = value_name.empty() ? nullptr : value_name.c_str();
  DWORD type = 0;
  DWORD bytes = 0;
  LONG status = ::RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
  if (status == ERROR_FILE_NOT_FOUND) {
    ::RegCloseKey(key);
    return true;
  }
  if (status != ERROR_SUCCESS) {
    ::RegCloseKey(key);
    if (why != nullptr) *why = L"RegQueryValueExW(size) failed with error " + std::to_wstring(status);
    return false;
  }
  if (type != REG_SZ && type != REG_EXPAND_SZ) {
    ::RegCloseKey(key);
    if (why != nullptr) *why = L"the registry value is not a string";
    return false;
  }
  std::wstring buffer(bytes / sizeof(wchar_t) + 1, L'\0');
  status = ::RegQueryValueExW(key, name, nullptr, &type,
                              reinterpret_cast<BYTE*>(buffer.data()), &bytes);
  ::RegCloseKey(key);
  if (status != ERROR_SUCCESS) {
    if (why != nullptr) *why = L"RegQueryValueExW failed with error " + std::to_wstring(status);
    return false;
  }
  buffer.resize(bytes / sizeof(wchar_t));
  while (!buffer.empty() && buffer.back() == L'\0') {
    buffer.pop_back();
  }
  *value = std::move(buffer);
  *exists = true;
  return true;
}

bool Win32RegistryOps::DeleteValue(RegistrationScope scope, const std::wstring& subkey,
                                   const std::wstring& value_name, bool* removed,
                                   std::wstring* why) {
  if (removed == nullptr) {
    if (why != nullptr) *why = L"null removed pointer";
    return false;
  }
  *removed = false;
  const HKEY root = scope == RegistrationScope::kMachine ? HKEY_LOCAL_MACHINE
                                                         : HKEY_CURRENT_USER;
  const REGSAM view =
      scope == RegistrationScope::kMachine ? KEY_WOW64_64KEY : 0;
  HKEY key = nullptr;
  const LONG opened =
      ::RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | KEY_WRITE | view, &key);
  if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) {
    return true;
  }
  if (opened != ERROR_SUCCESS) {
    if (why != nullptr) *why = L"RegOpenKeyExW failed with error " + std::to_wstring(opened);
    return false;
  }
  const wchar_t* name = value_name.empty() ? nullptr : value_name.c_str();
  const LONG status = ::RegDeleteValueW(key, name);
  ::RegCloseKey(key);
  if (status == ERROR_FILE_NOT_FOUND) {
    return true;
  }
  if (status != ERROR_SUCCESS) {
    if (why != nullptr) *why = L"RegDeleteValueW failed with error " + std::to_wstring(status);
    return false;
  }
  *removed = true;
  return true;
}

bool Win32RegistryOps::ListSubkeys(RegistrationScope scope, const std::wstring& subkey,
                                   std::vector<std::wstring>* names,
                                   std::wstring* why) {
  if (names == nullptr) {
    if (why != nullptr) *why = L"null names pointer";
    return false;
  }
  names->clear();
  const HKEY root = scope == RegistrationScope::kMachine ? HKEY_LOCAL_MACHINE
                                                         : HKEY_CURRENT_USER;
  const REGSAM view =
      scope == RegistrationScope::kMachine ? KEY_WOW64_64KEY : 0;
  HKEY key = nullptr;
  const LONG opened =
      ::RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | view, &key);
  if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) {
    return true;
  }
  if (opened != ERROR_SUCCESS) {
    if (why != nullptr) *why = L"RegOpenKeyExW failed with error " + std::to_wstring(opened);
    return false;
  }
  for (DWORD index = 0;; ++index) {
    wchar_t name[512] = {};
    DWORD length = static_cast<DWORD>(std::size(name));
    const LONG status = ::RegEnumKeyExW(key, index, name, &length, nullptr, nullptr,
                                        nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) {
      break;
    }
    if (status != ERROR_SUCCESS) {
      ::RegCloseKey(key);
      if (why != nullptr) *why = L"RegEnumKeyExW failed with error " + std::to_wstring(status);
      return false;
    }
    names->emplace_back(name, length);
  }
  ::RegCloseKey(key);
  return true;
}

bool Win32RegistryOps::ListValues(RegistrationScope scope, const std::wstring& subkey,
                                  std::vector<std::wstring>* names,
                                  std::wstring* why) {
  if (names == nullptr) {
    if (why != nullptr) *why = L"null names pointer";
    return false;
  }
  names->clear();
  const HKEY root = scope == RegistrationScope::kMachine ? HKEY_LOCAL_MACHINE
                                                         : HKEY_CURRENT_USER;
  const REGSAM view =
      scope == RegistrationScope::kMachine ? KEY_WOW64_64KEY : 0;
  HKEY key = nullptr;
  const LONG opened =
      ::RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ | view, &key);
  if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) {
    return true;
  }
  if (opened != ERROR_SUCCESS) {
    if (why != nullptr) *why = L"RegOpenKeyExW failed with error " + std::to_wstring(opened);
    return false;
  }
  for (DWORD index = 0;; ++index) {
    wchar_t name[512] = {};
    DWORD length = static_cast<DWORD>(std::size(name));
    LONG status = ::RegEnumValueW(key, index, name, &length, nullptr, nullptr, nullptr,
                                  nullptr);
    if (status == ERROR_NO_MORE_ITEMS) {
      break;
    }
    if (status != ERROR_SUCCESS) {
      ::RegCloseKey(key);
      if (why != nullptr) *why = L"RegEnumValueW failed with error " + std::to_wstring(status);
      return false;
    }
    names->emplace_back(name, length);
  }
  ::RegCloseKey(key);
  return true;
}

bool IsProcessElevated() {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return false;
  }
  TOKEN_ELEVATION elevation = {};
  DWORD size = sizeof(elevation);
  const BOOL read = ::GetTokenInformation(token, TokenElevation, &elevation,
                                          sizeof(elevation), &size);
  ::CloseHandle(token);
  return read != FALSE && elevation.TokenIsElevated != 0;
}

RegistrationResult RegisterTextServiceInScope(RegistrationScope scope,
                                              const std::wstring& module_path,
                                              RegistryOps* ops) {
  RegistrationGuard guard;
  RegistrationResult result;
  result.scope = scope;
  if (ops == nullptr) {
    result.error = L"no registry backend";
    return result;
  }
  if (module_path.empty() ||
      (module_path.size() > 1 && module_path[0] != L'\\' &&
       !(module_path.size() > 2 && module_path[1] == L':' &&
         (module_path[2] == L'\\' || module_path[2] == L'/')))) {
    result.error = L"InprocServer32 needs an absolute module path";
    return result;
  }
  RegistryOps* backend = ops;
  std::wstring why;
  ScopeSnapshot snapshot;
  const Owner owner = InspectOwner(scope, backend, &snapshot, &why);
  if (owner == Owner::kError) {
    result.error = why.empty() ? L"the existing registration could not be read"
                               : why;
    return result;
  }
  if (owner == Owner::kForeign) {
    result.foreign_registration = true;
    result.error = L"the CLSID is registered by another product; refusing to "
                   L"overwrite it";
    return result;
  }
  if (snapshot.threading.existed && snapshot.threading.value != kThreadingModel) {
    result.error = L"the CLSID already uses ThreadingModel=" +
                   snapshot.threading.value + L"; refusing to change an existing class";
    return result;
  }

  const bool ok =
      EnsureKey(scope, backend, BaseKey(), &result, &why) &&
      SetValueIfDifferent(scope, backend, BaseKey(), kMarkerName, kMarkerValue,
                          snapshot.marker, "set-value", &result, &why) &&
      EnsureKey(scope, backend, InprocKey(), &result, &why) &&
      SetValueIfDifferent(scope, backend, InprocKey(), L"", module_path,
                          snapshot.inproc_default, "set-default", &result, &why) &&
      SetValueIfDifferent(scope, backend, InprocKey(), L"ThreadingModel",
                          kThreadingModel, snapshot.threading, "set-value", &result,
                          &why) &&
      EnsureKey(scope, backend, CategoryRootKey(), &result, &why) &&
      EnsureKey(scope, backend, KeyboardCategoryKey(), &result, &why) &&
      EnsureKey(scope, backend, UiElementCategoryKey(), &result, &why);

  std::wstring verify_why;
  bool verified = ok;
  if (verified) {
    verified =
        VerifyValue(scope, backend, BaseKey(), kMarkerName, kMarkerValue, &result,
                    &verify_why) &&
        VerifyValue(scope, backend, InprocKey(), L"", module_path, &result,
                    &verify_why) &&
        VerifyValue(scope, backend, InprocKey(), L"ThreadingModel", kThreadingModel,
                    &result, &verify_why) &&
        VerifyKey(scope, backend, KeyboardCategoryKey(), &result, &verify_why) &&
        VerifyKey(scope, backend, UiElementCategoryKey(), &result, &verify_why);
    if (!verified && result.error.empty()) {
      result.error = verify_why;
    }
  }
  if (!verified) {
    if (result.error.empty()) {
      result.error = why;
    }
    Rollback(scope, backend, snapshot, &result);
    result.ok = false;
    return result;
  }
  Record(&result, "verify", BaseKey(), module_path, "ok");
  result.ok = true;
  return result;
}

RegistrationResult RegisterTextServiceAuto(const std::wstring& module_path,
                                           RegistryOps* ops) {
  RegistrationResult result;
  if (ops == nullptr) {
    result.error = L"no registry backend";
    return result;
  }
  std::wstring why;
  ScopeSnapshot machine;
  ScopeSnapshot user;
  const Owner machine_owner =
      InspectOwner(RegistrationScope::kMachine, ops, &machine, &why);
  const Owner user_owner = InspectOwner(RegistrationScope::kUser, ops, &user, &why);
  result.machine_present_before = machine_owner == Owner::kEpoCanva;
  result.user_present_before = user_owner == Owner::kEpoCanva;
  if (machine_owner == Owner::kError || user_owner == Owner::kError) {
    result.error = why.empty() ? L"the existing registration could not be read" : why;
    return result;
  }
  if (machine_owner == Owner::kForeign || user_owner == Owner::kForeign) {
    result.foreign_registration = true;
    result.error =
        L"the CLSID is registered by another product in one of the scopes; refusing "
        L"to overwrite it";
    return result;
  }
  RegistrationScope scope = RegistrationScope::kMachine;
  if (machine_owner == Owner::kEpoCanva) {
    scope = RegistrationScope::kMachine;
  } else if (user_owner == Owner::kEpoCanva) {
    scope = RegistrationScope::kUser;
  } else {
    scope = IsProcessElevated() ? RegistrationScope::kMachine
                                : RegistrationScope::kUser;
  }
  return RegisterTextServiceInScope(scope, module_path, ops);
}

RegistrationResult UnregisterTextService(RegistryOps* ops) {
  RegistrationGuard guard;
  RegistrationResult result;
  if (ops == nullptr) {
    result.error = L"no registry backend";
    return result;
  }
  result.ok = true;
  for (RegistrationScope scope :
       {RegistrationScope::kMachine, RegistrationScope::kUser}) {
    std::wstring why;
    ScopeSnapshot snapshot;
    const Owner owner = InspectOwner(scope, ops, &snapshot, &why);
    if (owner == Owner::kError) {
      result.ok = false;
      result.error = why.empty() ? L"the existing registration could not be read"
                                 : why;
      break;
    }
    if (owner == Owner::kNone) {
      Record(&result, "verify", BaseKey(), L"", "absent");
      continue;
    }
    if (owner == Owner::kForeign) {
      // Never delete another product's key. It is not this product's registration, so it
      // does not make the uninstall fail; the result records it for the operator.
      result.foreign_registration = true;
      Record(&result, "skip-foreign", BaseKey(), L"", "foreign");
      continue;
    }
    bool removed = false;
    if (!ops->DeleteKey(scope, UiElementCategoryKey(), &removed, &why)) {
      result.ok = false;
      result.error = why;
      break;
    }
    if (!ops->DeleteKey(scope, KeyboardCategoryKey(), &removed, &why)) {
      result.ok = false;
      result.error = why;
      break;
    }
    if (!ops->DeleteKey(scope, CategoryRootKey(), &removed, &why)) {
      result.ok = false;
      result.error = why;
      break;
    }
    if (!ops->DeleteValue(scope, BaseKey(), kMarkerName, &removed, &why)) {
      result.ok = false;
      result.error = why;
      break;
    }
    result.changed = true;
    Record(&result, "remove-key", BaseKey(), L"", "ok");
    if (!ops->DeleteKey(scope, InprocKey(), &removed, &why)) {
      result.ok = false;
      result.error = why;
      break;
    }
    std::vector<std::wstring> values;
    std::vector<std::wstring> subkeys;
    if (!ops->ListValues(scope, BaseKey(), &values, &why) ||
        !ops->ListSubkeys(scope, BaseKey(), &subkeys, &why)) {
      result.ok = false;
      result.error = why;
      break;
    }
    const bool base_empty = values.empty() && subkeys.empty();
    if (base_empty) {
      if (!ops->DeleteKey(scope, BaseKey(), &removed, &why)) {
        result.ok = false;
        result.error = why;
        break;
      }
      Record(&result, "remove-key", BaseKey(), L"", "ok");
    } else {
      Record(&result, "keep-key", BaseKey(), L"", "other values or subkeys remain");
    }
    // Readback is the claim: our marker, server path and categories must be gone.
    bool exists = false;
    if (!ops->KeyExists(scope, InprocKey(), &exists, &why) || exists) {
      result.ok = false;
      result.error = L"readback failed: InprocServer32 still exists";
      break;
    }
    if (!ops->KeyExists(scope, KeyboardCategoryKey(), &exists, &why) || exists) {
      result.ok = false;
      result.error = L"readback failed: the keyboard category still exists";
      break;
    }
    if (!ops->KeyExists(scope, UiElementCategoryKey(), &exists, &why) || exists) {
      result.ok = false;
      result.error = L"readback failed: the UIElement category still exists";
      break;
    }
    std::wstring marker;
    bool marker_exists = false;
    if (!ops->ReadString(scope, BaseKey(), kMarkerName, &marker, &marker_exists,
                         &why) ||
        marker_exists) {
      result.ok = false;
      result.error = L"readback failed: the product marker still exists";
      break;
    }
    Record(&result, "verify", BaseKey(), L"", "absent");
  }
  return result;
}

}  // namespace tsf
}  // namespace epocanva

// The two COM self-registration entry points. The .def file exports exactly these names
// (plus DllGetClassObject/DllCanUnloadNow); no __declspec(dllexport) exists in the sources.
//
// A structured, human-readable result is available from the registration API above and is
// written as JSON by the install/uninstall helper. These exports return the standard COM
// failure codes so regsvr32 and an installer can react: E_ACCESSDENIED means the CLSID is
// owned by another product or the registry denied the write, E_INVALIDARG means the module
// path could not be used. Callers that need the detailed reason use the API, not regsvr32.
extern "C" HRESULT STDAPICALLTYPE DllRegisterServer(void) {
  try {
    epocanva::tsf::RegistrationResult result;
    wchar_t module_path[32768] = {};
    const DWORD length = ::GetModuleFileNameW(
        epocanva::tsf::ModuleInstance(), module_path,
        static_cast<DWORD>(std::size(module_path)));
    if (length == 0 || length >= std::size(module_path)) {
      result.error = L"the text service module path is unavailable";
    } else {
      epocanva::tsf::Win32RegistryOps ops;
      result = epocanva::tsf::RegisterTextServiceAuto(module_path, &ops);
    }
    if (result.ok) {
      return S_OK;
    }
    return result.foreign_registration ? E_ACCESSDENIED : E_FAIL;
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}

extern "C" HRESULT STDAPICALLTYPE DllUnregisterServer(void) {
  try {
    epocanva::tsf::Win32RegistryOps ops;
    const epocanva::tsf::RegistrationResult result =
        epocanva::tsf::UnregisterTextService(&ops);
    return result.ok ? S_OK : E_FAIL;
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}
