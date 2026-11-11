// EpoCanva W1 -- read-only product status.
//
// Collects only facts the user shell can observe: file presence and BCrypt digests,
// the per-user directory probe, the settings load result and whether the CLSID is
// registered (read-only registry query). It never starts the engine, never reads user
// input and never writes into the product directory.
#ifndef EPOCANVA_PRODUCT_DIAGNOSTICS_H_
#define EPOCANVA_PRODUCT_DIAGNOSTICS_H_

#include <windows.h>

#include <string>
#include <vector>

#include "product_paths.h"
#include "product_settings.h"

namespace epocanva {
namespace product {

enum class StatusLevel {
  kOk = 0,
  kWarning,
  kError,
};

struct StatusItem {
  std::wstring label;
  std::wstring detail;
  StatusLevel level = StatusLevel::kOk;
};

struct ProductStatus {
  ProductPaths paths;
  std::wstring version;
  std::wstring commit;
  std::wstring host;
  bool package_ready = false;  // local integrity/isolation only; not release acceptance
  bool manifest_ok = false;
  bool internal_only = true;  // scope for format 2; legacy diagnostic field for format 1
  int manifest_format = 0;
  std::string acceptance_profile;
  std::string distribution_policy;
  std::string path_exception;
  std::wstring manifest_issue;
  int manifest_files = 0;
  int manifest_failures = 0;
  bool user_data_ok = false;
  bool tsf_registered = false;
  SettingsLoadResult settings;
  std::vector<StatusItem> items;
};

// `module` is nullptr for the executable or the DLL HINSTANCE. The returned status is
// meaningful even when `why` is non-empty; `why` explains a path-resolution failure.
bool CollectProductStatus(HMODULE module, ProductStatus* status, std::wstring* why);
bool CollectProductStatusForPaths(const ProductPaths& paths, ProductStatus* status,
                                  std::wstring* why);

// Machine-readable UTF-8 report used by --diagnose and package tests. One key=value per
// line; paths are the only values that may contain spaces or non-ASCII characters.
std::string FormatProductReport(const ProductStatus& status);
// Human-readable lines for the settings window.
std::wstring FormatStatusLines(const ProductStatus& status);
// Short summary used in the window header.
std::wstring PackageSummary(const ProductStatus& status);

bool IsTextServiceRegistered(std::wstring* server_path);

}  // namespace product
}  // namespace epocanva

#endif  // EPOCANVA_PRODUCT_DIAGNOSTICS_H_
