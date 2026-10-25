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
  observer_.SetSink(this);
}

ObservedBindingController::~ObservedBindingController() {
  // A notification must never reach a destroyed controller.
  observer_.SetSink(nullptr);
}

void ObservedBindingController::OnTargetLost(context::ContextDropReason reason) {
  ++diagnostics_.target_losses;
  diagnostics_.last_loss_reason = reason;
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
  if (context == nullptr || !target.valid()) {
    ++diagnostics_.binds_refused;
    diagnostics_.last_bind_status = BindStatus::kInvalidRequest;
    return;
  }
  if (coordinator_.bound()) {
    const edit::ContextIdentity current = coordinator_.identity();
    if (current.context_id == target.context_id &&
        current.generation == target.generation) {
      // Already the same binding; nothing to do.
      diagnostics_.last_bind_status = BindStatus::kOk;
      return;
    }
    coordinator_.InvalidateBinding();
    ++diagnostics_.invalidations;
  }

  const engine::ClientId client = clients_.NextClient();
  ++diagnostics_.engine_clients_requested;
  if (!client.valid()) {
    ++diagnostics_.binds_refused;
    diagnostics_.last_bind_status = BindStatus::kInvalidRequest;
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
      ++diagnostics_.binds_ok;
      return;
    case BindStatus::kBlockedUncertain:
      ++diagnostics_.binds_blocked_uncertain;
      return;
    default:
      ++diagnostics_.binds_refused;
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

ObservedBindingController::Diagnostics ObservedBindingController::Snapshot() const {
  Diagnostics copy = diagnostics_;
  copy.coordinator_bound = coordinator_.bound();
  return copy;
}

}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva
