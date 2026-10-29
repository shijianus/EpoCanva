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

int Status() {
  PrintComRegistration();
  ITfInputProcessorProfileMgr* manager = ProfileManager();
  if (manager == nullptr) {
    return 1;
  }
  const ProfileState state = ReadState(manager);
  PrintState(state);
  manager->Release();
  return 0;
}

int Register() {
  ITfInputProcessorProfiles* profiles = Profiles();
  if (profiles == nullptr) {
    return 1;
  }
  HRESULT hr = profiles->Register(Clsid());
  PrintHr(L"Register CLSID", hr);
  if (SUCCEEDED(hr)) {
    hr = profiles->AddLanguageProfile(
        Clsid(), kEnglishUs, ProfileGuid(), kDescription,
        static_cast<ULONG>(std::wcslen(kDescription)), nullptr, 0, 0);
    PrintHr(L"AddLanguageProfile en-US", hr);
  }
  profiles->Release();

  ITfInputProcessorProfileMgr* manager = ProfileManager();
  if (manager == nullptr) {
    return 1;
  }
  const ProfileState state = ReadState(manager);
  PrintState(state);
  manager->Release();
  if (!state.present) {
    std::fwprintf(stderr, L"verification failed: the profile is still absent\n");
    return 3;
  }
  return 0;
}

int Unregister() {
  ITfInputProcessorProfiles* profiles = Profiles();
  if (profiles == nullptr) {
    return 1;
  }
  HRESULT removed =
      profiles->RemoveLanguageProfile(Clsid(), kEnglishUs, ProfileGuid());
  PrintHr(L"RemoveLanguageProfile", removed);
  const HRESULT unregistered = profiles->Unregister(Clsid());
  PrintHr(L"Unregister CLSID", unregistered);
  profiles->Release();

  ITfInputProcessorProfileMgr* manager = ProfileManager();
  if (manager == nullptr) {
    return 1;
  }
  const ProfileState state = ReadState(manager);
  PrintState(state);
  manager->Release();
  if (state.present) {
    std::fwprintf(stderr, L"verification failed: the profile is still present\n");
    return 3;
  }
  return 0;
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
                  L"register|unregister|activate|deactivate|status\n");
    return 2;
  }
  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) {
    PrintHr(L"CoInitializeEx", com);
    return 1;
  }
  int result = 2;
  const std::wstring command = argv[1];
  if (command == L"register") {
    result = Register();
  } else if (command == L"unregister") {
    result = Unregister();
  } else if (command == L"activate") {
    result = SetActive(true);
  } else if (command == L"deactivate") {
    result = SetActive(false);
  } else if (command == L"status") {
    result = Status();
  } else {
    std::fwprintf(stderr, L"unknown command: %ls\n", argv[1]);
  }
  ::CoUninitialize();
  return result;
}
