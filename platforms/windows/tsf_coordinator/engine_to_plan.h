// EpoCanva P3.8 -- the pure mapping from one engine observation to one edit decision.
//
// WHAT THIS FILE IS. A side-effect-free function over two value inputs: the coordinator's
// local mirror of the bound document (what its writer last wrote and where the tracked
// caret is) and one copied engine observation (state + pending commit plus the coarse
// dispatch/selection facts). It returns a DECISION, not an EditPlan: the caller owns the
// ContextIdentity and builds the plan, so this function stays testable without a context,
// a session or TSF.
//
// UNITS (AGENTS.md section 5). `text_utf16` fields are UTF-16 code units. `caret_utf16` and
// `composition_start_utf16` are UTF-16 code-unit offsets in the document (ACP positions),
// never bytes and never caret columns.
//
// THE ONE UNSUPPORTED SHAPE. An observation that carries BOTH a pending commit AND a live
// preedit cannot be expressed as one Begin/Update/Commit/Cancel step: applying the commit
// and keeping the preedit needs an order this round does not define, and applying only one
// half would drop the other. The mapping returns kUnsupportedShape; the coordinator must
// block further normal input rather than guess (brief section 6). No such observation is
// produced by the pinned English engine today -- the guard exists so a future engine
// change fails loudly instead of corrupting text.
//
// WHAT IS DELIBERATELY NOT DECIDED HERE. Whether the key was "eaten" (pfEaten), candidate
// ordering, retry, resynchronisation and any document read-back. The function never sees a
// COM pointer, never reads the document, and its diagnostic strings are text-free.
#ifndef EPOCANVA_WINDOWS_TSF_COORDINATOR_ENGINE_TO_PLAN_H_
#define EPOCANVA_WINDOWS_TSF_COORDINATOR_ENGINE_TO_PLAN_H_

#include <windows.h>

#include <cstdint>
#include <string>

#include "engine_update.h"
#include "rime_session.h"

namespace epocanva {
namespace tsf {
namespace coordinator {

// The P3.4 engine value types this mapping reads, under one short name.
namespace engine = ::epocanva::windows::engine;

// The coordinator's mirror of the bound document. Maintained only from applied edits plus
// the binding-time caret; it is never read back from the document.
struct LocalDocumentView {
  bool composition_active = false;
  // UTF-16 code-unit offset of the composition this coordinator's writer started; -1 when
  // no composition has been started for the current binding.
  LONG composition_start_utf16 = -1;
  // The text the writer last wrote into that composition, in UTF-16 code units. Used to
  // recognise a redundant update (engine preedit == what is already displayed).
  std::u16string composition_text_utf16;
  // Tracked collapsed caret, UTF-16 code units from the document start.
  LONG caret_utf16 = 0;
};

// A normalised copy of one engine command result. Built by the coordinator from either a
// dispatch or a candidate-acceptance payload; it contains no borrowed engine memory.
struct EngineObservation {
  // The engine call was issued (process_key / select_candidate actually ran).
  bool engine_call_made = false;
  // The engine accepted/handled the call (its return value was True).
  bool engine_reported_handled = false;
  // The command ended without any engine state change to map: a policy refusal
  // (kNotDispatched, kDeferredCompositionBoundary), an out-of-range/not-ready candidate, or
  // an engine refusal (kDispatchedUnhandled, kRejectedByEngine).
  bool engine_refused = false;
  engine::SessionStateStatus state_status = engine::SessionStateStatus::kRuntimeNotInitialized;
  engine::SessionCommitStatus commit_status =
      engine::SessionCommitStatus::kRuntimeNotInitialized;
  engine::EngineState state;   // meaningful only when state_status == kOk
  engine::EngineCommit commit; // present == false means no commit was queued
};

enum class PlanDecisionKind : std::uint8_t {
  // The engine consumed the event and the document already reflects the result (or the
  // event changed nothing). No edit is submitted.
  kNoEdit = 0,
  // No composition exists locally and the engine now has a non-empty preedit.
  kBegin,
  // A local composition exists and the engine preedit differs from the displayed text.
  kUpdate,
  // The engine queued a commit (replaces and ends a local composition, or inserts once at
  // the tracked collapsed caret when no composition exists).
  kCommit,
  // The engine's composition ended with no commit; the local preedit must be removed.
  kCancel,
  // A commit and a live preedit arrived together; see the file comment.
  kUnsupportedShape,
  // The observation cannot be trusted (state/commit read failed, or a status this mapping
  // does not define). The caller must enter NeedsResync, not guess.
  kInvalidObservation,
};

const char* DescribePlanDecisionKind(PlanDecisionKind kind);

struct PlanDecision {
  PlanDecisionKind kind = PlanDecisionKind::kNoEdit;
  // UTF-16 code units; meaningful for kBegin/kUpdate/kCommit.
  std::u16string text_utf16;
  // The collapsed caret the writer must observe, UTF-16 code units; meaningful for kBegin
  // and for a kCommit that has no local composition. A kCommit over an existing
  // composition carries the tracked caret for build validation only.
  LONG caret_utf16 = 0;
  std::string diagnostic;  // status names and lengths only; never text
};

// The pure mapping. Rules, in order:
//   1. no engine call at all (not dispatched / deferred)       -> kNoEdit
//   2. a call ran, state_status != kOk / invalid, or commit
//      status not kNoCommit/kRead                              -> kInvalidObservation
//      (an unreadable observation of a dispatched call is never kNoEdit, even when the
//      engine reported the call unhandled)
//   3. a call ran, the observation is readable, and the engine
//      reported unhandled/refused                              -> kNoEdit
//   4. commit present AND engine still composing with preedit    -> kUnsupportedShape
//   5. commit present:
//        composition locally active                              -> kCommit(commit text)
//        no local composition, commit text empty                 -> kNoEdit (nothing to
//                                                                   insert; the no-preedit
//                                                                   commit path refuses it)
//        no local composition                                    -> kCommit at tracked caret
//   6. no commit, engine composing with non-empty preedit:
//        no local composition                                    -> kBegin at tracked caret
//        local text equals preedit                               -> kNoEdit (already shown)
//        otherwise                                               -> kUpdate(preedit)
//   7. no commit, engine not composing (or empty preedit):
//        local composition active                                -> kCancel
//        otherwise                                               -> kNoEdit
PlanDecision DecidePlanFromEngine(const LocalDocumentView& view,
                                  const EngineObservation& observation);

}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_COORDINATOR_ENGINE_TO_PLAN_H_
