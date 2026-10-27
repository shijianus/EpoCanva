// EpoCanva W2 -- product engine configuration, deployment snapshot and start transaction.
// The contract is in product_engine_config.h; this file applies it.
#include "product_engine_config.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "engine_host.h"
#include "product_io.h"
#include "product_paths.h"
#include "product_settings.h"

namespace epocanva {
namespace product {
namespace {

constexpr wchar_t kStateHeader[] = L"epocanva-deployed-state 1";
constexpr DWORD kStartLockTimeoutMs = 60000;

std::mutex g_mutex;
bool g_paths_ready = false;
ProductPaths g_paths;
std::atomic<DWORD> g_lock_owner{0};

struct DeployedState {
  std::string fingerprint;
  std::string settings_token;
};

bool HashPresentFile(const std::filesystem::path& path, std::string* digest,
                     std::wstring* why) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    *digest = "missing";
    return true;
  }
  std::uint64_t size = 0;
  if (!Sha256File(path, digest, &size, why)) {
    return false;
  }
  return true;
}

bool HashRequiredFile(const std::filesystem::path& path, std::string* digest,
                      std::wstring* why) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    if (why != nullptr) {
      *why = L"required product data file is missing: " + path.wstring();
    }
    return false;
  }
  std::uint64_t size = 0;
  if (!Sha256File(path, digest, &size, why) || size == 0) {
    if (why != nullptr && why->empty()) {
      *why = L"required product data file is empty: " + path.wstring();
    }
    return false;
  }
  return true;
}

// The snapshot contains only file digests and the settings token. It is not a place to
// store user content.
bool ComputeFingerprint(const ProductPaths& paths, const std::string& settings_token,
                        std::string* fingerprint, std::wstring* why) {
  std::string schema;
  std::string default_config;
  std::string custom = "missing";
  std::string lexicon;
  std::string rime;
  if (!HashRequiredFile(paths.schema_file, &schema, why) ||
      !HashRequiredFile(paths.default_config, &default_config, why) ||
      !HashRequiredFile(paths.lexicon_file, &lexicon, why)) {
    return false;
  }
  std::error_code ec;
  if (std::filesystem::is_regular_file(paths.schema_patch_file, ec) && !ec) {
    if (!HashPresentFile(paths.schema_patch_file, &custom, why)) {
      return false;
    }
  }
  if (!HashPresentFile(paths.rime_dll, &rime, why)) {
    return false;
  }
  std::string text = "format=1\n";
  text += "settings=" + (settings_token.empty() ? std::string("-") : settings_token) + "\n";
  text += "schema=" + schema + "\ndefault=" + default_config + "\ncustom=" + custom +
          "\nlexicon=" + lexicon + "\nrime=" + rime + "\n";
  return Sha256(text.data(), text.size(), fingerprint, why);
}

bool ParseStateText(const std::string& text, DeployedState* state) {
  bool header = false;
  bool fingerprint = false;
  for (std::size_t start = 0; start <= text.size();) {
    const std::size_t newline = text.find('\n', start);
    std::string line = text.substr(
        start, newline == std::string::npos ? std::string::npos : newline - start);
    start = newline == std::string::npos ? text.size() + 1 : newline + 1;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    if (!header) {
      if (line != "epocanva-deployed-state 1") {
        return false;
      }
      header = true;
      continue;
    }
    if (line.rfind("fingerprint=", 0) == 0) {
      state->fingerprint = line.substr(12);
      fingerprint = true;
      continue;
    }
    if (line.rfind("settings_token=", 0) == 0) {
      state->settings_token = line.substr(15);
      if (state->settings_token == "-") {
        state->settings_token.clear();
      }
      continue;
    }
    return false;
  }
  return header && fingerprint && IsLowercaseHexSha256(state->fingerprint);
}

bool ReadDeployedState(const std::filesystem::path& path, DeployedState* state,
                       bool* present, std::wstring* why) {
  *present = false;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    return true;
  }
  std::vector<std::uint8_t> bytes;
  if (!ReadFileBytes(path, &bytes, why)) {
    return false;
  }
  const std::string text(bytes.begin(), bytes.end());
  if (!ParseStateText(text, state)) {
    // The state file is our own bookkeeping, not user settings. A corrupt record is
    // treated as absent so the next activation deploys and rewrites it.
    *state = DeployedState{};
    return true;
  }
  *present = true;
  return true;
}

bool WriteDeployedState(const ProductPaths& paths, const std::string& fingerprint,
                        const std::string& token, std::wstring* why) {
  std::string text = "epocanva-deployed-state 1\n";
  text += "fingerprint=" + fingerprint + "\n";
  text += "settings_token=" + (token.empty() ? std::string("-") : token) + "\n";
  return WriteTextAtomically(paths.deployed_state_file, text, why);
}

bool CompiledArtifactPresent(const ProductPaths& paths) {
  std::error_code ec;
  for (const std::filesystem::path& candidate :
       {paths.compiled_schema_file, paths.compiled_schema_binary}) {
    if (!std::filesystem::is_regular_file(candidate, ec) || ec) {
      continue;
    }
    const std::uintmax_t size = std::filesystem::file_size(candidate, ec);
    if (!ec && size > 0) {
      return true;
    }
  }
  return false;
}

epocanva::tsf::EngineHostConfig BaseConfig(const ProductPaths& paths) {
  epocanva::tsf::EngineHostConfig config;
  config.rime_dll_path = paths.rime_dll;
  config.shared_data_dir = paths.data_dir;
  config.user_data_dir = paths.engine_user_dir;
  config.log_dir = paths.log_dir;
  config.capacity = 32;
  return config;
}

bool EnsurePaths(std::wstring* why) {
  std::lock_guard<std::mutex> guard(g_mutex);
  if (!g_paths_ready) {
    if (why != nullptr) {
      *why = L"the product engine configuration has not been initialised";
    }
    return false;
  }
  return true;
}

std::wstring StartLockName(const ProductPaths& paths) {
  const std::string utf8 = WideToUtf8(paths.engine_user_dir.wstring());
  std::string digest;
  std::wstring why;
  if (!Sha256(utf8.data(), utf8.size(), &digest, &why)) {
    return L"";
  }
  return L"Local\\EpoCanva.Deploy." + Utf8ToWide(digest);
}

void ReleaseStartLock(void** mutex) {
  if (*mutex == nullptr) {
    return;
  }
  ::ReleaseMutex(static_cast<HANDLE>(*mutex));
  ::CloseHandle(static_cast<HANDLE>(*mutex));
  *mutex = nullptr;
  g_lock_owner.store(0);
}

}  // namespace

bool ApplyProductEngineConfig(HMODULE module, std::wstring* why) {
  std::lock_guard<std::mutex> guard(g_mutex);
  if (g_paths_ready) {
    return true;
  }
  ProductPaths paths;
  if (!ResolveProductPaths(module, &paths, why)) {
    return false;
  }
  if (!EnsureUserDirectories(paths, why)) {
    return false;
  }
  const epocanva::tsf::EngineHostConfig base = BaseConfig(paths);
  if (!base.valid() || !epocanva::tsf::SetEngineHostConfig(base)) {
    if (why != nullptr) {
      *why = L"the product engine configuration is invalid";
    }
    return false;
  }
  g_paths = std::move(paths);
  g_paths_ready = true;
  return true;
}

EngineStartTransaction::EngineStartTransaction() = default;

EngineStartTransaction::~EngineStartTransaction() { Abort(); }

bool EngineStartTransaction::Prepare(std::wstring* why) {
  if (prepared_) {
    if (why != nullptr) {
      *why = L"the engine start transaction is already prepared";
    }
    return false;
  }
  if (!EnsurePaths(why)) {
    return false;
  }

  ProductPaths paths;
  {
    std::lock_guard<std::mutex> guard(g_mutex);
    paths = g_paths;
  }

  const DWORD self = ::GetCurrentThreadId();
  if (g_lock_owner.load() == self) {
    if (why != nullptr) {
      *why = L"recursive engine activation on the same thread";
    }
    return false;
  }

  const std::wstring lock_name = StartLockName(paths);
  if (lock_name.empty()) {
    if (why != nullptr) {
      *why = L"cannot derive the per-user deployment lock name";
    }
    return false;
  }
  HANDLE mutex = ::CreateMutexW(nullptr, FALSE, lock_name.c_str());
  if (mutex == nullptr) {
    if (why != nullptr) {
      *why = L"CreateMutexW failed with error " + std::to_wstring(::GetLastError());
    }
    return false;
  }
  const DWORD waited = ::WaitForSingleObject(mutex, kStartLockTimeoutMs);
  if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {
    if (why != nullptr) {
      *why = L"another process is still preparing the engine (wait result " +
             std::to_wstring(waited) + L")";
    }
    ::CloseHandle(mutex);
    return false;
  }
  g_lock_owner.store(self);
  mutex_ = mutex;

  auto fail = [this](std::wstring* target, const wchar_t* message) {
    if (target != nullptr) {
      *target = message;
    }
    ReleaseStartLock(&mutex_);
    return false;
  };

  std::string token;
  std::error_code ec;
  if (std::filesystem::is_regular_file(paths.deploy_marker_file, ec) && !ec) {
    if (!ReadDeployMarkerToken(paths.deploy_marker_file, &token)) {
      return fail(why, L"the deploy marker is malformed; open EpoCanva and save settings again");
    }
  } else {
    ec.clear();
    // The marker carries the SHA-256 of the exact settings bytes and is removed after a
    // successful deployment. Re-derive the same token from settings.ini so the snapshot
    // identity does not change merely because the marker was retired.
    if (std::filesystem::is_regular_file(paths.settings_file, ec) && !ec) {
      std::uint64_t settings_size = 0;
      if (!Sha256File(paths.settings_file, &token, &settings_size, why)) {
        ReleaseStartLock(&mutex_);
        return false;
      }
      if (!IsLowercaseHexSha256(token)) {
        return fail(why, L"the settings digest is not a SHA-256");
      }
    }
  }

  std::string fingerprint;
  if (!ComputeFingerprint(paths, token, &fingerprint, why)) {
    ReleaseStartLock(&mutex_);
    return false;
  }

  DeployedState state;
  bool state_present = false;
  if (!ReadDeployedState(paths.deployed_state_file, &state, &state_present, why)) {
    ReleaseStartLock(&mutex_);
    return false;
  }
  const bool state_matched = state_present && state.fingerprint == fingerprint;
  const bool artifact_present = CompiledArtifactPresent(paths);

  const epocanva::tsf::EngineHostConfig base = BaseConfig(paths);
  const bool running = epocanva::tsf::EngineHostBroker::Instance().IsRunningFor(base);
  const bool snapshot_current = state_matched && artifact_present;
  const bool deployment_needed = !snapshot_current;

  epocanva::tsf::EngineHostConfig config = base;
  if (deployment_needed && !running) {
    epocanva::tsf::EngineHostDeployment deployment;
    deployment.config_file = paths.default_config;
    deployment.schema_files.push_back(paths.schema_file);
    config.deployment = std::move(deployment);
  }
  if (!config.valid() || !epocanva::tsf::SetEngineHostConfig(config)) {
    return fail(why, L"the product engine configuration is invalid");
  }

  prepared_ = true;
  deployment_requested_ = deployment_needed && !running;
  deferred_while_running_ = deployment_needed && running;
  token_ = std::move(token);
  fingerprint_ = std::move(fingerprint);
  return true;
}

bool EngineStartTransaction::Complete(std::wstring* why) {
  if (!prepared_) {
    if (why != nullptr) {
      *why = L"the engine start transaction was not prepared";
    }
    return false;
  }
  ProductPaths paths;
  {
    std::lock_guard<std::mutex> guard(g_mutex);
    paths = g_paths;
  }

  bool deployment_ok = true;
  bool record_ok = true;
  if (deployment_requested_) {
    if (!CompiledArtifactPresent(paths)) {
      deployment_ok = false;
      if (why != nullptr) {
        *why = L"the engine reported a successful start but produced no compiled schema";
      }
    } else {
      std::wstring record_why;
      record_ok = WriteDeployedState(paths, fingerprint_, token_, &record_why);
      if (!record_ok && why != nullptr) {
        *why = record_why;
      }
    }
  }

  // The marker is removed only after the snapshot record exists. A deferred change (another
  // activation already runs the old engine) keeps its marker for the next real start.
  if (deployment_ok && record_ok && !deferred_while_running_ && !token_.empty()) {
    if (!ClearDeployMarkerIfToken(paths.deploy_marker_file, token_)) {
      if (why != nullptr && why->empty()) {
        *why = L"the deployment succeeded but the settings marker could not be retired";
      }
    }
  }

  if (deployment_requested_ || !deferred_while_running_) {
    // Drop the one-shot request from the stored configuration. The broker has already
    // cleared it from the running environment; this keeps later acquisitions matching.
    (void)epocanva::tsf::SetEngineHostConfig(BaseConfig(paths));
  }

  ReleaseStartLock(&mutex_);
  prepared_ = false;
  return deployment_ok;
}

void EngineStartTransaction::Abort() {
  if (!prepared_) {
    ReleaseStartLock(&mutex_);
    return;
  }
  ReleaseStartLock(&mutex_);
  prepared_ = false;
  deployment_requested_ = false;
  deferred_while_running_ = false;
  token_.clear();
  fingerprint_.clear();
}

}  // namespace product
}  // namespace epocanva
