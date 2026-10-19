// EpoCanva P2.5 -- see english_space_processor.h for the role of this class.
//
// Compiled INTO rime.dll alongside the Translator, the module registration and the
// language core it calls (tools/cmake/epocanva_rime_compiled_in_components.cmake).
//
// No LOG()/DLOG() anywhere in this file: ProcessKeyEvent() is handed the user's
// composition state on every space press, so emitting a log line here would put
// composing text in a log file (AGENTS.md section 2).
#include "english_space_processor.h"

#include <atomic>
#include <string>

#include <rime/composition.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/segmentation.h>

#include "english_space_policy.h"

namespace epocanva {
namespace english {
namespace {

std::atomic<unsigned long long> g_space_constructed{0};
std::atomic<unsigned long long> g_space_destroyed{0};
std::atomic<unsigned long long> g_space_live{0};
std::atomic<unsigned long long> g_key_events{0};
std::atomic<unsigned long long> g_space_seen{0};
std::atomic<unsigned long long> g_space_refused{0};
std::atomic<unsigned long long> g_case_a{0};
std::atomic<unsigned long long> g_case_b{0};
std::atomic<unsigned long long> g_case_c{0};
std::atomic<unsigned long long> g_composition_commits{0};
std::atomic<unsigned long long> g_space_characters{0};

// Relaxed atomics, matching the probe and Translator surfaces: these are witnesses,
// not a protocol, and a front end may own engines on more than one thread.

// "An explicit selection has been made and has not been committed yet."
//
// Read straight out of the live composition, never from a remembered word:
// RimeApi::select_candidate() reaches Context::Select (src/rime_api_impl.h:1040-1043),
// which sets Segment::status = kSelected (src/rime/context.cc:118-130), and
// ConcreteEngine::OnSelect raises it to kConfirmed when the segment ends at the input
// length (src/rime/engine.cc:259-270). Both are >= Segment::kSelected
// (src/rime/segmentation.h:18-23). Keystrokes that only highlight go through
// Context::Highlight (src/rime/context.cc:132-149), which moves selected_index and
// leaves the status alone -- that is exactly the difference case A has to detect,
// because a highlighted-but-not-selected completion must NOT be accepted by a space.
bool HasPendingSelection(const rime::Composition& composition) {
  for (const rime::Segment& segment : composition) {
    if (segment.status >= rime::Segment::kSelected) {
      return true;
    }
  }
  return false;
}

}  // namespace

EpoCanvaEnglishSpaceProcessor::EpoCanvaEnglishSpaceProcessor(
    const rime::Ticket& ticket)
    : rime::Processor(ticket) {
  g_space_constructed.fetch_add(1, std::memory_order_relaxed);
  g_space_live.fetch_add(1, std::memory_order_relaxed);
}

EpoCanvaEnglishSpaceProcessor::~EpoCanvaEnglishSpaceProcessor() {
  g_space_live.fetch_sub(1, std::memory_order_relaxed);
  g_space_destroyed.fetch_add(1, std::memory_order_relaxed);
}

rime::ProcessResult EpoCanvaEnglishSpaceProcessor::ProcessKeyEvent(
    const rime::KeyEvent& key_event) {
  g_key_events.fetch_add(1, std::memory_order_relaxed);
  const epocanva::en::SpaceKeyEvent kind = epocanva::en::ClassifySpaceKey(
      key_event.keycode(), static_cast<unsigned>(key_event.modifier()));
  if (kind == epocanva::en::SpaceKeyEvent::kNotSpace) {
    // Not ours. Returning kNoop leaves the event to the speller and the editor
    // exactly as they were before this processor existed, which is what makes the
    // 26 pre-existing CTest cases unchanged rather than merely still passing.
    return rime::kNoop;
  }
  g_space_seen.fetch_add(1, std::memory_order_relaxed);
  if (kind != epocanva::en::SpaceKeyEvent::kPlainSpace) {
    // A key release, or Ctrl/Alt/Super+Space. Refusing means kNoop, not kRejected:
    // kRejected would cut the chain short (src/rime/engine.cc:102-108) and stop the
    // editor from seeing a key that may be bound elsewhere.
    g_space_refused.fetch_add(1, std::memory_order_relaxed);
    return rime::kNoop;
  }

  // engine_ and context() are non-null here for the same reason upstream's Speller
  // assumes them (src/rime/gear/speller.cc:111): the engine constructs processors
  // with its own ticket and owns them for the whole key sequence.
  rime::Context* ctx = engine_->context();
  const epocanva::en::SpaceCase selected = epocanva::en::DecideSpaceCase(
      ctx->IsComposing(), HasPendingSelection(ctx->composition()));
  switch (selected) {
    case epocanva::en::SpaceCase::kCommitTokenThenSpace:
      g_case_a.fetch_add(1, std::memory_order_relaxed);
      break;
    case epocanva::en::SpaceCase::kCommitSelectionThenSpace:
      g_case_b.fetch_add(1, std::memory_order_relaxed);
      break;
    case epocanva::en::SpaceCase::kSpaceOnly:
      g_case_c.fetch_add(1, std::memory_order_relaxed);
      break;
    case epocanva::en::SpaceCase::kNotApplicable:
      break;
  }

  if (epocanva::en::CaseCommitsComposition(selected)) {
    if (selected == epocanva::en::SpaceCase::kCommitTokenThenSpace) {
      // Case A commits the RAW composition string: Context::input(), the same
      // bytes RimeApi::get_input() hands a front end
      // (src/rime_api_impl.h:1062-1070 returns ctx->input().c_str()). It is not the
      // preedit -- preedit is display-processed by Composition::GetPreedit and may
      // carry soft-cursor or per-segment display text. Committing input() is also
      // what keeps a merely HIGHLIGHTED completion unaccepted: the engine's own
      // space action would commit the highlighted candidate text instead
      // (Composition::GetCommitText, src/rime/composition.cc:102-120).
      engine_->CommitText(ctx->input());
      ctx->Clear();
    } else {
      // Case B lets the engine compute the commit text of the confirmed
      // composition, so the selected completion is committed exactly once and the
      // composition is cleared by the same call (src/rime/context.cc:18-26).
      ctx->Commit();
    }
    g_composition_commits.fetch_add(1, std::memory_order_relaxed);
  }
  if (epocanva::en::CaseEmitsSpace(selected)) {
    // One U+0020, as its own commit event. Session::OnCommit appends
    // (src/rime/service.cc:55-58), so a front end reading get_commit() after this
    // key sees the token and the space as one string while the counts above still
    // show that two commits were issued.
    engine_->CommitText(std::string(1, epocanva::en::kSpaceCharacter));
    g_space_characters.fetch_add(1, std::memory_order_relaxed);
  }
  return rime::kAccepted;
}

EnglishSpaceCounters SnapshotSpaceCounters() {
  EnglishSpaceCounters out;
  out.constructed = g_space_constructed.load(std::memory_order_relaxed);
  out.destroyed = g_space_destroyed.load(std::memory_order_relaxed);
  out.live = g_space_live.load(std::memory_order_relaxed);
  out.key_events = g_key_events.load(std::memory_order_relaxed);
  out.space_seen = g_space_seen.load(std::memory_order_relaxed);
  out.space_refused = g_space_refused.load(std::memory_order_relaxed);
  out.case_a = g_case_a.load(std::memory_order_relaxed);
  out.case_b = g_case_b.load(std::memory_order_relaxed);
  out.case_c = g_case_c.load(std::memory_order_relaxed);
  out.composition_commits = g_composition_commits.load(std::memory_order_relaxed);
  out.space_characters = g_space_characters.load(std::memory_order_relaxed);
  return out;
}

}  // namespace english
}  // namespace epocanva
