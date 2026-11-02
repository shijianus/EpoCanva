#include "product_paths.h"

#include <shlobj.h>

#include <cstring>
#include <mutex>
#include <utility>
#include <vector>
#include <atomic>

#include "../local_path/local_path.h"

#ifndef EPOCANVA_PRODUCT_VERSION
#define EPOCANVA_PRODUCT_VERSION "0.2.6-w3b"
#endif
#ifndef EPOCANVA_PRODUCT_COMMIT
#define EPOCANVA_PRODUCT_COMMIT ""
#endif

namespace epocanva {
namespace product {
namespace {

std::mutex g_override_mutex;
std::filesystem::path g_override_root;
namespace local = ::epocanva::windows::local_path;

bool PathFailure(std::wstring* why) {
  if (why != nullptr) *why = L"a product root is not a safe unaliased local disk path";
  return false;
}

bool PrepareUserPaths(const ProductPaths& paths, std::vector<local::PreparedPath>* prepared,
                      std::wstring* why) {
  // Validate every input before any create/open, including manually built ProductPaths.
  for (const auto& path : {paths.user_root, paths.engine_user_dir, paths.log_dir}) {
    local::PreparedPath item;
    if (!local::Prepare(path, &item)) return PathFailure(why);
    prepared->push_back(std::move(item));
  }
  const auto& root = (*prepared)[0].physical;
  if ((*prepared)[1].physical != root / L"engine" ||
      (*prepared)[2].physical != root / L"logs") return PathFailure(why);
  return true;
}

bool CreateUserPaths(const ProductPaths& paths, std::vector<local::PinnedPath>* pins,
                     std::wstring* why) {
  std::vector<local::PreparedPath> prepared;
  if (!PrepareUserPaths(paths, &prepared, why)) return false;
  // Pin all existing ancestors before creating anything. In particular a bad
  // logs/engine junction must not leave a newly created sibling behind.
  for (const auto& item : prepared) {
    local::PinnedPath pin;
    if (!pin.Open(item, true, true)) return PathFailure(why);
    pins->push_back(std::move(pin));
  }
  for (const auto& item : prepared) {
    local::PinnedPath pin;
    if (!pin.CreateDirectories(item)) return PathFailure(why);
    pins->push_back(std::move(pin));
  }
  return true;
}

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
  const HRESULT hr = ::SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY,
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
  std::wstring ignored;
  if (why == nullptr) why = &ignored;
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
  local::PreparedPath module_path;
  local::PreparedPath user_path;
  if (!local::Prepare(resolved.module_path, &module_path) ||
      !local::Prepare(user_root, &user_path)) return PathFailure(why);
  local::PinnedPath module_pin;
  local::PinnedPath user_pin;
  if (!module_pin.Open(module_path, false) || !user_pin.Open(user_path, true, true))
    return PathFailure(why);
  // This resolver creates nothing. Writable operations re-pin immediately before
  // use; a copied ProductPaths is a layout, not a permanent security capability.
  resolved.user_root = user_path.display;
  resolved.engine_user_dir = resolved.user_root / L"engine";
  resolved.log_dir = resolved.user_root / L"logs";
  resolved.settings_file = resolved.user_root / L"settings.ini";
  resolved.schema_patch_file =
      resolved.engine_user_dir / L"epocanva_english.custom.yaml";
  resolved.deploy_marker_file = resolved.engine_user_dir / L"deploy-required";
  resolved.deployed_state_file = resolved.engine_user_dir / L"deployed-state";
  resolved.engine_lock_file = resolved.engine_user_dir / L"engine.lock";
  resolved.compiled_schema_file =
      resolved.engine_user_dir / L"build" / L"epocanva_english.schema.yaml";
  resolved.compiled_schema_binary =
      resolved.engine_user_dir / L"build" / L"epocanva_english.schema.bin";

  *paths = std::move(resolved);
  return true;
}

bool EnsureUserDirectories(const ProductPaths& paths, std::wstring* why) {
  std::vector<local::PinnedPath> pins;
  return CreateUserPaths(paths, &pins, why);
}

bool ProbeUserDataWritable(const ProductPaths& paths, std::wstring* why) {
  std::vector<local::PinnedPath> pins;
  if (!CreateUserPaths(paths, &pins, why)) return false;
  // Keep all directory pins through the write. CREATE_NEW never opens a hostile
  // existing leaf; DELETE_ON_CLOSE removes our handle, not a re-resolved pathname.
  static std::atomic<unsigned long long> sequence{0};
  HANDLE file = INVALID_HANDLE_VALUE;
  for (int attempt = 0; attempt < 16; ++attempt) {
    const auto leaf = L"epocanva-probe-" + std::to_wstring(::GetCurrentProcessId()) +
        L"-" + std::to_wstring(::GetTickCount64()) + L"-" +
        std::to_wstring(sequence.fetch_add(1)) + L".tmp";
    const auto temp = pins[3].path() / leaf;  // the fully created user root
    file = ::CreateFileW(temp.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT |
        FILE_FLAG_OPEN_NO_RECALL, nullptr);
    if (file != INVALID_HANDLE_VALUE) break;
    if (::GetLastError() != ERROR_FILE_EXISTS && ::GetLastError() != ERROR_ALREADY_EXISTS) break;
  }
  if (file == INVALID_HANDLE_VALUE) return PathFailure(why);
  DWORD written_bytes = 0;
  const char byte = 'E';
  const BOOL ok = ::WriteFile(file, &byte, 1, &written_bytes, nullptr);
  ::CloseHandle(file);
  if (!ok || written_bytes != 1) return PathFailure(why);
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
