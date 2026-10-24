// EpoCanva P3.8 -- implementation of the pure engine-observation-to-edit-decision mapping.
// See engine_to_plan.h for the contract; every rule below is numbered there.
#include "engine_to_plan.h"

#include <utility>

namespace epocanva {
namespace tsf {
namespace coordinator {

namespace {

std::u16string PreeditText(const engine::EngineState& state) {
  return state.preedit.utf16_code_units;
}

bool EngineComposingWithPreedit(const engine::EngineState& state) {
  return state.is_composing && !state.preedit.utf16_code_units.empty();
}

}  // namespace

const char* DescribePlanDecisionKind(PlanDecisionKind kind) {
  switch (kind) {
    case PlanDecisionKind::kNoEdit:
      return "NoEdit";
    case PlanDecisionKind::kBegin:
      return "Begin";
    case PlanDecisionKind::kUpdate:
      return "Update";
    case PlanDecisionKind::kCommit:
      return "Commit";
    case PlanDecisionKind::kCancel:
      return "Cancel";
    case PlanDecisionKind::kUnsupportedShape:
      return "UnsupportedShape";
    case PlanDecisionKind::kInvalidObservation:
      return "InvalidObservation";
  }
  return "UnknownPlanDecision";
}

PlanDecision DecidePlanFromEngine(const LocalDocumentView& view,
                                  const EngineObservation& observation) {
  PlanDecision decision;

  // Rule 1: the command produced no mappable engine change.
  if (observation.engine_refused || !observation.engine_call_made) {
    decision.kind = PlanDecisionKind::kNoEdit;
    decision.diagnostic = "engine-refused-or-not-called";
    return decision;
  }

  // Rule 2: the observation itself is not trustworthy. A dispatch whose state read failed
  // has already changed the engine; claiming "no edit" or building a plan from a stale
  // state would both be guesses.
  const bool state_ok = observation.state_status == engine::SessionStateStatus::kOk &&
                        observation.state.valid;
  const bool commit_ok = observation.commit_status ==
                             engine::SessionCommitStatus::kNoCommit ||
                         observation.commit_status == engine::SessionCommitStatus::kRead;
  if (!state_ok || !commit_ok) {
    decision.kind = PlanDecisionKind::kInvalidObservation;
    decision.diagnostic = "observation-state-or-commit-unavailable";
    return decision;
  }

  const std::u16string preedit = PreeditText(observation.state);
  const bool has_commit = observation.commit.present;
  const std::u16string commit_text = observation.commit.text_utf16_code_units;

  // Rule 3: the one shape this round refuses to halve.
  if (has_commit && EngineComposingWithPreedit(observation.state)) {
    decision.kind = PlanDecisionKind::kUnsupportedShape;
    decision.diagnostic = "commit-and-preedit-in-one-observation";
    return decision;
  }

  if (has_commit) {
    if (!view.composition_active && commit_text.empty()) {
      // Rule 4b: an empty commit with no composition has nothing to insert; the P3.7
      // no-preedit commit path would refuse it (NothingToCommit), so it is decided here
      // instead of submitted to fail.
      decision.kind = PlanDecisionKind::kNoEdit;
      decision.diagnostic = "empty-commit-without-composition";
      return decision;
    }
    decision.kind = PlanDecisionKind::kCommit;
    decision.text_utf16 = commit_text;
    decision.caret_utf16 = view.caret_utf16;
    decision.diagnostic = "commit-with" +
                          std::string(view.composition_active ? "-composition"
                                                              : "-tracked-caret");
    return decision;
  }

  if (EngineComposingWithPreedit(observation.state)) {
    if (!view.composition_active) {
      decision.kind = PlanDecisionKind::kBegin;
      decision.text_utf16 = preedit;
      decision.caret_utf16 = view.caret_utf16;
      decision.diagnostic = "begin-preedit";
      return decision;
    }
    if (view.composition_text_utf16 == preedit) {
      decision.kind = PlanDecisionKind::kNoEdit;
      decision.diagnostic = "preedit-already-displayed";
      return decision;
    }
    decision.kind = PlanDecisionKind::kUpdate;
    decision.text_utf16 = preedit;
    decision.diagnostic = "update-preedit";
    return decision;
  }

  if (view.composition_active) {
    decision.kind = PlanDecisionKind::kCancel;
    decision.diagnostic = "engine-composition-ended";
    return decision;
  }

  decision.kind = PlanDecisionKind::kNoEdit;
  decision.diagnostic = "no-composition-no-commit";
  return decision;
}

}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva
