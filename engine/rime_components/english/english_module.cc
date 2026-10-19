// EpoCanva P2.1 -- module registration for the compiled-in English component.
//
// Uses librime's own mechanism at the pinned commit, with nothing added to it:
//
//  * RIME_REGISTER_CUSTOM_MODULE(epocanva_english) expands to a .CRT$XCU static
//    initializer that calls RimeRegisterModule(&module) when rime.dll is loaded
//    (third_party/librime/src/rime_api.h:528-533, 557-571);
//  * initialize() puts the Translator class into rime::Registry, the same idiom
//    the gears module uses for its processors and translators
//    (src/rime/gear/gears_module.cc:43-68);
//  * the module is LOADED when a front end names it in RimeTraits.modules --
//    RimeInitialize passes that list straight to LoadModules
//    (src/rime_api_impl.h:51-54; src/rime/setup.cc:33-40). Upstream's
//    kDefaultModules is not touched (AGENTS.md section 7 forbids the plugin
//    directory that would require), so a front end that forgets the name gets a
//    schema that lists a translator class nobody registered. Registry::Find
//    returns null, CreateComponentsFromList logs and skips it
//    (src/rime/engine.cc:310-315), and the schema still composes -- which is why
//    the integration test asserts the module and the load counters, not just the
//    candidates.
//  * get_api() publishes the diagnostics table through the slot the levers module
//    uses for its own custom API (src/rime/lever/levers_module.cc:35-40,
//    levers_api_impl.h:242-280). No new exported symbol is added anywhere here.
#include <rime_api.h>

#include <rime/common.h>
#include <rime/component.h>
#include <rime/registry.h>

#include "english_diagnostics.h"
#include "english_space_processor.h"
#include "english_translator.h"

static void rime_epocanva_english_initialize() {
  epocanva::english::NoteModuleInitialized();
  // The Component object is owned by the Registry from here on; RimeFinalize()
  // reaches Registry::Clear(), which deletes it (src/rime/registry.cc:31-37), and
  // a later initialize() re-registers. Both `new` and `delete` happen inside
  // rime.dll, which is the arrangement ADR-0004 exists to keep.
  rime::Registry::instance().Register(
      EPOCANVA_ENGLISH_TRANSLATOR_NAME,
      new rime::Component<epocanva::english::EpoCanvaEnglishTranslator>);
  // P2.5: the space/commit boundary Processor, registered the same way and in the
  // same registry -- a Processor is looked up under "engine/processors" by
  // CreateComponentsFromList (src/rime/engine.cc:297-317) exactly as the Translator
  // is under "engine/translators", and upstream registers its own processors in
  // this identical form (src/rime/gear/gears_module.cc:43-68). Registering a class
  // no schema lists costs nothing and changes nothing: nothing instantiates it.
  rime::Registry::instance().Register(
      EPOCANVA_ENGLISH_SPACE_PROCESSOR_NAME,
      new rime::Component<epocanva::english::EpoCanvaEnglishSpaceProcessor>);
}

static void rime_epocanva_english_finalize() {
  // Only the witness counter moves here. The registration itself is undone by
  // Registry::Clear(); repeating that would double-delete.
  epocanva::english::NoteModuleFinalized();
}

static Bool rime_epocanva_english_get_counters(EpoCanvaEnglishCounters* out) {
  return epocanva::english::CopyEnglishCounters(out);
}

static const char* rime_epocanva_english_last_load_status_name() {
  return epocanva::english::LastLoadStatusName();
}

static const char* rime_epocanva_english_build_id() {
  return epocanva::english::EnglishBuildId();
}

static RimeCustomApi* rime_epocanva_english_get_api() {
  static EpoCanvaEnglishApi s_api = {0};
  if (!s_api.data_size) {
    RIME_STRUCT_INIT(EpoCanvaEnglishApi, s_api);
    s_api.get_counters = rime_epocanva_english_get_counters;
    s_api.last_load_status_name = rime_epocanva_english_last_load_status_name;
    s_api.build_id = rime_epocanva_english_build_id;
  }
  // Cast to the base type the engine hands back, as levers_api_impl.h:279 does
  // for its own table. Nothing is allocated per call and the pointer addresses
  // static storage inside rime.dll.
  return (RimeCustomApi*)&s_api;
}

RIME_REGISTER_CUSTOM_MODULE(epocanva_english) {
  module->get_api = rime_epocanva_english_get_api;
}
