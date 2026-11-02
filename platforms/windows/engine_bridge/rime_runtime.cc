// EpoCanva P3.4 -- runtime implementation. See rime_runtime.h for the contract.
#include "rime_runtime.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../../../engine/language_packages/module_requirements.h"
#include "english_diagnostics.h"
#include "rime_api.h"
#include "rime_runtime_internal.h"
#include "../local_path/local_path.h"

namespace epocanva {
namespace windows {
namespace engine {
namespace {

// One RimeInitialize cycle per process. The librime service behind it is a process-global
// singleton (src/rime/service.cc:196-203), so two runtimes would tear down each other's
// modules. The flag is released only after a successful finalize + unload, or after a failed
// Start that cleaned up completely.
std::atomic<bool> g_runtime_slot_held{false};

constexpr char kAppName[] = "rime.epocanva.engine_bridge";
constexpr char kDistributionName[] = "EpoCanva";
constexpr char kDistributionCodeName[] = "epocanva";
constexpr char kDistributionVersion[] = "0.0.0-p3.4";

std::string LastErrorText(const char* what, DWORD error) {
  return std::string(what) + " failed (GetLastError=" + std::to_string(error) + ")";
}

namespace local = ::epocanva::windows::local_path;

struct RuntimePaths {
  local::PreparedPath dll;
  local::PreparedPath shared;
  local::PreparedPath user;
  local::PreparedPath log;
  std::vector<local::PreparedPath> schemas;
  std::vector<local::PinnedPath> pins;
  std::wstring dll_load_path;
};

local::PreparedPath ChildPath(local::PreparedPath parent, const std::wstring& name) {
  parent.components.push_back(name);
  parent.display /= name;
  parent.physical /= name;
  return parent;
}

std::wstring LoadedModulePath(HMODULE module) {
  std::vector<wchar_t> buffer(1024, L'\0');
  DWORD written =
      ::GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (written == 0) {
    return std::wstring();
  }
  if (written >= buffer.size()) {
    buffer.assign(static_cast<std::size_t>(written) + 1, L'\0');
    written = ::GetModuleFileNameW(module, buffer.data(),
                                   static_cast<DWORD>(buffer.size()));
    if (written == 0 || written >= buffer.size()) {
      return std::wstring();
    }
  }
  return std::wstring(buffer.data(), written);
}

bool SamePath(const std::wstring& left, const std::wstring& right) {
  if (left.empty() || right.empty()) {
    return false;
  }
  return ::CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) ==
         CSTR_EQUAL;
}

bool WideToUtf8(const std::wstring& wide, std::string* out) {
  out->clear();
  if (wide.empty()) {
    return true;
  }
  const int bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                          wide.data(), static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) {
    return false;
  }
  out->resize(static_cast<std::size_t>(bytes));
  const int written = ::WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
      out->data(), bytes, nullptr, nullptr);
  if (written != bytes) {
    out->clear();
    return false;
  }
  return true;
}

std::string WidePathToUtf8(const std::filesystem::path& path) {
  std::string out;
  if (!WideToUtf8(path.native(), &out)) {
    return std::string();
  }
  return out;
}

bool IsSafeSchemaId(const std::string& schema_id) {
  if (schema_id.empty() || schema_id == "." || schema_id == "..") {
    return false;
  }
  for (const char ch : schema_id) {
    const bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    const bool digit = ch >= '0' && ch <= '9';
    if (!letter && !digit && ch != '_' && ch != '-' && ch != '.') {
      return false;
    }
  }
  return true;
}

bool ValidateOptions(const RuntimeOptions& options, RuntimePaths* paths, std::string* why) {
  // Check ALL caller paths and local DOS mappings before the first filesystem open.
  // No is_directory/weakly_canonical/GetFullPathName may precede this boundary.
  if (!local::Prepare(options.rime_dll_path, &paths->dll) ||
      paths->dll.components.empty() ||
      !local::Prepare(options.shared_data_dir, &paths->shared) ||
      !local::Prepare(options.user_data_dir, &paths->user) ||
      !local::Prepare(options.log_dir, &paths->log)) {
    *why = "runtime paths must use unaliased absolute local physical-disk paths";
    return false;
  }
  const language_packages::ModuleRequirementsStatus module_status =
      language_packages::ValidateModuleRequirements(options.required_modules);
  if (module_status != language_packages::ModuleRequirementsStatus::kOk) {
    switch (module_status) {
      case language_packages::ModuleRequirementsStatus::kEmpty:
        *why = "required_modules must not be empty";
        break;
      case language_packages::ModuleRequirementsStatus::kUnsafeName:
        *why = "required_modules contains an unsafe module identifier";
        break;
      case language_packages::ModuleRequirementsStatus::kUnapprovedName:
        *why = "required_modules contains an unapproved built-in module";
        break;
      case language_packages::ModuleRequirementsStatus::kDuplicate:
        *why = "required_modules contains a duplicate module identifier";
        break;
      case language_packages::ModuleRequirementsStatus::kOk:
        break;
    }
    return false;
  }
  if (options.deployment.has_value()) {
    const DeploymentOptions& deployment = *options.deployment;
    if (deployment.schema_files.empty() ||
        !local::SafeComponent(std::filesystem::u8path(deployment.config_file_name).native())) {
      *why = "deployment requires schema files and a safe bare config file name";
      return false;
    }
    for (const auto& schema : deployment.schema_files) {
      local::PreparedPath prepared;
      if (!local::Prepare(schema, &prepared) ||
          !SamePath(prepared.physical.parent_path().native(), paths->shared.physical.native())) {
        *why = "deployment schemas must be local files directly inside shared_data_dir";
        return false;
      }
      paths->schemas.push_back(std::move(prepared));
    }
  }
  return true;
}

bool PinPath(RuntimePaths* paths, const local::PreparedPath& path, bool directory,
             bool allow_missing = false) {
  local::PinnedPath pin;
  if (!pin.Open(path, directory, allow_missing)) return false;
  paths->pins.push_back(std::move(pin));
  return true;
}

bool PinDirectories(RuntimePaths* paths, std::string* why) {
  for (const auto* root : {&paths->shared, &paths->user, &paths->log}) {
    if (!PinPath(paths, *root, true)) {
      *why = "a runtime directory is missing, redirected, recalled or cannot be pinned";
      return false;
    }
  }
  // Rime derives these children (third_party/librime/src/rime/setup.cc:56-65).
  // Do not allow an existing build junction to bypass an otherwise safe root.
  for (const auto* root : {&paths->shared, &paths->user}) {
    if (!PinPath(paths, ChildPath(*root, L"build"), true, true)) {
      *why = "a runtime build directory cannot be pinned without redirection";
      return false;
    }
  }
  for (const auto& schema : paths->schemas) {
    if (!PinPath(paths, schema, false)) {
      *why = "a deployment schema cannot be pinned as a resident local file";
      return false;
    }
  }
  return true;
}

std::string MissingBaseSlots(RimeApi* api) {
  std::string missing;
  auto note = [&missing](const char* name, bool present) {
    if (!present) {
      if (!missing.empty()) {
        missing += ",";
      }
      missing += name;
    }
  };
#define EPOCANVA_SLOT(name) note(#name, RIME_API_AVAILABLE(api, name) != 0)
  EPOCANVA_SLOT(setup);
  EPOCANVA_SLOT(initialize);
  EPOCANVA_SLOT(finalize);
  EPOCANVA_SLOT(create_session);
  EPOCANVA_SLOT(find_session);
  EPOCANVA_SLOT(destroy_session);
  EPOCANVA_SLOT(process_key);
  EPOCANVA_SLOT(select_candidate_on_current_page);
  EPOCANVA_SLOT(get_context);
  EPOCANVA_SLOT(free_context);
  EPOCANVA_SLOT(get_status);
  EPOCANVA_SLOT(free_status);
  EPOCANVA_SLOT(get_commit);
  EPOCANVA_SLOT(free_commit);
  EPOCANVA_SLOT(get_input);
  EPOCANVA_SLOT(select_schema);
  EPOCANVA_SLOT(schema_open);
  EPOCANVA_SLOT(config_close);
  EPOCANVA_SLOT(get_version);
  EPOCANVA_SLOT(find_module);
#undef EPOCANVA_SLOT
  return missing;
}

std::string MissingDeploymentSlots(RimeApi* api) {
  std::string missing;
  auto note = [&missing](const char* name, bool present) {
    if (!present) {
      if (!missing.empty()) {
        missing += ",";
      }
      missing += name;
    }
  };
#define EPOCANVA_SLOT(name) note(#name, RIME_API_AVAILABLE(api, name) != 0)
  EPOCANVA_SLOT(deploy_config_file);
  EPOCANVA_SLOT(deploy_schema);
#undef EPOCANVA_SLOT
  return missing;
}

RuntimeStartStatus CheckBaseModules(RuntimeCore& core, std::string* why) {
  const char* const required[] = {"core", "dict", "gears", "default"};
  for (const char* name : required) {
    if (core.api->find_module(name) == nullptr) {
      *why = std::string("required module is not registered: ") + name;
      return RuntimeStartStatus::kBaseModuleMissing;
    }
  }
  core.base_modules_registered = true;
  return RuntimeStartStatus::kOk;
}

RuntimeStartStatus CheckRequestedModules(RuntimeCore& core,
                                         const std::vector<std::string>& requested,
                                         std::string* why) {
  for (const std::string& name : requested) {
    if (core.api->find_module(name.c_str()) == nullptr) {
      *why = std::string("required module is not registered: ") + name;
      return name == EPOCANVA_ENGLISH_MODULE_NAME
                 ? RuntimeStartStatus::kEnglishModuleMissing
                 : RuntimeStartStatus::kBaseModuleMissing;
    }
  }
  return RuntimeStartStatus::kOk;
}

RuntimeStartStatus CheckEnglishModule(RuntimeCore& core, std::string* why) {
  RimeModule* module = core.api->find_module(EPOCANVA_ENGLISH_MODULE_NAME);
  if (module == nullptr) {
    *why = "the loaded rime.dll did not register " EPOCANVA_ENGLISH_MODULE_NAME;
    return RuntimeStartStatus::kEnglishModuleMissing;
  }
  if (!RIME_STRUCT_HAS_MEMBER(*module, module->get_api) || module->get_api == nullptr) {
    *why = "the English module has no get_api slot";
    return RuntimeStartStatus::kEnglishModuleIncompatible;
  }
  RimeCustomApi* custom = module->get_api();
  if (custom == nullptr) {
    *why = "the English module returned a null custom API";
    return RuntimeStartStatus::kEnglishModuleIncompatible;
  }
  const int expected_data_size =
      static_cast<int>(sizeof(EpoCanvaEnglishApi) - sizeof(int));
  if (custom->data_size < expected_data_size) {
    *why = "the English custom API is an older/shorter table than this bridge";
    return RuntimeStartStatus::kEnglishModuleIncompatible;
  }
  EpoCanvaEnglishApi* english = reinterpret_cast<EpoCanvaEnglishApi*>(custom);
  if (english->get_counters == nullptr) {
    *why = "the English custom API has no get_counters slot";
    return RuntimeStartStatus::kEnglishModuleIncompatible;
  }
  RIME_STRUCT(EpoCanvaEnglishCounters, counters);
  if (english->get_counters(&counters) == False) {
    *why = "the English module refused to copy its counters";
    return RuntimeStartStatus::kEnglishModuleIncompatible;
  }
  if (counters.module_initialized == 0) {
    *why = "the English module's initialize() has not run";
    return RuntimeStartStatus::kEnglishModuleIncompatible;
  }
  core.english_module_initialized = counters.module_initialized;
  core.english_module_loaded = true;
  return RuntimeStartStatus::kOk;
}

bool DestroyEngineSession(RuntimeCore& core, RimeSessionId session_id,
                          std::string* diagnostic) {
  core.counts.destroy_session_calls += 1;
  if (core.api->destroy_session(session_id) == False) {
    if (!diagnostic->empty()) {
      *diagnostic += "; ";
    }
    *diagnostic += "destroy_session refused during failure cleanup";
    return false;
  }
  return true;
}

}  // namespace

struct RimeRuntime::Impl {
  std::shared_ptr<RuntimeCore> core = std::make_shared<RuntimeCore>();
  std::unique_ptr<RuntimePaths> paths;
  bool start_attempted = false;
  bool initialize_entered = false;
  bool shutdown_done = false;
  // A partially finalized/unloaded service cannot be retried or shared with a new
  // runtime. Preserve the terminal failure for executor/broker cleanup reporting.
  RuntimeShutdownStatus shutdown_failure = RuntimeShutdownStatus::kOk;
  std::string last_start_diagnostic;
  std::string last_shutdown_diagnostic;
};

RimeRuntime::RimeRuntime() : impl_(new Impl) {}

RimeRuntime::~RimeRuntime() {
  if (impl_ && impl_->shutdown_done && impl_->core->module != nullptr) {
    (void)impl_->paths.release();  // Quarantine pins with a terminally retained DLL.
  }
  if (!impl_ || !impl_->start_attempted || impl_->shutdown_done) {
    return;
  }
  // Only the owner thread may finalize. On any other thread the DLL is deliberately left
  // loaded instead of being finalized and unloaded underneath sessions this destructor cannot
  // safely touch. That is a controlled leak, not a hidden success.
  if (impl_->core->IsOwnerThread()) {
    (void)Shutdown();
  }
  if (impl_->core->module != nullptr) {
    // Match the deliberately retained DLL on wrong-thread/failed cleanup.
    (void)impl_->paths.release();
  }
}

RuntimeStartStatus RimeRuntime::Start(const RuntimeOptions& options) {
  Impl& impl = *impl_;
  RuntimeCore& core = *impl.core;
  if (impl.start_attempted) {
    return RuntimeStartStatus::kAlreadyStarted;
  }
  impl.start_attempted = true;

  std::string why;
  auto paths = std::make_unique<RuntimePaths>();
  if (!ValidateOptions(options, paths.get(), &why) || !PinDirectories(paths.get(), &why)) {
    impl.last_start_diagnostic = why;
    impl.shutdown_done = true;
    return RuntimeStartStatus::kInvalidOptions;
  }
  local::PinnedPath dll_pin;
  if (!dll_pin.Open(paths->dll, false)) {
    impl.last_start_diagnostic = "rime_dll_path cannot be pinned as a resident local file";
    impl.shutdown_done = true;
    return dll_pin.error() == ERROR_FILE_NOT_FOUND || dll_pin.error() == ERROR_PATH_NOT_FOUND
        ? RuntimeStartStatus::kDllLoadFailed : RuntimeStartStatus::kInvalidOptions;
  }
  // The Win32 PE loader rejects this build's GLOBALROOT and volume-GUID paths
  // with ERROR_INVALID_PARAMETER. Keep the file/ancestors pinned, use the admitted
  // DOS spelling, and recheck its physical mapping at the loader boundary. A
  // concurrent DOS-device remap is not prevented by these filesystem handles.
  paths->dll_load_path = paths->dll.display.native();
  paths->pins.push_back(std::move(dll_pin));

  bool expected = false;
  if (!g_runtime_slot_held.compare_exchange_strong(expected, true)) {
    impl.last_start_diagnostic =
        "another RimeRuntime in this process already holds the librime service";
    impl.shutdown_done = true;
    return RuntimeStartStatus::kAnotherRuntimeActive;
  }

  core.started = true;
  core.owner_thread = std::this_thread::get_id();
  impl.paths = std::move(paths);
  core.requested_dll_path = impl.paths->dll.display.native();

  auto fail = [&](RuntimeStartStatus status, std::string diagnostic) {
    if (impl.initialize_entered) {
      // A throw may leave only part of librime finalized. Never enter it twice.
      impl.initialize_entered = false;
      core.finalize_calls += 1;
      try {
        core.api->finalize();
      } catch (...) {
        impl.shutdown_failure = RuntimeShutdownStatus::kFinalizeFailed;
        impl.last_shutdown_diagnostic = "finalize() threw during failed Start cleanup";
        diagnostic += "; finalize() threw, the DLL and runtime slot remain held";
      }
    }
    core.initialized = false;
    core.english_module_loaded = false;
    if (impl.shutdown_failure == RuntimeShutdownStatus::kOk && core.module != nullptr) {
      if (::FreeLibrary(core.module) != FALSE) {
        core.module = nullptr;
      } else {
        impl.shutdown_failure = RuntimeShutdownStatus::kUnloadFailed;
        impl.last_shutdown_diagnostic = LastErrorText("FreeLibrary", ::GetLastError());
        diagnostic += "; FreeLibrary failed, the DLL and runtime slot remain held";
      }
    }
    core.api = nullptr;
    core.started = false;
    core.owner_thread = std::thread::id();
    core.librime_version.clear();
    impl.last_start_diagnostic = std::move(diagnostic);
    impl.shutdown_done = true;
    if (impl.shutdown_failure == RuntimeShutdownStatus::kOk) {
      impl.paths.reset();
      g_runtime_slot_held.store(false);
    } else {
      (void)impl.paths.release();  // Retain namespace pins with the quarantined DLL.
    }
    return status;
  };

  if (!WideToUtf8(impl.paths->shared.physical.native(), &core.shared_data_dir_utf8) ||
      !WideToUtf8(impl.paths->user.physical.native(), &core.user_data_dir_utf8) ||
      !WideToUtf8(impl.paths->log.physical.native(), &core.log_dir_utf8)) {
    return fail(RuntimeStartStatus::kInvalidOptions,
                "a runtime directory cannot be encoded as UTF-8");
  }

  local::PreparedPath loader_path;
  if (!local::Prepare(impl.paths->dll_load_path, &loader_path) ||
      !SamePath(loader_path.physical.native(), impl.paths->dll.physical.native())) {
    return fail(RuntimeStartStatus::kInvalidOptions, "the DLL drive mapping changed before load");
  }
  const DWORD load_flags =
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
  HMODULE module =
      ::LoadLibraryExW(impl.paths->dll_load_path.c_str(), nullptr, load_flags);
  if (module == nullptr) {
    return fail(RuntimeStartStatus::kDllLoadFailed,
                LastErrorText("LoadLibraryExW", ::GetLastError()));
  }
  core.module = module;

  core.loaded_dll_path = LoadedModulePath(module);
  if (core.loaded_dll_path.empty()) {
    return fail(RuntimeStartStatus::kDllIdentityMismatch,
                "GetModuleFileNameW could not name the loaded module");
  }
  local::PreparedPath loaded;
  if (!local::Prepare(core.loaded_dll_path, &loaded) ||
      !SamePath(loaded.physical.native(), impl.paths->dll.physical.native())) {
    return fail(RuntimeStartStatus::kDllIdentityMismatch,
                "the loaded module's path is not the pinned requested path");
  }

  FARPROC symbol = ::GetProcAddress(module, "rime_get_api");
  if (symbol == nullptr) {
    return fail(RuntimeStartStatus::kApiSymbolMissing,
                LastErrorText("GetProcAddress(rime_get_api)", ::GetLastError()));
  }
  HMODULE symbol_owner = nullptr;
  if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(symbol), &symbol_owner) == FALSE ||
      symbol_owner != module) {
    return fail(RuntimeStartStatus::kDllIdentityMismatch,
                "rime_get_api does not belong to the loaded module");
  }

  using GetApiFn = RimeApi* (*)();
  GetApiFn get_api = reinterpret_cast<GetApiFn>(symbol);
  RimeApi* api = get_api();
  if (api == nullptr) {
    return fail(RuntimeStartStatus::kApiTableIncomplete,
                "rime_get_api returned null");
  }
  core.api = api;
  if (api->data_size <= 0) {
    return fail(RuntimeStartStatus::kApiTableIncomplete,
                "the RimeApi table has no versioned payload");
  }
  const std::string missing = MissingBaseSlots(api);
  if (!missing.empty()) {
    return fail(RuntimeStartStatus::kApiTableIncomplete,
                "missing RimeApi slots: " + missing);
  }
  if (options.deployment.has_value()) {
    const std::string deploy_missing = MissingDeploymentSlots(api);
    if (!deploy_missing.empty()) {
      return fail(RuntimeStartStatus::kDeploymentApiMissing,
                  "missing deployment slots: " + deploy_missing);
    }
  }
  core.api_complete = true;
  core.librime_version = api->get_version() == nullptr ? std::string() : api->get_version();
  if (core.librime_version.empty()) {
    return fail(RuntimeStartStatus::kApiTableIncomplete,
                "RimeApi::get_version returned an empty value");
  }

  // Traits. Every pointer below addresses storage owned by `core`, which is held by this
  // runtime and by every session; nothing points at a temporary. The NULL prebuilt/staging
  // slots make librime derive <shared>/build and <user>/build from the explicit directories
  // (src/rime/setup.cc:56-65), never from the working directory.
  RIME_STRUCT_INIT(RimeTraits, core.traits);
  core.traits.shared_data_dir = core.shared_data_dir_utf8.c_str();
  core.traits.user_data_dir = core.user_data_dir_utf8.c_str();
  core.traits.log_dir = core.log_dir_utf8.c_str();
  core.traits.prebuilt_data_dir = nullptr;
  core.traits.staging_dir = nullptr;
  core.traits.app_name = kAppName;
  core.traits.distribution_name = kDistributionName;
  core.traits.distribution_code_name = kDistributionCodeName;
  core.traits.distribution_version = kDistributionVersion;
  core.traits.min_log_level = 3;
  core.modules.clear();
  core.modules.push_back("default");
  for (const std::string& required_module : options.required_modules) {
    const char* approved = language_packages::ApprovedModuleName(required_module);
    if (approved == nullptr) {
      return fail(RuntimeStartStatus::kBaseModuleMissing,
                  "required module was not admitted by the runtime contract");
    }
    const bool already_requested = std::any_of(core.modules.begin(), core.modules.end(),
        [&required_module](const char* name) { return required_module == name; });
    if (!already_requested) {
      core.modules.push_back(approved);
    }
  }
  if (options.deployment.has_value()) {
    core.modules.push_back("levers");
  }
  core.modules.push_back(nullptr);
  core.traits.modules = core.modules.data();

  try {
    api->setup(&core.traits);
  } catch (const std::exception& ex) {
    return fail(RuntimeStartStatus::kSetupFailed,
                std::string("RimeApi::setup threw: ") + ex.what());
  } catch (...) {
    return fail(RuntimeStartStatus::kSetupFailed, "RimeApi::setup threw");
  }

  impl.initialize_entered = true;
  try {
    api->initialize(&core.traits);
  } catch (const std::exception& ex) {
    return fail(RuntimeStartStatus::kInitializeFailed,
                std::string("RimeApi::initialize threw: ") + ex.what());
  } catch (...) {
    return fail(RuntimeStartStatus::kInitializeFailed, "RimeApi::initialize threw");
  }
  core.initialize_calls += 1;

  RuntimeStartStatus module_status = CheckBaseModules(core, &why);
  if (module_status != RuntimeStartStatus::kOk) {
    return fail(module_status, why);
  }
  module_status = CheckRequestedModules(core, options.required_modules, &why);
  if (module_status != RuntimeStartStatus::kOk) {
    return fail(module_status, why);
  }
  if (std::find(options.required_modules.begin(), options.required_modules.end(),
                std::string(EPOCANVA_ENGLISH_MODULE_NAME)) != options.required_modules.end()) {
    module_status = CheckEnglishModule(core, &why);
    if (module_status != RuntimeStartStatus::kOk) {
      return fail(module_status, why);
    }
  }

  if (options.deployment.has_value()) {
    if (api->find_module("levers") == nullptr) {
      return fail(RuntimeStartStatus::kBaseModuleMissing,
                  "the levers module is not registered, so deployment is unavailable");
    }
    core.counts.deploy_calls += 1;
    core.deploy_calls += 1;
    if (api->deploy_config_file(options.deployment->config_file_name.c_str(),
                                "config_version") == False) {
      return fail(RuntimeStartStatus::kDeploymentFailed,
                  "deploy_config_file refused " + options.deployment->config_file_name);
    }
    for (const auto& schema : impl.paths->schemas) {
      const std::string schema_utf8 = WidePathToUtf8(schema.physical);
      core.counts.deploy_calls += 1;
      core.deploy_calls += 1;
      if (api->deploy_schema(schema_utf8.c_str()) == False) {
        return fail(RuntimeStartStatus::kDeploymentFailed,
                    "deploy_schema refused a file named in the deployment request");
      }
    }
  }

  core.initialized = true;
  impl.last_start_diagnostic.clear();
  return RuntimeStartStatus::kOk;
}

RuntimeShutdownStatus RimeRuntime::Shutdown() {
  Impl& impl = *impl_;
  RuntimeCore& core = *impl.core;
  if (!impl.start_attempted) {
    return RuntimeShutdownStatus::kNotStarted;
  }
  if (impl.shutdown_failure != RuntimeShutdownStatus::kOk) {
    return impl.shutdown_failure;
  }
  if (impl.shutdown_done) {
    return RuntimeShutdownStatus::kAlreadyShutdown;
  }
  if (!core.IsOwnerThread()) {
    impl.last_shutdown_diagnostic =
        "Shutdown refused: caller is not the runtime owner thread";
    return RuntimeShutdownStatus::kWrongThread;
  }
  if (core.live_sessions != 0) {
    impl.last_shutdown_diagnostic =
        "Shutdown refused: a session created by this runtime is still open";
    return RuntimeShutdownStatus::kLiveSessionsRemain;
  }

  if (impl.initialize_entered) {
    impl.initialize_entered = false;
    core.initialized = false;
    core.english_module_loaded = false;
    core.finalize_calls += 1;
    try {
      core.api->finalize();
    } catch (...) {
      // Keep the DLL mapped and the slot reserved for the rest of the process.
      // Mark terminal before recording text so even allocation failure cannot retry.
      impl.shutdown_failure = RuntimeShutdownStatus::kFinalizeFailed;
      impl.shutdown_done = true;
      impl.last_shutdown_diagnostic = "RimeApi::finalize threw; runtime quarantined";
      return impl.shutdown_failure;
    }
  }
  core.initialized = false;
  core.english_module_loaded = false;

  if (core.module != nullptr) {
    if (::FreeLibrary(core.module) == FALSE) {
      impl.shutdown_failure = RuntimeShutdownStatus::kUnloadFailed;
      impl.shutdown_done = true;
      impl.last_shutdown_diagnostic = LastErrorText("FreeLibrary", ::GetLastError());
      // The loader still owns this reference. Retain the handle and slot rather
      // than making another runtime reuse a DLL whose unload failed.
      core.api = nullptr;
      core.started = false;
      core.owner_thread = std::thread::id();
      return impl.shutdown_failure;
    }
    core.module = nullptr;
  }
  core.api = nullptr;
  core.started = false;
  core.owner_thread = std::thread::id();
  impl.shutdown_done = true;
  impl.last_shutdown_diagnostic.clear();
  impl.paths.reset();
  g_runtime_slot_held.store(false);
  return RuntimeShutdownStatus::kOk;
}

bool RimeRuntime::IsInitialized() const { return impl_->core->initialized; }

bool RimeRuntime::IsOwnerThread() const { return impl_->core->IsOwnerThread(); }

int RimeRuntime::live_session_count() const { return impl_->core->live_sessions; }

RuntimeInfo RimeRuntime::Info() const {
  const RuntimeCore& core = *impl_->core;
  RuntimeInfo info;
  info.requested_dll_path = core.requested_dll_path;
  info.loaded_dll_path = core.loaded_dll_path;
  info.shared_data_dir = std::filesystem::u8path(core.shared_data_dir_utf8).wstring();
  info.user_data_dir = std::filesystem::u8path(core.user_data_dir_utf8).wstring();
  info.log_dir = std::filesystem::u8path(core.log_dir_utf8).wstring();
  info.librime_version = core.librime_version;
  info.initialized = core.initialized;
  info.api_complete = core.api_complete;
  info.base_modules_registered = core.base_modules_registered;
  info.english_module_loaded = core.english_module_loaded;
  info.english_module_initialized = core.english_module_initialized;
  info.initialize_calls = core.initialize_calls;
  info.finalize_calls = core.finalize_calls;
  info.deploy_calls = core.deploy_calls;
  info.live_session_count = core.live_sessions;
  info.last_start_diagnostic = impl_->last_start_diagnostic;
  info.last_shutdown_diagnostic = impl_->last_shutdown_diagnostic;
  return info;
}

ApiCallCounts RimeRuntime::ApiCounts() const { return impl_->core->counts; }

SessionCreateResult RimeRuntime::CreateSession(const std::string& schema_id) {
  SessionCreateResult result;
  RuntimeCore& core = *impl_->core;
  if (!core.initialized) {
    result.status = SessionCreateStatus::kNotStarted;
    result.diagnostic = "CreateSession refused: runtime is not initialized";
    return result;
  }
  if (!core.IsOwnerThread()) {
    result.status = SessionCreateStatus::kWrongThread;
    result.diagnostic = "CreateSession refused: caller is not the runtime owner thread";
    return result;
  }
  if (!IsSafeSchemaId(schema_id)) {
    result.status = SessionCreateStatus::kInvalidSchemaId;
    result.diagnostic = "schema_id must be a non-empty identifier of letters, digits, "
                        "underscore, dash or dot; path separators are refused";
    return result;
  }

  // RimeConfig does not distinguish "file absent" from "loaded an empty config": the config
  // loader returns a Config whose LoadFromFile failed (src/rime/config/config_component.cc:
  // 178-195). The prepared-directory check is therefore explicit and comes first. The compiled
  // name is the one SchemaComponent asks for, schema_id + ".schema" + ".yaml"
  // (src/rime/schema.cc:40-42). ConfigFileUpdate writes it into staging_dir = <user>/build
  // (src/rime/setup.cc:62-65); the deployed resolver falls back to prebuilt_data_dir =
  // <shared>/build (src/rime/service.cc:181-187).
  const std::wstring compiled_name = std::filesystem::u8path(schema_id + ".schema.yaml").native();
  local::PinnedPath compiled;
  if (!compiled.Open(ChildPath(ChildPath(impl_->paths->user, L"build"), compiled_name), false)) {
    // Only absence permits fallback. Never fall through a redirected user schema
    // and subsequently let schema_open follow it while the shared copy exists.
    const DWORD error = compiled.error();
    if ((error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) ||
        !compiled.Open(ChildPath(ChildPath(impl_->paths->shared, L"build"), compiled_name), false)) {
      result.status = SessionCreateStatus::kSchemaUnavailable;
      result.diagnostic = "no resident local compiled schema can be pinned for this id";
      return result;
    }
  }
  impl_->paths->pins.push_back(std::move(compiled));

  RimeConfig config{};
  const Bool opened = core.api->schema_open(schema_id.c_str(), &config);
  if (opened == False) {
    result.status = SessionCreateStatus::kSchemaUnavailable;
    result.diagnostic = "schema_open refused even though the compiled schema file exists";
    return result;
  }
  core.counts.schema_config_acquired += 1;
  const Bool closed = core.api->config_close(&config);
  if (closed == False) {
    result.status = SessionCreateStatus::kSchemaCloseFailed;
    result.diagnostic = "config_close refused after schema_open";
    return result;
  }
  core.counts.schema_config_closed += 1;

  core.counts.create_session_calls += 1;
  const RimeSessionId session_id = core.api->create_session();
  if (session_id == 0) {
    result.status = SessionCreateStatus::kCreateFailed;
    result.diagnostic = "create_session returned the invalid id 0";
    return result;
  }

  std::string failure_detail;
  auto destroy = [&]() {
    (void)DestroyEngineSession(core, session_id, &failure_detail);
  };

  if (core.api->select_schema(session_id, schema_id.c_str()) == False) {
    destroy();
    result.status = SessionCreateStatus::kSchemaSelectionFailed;
    result.diagnostic = "select_schema refused" + failure_detail;
    return result;
  }

  RIME_STRUCT(RimeStatus, status);
  const Bool got_status = core.api->get_status(session_id, &status);
  if (got_status == False) {
    destroy();
    result.status = SessionCreateStatus::kStatusFailed;
    result.diagnostic = "get_status refused after select_schema" + failure_detail;
    return result;
  }
  core.counts.status_acquired += 1;
  std::string reported_schema_id;
  bool schema_copy_ok = status.schema_id != nullptr;
  if (schema_copy_ok) {
    reported_schema_id = status.schema_id;
  }
  const Bool freed = core.api->free_status(&status);
  if (freed != False) {
    core.counts.status_freed += 1;
  }
  if (freed == False) {
    destroy();
    result.status = SessionCreateStatus::kStatusFailed;
    result.diagnostic = "free_status refused" + failure_detail;
    return result;
  }
  if (!schema_copy_ok) {
    destroy();
    result.status = SessionCreateStatus::kStatusFailed;
    result.diagnostic = "get_status returned a null schema id" + failure_detail;
    return result;
  }
  if (reported_schema_id != schema_id) {
    destroy();
    result.status = SessionCreateStatus::kSchemaMismatch;
    result.diagnostic = "the session reported a different schema id" + failure_detail;
    return result;
  }

  std::unique_ptr<RimeSession> session;
  try {
    session = CreateSessionObject(impl_->core, session_id, schema_id);
  } catch (const std::exception& ex) {
    destroy();
    result.status = SessionCreateStatus::kCreateFailed;
    result.diagnostic = std::string("session wrapper allocation failed: ") + ex.what() +
                       failure_detail;
    return result;
  } catch (...) {
    destroy();
    result.status = SessionCreateStatus::kCreateFailed;
    result.diagnostic = "session wrapper allocation failed" + failure_detail;
    return result;
  }

  const SessionStateResult initial = session->CaptureState();
  if (!initial.ok()) {
    const SessionCloseStatus close = session->Close();
    result.status = SessionCreateStatus::kInitialStateFailed;
    result.diagnostic = "initial CaptureState: " +
                        std::string(DescribeSessionStateStatus(initial.status)) + ": " +
                        initial.diagnostic;
    if (close != SessionCloseStatus::kOk) {
      result.diagnostic += "; Close: " + std::string(DescribeSessionCloseStatus(close));
    }
    return result;
  }

  result.status = SessionCreateStatus::kOk;
  result.session = std::move(session);
  return result;
}

const char* DescribeRuntimeStartStatus(RuntimeStartStatus status) {
  switch (status) {
    case RuntimeStartStatus::kOk:
      return "ok";
    case RuntimeStartStatus::kAlreadyStarted:
      return "already-started";
    case RuntimeStartStatus::kAnotherRuntimeActive:
      return "another-runtime-active";
    case RuntimeStartStatus::kInvalidOptions:
      return "invalid-options";
    case RuntimeStartStatus::kDllLoadFailed:
      return "dll-load-failed";
    case RuntimeStartStatus::kDllIdentityMismatch:
      return "dll-identity-mismatch";
    case RuntimeStartStatus::kApiSymbolMissing:
      return "api-symbol-missing";
    case RuntimeStartStatus::kApiTableIncomplete:
      return "api-table-incomplete";
    case RuntimeStartStatus::kSetupFailed:
      return "setup-failed";
    case RuntimeStartStatus::kInitializeFailed:
      return "initialize-failed";
    case RuntimeStartStatus::kBaseModuleMissing:
      return "base-module-missing";
    case RuntimeStartStatus::kEnglishModuleMissing:
      return "english-module-missing";
    case RuntimeStartStatus::kEnglishModuleIncompatible:
      return "english-module-incompatible";
    case RuntimeStartStatus::kDeploymentApiMissing:
      return "deployment-api-missing";
    case RuntimeStartStatus::kDeploymentFailed:
      return "deployment-failed";
  }
  return "unknown-status";
}

const char* DescribeRuntimeShutdownStatus(RuntimeShutdownStatus status) {
  switch (status) {
    case RuntimeShutdownStatus::kOk:
      return "ok";
    case RuntimeShutdownStatus::kNotStarted:
      return "not-started";
    case RuntimeShutdownStatus::kWrongThread:
      return "wrong-thread";
    case RuntimeShutdownStatus::kLiveSessionsRemain:
      return "live-sessions-remain";
    case RuntimeShutdownStatus::kFinalizeFailed:
      return "finalize-failed";
    case RuntimeShutdownStatus::kUnloadFailed:
      return "unload-failed";
    case RuntimeShutdownStatus::kAlreadyShutdown:
      return "already-shutdown";
  }
  return "unknown-status";
}

const char* DescribeSessionCreateStatus(SessionCreateStatus status) {
  switch (status) {
    case SessionCreateStatus::kOk:
      return "ok";
    case SessionCreateStatus::kNotStarted:
      return "not-started";
    case SessionCreateStatus::kWrongThread:
      return "wrong-thread";
    case SessionCreateStatus::kInvalidSchemaId:
      return "invalid-schema-id";
    case SessionCreateStatus::kSchemaUnavailable:
      return "schema-unavailable";
    case SessionCreateStatus::kCreateFailed:
      return "create-failed";
    case SessionCreateStatus::kSchemaSelectionFailed:
      return "schema-selection-failed";
    case SessionCreateStatus::kSchemaMismatch:
      return "schema-mismatch";
    case SessionCreateStatus::kStatusFailed:
      return "status-failed";
    case SessionCreateStatus::kSchemaCloseFailed:
      return "schema-close-failed";
    case SessionCreateStatus::kInitialStateFailed:
      return "initial-state-failed";
  }
  return "unknown-status";
}

}  // namespace engine
}  // namespace windows
}  // namespace epocanva
