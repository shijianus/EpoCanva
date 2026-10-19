// EpoCanva P1.4 -- module registration for the compiled-in probe component.
//
// Uses librime's own mechanism, at the pinned commit, with nothing added to it:
//
//  * RIME_REGISTER_CUSTOM_MODULE(epocanva_probe) expands to a .CRT$XCU static
//    initializer that calls RimeRegisterModule(&module) when rime.dll is loaded
//    (third_party/librime/src/rime_api.h:528-533, 557-571);
//  * the module's initialize() registers the processor class in rime::Registry,
//    exactly the idiom the gears and levers modules use
//    (src/rime/gear/gears_module.cc:44-57, src/rime/lever/levers_module.cc:15-31);
//  * the module is *loaded* (i.e. initialize() is called) when a front end names
//    it in RimeTraits.modules -- RimeInitialize passes that list to LoadModules
//    (src/rime_api_impl.h:51-54; src/rime/setup.cc:33-40). Upstream's
//    kDefaultModules is not touched, because putting a name there requires the
//    plugin to be a directory inside third_party/librime/plugins/
//    (plugins/CMakeLists.txt:24-47, sample/CMakeLists.txt:36), which would make
//    the upstream working tree dirty.
//  * get_api() publishes the diagnostics table through the same slot the levers
//    module uses for its own custom API
//    (src/rime/lever/levers_module.cc:35-40, levers_api_impl.h:242-280). No new
//    exported symbol is introduced anywhere in this directory.
#include <rime_api.h>

#include <rime/common.h>
#include <rime/component.h>
#include <rime/registry.h>

#include "probe_diagnostics.h"
#include "probe_processor.h"

static void rime_epocanva_probe_initialize() {
  epocanva::probe::NoteModuleInitialized();
  // The Component object is owned by the Registry from here on; RimeFinalize()
  // reaches Registry::Clear(), which deletes it (src/rime/registry.cc:31-37), and
  // a later initialize() re-registers. `new` happens inside rime.dll, and so does
  // the delete -- which is the whole point of the compiled-in arrangement.
  rime::Registry::instance().Register(
      EPOCANVA_PROBE_PROCESSOR_NAME,
      new rime::Component<epocanva::probe::EpoCanvaProbeProcessor>);
}

static void rime_epocanva_probe_finalize() {
  // Only the witness counter moves here. The registration is undone by
  // Registry::Clear(); repeating it would double-delete.
  epocanva::probe::NoteModuleFinalized();
}

static Bool rime_epocanva_probe_get_counters(EpoCanvaProbeCounters* out) {
  return epocanva::probe::CopyCounters(out);
}

static Bool rime_epocanva_probe_reset_counters_when_idle() {
  return epocanva::probe::ResetCountersWhenIdle();
}

static const char* rime_epocanva_probe_build_id() {
  return epocanva::probe::ProbeBuildId();
}

static RimeCustomApi* rime_epocanva_probe_get_api() {
  static EpoCanvaProbeApi s_api = {0};
  if (!s_api.data_size) {
    RIME_STRUCT_INIT(EpoCanvaProbeApi, s_api);
    s_api.get_counters = rime_epocanva_probe_get_counters;
    s_api.reset_counters_when_idle = rime_epocanva_probe_reset_counters_when_idle;
    s_api.build_id = rime_epocanva_probe_build_id;
  }
  // Cast to the base type the engine hands back, exactly as
  // levers_api_impl.h:279 does for its own table. Nothing is allocated per call
  // and the pointer is static storage inside rime.dll.
  return (RimeCustomApi*)&s_api;
}

RIME_REGISTER_CUSTOM_MODULE(epocanva_probe) {
  module->get_api = rime_epocanva_probe_get_api;
}
