// EpoCanva W1 -- product directory policy.
//
// Every path below is derived from a module handle or from a documented per-user
// override. The process working directory, the developer repository and the
// install directory's writability are never used as inputs. Install data is
// read-only; settings, logs and the Rime user directory live under the user root.
#ifndef EPOCANVA_PRODUCT_PATHS_H_
#define EPOCANVA_PRODUCT_PATHS_H_

#include <windows.h>

#include <filesystem>
#include <string>

namespace epocanva {
namespace product {

// Layout inside the product directory (the directory that holds EpoCanva.exe and
// epocanva_tsf.dll):
//   rime.dll
//   data/default.yaml
//   data/epocanva_english.schema.yaml
//   data/lexicon/epocanva_en_alpha.tsv
//   licenses/
//
// Per-user layout:
//   <user root>/settings.ini
//   <user root>/logs/
//   <user root>/engine/                       (Rime user data)
//   <user root>/engine/epocanva_english.custom.yaml
//   <user root>/engine/deploy-required
//   <user root>/engine/deployed-state          (W2 deployment snapshot record)
//   <user root>/engine/build/epocanva_english.schema.yaml  (compiled schema)
struct ProductPaths {
  std::filesystem::path module_path;
  std::filesystem::path product_root;
  std::filesystem::path data_dir;
  std::filesystem::path schema_file;
  std::filesystem::path default_config;
  std::filesystem::path lexicon_file;
  std::filesystem::path rime_dll;
  std::filesystem::path tsf_dll;
  std::filesystem::path licenses_dir;
  std::filesystem::path notice_file;
  std::filesystem::path manifest_file;
  std::filesystem::path user_root;
  std::filesystem::path engine_user_dir;
  std::filesystem::path log_dir;
  std::filesystem::path settings_file;
  std::filesystem::path schema_patch_file;
  std::filesystem::path deploy_marker_file;
  std::filesystem::path deployed_state_file;
  std::filesystem::path engine_lock_file;
  std::filesystem::path compiled_schema_file;
  std::filesystem::path compiled_schema_binary;
};

// `module` is nullptr for the executable or the DLL's HINSTANCE for the text service.
// The optional user root is the documented EPOCANVA_USER_DATA_ROOT override (absolute);
// when empty, %LOCALAPPDATA%\EpoCanva is used. Nothing is created here. Existing
// ancestors are checked with no-follow handles; UNC, device aliases, nonphysical
// drive mappings and redirecting reparses are refused. ProductPaths itself remains
// a layout, not a retained handle capability: consumers must pin again before I/O.
bool ResolveProductPaths(HMODULE module, ProductPaths* paths, std::wstring* why);

void SetUserRootOverrideForProcess(const std::filesystem::path& root);
std::filesystem::path UserRootOverrideForProcess();

// Creates only the admitted user_root/engine/logs hierarchy, one component at a
// time with pinned parents and re-validation after creation. Pins end on return;
// this does not guard a later unrelated pathname operation or concurrent leaf swap.
bool EnsureUserDirectories(const ProductPaths& paths, std::wstring* why);

// A bounded write probe: pins directories, exclusively creates a new file below
// user_root, writes one byte and deletes that same handle on close.
bool ProbeUserDataWritable(const ProductPaths& paths, std::wstring* why);

std::wstring ProductVersion();
std::wstring ProductCommit();
std::wstring ProductVersionLine();

}  // namespace product
}  // namespace epocanva

#endif  // EPOCANVA_PRODUCT_PATHS_H_
