// EpoCanva P3.4 -- runtime implementation. See rime_runtime.h for the contract.
#include "rime_runtime.h"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "english_diagnostics.h"
#include "rime_api.h"
#include "rime_runtime_internal.h"

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

std::wstring NormalizePath(const std::filesystem::path& path) {
  const std::wstring raw = path.native();
  if (raw.empty()) {
    return std::wstring();
  }
  std::vector<wchar_t> buffer(1024, L'\0');
  DWORD written = ::GetFullPathNameW(raw.c_str(), static_cast<DWORD>(buffer.size()),
                                     buffer.data(), nullptr);
  if (written == 0) {
    return std::wstring();
  }
  if (written >= buffer.size()) {
    buffer.assign(static_cast<std::size_t>(written) + 1, L'\0');
    written = ::GetFullPathNameW(raw.c_str(), static_cast<DWORD>(buffer.size()),
                                 buffer.data(), nullptr);
    if (written == 0 || written >= buffer.size()) {
      return std::wstring();
    }
  }
  return std::wstring(buffer.data(), written);
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

bool IsAsciiPath(const std::filesystem::path& path) {
  const std::wstring& native = path.native();
  for (const wchar_t ch : native) {
    if (ch < 0 || ch > 0x7F) {
      return false;
    }
  }
  return true;
}

std::string NarrowAsciiPath(const std::filesystem::path& path) {
  const std::wstring& native = path.native();
  std::string out;
  out.reserve(native.size());
  for (const wchar_t ch : native) {
    out.push_back(static_cast<char>(ch));
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

bool ValidateDirectory(const std::filesystem::path& path, const char* name,
                       std::string* why) {
  if (!path.is_absolute()) {
    *why = std::string(name) + " must be absolute";
    return false;
  }
  if (!IsAsciiPath(path)) {
    *why = std::string(name) + " must be ASCII: librime's C traits take narrow paths and "
                               "this bridge will not guess an encoding";
    return false;
  }
  std::error_code ec;
  if (!std::filesystem::is_directory(path, ec) || ec) {
    *why = std::string(name) + " is not an existing directory";
    return false;
  }
  return true;
}

bool ValidateOptions(const RuntimeOptions& options, std::string* why) {
  if (!options.rime_dll_path.is_absolute()) {
    *why = "rime_dll_path must be absolute; the working directory is never searched";
    return false;
  }
  if (!ValidateDirectory(options.shared_data_dir, "shared_data_dir", why) ||
      !ValidateDirectory(options.user_data_dir, "user_data_dir", why) ||
      !ValidateDirectory(options.log_dir, "log_dir", why)) {
    return false;
  }
  if (!options.deployment.has_value()) {
    return true;
  }
  const DeploymentOptions& deployment = *options.deployment;
  if (deployment.schema_files.empty()) {
    *why = "deployment.schema_files is empty";
    return false;
  }
  const std::filesystem::path config_name(deployment.config_file_name);
  if (deployment.config_file_name.empty() ||
      config_name.filename() != config_name || deployment.config_file_name == "." ||
      deployment.config_file_name == "..") {
    *why = "deployment.config_file_name must be a bare file name";
    return false;
  }
  std::error_code ec;
  const std::filesystem::path shared =
      std::filesystem::weakly_canonical(options.shared_data_dir, ec);
  if (ec) {
    *why = "shared_data_dir cannot be canonicalized";
    return false;
  }
  for (const std::filesystem::path& schema : deployment.schema_files) {
    if (!schema.is_absolute()) {
      *why = "every deployment schema path must be absolute";
      return false;
    }
    if (!IsAsciiPath(schema)) {
      *why = "every deployment schema path must be ASCII";
      return false;
    }
    if (!std::filesystem::is_regular_file(schema, ec) || ec) {
      *why = "a deployment schema path is not an existing regular file";
      return false;
    }
    const std::filesystem::path parent =
        std::filesystem::weakly_canonical(schema.parent_path(), ec);
    if (ec || !SamePath(parent.native(), shared.native())) {
      *why = "a deployment schema is not directly inside shared_data_dir";
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
  EPOCANVA_SLOT(select_candidate);
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
  bool start_attempted = false;
  bool initialize_entered = false;
  bool shutdown_done = false;
  std::string last_start_diagnostic;
  std::string last_shutdown_diagnostic;
};

RimeRuntime::RimeRuntime() : impl_(new Impl) {}

RimeRuntime::~RimeRuntime() {
  if (!impl_ || !impl_->start_attempted || impl_->shutdown_done) {
    return;
  }
  // Only the owner thread may finalize. On any other thread the DLL is deliberately left
  // loaded instead of being finalized and unloaded underneath sessions this destructor cannot
  // safely touch. That is a controlled leak, not a hidden success.
  if (impl_->core->IsOwnerThread()) {
    (void)Shutdown();
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
  if (!ValidateOptions(options, &why)) {
    impl.last_start_diagnostic = why;
    impl.shutdown_done = true;
    return RuntimeStartStatus::kInvalidOptions;
  }

  bool expected = false;
  if (!g_runtime_slot_held.compare_exchange_strong(expected, true)) {
    impl.last_start_diagnostic =
        "another RimeRuntime in this process already holds the librime service";
    impl.shutdown_done = true;
    return RuntimeStartStatus::kAnotherRuntimeActive;
  }

  core.started = true;
  core.owner_thread = std::this_thread::get_id();
  core.requested_dll_path = NormalizePath(options.rime_dll_path);
  if (core.requested_dll_path.empty()) {
    impl.last_start_diagnostic = "rime_dll_path could not be normalized";
    core.started = false;
    impl.shutdown_done = true;
    g_runtime_slot_held.store(false);
    return RuntimeStartStatus::kInvalidOptions;
  }
  core.shared_data_dir_utf8 = NarrowAsciiPath(options.shared_data_dir);
  core.user_data_dir_utf8 = NarrowAsciiPath(options.user_data_dir);
  core.log_dir_utf8 = NarrowAsciiPath(options.log_dir);

  auto fail = [&](RuntimeStartStatus status, std::string diagnostic) {
    if (impl.initialize_entered) {
      bool finalized = false;
      try {
        core.api->finalize();
        core.finalize_calls += 1;
        finalized = true;
      } catch (...) {
        diagnostic += "; finalize() threw, the DLL was left loaded";
      }
      core.initialized = false;
      core.english_module_loaded = false;
      if (finalized && core.module != nullptr) {
        ::FreeLibrary(core.module);
        core.module = nullptr;
      }
    } else if (core.module != nullptr) {
      ::FreeLibrary(core.module);
      core.module = nullptr;
    }
    core.api = nullptr;
    core.started = false;
    core.owner_thread = std::thread::id();
    core.librime_version.clear();
    impl.last_start_diagnostic = std::move(diagnostic);
    impl.shutdown_done = true;
    g_runtime_slot_held.store(false);
    return status;
  };

  const DWORD load_flags =
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;
  HMODULE module =
      ::LoadLibraryExW(core.requested_dll_path.c_str(), nullptr, load_flags);
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
  if (!SamePath(core.loaded_dll_path, core.requested_dll_path)) {
    return fail(RuntimeStartStatus::kDllIdentityMismatch,
                "the loaded module's path is not the requested path");
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
  if (options.deployment.has_value()) {
    core.modules.push_back("levers");
  }
  core.modules.push_back(EPOCANVA_ENGLISH_MODULE_NAME);
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
  module_status = CheckEnglishModule(core, &why);
  if (module_status != RuntimeStartStatus::kOk) {
    return fail(module_status, why);
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
    for (const std::filesystem::path& schema : options.deployment->schema_files) {
      const std::string schema_utf8 = NarrowAsciiPath(schema);
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
    try {
      core.api->finalize();
      core.finalize_calls += 1;
    } catch (const std::exception& ex) {
      core.initialized = false;
      impl.last_shutdown_diagnostic =
          std::string("RimeApi::finalize threw: ") + ex.what();
      return RuntimeShutdownStatus::kFinalizeFailed;
    } catch (...) {
      core.initialized = false;
      impl.last_shutdown_diagnostic = "RimeApi::finalize threw";
      return RuntimeShutdownStatus::kFinalizeFailed;
    }
  }
  core.initialized = false;
  core.english_module_loaded = false;

  if (core.module != nullptr) {
    if (::FreeLibrary(core.module) == FALSE) {
      impl.last_shutdown_diagnostic =
          LastErrorText("FreeLibrary", ::GetLastError());
      core.module = nullptr;
      core.api = nullptr;
      core.started = false;
      core.owner_thread = std::thread::id();
      impl.shutdown_done = true;
      g_runtime_slot_held.store(false);
      return RuntimeShutdownStatus::kUnloadFailed;
    }
    core.module = nullptr;
  }
  core.api = nullptr;
  core.started = false;
  core.owner_thread = std::thread::id();
  impl.shutdown_done = true;
  impl.last_shutdown_diagnostic.clear();
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
  info.shared_data_dir = std::filesystem::path(core.shared_data_dir_utf8).wstring();
  info.user_data_dir = std::filesystem::path(core.user_data_dir_utf8).wstring();
  info.log_dir = std::filesystem::path(core.log_dir_utf8).wstring();
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
  // (src/rime/schema.cc:40-42), and ConfigFileUpdate writes it into staging_dir =
  // <user>/build (src/rime/setup.cc:62-65).
  std::error_code ec;
  const std::filesystem::path compiled_schema =
      std::filesystem::path(core.user_data_dir_utf8) / "build" /
      (schema_id + ".schema.yaml");
  if (!std::filesystem::is_regular_file(compiled_schema, ec) || ec) {
    result.status = SessionCreateStatus::kSchemaUnavailable;
    result.diagnostic = "the prepared user directory/build has no compiled schema for this id";
    return result;
  }

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
