// EpoCanva P3.10 -- the coordinator side of the context observer.
//
// WHAT THIS IS. A small adapter that turns the observer's three binding events into the
// coordinator's operations:
//
//   OnTargetLost            -> ContextCoordinator::InvalidateBinding (release-only)
//   OnTargetBound           -> a fresh engine client + ContextCoordinator::BindObserved
//   OnOwnEditCompleted      -> ContextCoordinator::ObserveRealCaret
//
// It owns no TSF session, no text and no policy beyond that mapping. It must run on the
// same STA as the observer and the coordinator.
//
// FRESH ENGINE CLIENT PER BINDING. The coordinator's InvalidateBinding discards the engine
// side of the old binding; the host must rebuild the engine client. ObservedEngineClients is
// the host-provided source of that client (the tests implement it over the executor).
// A client is requested for every OnTargetBound, so an external edit can never continue an
// old engine composition at a new caret. The controller does not close the old client: the
// executor owns it and the host decides its lifetime.
//
// UNCERTAIN ACROSS CONTROLLERS. The controller attaches the shared ContextRegistry to the
// coordinator. The latch is keyed by the real ITfContext, so a new controller/coordinator
// pair that shares the registry cannot bind the same context by supplying another integer.
//
// NO WRITES FROM A NOTIFICATION. OnTargetLost may be called from inside
// ITfTextEditSink::OnEndEdit. It only invalidates: session Invalidate/Close are
// release-only, the permission source is abandoned (no TSF call), and no text operation is
// issued. Binding and the read-only selection query happen in the observer's Pump, outside
// the notification.
#ifndef EPOCANVA_WINDOWS_TSF_COORDINATOR_OBSERVED_BINDING_H_
#define EPOCANVA_WINDOWS_TSF_COORDINATOR_OBSERVED_BINDING_H_

#include <cstdint>

#include "context_observer.h"
#include "context_registry.h"
#include "coordinator.h"

namespace epocanva {
namespace tsf {
namespace coordinator {

// The host's source of a fresh engine client for one binding. Called on the STA from
// OnTargetBound, never inside a TSF callback.
class ObservedEngineClients {
 public:
  virtual ~ObservedEngineClients() {}
  virtual engine::ClientId NextClient() = 0;
};

class ObservedBindingController final : public context::ContextObserverSink {
 public:
  // Attaches the registry to the coordinator and installs itself as the observer's sink
  // (SetSink, not a replay of an existing target). All four objects must outlive this one;
  // the destructor clears the observer's sink so a later notification cannot reach a
  // destroyed controller.
  ObservedBindingController(context::ContextObserver& observer,
                            ContextCoordinator& coordinator,
                            context::ContextRegistry& registry,
                            ObservedEngineClients& clients);
  ~ObservedBindingController() override;

  ObservedBindingController(const ObservedBindingController&) = delete;
  ObservedBindingController& operator=(const ObservedBindingController&) = delete;

  void OnTargetLost(context::ContextDropReason reason) override;
  void OnTargetBound(ITfContext* context,
                     const context::ObservedTarget& target) override;
  void OnOwnEditCompleted(ITfContext* context,
                          const context::ObservedTarget& target,
                          const context::SelectionSnapshot& selection) override;

  // Value-only diagnostics for assertions and text-free logging.
  struct Diagnostics {
    std::uint64_t target_binds = 0;
    std::uint64_t target_losses = 0;
    std::uint64_t binds_ok = 0;
    std::uint64_t binds_refused = 0;
    std::uint64_t binds_blocked_uncertain = 0;
    std::uint64_t binds_busy_retried = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t own_edit_updates = 0;
    std::uint64_t own_edits_stale = 0;
    std::uint64_t engine_clients_requested = 0;
    BindStatus last_bind_status = BindStatus::kOk;
    context::ContextDropReason last_loss_reason = context::ContextDropReason::kNone;
    bool coordinator_bound = false;
  };

  Diagnostics Snapshot() const;

 private:
  context::ContextObserver& observer_;
  ContextCoordinator& coordinator_;
  context::ContextRegistry& registry_;
  ObservedEngineClients& clients_;
  Diagnostics diagnostics_;
};

}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_COORDINATOR_OBSERVED_BINDING_H_
