// EpoCanva W1 -- persisted user settings and their engine projection.
//
// Storage is a Win32 profile (INI) file written atomically. It never contains input
// history, clipboard text, focused-field names or application identifiers. The engine
// projection is a Rime user patch file plus a deploy marker: the marker's token is the
// SHA-256 of the exact settings bytes, so a save that happens while an engine is
// deploying cannot be lost by deleting the newer marker.
#ifndef EPOCANVA_PRODUCT_SETTINGS_H_
#define EPOCANVA_PRODUCT_SETTINGS_H_

#include <filesystem>
#include <string>

#include "product_paths.h"

namespace epocanva {
namespace product {

// W3-B: `host` follows the application's presentation decision. `always_self` keeps the
// TIP window even when the host claims the candidate element, which is the only way to see
// candidates in a host that answers pbShow=FALSE and then draws nothing. It can duplicate
// the list when the host really does draw. It never overrides an explicit host Show(FALSE).
enum class CandidateDisplay {
  kHost = 0,
  kAlwaysSelf = 1,
};

struct ProductSettings {
  bool completion_enabled = true;
  int completion_candidates = 7;  // 1..7
  bool spelling_enabled = false;
  int spelling_candidates = 3;  // 1..3
  CandidateDisplay candidate_display = CandidateDisplay::kHost;

  static ProductSettings Defaults() { return ProductSettings{}; }
  bool valid() const;
};

enum class SettingsLoadStatus {
  kLoaded = 0,
  kMissing,             // no file: defaults are returned
  kCorrupt,             // unreadable, malformed, or a missing/out-of-range key
  kUnsupportedVersion,  // format is newer than this build understands
};

struct SettingsLoadResult {
  ProductSettings settings;
  SettingsLoadStatus status = SettingsLoadStatus::kLoaded;
  std::wstring diagnostic;
  bool ok() const { return status == SettingsLoadStatus::kLoaded; }
};

SettingsLoadResult LoadProductSettings(const std::filesystem::path& path);
const wchar_t* DescribeSettingsLoadStatus(SettingsLoadStatus status);

std::string BuildSettingsText(const ProductSettings& settings);
bool SaveProductSettings(const std::filesystem::path& path,
                         const ProductSettings& settings,
                         std::string* settings_sha256, std::wstring* why);

std::string BuildSchemaPatch(const ProductSettings& settings);
// Writes settings.ini, the schema patch and the deploy marker. The marker is last so a
// failed save never asks the engine to deploy a half-updated pair.
bool SaveSettingsBundle(const ProductPaths& paths, const ProductSettings& settings,
                        std::string* settings_sha256, std::wstring* why);

bool ReadDeployMarkerToken(const std::filesystem::path& marker_path,
                           std::string* token);
// Deletes the marker only when it still carries `token` (the save that requested the
// deployment). Returns true when the marker is absent or was removed; false when a newer
// save replaced it, in which case the new marker stays for the next engine start.
bool ClearDeployMarkerIfToken(const std::filesystem::path& marker_path,
                              const std::string& token);

}  // namespace product
}  // namespace epocanva

#endif  // EPOCANVA_PRODUCT_SETTINGS_H_
