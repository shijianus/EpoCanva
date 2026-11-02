// EpoCanva P3.10 -- implementation of the observer-to-coordinator binding adapter. The
// rules are in observed_binding.h; this file only applies them.
#include "observed_binding.h"

namespace epocanva {
namespace tsf {
namespace coordinator {

ObservedBindingController::ObservedBindingController(
    context::ContextObserver& observer, ContextCoordinator& coordinator,
    context::ContextRegistry& registry, ObservedEngineClients& clients)
    : observer_(observer),
      coordinator_(coordinator),
      registry_(registry),
      clients_(clients) {
  // A pre-bound coordinator would not accept the registry; the caller owns that contract.
  coordinator_.AttachContextRegistry(&registry_);
  // P3.13: the observer is the live target authority for ordinary input and edits. This
  // installs the submit/plan/write checks before any binding exists.
  coordinator_.AttachTargetRevalidator(&observer_);
  observer_.SetSink(this);
}

ObservedBindingController::~ObservedBindingController() {
  // A notification must never reach a destroyed controller.
  observer_.SetSink(nullptr);
}

void ObservedBindingController::OnTargetLost(context::ContextDropReason reason) {
  ++diagnostics_.target_losses;
  diagnostics_.last_loss_reason = reason;
  // The target this rebind was waiting for is no longer the one the user is on. A later
  // OnTargetBound (or the observer's Push/Pop revalidation) publishes the new one.
  rebind_pending_ = false;
  if (coordinator_.bound()) {
    // Release-only: the P3.7 session's invalidate/close never issues a text operation and
    // the permission source's abandon never requests an edit session. This is safe from
    // inside OnEndEdit's read-only callback.
    coordinator_.InvalidateBinding();
    ++diagnostics_.invalidations;
  }
}

void ObservedBindingController::OnTargetBound(ITfContext* context,
                                              const context::ObservedTarget& target) {
  ++diagnostics_.target_binds;
  diagnostics_.last_bind_evidence = target.evidence;
  if (context == nullptr || !target.valid()) {
    ++diagnostics_.binds_refused;
    diagnostics_.last_bind_status = BindStatus::kInvalidRequest;
    diagnostics_.last_refusal = BindStatus::kInvalidRequest;
    rebind_pending_ = false;
    return;
  }
  if (coordinator_.bound()) {
    const edit::ContextIdentity current = coordinator_.identity();
    if (current.context_id == target.context_id &&
        current.generation == target.generation) {
      // Already the same binding; nothing to do.
      diagnostics_.last_bind_status = BindStatus::kOk;
      rebind_pending_ = false;
      return;
    }
    coordinator_.InvalidateBinding();
    ++diagnostics_.invalidations;
  }

  // Admission BEFORE the host creates a client. A full retirement ledger must not leave an
  // unused engine client behind; the explicit kRetiring/kBusy status is reported instead.
  const BindStatus admitted = coordinator_.CheckBindingAdmission(
      context, target.context_id, target.generation, target.caret_utf16);
  if (admitted != BindStatus::kOk) {
    diagnostics_.last_bind_status = admitted;
    diagnostics_.last_refusal = admitted;
    diagnostics_.last_refused_context_id = target.context_id;
    diagnostics_.last_refused_generation = target.generation;
    if (admitted == BindStatus::kRetiring) {
      ++diagnostics_.binds_paused_retiring;
      // Do not promise this target is bound. Remember only that a recheck is owed; the
      // controller's Pump() re-reads the CURRENT focus after retirement, never this target.
      rebind_pending_ = true;
    } else if (admitted == BindStatus::kBusy) {
      ++diagnostics_.binds_paused_busy;
      rebind_pending_ = true;
    } else if (admitted == BindStatus::kBlockedUncertain) {
      ++diagnostics_.binds_blocked_uncertain;
    } else {
      ++diagnostics_.binds_refused;
    }
    return;
  }

  const engine::ClientId client = clients_.NextClientForTarget(target);
  ++diagnostics_.engine_clients_requested;
  if (!client.valid()) {
    ++diagnostics_.binds_refused;
    diagnostics_.last_bind_status = BindStatus::kInvalidRequest;
    diagnostics_.last_refusal = BindStatus::kInvalidRequest;
    rebind_pending_ = false;
    return;
  }

  BindStatus status = coordinator_.BindObserved(
      context, target.context_id, target.generation, target.caret_utf16, client);
  if (status == BindStatus::kBusy) {
    // A previous work item's engine result is still owed. Pump() never waits; if the
    // result is already ready it is consumed-and-discarded now, after which the binding
    // can be created. A result that is still executing is left to the host's own drive
    // loop and this attempt is reported as Busy (no second target will be replayed).
    ++diagnostics_.binds_busy_retried;
    coordinator_.Pump();
    status = coordinator_.BindObserved(context, target.context_id, target.generation,
                                       target.caret_utf16, client);
  }
  diagnostics_.last_bind_status = status;
  switch (status) {
    case BindStatus::kOk:
      coordinator_.SetBackendAccess(clients_.CurrentBackendAccess());
      ++diagnostics_.binds_ok;
      rebind_pending_ = false;
      return;
    case BindStatus::kBlockedUncertain:
      ++diagnostics_.binds_blocked_uncertain;
      rebind_pending_ = false;
      return;
    case BindStatus::kRetiring:
      ++diagnostics_.binds_paused_retiring;
      diagnostics_.last_refusal = status;
      diagnostics_.last_refused_context_id = target.context_id;
      diagnostics_.last_refused_generation = target.generation;
      rebind_pending_ = true;
      return;
    case BindStatus::kBusy:
      ++diagnostics_.binds_paused_busy;
      diagnostics_.last_refusal = status;
      diagnostics_.last_refused_context_id = target.context_id;
      diagnostics_.last_refused_generation = target.generation;
      rebind_pending_ = true;
      return;
    default:
      ++diagnostics_.binds_refused;
      diagnostics_.last_refusal = status;
      diagnostics_.last_refused_context_id = target.context_id;
      diagnostics_.last_refused_generation = target.generation;
      rebind_pending_ = false;
      return;
  }
}

void ObservedBindingController::OnOwnEditCompleted(
    ITfContext* context, const context::ObservedTarget& target,
    const context::SelectionSnapshot& selection) {
  (void)context;
  if (!selection.ok() || !coordinator_.bound()) {
    return;
  }
  const edit::ContextIdentity current = coordinator_.identity();
  if (current.context_id != target.context_id ||
      current.generation != target.generation) {
    ++diagnostics_.own_edits_stale;
    return;
  }
  coordinator_.ObserveRealCaret(selection.caret_utf16);
  ++diagnostics_.own_edit_updates;
}

void ObservedBindingController::OnHostEditCompleted(
    ITfContext* context, const context::ObservedTarget& target,
    const context::SelectionSnapshot& selection) {
  (void)context;
  if (!selection.ok() || !coordinator_.bound()) {
    return;
  }
  const edit::ContextIdentity current = coordinator_.identity();
  if (current.context_id != target.context_id ||
      current.generation != target.generation) {
    ++diagnostics_.own_edits_stale;
    return;
  }
  // Host-owned text: the document changed through the host's own input path. The binding
  // and engine client stay; only the caret mirror is updated. The coordinator never
  // re-reads or re-writes this text.
  coordinator_.ObserveRealCaret(selection.caret_utf16);
  ++diagnostics_.host_edit_updates;
}

ObservedBindingController::Diagnostics ObservedBindingController::Snapshot() const {
  Diagnostics copy = diagnostics_;
  copy.coordinator_bound = coordinator_.bound();
  copy.rebind_pending = rebind_pending_;
  return copy;
}

void ObservedBindingController::Pump() {
  if (!rebind_pending_) {
    return;
  }
  ++diagnostics_.rebind_retries;
  const CoordinatorSnapshot current = coordinator_.Snapshot();
  if (current.closed) {
    rebind_pending_ = false;
    return;
  }
  if (current.bound) {
    rebind_pending_ = false;
    return;
  }
  if (current.gate != CoordinatorGate::kOpen) {
    // The binding is locked down; re-reading the target cannot lift that by itself.
    rebind_pending_ = false;
    return;
  }
  if (current.phase != WorkPhase::kIdle ||
      current.retirement.retiring_bindings != 0 ||
      current.retirement.admission_paused) {
    // The retirement ledger is still the reason admission was refused. Wait for a real
    // terminal fact; the host calls Pump again after coordinator.Pump().
    return;
  }

  // The retirement ledger is terminal. Re-read the CURRENT thread focus/top context; the
  // refused target id/generation is kept only for diagnostics and is never replayed.
  ++diagnostics_.rebind_requests;
  observer_.RequestRebind();
  observer_.Pump();
  ++diagnostics_.rebind_revalidations;

  const context::ContextObserverSnapshot after = observer_.Snapshot();
  if (!after.has_top_context) {
    // Focus moved away while we were waiting. The observer will publish when a real target
    // returns; there is nothing to retry against.
    rebind_pending_ = false;
    return;
  }
  // A publication that was refused for a terminal reason (uncertain latch, registry
  // capacity, unreadable selection) produces no OnTargetBound. Do not retry a target the
  // observer cannot publish; the next real focus event will start a new attempt.
  if (!coordinator_.bound() && !after.awaiting_bind &&
      !after.selection_read_pending && !after.has_published_target) {
    rebind_pending_ = false;
  }
}

}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva
