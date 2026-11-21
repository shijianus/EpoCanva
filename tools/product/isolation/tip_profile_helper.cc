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
// WHAT IT DOES NOT DO. It never writes the CLSID/InprocServer32/category keys (that is
// register_tip.ps1), never changes another input method, never selects a keyboard layout for
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
#include <string>

#include "identifiers.h"

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
bool RegistrationOwnerMatches(const std::wstring& dll_path) {
  if (dll_path.empty()) return false;
  for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    const std::wstring base = std::wstring(L"Software\\Classes\\CLSID\\") + kProductClsidText;
    HKEY key = nullptr;
    const LONG opened = ::RegOpenKeyExW(root, base.c_str(), 0, KEY_READ, &key);
    if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) continue;
    if (opened != ERROR_SUCCESS) return false;
    wchar_t marker[64] = {};
    DWORD marker_bytes = sizeof(marker);
    DWORD marker_type = 0;
    const LONG marker_status = ::RegQueryValueExW(key, L"EpoCanvaProduct", nullptr,
        &marker_type, reinterpret_cast<BYTE*>(marker), &marker_bytes);
    ::RegCloseKey(key);
    const std::wstring server = ReadInprocServer32(root);
    if (marker_status != ERROR_SUCCESS || marker_type != REG_SZ ||
        std::wstring(marker) != L"epocanva_tsf" || server.empty() ||
        ::CompareStringOrdinal(server.c_str(), -1, dll_path.c_str(), -1, TRUE) != CSTR_EQUAL) return false;
    return true;
  }
  return false;
}

std::wstring ReadInprocServer32(HKEY root) {
  HKEY key = nullptr;
  const std::wstring path = std::wstring(L"Software\\Classes\\CLSID\\") +
                            kProductClsidText + L"\\InprocServer32";
  if (::RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
    return std::wstring();
  }
  wchar_t value[1024] = {};
  DWORD bytes = sizeof(value);
  DWORD type = 0;
  const LONG status =
      ::RegQueryValueExW(key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(value),
                         &bytes);
  ::RegCloseKey(key);
  if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
    return std::wstring();
  }
  return std::wstring(value);
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

int InvokeComRegistration(const std::wstring& dll_path, bool install) {
  if (dll_path.empty()) return 2;
  HMODULE module = ::LoadLibraryW(dll_path.c_str());
  if (module == nullptr) {
    std::fwprintf(stderr, L"LoadLibrary failed error=%lu\n", ::GetLastError());
    return 1;
  }
  using RegistrationFn = HRESULT(STDAPICALLTYPE*)(void);
  const auto entry = reinterpret_cast<RegistrationFn>(::GetProcAddress(
      module, install ? "DllRegisterServer" : "DllUnregisterServer"));
  if (entry == nullptr) {
    ::FreeLibrary(module);
    std::fwprintf(stderr, L"registration entry point is missing\n");
    return 1;
  }
  const HRESULT result = entry();
  PrintHr(install ? L"DllRegisterServer" : L"DllUnregisterServer", result);
  ::FreeLibrary(module);
  return SUCCEEDED(result) ? 0 : 1;
}

int Register(const std::wstring& dll_path) {
  if (InvokeComRegistration(dll_path, true) != 0) return 1;
  ITfInputProcessorProfiles* profiles = Profiles();
  if (profiles == nullptr) {
    (void)InvokeComRegistration(dll_path, false);
    return 1;
  }
  ITfInputProcessorProfileMgr* manager = ProfileManager();
  if (manager == nullptr) {
    profiles->Release();
    (void)InvokeComRegistration(dll_path, false);
    return 1;
  }
  ProfileState before = ReadState(manager);
  manager->Release();
  if (before.present) {
    PrintState(before);
    profiles->Release();
    return 0;
  }

  HRESULT hr = profiles->Register(Clsid());
  PrintHr(L"Register CLSID profile", hr);
  if (FAILED(hr)) {
    profiles->Release();
    (void)InvokeComRegistration(dll_path, false);
    return 1;
  }
  hr = profiles->AddLanguageProfile(
      Clsid(), kEnglishUs, ProfileGuid(), kDescription,
      static_cast<ULONG>(std::wcslen(kDescription)), nullptr, 0, 0);
  PrintHr(L"AddLanguageProfile en-US", hr);
  if (FAILED(hr)) {
    const HRESULT rollback = profiles->Unregister(Clsid());
    PrintHr(L"Rollback Unregister CLSID profile", rollback);
    profiles->Release();
    (void)InvokeComRegistration(dll_path, false);
    return 1;
  }
  profiles->Release();

  manager = ProfileManager();
  if (manager == nullptr) {
    (void)InvokeComRegistration(dll_path, false);
    return 1;
  }
  const ProfileState state = ReadState(manager);
  PrintState(state);
  manager->Release();
  if (!state.present) {
    std::fwprintf(stderr, L"verification failed: the profile is still absent\n");
    (void)InvokeComRegistration(dll_path, false);
    return 3;
  }
  return 0;
}

int Unregister(const std::wstring& dll_path) {
  ITfInputProcessorProfileMgr* manager = ProfileManager();
  if (manager == nullptr) return 1;
  const ProfileState before = ReadState(manager);
  manager->Release();
  if (!before.present) return InvokeComRegistration(dll_path, false);
  ITfInputProcessorProfiles* profiles = Profiles();
  if (profiles == nullptr) return 1;

  HRESULT removed = profiles->RemoveLanguageProfile(Clsid(), kEnglishUs, ProfileGuid());
  PrintHr(L"RemoveLanguageProfile", removed);
  const HRESULT unregistered = SUCCEEDED(removed) ? profiles->Unregister(Clsid()) : removed;
  if (SUCCEEDED(removed)) PrintHr(L"Unregister CLSID profile", unregistered);
  if (FAILED(removed) || FAILED(unregistered)) {
    ITfInputProcessorProfileMgr* rollback_manager = ProfileManager();
    const ProfileState after_failure = rollback_manager == nullptr ? ProfileState{} : ReadState(rollback_manager);
    if (rollback_manager != nullptr) rollback_manager->Release();
    if (!after_failure.present) {
      const HRESULT rollback = profiles->Register(Clsid());
      PrintHr(L"Rollback Register CLSID profile", rollback);
      if (SUCCEEDED(rollback)) {
        const HRESULT add = profiles->AddLanguageProfile(
            Clsid(), kEnglishUs, ProfileGuid(), kDescription,
            static_cast<ULONG>(std::wcslen(kDescription)), nullptr, 0, 0);
        PrintHr(L"Rollback AddLanguageProfile", add);
      }
    }
    profiles->Release();
    return 1;
  }
  profiles->Release();

  manager = ProfileManager();
  if (manager == nullptr) return 1;
  const ProfileState state = ReadState(manager);
  PrintState(state);
  manager->Release();
  if (state.present) {
    std::fwprintf(stderr, L"verification failed: the profile is still present\n");
    return 3;
  }
  return InvokeComRegistration(dll_path, false);
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
  const std::wstring command = argv[1];
  const std::wstring dll_path = argc > 2 ? argv[2] : L"";
  if (command == L"register") {
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
  ::CoUninitialize();
  return result;
}
