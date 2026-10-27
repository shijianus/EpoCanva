#include "product_paths.h"

#include <shlobj.h>

#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

#ifndef EPOCANVA_PRODUCT_VERSION
#define EPOCANVA_PRODUCT_VERSION "0.1.0-w1"
#endif
#ifndef EPOCANVA_PRODUCT_COMMIT
#define EPOCANVA_PRODUCT_COMMIT ""
#endif

namespace epocanva {
namespace product {
namespace {

std::mutex g_override_mutex;
std::filesystem::path g_override_root;

std::wstring AsciiToWide(const char* text) {
  std::wstring out;
  if (text == nullptr) {
    return out;
  }
  while (*text != '\0') {
    out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text)));
    ++text;
  }
  return out;
}

std::filesystem::path ModulePath(HMODULE module, std::wstring* why) {
  std::vector<wchar_t> buffer(512, L'\0');
  for (;;) {
    const DWORD written = ::GetModuleFileNameW(
        module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0) {
      *why = L"GetModuleFileNameW failed with error " + std::to_wstring(::GetLastError());
      return {};
    }
    if (written < buffer.size()) {
      return std::filesystem::path(std::wstring(buffer.data(), written));
    }
    buffer.assign(static_cast<std::size_t>(written) + 1, L'\0');
  }
}

bool ReadEnvironmentRoot(std::filesystem::path* root, std::wstring* why) {
  DWORD needed = ::GetEnvironmentVariableW(L"EPOCANVA_USER_DATA_ROOT", nullptr, 0);
  if (needed == 0) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_ENVVAR_NOT_FOUND) {
      return false;
    }
    *why = L"reading EPOCANVA_USER_DATA_ROOT failed with error " +
           std::to_wstring(error);
    return true;
  }
  std::vector<wchar_t> buffer(needed, L'\0');
  const DWORD written = ::GetEnvironmentVariableW(
      L"EPOCANVA_USER_DATA_ROOT", buffer.data(), static_cast<DWORD>(buffer.size()));
  if (written == 0 || written >= buffer.size()) {
    *why = L"EPOCANVA_USER_DATA_ROOT could not be read";
    return true;
  }
  std::filesystem::path value(std::wstring(buffer.data(), written));
  if (!value.is_absolute()) {
    *why = L"EPOCANVA_USER_DATA_ROOT must be an absolute path";
    return true;
  }
  *root = value;
  return true;
}

bool KnownFolderUserRoot(std::filesystem::path* root, std::wstring* why) {
  PWSTR raw = nullptr;
  const HRESULT hr = ::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT,
                                            nullptr, &raw);
  if (FAILED(hr) || raw == nullptr) {
    if (raw != nullptr) {
      ::CoTaskMemFree(raw);
    }
    *why = L"SHGetKnownFolderPath(LocalAppData) failed (HRESULT " +
           std::to_wstring(static_cast<unsigned long>(hr)) + L")";
    return false;
  }
  *root = std::filesystem::path(std::wstring(raw)) / L"EpoCanva";
  ::CoTaskMemFree(raw);
  return true;
}

}  // namespace

void SetUserRootOverrideForProcess(const std::filesystem::path& root) {
  std::lock_guard<std::mutex> guard(g_override_mutex);
  g_override_root = root;
}

std::filesystem::path UserRootOverrideForProcess() {
  std::lock_guard<std::mutex> guard(g_override_mutex);
  return g_override_root;
}

bool ResolveProductPaths(HMODULE module, ProductPaths* paths, std::wstring* why) {
  if (paths == nullptr) {
    if (why != nullptr) {
      *why = L"ResolveProductPaths received a null paths pointer";
    }
    return false;
  }
  ProductPaths resolved;
  resolved.module_path = ModulePath(module, why);
  if (resolved.module_path.empty()) {
    return false;
  }
  resolved.product_root = resolved.module_path.parent_path();
  resolved.data_dir = resolved.product_root / L"data";
  resolved.schema_file = resolved.data_dir / L"epocanva_english.schema.yaml";
  resolved.default_config = resolved.data_dir / L"default.yaml";
  resolved.lexicon_file =
      resolved.data_dir / L"lexicon" / L"epocanva_en_alpha.tsv";
  resolved.rime_dll = resolved.product_root / L"rime.dll";
  resolved.tsf_dll = resolved.product_root / L"epocanva_tsf.dll";
  resolved.licenses_dir = resolved.product_root / L"licenses";
  resolved.notice_file = resolved.product_root / L"NOTICE.txt";
  resolved.manifest_file = resolved.product_root / L"package.manifest";

  std::filesystem::path user_root = UserRootOverrideForProcess();
  if (user_root.empty()) {
    std::filesystem::path env_root;
    if (ReadEnvironmentRoot(&env_root, why)) {
      if (env_root.empty()) {
        return false;
      }
      user_root = env_root;
    }
  }
  if (user_root.empty() && !KnownFolderUserRoot(&user_root, why)) {
    return false;
  }
  if (!user_root.is_absolute()) {
    if (why != nullptr) {
      *why = L"the user data root must be absolute";
    }
    return false;
  }
  resolved.user_root = user_root;
  resolved.engine_user_dir = resolved.user_root / L"engine";
  resolved.log_dir = resolved.user_root / L"logs";
  resolved.settings_file = resolved.user_root / L"settings.ini";
  resolved.schema_patch_file =
      resolved.engine_user_dir / L"epocanva_english.custom.yaml";
  resolved.deploy_marker_file = resolved.engine_user_dir / L"deploy-required";

  *paths = std::move(resolved);
  return true;
}

bool EnsureUserDirectories(const ProductPaths& paths, std::wstring* why) {
  std::error_code ec;
  for (const std::filesystem::path& dir :
       {paths.user_root, paths.engine_user_dir, paths.log_dir}) {
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      if (why != nullptr) {
        *why = L"cannot create user directory " + dir.wstring() + L" (error " +
               std::to_wstring(ec.value()) + L")";
      }
      return false;
    }
  }
  return true;
}

bool ProbeUserDataWritable(const ProductPaths& paths, std::wstring* why) {
  if (!EnsureUserDirectories(paths, why)) {
    return false;
  }
  wchar_t temp[MAX_PATH] = {};
  if (::GetTempFileNameW(paths.user_root.c_str(), L"epc", 0, temp) == 0) {
    if (why != nullptr) {
      *why = L"GetTempFileNameW failed in the user data root with error " +
             std::to_wstring(::GetLastError());
    }
    return false;
  }
  const HANDLE file = ::CreateFileW(temp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (why != nullptr) {
      *why = L"cannot open a probe file in the user data root (error " +
             std::to_wstring(::GetLastError()) + L")";
    }
    ::DeleteFileW(temp);
    return false;
  }
  DWORD written = 0;
  const char byte = 'E';
  const BOOL ok = ::WriteFile(file, &byte, 1, &written, nullptr);
  ::CloseHandle(file);
  ::DeleteFileW(temp);
  if (!ok || written != 1) {
    if (why != nullptr) {
      *why = L"the user data root is not writable";
    }
    return false;
  }
  return true;
}

std::wstring ProductVersion() {
  return AsciiToWide(EPOCANVA_PRODUCT_VERSION);
}

std::wstring ProductCommit() {
  return AsciiToWide(EPOCANVA_PRODUCT_COMMIT);
}

std::wstring ProductVersionLine() {
  std::wstring line = ProductVersion();
  const std::wstring commit = ProductCommit();
  if (!commit.empty()) {
    line += L" (" + commit + L")";
  }
  return line;
}

}  // namespace product
}  // namespace epocanva
