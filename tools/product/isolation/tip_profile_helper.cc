// EpoCanva W2-B-R -- W3 isolation helper (NOT BUILT OR RUN IN THIS ROUND).
//
// Registers the EpoCanva CLSID as a TSF keyboard text service profile, and enables or
// disables that profile for the current session. The product DLL deliberately has no
// DllRegisterServer, so a separate, explicit helper is the only supported way to do this.
//
// BUILD (W3, on the authorized isolation machine only):
//   cl /EHsc /std:c++17 /DUNICODE /D_UNICODE tip_profile_helper.cc ole32.lib oleaut32.lib uuid.lib
// RUN (after the CLSID InprocServer32 key points at the staged product DLL):
//   tip_profile_helper.exe register
//   tip_profile_helper.exe activate
//   tip_profile_helper.exe deactivate
//   tip_profile_helper.exe unregister
//
// This source is not part of any product target, is not staged, and is never executed by
// the test suite.
#include <windows.h>
#include <msctf.h>

#include <cstdio>
#include <cwchar>
#include <string>

namespace {

// Must match platforms/windows/tsf/identifiers.h.
const CLSID kTextServiceClsid = {
    0xB440F702, 0x63E4, 0x4D47, {0xBC, 0xC4, 0x07, 0x48, 0x99, 0xAB, 0xA8, 0x36}};
// Test-only profile GUID; stable so uninstall can find it. Not a product identity.
const GUID kProfileGuid = {
    0x6C2C7B5E, 0x9E4A, 0x4E7E, {0x8F, 0x7A, 0x2B, 0x7B, 0x0C, 0x5F, 0x5A, 0x11}};
constexpr LANGID kEnglishUs = 0x0409;
constexpr wchar_t kDescription[] = L"EpoCanva English (isolation test)";

ITfInputProcessorProfiles* Profiles() {
  ITfInputProcessorProfiles* profiles = nullptr;
  const HRESULT hr = ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_ITfInputProcessorProfiles,
                                        reinterpret_cast<void**>(&profiles));
  if (FAILED(hr)) {
    std::fwprintf(stderr, L"CoCreateInstance(CLSID_TF_InputProcessorProfiles) failed 0x%08lX\n",
                  static_cast<unsigned long>(hr));
    return nullptr;
  }
  return profiles;
}

ITfInputProcessorProfileMgr* ProfileManager() {
  ITfInputProcessorProfileMgr* manager = nullptr;
  const HRESULT hr = ::CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_ITfInputProcessorProfileMgr,
                                        reinterpret_cast<void**>(&manager));
  if (FAILED(hr)) {
    std::fwprintf(stderr, L"CoCreateInstance(ITfInputProcessorProfileMgr) failed 0x%08lX\n",
                  static_cast<unsigned long>(hr));
    return nullptr;
  }
  return manager;
}

int Register() {
  ITfInputProcessorProfiles* profiles = Profiles();
  if (profiles == nullptr) {
    return 1;
  }
  HRESULT hr = profiles->Register(kTextServiceClsid);
  std::wprintf(L"Register CLSID: 0x%08lX\n", static_cast<unsigned long>(hr));
  if (SUCCEEDED(hr)) {
    hr = profiles->AddLanguageProfile(kTextServiceClsid, kEnglishUs, kProfileGuid,
                                      kDescription,
                                      static_cast<ULONG>(std::wcslen(kDescription)),
                                      nullptr, 0, 0);
    std::wprintf(L"AddLanguageProfile en-US: 0x%08lX\n",
                 static_cast<unsigned long>(hr));
  }
  profiles->Release();
  return SUCCEEDED(hr) ? 0 : 2;
}

int Unregister() {
  ITfInputProcessorProfiles* profiles = Profiles();
  if (profiles == nullptr) {
    return 1;
  }
  HRESULT hr = profiles->RemoveLanguageProfile(kTextServiceClsid, kEnglishUs,
                                               kProfileGuid);
  std::wprintf(L"RemoveLanguageProfile: 0x%08lX\n", static_cast<unsigned long>(hr));
  hr = profiles->Unregister(kTextServiceClsid);
  std::wprintf(L"Unregister CLSID: 0x%08lX\n", static_cast<unsigned long>(hr));
  profiles->Release();
  return SUCCEEDED(hr) ? 0 : 2;
}

int SetActive(bool active) {
  ITfInputProcessorProfileMgr* manager = ProfileManager();
  if (manager == nullptr) {
    return 1;
  }
  const DWORD flags =
      TF_IPPMF_FORSESSION | (active ? TF_IPPMF_ENABLEPROFILE : TF_IPPMF_DISABLEPROFILE);
  const HRESULT hr =
      active ? manager->ActivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, kEnglishUs,
                                        kTextServiceClsid, kProfileGuid, nullptr, flags)
             : manager->DeactivateProfile(TF_PROFILETYPE_INPUTPROCESSOR, kEnglishUs,
                                          kTextServiceClsid, kProfileGuid, nullptr,
                                          flags);
  std::wprintf(L"%s profile: 0x%08lX\n", active ? L"Activate" : L"Deactivate",
               static_cast<unsigned long>(hr));
  manager->Release();
  return SUCCEEDED(hr) ? 0 : 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::fwprintf(stderr,
                  L"usage: tip_profile_helper.exe register|unregister|activate|deactivate\n");
    return 2;
  }
  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com)) {
    std::fwprintf(stderr, L"CoInitializeEx failed 0x%08lX\n",
                  static_cast<unsigned long>(com));
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
  } else {
    std::fwprintf(stderr, L"unknown command: %ls\n", argv[1]);
  }
  ::CoUninitialize();
  return result;
}
