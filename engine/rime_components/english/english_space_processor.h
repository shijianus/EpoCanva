// EpoCanva P2.5 -- the space/commit boundary Processor, compiled into rime.dll.
//
// ROLE: an adapter and nothing more. It reads the live engine state for one key
// event, asks engine/languages/en/english_space_policy.h what the space bar means
// in that state, performs the two actions that answer selects, and records counts.
// Every rule -- which modifiers block a space, which state is case A versus B,
// whether a case commits text at all -- lives in the policy header, so that the
// rules can be unit-tested without an engine and so this file cannot quietly grow
// a policy of its own.
//
// It deliberately has NO DATA MEMBERS. No copied composition, no previous word, no
// "already committed" flag. The P2.5 brief forbids patching duplicate commits by
// remembering the last input, and the state needed to decide is already in the
// Context: IsComposing() and the segment statuses. Keeping nothing is also what
// makes session isolation a structural property rather than a tested hope.
//
// NO LOGGING of any kind (AGENTS.md section 2): this function sees the user's raw
// composition on every space press. A single LOG() here would put composing text
// in a log file. Only aggregate event counts leave this translation unit, through
// the C surface in english_diagnostics.h.
//
// LIFETIME: constructed by the engine's own component list
// (src/rime/engine.cc:297-317, CreateComponentsFromList) when the schema names this
// class in engine/processors, and destroyed with the engine. The `new` and `delete`
// both happen inside rime.dll, which is the arrangement
// docs/adr/0004-compiled-in-rime-components.md exists to keep.
#ifndef EPOCANVA_ENGLISH_SPACE_PROCESSOR_H_
#define EPOCANVA_ENGLISH_SPACE_PROCESSOR_H_

#include <rime/processor.h>

#include "english_diagnostics.h"

namespace epocanva {
namespace english {

class EpoCanvaEnglishSpaceProcessor : public rime::Processor {
 public:
  explicit EpoCanvaEnglishSpaceProcessor(const rime::Ticket& ticket);
  ~EpoCanvaEnglishSpaceProcessor() override;

  EpoCanvaEnglishSpaceProcessor(const EpoCanvaEnglishSpaceProcessor&) = delete;
  EpoCanvaEnglishSpaceProcessor& operator=(const EpoCanvaEnglishSpaceProcessor&) =
      delete;

  rime::ProcessResult ProcessKeyEvent(const rime::KeyEvent& key_event) override;
};

// --- diagnostics storage, defined in english_space_processor.cc -----------------
// The value type the C struct is filled from. Internal to the engine build; the
// only caller is CopyEnglishCounters() in english_translator.cc, which keeps the
// published C ABI a single struct.
struct EnglishSpaceCounters {
  unsigned long long constructed = 0;
  unsigned long long destroyed = 0;
  unsigned long long live = 0;
  unsigned long long key_events = 0;
  unsigned long long space_seen = 0;
  unsigned long long space_refused = 0;
  unsigned long long case_a = 0;
  unsigned long long case_b = 0;
  unsigned long long case_c = 0;
  unsigned long long composition_commits = 0;
  unsigned long long space_characters = 0;
};

EnglishSpaceCounters SnapshotSpaceCounters();

}  // namespace english
}  // namespace epocanva

#endif  // EPOCANVA_ENGLISH_SPACE_PROCESSOR_H_
