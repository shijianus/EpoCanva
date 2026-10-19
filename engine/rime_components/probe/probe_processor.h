// EpoCanva P1.4 -- the probe Processor class, and the internal (C++) side of the
// diagnostics surface that probe_module.cc publishes through the module's
// get_api() slot.
//
// INTERNAL TO THE ENGINE BUILD. Nothing outside rime.dll includes this file: a
// caller outside the engine uses probe_diagnostics.h only. The C++ declarations
// here depend on librime's private headers at the pinned commit and are not a
// stable SDK (docs/adr/0004-compiled-in-rime-components.md, "限制" 1).
#ifndef EPOCANVA_PROBE_PROCESSOR_H_
#define EPOCANVA_PROBE_PROCESSOR_H_

#include <rime/key_event.h>
#include <rime/processor.h>
#include <rime/ticket.h>

#include "probe_diagnostics.h"

namespace epocanva {
namespace probe {

// Reads the fixed test key, consumes it, and lets everything else continue along
// the original processor chain. It deliberately reads no configuration, opens no
// resource and touches no Context: the point of this class is to be an
// unambiguous witness that a project-owned component inside rime.dll received a
// key event.
class EpoCanvaProbeProcessor : public rime::Processor {
 public:
  explicit EpoCanvaProbeProcessor(const rime::Ticket& ticket);
  ~EpoCanvaProbeProcessor() override;

  rime::ProcessResult ProcessKeyEvent(const rime::KeyEvent& key_event) override;

  EpoCanvaProbeProcessor(const EpoCanvaProbeProcessor&) = delete;
  EpoCanvaProbeProcessor& operator=(const EpoCanvaProbeProcessor&) = delete;
};

// --- diagnostics storage, defined in probe_processor.cc -----------------------
// Called by the module's initialize()/finalize() (probe_module.cc).
void NoteModuleInitialized();
void NoteModuleFinalized();

// Implement the three function pointers of EpoCanvaProbeApi. CopyCounters and
// ResetCountersWhenIdle report success with librime's own Bool (rime_api.h:44-46
// defines the macro `Bool` as `int`), and neither allocates nor frees.
Bool CopyCounters(EpoCanvaProbeCounters* out);
Bool ResetCountersWhenIdle();
const char* ProbeBuildId();

}  // namespace probe
}  // namespace epocanva

#endif  // EPOCANVA_PROBE_PROCESSOR_H_
