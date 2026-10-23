// EpoCanva P3.4 -- internal state shared by RimeRuntime and RimeSession. Not a public header:
// nothing under tests/ or another platform layer should include it.
#ifndef EPOCANVA_WINDOWS_ENGINE_BRIDGE_RIME_RUNTIME_INTERNAL_H_
#define EPOCANVA_WINDOWS_ENGINE_BRIDGE_RIME_RUNTIME_INTERNAL_H_

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "rime_api.h"
#include "rime_runtime.h"
#include "rime_session.h"

namespace epocanva {
namespace windows {
namespace engine {

// The handle, API table and counters all sessions borrow. The concrete RimeApi table lives
// inside rime.dll and is valid only between initialize and finalize; `initialized` is the
// gate every session method checks before touching it.
struct RuntimeCore {
  std::thread::id owner_thread;
  bool started = false;      // owner_thread is meaningful and the process-global slot is held
  bool initialized = false;  // setup/initialize succeeded and finalize has not run
  HMODULE module = nullptr;
  RimeApi* api = nullptr;

  std::wstring requested_dll_path;
  std::wstring loaded_dll_path;
  std::string shared_data_dir_utf8;
  std::string user_data_dir_utf8;
  std::string log_dir_utf8;

  RimeTraits traits{};               // lives as long as the strings above
  std::vector<const char*> modules;  // NULL-terminated view into traits.modules

  std::uint64_t initialize_calls = 0;
  std::uint64_t finalize_calls = 0;
  std::uint64_t deploy_calls = 0;
  int live_sessions = 0;

  bool api_complete = false;
  bool base_modules_registered = false;
  bool english_module_loaded = false;
  std::uint64_t english_module_initialized = 0;
  std::string librime_version;

  ApiCallCounts counts;

  bool IsOwnerThread() const {
    return started && std::this_thread::get_id() == owner_thread;
  }
};

// Defined in rime_session.cc. The declaration is friended by RimeSession so the runtime can
// manufacture one without exposing Impl in a public header.
std::unique_ptr<RimeSession> CreateSessionObject(std::shared_ptr<RuntimeCore> core,
                                                 std::uintptr_t session_id,
                                                 std::string schema_id);

}  // namespace engine
}  // namespace windows
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_ENGINE_BRIDGE_RIME_RUNTIME_INTERNAL_H_
