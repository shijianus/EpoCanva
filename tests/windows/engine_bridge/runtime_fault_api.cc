// Test-only RimeApi DLL for runtime lifecycle faults. No sessions or input are implemented.
#include <cstring>
#include <stdexcept>

#define RIME_EXPORTS
#include "english_diagnostics.h"
#include "rime_api.h"

namespace {
int mode = 0;
int initialize_calls = 0;
int finalize_calls = 0;
int unexpected_calls = 0;

void Initialize(RimeTraits*) {
  ++initialize_calls;
  if (mode == 1 || mode == 2) {
    throw std::runtime_error("injected initialize failure");
  }
}

void Finalize() {
  ++finalize_calls;
  if (mode == 2 || mode == 3 || mode == 4) {
    throw std::runtime_error("injected finalize failure");
  }
}

Bool Counters(EpoCanvaEnglishCounters* out) {
  out->module_initialized = 1;
  return True;
}

RimeCustomApi* EnglishApi() {
  static EpoCanvaEnglishApi api{};
  RIME_STRUCT_INIT(EpoCanvaEnglishApi, api);
  api.get_counters = &Counters;
  return reinterpret_cast<RimeCustomApi*>(&api);
}

RimeModule* FindModule(const char* name) {
  if (mode == 4 && std::strcmp(name, EPOCANVA_ENGLISH_MODULE_NAME) == 0) {
    return nullptr;
  }
  static RimeModule module{};
  RIME_STRUCT_INIT(RimeModule, module);
  module.get_api = &EnglishApi;
  return &module;
}

Bool Unexpected() {
  ++unexpected_calls;
  return False;
}
}  // namespace

extern "C" __declspec(dllexport) void RuntimeFaultConfigure(int value) {
  mode = value;
  initialize_calls = 0;
  finalize_calls = 0;
  unexpected_calls = 0;
}

extern "C" __declspec(dllexport) int RuntimeFaultCount(int counter) {
  return counter == 0 ? initialize_calls
                     : (counter == 1 ? finalize_calls : unexpected_calls);
}

extern "C" __declspec(dllexport) RimeApi* rime_get_api() {
  static RimeApi api{};
  RIME_STRUCT_INIT(RimeApi, api);
  api.setup = [](RimeTraits*) {};
  api.initialize = &Initialize;
  api.finalize = &Finalize;
  api.find_module = &FindModule;
  api.get_version = []() { return "runtime-fault-fixture"; };
  api.create_session = []() -> RimeSessionId { Unexpected(); return 0; };
  api.find_session = [](RimeSessionId) { return Unexpected(); };
  api.destroy_session = [](RimeSessionId) { return Unexpected(); };
  api.process_key = [](RimeSessionId, int, int) { return Unexpected(); };
  api.select_candidate_on_current_page = [](RimeSessionId, size_t) { return Unexpected(); };
  api.get_context = [](RimeSessionId, RimeContext*) { return Unexpected(); };
  api.free_context = [](RimeContext*) { return Unexpected(); };
  api.get_status = [](RimeSessionId, RimeStatus*) { return Unexpected(); };
  api.free_status = [](RimeStatus*) { return Unexpected(); };
  api.get_commit = [](RimeSessionId, RimeCommit*) { return Unexpected(); };
  api.free_commit = [](RimeCommit*) { return Unexpected(); };
  api.get_input = [](RimeSessionId) -> const char* { Unexpected(); return nullptr; };
  api.select_schema = [](RimeSessionId, const char*) { return Unexpected(); };
  api.schema_open = [](const char*, RimeConfig*) { return Unexpected(); };
  api.config_close = [](RimeConfig*) { return Unexpected(); };
  return &api;
}
