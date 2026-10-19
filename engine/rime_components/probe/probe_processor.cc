// EpoCanva P1.4 -- probe Processor implementation and the counter storage behind
// the module's diagnostics surface.
//
// Compiled INTO rime.dll (see tools/cmake/epocanva_rime_compiled_in_components.cmake),
// so the object this constructor `new`s and the object the engine's an<> holder
// deletes live in the same PE module and the same CRT heap. That is the property
// docs/adr/0004-compiled-in-rime-components.md exists to keep.
//
// No logging: this translation unit emits no LOG()/DLOG() of any kind, so there
// is no chance of an input string reaching a log sink. Counters are aggregate
// counts only (AGENTS.md section 2).
#include "probe_processor.h"

#include <atomic>

#include <rime_api.h>

namespace epocanva {
namespace probe {
namespace {

// Payload size used for the caller's struct-versioning check: everything after
// data_size, matching RIME_STRUCT_INIT's own definition
// (third_party/librime/src/rime_api.h:60-61).
constexpr unsigned long long kProbePayloadSize =
    sizeof(EpoCanvaProbeCounters) - sizeof(int);

// Relaxed atomics: the values are counters, not a protocol, and a future
// front-end may own engines on more than one thread. Keeping them atomic costs
// nothing here and removes a data-race class from the witness itself.
std::atomic<unsigned long long> g_module_initialized{0};
std::atomic<unsigned long long> g_module_finalized{0};
std::atomic<unsigned long long> g_component_constructed{0};
std::atomic<unsigned long long> g_component_destroyed{0};
std::atomic<unsigned long long> g_live_instances{0};
std::atomic<unsigned long long> g_process_key_calls{0};
std::atomic<unsigned long long> g_keys_passed_through{0};
std::atomic<unsigned long long> g_test_keys_consumed{0};

}  // namespace

EpoCanvaProbeProcessor::EpoCanvaProbeProcessor(const rime::Ticket& ticket)
    : rime::Processor(ticket) {
  g_component_constructed.fetch_add(1, std::memory_order_relaxed);
  g_live_instances.fetch_add(1, std::memory_order_relaxed);
}

EpoCanvaProbeProcessor::~EpoCanvaProbeProcessor() {
  // Runs when the last an<Processor> in the engine's processors_ vector goes
  // away, i.e. when the engine (and with it the session) is released.
  g_live_instances.fetch_sub(1, std::memory_order_relaxed);
  g_component_destroyed.fetch_add(1, std::memory_order_relaxed);
}

rime::ProcessResult EpoCanvaProbeProcessor::ProcessKeyEvent(
    const rime::KeyEvent& key_event) {
  g_process_key_calls.fetch_add(1, std::memory_order_relaxed);
  if (key_event.keycode() == EPOCANVA_PROBE_TEST_KEYCODE &&
      key_event.modifier() == 0) {
    // Consume the fixture's test key. Nothing else happens: no Context method is
    // called, so the composition keeps whatever the previous processor left and
    // no commit text is produced. ConcreteEngine::ProcessKey returns true for
    // kAccepted (src/rime/engine.cc:99-104), which is what makes the consumption
    // observable through RimeApi::process_key().
    g_test_keys_consumed.fetch_add(1, std::memory_order_relaxed);
    return rime::kAccepted;
  }
  // Everything else is released unchanged to the next processor in the chain.
  g_keys_passed_through.fetch_add(1, std::memory_order_relaxed);
  return rime::kNoop;
}

// --- diagnostics storage ------------------------------------------------------

void NoteModuleInitialized() {
  g_module_initialized.fetch_add(1, std::memory_order_relaxed);
}

void NoteModuleFinalized() {
  g_module_finalized.fetch_add(1, std::memory_order_relaxed);
}

Bool CopyCounters(EpoCanvaProbeCounters* out) {
  if (!out) {
    return False;
  }
  if (out->data_size < static_cast<int>(kProbePayloadSize)) {
    // The caller's struct cannot hold every field this build writes; refuse
    // rather than overrun a buffer that belongs to the caller.
    return False;
  }
  out->module_initialized = g_module_initialized.load(std::memory_order_relaxed);
  out->module_finalized = g_module_finalized.load(std::memory_order_relaxed);
  out->component_constructed =
      g_component_constructed.load(std::memory_order_relaxed);
  out->component_destroyed =
      g_component_destroyed.load(std::memory_order_relaxed);
  out->live_instances = g_live_instances.load(std::memory_order_relaxed);
  out->process_key_calls = g_process_key_calls.load(std::memory_order_relaxed);
  out->keys_passed_through = g_keys_passed_through.load(std::memory_order_relaxed);
  out->test_keys_consumed = g_test_keys_consumed.load(std::memory_order_relaxed);
  return True;
}

Bool ResetCountersWhenIdle() {
  // Refuse while any instance is alive: resetting mid-run would destroy the
  // evidence a caller is in the middle of collecting, and would make the
  // instance counters uninterpretable.
  if (g_live_instances.load(std::memory_order_relaxed) != 0) {
    return False;
  }
  g_component_constructed.store(0, std::memory_order_relaxed);
  g_component_destroyed.store(0, std::memory_order_relaxed);
  g_live_instances.store(0, std::memory_order_relaxed);
  g_process_key_calls.store(0, std::memory_order_relaxed);
  g_keys_passed_through.store(0, std::memory_order_relaxed);
  g_test_keys_consumed.store(0, std::memory_order_relaxed);
  // module_initialized / module_finalized are never reset: they record that this
  // DLL's module ran, which is the fact a reachability test is about.
  return True;
}

const char* ProbeBuildId() {
  return "epocanva-probe/0.0.0-p1.4";
}

}  // namespace probe
}  // namespace epocanva
