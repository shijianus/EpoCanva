// EpoCanva P3.4 -- the one controlled librime runtime a process may have.
//
// WHY A RUNTIME OBJECT AT ALL. librime's service, session map, deployer, registry and module
// manager are process-global singletons (src/rime/service.cc:196-203,
// src/rime/module.cc:48-52, src/rime/registry.cc:29-37). `RimeInitialize` starts that service
// and `RimeFinalize` stops it, clears the registry and unloads every module
// (src/rime_api_impl.h:51-63). Calling that per session would tear down other sessions'
// registry entries and could unload the DLL under a live session. This runtime therefore owns
// exactly one setup/initialize/finalize cycle for the whole process; sessions only borrow it.
//
// DLL IDENTITY. The caller gives an ABSOLUTE path to one `rime.dll`. It is loaded with
// LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32) so the
// process working directory and PATH are not searched. After loading, the module that owns
// rime_get_api() is resolved with GetModuleHandleExW(FROM_ADDRESS) and its full path is
// compared with the requested one. A different DLL that happens to export the same symbol is
// refused, and the actual path is kept in RuntimeInfo.
//
// NO PRIVATE REGISTRY ACCESS. Modules are named in RimeTraits.modules; librime's own
// LoadModules() calls their initialize() slots (src/rime/setup.cc:33-40). This code never
// calls rime::Registry from outside the DLL and never adds or replaces a component.
//
// DIRECTORIES. shared_data_dir, user_data_dir and log_dir are absolute and supplied by the
// caller. Nothing here reads %APPDATA%, the current directory or an existing Rime installation,
// and nothing downloads, scans for language packs or deploys data implicitly. RimeTraits
// carries UTF-8 strings and librime converts them with std::filesystem::u8path on Windows
// (third_party/librime/src/rime/common.h:84-96), so this bridge converts each native wide path
// to UTF-8 with WideCharToMultiByte instead of refusing non-ASCII directories.
//
// DEPLOYMENT IS NOT RUNTIME USE. A runtime may be asked, once during Start, to deploy named
// files that are already in shared_data_dir. This exists for test preparation and for an
// explicit installer step; it never happens on session creation or on a key event. The normal
// bridge starts with `deployment` unset and consumes an already-prepared user_data_dir.
//
// THREADING. Start records the owner thread. Every engine operation (create/destroy session,
// process_key, select_candidate, state/commit reads, Shutdown) refuses to run on any other
// thread. Shutdown with a live session returns kLiveSessionsRemain and does not finalize.
#ifndef EPOCANVA_WINDOWS_ENGINE_BRIDGE_RIME_RUNTIME_H_
#define EPOCANVA_WINDOWS_ENGINE_BRIDGE_RIME_RUNTIME_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rime_session.h"

namespace epocanva {
namespace windows {
namespace engine {

enum class RuntimeStartStatus {
  kOk = 0,
  kAlreadyStarted,   // this object's Start was already attempted
  kAnotherRuntimeActive,  // a process-global RimeInitialize is already live
  kInvalidOptions,   // relative path, missing directory, path not encodable as UTF-8, ...
  kDllLoadFailed,    // LoadLibraryExW failed; diagnostic carries GetLastError()
  kDllIdentityMismatch,  // the module that owns rime_get_api() is not the requested file
  kApiSymbolMissing,     // rime_get_api is not exported
  kApiTableIncomplete,   // a required RimeApi slot is absent
  kSetupFailed,
  kInitializeFailed,
  kBaseModuleMissing,    // core/dict/gears/default was not registered by this DLL
  kEnglishModuleMissing, // epocanva_english was not registered by this DLL
  kEnglishModuleIncompatible,  // registered, but its custom API is absent or a different shape
  kDeploymentApiMissing,
  kDeploymentFailed,
};

enum class RuntimeShutdownStatus {
  kOk = 0,
  kNotStarted,
  kWrongThread,
  kLiveSessionsRemain,
  kFinalizeFailed,
  kUnloadFailed,
  kAlreadyShutdown,
};

enum class SessionCreateStatus {
  kOk = 0,
  kNotStarted,
  kWrongThread,
  kInvalidSchemaId,  // empty or non-ASCII
  kSchemaUnavailable,  // schema_open refused: no compiled schema in the prepared directory
  kCreateFailed,        // create_session returned the invalid id
  kSchemaSelectionFailed,
  kSchemaMismatch,      // get_status did not report the requested schema
  kStatusFailed,
  kSchemaCloseFailed,
  kInitialStateFailed,
};

struct DeploymentOptions {
  // Absolute schema YAML paths, each directly inside RuntimeOptions::shared_data_dir. The
  // engine compiles id + ".schema.yaml" into user_data_dir/build; a path elsewhere is refused
  // so a caller cannot make this scan a directory.
  std::vector<std::filesystem::path> schema_files;
  std::string config_file_name = "default.yaml";
};

struct RuntimeOptions {
  std::filesystem::path rime_dll_path;   // absolute
  std::filesystem::path shared_data_dir; // absolute, existing directory
  std::filesystem::path user_data_dir;   // absolute, existing directory
  std::filesystem::path log_dir;         // absolute, existing directory
  std::optional<DeploymentOptions> deployment;
};

// API acquisition/release witnesses. Every `_acquired` counter moves only when a get_* call
// returned True and therefore handed this process memory that a free_* must release; every
// `_freed` counter moves only when the matching call returned True. `balanced()` is the
// pairing check, not a proof that no other allocation exists.
struct ApiCallCounts {
  std::uint64_t create_session_calls = 0;
  std::uint64_t destroy_session_calls = 0;
  std::uint64_t process_key_calls = 0;
  std::uint64_t select_candidate_calls = 0;
  std::uint64_t status_acquired = 0;
  std::uint64_t status_freed = 0;
  std::uint64_t context_acquired = 0;
  std::uint64_t context_freed = 0;
  std::uint64_t commit_acquired = 0;
  std::uint64_t commit_freed = 0;
  std::uint64_t schema_config_acquired = 0;
  std::uint64_t schema_config_closed = 0;
  std::uint64_t deploy_calls = 0;

  bool balanced() const {
    return status_acquired == status_freed &&
           context_acquired == context_freed &&
           commit_acquired == commit_freed &&
           schema_config_acquired == schema_config_closed &&
           create_session_calls == destroy_session_calls;
  }
};

struct RuntimeInfo {
  std::wstring requested_dll_path;
  std::wstring loaded_dll_path;
  std::wstring shared_data_dir;
  std::wstring user_data_dir;
  std::wstring log_dir;
  std::string librime_version;  // UTF-8 copy of RimeApi::get_version()
  bool initialized = false;
  bool api_complete = false;
  bool base_modules_registered = false;
  bool english_module_loaded = false;
  std::uint64_t english_module_initialized = 0;  // module lifecycle witness, not a session count
  std::uint64_t initialize_calls = 0;
  std::uint64_t finalize_calls = 0;
  std::uint64_t deploy_calls = 0;
  int live_session_count = 0;
  std::string last_start_diagnostic;     // text-free: API/stage/status only
  std::string last_shutdown_diagnostic;  // same
};

struct SessionCreateResult {
  SessionCreateStatus status = SessionCreateStatus::kNotStarted;
  std::unique_ptr<RimeSession> session;
  std::string diagnostic;
  bool ok() const { return status == SessionCreateStatus::kOk && session != nullptr; }
};

class RimeRuntime {
 public:
  RimeRuntime();
  ~RimeRuntime();

  RimeRuntime(const RimeRuntime&) = delete;
  RimeRuntime& operator=(const RimeRuntime&) = delete;

  RuntimeStartStatus Start(const RuntimeOptions& options);
  RuntimeShutdownStatus Shutdown();

  bool IsInitialized() const;
  bool IsOwnerThread() const;
  int live_session_count() const;

  // A copy; safe to call after Shutdown. It makes no engine call.
  RuntimeInfo Info() const;
  ApiCallCounts ApiCounts() const;

  // Owner-thread only. Creates the engine session, verifies that the named compiled schema
  // exists, selects it, confirms the reported schema id, and captures one initial read-only
  // state. Any failure destroys the engine session before returning.
  SessionCreateResult CreateSession(const std::string& schema_id);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

const char* DescribeRuntimeStartStatus(RuntimeStartStatus status);
const char* DescribeRuntimeShutdownStatus(RuntimeShutdownStatus status);
const char* DescribeSessionCreateStatus(SessionCreateStatus status);

}  // namespace engine
}  // namespace windows
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_ENGINE_BRIDGE_RIME_RUNTIME_H_
