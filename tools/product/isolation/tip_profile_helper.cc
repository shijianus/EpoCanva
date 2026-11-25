// EpoCanva W3-A -- isolation registration helper. NOT part of the product package.
//
// WHAT IT DOES. It performs only the TSF language-profile operations that the COM class
// registration cannot express:
//   register    ITfInputProcessorProfiles::Register + AddLanguageProfile (en-US)
//   unregister  RemoveLanguageProfile + Unregister
//   activate    ITfInputProcessorProfileMgr::ActivateProfile (session enable)
//   deactivate  DeactivateProfile (session disable)
//   status      read-only report: COM registration, profile presence, active profile
//
// It invokes the product DLL exports for CLSID/InprocServer32/category registration.
// It never changes another input method, never selects a keyboard layout for
// the user, and never claims that a successful profile registration means the TIP is active
// in an application. Every command prints the HRESULT it observed and then re-reads the
// state to verify the claim; a verification mismatch exits non-zero.
//
// BUILD (authorized isolation machine, from the product build tree):
//   cmake -S platforms/windows/engine_bridge -B build/isolation-tools -G "Visual Studio 17 2022" -A x64 \
//         -DEPOCANVA_BUILD_ISOLATION_TOOLS=ON
//   cmake --build build/isolation-tools --config Release --target epocanva_tip_profile_helper
//
// The CLSID and profile GUID come from platforms/windows/tsf/identifiers.h -- the same
// frozen product identity the DLL and the installer use. There is no test-only identity.
#include <windows.h>
#include <msctf.h>
#include <objbase.h>
#include <winreg.h>
#include <shellapi.h>

#include <cstdio>
#include <cwchar>
#include <exception>
#include <vector>
#include <string>

#include "identifiers.h"
#include "tip_profile_policy.h"

namespace {

constexpr LANGID kEnglishUs = 0x0409;
constexpr wchar_t kDescription[] = L"EpoCanva English";
constexpr wchar_t kProductClsidText[] = L"{B440F702-63E4-4D47-BCC4-074899ABA836}";
constexpr wchar_t kProductProfileText[] = L"{7D3E2A94-5C61-4F0B-9E3A-1B8D6C4F2A71}";

const GUID& Clsid() { return epocanva::tsf::kTextServiceClsid; }
const GUID& ProfileGuid() { return epocanva::tsf::kEnglishLanguageProfileGuid; }

void PrintHr(const wchar_t* what, HRESULT hr) {
  std::wprintf(L"%-32ls 0x%08lX\n", what, static_cast<unsigned long>(hr));
}

bool SameGuid(const GUID& left, const GUID& right) {
  return ::IsEqualGUID(left, right) != FALSE;
}
std::wstring ReadInprocServer32(HKEY root);
using epocanva::tip_profile::Owner;
using epocanva::tip_profile::Mutation;
using epocanva::tip_profile::State;

struct RegistryKey {
  HKEY value = nullptr;
  RegistryKey() = default;
  RegistryKey(const RegistryKey&) = delete;
  RegistryKey& operator=(const RegistryKey&) = delete;
  ~RegistryKey() { if (value) ::RegCloseKey(value); }
};

bool ReadRegistryString(HKEY key, const wchar_t* name, std::wstring* text) {
  // RegQueryValueExW does not promise a terminator for malformed REG_SZ data.
  // All capacities/counts here are UTF-16 code units or bytes as named.
  std::vector<wchar_t> value_utf16(32768);
  DWORD value_bytes = static_cast<DWORD>(value_utf16.size() * sizeof(wchar_t));
  DWORD type = 0;
  const LONG status = ::RegQueryValueExW(key, name, nullptr, &type,
      reinterpret_cast<BYTE*>(value_utf16.data()), &value_bytes);
  if (status != ERROR_SUCCESS || type != REG_SZ || value_bytes < sizeof(wchar_t) ||
      value_bytes % sizeof(wchar_t) != 0 ||
      value_bytes > value_utf16.size() * sizeof(wchar_t)) return false;
  const std::size_t code_units = value_bytes / sizeof(wchar_t);
  if (value_utf16[code_units - 1] != L'\0') return false;
  for (std::size_t utf16_index = 0; utf16_index + 1 < code_units; ++utf16_index) {
    if (value_utf16[utf16_index] == L'\0') return false;
  }
  text->assign(value_utf16.data(), code_units - 1);
  return true;
}

Owner ReadScopeOwner(HKEY root, const std::wstring& dll_path) {
  const std::wstring base = std::wstring(L"Software\\Classes\\CLSID\\") + kProductClsidText;
  RegistryKey key;
  const LONG opened = ::RegOpenKeyExW(root, base.c_str(), 0,
                                    KEY_READ | KEY_WOW64_64KEY, &key.value);
  if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return Owner::kAbsent;
  if (opened != ERROR_SUCCESS) return Owner::kBlocked;
  std::wstring marker;
  const bool marker_ok = ReadRegistryString(key.value, L"EpoCanvaProduct", &marker);
  RegistryKey server_key;
  const LONG server_opened = ::RegOpenKeyExW(key.value, L"InprocServer32", 0,
                                           KEY_READ | KEY_WOW64_64KEY, &server_key.value);
  if (server_opened != ERROR_SUCCESS) return Owner::kBlocked;
  std::wstring server, threading;
  const bool server_ok = ReadRegistryString(server_key.value, nullptr, &server);
  const bool threading_ok = ReadRegistryString(server_key.value, L"ThreadingModel", &threading);
  if (!marker_ok || marker != L"epocanva_tsf" || !server_ok || !threading_ok ||
      threading != L"Apartment" || dll_path.empty() ||
      ::CompareStringOrdinal(server.c_str(), -1, dll_path.c_str(), -1, TRUE) != CSTR_EQUAL) {
    return Owner::kBlocked;
  }
  return root == HKEY_CURRENT_USER ? Owner::kUser : Owner::kMachine;
}

Owner ReadRegistrationOwner(const std::wstring& dll_path) {
  if (dll_path.empty()) return Owner::kBlocked;
  // Always inspect both scopes: DllUnregisterServer visits both, not only HKCR's
  // effective registration. A matching HKCU must not hide a foreign HKLM.
  return epocanva::tip_profile::CombineOwners(
      ReadScopeOwner(HKEY_CURRENT_USER, dll_path),
      ReadScopeOwner(HKEY_LOCAL_MACHINE, dll_path));
}

bool RegistrationOwnerMatches(const std::wstring& dll_path) {
  return epocanva::tip_profile::Owned(ReadRegistrationOwner(dll_path));
}

std::wstring ReadInprocServer32(HKEY root) {
  RegistryKey key;
  const std::wstring path = std::wstring(L"Software\\Classes\\CLSID\\") +
                            kProductClsidText + L"\\InprocServer32";
  if (::RegOpenKeyExW(root, path.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &key.value) != ERROR_SUCCESS) {
    return std::wstring();
  }
  std::wstring value;
  const bool read = ReadRegistryString(key.value, nullptr, &value);
  return read ? value : std::wstring();
}

void PrintComRegistration() {
  const std::wstring machine = ReadInprocServer32(HKEY_LOCAL_MACHINE);
  const std::wstring user = ReadInprocServer32(HKEY_CURRENT_USER);
  std::wprintf(L"com_registration HKLM     %ls %ls\n", machine.empty() ? L"absent" : L"present",
               machine.c_str());
  std::wprintf(L"com_registration HKCU     %ls %ls\n", user.empty() ? L"absent" : L"present",
               user.c_str());
}

ITfInputProcessorProfiles* Profiles() {
  ITfInputProcessorProfiles* profiles = nullptr;
  const HRESULT hr = ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_ITfInputProcessorProfiles,
                                        reinterpret_cast<void**>(&profiles));
  PrintHr(L"CoCreateInstance(profiles)", hr);
  return SUCCEEDED(hr) ? profiles : nullptr;
}

ITfInputProcessorProfileMgr* ProfileManager() {
  ITfInputProcessorProfileMgr* manager = nullptr;
  const HRESULT hr = ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_ITfInputProcessorProfileMgr,
                                        reinterpret_cast<void**>(&manager));
  PrintHr(L"CoCreateInstance(profile mgr)", hr);
  return SUCCEEDED(hr) ? manager : nullptr;
}

struct ProfileState {
  bool present = false;
  bool active_keyboard = false;
  bool enabled = false;
  HRESULT get_profile_hr = E_FAIL;
  HRESULT get_active_hr = E_FAIL;
  TF_INPUTPROCESSORPROFILE profile{};
};

ProfileState ReadState(ITfInputProcessorProfileMgr* manager) {
  ProfileState state;
  state.get_profile_hr = manager->GetProfile(
      TF_PROFILETYPE_INPUTPROCESSOR, kEnglishUs, Clsid(), ProfileGuid(), nullptr,
      &state.profile);
  state.present = SUCCEEDED(state.get_profile_hr);
  if (state.present) {
    state.enabled = (state.profile.dwFlags & TF_IPP_FLAG_ENABLED) != 0;
  }
  TF_INPUTPROCESSORPROFILE active{};
  state.get_active_hr = manager->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active);
  if (SUCCEEDED(state.get_active_hr) &&
      (active.dwFlags & TF_IPP_FLAG_ACTIVE) != 0 && SameGuid(active.clsid, Clsid()) &&
      SameGuid(active.guidProfile, ProfileGuid())) {
    state.active_keyboard = true;
  }
  return state;
}

void PrintState(const ProfileState& state) {
  std::wprintf(L"profile_present          %ls\n", state.present ? L"yes" : L"no");
  std::wprintf(L"profile_enabled          %ls\n", state.enabled ? L"yes" : L"no");
  std::wprintf(L"profile_active_keyboard  %ls\n",
               state.active_keyboard ? L"yes" : L"no");
  PrintHr(L"GetProfile", state.get_profile_hr);
  PrintHr(L"GetActiveProfile", state.get_active_hr);
}

int InvokeComRegistration(const std::wstring& dll_path, bool install);

int Status(const std::wstring& dll_path) {
  PrintComRegistration();
  ITfInputProcessorProfileMgr* manager = ProfileManager();
  if (manager == nullptr) return 1;
  const ProfileState state = ReadState(manager);
  PrintState(state);
  manager->Release();
  if (state.present && !dll_path.empty()) {
    if (!RegistrationOwnerMatches(dll_path)) {
      std::fwprintf(stderr, L"refusing foreign or mismatched COM registration\n");
      return 4;
    }
    return 0;
  }
  return 0;
}

template <class T>
class ComPtr {
 public:
  ~ComPtr() { if (value) value->Release(); }
  ComPtr() = default;
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  T* value = nullptr;
};

class RegistrationModule {
 public:
  using Entry = HRESULT(STDAPICALLTYPE*)(void);
  ~RegistrationModule() { if (module_) ::FreeLibrary(module_); }
  RegistrationModule() = default;
  RegistrationModule(const RegistrationModule&) = delete;
  RegistrationModule& operator=(const RegistrationModule&) = delete;
  bool Open(const std::wstring& path) {
    module_ = ::LoadLibraryExW(path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module_) {
      std::fwprintf(stderr, L"LoadLibraryExW failed error=%lu\n", ::GetLastError());
      return false;
    }
    register_ = reinterpret_cast<Entry>(::GetProcAddress(module_, "DllRegisterServer"));
    unregister_ = reinterpret_cast<Entry>(::GetProcAddress(module_, "DllUnregisterServer"));
    wchar_t loaded_utf16[32768] = {};
    const DWORD code_units = ::GetModuleFileNameW(module_, loaded_utf16, 32768);
    if (!register_ || !unregister_ || code_units == 0 || code_units >= 32768 ||
        ::CompareStringOrdinal(path.c_str(), -1, loaded_utf16, -1, TRUE) != CSTR_EQUAL) {
      std::fwprintf(stderr, L"registration exports or loaded module identity invalid\n");
      return false;
    }
    return true;
  }
  bool Change(bool install) {
    const HRESULT hr = (install ? register_ : unregister_)();
    PrintHr(install ? L"DllRegisterServer" : L"DllUnregisterServer", hr);
    return hr == S_OK;
  }
 private:
  HMODULE module_ = nullptr;
  Entry register_ = nullptr;
  Entry unregister_ = nullptr;
};

// All objects are created, used and released synchronously on wmain's STA.
class ProfileOperations {
 public:
  ProfileOperations(const std::wstring& path, ITfInputProcessorProfiles* profiles,
                    RegistrationModule* module)
      : path_(path), profiles_(profiles), module_(module) {}
  Owner ReadOwner() { return ReadRegistrationOwner(path_); }
  bool Read(State* state) {
    *state = State{};
    state->owner = ReadOwner();
    if (state->owner == Owner::kBlocked) return false;
    ComPtr<IEnumGUID> processors;
    HRESULT hr = profiles_->EnumInputProcessorInfo(&processors.value);
    PrintHr(L"EnumInputProcessorInfo", hr);
    if (hr != S_OK || !processors.value) return false;
    for (;;) {
      GUID clsid{};
      ULONG fetched = 0;  // Number of GUID records, not a text offset.
      hr = processors.value->Next(1, &clsid, &fetched);
      if (hr == S_FALSE && fetched == 0) break;
      if (hr != S_OK || fetched != 1) {
        PrintHr(L"EnumInputProcessorInfo.Next", hr);
        return false;
      }
      if (SameGuid(clsid, Clsid())) state->processor = true;
    }
    LANGID* language_ids = nullptr;
    ULONG language_count = 0;  // Number of LANGID records.
    hr = profiles_->GetLanguageList(&language_ids, &language_count);
    PrintHr(L"GetLanguageList", hr);
    struct LanguageList {
      LANGID* value;
      ~LanguageList() { ::CoTaskMemFree(value); }
    } language_list{language_ids};
    if (hr != S_OK || (language_count != 0 && !language_ids) ||
        language_count > 65536) return false;
    // Always inspect our language, including when it is not the current language.
    std::vector<LANGID> languages{kEnglishUs};
    for (ULONG record = 0; record < language_count; ++record) {
      if (language_ids[record] != kEnglishUs) languages.push_back(language_ids[record]);
    }
    for (LANGID language : languages) {
      ComPtr<IEnumTfLanguageProfiles> profiles;
      hr = profiles_->EnumLanguageProfiles(language, &profiles.value);
      PrintHr(L"EnumLanguageProfiles", hr);
      if (hr != S_OK || !profiles.value) return false;
      for (;;) {
        TF_LANGUAGEPROFILE profile{};
        ULONG fetched = 0;  // Number of profile records.
        hr = profiles.value->Next(1, &profile, &fetched);
        if (hr == S_FALSE && fetched == 0) break;
        if (hr != S_OK || fetched != 1) {
          PrintHr(L"EnumLanguageProfiles.Next", hr);
          return false;
        }
        if (!SameGuid(profile.clsid, Clsid())) continue;
        if (profile.langid == kEnglishUs && SameGuid(profile.guidProfile, ProfileGuid())) {
          state->target = true;
        } else {
          state->other_profiles = true;
        }
      }
    }
    // Do not let a scope/owner change during enumeration bless the old snapshot.
    if (state->owner != ReadOwner()) return false;
    observed_owner_ = state->owner;
    std::wprintf(L"observed_registration    owner=%d processor=%d target=%d other=%d\n",
                 static_cast<int>(state->owner), state->processor ? 1 : 0,
                 state->target ? 1 : 0, state->other_profiles ? 1 : 0);
    return true;
  }
  bool Change(Mutation mutation) {
    const Owner owner = ReadOwner();
    if (owner != observed_owner_) return false;
    if (mutation == Mutation::kRegisterCom) {
      if (owner != Owner::kAbsent && !epocanva::tip_profile::Owned(owner)) return false;
      return module_->Change(true);
    }
    if (!epocanva::tip_profile::Owned(owner)) return false;
    HRESULT hr = E_FAIL;
    const wchar_t* operation = L"unknown mutation";
    switch (mutation) {
      case Mutation::kRegisterProcessor:
        hr = profiles_->Register(Clsid()); operation = L"Register CLSID profile"; break;
      case Mutation::kAddTarget:
        // AddLanguageProfile's description count is UTF-16 code units, excluding NUL.
        hr = profiles_->AddLanguageProfile(Clsid(), kEnglishUs, ProfileGuid(), kDescription,
            static_cast<ULONG>(std::wcslen(kDescription)), nullptr, 0, 0);
        operation = L"AddLanguageProfile en-US"; break;
      case Mutation::kRemoveTarget:
        hr = profiles_->RemoveLanguageProfile(Clsid(), kEnglishUs, ProfileGuid());
        operation = L"RemoveLanguageProfile"; break;
      case Mutation::kUnregisterProcessor:
        hr = profiles_->Unregister(Clsid()); operation = L"Unregister CLSID profile"; break;
      case Mutation::kUnregisterCom:
        return module_->Change(false);
      default: return false;
    }
    PrintHr(operation, hr);
    return hr == S_OK;
  }
 private:
  const std::wstring& path_;
  ITfInputProcessorProfiles* profiles_;  // Borrowed from the surrounding STA scope.
  RegistrationModule* module_;          // Outlives all mutations and compensation.
  Owner observed_owner_ = Owner::kBlocked;
};

int InvokeComRegistration(const std::wstring& dll_path, bool install) {
  const Owner before = ReadRegistrationOwner(dll_path);
  if (before == Owner::kBlocked) return 4;
  if (!install && before == Owner::kAbsent) return 0;
  RegistrationModule module;
  if (!module.Open(dll_path)) return 1;
  if (ReadRegistrationOwner(dll_path) != before) return 4;
  if (!module.Change(install)) return 1;
  const Owner after = ReadRegistrationOwner(dll_path);
  return (install ? epocanva::tip_profile::Owned(after) : after == Owner::kAbsent) ? 0 : 3;
}

int ChangeRegistration(const std::wstring& dll_path, bool install) {
  if (ReadRegistrationOwner(dll_path) == Owner::kBlocked) return 4;
  // Resolve both exports before touching TSF: missing DLL/export must not leave
  // a removed profile behind. Hold the module through the entire operation.
  RegistrationModule module;
  if (!module.Open(dll_path)) return 1;
  ComPtr<ITfInputProcessorProfiles> profiles;
  profiles.value = Profiles();
  if (!profiles.value) return 1;
  ProfileOperations ops(dll_path, profiles.value, &module);
  const int result = install ? epocanva::tip_profile::Register(ops)
                             : epocanva::tip_profile::Unregister(ops);
  std::wprintf(L"registration_result      %d\n", result);
  if (result == 5) {
    std::fwprintf(stderr, L"compensation incomplete; retained state requires inspection\n");
  }
  return result;
}

int Register(const std::wstring& dll_path) { return ChangeRegistration(dll_path, true); }
int Unregister(const std::wstring& dll_path) { return ChangeRegistration(dll_path, false); }

bool NormalizeDllPath(const wchar_t* input, std::wstring* normalized) {
  const std::wstring path(input);
  const bool drive_absolute = path.size() >= 3 && path[1] == L':' &&
                             (path[2] == L'\\' || path[2] == L'/');
  const bool unc_absolute = path.size() >= 3 && path[0] == L'\\' && path[1] == L'\\';
  if (!drive_absolute && !unc_absolute) return false;
  wchar_t path_utf16[32768] = {};
  const DWORD code_units = ::GetFullPathNameW(input, 32768, path_utf16, nullptr);
  if (code_units == 0 || code_units >= 32768) return false;
  normalized->assign(path_utf16, code_units);
  return true;
}


int SetActive(bool active) {
  ITfInputProcessorProfileMgr* manager = ProfileManager();
  if (manager == nullptr) {
    return 1;
  }
  const DWORD flags =
      TF_IPPMF_FORSESSION | (active ? TF_IPPMF_ENABLEPROFILE : TF_IPPMF_DISABLEPROFILE);
  const HRESULT changed =
      active ? manager->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, kEnglishUs,
                                        Clsid(), ProfileGuid(), nullptr, flags)
             : manager->DeactivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, kEnglishUs,
                                          Clsid(), ProfileGuid(), nullptr, flags);
  PrintHr(active ? L"ActivateProfile" : L"DeactivateProfile", changed);
  const ProfileState state = ReadState(manager);
  PrintState(state);
  manager->Release();
  if (active && !state.active_keyboard) {
    std::fwprintf(stderr,
                  L"verification failed: the profile is not the active keyboard TIP\n");
    return 3;
  }
  if (!active && state.active_keyboard) {
    std::fwprintf(stderr, L"verification failed: the profile is still active\n");
    return 3;
  }
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::wprintf(L"product_clsid   %ls\n", kProductClsidText);
  std::wprintf(L"product_profile %ls\n", kProductProfileText);
  if (argc < 2) {
    std::fwprintf(stderr,
                  L"usage: epocanva_tip_profile_helper "
                  L"register|unregister|status [installed-tsf-dll]\n");
    return 2;
  }
  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) {
    PrintHr(L"CoInitializeEx", com);
    return 1;
  }
  int result = 2;
  try {
    const std::wstring command = argv[1];
    std::wstring dll_path;
    if (argc > 3 || (argc == 3 && !NormalizeDllPath(argv[2], &dll_path)) ||
        (argc < 3 && command != L"status")) {
      std::fwprintf(stderr, L"registration requires one absolute DLL path\n");
    } else if (command == L"register") {
      result = Register(dll_path);
    } else if (command == L"unregister") {
      result = Unregister(dll_path);
    } else if (command == L"register-com") {
      result = InvokeComRegistration(dll_path, true);
    } else if (command == L"unregister-com") {
      result = InvokeComRegistration(dll_path, false);
    } else if (command == L"status") {
      result = Status(dll_path);
    } else {
      std::fwprintf(stderr, L"unknown command: %ls\n", argv[1]);
    }
  } catch (const std::exception&) {
    // Exceptions cannot turn incomplete registration/compensation into success.
    std::fwprintf(stderr, L"registration aborted; state requires inspection\n");
    result = 5;
  }
  ::CoUninitialize();
  return result;
}
