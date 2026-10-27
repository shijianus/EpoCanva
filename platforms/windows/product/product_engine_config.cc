#include "product_engine_config.h"

#include <filesystem>
#include <mutex>
#include <utility>

#include "engine_host.h"
#include "product_paths.h"
#include "product_settings.h"

namespace epocanva {
namespace product {
namespace {

std::mutex g_mutex;
bool g_applied = false;
std::string g_pending_token;
std::filesystem::path g_marker_path;

}  // namespace

bool ApplyProductEngineConfig(HMODULE module, std::wstring* why) {
  std::lock_guard<std::mutex> guard(g_mutex);
  if (g_applied) {
    return true;
  }
  ProductPaths paths;
  if (!ResolveProductPaths(module, &paths, why)) {
    return false;
  }
  if (!EnsureUserDirectories(paths, why)) {
    return false;
  }

  std::error_code ec;
  const bool marker_exists =
      std::filesystem::is_regular_file(paths.deploy_marker_file, ec) && !ec;
  std::string token;
  if (marker_exists && !ReadDeployMarkerToken(paths.deploy_marker_file, &token)) {
    if (why != nullptr) {
      *why = L"the deploy marker is malformed; open EpoCanva and save settings again";
    }
    return false;
  }

  epocanva::tsf::EngineHostConfig config;
  config.rime_dll_path = paths.rime_dll;
  config.shared_data_dir = paths.data_dir;
  config.user_data_dir = paths.engine_user_dir;
  config.log_dir = paths.log_dir;
  config.capacity = 32;
  if (!token.empty()) {
    epocanva::tsf::EngineHostDeployment deployment;
    deployment.config_file = paths.default_config;
    deployment.schema_files.push_back(paths.schema_file);
    config.deployment = std::move(deployment);
  }
  if (!config.valid() || !epocanva::tsf::SetEngineHostConfig(config)) {
    if (why != nullptr) {
      *why = L"the product engine configuration is invalid";
    }
    return false;
  }
  g_applied = true;
  g_pending_token = token;
  g_marker_path = paths.deploy_marker_file;
  return true;
}

void NotifyEngineStarted() {
  std::string token;
  std::filesystem::path marker;
  {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_pending_token.empty()) {
      return;
    }
    token = std::move(g_pending_token);
    g_pending_token.clear();
    marker = g_marker_path;
  }
  ClearDeployMarkerIfToken(marker, token);
}

bool PendingDeploymentToken(std::string* token) {
  if (token == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> guard(g_mutex);
  *token = g_pending_token;
  return !g_pending_token.empty();
}

}  // namespace product
}  // namespace epocanva
