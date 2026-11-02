// EpoCanva P3.10/P3.13 -- the coordinator side of the context observer.
//
// WHAT THIS IS. A small adapter that turns the observer's three binding events into the
// coordinator's operations:
//
//   OnTargetLost            -> ContextCoordinator::InvalidateBinding (logical retire only)
//   OnTargetBound           -> a fresh engine client + ContextCoordinator::BindObserved
//   OnOwnEditCompleted      -> ContextCoordinator::ObserveRealCaret
//
// It owns no TSF session, no text and no policy beyond that mapping. It must run on the
// same STA as the observer and the coordinator.
//
// FRESH ENGINE CLIENT PER BINDING. The coordinator's InvalidateBinding moves the old binding
// into its bounded retirement ledger; the host must rebuild the engine client. ObservedEngine
// Clients is the host-provided source of that client (the tests implement it over the
// executor). Admission is checked BEFORE NextClient(), so a full retirement ledger refuses
// the binding as kRetiring without creating another client. A client is requested for every
// admitted OnTargetBound, so an external edit can never continue an old engine composition
// at a new caret. Once bound, the executor closes that client on retirement and the close
// result is consumed by the coordinator's Pump; StopAndJoin is not the only owner.
//
// UNCERTAIN ACROSS CONTROLLERS. The controller attaches the shared ContextRegistry to the
// coordinator. The latch is keyed by the real ITfContext, so a new controller/coordinator
// pair that shares the registry cannot bind the same context by supplying another integer.
//
// TARGET AUTHORITY AND REBIND (P3.13). The controller attaches the observer as the
// coordinator's live TargetRevalidator. A target refused as Retiring/Busy is not promised
// bound, creates no client, and only sets `rebind_pending`. Pump() re-reads the CURRENT
// focus after the retirement ledger is terminal; it never replays the refused target id or
// any refused key/candidate/commit.
//
// NO WRITES FROM A NOTIFICATION. OnTargetLost may be called from inside
// ITfTextEditSink::OnEndEdit. It only invalidates: the session is moved into the retirement
// ledger, its pending plan is abandoned, the permission source is abandoned (no TSF call),
// and no text operation is issued. The legal EndOwnedComposition cleanup and the engine
// close happen in the host's Pump, outside the notification. Binding and the read-only
// selection query happen in the observer's Pump, outside the notification.
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
  virtual engine::ClientId NextClientForTarget(const context::ObservedTarget& target) {
    (void)target; return NextClient();
  }
  virtual engine::BackendAccess CurrentBackendAccess() const { return {}; }
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
  void OnHostEditCompleted(ITfContext* context,
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
    std::uint64_t binds_paused_retiring = 0;
    std::uint64_t binds_paused_busy = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t own_edit_updates = 0;
    std::uint64_t own_edits_stale = 0;
    std::uint64_t host_edit_updates = 0;
    std::uint64_t engine_clients_requested = 0;
    BindStatus last_bind_status = BindStatus::kOk;
    context::ContextDropReason last_loss_reason = context::ContextDropReason::kNone;
    context::TargetBindingEvidence last_bind_evidence =
        context::TargetBindingEvidence::kNone;
    // P3.13: a target refused as Retiring/Busy is not promised bound. The controller
    // remembers that a recheck is owed and, once the retirement ledger is terminal, asks
    // the observer to re-read the CURRENT GetFocus/GetTop. The refused target itself is
    // never replayed and no client is created for it.
    bool rebind_pending = false;
    std::uint64_t rebind_requests = 0;
    std::uint64_t rebind_retries = 0;
    std::uint64_t rebind_revalidations = 0;
    std::uint64_t last_refused_context_id = 0;
    std::uint64_t last_refused_generation = 0;
    BindStatus last_refusal = BindStatus::kOk;
    bool coordinator_bound = false;
  };

  Diagnostics Snapshot() const;

  // P3.13: advances a pending rebind. Called by the host after ContextCoordinator::Pump,
  // outside every TSF notification. It does nothing while the coordinator is busy or its
  // retirement ledger is non-empty, then asks the observer for a fresh read of the current
  // TSF focus. No stored target, key, candidate or commit is replayed.
  void Pump();

 private:
  context::ContextObserver& observer_;
  ContextCoordinator& coordinator_;
  context::ContextRegistry& registry_;
  ObservedEngineClients& clients_;
  Diagnostics diagnostics_;
  bool rebind_pending_ = false;
};

}  // namespace coordinator
}  // namespace tsf
}  // namespace epocanva

#endif  // EPOCANVA_WINDOWS_TSF_COORDINATOR_OBSERVED_BINDING_H_
